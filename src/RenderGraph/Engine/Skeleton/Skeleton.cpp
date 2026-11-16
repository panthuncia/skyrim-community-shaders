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
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Engine/Native/Native.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameSequence.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/PassBuilders.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/VersionedTexture.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <atomic>
#	include <cstring>
#	include <limits>
#	include <map>
#	include <optional>
#	include <set>
#	include <tuple>
#	include <vector>

namespace DCLF
{
	// Features/DrawcallLimitFix/Core/Render/Shadow/ShadowPoints.cpp: whether DCLF draws this frame's shadow views. Render thread.
	bool DrawsShadowViews();
	// Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.cpp: whether DCLF's epochs run from here on in this frame (no load hold).
	bool EpochsRun();
	// The same file: whether the engine is inside Main::RenderDepth.
	bool InDepthPass();
}

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
		// Native mode only: colour views the engine clears that the RE list above leaves out. Their clears reach the native immediate
		// context, which hands them here (Sites::TakeContextColourClear; org-native-frame-graph.md, Phase 3): the TAA mask in the
		// ImageSpace chain and the two water targets at the water's draws. Guest mode keeps the engine's clears of them.
		constexpr std::array<std::uint32_t, 3> kNativeColourTargets{ ColourTarget::kTEMPORAL_AA_MASK, ColourTarget::kWATER_1, ColourTarget::kWATER_2 };
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

		/**
		 * @brief The engine copies the skeleton runs. Each has an anchor of its own and no run gate: a copy has no indirect form, so a
		 * run of its anchor is only ever made for that copy (an inactive client submits nothing).
		 */
		enum class CopyKind : std::uint8_t
		{
			Depth,  // the main depth into kPOST_ZPREPASS_COPY (Main::RenderDepth, FinishAccumulating, Deferred::DeferredPasses)
			Main,   // native mode only: kMAIN into its copy texture ("kMAIN Copy"), at the native immediate context's CopyResource
			Count
		};
		constexpr std::size_t kCopies = static_cast<std::size_t>(CopyKind::Count);
		constexpr std::array<Anchor, kCopies> kCopyAnchors{ Anchor::EngineDepthCopy, Anchor::EngineCopy };
		constexpr std::array<const char*, kCopies> kCopyIds{ "cs.engine.skeleton.depth-copy", "cs.engine.skeleton.main-copy" };
		constexpr std::array<const char*, kCopies> kCopySteps{ "post-zprepass", "main" };
		constexpr std::array<const char*, kCopies> kCopyNames{ "the depth copy (kMAIN into kPOST_ZPREPASS_COPY)", "the kMAIN copy (kMAIN into kMAIN Copy)" };

		/**
		 * @brief Where a world frame's clear or copy runs (org-native-frame-graph.md, "The declared frame, stage 2"). Own: on the
		 * skeleton's own anchor, a ticket of its own (every other frame kind, and what has no host yet). Any other: the pass is a
		 * member of the graph of the epoch that writes the target first after the engine's call, and that epoch's commit writes it
		 * from what the tap gathered (an early tap). Drawcall Limit Fix's epochs hold theirs at the start of their graph
		 * (HostedExtension); an anchor holds its as a client of its own, first by order.
		 */
		enum class Host : std::uint8_t
		{
			Own,
			ShadowView,      // DCLF's shadow epoch: the shadow map slices (its casters are the slices' only writers)
			ZPrepass,        // DCLF's Z-prepass: the main depth's clear at Main::RenderDepth's start
			SkyReflections,  // the reflection faces' sky (cs.sky.faces): each face and the faces' depth, cleared just before
			Prepass,         // the Prepass anchor: the normals target and kMAIN, cleared by Main::Draw (only light culling runs between)
			EngineDraws,     // the engine draws anchor: the shadow mask and the water's stencil, each just before its draws
			DeferredPasses,  // the DeferredPasses anchor: CS's own depth copy, made just before it in Deferred::DeferredPasses
			ImageSpace,      // the ImageSpace anchor: the water targets, cleared after the water's draws for the chain's water blend
			Count
		};
		constexpr std::size_t kHosts = static_cast<std::size_t>(Host::Count);

		struct HostInfo
		{
			const char* name;
			bool dclf;                           // a Drawcall Limit Fix epoch (segment), else an anchor
			RenderGraphRuntime::Segment segment;  // the DCLF epoch
			Anchor anchor;                       // the anchor
		};
		constexpr std::array<HostInfo, kHosts> kHostInfo{ {
			{ "own", false, RenderGraphRuntime::Segment::Count, Anchor::Count },
			{ "shadow-view", true, RenderGraphRuntime::Segment::ShadowView, Anchor::Count },
			{ "z-prepass", true, RenderGraphRuntime::Segment::ZPrepass, Anchor::Count },
			{ "sky-reflections", false, RenderGraphRuntime::Segment::Count, Anchor::SkyReflections },
			{ "prepass", false, RenderGraphRuntime::Segment::Count, Anchor::Prepass },
			{ "engine-draws", false, RenderGraphRuntime::Segment::Count, Anchor::EngineDraws },
			{ "deferred-passes", false, RenderGraphRuntime::Segment::Count, Anchor::DeferredPasses },
			{ "image-space", false, RenderGraphRuntime::Segment::Count, Anchor::ImageSpace },
		} };

		std::size_t HostIndex(Host a_host) { return static_cast<std::size_t>(a_host); }

		/**
		 * @brief Whether a host is an anchor a CS stage holds (pre-main's EngineDraws and Prepass steps, the deferred stage's DeferredPasses
		 * step, the water stage's EngineDraws and ImageSpace steps). Its clears are passes of the stage: the engine's call stages their gate entry
		 * and colour into the stage's frame inputs (FrameAnchors::StageInputs), and the stage's one commit carries them. Nothing waits for
		 * a host epoch, so no order between the call and the host's tap matters.
		 */
		bool StageHost(Host a_host)
		{
			return a_host == Host::Prepass || a_host == Host::EngineDraws || a_host == Host::DeferredPasses || a_host == Host::ImageSpace;
		}

		/** @brief CS_ORG_SEGMENT_CLEARS (native mode with segments, default on; =0 every clear and copy keeps its own anchor's ticket). */
		bool HostingOn()
		{
			static const bool on = [] {
				const bool requested = Native::Requested() && FrameAnchors::SegmentsOn() && !Switches::Off("CS_ORG_SEGMENT_CLEARS");
				logger::info("[ORG] engine skeleton: world frames' clears and copies run in the epochs that write their targets first: {}",
					requested ? "on (CS_ORG_SEGMENT_CLEARS=0 turns it off)" : "off");
				return requested;
			}();
			return on;
		}

		/** @brief Whether this frame's description draws the world (a world frame, or a frozen world's capture frame). Render owner. */
		bool WorldFrame()
		{
			using RenderGraph::Frame::Kind;
			const auto& frame = RenderGraph::Frame::Driver::Current();
			return frame.kind == Kind::World || (frame.kind == Kind::FrozenWorld && frame.capture);
		}

		/** @brief The segment value a host's runs submit (what RenderGraphRuntime's epoch hooks are called with). */
		std::uint32_t HostSegment(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			return info.dclf ? static_cast<std::uint32_t>(info.segment) : FrameAnchors::RunSegment(info.anchor);
		}

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
			FrameAnchors::Texture resource;  // the target's slot
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
				const auto declare = [&](const auto& a_range) -> ClearBindings {
					if (spec->depth)
						return { a_builder.DepthReadWrite(a_range, org::DsvView{ .slice = spec->slice }).View() };
					return { a_builder.RenderTarget(a_range, org::RtvView{ .slice = spec->slice }).View() };
				};
				// The slot of an engine target, so a new version of it needs no new pass; its slice and mip 0 through the range.
				const auto& texture = spec->resource;
				if (const auto& slot = texture.Slot()) {
					org::ResourceResolverAndRange range(*slot);
					range.range = org::Subresources(texture.Current(), org::Mip(0, 1), org::Slice(spec->slice, 1)).range;
					return declare(range);
				}
				return declare(org::Subresources(texture.Plain(), org::Mip(0, 1), org::Slice(spec->slice, 1)));
			}

			// The recording depends only on what the pass was made with (its registration and its first gate entry); the gate decides
			// each run.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(spec->generation);
				a_out.push_back(spec->draws.front().entry);
			}

			ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { spec }; }

			// Its commands depend on the run only through SkipsAtExecution: a kept late batch keeps its list while the answer holds.
			static constexpr bool kRecordKeyedBySkip = true;

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
			Target target;  // the view's engine table entry (named in the frame sequence census)
			Host host = Host::Own;
			bool skipped = false;  // a host-skipped tap error was counted for it
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
			std::vector<Pending> pending;        // the clears for the group's own anchor, flushed at the engine's bind
			std::vector<Pending> hostedPending;  // the clears waiting for their host's commit (a world frame's)
			// Per host: the hosted passes (each a view's draws that host runs) and whether they are in the host's graph now.
			std::array<std::vector<std::shared_ptr<const ClearSpec>>, kHosts> hosted;
			std::array<bool, kHosts> hostedRegistered{};  // an anchor host's client is registered
			std::array<std::atomic<std::uint32_t>, kHosts> hostedBuilt{};  // a DCLF host's graph was built with this registration
			std::uint32_t generation = 0;
			std::uint32_t committedLast = 0;  // the highest gate entry the last commit opened (regions: ClientDesc::joins)
			bool registered = false;  // a registration is live
			bool stale = false;       // a view's texture was made again: registered again at the next flush with nothing pending
			bool executed = false;    // the current run's executed signal
			bool failed = false;      // stays on D3D11 for the session
			bool announced = false;   // the first clear on the graph was logged
			std::uint64_t taken = 0, replayed = 0;
		};

		struct CopyState
		{
			// A world frame's copy runs in this host's next run (Host::Own: its own anchor's ticket).
			Host host = Host::Own;
			bool hostedRegistered = false;
			FrameAnchors::Texture source, target;
			std::shared_ptr<RunGate> gate;  // one entry, opened by the commit: the copy is gated, so it can run in a frame phase
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
			std::array<CopyState, kCopies> copies;
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

		CopyState& CopyOf(CopyKind a_kind) { return Skel().copies[static_cast<std::size_t>(a_kind)]; }

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
		FrameAnchors::Texture ResourceOf(const Target& a_target)
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
			if (Native::Requested())
				for (const auto index : kNativeColourTargets)
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
		 * @brief The host a world frame's clear of a_target with a_flags runs in (0 for colour). From the frame sequence census
		 * (CS_ORG_FRAME_SEQUENCE): the epoch that runs next after the engine's call and writes the target first.
		 */
		Host HostFor(const Target& a_target, std::uint32_t a_flags)
		{
			if (a_target.kind == Kind::Cube)
				return Host::SkyReflections;
			if (a_target.kind == Kind::Depth) {
				switch (a_target.index) {
				case DepthTarget::kCUBEMAP_REFLECTIONS:
					return Host::SkyReflections;
				case DepthTarget::kSHADOWMAPS_ESRAM:
				case DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM:
				case DepthTarget::kSHADOWMAPS:
					return Host::ShadowView;
				// The precipitation occlusion map is cleared by Precipitation::RenderOcclusion, at no fixed place before the shadow view.
				// It keeps its own ticket.
				case DepthTarget::kMAIN:
					// Main::RenderDepth's start clears depth and stencil; FinishAccumulating clears the stencil for the water mask.
					if (a_flags == (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL))
						return Host::ZPrepass;
					if (a_flags == D3D11_CLEAR_STENCIL)
						return Host::EngineDraws;
					return Host::Own;
				default:
					return Host::Own;
				}
			}
			switch (a_target.index) {
			case ColourTarget::kSHADOW_MASK:
				return Host::EngineDraws;
			case ColourTarget::kMAIN:
			case ColourTarget::kNORMAL_TAAMASK_SSRMASK:
			case ColourTarget::kNORMAL_TAAMASK_SSRMASK_SWAP:
				return Host::Prepass;
			case ColourTarget::kWATER_1:
			case ColourTarget::kWATER_2:
				return Host::ImageSpace;
			// The TAA mask and kMAIN_ONLY_ALPHA are cleared just before DCLF's forward epoch every frame, but the TAA mask also when TAA
			// is turned on, inside the ImageSpace chain: one host cannot follow both. They keep their own ticket.
			default:
				return Host::Own;
			}
		}

		/** @brief Whether the host can hold passes: a DCLF epoch in native mode, or an anchor with an epoch. */
		bool HostAvailable(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			if (a_host == Host::Own || !HostingOn())
				return false;
			return info.dclf ? RenderGraphRuntime::EpochsEnabled() : FrameAnchors::HasEpoch(info.anchor);
		}

		std::string HostedClientId(std::size_t a_group, Host a_host)
		{
			return fmt::format("{}@{}", kGroupIds[a_group], kHostInfo[HostIndex(a_host)].name);
		}

		/** @brief What the hosted extension puts in Drawcall Limit Fix's graphs: each group's passes per DCLF host, and their registration. */
		struct HostedSet
		{
			std::array<std::array<std::vector<std::shared_ptr<const ClearSpec>>, kHosts>, kGroups> specs;
			std::array<std::uint32_t, kGroups> generation{};
		};

		constexpr const char* kHostedExtensionId = "cs.engine.skeleton.hosted";

		/**
		 * @brief The skeleton's clears in Drawcall Limit Fix's epochs: at the start of the graph of each DCLF host, before DCLF's own
		 * passes that write the same views. Their gates and colours are written by the epoch's commit (EpochCommit). When the graph
		 * attaches, the registration it holds is noted, so a tap hands a clear to it only once its graph holds the pass.
		 */
		class HostedExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit HostedExtension(std::shared_ptr<const HostedSet> a_set) :
				set(std::move(a_set)) {}

			void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				if (!set)
					return;
				const std::uint32_t epoch = a_graph.PassEpochFilter();
				for (std::size_t h = 0; h < kHosts; ++h) {
					const auto& info = kHostInfo[h];
					if (!info.dclf || RenderGraphRuntime::EpochOf(info.segment) != epoch)
						continue;
					std::array<std::uint32_t, kGroups> built{};
					for (std::size_t g = 0; g < kGroups; ++g) {
						for (const auto& spec : set->specs[g][h])
							a_out.push_back(std::move(org::RenderGraph::ExternalPassDesc::Render(spec->name, std::make_shared<ClearPass>(spec))
									.At(org::ExternalInsertPoint::Begin())
									.Epoch(epoch)));
						built[g] = set->specs[g][h].empty() ? 0 : set->generation[g];
					}
					const auto attached = [h, built] {
						for (std::size_t g = 0; g < kGroups; ++g)
							Skel().groups[g].hostedBuilt[h].store(built[g], std::memory_order_release);
					};
					if (!RenderGraph::Builds::Defer(attached))
						attached();
				}
			}

		private:
			std::shared_ptr<const HostedSet> set;
		};

		/** @brief The hosted extension made again from every group's DCLF-hosted passes: their epochs' graphs are built again with them. */
		void PublishHosted()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !HostingOn())
				return;
			auto set = std::make_shared<HostedSet>();
			std::vector<std::uint32_t> epochs;
			for (std::size_t g = 0; g < kGroups; ++g) {
				set->generation[g] = Skel().groups[g].generation;
				for (std::size_t h = 0; h < kHosts; ++h)
					if (kHostInfo[h].dclf) {
						set->specs[g][h] = Skel().groups[g].hosted[h];
						if (std::ranges::find(epochs, RenderGraphRuntime::EpochOf(kHostInfo[h].segment)) == epochs.end())
							epochs.push_back(RenderGraphRuntime::EpochOf(kHostInfo[h].segment));
					}
			}
			std::shared_ptr<const HostedSet> published = std::move(set);
			host->AddExtension(kHostedExtensionId, [published]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
				return std::make_unique<HostedExtension>(published);
			}, epochs);
		}

		bool HasHostedPending(Group a_group, Host a_host)
		{
			return std::ranges::any_of(GroupOf(a_group).hostedPending, [&](const Pending& a_pending) { return a_pending.host == a_host; });
		}

		/**
		 * @brief The host's commit: the clears the taps handed to it since its last run are written into its run (each colour into
		 * the group's colour latch, each draw's gate entry opened), and leave the waiting list. a_close: the run's gate entries are
		 * closed first (a DCLF host's; an anchor closes its clients' gates itself).
		 */
		void CommitHosted(Group a_group, Host a_host, bool a_close)
		{
			auto& group = GroupOf(a_group);
			if (!group.gate || !group.colours)
				return;
			const org::RunRef run = RunGate::Current();
			if (a_close)
				group.gate->Close(run);
			std::uint32_t count = 0;
			std::erase_if(group.hostedPending, [&](const Pending& a_pending) {
				if (a_pending.host != a_host)
					return false;
				if (!a_pending.depth)
					group.colours->WriteValue(run, a_pending.gateEntry * RunGate::kEntryBytes, a_pending.colour);
				group.gate->OpenDraw(run, a_pending.gateEntry);
				++count;
				return true;
			});
			if (count) {
				group.taken += count;
				RenderGraph::Frame::Driver::NoteHosted(kHostInfo[HostIndex(a_host)].name, count);
			}
		}

		/**
		 * @brief The group's hosted passes, made from its registered ones: for each host, each view's draws that host runs in a world
		 * frame. An anchor host gets a client of its own; the DCLF hosts get the hosted extension.
		 */
		void RegisterHosted(Group a_group)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			for (auto& specs : group.hosted)
				specs.clear();
			if (HostingOn()) {
				for (const auto& [target, spec] : group.specs) {
					std::array<std::vector<ClearDraw>, kHosts> draws;
					for (const auto& draw : spec->draws)
						if (const auto host = HostFor(target, draw.flags); HostAvailable(host))
							draws[HostIndex(host)].push_back(draw);
					for (std::size_t h = 0; h < kHosts; ++h) {
						if (draws[h].empty())
							continue;
						auto hosted = std::make_shared<ClearSpec>(*spec);
						hosted->draws = std::move(draws[h]);
						hosted->name = fmt::format("{}@{}", spec->name, kHostInfo[h].name);
						group.hosted[h].push_back(std::move(hosted));
					}
				}
			}
			for (std::size_t h = 0; h < kHosts; ++h) {
				const auto host = static_cast<Host>(h);
				if (kHostInfo[h].dclf || host == Host::Own)
					continue;
				const auto id = HostedClientId(index, host);
				if (group.hosted[h].empty()) {
					if (std::exchange(group.hostedRegistered[h], false))
						FrameAnchors::Unregister(id);
					continue;
				}
				FrameAnchors::ClientDesc client{};
				client.id = id;
				for (const auto& spec : group.hosted[h]) {
					std::shared_ptr<const ClearSpec> pass = spec;
					client.passes.push_back({ .name = spec->name,
						.anchor = kHostInfo[h].anchor,
						// First at the anchor: the host's own passes write the cleared view after it.
						.order = std::numeric_limits<std::int32_t>::min() + static_cast<std::int32_t>(spec->draws.front().entry),
						.kind = FrameAnchors::PassKind::Render,
						.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
						.atExecution = true,
						.gated = true,
						.gpuGated = true });
				}
				if (StageHost(host)) {
					// A pass of a CS stage: the engine's call stages its entry and colour (Take); its step's tap has nothing to commit.
					client.active = [] { return false; };
				} else {
					client.active = [a_group, host] { return HasHostedPending(a_group, host); };
					client.commit = [a_group, host](const FrameAnchors::Point&) { CommitHosted(a_group, host, false); };
				}
				client.gates = { group.gate };
				group.hostedRegistered[h] = FrameAnchors::Register(std::move(client));
				if (!group.hostedRegistered[h]) {
					group.hosted[h].clear();
					logger::warn("[ORG] engine skeleton: the {} anchor refused the {} clears it hosts; they run on the {} anchor", FrameAnchors::Name(kHostInfo[h].anchor),
						kGroupNames[index], FrameAnchors::Name(kGroupAnchors[index]));
				}
			}
			for (std::size_t h = 0; h < kHosts; ++h)
				if (kHostInfo[h].dclf)
					group.hostedBuilt[h].store(0, std::memory_order_release);
			std::string summary;
			for (std::size_t h = 0; h < kHosts; ++h)
				if (!group.hosted[h].empty())
					summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", kHostInfo[h].name, group.hosted[h].size());
			if (!summary.empty())
				logger::info("[ORG] engine skeleton: a world frame's {} clears run in their hosts' epochs (views by host: {})", kGroupNames[index], summary);
		}

		/** @brief Whether a world frame's clear can go to a_host now: its graph holds this registration's passes. */
		bool HostReady(Group a_group, Host a_host)
		{
			if (a_host == Host::Own || StageHost(a_host) || !HostingOn() || !WorldFrame())
				return false;
			const auto& group = GroupOf(a_group);
			const auto h = HostIndex(a_host);
			if (group.hosted[h].empty())
				return false;
			// A Drawcall Limit Fix epoch runs only once DCLF draws the world (the frame's description says so). Its shadow epoch draws
			// the views only when DCLF's coverage decision for the frame, made before the first shadow map clear, took them.
			if (kHostInfo[h].dclf && (!RenderGraph::Frame::Driver::Current().dclfDraws || !DCLF::EpochsRun()))
				return false;
			if (a_host == Host::ShadowView && !DCLF::DrawsShadowViews())
				return false;
			// The main depth is cleared outside Main::RenderDepth too (a menu's 3D scene); only RenderDepth's clear is the Z-prepass's.
			if (a_host == Host::ZPrepass && !DCLF::InDepthPass())
				return false;
			if (kHostInfo[h].dclf)
				return group.hostedBuilt[h].load(std::memory_order_acquire) == group.generation;
			return group.hostedRegistered[h] && FrameAnchors::CanRun(HostedClientId(IndexOf(a_group), a_host), kHostInfo[h].anchor);
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
					.atExecution = true,
					// Its clears are gated draws that load the view: a region execution may hold the pass unopened.
					.gated = true,
					// Every clear is an indirect draw through the gate, so native mode records it ahead.
					.gpuGated = true });
				group.specs.emplace(spec->target, std::move(pass));
			}
			client.active = [a_group] { return !GroupOf(a_group).pending.empty(); };
			client.commit = [a_group](const FrameAnchors::Point&) {
				auto& state = GroupOf(a_group);
				const org::RunRef run = RenderGraphRuntime::Get().Host()->CurrentRun();
				std::uint32_t last = 0;
				for (const auto& pending : state.pending) {
					if (!pending.depth)
						state.colours->WriteValue(run, pending.gateEntry * RunGate::kEntryBytes, pending.colour);
					state.gate->OpenDraw(pending.gateEntry);
					last = (std::max)(last, pending.gateEntry);
				}
				state.committedLast = last;
			};
			// Regions: a run's clears join the execution an earlier run of this group committed into when each comes after every entry
			// that commit opened. Each view has its own entry and colour, and the passes run by entry, so they record in the
			// engine's order and write nothing twice. Back-to-back clears of the shadow slices and the frame's targets then share
			// one execution.
			client.joins = [a_group] {
				const auto& state = GroupOf(a_group);
				if (state.pending.empty())
					return false;
				for (const auto& pending : state.pending)
					if (pending.gateEntry <= state.committedLast)
						return false;
				return true;
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
			RegisterHosted(a_group);
			PublishHosted();
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
			a_pending.target = a_target;
			const auto host = HostFor(a_target, a_flags);
			// A world frame's clear of a target a CS stage clears: its entry and colour are staged into the stage's frame inputs now.
			if (StageHost(host) && HostingOn() && WorldFrame() && !GroupOf(group).hosted[HostIndex(host)].empty()) {
				const auto* spec = &state;
				const bool staged = FrameAnchors::StageInputs(HostedClientId(IndexOf(group), host), [&] {
					const org::RunRef run = RunGate::Current();
					if (!a_pending.depth)
						spec->colours->WriteValue(run, a_pending.gateEntry * RunGate::kEntryBytes, a_pending.colour);
					spec->gate->OpenDraw(run, a_pending.gateEntry);
				});
				if (staged) {
					++state.taken;
					RenderGraph::Frame::Driver::NoteHosted(kHostInfo[HostIndex(host)].name, 1);
					return true;
				}
			}
			// A world frame's clear waits for the Drawcall Limit Fix epoch (or the reflection faces' anchor) that writes the view first.
			if (HostReady(group, host)) {
				a_pending.host = host;
				state.hostedPending.push_back(a_pending);
				return true;
			}
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

		/** @brief A copy's engine textures now: its source and its target (D3D11 handles, compared to see a target made again). */
		std::pair<const void*, const void*> EngineTexturesOf(CopyKind a_kind)
		{
			auto* renderer = globals::game::renderer;
			if (a_kind == CopyKind::Depth) {
				const auto& depths = renderer->GetDepthStencilData().depthStencils;
				return { depths[DepthTarget::kMAIN].texture, depths[DepthTarget::kPOST_ZPREPASS_COPY].texture };
			}
			const auto& main = renderer->GetRuntimeData().renderTargets[ColourTarget::kMAIN];
			return { main.texture, main.textureCopy };
		}

		/**
		 * @brief A copy's graph textures: the engine targets' slots. kMAIN's copy texture is the published "Engine::kMAIN Copy" while
		 * ORG owns kMAIN (Engine::Targets), else an import of the D3D11 texture.
		 */
		std::pair<FrameAnchors::Texture, FrameAnchors::Texture> GraphTexturesOf(CopyKind a_kind)
		{
			if (a_kind == CopyKind::Depth)
				return { FrameAnchors::EngineDepth(DepthTarget::kMAIN), FrameAnchors::EngineDepth(DepthTarget::kPOST_ZPREPASS_COPY) };
			const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[ColourTarget::kMAIN];
			auto copy = Targets::Find(reinterpret_cast<const ID3D11Resource*>(main.textureCopy));
			if (!copy && main.textureCopy)
				copy = FrameAnchors::D3D11Texture("engine.kMAIN.copy", main.textureCopy, main.SRVCopy, nullptr);
			return { FrameAnchors::EngineTarget(ColourTarget::kMAIN), std::move(copy) };
		}

		void RegisterHostedCopy(CopyKind a_kind);

		/**
		 * @brief A copy's client registered for the engine's textures as they are now. Setup, and again only when the engine made
		 * either texture again (a resize).
		 *
		 * The copy has no run gate: a copy has no indirect form, and a raster copy can write neither the stencil the engine's
		 * CopyResource copies nor a copy texture (it has no render target view). It needs none: its anchor is the skeleton's own,
		 * nothing else registers there, and the anchor runs only from RunCopy, whose run always wants the copy (an inactive client
		 * submits nothing).
		 */
		void RegisterCopy(CopyKind a_kind)
		{
			const auto index = static_cast<std::size_t>(a_kind);
			auto& copy = CopyOf(a_kind);
			if (!globals::game::renderer)
				return;
			std::tie(copy.sourceTexture, copy.targetTexture) = EngineTexturesOf(a_kind);
			std::tie(copy.source, copy.target) = GraphTexturesOf(a_kind);
			if (!copy.source || !copy.target) {
				if (std::exchange(copy.registered, false))
					FrameAnchors::Unregister(kCopyIds[index]);
				logger::info("[ORG] engine skeleton: {}: a texture has no graph texture; it stays on D3D11", kCopyNames[index]);
				return;
			}
			if (copy.source.Width() != copy.target.Width() || copy.source.Height() != copy.target.Height() || copy.source.Format() != copy.target.Format()) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: {}: the textures differ in size or format; it stays on D3D11", kCopyNames[index]);
				return;
			}
			if (!copy.gate)
				copy.gate = RunGate::Create(kCopyIds[index], 1);
			if (!copy.gate) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: {}: no run gate; it stays on D3D11", kCopyNames[index]);
				return;
			}
			AnchorPasses::Builder builder(kCopyAnchors[index], kCopyIds[index], 0, {}, copy.gate);
			// With hosting the depth copy is a pass of the pre-main stage, which runs it in every execution (Main::RenderDepth always
			// copies there): ungated. Its own anchor's epoch runs only when the engine's call asks for it, so ungated is right there too.
			const bool declared = a_kind == CopyKind::Depth && HostingOn();
			builder.Add(AnchorPasses::Copy{
				.name = kCopySteps[index],
				.source = copy.source,
				.target = copy.target,
				.width = copy.source.Width(),
				.height = copy.source.Height(),
				.gate = declared ? std::nullopt : std::optional<std::uint32_t>(0u) });
			FrameAnchors::ClientDesc client{};
			client.id = kCopyIds[index];
			client.passes = builder.Take();
			client.gates = { copy.gate };
			// The run wants the copy (RunCopy): its entry opens.
			client.commit = [a_kind](const FrameAnchors::Point&) {
				if (const auto& gate = CopyOf(a_kind).gate)
					gate->OpenDispatch(0, 1);
			};
			client.active = [a_kind] { return CopyOf(a_kind).wanted; };
			client.executed = [a_kind](const FrameAnchors::Point&) { CopyOf(a_kind).executed = true; };
			client.onFailed = [a_kind] {
				const auto failed = static_cast<std::size_t>(a_kind);
				auto& state = CopyOf(a_kind);
				state.failed = true;
				state.registered = false;
				logger::error("[ORG] engine skeleton: the {} anchor failed; {} returns to D3D11", FrameAnchors::Name(kCopyAnchors[failed]), kCopyNames[failed]);
			};
			copy.registered = FrameAnchors::Register(std::move(client));
			if (!copy.registered) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: the {} anchor refused {}; it stays on D3D11", FrameAnchors::Name(kCopyAnchors[index]), kCopyNames[index]);
				return;
			}
			RegisterHostedCopy(a_kind);
		}

		/**
		 * @brief The copy in a world frame's host: CS's own depth copy runs first in the DeferredPasses anchor (Deferred::DeferredPasses
		 * makes it just before that anchor's run). The copy is gated there: the tap opens it for the host's next run.
		 */
		void RegisterHostedCopy(CopyKind a_kind)
		{
			auto& copy = CopyOf(a_kind);
			const auto index = static_cast<std::size_t>(a_kind);
			// The kMAIN copy keeps its own ticket: outdoors the water surface's draws follow it, indoors the ImageSpace chain does, and one
			// host cannot follow both.
			const Host host = a_kind == CopyKind::Depth ? Host::DeferredPasses : Host::Own;
			if (host == Host::Own)
				return;
			const auto& info = kHostInfo[HostIndex(host)];
			const auto id = fmt::format("{}@{}", kCopyIds[index], info.name);
			if (!HostAvailable(host)) {
				if (std::exchange(copy.hostedRegistered, false))
					FrameAnchors::Unregister(id);
				copy.host = Host::Own;
				return;
			}
			// A declared pass of the deferred stage, first at its DeferredPasses step: ungated, it runs in every execution of
			// the stage (Deferred::DeferredPasses copies in every world frame). The engine's call only checks that the stage holds it.
			AnchorPasses::Builder builder(info.anchor, id, std::numeric_limits<std::int32_t>::min(), {}, nullptr);
			builder.Add(AnchorPasses::Copy{
				.name = fmt::format("{}@{}", kCopySteps[index], info.name),
				.source = copy.source,
				.target = copy.target,
				.width = copy.source.Width(),
				.height = copy.source.Height(),
				.gate = std::nullopt });
			FrameAnchors::ClientDesc client{};
			client.id = id;
			client.passes = builder.Take();
			client.active = [] { return false; };
			copy.hostedRegistered = FrameAnchors::Register(std::move(client));
			copy.host = copy.hostedRegistered ? host : Host::Own;
			if (copy.hostedRegistered)
				logger::info("[ORG] engine skeleton: in a world frame {} runs first at the {} anchor", kCopyNames[index], FrameAnchors::Name(info.anchor));
		}

		/**
		 * @brief Whether a world frame's depth copy is a declared pass of the stage open now: the pre-main stage's (Main::RenderDepth's
			 * copy) or the deferred stage's (CS's own in Deferred::DeferredPasses). The engine's call then does nothing more.
		 */
		bool CopyDeclared(CopyKind a_kind, bool a_hostable)
		{
			const auto& copy = CopyOf(a_kind);
			if (a_kind != CopyKind::Depth || !HostingOn() || !WorldFrame())
				return false;
			if (a_hostable)
				return copy.host != Host::Own && copy.hostedRegistered &&
				       FrameAnchors::CanTap(fmt::format("{}@{}", kCopyIds[static_cast<std::size_t>(a_kind)], kHostInfo[HostIndex(copy.host)].name));
			return copy.registered && FrameAnchors::CanTap(kCopyIds[static_cast<std::size_t>(a_kind)]);
		}

		/**
		 * @brief The copy on the graph at this point; false when it did not run (the caller copies). a_hostable: the call site is
		 * the one the copy's host follows (CS's own depth copy, the kMAIN copy), so a world frame's copy waits for its host.
		 */
		bool RunCopy(CopyKind a_kind, bool a_hostable = false)
		{
			const auto index = static_cast<std::size_t>(a_kind);
			auto& copy = CopyOf(a_kind);
			if (copy.failed || !Usable())
				return false;
			if (EngineTexturesOf(a_kind) == std::pair{ copy.sourceTexture, copy.targetTexture } && CopyDeclared(a_kind, a_hostable)) {
				RenderGraph::Frame::Driver::NoteHosted(a_hostable ? "deferred stage" : "pre-main stage", 1);
				return true;
			}
			if (EngineTexturesOf(a_kind) != std::pair{ copy.sourceTexture, copy.targetTexture }) {
				logger::info("[ORG] engine skeleton: {}: a texture was made again; it registers again", kCopyNames[index]);
				RegisterCopy(a_kind);
				return false;
			}
			if (!copy.registered)
				return false;
			if (!FrameAnchors::CanRun(kCopyIds[index], kCopyAnchors[index]))
				return false;
			copy.wanted = true;
			copy.executed = false;
			auto& state = Skel();
			state.flushing = true;
			const auto result = FrameAnchors::Run(kCopyAnchors[index]);
			state.flushing = false;
			copy.wanted = false;
			// Drawn only when the run submitted with this registration's pass in its graph (the executed signal).
			const bool done = result && copy.executed;
			if (done && !std::exchange(copy.announced, true))
				logger::info("[ORG] engine skeleton: {} runs on the graph ({} anchor)", kCopyNames[index], FrameAnchors::Name(kCopyAnchors[index]));
			if (!done)
				FailedRun(kCopyNames[index], result.status);
			return done;
		}

		/**
		 * @brief One group's clears taken so far, on the graph now. When its run did not draw them, the engine's D3D11 calls are made
		 * in order through a_context. Then the group registers again if one of its targets was made again.
		 */
		void FlushGroup(ID3D11DeviceContext* a_context, std::size_t a_index)
		{
			auto& state = Skel();
			auto& group = state.groups[a_index];
			if (!group.pending.empty()) {
				// The frame sequence census (CS_ORG_FRAME_SEQUENCE) names the views each flush clears.
				if (RenderGraph::FrameSequence::On()) {
					std::string views;
					for (const auto& pending : group.pending)
						views += fmt::format("{}{}{}", views.empty() ? "" : ",", TargetName(pending.target), pending.depth ? fmt::format("/{}", pending.flags) : std::string());
					RenderGraph::FrameSequence::NoteBoundary(fmt::format("skeleton {} clears: {}", kGroupNames[a_index], views));
				}
				bool done = false;
				auto status = FrameAnchors::RunStatus::Failed;
				if (!group.failed) {
					// The TAA mask and kMAIN_ONLY_ALPHA are cleared after the water blend, the world segment's last step. Their ticket
					// must not be submitted while that segment holds its ticket: it would run on the GPU before the segment's work, and
					// the segment's reflections read kMAIN_ONLY_ALPHA. So the segment ends here, before the clear's ticket.
					const bool afterWorld = a_index == IndexOf(Group::Frame) && std::ranges::any_of(group.pending, [](const Pending& a_pending) {
						return a_pending.target.kind == Kind::Colour &&
					           (a_pending.target.index == ColourTarget::kTEMPORAL_AA_MASK || a_pending.target.index == ColourTarget::kMAIN_ONLY_ALPHA);
					});
					if (afterWorld)
						(void)FrameAnchors::TapPoint(static_cast<std::uint32_t>(RenderGraphRuntime::Segment::DeclaredWorld));
					group.executed = false;
					state.flushing = true;
					const auto result = FrameAnchors::Run(kGroupAnchors[a_index]);
					state.flushing = false;
					status = result.status;
					// The executed signal says the run's graph held this registration's passes. Without it the run failed (an error,
					// logged): the engine's clears are made here, in their order, so the frame stays whole.
					done = result && group.executed;
				}
				if (done) {
					group.taken += group.pending.size();
					if (!std::exchange(group.announced, true))
						logger::info("[ORG] engine skeleton: the first {} clears ran on the graph ({} anchor)", kGroupNames[a_index], FrameAnchors::Name(kGroupAnchors[a_index]));
				} else {
					group.replayed += group.pending.size();
					FailedRun(kGroupNames[a_index], status);
					// Still flushing while the calls are made. In native mode they reach the immediate context, which must not hand
					// them back here.
					state.flushing = true;
					Replay(a_context, group.pending);
					state.flushing = false;
				}
				group.pending.clear();
			}
			// The engine made one of the group's targets again (a resize): its passes are made again, once, with nothing pending.
			if (group.stale && !group.failed)
				Build(static_cast<Group>(a_index));
		}

		/** @brief The engine's immediate context (native mode's handle there). */
		ID3D11DeviceContext* EngineContext()
		{
			return reinterpret_cast<ID3D11DeviceContext*>(globals::game::renderer->GetRuntimeData().context);
		}

		/**
		 * @brief A clear the native immediate context was asked for outside the patched sites. It is taken and run on the graph now,
		 * at its place in the stream. False when the skeleton does not take it: the context records it itself.
		 */
		bool TakeNow(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, const Pending& a_pending)
		{
			if (FrameAnchors::InRun() || !Take(a_target, a_view, a_flags, a_pending))
				return false;
			FlushGroup(EngineContext(), IndexOf(GroupFor(a_target)));
			return true;
		}
	}

	namespace
	{
		/** @brief Before every epoch's submission: hosted work still waiting for another host saw this epoch go first (a tap error). */
		void EpochStarting(RenderGraphRuntime::Segment a_segment)
		{
			// A load that started in this frame ends its world work: what waits for it is abandoned at the next Begin.
			if (!FrameAnchors::IsRenderOwner() || RenderGraph::Frame::Driver::LoadStartedThisFrame())
				return;
			const auto segment = static_cast<std::uint32_t>(a_segment);
			auto& state = Skel();
			for (std::size_t g = 0; g < kGroups; ++g)
				for (auto& pending : state.groups[g].hostedPending) {
					if (pending.skipped || HostSegment(pending.host) == segment)
						continue;
					pending.skipped = true;
					RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::HostSkipped, RenderGraph::Frame::SegmentId::Count,
						fmt::format("the {} clear of {} waits for its host ({}), and segment {} was submitted first", kGroupNames[g], TargetName(pending.target),
							kHostInfo[HostIndex(pending.host)].name, segment));
				}
		}

		/** @brief First in a Drawcall Limit Fix host's commit: the clears its graph holds are written into its run. */
		void EpochCommit(RenderGraphRuntime::Segment a_segment)
		{
			if (!FrameAnchors::IsRenderOwner())
				return;
			for (std::size_t h = 0; h < kHosts; ++h) {
				if (!kHostInfo[h].dclf || kHostInfo[h].segment != a_segment)
					continue;
				for (std::size_t g = 0; g < kGroups; ++g) {
					auto& group = Skel().groups[g];
					if (!group.hosted[h].empty() && group.hostedBuilt[h].load(std::memory_order_acquire) == group.generation)
						CommitHosted(static_cast<Group>(g), static_cast<Host>(h), true);
				}
			}
		}
	}

	void BeginFrame()
	{
		auto& state = Skel();
		if (!state.setUp || !HostingOn() || !FrameAnchors::IsRenderOwner())
			return;
		// The frame that ends here was cut short by a loading screen: its waiting work is abandoned with the rest of its world work.
		if (RenderGraph::Frame::Driver::LoadStartedThisFrame()) {
			std::uint32_t abandoned = 0;
			for (auto& group : state.groups)
				abandoned += static_cast<std::uint32_t>(std::exchange(group.hostedPending, {}).size());
			if (abandoned)
				RenderGraph::Frame::Driver::NoteHosted("abandoned at a load's start", abandoned);
			return;
		}
		for (std::size_t g = 0; g < kGroups; ++g) {
			auto& group = state.groups[g];
			for (const auto& pending : group.hostedPending)
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::HostMissing, RenderGraph::Frame::SegmentId::Count,
					fmt::format("the {} clear of {} waited for its host ({}) until the frame ended; dropped", kGroupNames[g], TargetName(pending.target),
						kHostInfo[HostIndex(pending.host)].name));
			group.hostedPending.clear();
		}
	}

	void Reserve()
	{
		if (!Requested())
			return;
		bool reserved = true;
		for (const auto anchor : { Anchor::EngineShadowClear, Anchor::EngineCubeClear, Anchor::EngineFrameClear, Anchor::EngineDepthCopy })
			reserved = FrameAnchors::Reserve(anchor) && reserved;
		// The kMAIN copy reaches the skeleton only from native mode's immediate context.
		if (Native::Requested())
			reserved = FrameAnchors::Reserve(Anchor::EngineCopy) && reserved;
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
		for (std::size_t i = 0; i < kCopies; ++i) {
			const auto kind = static_cast<CopyKind>(i);
			auto& copy = state.copies[i];
			copy.failed = !FrameAnchors::HasEpoch(kCopyAnchors[i]) || (kind == CopyKind::Main && !Native::Requested());
			if (!copy.failed)
				RegisterCopy(kind);
		}
		state.setUp = true;
		if (HostingOn())
			RenderGraphRuntime::SetEpochHooks({ .starting = &EpochStarting, .commit = &EpochCommit });
		logger::info("[ORG] engine skeleton: on (CS_ORG_ENGINE_SKELETON); the engine's clears and its main depth copy are registered with the graph");
	}

	bool CopyPostZPrepassDepth()
	{
		return Requested() && RunCopy(CopyKind::Depth, true);
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
			for (std::size_t i = 0; i < kGroups; ++i)
				FlushGroup(a_context, i);
		}

		bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !Requested())
				return false;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			if (a_destination != depths[DepthTarget::kPOST_ZPREPASS_COPY].texture || a_source != depths[DepthTarget::kMAIN].texture)
				return false;
			return RunCopy(CopyKind::Depth);
		}

		bool TakeContextColourClear(ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Usable() || !a_colour)
				return false;
			const auto target = FindColour(a_view);
			if (!target)
				return false;
			Pending pending{ .depth = false, .view = a_view };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return TakeNow(*target, a_view, 0, pending);
		}

		bool TakeContextDepthClear(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			// As TakeDepthClear: the clear programs write depth 1 and stencil 0.
			if (!Usable() || ((a_flags & D3D11_CLEAR_DEPTH) && a_depth != 1.0f) || ((a_flags & D3D11_CLEAR_STENCIL) && a_stencil != 0))
				return false;
			const auto target = FindDepth(a_view);
			if (!target)
				return false;
			std::uint32_t flags = a_flags & (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL);
			const auto& specs = GroupOf(GroupFor(*target)).specs;
			if (const auto found = specs.find(*target); found != specs.end() && found->second->draws.size() == 1 && found->second->draws.front().flags == D3D11_CLEAR_DEPTH)
				flags &= D3D11_CLEAR_DEPTH;
			if (flags == 0)
				return false;
			return TakeNow(*target, a_view, flags, Pending{ .depth = true, .view = a_view, .flags = a_flags, .depthValue = a_depth, .stencil = a_stencil });
		}

		bool TakeContextCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			if (!Requested() || !globals::game::renderer || !a_destination || FrameAnchors::InRun())
				return false;
			for (std::size_t i = 0; i < kCopies; ++i) {
				const auto kind = static_cast<CopyKind>(i);
				if (EngineTexturesOf(kind) == std::pair<const void*, const void*>{ a_source, a_destination })
					return RunCopy(kind, kind == CopyKind::Main);
			}
			return false;
		}
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

namespace RenderGraph::Engine::Skeleton
{
	void Reserve() {}
	void Setup() {}
	void BeginFrame() {}
	bool CopyPostZPrepassDepth() { return false; }
	void MarkInstalled() {}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]) { return false; }
		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8) { return false; }
		void Flush(ID3D11DeviceContext*) {}
		bool TakeDepthCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
		bool TakeContextColourClear(ID3D11RenderTargetView*, const FLOAT[4]) { return false; }
		bool TakeContextDepthClear(ID3D11DepthStencilView*, UINT, FLOAT, UINT8) { return false; }
		bool TakeContextCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
	}
}

#endif
