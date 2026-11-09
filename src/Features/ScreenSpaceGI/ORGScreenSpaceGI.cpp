#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGScreenSpaceGI.h"

#	include "Deferred.h"
#	include "Features/ScreenSpaceGI.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <cstddef>
#	include <cstring>
#	include <format>
#	include <span>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.ssgi";
	constexpr const char* kNoiseName = "ssgi.noise";
	constexpr std::uint32_t kMips = 5;
	// Below Subsurface Scattering's keys (199-202): the D3D11 frame runs SSGI first.
	constexpr std::int32_t kOrderReset = 100;
	constexpr std::int32_t kOrderFirstStep = 110;
	constexpr std::size_t kHistoryTextures = 6;

	/** @brief The chain's dispatches, in their D3D11 order; also each one's group counts in the latch region. */
	enum Step : std::uint32_t
	{
		kPrefilterDepths,
		kRadianceDisocc,
		kPrefilterRadiance,
		kPrefilterNormal,
		kGi,
		kBlur,
		kUpsample,
		kSteps
	};

	struct StepInfo
	{
		const char* pass;
		const char* source;
		std::uint32_t views;  // the shader's SRV and UAV entries, after its SHARED, latch and sampler entries
	};

	constexpr std::array<StepInfo, kSteps> kStepInfo{ {
		{ "cs.ssgi.prefilter-depths", "ScreenSpaceGI/prefilterDepths.cs.hlsl", 1 + kMips },
		{ "cs.ssgi.radiance-disocc", "ScreenSpaceGI/radianceDisocc.cs.hlsl", 10 + 6 },
		{ "cs.ssgi.prefilter-radiance", "ScreenSpaceGI/prefilterRadiance.cs.hlsl", 1 + kMips },
		{ "cs.ssgi.prefilter-normal", "ScreenSpaceGI/prefilterNormal.cs.hlsl", 1 + kMips },
		{ "cs.ssgi.gi", "ScreenSpaceGI/gi.cs.hlsl", 10 + 5 },
		{ "cs.ssgi.blur", "ScreenSpaceGI/blur.cs.hlsl", 5 + 3 },
		{ "cs.ssgi.upsample", "ScreenSpaceGI/upsample.cs.hlsl", 5 + 4 },
	} };

	// Each push block: the four shared constant handles, the latch (handle, offset), the point and linear samplers, then the views.
	constexpr std::uint32_t kFixedWords = 4 + 2 + 2;

	/** @brief One execution's values, in its slot of the latch block: SSGICB (the shaders' latch entry), then each step's groups. */
	struct LatchData
	{
		ScreenSpaceGI::SSGICB constants;
		std::uint32_t groups[kSteps][4];  // x, y, z, padding
	};
	static_assert(sizeof(ScreenSpaceGI::SSGICB) % 16 == 0);
	static_assert(offsetof(LatchData, groups) % 16 == 0);

	/** @brief The settings the programs and the textures are made from: a change makes them again. */
	struct Config
	{
		int resolutionMode = 0;
		bool enabled = false;
		bool gi = false;
		bool specular = false;
		bool temporal = false;
		bool blur = false;

		bool operator==(const Config&) const = default;

		bool FullRes() const { return resolutionMode == 0; }
		// The shaders touch the specular textures with the specular GI or the temporal denoiser on.
		bool SpecularTextures() const { return specular || temporal; }

		static Config Current()
		{
			const auto& settings = globals::features::screenSpaceGI.settings;
			return { settings.ResolutionMode, settings.Enabled, settings.EnableGI, settings.EnableExperimentalSpecularGI,
				settings.EnableTemporalDenoiser, settings.EnableBlur };
		}
	};

	/** @brief A step's program, with the indirect dispatch signature made against its layout. */
	struct Program
	{
		std::shared_ptr<const ComputeProgram> compute;
		rhi::CommandSignaturePtr signature;
		std::uint32_t words = 0;
	};

	/** @brief The engine and feature textures the chain reads, as graph textures. */
	struct Inputs
	{
		std::shared_ptr<org::ExternalTextureResource> depth, main, normalRoughness, motionVectors, noise;

		bool operator==(const Inputs&) const = default;
		explicit operator bool() const { return depth && main && normalRoughness && motionVectors && noise; }
	};

	using Texture = std::unique_ptr<GraphResources::Texture2D>;

	/**
	 * @brief What one configuration's passes are made from. Made on the render thread; a new one replaces it on a settings change.
	 * Passes copy what they need when they are made (ClientDesc's make), so the host thread never reads this.
	 */
	struct GiState
	{
		Config config;
		std::array<std::shared_ptr<const Program>, kSteps> programs;
		std::shared_ptr<org::LatchBlock> latch;
		Inputs inputs;

		// Transient within the chain: aliasable unless exported.
		Texture workingDepth, normal, radiance, radianceTemp, accumScratch, aoScratch, yScratch, coCgScratch, specScratch, yBlurInput,
			coCgBlurInput;
		// Read by the next frame's disocclusion pass.
		Texture accum, ao, y, coCg, spec, prevGeo;

		// The history reset: pending until a build has its clears, then dropped from the next build.
		bool resetPending = true;
		bool clearBuilt = false;

		/** @brief The textures the composite reads: the history at full resolution, the upsampled scratch otherwise. */
		const Texture& OutAo() const { return config.FullRes() ? ao : aoScratch; }
		const Texture& OutY() const { return config.FullRes() ? y : yScratch; }
		const Texture& OutCoCg() const { return config.FullRes() ? coCg : coCgScratch; }
		const Texture& OutSpec() const { return config.FullRes() ? spec : specScratch; }

		std::vector<const Texture*> All() const
		{
			return { &workingDepth, &normal, &radiance, &radianceTemp, &accumScratch, &aoScratch, &yScratch, &coCgScratch, &specScratch,
				&yBlurInput, &coCgBlurInput, &accum, &ao, &y, &coCg, &spec, &prevGeo };
		}

		std::array<const Texture*, kHistoryTextures> History() const { return { &accum, &ao, &y, &coCg, &spec, &prevGeo }; }
	};

	std::shared_ptr<org::Resource> GraphOf(const Texture& a_texture) { return a_texture ? a_texture->Graph() : nullptr; }

	/** @brief The defines CompileComputeShaders and Util::CompileShader give the D3D11 build of a step. */
	std::vector<std::pair<std::wstring, std::wstring>> StepDefines(Step a_step, const Config& a_config)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		if (a_step == kPrefilterDepths)
			defines.emplace_back("LINEAR_FILTER", "");
		if (a_config.resolutionMode == 1)
			defines.emplace_back("HALF_RES", "");
		if (a_config.resolutionMode == 2)
			defines.emplace_back("QUARTER_RES", "");
		if (a_config.temporal)
			defines.emplace_back("TEMPORAL_DENOISER", "");
		if (a_config.gi)
			defines.emplace_back("GI", "");
		if (a_config.specular)
			defines.emplace_back("GI_SPECULAR", "");
		std::vector<std::string_view> names;
		for (const auto& define : defines)
			names.push_back(define.first);
		for (auto& define : Util::GetCommonShaderDefines(names))
			defines.push_back(std::move(define));
		for (const char* name : { "COMPUTESHADER", "WINPC", "DX11" })
			defines.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : defines)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	std::shared_ptr<const Program> LoadProgram(rhi::Device a_device, Step a_step, const Config& a_config)
	{
		auto program = std::make_shared<Program>();
		program->words = kFixedWords + kStepInfo[a_step].views;
		program->compute = ComputeProgram::Load(a_device, { .source = kStepInfo[a_step].source, .constantWords = program->words, .defines = StepDefines(a_step, a_config) });
		if (!program->compute)
			return nullptr;
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 },
				program->compute->layout->GetHandle(), program->signature) != rhi::Result::Ok)
			return nullptr;
		return program;
	}

	/** @brief What each step reads and writes, in its shader's list order after the fixed entries. */
	enum class UseKind : std::uint8_t
	{
		Srv,
		Uav,
		UavMips,  // a UAV of each of the kMips mips
	};

	struct Use
	{
		UseKind kind = UseKind::Srv;
		std::shared_ptr<org::Resource> resource;  // null: a resource the permutation does not touch; its handle is 0
	};

	Use Srv(std::shared_ptr<org::Resource> a_resource) { return { UseKind::Srv, std::move(a_resource) }; }
	Use Uav(std::shared_ptr<org::Resource> a_resource) { return { UseKind::Uav, std::move(a_resource) }; }
	Use UavMips(std::shared_ptr<org::Resource> a_resource) { return { UseKind::UavMips, std::move(a_resource) }; }

	std::vector<Use> StepUses(const GiState& a_state, Step a_step)
	{
		const auto& s = a_state;
		const auto& in = s.inputs;
		// With the blur, GI writes scratch textures that the blur reads, and the blur writes the history.
		const bool blur = s.config.blur;
		switch (a_step) {
		case kPrefilterDepths:
			return { Srv(in.depth), UavMips(GraphOf(s.workingDepth)) };
		case kRadianceDisocc:
			return { Srv(in.main), Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.prevGeo)), Srv(in.motionVectors),
				Srv(GraphOf(s.accum)), Srv(GraphOf(s.ao)), Srv(GraphOf(s.y)), Srv(GraphOf(s.coCg)), Srv(GraphOf(s.spec)),
				Uav(GraphOf(s.radianceTemp)), Uav(GraphOf(s.accumScratch)), Uav(GraphOf(s.aoScratch)), Uav(GraphOf(s.yScratch)),
				Uav(GraphOf(s.coCgScratch)), Uav(GraphOf(s.specScratch)) };
		case kPrefilterRadiance:
			return { Srv(GraphOf(s.radianceTemp)), UavMips(GraphOf(s.radiance)) };
		case kPrefilterNormal:
			return { Srv(in.normalRoughness), UavMips(GraphOf(s.normal)) };
		case kGi:
			return { Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.radiance)), Srv(in.noise), Srv(GraphOf(s.accumScratch)),
				Srv(GraphOf(s.yScratch)), Srv(GraphOf(s.coCgScratch)), Srv(GraphOf(s.specScratch)), Srv(GraphOf(s.normal)), Srv(GraphOf(s.aoScratch)),
				Uav(GraphOf(s.ao)), Uav(GraphOf(blur ? s.yBlurInput : s.y)), Uav(GraphOf(blur ? s.coCgBlurInput : s.coCg)), Uav(GraphOf(s.spec)),
				Uav(GraphOf(s.prevGeo)) };
		case kBlur:
			return { Srv(GraphOf(s.workingDepth)), Srv(in.normalRoughness), Srv(GraphOf(s.accumScratch)), Srv(GraphOf(s.yBlurInput)),
				Srv(GraphOf(s.coCgBlurInput)), Uav(GraphOf(s.accum)), Uav(GraphOf(s.y)), Uav(GraphOf(s.coCg)) };
		case kUpsample:
			return { Srv(GraphOf(s.workingDepth)), Srv(GraphOf(s.ao)), Srv(GraphOf(s.y)), Srv(GraphOf(s.coCg)), Srv(GraphOf(s.spec)),
				Uav(GraphOf(s.aoScratch)), Uav(GraphOf(s.yScratch)), Uav(GraphOf(s.coCgScratch)), Uav(GraphOf(s.specScratch)) };
		default:
			return {};
		}
	}

	struct DispatchBindings
	{
		SharedConstants::Bindings shared;
		std::vector<org::DeclaredViewToken> views;  // one per handle, in list order
		std::vector<std::uint8_t> present;          // 0: the handle is 0
	};

	struct DispatchFrame
	{
		std::shared_ptr<const Program> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t pointSampler = 0, linearSampler = 0;
		Step step = kSteps;
	};

	/** @brief One step's dispatch: its push block from the declared views, its group counts from the execution's latch region. */
	class DispatchPass final : public org::TypedRenderGraphPass<DispatchPass, DispatchFrame, DispatchBindings>
	{
	public:
		// At the build, on the render thread: the state as it is now.
		DispatchPass(const GiState& a_state, Step a_step, std::uint32_t a_pointSampler, std::uint32_t a_linearSampler) :
			frame{ a_state.programs[a_step], a_state.latch, a_pointSampler, a_linearSampler, a_step }, uses(StepUses(a_state, a_step)) {}

		DispatchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			DispatchBindings bindings{};
			bindings.shared = SharedConstants::Declare(a_builder);
			for (const auto& use : uses) {
				const std::uint32_t count = use.kind == UseKind::UavMips ? kMips : 1;
				if (!use.resource) {
					bindings.views.insert(bindings.views.end(), count, org::DeclaredViewToken{});
					bindings.present.insert(bindings.present.end(), count, 0);
					continue;
				}
				switch (use.kind) {
				case UseKind::Srv:
					bindings.views.push_back(a_builder.ShaderResource(use.resource).View());
					break;
				case UseKind::Uav:
					bindings.views.push_back(a_builder.UnorderedAccess(use.resource).View());
					break;
				case UseKind::UavMips:
					{
						std::array<org::UavView, kMips> mips{};
						for (std::uint32_t mip = 0; mip < kMips; ++mip)
							mips[mip].mip = mip;
						const auto declared = a_builder.UnorderedAccess(use.resource, std::span<const org::UavView>(mips));
						for (std::uint32_t mip = 0; mip < kMips; ++mip)
							bindings.views.push_back(declared.View(mip));
						break;
					}
				}
				bindings.present.insert(bindings.present.end(), count, 1);
			}
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(frame.latch.get()));
			a_out.push_back(frame.pointSampler);
			a_out.push_back(frame.linearSampler);
			a_out.push_back(frame.step);
			for (const auto& use : uses)
				a_out.push_back(reinterpret_cast<std::uintptr_t>(use.resource.get()));
		}

		DispatchFrame Prepare(const DispatchBindings&, const org::PassPrepareContext&) const { return frame; }

		static void Record(const DispatchBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.latch || a_bindings.views.size() + kFixedWords != a_frame.program->words)
				return;
			const std::uint32_t slot = a_recording.FrameSlot();
			std::array<std::uint32_t, ShaderBindings::kMaxPushWords> words{};
			const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
			const auto latch = ShaderBindings::LatchOf(*a_frame.latch, slot, offsetof(LatchData, constants));
			std::uint32_t count = 0;
			for (const std::uint32_t word : { shared.frameBuffer, shared.sharedData, shared.featureData, shared.permutation, latch.handle,
					 latch.offset, a_frame.pointSampler, a_frame.linearSampler })
				words[count++] = word;
			for (std::size_t i = 0; i < a_bindings.views.size(); ++i)
				words[count++] = a_bindings.present[i] ? a_recording.Index(a_bindings.views[i]) : 0;

			const auto& compute = *a_frame.program->compute;
			auto& commands = a_recording.Commands();
			commands.BindLayout(compute.layout->GetHandle());
			commands.BindPipeline(compute.Pipeline());
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, count, words.data());
			commands.ExecuteIndirect(a_frame.program->signature->GetHandle(), a_frame.latch->Resource()->GetAPIResource().GetHandle(),
				a_frame.latch->Offset(slot) + offsetof(LatchData, groups) + a_frame.step * sizeof(LatchData::groups[0]), {}, 0, 1);
		}

	private:
		DispatchFrame frame;
		std::vector<Use> uses;
	};

	// A history texture cleared to zero: a history reset, as the D3D11 path clears its accumulation counts.
	struct ClearBindings
	{
		org::ResourceBindingToken resource;
		org::DeclaredViewToken shaderVisible, cpuVisible;
	};

	struct ClearFrame
	{};

	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit ClearPass(std::shared_ptr<org::Resource> a_texture) :
			texture(std::move(a_texture)) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = a_builder.UnorderedAccessClear(texture);
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(texture.get())); }

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return {}; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame&, org::PassRecordContext& a_recording)
		{
			rhi::UavClearInfo info{};
			info.shaderVisible = a_recording.View(a_bindings.shaderVisible);
			info.cpuVisible = a_recording.View(a_bindings.cpuVisible);
			info.resource = a_recording.Resolve(a_bindings.resource);
			const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			a_recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(zero));
		}

	private:
		std::shared_ptr<org::Resource> texture;
	};

	// Without the blur, the next frame reads the counts the disocclusion pass wrote, which it cannot write in place.
	struct CopyBindings
	{
		org::ResourceBindingToken source, destination;
	};

	struct CopyFrame
	{
		std::uint32_t width = 0, height = 0;
	};

	class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
	{
	public:
		CopyPass(std::shared_ptr<org::Resource> a_source, std::shared_ptr<org::Resource> a_destination, std::uint32_t a_width, std::uint32_t a_height) :
			source(std::move(a_source)), destination(std::move(a_destination)), frame{ a_width, a_height } {}

		CopyBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopySource(source), a_builder.CopyDestination(destination) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(source.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(destination.get()));
			a_out.push_back((std::uint64_t(frame.width) << 32) | frame.height);
		}

		CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return frame; }

		static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
		{
			rhi::TextureCopyRegion to{ .texture = a_recording.Resolve(a_bindings.destination).GetHandle(), .width = a_frame.width, .height = a_frame.height };
			rhi::TextureCopyRegion from{ .texture = a_recording.Resolve(a_bindings.source).GetHandle(), .width = a_frame.width, .height = a_frame.height };
			a_recording.Commands().CopyTextureRegion(to, from);
		}

	private:
		std::shared_ptr<org::Resource> source, destination;
		CopyFrame frame;
	};

	/** @brief The inputs as they are now: the final depth copy, the main target, the GBuffer normals, motion vectors and noise. */
	Inputs CurrentInputs()
	{
		auto& feature = globals::features::screenSpaceGI;
		Inputs inputs;
		// At DeferredPasses the D3D11 path reads Util::GetCurrentSceneDepthSRV(), which is the final depth copy by then.
		inputs.depth = FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY);
		if (globals::deferred)
			inputs.main = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(globals::deferred->forwardRenderTargets[0]));
		inputs.normalRoughness = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(NORMALROUGHNESS));
		inputs.motionVectors = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kMOTION_VECTOR));
		if (feature.texNoise)
			inputs.noise = FrameAnchors::D3D11Texture(kNoiseName, feature.texNoise->resource.get(), feature.texNoise->srv.get(), nullptr);
		return inputs;
	}

	/** @brief What the client's callbacks share: the current state, and whether the graph path failed. */
	struct Shared
	{
		std::shared_ptr<GiState> state;
		org::OwnedDescriptorBinding pointSampler, linearSampler;
		bool frameReady = false;
		bool failed = false;
	};

	std::shared_ptr<org::RenderPass> MakeStep(const std::shared_ptr<Shared>& a_shared, Step a_step)
	{
		const auto& state = a_shared->state;
		if (!state || !state->config.enabled || !state->programs[a_step])
			return nullptr;
		return std::make_shared<DispatchPass>(*state, a_step, a_shared->pointSampler.Index(), a_shared->linearSampler.Index());
	}
}

struct ORGScreenSpaceGI::Impl
{
	std::shared_ptr<Shared> shared = std::make_shared<Shared>();
	D3D11_TEXTURE2D_DESC main{};
	bool registered = false;

	/** @brief A texture of the chain, the size of the main target. */
	Texture MakeTexture(DXGI_FORMAT a_format, UINT a_mips, const char* a_name, GraphResources::Lifetime a_lifetime, bool a_exported) const
	{
		D3D11_TEXTURE2D_DESC desc{
			.Width = main.Width,
			.Height = main.Height,
			.MipLevels = a_mips,
			.ArraySize = 1,
			.Format = a_format,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		return std::make_unique<GraphResources::Texture2D>(desc, a_name,
			GraphResources::Options{ .lifetime = a_lifetime, .d3d11 = a_exported ? GraphResources::Export::SRV : GraphResources::Export::None });
	}

	/** @brief The programs and textures of a configuration; null, logged, when one cannot be made. */
	std::shared_ptr<GiState> Build(const Config& a_config) const
	{
		auto state = std::make_shared<GiState>();
		state->config = a_config;
		if (!a_config.enabled)
			return state;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return nullptr;
		for (std::uint32_t step = 0; step < kSteps; ++step) {
			if ((step == kBlur && !a_config.blur) || (step == kUpsample && a_config.FullRes()))
				continue;
			state->programs[step] = LoadProgram(host->Device(), static_cast<Step>(step), a_config);
			if (!state->programs[step]) {
				logger::error("[ORG] Screen Space GI: {} could not be built", kStepInfo[step].source);
				return nullptr;
			}
		}
		state->latch = std::make_shared<org::LatchBlock>("cs.ssgi.latch", static_cast<std::uint32_t>(sizeof(LatchData)), host->FrameSlots());

		using enum GraphResources::Lifetime;
		const bool full = a_config.FullRes();
		state->workingDepth = MakeTexture(DXGI_FORMAT_R16_FLOAT, kMips, "SSGI::WorkingDepth", Transient, false);
		state->normal = MakeTexture(DXGI_FORMAT_R8G8_UNORM, kMips, "SSGI::Normal", Transient, false);
		state->radiance = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, kMips, "SSGI::Radiance", Transient, false);
		state->radianceTemp = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::RadianceTemp", Transient, false);
		state->accumScratch = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFramesScratch", Transient, false);
		state->accum = MakeTexture(DXGI_FORMAT_R8_UNORM, 1, "SSGI::AccumFrames", History, false);
		// 8-bit history stalls the temporal AO blend.
		state->ao = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AO", History, full);
		state->aoScratch = MakeTexture(DXGI_FORMAT_R16_UNORM, 1, "SSGI::AOScratch", Transient, !full);
		state->y = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlY", History, full);
		state->yScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYScratch", Transient, !full);
		state->coCg = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCg", History, full);
		state->coCgScratch = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgScratch", Transient, !full);
		if (a_config.blur) {
			state->yBlurInput = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::IlYBlurInput", Transient, false);
			state->coCgBlurInput = MakeTexture(DXGI_FORMAT_R16G16_FLOAT, 1, "SSGI::IlCoCgBlurInput", Transient, false);
		}
		if (a_config.SpecularTextures()) {
			state->spec = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecular", History, full);
			state->specScratch = MakeTexture(DXGI_FORMAT_R16G16B16A16_FLOAT, 1, "SSGI::GiSpecularScratch", Transient, !full);
		}
		state->prevGeo = MakeTexture(DXGI_FORMAT_R11G11B10_FLOAT, 1, "SSGI::PrevGeo", History, false);
		for (const auto* texture : state->All()) {
			// Only the optional ones may be missing, and only when their option is off.
			if (*texture && !**texture) {
				logger::error("[ORG] Screen Space GI: {} could not be made", (*texture)->Name());
				return nullptr;
			}
		}
		for (const Texture* exported : { &state->OutAo(), &state->OutY(), &state->OutCoCg() }) {
			if (!*exported || !(*exported)->SRV()) {
				logger::error("[ORG] Screen Space GI: the composite's inputs could not be exported to D3D11");
				return nullptr;
			}
		}
		return state;
	}

	bool Register()
	{
		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.sharedConstants = true;
		const auto owner = this->shared;
		// The history clears, in builds after a reset only.
		for (std::size_t i = 0; i < kHistoryTextures; ++i) {
			client.passes.push_back({ .name = std::format("cs.ssgi.reset.{}", i),
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.order = kOrderReset,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [owner, i]() -> std::shared_ptr<org::RenderPass> {
					const auto& state = owner->state;
					if (!state || !state->config.enabled || !state->resetPending)
						return nullptr;
					const auto& texture = *state->History()[i];
					if (!texture)
						return nullptr;
					state->clearBuilt = true;
					return std::make_shared<ClearPass>(texture->Graph());
				} });
		}
		for (std::uint32_t step = 0; step < kSteps; ++step) {
			client.passes.push_back({ .name = kStepInfo[step].pass,
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.order = kOrderFirstStep + static_cast<std::int32_t>(step),
				.kind = FrameAnchors::PassKind::Compute,
				.make = [owner, step]() { return MakeStep(owner, static_cast<Step>(step)); } });
		}
		client.passes.push_back({ .name = "cs.ssgi.keep-accumulation",
			.anchor = FrameAnchors::Anchor::DeferredPasses,
			.order = kOrderFirstStep + kSteps,
			.kind = FrameAnchors::PassKind::Copy,
			.make = [owner]() -> std::shared_ptr<org::RenderPass> {
				const auto& state = owner->state;
				if (!state || !state->config.enabled || state->config.blur)
					return nullptr;
				return std::make_shared<CopyPass>(state->accumScratch->Graph(), state->accum->Graph(), state->accum->Desc().Width,
					state->accum->Desc().Height);
			} });
		client.registerResources = [owner](org::RenderGraph& a_graph) {
			if (owner->state)
				for (const auto* texture : owner->state->All())
					if (*texture)
						(*texture)->Register(a_graph);
		};
		client.commit = [owner](const FrameAnchors::Point&) {
			const auto& state = owner->state;
			if (!state || !state->config.enabled || !state->latch)
				return;
			// The build that just ran holds the clears.
			if (state->clearBuilt)
				state->resetPending = false;
			auto& feature = globals::features::screenSpaceGI;
			LatchData data{};
			data.constants = feature.BuildConstants({ static_cast<float>(state->ao->Desc().Width), static_cast<float>(state->ao->Desc().Height) });
			const auto sizes = feature.GetDispatchSizes();
			const auto groups = [&](Step a_step, const std::array<uint, 2>& a_size, uint a_tile) {
				data.groups[a_step][0] = (a_size[0] + a_tile - 1) / a_tile;
				data.groups[a_step][1] = (a_size[1] + a_tile - 1) / a_tile;
				data.groups[a_step][2] = 1;
			};
			groups(kPrefilterDepths, sizes.full, 16);
			groups(kRadianceDisocc, sizes.internal, 8);
			groups(kPrefilterRadiance, sizes.internal, 16);
			groups(kPrefilterNormal, sizes.internal, 16);
			groups(kGi, sizes.internal, 8);
			groups(kBlur, sizes.internal, 8);
			groups(kUpsample, sizes.full, 8);
			state->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, data);
		};
		client.active = [owner] { return owner->frameReady; };
		client.onFailed = [owner] {
			logger::error("[ORG] Screen Space GI returns to D3D11: the render graph failed");
			owner->failed = true;
			owner->frameReady = false;
		};
		registered = FrameAnchors::Register(std::move(client));
		return registered;
	}
};

bool ORGScreenSpaceGI::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_SCREEN_SPACE_GI", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGScreenSpaceGI& ORGScreenSpaceGI::Get()
{
	static ORGScreenSpaceGI s_gi;
	return s_gi;
}

ORGScreenSpaceGI::~ORGScreenSpaceGI() = default;

void ORGScreenSpaceGI::Reserve()
{
	if (Requested())
		FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses);
}

void ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC& a_main)
{
	impl.reset();
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses)) {
		logger::warn("[ORG] Screen Space GI stays on D3D11: the render graph or its DeferredPasses anchor is not available");
		return;
	}
	try {
		auto next = std::make_unique<Impl>();
		next->main = a_main;
		auto& shared = *next->shared;
		shared.pointSampler = ShaderBindings::CreateSampler({ .minFilter = rhi::Filter::Nearest,
			.magFilter = rhi::Filter::Nearest,
			.mipFilter = rhi::MipFilter::Nearest });
		shared.linearSampler = ShaderBindings::CreateSampler({});
		shared.state = next->Build(Config::Current());
		if (!shared.pointSampler || !shared.linearSampler || !shared.state) {
			logger::warn("[ORG] Screen Space GI stays on D3D11: its samplers, programs or textures could not be made");
			return;
		}
		impl = std::move(next);
		logger::info("[ORG] Screen Space GI runs on the render graph ({}x{}, resolution mode {}, blur {})", a_main.Width, a_main.Height,
			shared.state->config.resolutionMode, shared.state->config.blur);
	} catch (const std::exception& e) {
		logger::error("[ORG] Screen Space GI setup failed, staying on D3D11: {}", e.what());
		impl.reset();
	}
}

void ORGScreenSpaceGI::Prepass()
{
	if (!impl)
		return;
	auto& shared = *impl->shared;
	shared.frameReady = false;
	if (shared.failed || !RenderGraphRuntime::Get().IsActive())
		return;
	auto& feature = globals::features::screenSpaceGI;
	bool refresh = false;

	// A setting the programs or textures depend on, or a recompile from the menu: both made again, and the history with them.
	bool rebuild = false;
	if (feature.recompileFlag) {
		// The D3D11 shaders too, so a later fallback finds them current. It also queues the history reset.
		feature.ClearShaderCache();
		rebuild = true;
	}
	const auto config = Config::Current();
	if (rebuild || config != shared.state->config) {
		auto state = impl->Build(config);
		if (!state) {
			logger::error("[ORG] Screen Space GI returns to D3D11: the new settings' programs or textures could not be made");
			shared.failed = true;
			return;
		}
		state->inputs = shared.state->inputs;
		shared.state = std::move(state);
		refresh = true;
	}
	auto& state = *shared.state;
	if (!state.config.enabled) {
		if (refresh && impl->registered)
			FrameAnchors::Refresh(kClientId);
		return;
	}

	// A new engine texture (a resolution change) or a new noise texture.
	if (auto inputs = CurrentInputs(); !(inputs == state.inputs)) {
		if (!inputs) {
			logger::error("[ORG] Screen Space GI returns to D3D11: an input could not be imported");
			shared.failed = true;
			return;
		}
		state.inputs = std::move(inputs);
		refresh = true;
	}

	if (feature.queuedResetHistory.exchange(false)) {
		state.resetPending = true;
		refresh = true;
	} else if (state.clearBuilt && !state.resetPending) {
		// The clears ran: the next build leaves them out.
		state.clearBuilt = false;
		refresh = true;
	}

	if (!impl->registered) {
		// Registered at the first frame: the shared constants exist once State has uploaded them.
		if (!impl->Register()) {
			logger::error("[ORG] Screen Space GI returns to D3D11: its anchor client could not be registered");
			shared.failed = true;
			return;
		}
	} else if (refresh) {
		FrameAnchors::Refresh(kClientId);
	}
	shared.frameReady = true;
}

bool ORGScreenSpaceGI::Draw()
{
	if (!impl)
		return false;
	if (impl->shared->failed) {
		FrameAnchors::Unregister(kClientId);
		impl.reset();
		logger::warn("[ORG] Screen Space GI: drawing with D3D11 from now on");
		return false;
	}
	return true;
}

bool ORGScreenSpaceGI::Running() const
{
	return impl && !impl->shared->failed;
}

ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const
{
	if (!impl || !impl->shared->state || !impl->shared->state->config.enabled)
		return {};
	const auto& state = *impl->shared->state;
	const auto srv = [](const Texture& a_texture) { return a_texture ? a_texture->SRV() : nullptr; };
	return { srv(state.OutAo()), srv(state.OutY()), srv(state.OutCoCg()), srv(state.OutSpec()) };
}

ORGScreenSpaceGI::GraphOutputs ORGScreenSpaceGI::GetGraphOutputs() const
{
	if (!impl || !impl->shared->state || !impl->shared->state->config.enabled)
		return {};
	const auto& state = *impl->shared->state;
	return { GraphOf(state.OutAo()), GraphOf(state.OutY()), GraphOf(state.OutCoCg()), GraphOf(state.OutSpec()) };
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: Screen Space GI always draws with D3D11.
#	include "ORGScreenSpaceGI.h"

struct ORGScreenSpaceGI::Impl
{};

bool ORGScreenSpaceGI::Requested() { return false; }

ORGScreenSpaceGI& ORGScreenSpaceGI::Get()
{
	static ORGScreenSpaceGI s_gi;
	return s_gi;
}

ORGScreenSpaceGI::~ORGScreenSpaceGI() = default;
void ORGScreenSpaceGI::Reserve() {}
void ORGScreenSpaceGI::Setup(const D3D11_TEXTURE2D_DESC&) {}
void ORGScreenSpaceGI::Prepass() {}
bool ORGScreenSpaceGI::Draw() { return false; }
bool ORGScreenSpaceGI::Running() const { return false; }
ORGScreenSpaceGI::Outputs ORGScreenSpaceGI::GetOutputs() const { return {}; }
ORGScreenSpaceGI::GraphOutputs ORGScreenSpaceGI::GetGraphOutputs() const { return {}; }

#endif
