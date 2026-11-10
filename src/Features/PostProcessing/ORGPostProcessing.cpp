#include "ORGPostProcessing.h"

#include <cstring>

namespace
{
	bool ReadRequested()
	{
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_POST_PROCESSING", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}
}

bool ORGPostProcessing::Requested()
{
	static const bool requested = ReadRequested();
	return requested;
}

ORGPostProcessing& ORGPostProcessing::Get()
{
	static ORGPostProcessing s_postProcessing;
	return s_postProcessing;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "ORGPostEffect.h"

#	include "Features/LinearLighting.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/SharedConstants.h"
#	include "State.h"
#	include "Utils/ColorSpace.h"
#	include "Utils/D3D.h"
#	include "Utils/Format.h"
#	include "Utils/Game.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <atomic>
#	include <cmath>
#	include <format>
#	include <map>
#	include <span>
#	include <unordered_map>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kClientId = "cs.pp";
		constexpr const char* kCopyShader = "PostProcessing/copy.ps.hlsl";
		// After any earlier client of the Tonemap anchor; the chain's own images order its passes among themselves.
		constexpr std::int32_t kOrderBase = 1000;
		constexpr DXGI_FORMAT kInputFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;  // PostProcessing::texInput's
		constexpr std::uint8_t kNotPrepared = 2;
	}

	/** @brief Whether the chain runs this frame, shared by every pass of every build. Render thread writes, the graph's passes read. */
	struct Gate
	{
		std::atomic<bool> on{ false };
	};

	/** @brief The program caches, for the session: a rebuild (a settings change) finds the programs it built before. */
	struct Programs
	{
		rhi::Device device;
		std::unordered_map<std::string, std::shared_ptr<const RasterProgram>> raster;
		std::unordered_map<std::string, std::shared_ptr<const ComputeProgram>> compute;
	};

	/** @brief One build of the chain: what its passes are made from. Immutable once registered. */
	struct BuildState
	{
		std::shared_ptr<const Gate> gate;
		std::shared_ptr<org::LatchBlock> latch;
		std::uint32_t latchUsed = 0;
		std::vector<std::unique_ptr<GraphResources::Texture2D>> images;
		std::vector<std::pair<std::string, std::shared_ptr<org::Resource>>> registered;
		std::vector<FrameAnchors::PassDesc> passes;
		std::map<std::string, Image, std::less<>> published;
		std::map<std::string, std::pair<std::uint32_t, std::uint32_t>, std::less<>> publishedLatches;  // offset, size
		Image current;
		std::string failure;           // empty while the build is good
		bool sharedConstants = false;  // a pass reads CS's shared constants (Bind::Shared)
	};

	namespace
	{
		/** @brief A pass's push words, at most ShaderBindings::kMaxPushWords. */
		struct PushWords
		{
			std::array<std::uint32_t, ShaderBindings::kMaxPushWords> data{};
			std::uint32_t count = 0;

			std::span<const std::uint32_t> Span() const { return { data.data(), count }; }
		};

		std::uint32_t WordCount(const std::vector<Bind>& a_bindings)
		{
			std::uint32_t words = 0;
			for (const auto& bind : a_bindings)
				words += bind.kind == Bind::Kind::Latch ? 2u : (bind.kind == Bind::Kind::Shared ? 4u : 1u);
			return words;
		}

		bool ReadsShared(const std::vector<Bind>& a_bindings)
		{
			return std::ranges::any_of(a_bindings, [](const Bind& a_bind) { return a_bind.kind == Bind::Kind::Shared; });
		}

		/** @brief A pass's declared entries: a view for each SRV and UAV entry (empty for the others), and the shared buffers. */
		struct DeclaredBindings
		{
			std::vector<org::DeclaredViewToken> views;
			SharedConstants::Bindings shared;
		};

		/** @brief One mip of a texture, for a pass that reads or writes that mip alone (other mips may be another use of the pass). */
		org::ResourcePtrAndRange MipOf(const std::shared_ptr<org::Resource>& a_texture, std::uint32_t a_mip)
		{
			return org::Subresources(a_texture, org::Mip(a_mip, 1));
		}

		DeclaredBindings DeclareBindings(org::PassBuilder& a_builder, const std::vector<Bind>& a_bindings)
		{
			DeclaredBindings declared;
			declared.views.resize(a_bindings.size());
			for (std::size_t i = 0; i < a_bindings.size(); ++i) {
				const auto& bind = a_bindings[i];
				const bool whole = bind.mip == kAllMips;
				if (bind.kind == Bind::Kind::Srv)
					declared.views[i] = whole ? a_builder.ShaderResource(bind.resource).View() :
					                            a_builder.ShaderResource(MipOf(bind.resource, bind.mip), org::SrvView{ .mip = bind.mip }).View();
				else if (bind.kind == Bind::Kind::Uav)
					declared.views[i] = whole ? a_builder.UnorderedAccess(bind.resource).View() :
					                            a_builder.UnorderedAccess(MipOf(bind.resource, bind.mip), org::UavView{ .mip = bind.mip }).View();
			}
			if (ReadsShared(a_bindings))
				declared.shared = SharedConstants::Declare(a_builder);
			return declared;
		}

		PushWords FillWords(const std::vector<Bind>& a_bindings, const DeclaredBindings& a_declared, const org::LatchBlock& a_latch,
			org::PassRecordContext& a_recording)
		{
			PushWords words;
			for (std::size_t i = 0; i < a_bindings.size(); ++i) {
				const auto& bind = a_bindings[i];
				switch (bind.kind) {
				case Bind::Kind::Srv:
				case Bind::Kind::Uav:
					words.data[words.count++] = a_recording.Index(a_declared.views[i]);
					break;
				case Bind::Kind::Handle:
					words.data[words.count++] = bind.value;
					break;
				case Bind::Kind::Latch:
					{
						const auto latch = ShaderBindings::LatchOf(a_latch, a_recording.FrameSlot(), bind.value);
						words.data[words.count++] = latch.handle;
						words.data[words.count++] = latch.offset;
						break;
					}
				case Bind::Kind::Shared:
					{
						const auto shared = SharedConstants::Index(a_recording, a_declared.shared);
						for (const auto handle : { shared.frameBuffer, shared.sharedData, shared.featureData, shared.permutation })
							words.data[words.count++] = handle;
						break;
					}
				}
			}
			return words;
		}

		/**
		 * @brief The gate as one pass saw it: the revision follows what the pass was prepared with, so a recording prepared before
		 * the hook changed the gate is prepared again (as ORGCloudShadows' copy pass).
		 */
		class GateUse
		{
		public:
			explicit GateUse(std::shared_ptr<const Gate> a_gate) :
				gate(std::move(a_gate)) {}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto live = static_cast<std::uint8_t>(gate->on.load(std::memory_order_acquire) ? 1 : 0);
				const std::uint8_t used = prepared.load(std::memory_order_acquire);
				a_out.push_back(used);
				if (used != live)
					a_out.push_back(mismatch.fetch_add(1, std::memory_order_relaxed) + 1);
			}

			bool Prepare() const
			{
				const bool on = gate->on.load(std::memory_order_acquire);
				prepared.store(static_cast<std::uint8_t>(on ? 1 : 0), std::memory_order_release);
				return on;
			}

		private:
			std::shared_ptr<const Gate> gate;
			mutable std::atomic<std::uint8_t> prepared = kNotPrepared;
			mutable std::atomic<std::uint64_t> mismatch = 0;
		};

		struct DrawSpec
		{
			std::string name;
			std::shared_ptr<const RasterProgram> program;
			std::vector<Bind> bindings;
			Image target;
			RasterProgram::Viewport viewport;
			std::shared_ptr<const org::LatchBlock> latch;
		};

		struct DrawBindings
		{
			org::DeclaredViewToken target;
			DeclaredBindings declared;
		};

		struct DrawFrame
		{
			std::shared_ptr<const DrawSpec> spec;  // null: the chain does not run this frame
		};

		class FullscreenPass final : public org::TypedRenderGraphPass<FullscreenPass, DrawFrame, DrawBindings>
		{
		public:
			FullscreenPass(std::shared_ptr<const DrawSpec> a_spec, std::shared_ptr<const Gate> a_gate) :
				spec(std::move(a_spec)), gate(std::move(a_gate)) {}

			DrawBindings Declare(org::PassBuilder& a_builder)
			{
				const auto& target = spec->target;
				auto view = target.mip == kAllMips ? a_builder.RenderTarget(target.graph).View() :
				                                     a_builder.RenderTarget(MipOf(target.graph, target.mip), org::RtvView{ .mip = target.mip }).View();
				return { view, DeclareBindings(a_builder, spec->bindings) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get()));
				gate.Revision(a_out);
			}

			DrawFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { gate.Prepare() ? spec : nullptr }; }

			static void Record(const DrawBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.spec)
					return;
				const auto& draw = *a_frame.spec;
				const auto words = FillWords(draw.bindings, a_bindings.declared, *draw.latch, a_recording);
				const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = rhi::LoadOp::Load };
				draw.program->Draw(a_recording, { .colours = { &colour, 1 }, .viewport = draw.viewport, .constants = words.Span(), .debugName = draw.name.c_str() });
			}

		private:
			std::shared_ptr<const DrawSpec> spec;
			GateUse gate;
		};

		struct DispatchSpec
		{
			std::string name;
			std::shared_ptr<const ComputeProgram> program;
			std::vector<Bind> bindings;
			std::array<std::uint32_t, 3> groups{};
			std::optional<std::uint32_t> indirect;  // the latch offset of the group counts
			rhi::CommandSignaturePtr signature;
			std::shared_ptr<std::atomic<bool>> prepared;
			std::shared_ptr<const org::LatchBlock> latch;
		};

		struct DispatchBindings
		{
			DeclaredBindings declared;
		};

		struct DispatchFrame
		{
			std::shared_ptr<const DispatchSpec> spec;
		};

		class DispatchPass final : public org::TypedRenderGraphPass<DispatchPass, DispatchFrame, DispatchBindings>
		{
		public:
			DispatchPass(std::shared_ptr<const DispatchSpec> a_spec, std::shared_ptr<const Gate> a_gate) :
				spec(std::move(a_spec)), gate(std::move(a_gate)) {}

			DispatchBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { DeclareBindings(a_builder, spec->bindings) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get()));
				gate.Revision(a_out);
			}

			DispatchFrame Prepare(const DispatchBindings&, const org::PassPrepareContext&) const
			{
				if (!gate.Prepare())
					return { nullptr };
				if (spec->prepared)
					spec->prepared->store(true, std::memory_order_release);
				return { spec };
			}

			static void Record(const DispatchBindings& a_bindings, const DispatchFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.spec)
					return;
				const auto& dispatch = *a_frame.spec;
				const auto words = FillWords(dispatch.bindings, a_bindings.declared, *dispatch.latch, a_recording);
				if (!dispatch.indirect) {
					dispatch.program->Dispatch(a_recording, words.Span(), dispatch.groups[0], dispatch.groups[1], dispatch.groups[2]);
					return;
				}
				auto& commands = a_recording.Commands();
				commands.BindLayout(dispatch.program->layout->GetHandle());
				commands.BindPipeline(dispatch.program->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, words.count, words.data.data());
				commands.ExecuteIndirect(dispatch.signature->GetHandle(), dispatch.latch->Resource()->GetAPIResource().GetHandle(),
					dispatch.latch->Offset(a_recording.FrameSlot()) + *dispatch.indirect, {}, 0, 1);
			}

		private:
			std::shared_ptr<const DispatchSpec> spec;
			GateUse gate;
		};

		struct CopySpec
		{
			Image source, target;
			UINT width = 0, height = 0;
		};

		struct CopyBindings
		{
			org::ResourceBindingToken source, target;
		};

		struct CopyFrame
		{
			std::shared_ptr<const CopySpec> spec;
		};

		class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
		{
		public:
			CopyPass(std::shared_ptr<const CopySpec> a_spec, std::shared_ptr<const Gate> a_gate) :
				spec(std::move(a_spec)), gate(std::move(a_gate)) {}

			CopyBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopySource(spec->source.graph), a_builder.CopyDestination(spec->target.graph) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(spec.get()));
				gate.Revision(a_out);
			}

			CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { gate.Prepare() ? spec : nullptr }; }

			static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.spec)
					return;
				const auto& copy = *a_frame.spec;
				const rhi::TextureCopyRegion target{ .texture = a_recording.Resolve(a_bindings.target).GetHandle(), .width = copy.width, .height = copy.height };
				const rhi::TextureCopyRegion source{ .texture = a_recording.Resolve(a_bindings.source).GetHandle(), .width = copy.width, .height = copy.height };
				a_recording.Commands().CopyTextureRegion(target, source);
			}

		private:
			std::shared_ptr<const CopySpec> spec;
			GateUse gate;
		};

		/** @brief a_own plus the defines Util::CompileShader gives every D3D11 build of the stage, so both paths build one permutation. */
		std::vector<std::pair<std::wstring, std::wstring>> ProgramDefines(std::vector<std::pair<std::string, std::string>> a_own, const char* a_stage)
		{
			std::vector<std::string_view> names;
			for (const auto& define : a_own)
				names.push_back(define.first);
			for (auto& define : Util::GetCommonShaderDefines(names))
				a_own.push_back(std::move(define));
			for (const char* name : { a_stage, "WINPC", "DX11" })
				a_own.emplace_back(name, "");
			// Defines are ASCII.
			std::vector<std::pair<std::wstring, std::wstring>> wide;
			for (const auto& [name, value] : a_own)
				wide.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
			return wide;
		}

		std::string ProgramKey(std::string_view a_source, const std::wstring& a_entry, std::uint32_t a_words,
			const std::vector<std::pair<std::string, std::string>>& a_defines)
		{
			std::string key = std::format("{}|{}|{}", a_source, Util::WStringToString(a_entry), a_words);
			for (const auto& [name, value] : a_defines)
				key += std::format("|{}={}", name, value);
			return key;
		}

		/** @brief The pixels a D3D11 viewport a_size wide covers: those whose centre lies inside it. */
		std::uint32_t CoveredPixels(float a_size) { return static_cast<std::uint32_t>((std::max)(std::ceil(a_size - 0.5f), 0.0f)); }
	}

	Builder::Builder(BuildState& a_state, Programs& a_programs, const Shape& a_shape, std::string_view a_effect) :
		state(a_state), programs(a_programs), shape(a_shape), effect(a_effect) {}

	const Image& Builder::Current() const { return state.current; }

	void Builder::SetCurrent(const Image& a_image)
	{
		if (!a_image) {
			Fail("its output image is missing");
			return;
		}
		state.current = a_image;
	}

	Image Builder::CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags)
	{
		return CreateImage(a_name, a_format, a_bindFlags, shape.width, shape.height);
	}

	Image Builder::CreateMipChain(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_mips)
	{
		return CreateImage(a_name, a_format, a_bindFlags, shape.width, shape.height, a_mips);
	}

	Image Builder::CreateImage(std::string_view a_name, DXGI_FORMAT a_format, UINT a_bindFlags, UINT a_width, UINT a_height, UINT a_mips)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = (std::max)(a_mips, 1u);
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc = { 1, 0 };
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | a_bindFlags;
		// Transient and not exported: the graph aliases it with the chain's other images.
		auto texture = std::make_unique<GraphResources::Texture2D>(desc, std::string(a_name));
		if (!*texture) {
			Fail(std::format("{} could not be created", a_name));
			return {};
		}
		Image image{ texture->Graph(), a_format };
		state.images.push_back(std::move(texture));
		return image;
	}

	void Builder::Register(const GraphResources::Resource& a_resource)
	{
		if (!a_resource) {
			Fail(std::format("{} is missing", a_resource.Name()));
			return;
		}
		state.registered.emplace_back(a_resource.Name(), a_resource.Graph());
	}

	void Builder::Publish(std::string_view a_name, const Image& a_image) { state.published.insert_or_assign(std::string(a_name), a_image); }

	Image Builder::Find(std::string_view a_name) const
	{
		const auto found = state.published.find(a_name);
		return found != state.published.end() ? found->second : Image{};
	}

	void Builder::PublishLatchBytes(std::string_view a_name, std::uint32_t a_offset, std::uint32_t a_size)
	{
		state.publishedLatches.insert_or_assign(std::string(a_name), std::pair{ a_offset, a_size });
	}

	std::optional<std::uint32_t> Builder::FindLatchBytes(std::string_view a_name, std::uint32_t a_size)
	{
		const auto found = state.publishedLatches.find(a_name);
		if (found == state.publishedLatches.end())
			return std::nullopt;
		if (found->second.second != a_size) {
			Fail(std::format("the latch region {} holds {} bytes, not {}", a_name, found->second.second, a_size));
			return std::nullopt;
		}
		return found->second.first;
	}

	std::uint32_t Builder::Reserve(std::uint32_t a_bytes)
	{
		const std::uint32_t offset = (state.latchUsed + 15u) & ~15u;
		if (offset + a_bytes > kLatchBytes) {
			Fail(std::format("the chain's latch is full ({} bytes)", kLatchBytes));
			return 0;
		}
		state.latchUsed = offset + a_bytes;
		return offset;
	}

	std::shared_ptr<const org::LatchBlock> Builder::LatchBlock() const { return state.latch; }

	std::shared_ptr<const RasterProgram> Builder::Raster(std::string_view a_source, std::uint32_t a_words, std::vector<DXGI_FORMAT> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		std::vector<RasterProgram::Target> targets;
		for (const auto format : a_targets)
			targets.push_back({ .format = rhi::helpers::ToRHI(format) });
		return RasterTargets(a_source, a_words, std::move(targets), std::move(a_defines), std::move(a_entry));
	}

	std::shared_ptr<const RasterProgram> Builder::RasterTargets(std::string_view a_source, std::uint32_t a_words, std::vector<RasterProgram::Target> a_targets,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		for (const auto& target : a_targets) {
			const auto& blend = target.blend;
			key += std::format("|f{}", static_cast<int>(target.format));
			if (blend.enable)
				key += std::format("b{},{},{},{},{},{},{}", static_cast<int>(blend.srcColor), static_cast<int>(blend.dstColor), static_cast<int>(blend.colorOp),
					static_cast<int>(blend.srcAlpha), static_cast<int>(blend.dstAlpha), static_cast<int>(blend.alphaOp), static_cast<int>(blend.writeMask));
			else if (blend.writeMask != rhi::ColorWriteEnable::All)
				key += std::format("m{}", static_cast<int>(blend.writeMask));
		}
		if (const auto found = programs.raster.find(key); found != programs.raster.end())
			return found->second;
		RasterProgram::Desc desc{};
		desc.pixelSource = std::string(a_source);
		desc.pixelEntry = std::move(a_entry);
		desc.constantWords = a_words;
		desc.defines = ProgramDefines(std::move(a_defines), "PSHADER");
		desc.targets = std::move(a_targets);
		auto program = RasterProgram::Load(programs.device, desc);
		if (!program) {
			Fail(std::format("{} did not build", a_source));
			return nullptr;
		}
		programs.raster.emplace(std::move(key), program);
		return program;
	}

	std::shared_ptr<const ComputeProgram> Builder::Compute(std::string_view a_source, std::uint32_t a_words,
		std::vector<std::pair<std::string, std::string>> a_defines, std::wstring a_entry)
	{
		auto key = ProgramKey(a_source, a_entry, a_words, a_defines);
		if (const auto found = programs.compute.find(key); found != programs.compute.end())
			return found->second;
		auto program = ComputeProgram::Load(programs.device,
			{ .source = std::string(a_source), .entry = std::move(a_entry), .constantWords = a_words, .defines = ProgramDefines(std::move(a_defines), "COMPUTESHADER") });
		if (!program) {
			Fail(std::format("{} did not build", a_source));
			return nullptr;
		}
		programs.compute.emplace(std::move(key), program);
		return program;
	}

	RasterProgram::Viewport Builder::FullViewport() const { return { .width = shape.width, .height = shape.height }; }

	RasterProgram::Viewport Builder::DynamicViewport() const
	{
		return { .width = (std::min)(CoveredPixels(shape.dynamicWidth), shape.width), .height = (std::min)(CoveredPixels(shape.dynamicHeight), shape.height) };
	}

	RasterProgram::Viewport Builder::MipViewport(std::uint32_t a_mip) const
	{
		if (a_mip == kAllMips)
			return FullViewport();
		return { .width = (std::max)(shape.width >> a_mip, 1u), .height = (std::max)(shape.height >> a_mip, 1u) };
	}

	namespace
	{
		bool BindingsValid(const std::vector<Bind>& a_bindings)
		{
			return std::ranges::all_of(a_bindings, [](const Bind& a_bind) {
				return (a_bind.kind != Bind::Kind::Srv && a_bind.kind != Bind::Kind::Uav) || a_bind.resource;
			});
		}
	}

	void Builder::Draw(FullscreenDraw a_draw)
	{
		if (!a_draw.program || !a_draw.target || !BindingsValid(a_draw.bindings)) {
			Fail(std::format("{}: a program, target or input is missing", a_draw.name));
			return;
		}
		if (const auto words = WordCount(a_draw.bindings); words != a_draw.program->constantWords) {
			Fail(std::format("{}: {} push words, the program takes {}", a_draw.name, words, a_draw.program->constantWords));
			return;
		}
		state.sharedConstants = state.sharedConstants || ReadsShared(a_draw.bindings);
		auto spec = std::make_shared<DrawSpec>();
		spec->name = PassName(a_draw.name);
		spec->program = std::move(a_draw.program);
		spec->bindings = std::move(a_draw.bindings);
		spec->target = std::move(a_draw.target);
		spec->viewport = a_draw.viewport.value_or(MipViewport(spec->target.mip));
		spec->latch = state.latch;
		auto gate = state.gate;
		state.passes.push_back({ .name = spec->name,
			.anchor = FrameAnchors::Anchor::Tonemap,
			.order = kOrderBase + static_cast<std::int32_t>(state.passes.size()),
			.kind = FrameAnchors::PassKind::Render,
			.make = [spec, gate]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<FullscreenPass>(spec, gate); } });
	}

	void Builder::Dispatch(ComputeDispatch a_dispatch)
	{
		if (!a_dispatch.program || !BindingsValid(a_dispatch.bindings)) {
			Fail(std::format("{}: a program or input is missing", a_dispatch.name));
			return;
		}
		if (const auto words = WordCount(a_dispatch.bindings); words != a_dispatch.program->constantWords) {
			Fail(std::format("{}: {} push words, the program takes {}", a_dispatch.name, words, a_dispatch.program->constantWords));
			return;
		}
		state.sharedConstants = state.sharedConstants || ReadsShared(a_dispatch.bindings);
		auto spec = std::make_shared<DispatchSpec>();
		spec->name = PassName(a_dispatch.name);
		spec->program = std::move(a_dispatch.program);
		spec->bindings = std::move(a_dispatch.bindings);
		spec->groups = a_dispatch.groups;
		spec->latch = state.latch;
		spec->prepared = std::move(a_dispatch.prepared);
		if (a_dispatch.indirect) {
			spec->indirect = a_dispatch.indirect->offset;
			rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
			if (programs.device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 },
					spec->program->layout->GetHandle(), spec->signature) != rhi::Result::Ok) {
				Fail(std::format("{}: no dispatch signature", a_dispatch.name));
				return;
			}
		}
		auto gate = state.gate;
		state.passes.push_back({ .name = spec->name,
			.anchor = FrameAnchors::Anchor::Tonemap,
			.order = kOrderBase + static_cast<std::int32_t>(state.passes.size()),
			.kind = FrameAnchors::PassKind::Compute,
			.make = [spec, gate]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<DispatchPass>(spec, gate); } });
	}

	void Builder::Copy(std::string_view a_name, const Image& a_source, const Image& a_target)
	{
		if (!a_source || !a_target || a_source.format != a_target.format || a_source.mip != kAllMips || a_target.mip != kAllMips) {
			Fail(std::format("{}: the copy's images are missing, differ in format, or are single mips", a_name));
			return;
		}
		auto spec = std::make_shared<CopySpec>(CopySpec{ a_source, a_target, shape.width, shape.height });
		auto gate = state.gate;
		state.passes.push_back({ .name = PassName(a_name),
			.anchor = FrameAnchors::Anchor::Tonemap,
			.order = kOrderBase + static_cast<std::int32_t>(state.passes.size()),
			.kind = FrameAnchors::PassKind::Copy,
			.make = [spec, gate]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<CopyPass>(spec, gate); } });
	}

	void Builder::Add(std::string_view a_name, FrameAnchors::PassKind a_kind, std::function<std::shared_ptr<org::RenderPass>()> a_make)
	{
		if (!a_make) {
			Fail(std::format("{}: no pass", a_name));
			return;
		}
		state.passes.push_back({ .name = PassName(a_name),
			.anchor = FrameAnchors::Anchor::Tonemap,
			.order = kOrderBase + static_cast<std::int32_t>(state.passes.size()),
			.kind = a_kind,
			.make = std::move(a_make) });
	}

	void Builder::Fail(std::string_view a_reason)
	{
		if (state.failure.empty())
			state.failure = std::format("{}: {}", effect, a_reason);
	}

	bool Builder::Failed() const { return !state.failure.empty(); }

	std::string Builder::PassName(std::string_view a_step) const { return std::format("{}.{}.{}", kClientId, effect, a_step); }

	void CommitContext::WriteBytes(std::uint32_t a_offset, const void* a_data, std::size_t a_size) const
	{
		latch.Write(slot, a_offset, std::span<const std::byte>(static_cast<const std::byte*>(a_data), a_size));
	}
}

namespace
{
	using ORGPostChain::Bind;
	using Index = PostProcessing::FeaturePipelineIndex;

	constexpr std::size_t kSlots = static_cast<std::size_t>(Index::COUNT);

	/** @brief The ORG implementation of a pipeline slot, or null: not ported, so the chain runs on D3D11 while it is enabled. */
	std::unique_ptr<ORGPostChain::Effect> MakeEffect(Index a_index)
	{
		switch (a_index) {
		case Index::Vignette:
			return ORGPostChain::MakeVignette();
		case Index::ColorGrading:
			return ORGPostChain::MakeColorGrading();
		case Index::LUT:
			return ORGPostChain::MakeLUT();
		case Index::Border:
			return ORGPostChain::MakeBorder();
		case Index::LensFlare:
			return ORGPostChain::MakeLensFlare();
		case Index::CODBloom:
			return ORGPostChain::MakeCODBloom();
		case Index::PhysicalGlare:
			return ORGPostChain::MakePhysicalGlare();
		case Index::DoF:
			return ORGPostChain::MakeDoF();
		case Index::Camera:
			return ORGPostChain::MakeCamera();
		case Index::Composite:
			return ORGPostChain::MakeComposite();
		case Index::MotionBlur:
			return ORGPostChain::MakeMotionBlur();
		case Index::LocalExposure:
			return ORGPostChain::MakeLocalExposure();
		case Index::AutoExposure:
			return ORGPostChain::MakeHistogramAutoExposure();
		default:
			return nullptr;
		}
	}

	/** @brief A slot's step in the pass names: "cs.pp.<step>.<pass>". */
	const char* StepName(Index a_index)
	{
		switch (a_index) {
		case Index::DoF:
			return "dof";
		case Index::Vignette:
			return "vignette";
		case Index::LocalExposure:
			return "local-exposure";
		case Index::AutoExposure:
			return "auto-exposure";
		case Index::MotionBlur:
			return "motion-blur";
		case Index::PhysicalGlare:
			return "physical-glare";
		case Index::CODBloom:
			return "cod-bloom";
		case Index::LensFlare:
			return "lens-flare";
		case Index::Composite:
			return "composite";
		case Index::ColorGrading:
			return "color-grading";
		case Index::LUT:
			return "lut";
		case Index::Camera:
			return "camera";
		case Index::Border:
			return "border";
		default:
			return "effect";
		}
	}

	// copy.ps.hlsl's push block: COPY_RESOURCES under ORG, in order.
	struct CopyBlock
	{
		ShaderBindings::Handle texSrc, texSrcAlt;
		ShaderBindings::Latch copy;
	};

	// copy.ps.hlsl's CopyConstants: PostProcessing::CopyCB with the ORG source selector in its last word.
	struct CopyLatch
	{
		std::uint32_t inputGamut = 0;
		std::uint32_t outputGamut = 0;
		float gamma = 1.0f;
		std::uint32_t source = 0;
	};
	static_assert(sizeof(CopyLatch) == sizeof(PostProcessing::CopyCB));

	bool Converts(const PostProcessing::CopyCB& a_conversion) { return a_conversion.gamma != 1.f || a_conversion.inputGamut != a_conversion.outputGamut; }

	std::uint64_t Bits(float a_value)
	{
		std::uint32_t bits = 0;
		std::memcpy(&bits, &a_value, sizeof(bits));
		return bits;
	}
}

struct ORGPostProcessing::Impl
{
	ORGPostChain::Programs programs;
	std::array<std::unique_ptr<ORGPostChain::Effect>, kSlots> effects;
	std::shared_ptr<ORGPostChain::Gate> gate = std::make_shared<ORGPostChain::Gate>();

	// The registered build, what it was made for, and the last key that failed to build (not tried again).
	std::shared_ptr<ORGPostChain::BuildState> build;
	std::vector<std::uint64_t> key;
	std::vector<std::uint64_t> failedKey;
	ORGPostChain::LatchRegion<CopyLatch> input, output;
	bool failed = false;

	// This frame's, from BeforeTonemap, for the commit.
	bool ready = false;
	bool ran = false;
	bool readsMainCopy = false;
	bool linear = false;
	bool colorGraded = false;
	std::vector<Index> order;

	// Why the chain runs on D3D11 (empty: on the graph), logged when it changes.
	std::optional<std::string> reason;

	void SetReason(std::string a_reason)
	{
		if (reason && *reason == a_reason)
			return;
		if (a_reason.empty())
			logger::info("[ORG] Post Processing: the chain runs on the render graph ({} passes)", build ? build->passes.size() : 0);
		else
			logger::info("[ORG] Post Processing: the chain runs on D3D11: {}", a_reason);
		reason = std::move(a_reason);
	}

	/** @brief The chain for a_order as a new client of the Tonemap anchor. False, with the reason in a_failure, when it cannot. */
	bool Rebuild(const ORGPostChain::Shape& a_shape, const ORGPostChain::Image& a_main, const ORGPostChain::Image& a_mainCopy,
		const std::vector<Index>& a_order, const PostProcessing::CopyCB& a_conversion, std::vector<std::uint64_t> a_key, std::string& a_failure)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host) {
			a_failure = "the render graph has no host";
			return false;
		}
		auto next = std::make_shared<ORGPostChain::BuildState>();
		next->gate = gate;
		next->latch = std::make_shared<org::LatchBlock>("cs.pp.latch", ORGPostChain::kLatchBytes, host->FrameSlots());
		auto& pp = globals::features::postProcessing;
		ORGPostChain::LatchRegion<CopyLatch> nextInput, nextOutput;

		// As BeginLinearProcessing: the scene in RGBA16F, linear unless Linear Lighting already is. Under ORG the input pass also
		// picks kMAIN or kMAIN_COPY (CopyLatch::source), so one recording serves either.
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "input");
			auto program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { ORGPostChain::kInputFormat });
			auto image = builder.CreateImage("PostProcessing::Input", ORGPostChain::kInputFormat, D3D11_BIND_RENDER_TARGET);
			nextInput = builder.Latch<CopyLatch>();
			builder.Draw({ .name = "copy",
				.program = std::move(program),
				.bindings = { Bind::Srv(a_main), Bind::Srv(a_mainCopy), Bind::Latch(nextInput) },
				.target = image });
			builder.SetCurrent(image);
		}

		for (const auto index : a_order) {
			if (!next->failure.empty())
				break;
			ORGPostChain::Builder builder(*next, programs, a_shape, StepName(index));
			effects[static_cast<std::size_t>(index)]->Build(builder, *pp.pipeline[static_cast<std::size_t>(index)]);
		}

		// As PreProcess's two CopyToRenderTarget: the chain's output into both main targets. Both receive the same image, as the
		// D3D11 copy of one into the other gives (when their formats differ, each is converted from the output directly).
		{
			ORGPostChain::Builder builder(*next, programs, a_shape, "output");
			const auto chainOutput = builder.Current();
			nextOutput = builder.Latch<CopyLatch>();
			const bool convert = Converts(a_conversion);
			for (const auto& [target, name] : { std::pair{ &a_main, "main" }, std::pair{ &a_mainCopy, "main-copy" } }) {
				if (!convert && chainOutput.format == target->format) {
					builder.Copy(name, chainOutput, *target);
					continue;
				}
				builder.Draw({ .name = name,
					.program = builder.Raster(ORGPostChain::kCopyShader, ShaderBindings::Words<CopyBlock>(), { target->format }),
					.bindings = { Bind::Srv(chainOutput), Bind::Unused(), Bind::Latch(nextOutput) },
					.target = *target });
			}
		}

		if (!next->failure.empty()) {
			a_failure = next->failure;
			return false;
		}

		FrameAnchors::ClientDesc client{};
		client.id = ORGPostChain::kClientId;
		client.passes = next->passes;
		client.sharedConstants = next->sharedConstants;
		client.registerResources = [next](org::RenderGraph& a_graph) {
			for (const auto& image : next->images)
				image->Register(a_graph);
			for (const auto& [name, resource] : next->registered)
				a_graph.RegisterResource(org::ResourceIdentifier(name), resource);
		};
		client.commit = [this](const FrameAnchors::Point&) { Commit(); };
		client.active = [this] { return ready; };
		client.onFailed = [this] {
			logger::error("[ORG] Post Processing: the render graph failed; the chain returns to D3D11 for the session");
			failed = true;
			ready = false;
			gate->on.store(false, std::memory_order_release);
			build.reset();
			key.clear();
		};
		if (!FrameAnchors::Register(std::move(client))) {
			a_failure = "the Tonemap anchor refused the client";
			return false;
		}
		build = std::move(next);
		key = std::move(a_key);
		input = nextInput;
		output = nextOutput;
		std::string steps = "input";
		for (const auto index : a_order)
			steps += std::format(", {}", StepName(index));
		logger::info("[ORG] Post Processing: chain built ({}, output; {} passes, {}x{})", steps, build->passes.size(), a_shape.width, a_shape.height);
		return true;
	}

	/** @brief In the epoch: this frame's values into the latch. */
	void Commit()
	{
		ran = false;
		if (!ready || !build)
			return;
		auto& pp = globals::features::postProcessing;
		const ORGPostChain::CommitContext commit(*build->latch, RenderGraphRuntime::Get().Host()->CurrentFrameSlot());
		commit.Write(input, CopyLatch{ .gamma = linear ? 1.0f : Util::ColorSpace::GAME_GAMMA, .source = readsMainCopy ? 1u : 0u });
		for (const auto index : order)
			effects[static_cast<std::size_t>(index)]->Commit(commit, *pp.pipeline[static_cast<std::size_t>(index)]);
		const auto conversion = pp.OutputConversion(colorGraded);
		commit.Write(output, CopyLatch{ .inputGamut = static_cast<std::uint32_t>(conversion.inputGamut),
								 .outputGamut = static_cast<std::uint32_t>(conversion.outputGamut),
								 .gamma = conversion.gamma });
		ran = true;
	}
};

ORGPostProcessing::~ORGPostProcessing() = default;

void ORGPostProcessing::Reserve()
{
	if (Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::Tonemap))
		logger::warn("[ORG] Post Processing: the Tonemap anchor could not be reserved; the chain stays on D3D11");
}

void ORGPostProcessing::Setup()
{
	if (!Requested() || impl)
		return;
	auto& runtime = RenderGraphRuntime::Get();
	if (!runtime.Host() || !runtime.IsActive() || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Tonemap)) {
		logger::warn("[ORG] Post Processing: CS_ORG_POST_PROCESSING=1 but the render graph or its Tonemap anchor is not available; the chain stays on D3D11");
		return;
	}
	auto state = std::make_unique<Impl>();
	state->programs.device = runtime.Host()->Device();
	std::string ported;
	auto& pp = globals::features::postProcessing;
	for (std::size_t i = 0; i < kSlots; ++i) {
		state->effects[i] = MakeEffect(static_cast<Index>(i));
		if (state->effects[i] && pp.pipeline[i])
			ported += std::format("{}{}", ported.empty() ? "" : ", ", pp.pipeline[i]->GetType());
	}
	impl = std::move(state);
	logger::info("[ORG] Post Processing: the tonemap chain runs on the render graph while every enabled effect is ported ({})", ported);
}

void ORGPostProcessing::BeforeTonemap(bool a_readsMainCopy)
{
	if (!impl)
		return;
	auto& s = *impl;
	s.ready = false;
	s.ran = false;
	s.gate->on.store(false, std::memory_order_release);
	if (s.failed || !RenderGraphRuntime::Get().IsActive())
		return;

	auto& pp = globals::features::postProcessing;
	auto* state = globals::state;
	const bool linear = globals::features::linearLighting.IsLinearLightingActive();
	// PreProcess draws nothing in these cases, so neither does the graph.
	if (linear && (state->permutationData.RenderToUI || state->IsMainOrLoadingMenuOpen()))
		return;
	if (pp.bypass && !linear)
		return;
	// Effects11 owns the tonemap: it renders it, or PreProcess runs the D3D11 chain when it cannot.
	if (state->GetTonemapOwner() != State::TonemapOwner::kPostProcessing)
		return;

	pp.UpdatePipeline();
	auto order = pp.ChainOrder();

	// The mixed rule: the graph runs the chain only when it can run every effect in it.
	std::vector<std::uint64_t> key;
	std::string held;
	const auto hold = [&](const std::string& a_why) { held += std::format("{}{}", held.empty() ? "" : "; ", a_why); };
	for (const auto index : order) {
		const auto slot = static_cast<std::size_t>(index);
		auto& feature = *pp.pipeline[slot];
		key.push_back(slot);
		if (!s.effects[slot]) {
			hold(std::format("{} has no ORG implementation", feature.GetType()));
			continue;
		}
		if (const char* why = s.effects[slot]->Update(feature, key))
			hold(std::format("{}: {}", feature.GetType(), why));
	}
	if (!held.empty()) {
		s.SetReason(held);
		return;
	}

	auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto mainTarget = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
	auto mainCopyTarget = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN_COPY));
	auto* mainTexture = targets[RE::RENDER_TARGETS::kMAIN].texture;
	auto* mainCopyTexture = targets[RE::RENDER_TARGETS::kMAIN_COPY].texture;
	if (!mainTarget || !mainCopyTarget || !mainTexture || !mainCopyTexture) {
		s.SetReason("kMAIN or kMAIN_COPY cannot be imported");
		return;
	}
	D3D11_TEXTURE2D_DESC mainDesc{}, mainCopyDesc{};
	mainTexture->GetDesc(&mainDesc);
	mainCopyTexture->GetDesc(&mainCopyDesc);
	if (mainDesc.Width != mainCopyDesc.Width || mainDesc.Height != mainCopyDesc.Height) {
		s.SetReason("kMAIN and kMAIN_COPY differ in size");
		return;
	}
	ORGPostChain::Shape shape{};
	shape.width = mainDesc.Width;
	shape.height = mainDesc.Height;
	const auto dynamic = Util::ConvertToDynamic(float2(static_cast<float>(mainDesc.Width), static_cast<float>(mainDesc.Height)));
	shape.dynamicWidth = dynamic.x;
	shape.dynamicHeight = dynamic.y;
	shape.mainFormat = mainDesc.Format;
	shape.mainCopyFormat = mainCopyDesc.Format;

	const bool colorGraded = std::ranges::find(order, Index::ColorGrading) != order.end();
	const auto conversion = pp.OutputConversion(colorGraded);
	for (const std::uint64_t word : { std::uint64_t(shape.width), std::uint64_t(shape.height), Bits(shape.dynamicWidth), Bits(shape.dynamicHeight),
			 std::uint64_t(shape.mainFormat), std::uint64_t(shape.mainCopyFormat), std::uint64_t(linear), std::uint64_t(Converts(conversion)),
			 reinterpret_cast<std::uintptr_t>(mainTarget.get()), reinterpret_cast<std::uintptr_t>(mainCopyTarget.get()) })
		key.push_back(word);

	if (key != s.key || !s.build) {
		if (key == s.failedKey) {
			s.SetReason("the chain failed to build for these settings (logged above)");
			return;
		}
		std::string failure;
		bool built = false;
		try {
			built = s.Rebuild(shape, { mainTarget, shape.mainFormat }, { mainCopyTarget, shape.mainCopyFormat }, order, conversion, key, failure);
		} catch (const std::exception& e) {
			failure = e.what();
		}
		if (!built) {
			logger::error("[ORG] Post Processing: the chain could not be built: {}", failure);
			s.failedKey = std::move(key);
			s.SetReason("the chain failed to build for these settings (logged above)");
			return;
		}
		s.failedKey.clear();
	}

	s.order = std::move(order);
	s.linear = linear;
	s.readsMainCopy = a_readsMainCopy;
	s.colorGraded = colorGraded;
	s.ready = true;
	s.gate->on.store(true, std::memory_order_release);
	s.SetReason({});

	// As PreProcess: the main targets leave the output merger, which ISRefraction may still have kMAIN_COPY bound to.
	globals::d3d::context->OMSetRenderTargets(0, nullptr, nullptr);
	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
}

bool ORGPostProcessing::Ran() const
{
	return impl && impl->ran;
}

#else  // !CS_HAS_RENDER_GRAPH

// Built without OpenRenderGraph: the chain always runs on D3D11.
struct ORGPostProcessing::Impl
{};

ORGPostProcessing::~ORGPostProcessing() = default;
void ORGPostProcessing::Reserve() {}
void ORGPostProcessing::Setup() {}
void ORGPostProcessing::BeforeTonemap(bool) {}
bool ORGPostProcessing::Ran() const { return false; }

#endif
