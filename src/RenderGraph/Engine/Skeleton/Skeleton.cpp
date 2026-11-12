#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::Skeleton::Requested()
{
	static const bool requested = Switches::On("CS_ORG_ENGINE_SKELETON");
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassBuilders.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <cstring>
#	include <map>
#	include <optional>
#	include <set>
#	include <tuple>
#	include <vector>

namespace RenderGraph::Engine::Skeleton
{
	namespace
	{
		using FrameAnchors::Anchor;
		using ColourTarget = RE::RENDER_TARGETS;
		using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL;

		constexpr const char* kShader = "RenderGraph/EngineClear.hlsl";

		// The views the engine clears (docs/development/p5/skeleton.md, RE): each gets one pass, registered once at Setup. A clear
		// of any other view keeps the engine's call (logged once per view), so a new view never changes the registration.
		// Colour: kMAIN, the G-buffer targets Deferred asks to clear (2-7, the normals 4/5 by frame parity among them), and the
		// shadow mask.
		constexpr std::array<std::uint32_t, 8> kColourTargets{ ColourTarget::kMAIN, ColourTarget::kMAIN_COPY, ColourTarget::kMAIN_ONLY_ALPHA,
			ColourTarget::kNORMAL_TAAMASK_SSRMASK, ColourTarget::kNORMAL_TAAMASK_SSRMASK_SWAP, ColourTarget::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED,
			ColourTarget::kMOTION_VECTOR, ColourTarget::kSHADOW_MASK };
		// Depth: the main depth, the shadow maps' slices and the reflection faces' depth.
		constexpr std::array<std::uint32_t, 6> kDepthTargets{ DepthTarget::kMAIN, DepthTarget::kSHADOWMAPS_ESRAM, DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM,
			DepthTarget::kSHADOWMAPS, DepthTarget::kPRECIPITATION_OCCLUSION_MAP, DepthTarget::kCUBEMAP_REFLECTIONS };
		// The depth views' slices the engine table holds (depthStencils[i].views).
		constexpr std::uint32_t kDepthSlices = 8;

		/** @brief The three clear groups, each an anchor of its own so a run declares only its group's views. */
		enum class Group : std::uint8_t
		{
			Shadow,
			Cube,
			Frame,
			Count
		};
		constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
		constexpr std::array<Anchor, kGroups> kGroupAnchors{ Anchor::EngineShadowClear, Anchor::EngineCubeClear, Anchor::EngineFrameClear };
		constexpr std::array<const char*, kGroups> kGroupIds{ "cs.engine.skeleton.shadow", "cs.engine.skeleton.cube", "cs.engine.skeleton.frame" };
		constexpr std::array<const char*, kGroups> kGroupNames{ "shadow", "cube", "frame" };
		constexpr const char* kCopyId = "cs.engine.skeleton.depth-copy";

		/** @brief Which engine table a view comes from. */
		enum class Kind : std::uint8_t
		{
			Colour,  // RendererData::renderTargets[index].RTV
			Depth,   // depthStencils[index].views[slice]
			Cube,    // cubemapRenderTargets[index].cubeSideRTV[face]
		};

		struct Target
		{
			Kind kind = Kind::Colour;
			std::uint32_t index = 0;
			std::uint32_t slice = 0;  // the depth view's slice or the cube face; the view's own array slice is checked against it

			auto Tie() const { return std::tie(kind, index, slice); }
			bool operator<(const Target& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const Target& a_other) const { return Tie() == a_other.Tie(); }
		};

		/**
		 * @brief One clear draw of a view: the D3D11 clear flags it does (0 for colour), its program and its gate entry. A depth view
		 * has one per set of flags the engine uses on its format, so a view is one pass whatever it is cleared with.
		 */
		struct ClearDraw
		{
			std::uint32_t flags = 0;
			std::shared_ptr<const RasterProgram> program;
			std::uint32_t entry = 0;
		};

		/** @brief One clear pass's immutable spec: its view, its draws, the group's gate and colour latch. */
		struct ClearSpec
		{
			std::string name;
			Target target;
			std::shared_ptr<org::ExternalTextureResource> resource;
			const void* view = nullptr;     // the engine's view the pass was made for
			const void* texture = nullptr;  // the engine's texture then: another one there means the target was made again
			std::uint32_t slice = 0;
			bool depth = false;
			std::uint32_t width = 0, height = 0;
			std::vector<ClearDraw> draws;
			std::uint32_t generation = 0;  // the group's registration this spec belongs to
			std::shared_ptr<const RunGate> gate;
			std::shared_ptr<const org::LatchBlock> colours;  // a float4 per gate entry, written by the commit for colour clears
		};

		/** @brief The colour program's push block (EngineClear.hlsl's ENGINE_CLEAR_RESOURCES). */
		struct ClearBlock
		{
			ShaderBindings::Latch clear;
		};

		struct ClearBindings
		{
			org::DeclaredViewToken view;
		};

		struct ClearFrame
		{
			std::shared_ptr<const ClearSpec> spec;
		};

		/** @brief A view's clears through a run gate: each draw clears when the commit opened its entry, and does nothing otherwise. */
		class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
		{
		public:
			explicit ClearPass(std::shared_ptr<const ClearSpec> a_spec) :
				spec(std::move(a_spec)) {}

			ClearBindings Declare(org::PassBuilder& a_builder)
			{
				const auto range = org::Subresources(spec->resource, org::Mip(0, 1), org::Slice(spec->slice, 1));
				if (spec->depth)
					return { a_builder.DepthReadWrite(range, org::DsvView{ .slice = spec->slice }).View() };
				return { a_builder.RenderTarget(range, org::RtvView{ .slice = spec->slice }).View() };
			}

			// The recording depends only on what the pass was made with (its registration and its first gate entry); the gate decides
			// each run.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(spec->generation);
				a_out.push_back(spec->draws.front().entry);
			}

			ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { spec }; }

			// Recorded at execution (FrameAnchors::PassDesc::atExecution): a view none of whose clears the run's commit opened is not
			// recorded.
			static bool SkipsAtExecution(const ClearBindings&, const ClearFrame& a_frame, const org::PassRecordContext& a_recording)
			{
				const auto& clear = *a_frame.spec;
				return std::ranges::none_of(clear.draws, [&](const ClearDraw& a_draw) { return clear.gate->IsOpen(a_recording.Run(), a_draw.entry); });
			}

			static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto& clear = *a_frame.spec;
				for (const auto& variant : clear.draws) {
					RasterProgram::DrawDesc draw{};
					draw.viewport = { .x = 0, .y = 0, .width = clear.width, .height = clear.height };
					draw.debugName = clear.name.c_str();
					draw.indirect = &clear.gate->Latch();
					draw.indirectOffset = clear.gate->Offset(variant.entry);
					if (clear.depth) {
						const RasterProgram::Depth depth{ .view = &a_bindings.view, .readOnly = false, .load = rhi::LoadOp::Load };
						draw.depth = &depth;
						variant.program->Draw(a_recording, draw);
					} else {
						const RasterProgram::Colour colour{ .view = &a_bindings.view, .load = rhi::LoadOp::Load };
						draw.colours = { &colour, 1 };
						const ClearBlock block{ ShaderBindings::LatchOf(*clear.colours, a_recording.Run(), variant.entry * RunGate::kEntryBytes) };
						draw.constants = ShaderBindings::Data(block);
						variant.program->Draw(a_recording, draw);
					}
				}
			}

		private:
			std::shared_ptr<const ClearSpec> spec;
		};

		/** @brief A clear the engine asked for this flush, with what replays it on D3D11 when the pass did not run. */
		struct Pending
		{
			std::uint32_t gateEntry = 0;
			bool depth = false;
			void* view = nullptr;
			std::array<float, 4> colour{};
			UINT flags = 0;
			FLOAT depthValue = 1.0f;
			UINT8 stencil = 0;
		};

		struct GroupState
		{
			std::map<Target, std::shared_ptr<const ClearSpec>> specs;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> colours;
			std::vector<Pending> pending;
			std::uint32_t generation = 0;
			bool registered = false;  // a registration is live
			bool stale = false;       // a view's texture was made again: registered again at the next flush with nothing pending
			bool executed = false;    // the current run's executed signal
			bool failed = false;      // stays on D3D11 for the session
			bool announced = false;   // the first clear on the graph was logged
			std::uint64_t taken = 0, replayed = 0;
		};

		struct CopyState
		{
			std::shared_ptr<org::ExternalTextureResource> source, target;
			const void* sourceTexture = nullptr;
			const void* targetTexture = nullptr;
			bool registered = false;
			bool wanted = false;
			bool executed = false;
			bool failed = false;
			bool announced = false;
		};

		struct SkeletonState
		{
			bool installed = false;
			bool setUp = false;
			bool flushing = false;
			std::array<GroupState, kGroups> groups;
			CopyState copy;
			// Programs by (format, depth flags; 0 for colour), made when a registration is (Setup, or after a target was made again).
			std::map<std::pair<rhi::Format, std::uint32_t>, std::shared_ptr<const RasterProgram>> programs;
			std::set<Target> refused;  // views left to the engine (logged once each)
		};

		SkeletonState& Skel()
		{
			static SkeletonState state;
			return state;
		}

		GroupState& GroupOf(Group a_group) { return Skel().groups[static_cast<std::size_t>(a_group)]; }

		std::size_t IndexOf(Group a_group) { return static_cast<std::size_t>(a_group); }

		bool Usable()
		{
			auto& state = Skel();
			return state.setUp && !state.flushing && FrameAnchors::IsRenderOwner() && RenderGraphRuntime::Get().IsActive();
		}

		std::string TargetName(const Target& a_target)
		{
			switch (a_target.kind) {
			case Kind::Colour:
				return fmt::format("rt{}", a_target.index);
			case Kind::Depth:
				return fmt::format("depth{}.{}", a_target.index, a_target.slice);
			default:
				return fmt::format("cube{}.{}", a_target.index, a_target.slice);
			}
		}

		Group GroupFor(const Target& a_target)
		{
			if (a_target.kind == Kind::Cube)
				return Group::Cube;
			if (a_target.kind == Kind::Depth) {
				switch (a_target.index) {
				case DepthTarget::kCUBEMAP_REFLECTIONS:
					return Group::Cube;
				case DepthTarget::kSHADOWMAPS_ESRAM:
				case DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM:
				case DepthTarget::kSHADOWMAPS:
				case DepthTarget::kPRECIPITATION_OCCLUSION_MAP:
					return Group::Shadow;
				default:
					break;
				}
			}
			return Group::Frame;
		}

		/** @brief The engine table entry a render target view is, or none. kFRAMEBUFFER (the window's) is left to D3D11. */
		std::optional<Target> FindColour(const ID3D11RenderTargetView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return std::nullopt;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = ColourTarget::kMAIN; i < ColourTarget::kTOTAL; ++i)
				if (targets[i].RTV == a_view)
					return Target{ Kind::Colour, i, 0 };
			const auto& cubes = renderer->GetRendererData().cubemapRenderTargets;
			for (std::uint32_t i = 0; i < RE::RENDER_TARGET_CUBEMAP::kTOTAL; ++i)
				for (std::uint32_t face = 0; face < 6; ++face)
					if (cubes[i].cubeSideRTV[face] == a_view)
						return Target{ Kind::Cube, i, face };
			return std::nullopt;
		}

		/** @brief The engine table entry a depth-stencil view is (a writable view only), or none. */
		std::optional<Target> FindDepth(const ID3D11DepthStencilView* a_view)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_view)
				return std::nullopt;
			// The engine's depth table has kTOTAL entries on SE and AE, whatever the array's declared size.
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < DepthTarget::kTOTAL; ++i)
				for (std::uint32_t slice = 0; slice < kDepthSlices; ++slice)
					if (depths[i].views[slice] == a_view)
						return Target{ Kind::Depth, i, slice };
			return std::nullopt;
		}

		/** @brief The graph texture of a target now (the published one when ORG owns it, else the anchors' import). */
		std::shared_ptr<org::ExternalTextureResource> ResourceOf(const Target& a_target)
		{
			switch (a_target.kind) {
			case Kind::Colour:
				return FrameAnchors::EngineTarget(a_target.index);
			case Kind::Depth:
				return FrameAnchors::EngineDepth(a_target.index);
			default:
				return FrameAnchors::EngineCubemap(a_target.index);
			}
		}

		ID3D11Texture2D* TextureOf(const Target& a_target)
		{
			auto* renderer = globals::game::renderer;
			switch (a_target.kind) {
			case Kind::Colour:
				return renderer->GetRuntimeData().renderTargets[a_target.index].texture;
			case Kind::Depth:
				return renderer->GetDepthStencilData().depthStencils[a_target.index].texture;
			default:
				return renderer->GetRendererData().cubemapRenderTargets[a_target.index].texture;
			}
		}

		ID3D11View* ViewOf(const Target& a_target)
		{
			auto* renderer = globals::game::renderer;
			switch (a_target.kind) {
			case Kind::Colour:
				return reinterpret_cast<ID3D11View*>(renderer->GetRuntimeData().renderTargets[a_target.index].RTV);
			case Kind::Depth:
				return reinterpret_cast<ID3D11View*>(renderer->GetDepthStencilData().depthStencils[a_target.index].views[a_target.slice]);
			default:
				return reinterpret_cast<ID3D11View*>(renderer->GetRendererData().cubemapRenderTargets[a_target.index].cubeSideRTV[a_target.slice]);
			}
		}

		bool IsIntegerFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_UINT:
			case DXGI_FORMAT_R32G32B32A32_SINT:
			case DXGI_FORMAT_R32G32B32_UINT:
			case DXGI_FORMAT_R32G32B32_SINT:
			case DXGI_FORMAT_R16G16B16A16_UINT:
			case DXGI_FORMAT_R16G16B16A16_SINT:
			case DXGI_FORMAT_R32G32_UINT:
			case DXGI_FORMAT_R32G32_SINT:
			case DXGI_FORMAT_R10G10B10A2_UINT:
			case DXGI_FORMAT_R8G8B8A8_UINT:
			case DXGI_FORMAT_R8G8B8A8_SINT:
			case DXGI_FORMAT_R16G16_UINT:
			case DXGI_FORMAT_R16G16_SINT:
			case DXGI_FORMAT_R32_UINT:
			case DXGI_FORMAT_R32_SINT:
			case DXGI_FORMAT_R8G8_UINT:
			case DXGI_FORMAT_R8G8_SINT:
			case DXGI_FORMAT_R16_UINT:
			case DXGI_FORMAT_R16_SINT:
			case DXGI_FORMAT_R8_UINT:
			case DXGI_FORMAT_R8_SINT:
				return true;
			default:
				return false;
			}
		}

		bool HasStencil(DXGI_FORMAT a_format) { return a_format == DXGI_FORMAT_D24_UNORM_S8_UINT || a_format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT; }

		/**
		 * @brief The clear program for a format: colour (a_depthFlags 0) or depth with the D3D11 clear flags. Built only when a group
		 * registers (Setup, or after its targets were made again), never per clear; one attempt per key.
		 */
		std::shared_ptr<const RasterProgram> ProgramFor(rhi::Format a_format, std::uint32_t a_depthFlags)
		{
			auto& state = Skel();
			const auto key = std::make_pair(a_format, a_depthFlags);
			if (const auto it = state.programs.find(key); it != state.programs.end())
				return it->second;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			RasterProgram::Desc desc{ .pixelSource = kShader, .vertexSource = kShader, .vertexEntry = L"VSMain", .commonDefines = false };
			if (a_depthFlags == 0) {
				desc.pixelEntry = L"PSColour";
				desc.constantWords = ShaderBindings::Words<ClearBlock>();
				desc.targets = { { .format = a_format } };
			} else {
				desc.pixelEntry = L"PSDepth";
				desc.depthFormat = a_format;
				const bool depth = (a_depthFlags & D3D11_CLEAR_DEPTH) != 0;
				desc.depth = { .depthEnable = depth, .depthWrite = depth, .depthFunc = rhi::CompareOp::Always };
				if (a_depthFlags & D3D11_CLEAR_STENCIL) {
					// Zero whatever the test: the engine clears the stencil to 0, and no reference is needed.
					const rhi::StencilFaceState zero{ .failOp = rhi::StencilOp::Zero, .depthFailOp = rhi::StencilOp::Zero, .passOp = rhi::StencilOp::Zero, .func = rhi::CompareOp::Always };
					desc.depth.stencilEnable = true;
					desc.depth.stencilReadMask = 0;
					desc.depth.stencilWriteMask = 0xFF;
					desc.depth.front = zero;
					desc.depth.back = zero;
				}
			}
			auto program = RasterProgram::Load(host->Device(), desc);
			if (!program)
				logger::warn("[ORG] engine skeleton: the clear program for format {} (flags {}) could not be made; those clears stay on D3D11",
					static_cast<int>(a_format), a_depthFlags);
			// Kept even when null: one attempt per format.
			state.programs.emplace(key, program);
			return program;
		}

		/** @brief A view the skeleton has no pass for: the engine clears it, logged once per view. */
		void Refuse(const Target& a_target, std::string_view a_reason)
		{
			if (Skel().refused.emplace(a_target).second)
				logger::info("[ORG] engine skeleton: {} stays on D3D11 ({})", TargetName(a_target), a_reason);
		}

		/**
		 * @brief A run that did not draw what it took (no executed signal): an error, logged once per kind of work and status, not a
		 * readiness state. The caller makes the engine's calls for that run.
		 */
		void FailedRun(const char* a_what, FrameAnchors::RunStatus a_status)
		{
			static std::set<std::pair<std::string, int>> logged;
			if (!logged.emplace(a_what, static_cast<int>(a_status)).second)
				return;
			logger::error("[ORG] engine skeleton: a run of {} did not execute (run status {}); the engine's calls were made for it", a_what,
				static_cast<int>(a_status));
		}

		/** @brief The view's format and its array slice, from its D3D11 description; false when it is not one mip-0 slice. */
		bool ViewShape(ID3D11View* a_view, bool a_depth, DXGI_FORMAT& a_format, std::uint32_t& a_slice)
		{
			if (a_depth) {
				D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
				static_cast<ID3D11DepthStencilView*>(a_view)->GetDesc(&desc);
				a_format = desc.Format;
				if (desc.Flags != 0)
					return false;
				if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D) {
					a_slice = 0;
					return desc.Texture2D.MipSlice == 0;
				}
				if (desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY) {
					a_slice = desc.Texture2DArray.FirstArraySlice;
					return desc.Texture2DArray.MipSlice == 0 && desc.Texture2DArray.ArraySize == 1;
				}
				return false;
			}
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			static_cast<ID3D11RenderTargetView*>(a_view)->GetDesc(&desc);
			a_format = desc.Format;
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D) {
				a_slice = 0;
				return desc.Texture2D.MipSlice == 0;
			}
			if (desc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY) {
				a_slice = desc.Texture2DArray.FirstArraySlice;
				return desc.Texture2DArray.MipSlice == 0 && desc.Texture2DArray.ArraySize == 1;
			}
			return false;
		}

		/** @brief Every view of the engine's tables the skeleton clears, in a stable order. */
		std::vector<Target> KnownTargets()
		{
			std::vector<Target> targets;
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return targets;
			for (const auto index : kColourTargets)
				targets.push_back({ Kind::Colour, index, 0 });
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (const auto index : kDepthTargets) {
				for (std::uint32_t slice = 0; slice < kDepthSlices; ++slice) {
					const auto* view = depths[index].views[slice];
					// A slot repeating an earlier slot's view is the same view: FindDepth finds the first.
					bool repeat = false;
					for (std::uint32_t earlier = 0; earlier < slice; ++earlier)
						repeat = repeat || depths[index].views[earlier] == view;
					if (view && !repeat)
						targets.push_back({ Kind::Depth, index, slice });
				}
			}
			for (std::uint32_t index = 0; index < RE::RENDER_TARGET_CUBEMAP::kTOTAL; ++index)
				for (std::uint32_t face = 0; face < 6; ++face)
					targets.push_back({ Kind::Cube, index, face });
			return targets;
		}

		/**
		 * @brief The pass spec for a view as the engine's tables hold it now, without its gate entries; null (logged) when the view
		 * cannot be cleared on the graph.
		 */
		std::shared_ptr<ClearSpec> MakeSpec(const Target& a_target)
		{
			auto* view = ViewOf(a_target);
			auto* texture = TextureOf(a_target);
			if (!view || !texture)
				return nullptr;  // the engine has no such view this session
			const bool depth = a_target.kind == Kind::Depth;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t slice = 0;
			if (!ViewShape(view, depth, format, slice) || (a_target.kind == Kind::Cube && slice != a_target.slice)) {
				Refuse(a_target, "not a single mip-0 slice");
				return nullptr;
			}
			if (IsIntegerFormat(format)) {
				Refuse(a_target, "an integer format");
				return nullptr;
			}
			auto resource = ResourceOf(a_target);
			if (!resource) {
				Refuse(a_target, "no graph texture");
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			texture->GetDesc(&textureDesc);
			auto spec = std::make_shared<ClearSpec>();
			spec->target = a_target;
			spec->resource = std::move(resource);
			spec->view = view;
			spec->texture = texture;
			spec->slice = slice;
			spec->depth = depth;
			spec->width = textureDesc.Width;
			spec->height = textureDesc.Height;
			// The flags the engine clears each format with (RE): 1 on a depth-only format (shadow slices), 3 and 2 on a format with
			// stencil (the main depth and its stencil-only clear in FinishAccumulating, the reflection faces' depth).
			std::vector<std::uint32_t> flags{ 0u };
			if (depth)
				flags = HasStencil(format) ? std::vector<std::uint32_t>{ D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, D3D11_CLEAR_STENCIL } :
				                             std::vector<std::uint32_t>{ D3D11_CLEAR_DEPTH };
			for (const auto flag : flags) {
				auto program = ProgramFor(rhi::helpers::ToRHI(format), flag);
				if (!program)
					continue;
				spec->draws.push_back({ .flags = flag, .program = std::move(program) });
			}
			if (spec->draws.empty()) {
				Refuse(a_target, "no clear program");
				return nullptr;
			}
			return spec;
		}

		/**
		 * @brief The group's passes made from the engine's tables as they are now and registered: one pass per view, a new gate and
		 * colour latch, so a recording of an older registration (its retired gates stay closed) can never clear the wrong view.
		 * Called at Setup, and again only when the engine made one of the group's targets again (a resize).
		 */
		void Build(Group a_group)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			std::vector<std::shared_ptr<ClearSpec>> made;
			std::uint32_t entries = 0;
			for (const auto& target : KnownTargets()) {
				if (GroupFor(target) != a_group)
					continue;
				if (auto spec = MakeSpec(target)) {
					entries += static_cast<std::uint32_t>(spec->draws.size());
					made.push_back(std::move(spec));
				}
			}
			group.specs.clear();
			group.stale = false;
			if (made.empty()) {
				if (std::exchange(group.registered, false))
					FrameAnchors::Unregister(kGroupIds[index]);
				return;
			}
			group.gate = RunGate::Create(kGroupIds[index], entries);
			auto* host = RenderGraphRuntime::Get().Host();
			group.colours = host ? org::LatchBlock::Create(fmt::format("{}.colours", kGroupIds[index]), entries * RunGate::kEntryBytes) : nullptr;
			if (!group.gate || !group.colours) {
				group.failed = true;
				logger::warn("[ORG] engine skeleton: the {} clears' run gate could not be made; they stay on D3D11", kGroupNames[index]);
				return;
			}
			++group.generation;
			FrameAnchors::ClientDesc client{};
			client.id = kGroupIds[index];
			std::uint32_t entry = 0;
			for (auto& spec : made) {
				for (auto& draw : spec->draws)
					draw.entry = entry++;
				spec->name = fmt::format("{}.{}", kGroupIds[index], TargetName(spec->target));
				spec->generation = group.generation;
				spec->gate = group.gate;
				spec->colours = group.colours;
				std::shared_ptr<const ClearSpec> pass = spec;
				client.passes.push_back({ .name = spec->name,
					.anchor = kGroupAnchors[index],
					.order = static_cast<std::int32_t>(spec->draws.front().entry),
					.kind = FrameAnchors::PassKind::Render,
					.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
					// A run clears a few of the group's views: recorded after the commit, the others record nothing.
					.atExecution = true });
				group.specs.emplace(spec->target, std::move(pass));
			}
			client.active = [a_group] { return !GroupOf(a_group).pending.empty(); };
			client.commit = [a_group](const FrameAnchors::Point&) {
				const auto& state = GroupOf(a_group);
				const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
				for (const auto& pending : state.pending) {
					if (!pending.depth)
						state.colours->WriteValue(run, pending.gateEntry * RunGate::kEntryBytes, pending.colour);
					state.gate->OpenDraw(pending.gateEntry);
				}
			};
			client.executed = [a_group](const FrameAnchors::Point&) { GroupOf(a_group).executed = true; };
			client.onFailed = [a_group] {
				auto& state = GroupOf(a_group);
				state.failed = true;
				state.registered = false;
				logger::error("[ORG] engine skeleton: the {} anchor failed; its clears return to D3D11", FrameAnchors::Name(kGroupAnchors[IndexOf(a_group)]));
			};
			client.gates = { group.gate };
			const auto passes = client.passes.size();
			group.registered = FrameAnchors::Register(std::move(client));
			if (!group.registered) {
				group.specs.clear();
				group.failed = true;
				logger::warn("[ORG] engine skeleton: the {} anchor refused its clears; they stay on D3D11", FrameAnchors::Name(kGroupAnchors[index]));
				return;
			}
			logger::info("[ORG] engine skeleton: the {} clears registered at the {} anchor ({} views, {} gate entries, registration {})", kGroupNames[index],
				FrameAnchors::Name(kGroupAnchors[index]), passes, entries, group.generation);
		}

		/**
		 * @brief The registered draw for a clear of a_target with a_flags (0 for colour), or null when the caller must use D3D11 now: a
		 * view the skeleton has no pass for, a target the engine made again since (the group registers again), or a group whose
		 * registration is not in its anchor's built graph yet.
		 */
		const ClearDraw* DrawFor(Group a_group, const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags)
		{
			auto& group = GroupOf(a_group);
			const auto found = group.specs.find(a_target);
			if (found == group.specs.end()) {
				Refuse(a_target, "not a view the skeleton clears");
				return nullptr;
			}
			const auto& spec = *found->second;
			if (spec.view != a_view || spec.texture != TextureOf(a_target)) {
				if (!std::exchange(group.stale, true))
					logger::info("[ORG] engine skeleton: {} was made again; the {} clears register again", TargetName(a_target), kGroupNames[IndexOf(a_group)]);
				return nullptr;
			}
			// Setup's build point put the group in its anchor's graph. CanRun is false only on a full ring (an inherent limit) or between
			// a re-registration after a resize and the next build point.
			if (!FrameAnchors::CanRun(kGroupIds[IndexOf(a_group)], kGroupAnchors[IndexOf(a_group)]))
				return nullptr;
			for (const auto& draw : spec.draws)
				if (draw.flags == a_flags)
					return &draw;
			Refuse(a_target, "its clear flags have no pass");
			return nullptr;
		}

		bool Take(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, Pending a_pending)
		{
			const Group group = GroupFor(a_target);
			auto& state = GroupOf(group);
			if (state.failed || state.stale || !state.registered)
				return false;
			const auto* draw = DrawFor(group, a_target, a_view, a_flags);
			if (!draw)
				return false;
			a_pending.gateEntry = draw->entry;
			state.pending.push_back(a_pending);
			return true;
		}

		void Replay(ID3D11DeviceContext* a_context, const std::vector<Pending>& a_pending)
		{
			for (const auto& pending : a_pending) {
				if (pending.depth)
					a_context->ClearDepthStencilView(static_cast<ID3D11DepthStencilView*>(pending.view), pending.flags, pending.depthValue, pending.stencil);
				else
					a_context->ClearRenderTargetView(static_cast<ID3D11RenderTargetView*>(pending.view), pending.colour.data());
			}
		}

		/**
		 * @brief The copy's client registered for the engine's kMAIN and kPOST_ZPREPASS_COPY as they are now. Setup, and again only
		 * when the engine made either again (a resize).
		 *
		 * The copy has no run gate: a copy has no indirect form, and a raster copy cannot write the stencil the engine's
		 * CopyResource copies. It needs none: EngineDepthCopy is the skeleton's own anchor, nothing else registers there, and the
		 * anchor runs only from RunCopy, whose run always wants the copy (an inactive client submits nothing).
		 */
		void RegisterCopy()
		{
			auto& copy = Skel().copy;
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			copy.sourceTexture = depths[DepthTarget::kMAIN].texture;
			copy.targetTexture = depths[DepthTarget::kPOST_ZPREPASS_COPY].texture;
			copy.source = FrameAnchors::EngineDepth(DepthTarget::kMAIN);
			copy.target = FrameAnchors::EngineDepth(DepthTarget::kPOST_ZPREPASS_COPY);
			if (!copy.source || !copy.target) {
				if (std::exchange(copy.registered, false))
					FrameAnchors::Unregister(kCopyId);
				logger::info("[ORG] engine skeleton: kMAIN or kPOST_ZPREPASS_COPY has no graph texture; the depth copy stays on D3D11");
				return;
			}
			if (copy.source->GetWidth() != copy.target->GetWidth() || copy.source->GetHeight() != copy.target->GetHeight() ||
				copy.source->GetFormat() != copy.target->GetFormat()) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: kMAIN and kPOST_ZPREPASS_COPY differ in size or format; the depth copy stays on D3D11");
				return;
			}
			AnchorPasses::Builder builder(Anchor::EngineDepthCopy, kCopyId, 0);
			builder.Add(AnchorPasses::Copy{ .name = "post-zprepass", .source = copy.source, .target = copy.target, .width = copy.source->GetWidth(), .height = copy.source->GetHeight() });
			FrameAnchors::ClientDesc client{};
			client.id = kCopyId;
			client.passes = builder.Take();
			client.active = [] { return Skel().copy.wanted; };
			client.executed = [](const FrameAnchors::Point&) { Skel().copy.executed = true; };
			client.onFailed = [] {
				auto& state = Skel().copy;
				state.failed = true;
				state.registered = false;
				logger::error("[ORG] engine skeleton: the EngineDepthCopy anchor failed; the depth copy returns to D3D11");
			};
			copy.registered = FrameAnchors::Register(std::move(client));
			if (!copy.registered) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: the EngineDepthCopy anchor refused the copy; it stays on D3D11");
			}
		}

		/** @brief The copy on the graph at this point; false when it did not run (the caller copies). */
		bool RunCopy()
		{
			auto& copy = Skel().copy;
			if (copy.failed || !Usable())
				return false;
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			if (depths[DepthTarget::kMAIN].texture != copy.sourceTexture || depths[DepthTarget::kPOST_ZPREPASS_COPY].texture != copy.targetTexture) {
				logger::info("[ORG] engine skeleton: kMAIN or kPOST_ZPREPASS_COPY was made again; the depth copy registers again");
				RegisterCopy();
				return false;
			}
			if (!copy.registered)
				return false;
			if (!FrameAnchors::CanRun(kCopyId, Anchor::EngineDepthCopy))
				return false;
			copy.wanted = true;
			copy.executed = false;
			auto& state = Skel();
			state.flushing = true;
			const auto result = FrameAnchors::Run(Anchor::EngineDepthCopy);
			state.flushing = false;
			copy.wanted = false;
			// Drawn only when the run submitted with this registration's pass in its graph (the executed signal).
			const bool done = result && copy.executed;
			if (done && !std::exchange(copy.announced, true))
				logger::info("[ORG] engine skeleton: the main depth is copied into kPOST_ZPREPASS_COPY on the graph");
			if (!done)
				FailedRun("the depth copy", result.status);
			return done;
		}
	}

	void Reserve()
	{
		if (!Requested())
			return;
		bool reserved = true;
		for (const auto anchor : { Anchor::EngineShadowClear, Anchor::EngineCubeClear, Anchor::EngineFrameClear, Anchor::EngineDepthCopy })
			reserved = FrameAnchors::Reserve(anchor) && reserved;
		if (!reserved)
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but not every anchor could be reserved; those clears stay on D3D11");
	}

	void Setup()
	{
		auto& state = Skel();
		if (!Requested() || state.setUp)
			return;
		if (!state.installed) {
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but its engine hooks are not installed; the engine clears with D3D11");
			return;
		}
		if (!RenderGraphRuntime::Get().IsActive() || !globals::game::renderer) {
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but the render graph is not active; the engine clears with D3D11");
			return;
		}
		// Every pass is made and registered here, from the engine's tables: a clear never registers anything during play.
		for (std::size_t i = 0; i < kGroups; ++i) {
			if (!FrameAnchors::HasEpoch(kGroupAnchors[i])) {
				state.groups[i].failed = true;
				continue;
			}
			Build(static_cast<Group>(i));
		}
		state.copy.failed = !FrameAnchors::HasEpoch(Anchor::EngineDepthCopy);
		if (!state.copy.failed)
			RegisterCopy();
		state.setUp = true;
		logger::info("[ORG] engine skeleton: on (CS_ORG_ENGINE_SKELETON); the engine's clears and its main depth copy are registered with the graph");
	}

	bool CopyPostZPrepassDepth()
	{
		return Requested() && RunCopy();
	}

	void MarkInstalled()
	{
		Skel().installed = true;
	}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Usable() || !a_colour)
				return false;
			const auto target = FindColour(a_view);
			if (!target)
				return false;
			Pending pending{ .depth = false, .view = a_view };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return Take(*target, a_view, 0, pending);
		}

		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			// The clear programs write depth 1 and stencil 0, the only values the engine clears to.
			if (!Usable() || ((a_flags & D3D11_CLEAR_DEPTH) && a_depth != 1.0f) || ((a_flags & D3D11_CLEAR_STENCIL) && a_stencil != 0))
				return false;
			const auto target = FindDepth(a_view);
			if (!target)
				return false;
			// A format without stencil clears depth only, whatever the flags ask.
			std::uint32_t flags = a_flags & (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL);
			const auto& specs = GroupOf(GroupFor(*target)).specs;
			if (const auto found = specs.find(*target); found != specs.end() && found->second->draws.size() == 1 && found->second->draws.front().flags == D3D11_CLEAR_DEPTH)
				flags &= D3D11_CLEAR_DEPTH;
			if (flags == 0)
				return false;  // nothing a pass would clear: the D3D11 call does nothing either
			return Take(*target, a_view, flags, Pending{ .depth = true, .view = a_view, .flags = a_flags, .depthValue = a_depth, .stencil = a_stencil });
		}

		void Flush(ID3D11DeviceContext* a_context)
		{
			auto& state = Skel();
			// Clears are taken on the render owner only (FrameAnchors::IsRenderOwner); another thread has none to flush.
			if (!state.setUp || state.flushing || !FrameAnchors::IsRenderOwner())
				return;
			for (std::size_t i = 0; i < kGroups; ++i) {
				auto& group = state.groups[i];
				if (!group.pending.empty()) {
					bool done = false;
					auto status = FrameAnchors::RunStatus::Failed;
					if (!group.failed) {
						group.executed = false;
						state.flushing = true;
						const auto result = FrameAnchors::Run(kGroupAnchors[i]);
						state.flushing = false;
						status = result.status;
						// The executed signal says the run's graph held this registration's passes. Without it the run failed (an error,
						// logged): the engine's clears are made here, in their order, so the frame stays whole.
						done = result && group.executed;
					}
					if (done) {
						group.taken += group.pending.size();
						if (!std::exchange(group.announced, true))
							logger::info("[ORG] engine skeleton: the first {} clears ran on the graph ({} anchor)", kGroupNames[i], FrameAnchors::Name(kGroupAnchors[i]));
					} else {
						group.replayed += group.pending.size();
						FailedRun(kGroupNames[i], status);
						Replay(a_context, group.pending);
					}
					group.pending.clear();
				}
				// The engine made one of the group's targets again (a resize): its passes are made again, once, with nothing pending.
				if (group.stale && !group.failed)
					Build(static_cast<Group>(i));
			}
		}

		bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !Requested())
				return false;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			if (a_destination != depths[DepthTarget::kPOST_ZPREPASS_COPY].texture || a_source != depths[DepthTarget::kMAIN].texture)
				return false;
			return RunCopy();
		}
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

namespace RenderGraph::Engine::Skeleton
{
	void Reserve() {}
	void Setup() {}
	bool CopyPostZPrepassDepth() { return false; }
	void MarkInstalled() {}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) { return false; }
		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8) { return false; }
		void Flush(ID3D11DeviceContext*) {}
		bool TakeDepthCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
	}
}

#endif
