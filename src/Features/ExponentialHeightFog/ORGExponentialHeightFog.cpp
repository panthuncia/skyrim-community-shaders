#include "ORGExponentialHeightFog.h"

#include <cstring>

bool ORGExponentialHeightFog::Requested()
{
	static const bool requested = [] {
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_EXPONENTIAL_HEIGHT_FOG", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}();
	return requested;
}

ORGExponentialHeightFog& ORGExponentialHeightFog::Get()
{
	static ORGExponentialHeightFog s_fog;
	return s_fog;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Deferred.h"
#	include "Features/CloudShadows.h"
#	include "Features/CloudShadows/ORGCloudShadows.h"
#	include "Features/ExponentialHeightFog.h"
#	include "Features/IBL.h"
#	include "Features/IBL/ORGIBL.h"
#	include "Features/LightLimitFix.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "Features/LightLimitFix/ORGLocalShadows.h"
#	include "Features/Skylighting.h"
#	include "Features/Skylighting/ORGSkylighting.h"
#	include "Features/TerrainBlending.h"
#	include "Features/TerrainShadows.h"
#	include "Features/TerrainShadows/ORGTerrainShadows.h"
#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>

#	include <array>
#	include <atomic>
#	include <cstddef>
#	include <functional>
#	include <map>
#	include <optional>
#	include <string>
#	include <vector>

namespace
{
	constexpr const char* kClientId = "cs.exp-height-fog";
	constexpr const char* kShaderDir = "ExponentialHeightFog/";
	// After IBL (0, 1) and Skylighting at the Prepass anchor; the graph orders the reads of their outputs in any case.
	constexpr std::int32_t kOrder = 300;

	using ShaderBindings::Handle;

	// The frame's values, in its latch region: the shaders' LATCH entries, and the directional shadow light the upload pass
	// copies into its buffer (t98).
	struct LatchData
	{
		ExponentialHeightFog::VolumetricFogCB fog;
		ExponentialHeightFog::EngineLocalShadows engineShadows;
		Deferred::DirectionalShadowLightData directionalShadow;
	};
	static_assert(offsetof(LatchData, engineShadows) % 16 == 0 && offsetof(LatchData, directionalShadow) % 16 == 0);

	// The shaders' push blocks, as their resource lists give them (check-dual-target-shaders reports the sizes).
	struct VolumeBlock  // VolumetricFogMaterialCS, VolumetricFogConservativeDepthCS
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle output, depth;
	};
	static_assert(sizeof(VolumeBlock) == 32);

	struct IntegrationBlock  // VolumetricFogIntegrationCS
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle lightScattering, integrated, depth;
	};
	static_assert(sizeof(IntegrationBlock) == 36);

	struct ScatteringBlock  // VolumetricFogLightScatteringCS, with Light Limit Fix's tail
	{
		SharedConstants::Handles shared;
		ShaderBindings::Latch fog;
		Handle linearSampler, shadowSampler;
		Handle vBuffer, directionalShadowMap, history, conservativeDepth, previousConservativeDepth;
		Handle lightScattering;
		Handle directionalShadowLights;
		Handle depth, envIBL, skyIBL, probes, shadowHeights, cloudShadows;
		Handle lights, lightList, lightGrid, localShadows, localShadowMaps;
		Handle engineShadowMaps;
		ShaderBindings::Latch engineShadows;
	};
	static_assert(sizeof(ScatteringBlock) == 116);

	/** @brief A graph resource a pass reads: one the client holds or imported, or one another client registers by name. */
	struct Source
	{
		std::shared_ptr<org::Resource> resource;
		std::string name;
		// A texture array the shaders read whole (Texture2DArray): its whole-array view, not the default first slice.
		bool wholeArray = false;

		explicit operator bool() const { return resource || !name.empty(); }
		bool operator==(const Source&) const = default;
	};

	/** @brief The light scattering pass's inputs that are not the client's own. */
	enum Input : std::uint32_t
	{
		kDepth,
		kDirectionalShadowMap,
		kEnvIBL,
		kSkyIBL,
		kProbes,
		kShadowHeights,
		kCloudShadows,
		kLights,
		kLightList,
		kLightGrid,
		kLocalShadows,
		kLocalShadowMaps,
		kEngineShadowMaps,
		kInputs
	};

	/** @brief What the graph is built from: a change makes a new client. */
	struct Shape
	{
		ExponentialHeightFog::GridSizes sizes;
		bool farEnabled = false;
		bool temporal = false;
		std::array<Source, kInputs> inputs;

		bool operator==(const Shape& a_other) const
		{
			const auto same = [](const DirectX::XMUINT4& a, const DirectX::XMUINT4& b) { return a.x == b.x && a.y == b.y && a.z == b.z; };
			return same(sizes.nearSize, a_other.sizes.nearSize) && same(sizes.farSize, a_other.sizes.farSize) &&
			       sizes.nearGroupReduce == a_other.sizes.nearGroupReduce && sizes.farGroupReduce == a_other.sizes.farGroupReduce &&
			       farEnabled == a_other.farEnabled && temporal == a_other.temporal && inputs == a_other.inputs;
		}
	};

	/** @brief One volume's programs and textures. */
	struct Volume
	{
		std::shared_ptr<const ComputeProgram> depthProgram, materialProgram, scatteringProgram, integrationProgram;
		std::unique_ptr<GraphResources::Texture3D> vBuffer, scattering, scatteringHistory, integrated;
		std::unique_ptr<GraphResources::Texture2D> conservativeDepth, conservativeDepthHistory;
		DirectX::XMUINT4 size{};
		bool groupReduce = false;
	};

	/** @brief Everything a build's passes are made from: immutable once registered. */
	struct FogState
	{
		Shape shape;
		Volume nearVolume, farVolume;
		std::unique_ptr<GraphResources::StructuredBuffer> directionalShadowLights;
		std::shared_ptr<org::LatchBlock> latch;
		org::OwnedDescriptorBinding linearSampler, shadowSampler;
		// Whether this run has the client's frame (set at the hook): a run another client keeps going records nothing here.
		std::shared_ptr<std::atomic<bool>> live = std::make_shared<std::atomic<bool>>(false);

		bool Live() const { return live->load(std::memory_order_acquire); }
	};

	org::DeclaredViewToken Read(org::PassBuilder& a_builder, const Source& a_source)
	{
		org::SrvView view{};
		if (a_source.wholeArray)
			view.variant = static_cast<std::uint32_t>(org::SRVViewType::Texture2DArrayFull);
		if (a_source.resource)
			return a_builder.ShaderResource(a_source.resource, view).View();
		return a_builder.ShaderResource(org::ResourceIdentifier(a_source.name), view).View();
	}

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Passes
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/** @brief One of the volume compute passes; which one and for which volume is fixed when it is made. */
	enum class Step : std::uint8_t
	{
		Depth,
		Material,
		Scattering,
		Integration
	};

	struct DispatchBindings
	{
		SharedConstants::Bindings shared;
		org::DeclaredViewToken output;
		std::array<org::DeclaredViewToken, 6> own{};  // the step's reads of the client's resources
		std::array<org::DeclaredViewToken, kInputs> inputs{};
		std::array<bool, kInputs> hasInput{};
	};

	struct DispatchFrame
	{
		std::shared_ptr<const FogState> state;
		const Volume* volume = nullptr;
		Step step = Step::Material;
	};

	class DispatchPass final : public org::TypedRenderGraphPass<DispatchPass, DispatchFrame, DispatchBindings>
	{
	public:
		DispatchPass(std::shared_ptr<const FogState> a_state, bool a_far, Step a_step) :
			state(std::move(a_state)), isFar(a_far), step(a_step) {}

		DispatchBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& v = Of();
			DispatchBindings b{};
			b.shared = SharedConstants::Declare(a_builder);
			switch (step) {
			case Step::Depth:
				b.output = a_builder.UnorderedAccess(v.conservativeDepth->Graph()).View();
				b.inputs[kDepth] = Read(a_builder, state->shape.inputs[kDepth]);
				b.hasInput[kDepth] = true;
				break;
			case Step::Material:
				b.output = a_builder.UnorderedAccess(v.vBuffer->Graph()).View();
				break;
			case Step::Scattering:
				b.output = a_builder.UnorderedAccess(v.scattering->Graph()).View();
				b.own[0] = a_builder.ShaderResource(v.vBuffer->Graph()).View();
				b.own[1] = a_builder.ShaderResource(v.scatteringHistory->Graph()).View();
				b.own[2] = a_builder.ShaderResource(v.conservativeDepth->Graph()).View();
				b.own[3] = a_builder.ShaderResource(v.conservativeDepthHistory->Graph()).View();
				b.own[4] = a_builder.ShaderResource(state->directionalShadowLights->Graph()).View();
				// The far volume reads no local lights (its shader leaves them out), the near one every input there is.
				for (std::uint32_t i = 0; i < kInputs; ++i) {
					const bool local = i >= kLights && i <= kEngineShadowMaps;
					if (i == kDepth || (isFar && local) || !state->shape.inputs[i])
						continue;
					b.inputs[i] = Read(a_builder, state->shape.inputs[i]);
					b.hasInput[i] = true;
				}
				break;
			case Step::Integration:
				b.output = a_builder.UnorderedAccess(v.integrated->Graph()).View();
				b.own[0] = a_builder.ShaderResource(v.scattering->Graph()).View();
				break;
			}
			return b;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(state->Live());
		}

		DispatchFrame Prepare(const DispatchBindings&, const org::PassPrepareContext&) const
		{
			if (!state->Live())
				return {};
			return { state, &Of(), step };
		}

		static void Record(const DispatchBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state || !a_frame.volume)
				return;
			const auto& s = *a_frame.state;
			const auto& v = *a_frame.volume;
			const std::uint32_t slot = a_recording.FrameSlot();
			const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
			const auto fog = ShaderBindings::LatchOf(*s.latch, slot, static_cast<std::uint32_t>(offsetof(LatchData, fog)));
			const std::uint32_t groupsX = (v.size.x + 7) / 8;
			const std::uint32_t groupsY = (v.size.y + 7) / 8;
			const std::uint32_t groupsZ = (v.size.z + 3) / 4;
			const auto input = [&](Input a_input) -> Handle { return a_bindings.hasInput[a_input] ? a_recording.Index(a_bindings.inputs[a_input]) : 0; };
			switch (a_frame.step) {
			case Step::Depth:
				{
					const VolumeBlock block{ shared, fog, a_recording.Index(a_bindings.output), input(kDepth) };
					if (v.groupReduce)
						v.depthProgram->Dispatch(a_recording, ShaderBindings::Data(block), v.size.x, v.size.y, 1);
					else
						v.depthProgram->Dispatch(a_recording, ShaderBindings::Data(block), groupsX, groupsY, 1);
					break;
				}
			case Step::Material:
				{
					const VolumeBlock block{ shared, fog, a_recording.Index(a_bindings.output), 0 };
					v.materialProgram->Dispatch(a_recording, ShaderBindings::Data(block), groupsX, groupsY, groupsZ);
					break;
				}
			case Step::Scattering:
				{
					ScatteringBlock block{};
					block.shared = shared;
					block.fog = fog;
					block.linearSampler = s.linearSampler.Index();
					block.shadowSampler = s.shadowSampler.Index();
					block.vBuffer = a_recording.Index(a_bindings.own[0]);
					block.directionalShadowMap = input(kDirectionalShadowMap);
					block.history = a_recording.Index(a_bindings.own[1]);
					block.conservativeDepth = a_recording.Index(a_bindings.own[2]);
					block.previousConservativeDepth = a_recording.Index(a_bindings.own[3]);
					block.lightScattering = a_recording.Index(a_bindings.output);
					block.directionalShadowLights = a_recording.Index(a_bindings.own[4]);
					block.envIBL = input(kEnvIBL);
					block.skyIBL = input(kSkyIBL);
					block.probes = input(kProbes);
					block.shadowHeights = input(kShadowHeights);
					block.cloudShadows = input(kCloudShadows);
					block.lights = input(kLights);
					block.lightList = input(kLightList);
					block.lightGrid = input(kLightGrid);
					block.localShadows = input(kLocalShadows);
					block.localShadowMaps = input(kLocalShadowMaps);
					block.engineShadowMaps = input(kEngineShadowMaps);
					block.engineShadows = ShaderBindings::LatchOf(*s.latch, slot, static_cast<std::uint32_t>(offsetof(LatchData, engineShadows)));
					v.scatteringProgram->Dispatch(a_recording, ShaderBindings::Data(block), groupsX, groupsY, groupsZ);
					break;
				}
			case Step::Integration:
				{
					const IntegrationBlock block{ shared, fog, a_recording.Index(a_bindings.own[0]), a_recording.Index(a_bindings.output), 0 };
					v.integrationProgram->Dispatch(a_recording, ShaderBindings::Data(block), groupsX, groupsY, 1);
					break;
				}
			}
		}

	private:
		const Volume& Of() const { return isFar ? state->farVolume : state->nearVolume; }

		std::shared_ptr<const FogState> state;
		bool isFar;
		Step step;
	};

	// The directional shadow light, from the latch into its buffer, as Deferred::CopyShadowLightData maps it on D3D11.
	struct UploadBindings
	{
		org::ResourceBindingToken buffer;
	};

	struct UploadFrame
	{
		std::shared_ptr<const FogState> state;
	};

	class UploadPass final : public org::TypedRenderGraphPass<UploadPass, UploadFrame, UploadBindings>
	{
	public:
		explicit UploadPass(std::shared_ptr<const FogState> a_state) :
			state(std::move(a_state)) {}

		UploadBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			return { a_builder.CopyDestination(state->directionalShadowLights->Graph()) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(state->Live());
		}

		UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const
		{
			if (!state->Live())
				return {};
			return { state };
		}

		static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.state)
				return;
			const auto& latch = *a_frame.state->latch;
			a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.buffer).GetHandle(), 0,
				latch.Resource()->GetAPIResource().GetHandle(), latch.Offset(a_recording.FrameSlot()) + offsetof(LatchData, directionalShadow),
				sizeof(Deferred::DirectionalShadowLightData));
		}

	private:
		std::shared_ptr<const FogState> state;
	};

	// A volume's scattering and conservative depth into their histories, as the D3D11 path's CopyResource calls.
	struct HistoryBindings
	{
		org::ResourceBindingToken scattering, scatteringHistory, depth, depthHistory;
	};

	struct HistoryFrame
	{
		const Volume* volume = nullptr;
	};

	class HistoryPass final : public org::TypedRenderGraphPass<HistoryPass, HistoryFrame, HistoryBindings>
	{
	public:
		HistoryPass(std::shared_ptr<const FogState> a_state, bool a_far) :
			state(std::move(a_state)), isFar(a_far) {}

		HistoryBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto& v = Of();
			return { a_builder.CopySource(v.scattering->Graph()), a_builder.CopyDestination(v.scatteringHistory->Graph()),
				a_builder.CopySource(v.conservativeDepth->Graph()), a_builder.CopyDestination(v.conservativeDepthHistory->Graph()) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(state->Live());
		}

		HistoryFrame Prepare(const HistoryBindings&, const org::PassPrepareContext&) const
		{
			if (!state->Live())
				return {};
			return { &Of() };
		}

		static void Record(const HistoryBindings& a_bindings, const HistoryFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.volume)
				return;
			const auto& size = a_frame.volume->size;
			auto& commands = a_recording.Commands();
			const auto copy = [&](org::ResourceBindingToken a_from, org::ResourceBindingToken a_to, std::uint32_t a_depth) {
				const rhi::TextureCopyRegion to{ .texture = a_recording.Resolve(a_to).GetHandle(), .width = size.x, .height = size.y, .depth = a_depth };
				const rhi::TextureCopyRegion from{ .texture = a_recording.Resolve(a_from).GetHandle(), .width = size.x, .height = size.y, .depth = a_depth };
				commands.CopyTextureRegion(to, from);
			};
			copy(a_bindings.scattering, a_bindings.scatteringHistory, size.z);
			copy(a_bindings.depth, a_bindings.depthHistory, 1);
		}

	private:
		const Volume& Of() const { return isFar ? state->farVolume : state->nearVolume; }

		std::shared_ptr<const FogState> state;
		bool isFar;
	};

	// The far volume's integrated scattering cleared to no fog (transmittance 1), as the D3D11 path clears it with the far volume off.
	struct ClearBindings
	{
		org::ResourceBindingToken resource;
		org::DeclaredViewToken shaderVisible, cpuVisible;
	};

	struct ClearFrame
	{
		bool live = false;
	};

	class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
	{
	public:
		explicit ClearPass(std::shared_ptr<const FogState> a_state) :
			state(std::move(a_state)), target(state->farVolume.integrated->Graph()) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = a_builder.UnorderedAccessClear(target);
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(state.get()));
			a_out.push_back(state->Live());
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { state->Live() }; }

		static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.live)
				return;
			rhi::UavClearInfo info{};
			info.shaderVisible = a_recording.View(a_bindings.shaderVisible);
			info.cpuVisible = a_recording.View(a_bindings.cpuVisible);
			info.resource = a_recording.Resolve(a_bindings.resource);
			const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			a_recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(clear));
		}

	private:
		std::shared_ptr<const FogState> state;
		std::shared_ptr<org::Resource> target;
	};

	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
	// Inputs
	// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

	/** @brief A D3D11 feature's 2D texture behind a_view, imported (FrameAnchors::D3D11Texture). Empty without one. */
	Source ImportView(std::string_view a_name, ID3D11ShaderResourceView* a_view)
	{
		if (!a_view)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return {};
		return { FrameAnchors::D3D11Texture(a_name, texture.get(), a_view, nullptr), {} };
	}

	/** @brief A depth texture behind a_view: the engine's depth-stencil import when it is one of the engine's, else its own. */
	Source ImportDepthView(ID3D11ShaderResourceView* a_view, const char* a_id)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return {};
		D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
		a_view->GetDesc(&viewDesc);
		const bool wholeArray = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
		const auto& depthStencils = globals::game::renderer->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture == texture.get())
				return { FrameAnchors::EngineDepth(i), {}, wholeArray };
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = a_view;
		return { FrameAnchors::ImportDepth(target, a_id), {}, wholeArray };
	}

	/** @brief The scene depth the D3D11 path reads (Util::GetCurrentSceneDepthSRV(true)). */
	Source SceneDepth(ID3D11ShaderResourceView* a_view)
	{
		if (!a_view || !globals::game::renderer)
			return {};
		if (a_view == globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV)
			return { FrameAnchors::EngineDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY), {} };
		// Terrain Blending's 16-bit blended depth.
		return ImportView("TerrainBlending::BlendedDepth16", a_view);
	}

	/** @brief The defines the D3D11 build gets (ExponentialHeightFog's Get*CS and Util::CompileShader), as ComputeProgram takes them. */
	std::vector<std::pair<std::wstring, std::wstring>> Defines(std::vector<std::string> a_names)
	{
		std::vector<std::pair<std::string, std::string>> defines;
		for (auto& name : a_names)
			defines.emplace_back(std::move(name), "");
		std::vector<std::string_view> explicitNames;
		for (const auto& define : defines)
			explicitNames.push_back(define.first);
		for (auto& define : Util::GetCommonShaderDefines(explicitNames))
			defines.push_back(std::move(define));
		for (const char* name : { "COMPUTESHADER", "WINPC", "DX11" })
			defines.emplace_back(name, "");
		// Defines are ASCII.
		std::vector<std::pair<std::wstring, std::wstring>> wide;
		for (const auto& [name, value] : defines)
			wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		return wide;
	}

	D3D11_TEXTURE3D_DESC VolumeDesc(const DirectX::XMUINT4& a_size, DXGI_FORMAT a_format)
	{
		D3D11_TEXTURE3D_DESC desc{};
		desc.Width = a_size.x;
		desc.Height = a_size.y;
		desc.Depth = a_size.z;
		desc.MipLevels = 1;
		desc.Format = a_format;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}

	D3D11_TEXTURE2D_DESC DepthDesc(const DirectX::XMUINT4& a_size)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_size.x;
		desc.Height = a_size.y;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R32_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		return desc;
	}
}

struct ORGExponentialHeightFog::Impl
{
	std::shared_ptr<const FogState> state;
	// Programs by source and defines, kept across shapes: a grid change does not compile again.
	std::map<std::pair<std::string, std::string>, std::shared_ptr<const ComputeProgram>> programs;
	// This frame's values, written at Prepass and latched by the commit.
	std::optional<LatchData> pending;
	// The histories' state, as the D3D11 path keeps its own.
	bool hasScatteringHistory = false, hasDepthHistory = false, hasScatteringHistoryFar = false, hasDepthHistoryFar = false;
	std::uint32_t lastFrame = UINT32_MAX;
	bool ranLastFrame = false;
	std::string fallbackReason;  // why the last frame ran on D3D11, logged when it changes

	std::shared_ptr<const ComputeProgram> Program(const char* a_source, std::vector<std::string> a_defines, std::uint32_t a_words)
	{
		std::string key;
		for (const auto& define : a_defines)
			key += define + ";";
		auto& program = programs[{ a_source, key }];
		if (!program)
			program = ComputeProgram::Load(RenderGraphRuntime::Get().Host()->Device(),
				{ .source = std::string(kShaderDir) + a_source, .constantWords = a_words, .defines = Defines(std::move(a_defines)) });
		return program;
	}

	bool MakeVolume(Volume& a_volume, const DirectX::XMUINT4& a_size, bool a_groupReduce, bool a_far)
	{
		const auto& lightLimitFix = globals::features::lightLimitFix;
		std::vector<std::string> scatteringDefines;
		if (a_far)
			scatteringDefines.emplace_back("VOLUMETRIC_FOG_FAR_GRID");
		if (lightLimitFix.loaded)
			scatteringDefines.emplace_back("LIGHT_LIMIT_FIX");
		if (globals::features::terrainShadows.loaded)
			scatteringDefines.emplace_back("TERRAIN_SHADOWS");
		if (globals::features::cloudShadows.loaded)
			scatteringDefines.emplace_back("CLOUD_SHADOWS");
		std::vector<std::string> volumeDefines;
		if (a_far)
			volumeDefines.emplace_back("VOLUMETRIC_FOG_FAR_GRID");
		auto depthDefines = volumeDefines;
		if (a_groupReduce)
			depthDefines.emplace_back("GROUP_REDUCE");

		a_volume.size = a_size;
		a_volume.groupReduce = a_groupReduce;
		a_volume.depthProgram = Program("VolumetricFogConservativeDepthCS.hlsl", depthDefines, ShaderBindings::Words<VolumeBlock>());
		a_volume.materialProgram = Program("VolumetricFogMaterialCS.hlsl", volumeDefines, ShaderBindings::Words<VolumeBlock>());
		a_volume.scatteringProgram = Program("VolumetricFogLightScatteringCS.hlsl", scatteringDefines, ShaderBindings::Words<ScatteringBlock>());
		a_volume.integrationProgram = Program("VolumetricFogIntegrationCS.hlsl", volumeDefines, ShaderBindings::Words<IntegrationBlock>());
		if (!a_volume.depthProgram || !a_volume.materialProgram || !a_volume.scatteringProgram || !a_volume.integrationProgram) {
			logger::error("[ORG] Exponential Height Fog: a volumetric fog program could not be built");
			return false;
		}

		const std::string prefix = a_far ? "ExponentialHeightFog::Far" : "ExponentialHeightFog::";
		const GraphResources::Options transient{ .lifetime = GraphResources::Lifetime::Transient };
		const GraphResources::Options history{ .lifetime = GraphResources::Lifetime::History };
		// Written every frame, but the engine's draws read them through D3D11: exported, so never aliased.
		const GraphResources::Options exported{ .lifetime = GraphResources::Lifetime::Transient, .d3d11 = GraphResources::Export::SRV };
		a_volume.vBuffer = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R16G16B16A16_FLOAT), prefix + "VBufferA", transient);
		a_volume.scattering = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R32G32B32A32_FLOAT), prefix + "LightScattering", transient);
		a_volume.scatteringHistory = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R32G32B32A32_FLOAT), prefix + "LightScatteringHistory", history);
		a_volume.integrated = std::make_unique<GraphResources::Texture3D>(VolumeDesc(a_size, DXGI_FORMAT_R16G16B16A16_FLOAT), prefix + "IntegratedLightScattering", exported);
		a_volume.conservativeDepth = std::make_unique<GraphResources::Texture2D>(DepthDesc(a_size), prefix + "ConservativeDepth", transient);
		a_volume.conservativeDepthHistory = std::make_unique<GraphResources::Texture2D>(DepthDesc(a_size), prefix + "ConservativeDepthHistory", history);
		if (!*a_volume.vBuffer || !*a_volume.scattering || !*a_volume.scatteringHistory || !*a_volume.integrated || !a_volume.integrated->SRV() ||
			!*a_volume.conservativeDepth || !*a_volume.conservativeDepthHistory) {
			logger::error("[ORG] Exponential Height Fog: the {} volume's textures could not be made", a_far ? "far" : "near");
			return false;
		}
		return true;
	}

	/** @brief A new state for a_shape, registered as the client. False, logged, when anything could not be made. */
	bool Build(const Shape& a_shape)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto built = std::make_shared<FogState>();
		built->shape = a_shape;
		if (!MakeVolume(built->nearVolume, a_shape.sizes.nearSize, a_shape.sizes.nearGroupReduce, false) ||
			!MakeVolume(built->farVolume, a_shape.sizes.farSize, a_shape.sizes.farGroupReduce, true))
			return false;
		D3D11_BUFFER_DESC bufferDesc{};
		bufferDesc.Usage = D3D11_USAGE_DEFAULT;
		bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		bufferDesc.StructureByteStride = sizeof(Deferred::DirectionalShadowLightData);
		bufferDesc.ByteWidth = sizeof(Deferred::DirectionalShadowLightData);
		built->directionalShadowLights = std::make_unique<GraphResources::StructuredBuffer>(bufferDesc, 1, "ExponentialHeightFog::DirectionalShadowLights",
			GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
		built->latch = std::make_shared<org::LatchBlock>("cs.exp-height-fog.latch", static_cast<std::uint32_t>(sizeof(LatchData)), host->FrameSlots());
		// The D3D11 path's samplers: linear clamp, and the comparison sampler for the directional shadow map.
		built->linearSampler = ShaderBindings::CreateSampler({});
		built->shadowSampler = ShaderBindings::CreateSampler({ .compareEnable = true, .compareOp = rhi::CompareOp::LessEqual, .reduction = rhi::ReductionMode::Comparison });
		if (!*built->directionalShadowLights || !built->linearSampler || !built->shadowSampler) {
			logger::error("[ORG] Exponential Height Fog: the shadow light buffer or a sampler could not be made");
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		const std::shared_ptr<const FogState> shared = built;
		std::int32_t order = kOrder;
		const auto add = [&](std::string a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make) {
			client.passes.push_back({ .name = "cs.exp-height-fog." + std::move(a_name), .anchor = FrameAnchors::Anchor::Prepass, .order = order++, .kind = a_kind, .make = std::move(a_make) });
		};
		add("upload", FrameAnchors::PassKind::Copy, [shared] { return std::make_shared<UploadPass>(shared); });
		const auto addVolume = [&](bool a_far) {
			const std::string volume = a_far ? "far." : "near.";
			constexpr std::array<std::pair<Step, const char*>, 4> steps{ { { Step::Depth, "depth" }, { Step::Material, "material" },
				{ Step::Scattering, "scattering" }, { Step::Integration, "integration" } } };
			for (const auto& entry : steps) {
				const Step step = entry.first;
				add(volume + entry.second, FrameAnchors::PassKind::Compute, [shared, a_far, step] { return std::make_shared<DispatchPass>(shared, a_far, step); });
			}
			if (a_shape.temporal)
				add(volume + "history", FrameAnchors::PassKind::Copy, [shared, a_far] { return std::make_shared<HistoryPass>(shared, a_far); });
		};
		addVolume(false);
		if (a_shape.farEnabled)
			addVolume(true);
		else
			add("far.clear", FrameAnchors::PassKind::Compute, [shared] { return std::make_shared<ClearPass>(shared); });
		client.registerResources = [shared](org::RenderGraph& a_graph) {
			for (const Volume* volume : { &shared->nearVolume, &shared->farVolume }) {
				volume->vBuffer->Register(a_graph);
				volume->scattering->Register(a_graph);
				volume->scatteringHistory->Register(a_graph);
				volume->integrated->Register(a_graph);
				volume->conservativeDepth->Register(a_graph);
				volume->conservativeDepthHistory->Register(a_graph);
			}
			shared->directionalShadowLights->Register(a_graph);
		};
		client.commit = [this, shared](const FrameAnchors::Point&) {
			if (pending)
				shared->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, *pending);
			pending.reset();
		};
		// At the hook, before the epoch: the passes record only for a frame Prepass gave the graph.
		client.active = [this, shared] {
			const bool live = pending.has_value();
			shared->live->store(live, std::memory_order_release);
			return live;
		};
		client.onFailed = [] {
			logger::error("[ORG] Exponential Height Fog returns to D3D11: the render graph failed");
			auto& self = Get();
			self.failed = true;
			self.impl.reset();
		};
		client.sharedConstants = true;
		if (!FrameAnchors::Register(std::move(client)))
			return false;
		this->state = std::move(built);
		// New histories: nothing to reproject from.
		hasScatteringHistory = hasDepthHistory = hasScatteringHistoryFar = hasDepthHistoryFar = false;
		lastFrame = UINT32_MAX;
		logger::info("[ORG] Exponential Height Fog runs on the render graph (near {}x{}x{}, far {}x{}x{}{}, {})", a_shape.sizes.nearSize.x,
			a_shape.sizes.nearSize.y, a_shape.sizes.nearSize.z, a_shape.sizes.farSize.x, a_shape.sizes.farSize.y, a_shape.sizes.farSize.z,
			a_shape.farEnabled ? "" : " off", a_shape.temporal ? "temporal" : "no history");
		return true;
	}
};

ORGExponentialHeightFog::~ORGExponentialHeightFog() = default;

void ORGExponentialHeightFog::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
		logger::warn("[ORG] Exponential Height Fog: the Prepass anchor could not be reserved; the fog stays on D3D11");
}

bool ORGExponentialHeightFog::Prepass()
{
	if (!Requested() || failed)
		return false;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.IsActive() || !runtime.Host() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass))
		return false;
	if (!impl)
		impl = std::make_unique<Impl>();
	auto& fog = globals::features::exponentialHeightFog;
	impl->pending.reset();

	// This frame's inputs, as the D3D11 Prepass gathers them, each as a graph resource; a frame the graph cannot read runs on D3D11.
	Shape shape;
	shape.sizes = fog.ComputeGridSizes();
	shape.temporal = Util::GetTemporal();
	const auto fallback = [&](const char* a_reason) {
		if (impl->fallbackReason != a_reason) {
			logger::info("[ORG] Exponential Height Fog: this frame runs on D3D11 ({})", a_reason);
			impl->fallbackReason = a_reason;
		}
		impl->ranLastFrame = false;
		return false;
	};

	ExponentialHeightFog::ConstantInputs inputs;
	inputs.sizes = shape.sizes;

	shape.inputs[kDepth] = SceneDepth(Util::GetCurrentSceneDepthSRV(true));
	if (!shape.inputs[kDepth])
		return fallback("the scene depth is missing or cannot be imported");
	inputs.depth = true;

	LatchData data{};
	const bool hasShadowLight = globals::deferred && globals::deferred->GetDirectionalShadowLightData(data.directionalShadow);
	if (fog.directionalShadowMap && hasShadowLight) {
		shape.inputs[kDirectionalShadowMap] = ImportDepthView(fog.directionalShadowMap.get(), "cs.exp-height-fog.directional-shadow-map");
		if (!shape.inputs[kDirectionalShadowMap])
			return fallback("the directional shadow map cannot be imported");
		inputs.directionalShadow = true;
	}

	auto& ibl = globals::features::ibl;
	if (ibl.loaded && ibl.settings.EnableIBL != 0 && !ibl.IsDisabledForCurrentScene() && ibl.EnvIBLSRV() && ibl.SkyIBLSRV()) {
		const auto& graphIBL = ORGIBL::Get();
		if (graphIBL.IsActive() && ibl.EnvIBLSRV() == graphIBL.EnvSRV() && ibl.SkyIBLSRV() == graphIBL.SkySRV()) {
			shape.inputs[kEnvIBL] = { nullptr, "IBL::EnvIBL" };
			shape.inputs[kSkyIBL] = { nullptr, "IBL::SkyIBL" };
		} else {
			shape.inputs[kEnvIBL] = ImportView("IBL::EnvIBL", ibl.EnvIBLSRV());
			shape.inputs[kSkyIBL] = ImportView("IBL::SkyIBL", ibl.SkyIBLSRV());
		}
		if (!shape.inputs[kEnvIBL] || !shape.inputs[kSkyIBL])
			return fallback("IBL's SH textures cannot be imported");
		inputs.ibl = true;
	}

	auto& skylighting = globals::features::skylighting;
	if (skylighting.loaded && skylighting.ProbeArraySRV()) {
		// A 3D texture: only the graph's own probe volume can be read.
		if (!ORGSkylighting::Get().IsActive())
			return fallback("Skylighting's probes are on D3D11");
		shape.inputs[kProbes] = { nullptr, "Skylighting::ProbeArray" };
		inputs.skylighting = true;
	}

	auto& terrainShadows = globals::features::terrainShadows;
	if (terrainShadows.loaded) {
		auto* view = terrainShadows.GetShadowHeightSRV();
		const auto& graphTerrain = ORGTerrainShadows::Get();
		if (view && graphTerrain.Active() && view == graphTerrain.ShadowHeightSRV())
			shape.inputs[kShadowHeights] = { nullptr, "TerrainShadows::ShadowHeight" };
		else
			shape.inputs[kShadowHeights] = ImportView("TerrainShadows::ShadowHeight", view);
		// The shader reads the heights whenever terrain shadows are on.
		if (!shape.inputs[kShadowHeights])
			return fallback("Terrain Shadows' heights are missing");
	}

	if (globals::features::cloudShadows.loaded) {
		// The view Cloud Shadows leaves bound at CS t25, which the D3D11 path reads: the graph's own cubemap when it is that one.
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		globals::d3d::context->CSGetShaderResources(25, 1, view.put());
		const auto& graphClouds = ORGCloudShadows::Get();
		if (view && graphClouds.Active() && view.get() == graphClouds.OcclusionSRV())
			shape.inputs[kCloudShadows] = { nullptr, "CloudShadows::Occlusion" };
		else
			shape.inputs[kCloudShadows] = ImportView("CloudShadows::Occlusion", view.get());
		if (!shape.inputs[kCloudShadows])
			return fallback("Cloud Shadows' texture is missing");
	}

	auto& lightLimitFix = globals::features::lightLimitFix;
	if (lightLimitFix.loaded && lightLimitFix.GetLightsSRV() && lightLimitFix.GetLightIndexListSRV() && lightLimitFix.GetLightGridSRV()) {
		const auto& culling = ORGLightCulling::Get();
		if (!culling.IsActive() || lightLimitFix.GetLightsSRV() != culling.GetLightsSRV())
			return fallback("Light Limit Fix's light lists are on D3D11");
		shape.inputs[kLights] = { nullptr, "cs.llf.lights" };
		shape.inputs[kLightList] = { nullptr, "cs.llf.light-index-list" };
		shape.inputs[kLightGrid] = { nullptr, "cs.llf.light-grid" };
		inputs.localLights = true;
		if (lightLimitFix.IsLocalShadowCacheActive() && lightLimitFix.GetLocalShadowDataSRV()) {
			const auto& localShadows = ORGLocalShadows::Get();
			if (!localShadows.IsActive() || lightLimitFix.GetLocalShadowDataSRV() != localShadows.GetDataSRV())
				return fallback("Light Limit Fix's local shadows are on D3D11");
			shape.inputs[kLocalShadows] = { nullptr, "LightLimitFix::LocalShadowData" };
			shape.inputs[kLocalShadowMaps] = { nullptr, "LightLimitFix::LocalShadowCache", true };
			inputs.localShadows = true;
		}
		if (auto* view = fog.BuildEngineLocalShadowData(data.engineShadows)) {
			shape.inputs[kEngineShadowMaps] = ImportDepthView(view, "cs.exp-height-fog.engine-shadow-maps");
			if (!shape.inputs[kEngineShadowMaps])
				return fallback("the engine's shadow maps cannot be imported");
			inputs.engineLocalShadows = true;
		}
	}

	// The far volume's extent depends on the settings and the near plane only: computed once more below with the history state.
	bool farEnabled = false;
	fog.BuildConstants(inputs, farEnabled);
	shape.farEnabled = farEnabled;

	if (!impl->state || !(impl->state->shape == shape)) {
		if (!impl->Build(shape)) {
			logger::error("[ORG] Exponential Height Fog returns to D3D11 for good");
			FrameAnchors::Unregister(kClientId);
			failed = true;
			impl.reset();
			return false;
		}
	}

	// The history state, as the D3D11 Prepass keeps it.
	const std::uint32_t frame = globals::state->frameCount;
	const bool consecutive = impl->lastFrame != UINT32_MAX && frame == impl->lastFrame + 1u;
	inputs.temporal = shape.temporal;
	inputs.historyValid = shape.temporal && impl->hasScatteringHistory && consecutive;
	inputs.historyValidFar = shape.temporal && impl->hasScatteringHistoryFar && consecutive;
	inputs.depthHistory = impl->hasDepthHistory;
	inputs.depthHistoryFar = impl->hasDepthHistoryFar;
	data.fog = fog.BuildConstants(inputs, farEnabled);
	impl->pending = data;

	impl->hasScatteringHistory = shape.temporal;
	impl->hasDepthHistory = shape.temporal && inputs.depth;
	impl->hasScatteringHistoryFar = shape.temporal && farEnabled;
	impl->hasDepthHistoryFar = shape.temporal && farEnabled && inputs.depth;
	impl->lastFrame = frame;
	impl->ranLastFrame = true;
	impl->fallbackReason.clear();
	return true;
}

ID3D11ShaderResourceView* ORGExponentialHeightFog::NearSRV() const
{
	return impl && impl->state && impl->ranLastFrame ? impl->state->nearVolume.integrated->SRV() : nullptr;
}

ID3D11ShaderResourceView* ORGExponentialHeightFog::FarSRV() const
{
	return impl && impl->state && impl->ranLastFrame ? impl->state->farVolume.integrated->SRV() : nullptr;
}

void ORGExponentialHeightFog::Release()
{
	if (!impl)
		return;
	FrameAnchors::Unregister(kClientId);
	impl.reset();
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the volumetric fog always runs on D3D11.
struct ORGExponentialHeightFog::Impl
{};

ORGExponentialHeightFog::~ORGExponentialHeightFog() = default;
void ORGExponentialHeightFog::Reserve() {}
bool ORGExponentialHeightFog::Prepass() { return false; }
ID3D11ShaderResourceView* ORGExponentialHeightFog::NearSRV() const { return nullptr; }
ID3D11ShaderResourceView* ORGExponentialHeightFog::FarSRV() const { return nullptr; }
void ORGExponentialHeightFog::Release() {}

#endif
