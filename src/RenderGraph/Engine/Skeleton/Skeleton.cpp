#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Switches.h"
#include "Snapshot/EngineReads.h"

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#include "Globals.h"
#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/BuildManager.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/Observed.h"
#include "RenderGraph/Frame/PostChain.h"
#include "RenderGraph/Frame/ProgramSet.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/Switches.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/PassBuilders.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/VersionedTexture.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <source_location>
#include <string>
#include <tuple>
#include <vector>

namespace DCLF
{
	// From DCLF's ShadowPoints.cpp. Render thread.
	bool DrawsShadowViews();
	// From DCLF's FrameView.cpp. False during a load hold.
	bool EpochsRun();
	bool InDepthPass();
}

namespace RenderGraph::Engine::Skeleton
{
	namespace
	{
		using FrameAnchors::Anchor;
		namespace RF = ::RenderGraph::Frame;
		using ColourTarget = RE::RENDER_TARGETS;
		using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL;

		constexpr const char* kShader = "RenderGraph/EngineClear.hlsl";

		// The views the engine clears get one pass each, declared at Setup and registered again only for a remade target.
		// At a patched engine site, a clear of any other engine view is a tap error (EngineWork), so a new view never changes the registration.
		constexpr std::array<std::uint32_t, 8> kColourTargets{ ColourTarget::kMAIN, ColourTarget::kMAIN_COPY, ColourTarget::kMAIN_ONLY_ALPHA,
			ColourTarget::kNORMAL_TAAMASK_SSRMASK, ColourTarget::kNORMAL_TAAMASK_SSRMASK_SWAP, ColourTarget::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED,
			ColourTarget::kMOTION_VECTOR, ColourTarget::kSHADOW_MASK };
		// Cleared views missing from the RE list. Native mode hands their clears over through Sites::TakeContextColourClear.
		constexpr std::array<std::uint32_t, 5> kMoreColourTargets{ ColourTarget::kTEMPORAL_AA_MASK, ColourTarget::kWATER_1, ColourTarget::kWATER_2,
			ColourTarget::kRAW_WATER, ColourTarget::kUNDERWATER_MASK };
		constexpr std::array<std::uint32_t, 6> kDepthTargets{ DepthTarget::kMAIN, DepthTarget::kSHADOWMAPS_ESRAM, DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM,
			DepthTarget::kSHADOWMAPS, DepthTarget::kPRECIPITATION_OCCLUSION_MAP, DepthTarget::kCUBEMAP_REFLECTIONS };
		constexpr std::uint32_t kDepthSlices = 8;

		/**
		* @brief Each group has its own anchor, so a run declares only its group's views. The cube group has none: the start stage's
		* face sky clears the faces.
		*/
		enum class Group : std::uint8_t
		{
			Shadow,
			Cube,
			Frame,
			Count
		};
		constexpr std::size_t kGroups = static_cast<std::size_t>(Group::Count);
		constexpr std::array<const char*, kGroups> kGroupIds{ "cs.engine.skeleton.shadow", "cs.engine.skeleton.cube", "cs.engine.skeleton.frame" };
		constexpr std::array<const char*, kGroups> kGroupNames{ "shadow", "cube", "frame" };

		/** @brief Each copy has its own anchor and no run gate, since a copy has no indirect form. */
		enum class CopyKind : std::uint8_t
		{
			Depth,  // Main::RenderDepth, FinishAccumulating, Deferred::DeferredPasses
			Main,   // at the immediate context's CopyResource
			Count
		};
		constexpr std::size_t kCopies = static_cast<std::size_t>(CopyKind::Count);
		constexpr std::array<Anchor, kCopies> kCopyAnchors{ Anchor::EngineDepthCopy, Anchor::EngineCopy };
		constexpr std::array<const char*, kCopies> kCopyIds{ "cs.engine.skeleton.depth-copy", "cs.engine.skeleton.main-copy" };
		constexpr std::array<const char*, kCopies> kCopySteps{ "post-zprepass", "main" };
		constexpr std::array<const char*, kCopies> kCopyNames{ "the depth copy (kMAIN into kPOST_ZPREPASS_COPY)", "the kMAIN copy (kMAIN into kMAIN Copy)" };
		// The UI stage runs every frame but the engine copies only when a scene draws, so this client is gated.
		constexpr const char* kUiDepthCopyId = "cs.engine.skeleton.depth-copy@ui";

		/**
		* @brief Where a world frame's clear or copy runs. Own means the skeleton's own anchor.
		* Otherwise the pass joins the graph of the epoch that writes the target first after the engine's call.
		*/
		enum class Host : std::uint8_t
		{
			Own,
			ShadowView,      // its casters are the slices' only writers
			ZPrepass,
			Prepass,         // only light culling runs between these clears and the Prepass anchor
			EngineDraws,
			DeferredPasses,
			ImageSpace,      // the water targets, cleared after the water draws. Also the TAA
			                 // mask and kMAIN_ONLY_ALPHA, cleared after the water blend
			PostChain,       // the TAA mask, cleared inside the post chain when TAA is turned on
			Start,           // shadow slices when DCLF's shadow view does not host them
			PreDepth,        // the main depth clear when DCLF's Z-prepass does not host it
			Count
		};
		constexpr std::size_t kHosts = static_cast<std::size_t>(Host::Count);

		struct HostInfo
		{
			const char* name;
			bool dclf;                           // a DCLF epoch, else an anchor
			// A DCLF host's epoch, or the stage that holds an anchor host wholly. Count for an anchor that stages share.
			RenderGraphRuntime::Segment segment;
			Anchor anchor;
		};
		constexpr std::array<HostInfo, kHosts> kHostInfo{ {
			{ "own", false, RenderGraphRuntime::Segment::Count, Anchor::Count },
			{ "shadow-view", true, RenderGraphRuntime::Segment::ShadowView, Anchor::Count },
			{ "z-prepass", true, RenderGraphRuntime::Segment::ZPrepass, Anchor::Count },
			{ "prepass", false, RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(RenderGraph::Frame::SegmentId::PreMain)), Anchor::Prepass },
			{ "engine-draws", false, RenderGraphRuntime::Segment::Count, Anchor::EngineDraws },
			{ "deferred-passes", false, RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(RenderGraph::Frame::SegmentId::Deferred)), Anchor::DeferredPasses },
			{ "image-space", false, RenderGraphRuntime::Segment::Count, Anchor::ImageSpace },
			{ "post-chain", false, RenderGraphRuntime::Segment::Count, Anchor::ImageSpace },
			{ "start", false, RenderGraphRuntime::Segment::Count, Anchor::EngineShadowClear },
			{ "pre-depth", false, RenderGraphRuntime::Segment::Count, Anchor::EngineFrameClear },
		} };

		std::size_t HostIndex(Host a_host) { return static_cast<std::size_t>(a_host); }

		/**
		* @brief Places in the UI stage where a menu scene's clears run: stage start, scene start and after the composite.
		* Each place has its own gate entries because the main depth can be cleared at all three in one frame.
		*/
		enum class Place : std::uint8_t
		{
			MenuStart,
			MenuScene,
			MenuAfter,
			Count
		};
		constexpr std::size_t kPlaces = static_cast<std::size_t>(Place::Count);
		constexpr std::array<const char*, kPlaces> kPlaceNames{ "menu-start", "menu-scene", "menu-after" };

		/**
		* @brief Whether a host is an anchor a CS stage holds. Its clears are staged into the stage's frame inputs and
		* carried by the stage's one commit, so the order between the engine's call and the host's tap does not matter.
		*/
		bool StageHost(Host a_host)
		{
			return a_host == Host::Prepass || a_host == Host::EngineDraws || a_host == Host::DeferredPasses || a_host == Host::ImageSpace || a_host == Host::PostChain ||
			       a_host == Host::Start || a_host == Host::PreDepth;
		}

		/** @brief The CS stage place a DCLF host's clears fall back to in a frame where that host does not draw. */
		Host StagePlaceFor(Host a_host)
		{
			if (a_host == Host::ShadowView)
				return Host::Start;
			if (a_host == Host::ZPrepass)
				return Host::PreDepth;
			return StageHost(a_host) ? a_host : Host::Own;
		}

		/** @brief Whether a call belongs to a Bink frame: one is current, or a movie tick's work comes before the Begin of its own. */
		bool BinkFrame()
		{
			return RenderGraph::Frame::Driver::Current().kind == RenderGraph::Frame::Kind::Bink || RenderGraph::Frame::Driver::MovieFrameNext();
		}

		/** @brief A world frame or a frozen world's capture frame. A movie tick before its Begin is the coming Bink frame's. Render owner. */
		bool WorldFrame()
		{
			using RenderGraph::Frame::Kind;
			if (RenderGraph::Frame::Driver::MovieFrameNext())
				return false;
			const auto& frame = RenderGraph::Frame::Driver::Current();
			return frame.kind == Kind::World || (frame.kind == Kind::FrozenWorld && frame.capture);
		}

		std::uint32_t HostSegment(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			return static_cast<std::uint32_t>(info.segment);
		}

		enum class Kind : std::uint8_t
		{
			Colour,
			Depth,
			Cube,
		};

		struct Target
		{
			Kind kind = Kind::Colour;
			std::uint32_t index = 0;
			std::uint32_t slice = 0;  // depth slice or cube face

			auto Tie() const { return std::tie(kind, index, slice); }
			bool operator<(const Target& a_other) const { return Tie() < a_other.Tie(); }
			bool operator==(const Target& a_other) const { return Tie() == a_other.Tie(); }
		};

		/** @brief One clear draw of a view. A depth view has one per flag set the engine uses, so the view stays one pass. */
		struct ClearDraw
		{
			std::uint32_t flags = 0;
			// From the revision that registers the view, which starts only once it is built.
			std::shared_ptr<const RasterProgram> program;
			std::uint32_t entry = 0;
		};

		struct ClearSpec
		{
			std::string name;
			Target target;
			FrameAnchors::Texture resource;
			const void* view = nullptr;
			const void* texture = nullptr;  // a different texture here means the target was remade
			std::uint32_t slice = 0;
			bool depth = false;
			std::uint32_t width = 0, height = 0;
			std::vector<ClearDraw> draws;
			std::uint32_t generation = 0;
			std::shared_ptr<const RunGate> gate;
			std::shared_ptr<const org::LatchBlock> colours;  // a float4 per gate entry for colour clears
		};

		/** @brief Mirrors ENGINE_CLEAR_RESOURCES in EngineClear.hlsl. */
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
			std::uint32_t width = 0, height = 0;  // the version the frame binds
		};

		/**
		* @brief The clear's extent: the slot's version the preparation binds (a resize is a new version, and a revision may be prepared
		* while the old one is adopted), else the extent it was registered with.
		*/
		std::pair<std::uint32_t, std::uint32_t> ClearExtent(const ClearSpec& a_spec, const org::PassPrepareContext& a_context)
		{
			if (a_spec.resource.Slot()) {
				const auto size = a_spec.resource.PreparedSize(a_context);
				return { size.width, size.height };
			}
			return { a_spec.width, a_spec.height };
		}

		/** @brief Each draw clears only when the commit opened its gate entry. */
		class ClearPass final : public org::TypedRenderGraphPass<ClearPass, ClearFrame, ClearBindings>
		{
		public:
			explicit ClearPass(std::shared_ptr<const ClearSpec> a_spec) :
				spec(std::move(a_spec)) {}

			ClearBindings Declare(org::PassBuilder& a_builder)
			{
				// The engine's clear runs only on the runs whose tap opened one of its entries, whatever later passes read the target.
				a_builder.MayNotWrite();
				const auto declare = [&](const auto& a_range) -> ClearBindings {
					if (spec->depth)
						return { a_builder.DepthReadWrite(a_range, org::DsvView{ .slice = spec->slice }).View() };
					return { a_builder.RenderTarget(a_range, org::RtvView{ .slice = spec->slice }).View() };
				};
				// Uses the target's slot so a new version of it needs no new pass.
				const auto& texture = spec->resource;
				if (const auto& slot = texture.Slot()) {
					org::ResourceResolverAndRange range(*slot);
					range.range = org::Subresources(texture.Current(), org::Mip(0, 1), org::Slice(spec->slice, 1)).range;
					return declare(range);
				}
				return declare(org::Subresources(texture.Plain(), org::Mip(0, 1), org::Slice(spec->slice, 1)));
			}

			// The recording depends only on the registration and first gate entry. The gate decides each run. The extent follows
			// the bound version, which a kept recording checks itself.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(spec->generation);
				a_out.push_back(spec->draws.front().entry);
			}

			ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext& a_context) const
			{
				const auto [width, height] = ClearExtent(*spec, a_context);
				return { spec, width, height };
			}

			// A kept late batch keeps its list while SkipsAtExecution's answer holds.
			static constexpr bool kRecordKeyedBySkip = true;

			// A view none of whose clears were opened this run is not recorded.
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
					draw.viewport = { .x = 0, .y = 0, .width = a_frame.width, .height = a_frame.height };
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

		struct Pending
		{
			Target target;  // named in the frame sequence census
			Host host = Host::Own;
			bool skipped = false;
			std::uint32_t gateEntry = 0;
			bool depth = false;
			std::array<float, 4> colour{};
			UINT flags = 0;  // named in the frame sequence census
		};

		/** @brief One view a group may clear, as a frame start saw it. The pointers only name the engine's objects. */
		struct TargetFacts
		{
			Target target;
			const void* view = nullptr;
			const void* texture = nullptr;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			std::uint32_t slice = 0;
			bool shaped = false;  // one mip-0 slice
			std::uint32_t width = 0, height = 0;

			bool operator==(const TargetFacts&) const = default;
		};
		using GroupFacts = std::vector<TargetFacts>;

		/** @brief A clear program: the view's format and the depth flags, 0 for colour. */
		using ProgramKey = std::pair<rhi::Format, std::uint32_t>;

		/** @brief One registration of a group's clears: its views, its gates, and the copies each host and place runs. */
		struct GroupBuild
		{
			GroupFacts facts;
			std::uint32_t generation = 0;
			std::uint32_t entries = 0;
			std::map<Target, std::shared_ptr<const ClearSpec>> specs;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> colours;
			std::array<std::vector<std::shared_ptr<const ClearSpec>>, kHosts> hosted;
			std::array<std::map<Target, std::shared_ptr<const ClearSpec>>, kPlaces> placeSpecs;
			std::shared_ptr<RunGate> placeGate;
			std::shared_ptr<org::LatchBlock> placeColours;
		};

		/** @brief A client's key: the group's build for the revision's facts. Equal facts give the same build. */
		struct BuildKey
		{
			std::shared_ptr<GroupBuild> build;
			bool operator==(const BuildKey&) const = default;
		};

		/** @brief How many builds a group keeps, so a revision whose facts came back reuses its build. The clients keep theirs. */
		constexpr std::size_t kKeptBuilds = 8;

		struct GroupState
		{
			std::unique_ptr<RF::Observed<GroupFacts>> facts;
			std::deque<std::shared_ptr<GroupBuild>> builds;  // newest last
			std::array<RF::DeclaredClientOf<GroupBuild>, kHosts> hostClients;
			std::array<RF::DeclaredClientOf<GroupBuild>, kPlaces> placeClients;
			std::vector<Pending> hostedPending;
			std::array<std::atomic<std::uint32_t>, kHosts> hostedBuilt{};
			std::uint32_t generation = 0;
			std::uint64_t lent = 0;   // clears that found a lent slot
			bool failed = false;      // the cube group without an anchor: a stage's face sky clears the faces
			std::uint64_t taken = 0;
			std::uint64_t dropped = 0;  // clears replaced by a stage's load operations
			std::uint64_t placed = 0;
		};

		/** @brief A copy's engine textures, as a frame start saw them. */
		struct CopyFacts
		{
			const void* source = nullptr;
			const void* target = nullptr;
			bool operator==(const CopyFacts&) const = default;
		};

		/** @brief Where a copy client runs: its own anchor, first in a host's anchor, or in the UI stage. */
		enum class CopyAt : std::uint8_t
		{
			Own,
			Hosted,
			Ui,
		};

		struct CopyState
		{
			std::unique_ptr<RF::Observed<CopyFacts>> facts;
			Host host = Host::Own;
			std::shared_ptr<RunGate> gate;  // gated so the copy can run in a frame phase
			// Opened by the engine's call in the stage's frame inputs.
			std::shared_ptr<RunGate> uiGate;
			// Each registration's state is its key, so the frame reads the adopted textures from client.Adopted().
			RF::DeclaredClientOf<CopyFacts> client, hostedClient, uiClient;
			bool wanted = false;
			bool executed = false;
			bool announced = false;
			std::uint64_t dropped = 0;  // native mode: copies with no reader
		};

		struct SkeletonState
		{
			bool installed = false;
			bool setUp = false;
			bool flushing = false;
			std::array<GroupState, kGroups> groups;
			std::array<CopyState, kCopies> copies;
			// Keys known at setup join CS's setup batch. A new format is wanted at a frame start, and the revision waits for it.
			std::unique_ptr<RF::ProgramSet<ProgramKey, RasterProgram>> programs;
			std::map<Target, std::string> noClearPass;  // why a known view has no clear pass, logged once each
			std::set<Target> keptOnD3D11;                // engine clears with no pass, logged once each
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

		/** @brief The engine target a field handle is. Empty for anything else, such as a CS texture lent into a slot. */
		std::optional<Targets::TargetRef> IdentifyField(const ID3D11View* a_view)
		{
			if (!a_view || !Tripwire::IsHandle(a_view))
				return std::nullopt;
			return Targets::Identify(a_view);
		}

		/** @brief kFRAMEBUFFER, the window's, is left to D3D11, and so are mip views of a target. */
		std::optional<Target> FindColour(const ID3D11RenderTargetView* a_view)
		{
			const auto target = IdentifyField(a_view);
			if (!target)
				return std::nullopt;
			if (target->kind == Targets::Kind::Cubemap && target->view == Targets::View::CubeFace)
				return Target{ Kind::Cube, target->index, target->slice };
			if (target->kind != Targets::Kind::Colour || target->view != Targets::View::Rtv || target->index == ColourTarget::kFRAMEBUFFER)
				return std::nullopt;
			// A mip view target's RTV names its source with the mip. Only the source's own RTV is the target's.
			if (CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[target->index].RTV != a_view)
				return std::nullopt;
			return Target{ Kind::Colour, target->index, 0 };
		}

		/** @brief Writable views only. A view repeated in several slices is the first slice's, as KnownTargets lists it. */
		std::optional<Target> FindDepth(const ID3D11DepthStencilView* a_view)
		{
			const auto target = IdentifyField(a_view);
			if (!target || target->kind != Targets::Kind::Depth || target->view != Targets::View::Dsv)
				return std::nullopt;
			const auto& views = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[target->index].views;
			for (std::uint32_t slice = 0; slice < kDepthSlices; ++slice)
				if (views[slice] == a_view)
					return Target{ Kind::Depth, target->index, slice };
			return std::nullopt;
		}

		/** @brief The published texture when ORG owns the target, else the anchors' import. */
		FrameAnchors::Texture ResourceOf(const Target& a_target)
		{
			switch (a_target.kind) {
			case Kind::Colour:
				return FrameAnchors::EngineTexture(FrameAnchors::Colour(a_target.index));
			case Kind::Depth:
				return FrameAnchors::EngineTexture(FrameAnchors::Depth(a_target.index));
			default:
				return FrameAnchors::EngineTexture(FrameAnchors::Cubemap(a_target.index));
			}
		}

		ID3D11Texture2D* TextureOf(const Target& a_target)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
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
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
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

		/** @brief A clear program's description. Its key's depth flags are 0 for colour. */
		RasterProgram::Desc DescribeClear(const ProgramKey& a_key)
		{
			const auto [format, depthFlags] = a_key;
			RasterProgram::Desc desc{ .pixelSource = kShader, .vertexSource = kShader, .vertexEntry = L"VSMain", .commonDefines = false };
			if (depthFlags == 0) {
				desc.pixelEntry = L"PSColour";
				desc.constantWords = ShaderBindings::Words<ClearBlock>();
				desc.targets = { { .format = format } };
				return desc;
			}
			desc.pixelEntry = L"PSDepth";
			desc.depthFormat = format;
			const bool depth = (depthFlags & D3D11_CLEAR_DEPTH) != 0;
			desc.depth = { .depthEnable = depth, .depthWrite = depth, .depthFunc = rhi::CompareOp::Always };
			if (depthFlags & D3D11_CLEAR_STENCIL) {
				// The engine clears stencil to 0, so zero it whatever the test.
				const rhi::StencilFaceState zero{ .failOp = rhi::StencilOp::Zero, .depthFailOp = rhi::StencilOp::Zero, .passOp = rhi::StencilOp::Zero, .func = rhi::CompareOp::Always };
				desc.depth.stencilEnable = true;
				desc.depth.stencilReadMask = 0;
				desc.depth.stencilWriteMask = 0xFF;
				desc.depth.front = zero;
				desc.depth.back = zero;
			}
			return desc;
		}

		/** @brief A known view the skeleton makes no clear pass for. An engine clear of it is a tap error. Logged once per view. */
		void NoteNoClearPass(const Target& a_target, std::string_view a_reason)
		{
			if (Skel().noClearPass.insert_or_assign(a_target, std::string(a_reason)).second)
				logger::info("[ORG] engine skeleton: {} has no clear pass ({}); an engine clear of it is a tap error", TargetName(a_target), a_reason);
		}

		/** @brief An engine clear graph mode has no pass for. A tap error, and the clear is dropped. */
		void RefuseEngineClear(const Target& a_target, std::uint32_t a_flags, std::string_view a_why,
			const std::source_location& a_where = std::source_location::current())
		{
			RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::EngineWork, RenderGraph::Frame::SegmentId::Count,
				fmt::format("an engine clear of {} (flags {}): {}", TargetName(a_target), a_flags, a_why), a_where);
		}

		/** @brief A run that did not draw what it took. Development builds stop. Otherwise the work is not drawn and the engine does not redo it. */
		void FailedRun(const char* a_what, FrameAnchors::RunStatus a_status)
		{
			RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotExecuted, RenderGraph::Frame::SegmentId::Count,
				fmt::format("a run of the skeleton's {} did not execute (run status {})", a_what, static_cast<int>(a_status)));
		}

		/** @brief False when the view is not one mip-0 slice. */
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

		/** @brief In a stable order. */
		std::vector<Target> KnownTargets()
		{
			std::vector<Target> targets;
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			if (!renderer)
				return targets;
			// An unbacked target gets no clear pass. Its engine clear is declared dropped.
			for (const auto index : kColourTargets)
				if (!Targets::Unbacked(Targets::Colour(index)))
					targets.push_back({ Kind::Colour, index, 0 });
			for (const auto index : kMoreColourTargets)
				targets.push_back({ Kind::Colour, index, 0 });
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (const auto index : kDepthTargets) {
				for (std::uint32_t slice = 0; slice < kDepthSlices; ++slice) {
					const auto* view = depths[index].views[slice];
					// A slot repeating an earlier slot's view is the same view, and FindDepth finds the first.
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

		/** @brief The group's known views that the engine has this session, with only their objects filled. */
		GroupFacts ObjectsOf(const std::vector<Target>& a_known, Group a_group)
		{
			GroupFacts facts;
			for (const auto& target : a_known) {
				if (GroupFor(target) != a_group)
					continue;
				auto* view = ViewOf(target);
				auto* texture = TextureOf(target);
				if (view && texture)
					facts.push_back({ .target = target, .view = view, .texture = texture });
			}
			return facts;
		}

		/** @brief Whether two fact lists name the same views and textures. */
		bool SameObjects(const GroupFacts& a_left, const GroupFacts& a_right)
		{
			return std::ranges::equal(a_left, a_right, [](const TargetFacts& a_l, const TargetFacts& a_r) {
				return a_l.target == a_r.target && a_l.view == a_r.view && a_l.texture == a_r.texture;
			});
		}

		/** @brief Fills each view's shape, format and size from the engine's objects. Render owner, at a frame start. */
		void Describe(GroupFacts& a_facts)
		{
			for (auto& fact : a_facts) {
				const bool depth = fact.target.kind == Kind::Depth;
				fact.shaped = ViewShape(ViewOf(fact.target), depth, fact.format, fact.slice) && (fact.target.kind != Kind::Cube || fact.slice == fact.target.slice);
				D3D11_TEXTURE2D_DESC desc{};
				TextureOf(fact.target)->GetDesc(&desc);
				fact.width = desc.Width;
				fact.height = desc.Height;
			}
		}

		/** @brief The flag sets the engine clears a view with. The engine clears depth-only formats with 1, and stencil formats with 3 and 2. */
		std::vector<std::uint32_t> ClearFlagsOf(const TargetFacts& a_fact)
		{
			if (a_fact.target.kind != Kind::Depth)
				return { 0u };
			if (HasStencil(a_fact.format))
				return { D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, D3D11_CLEAR_STENCIL };
			return { D3D11_CLEAR_DEPTH };
		}

		/** @brief Whether the graph can clear the view: one mip-0 slice of a format that is not an integer one. */
		bool Clearable(const TargetFacts& a_fact)
		{
			return a_fact.shaped && !IsIntegerFormat(a_fact.format);
		}

		/** @brief Asks for the clear programs of a_facts. At setup they join CS's setup batch, later they gate the next revision. */
		void WantPrograms(const GroupFacts& a_facts, bool a_setup)
		{
			auto& programs = *Skel().programs;
			for (const auto& fact : a_facts) {
				if (!Clearable(fact))
					continue;
				for (const auto flags : ClearFlagsOf(fact)) {
					const ProgramKey key{ rhi::helpers::ToRHI(fact.format), flags };
					if (a_setup)
						programs.Queue(key);
					else
						(void)programs.Want(key);
				}
			}
		}

		/** @brief Null, logged, when the view cannot be cleared on the graph. a_in is the revision that registers it. */
		std::shared_ptr<ClearSpec> MakeSpec(const TargetFacts& a_fact, const RF::Inputs& a_in)
		{
			const auto& target = a_fact.target;
			if (!a_fact.shaped) {
				NoteNoClearPass(target, "not a single mip-0 slice");
				return nullptr;
			}
			if (IsIntegerFormat(a_fact.format)) {
				NoteNoClearPass(target, "an integer format");
				return nullptr;
			}
			auto resource = ResourceOf(target);
			if (!resource) {
				NoteNoClearPass(target, "no graph texture");
				return nullptr;
			}
			auto spec = std::make_shared<ClearSpec>();
			spec->target = target;
			spec->resource = std::move(resource);
			spec->view = a_fact.view;
			spec->texture = a_fact.texture;
			spec->slice = a_fact.slice;
			spec->depth = target.kind == Kind::Depth;
			spec->width = a_fact.width;
			spec->height = a_fact.height;
			for (const auto flags : ClearFlagsOf(a_fact)) {
				// The frame start that saw the view wanted its program, and this revision starts only once it is built.
				auto program = Skel().programs->For(a_in, ProgramKey{ rhi::helpers::ToRHI(a_fact.format), flags });
				if (!program)
					RenderGraph::Fatal("engine skeleton", fmt::format("{} has no clear program for flags {} in the revision that registers it", TargetName(target), flags));
				spec->draws.push_back({ .flags = flags, .program = std::move(program) });
			}
			Skel().noClearPass.erase(target);
			return spec;
		}

		/** @brief The epoch that runs next after the engine's call and writes the target first. */
		Host HostFor(const Target& a_target, std::uint32_t a_flags)
		{
			// In a world frame the start stage's sky draws clear the reflection faces with load operations, so the engine's clears drop.
			if (a_target.kind == Kind::Cube)
				return Host::Own;
			if (a_target.kind == Kind::Depth) {
				switch (a_target.index) {
				case DepthTarget::kCUBEMAP_REFLECTIONS:
					return Host::Own;
				case DepthTarget::kSHADOWMAPS_ESRAM:
				case DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM:
				case DepthTarget::kSHADOWMAPS:
					return Host::ShadowView;
				// Precipitation::RenderOcclusion clears this at no fixed place, so it keeps its own ticket.
				case DepthTarget::kMAIN:
					// Main::RenderDepth's start clears depth and stencil. FinishAccumulating clears stencil for the water mask.
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
			// The water blend (SE 100304, AE 107285) clears the three water targets together.
			case ColourTarget::kWATER_1:
			case ColourTarget::kWATER_2:
			case ColourTarget::kRAW_WATER:
				return Host::ImageSpace;
			// The underwater depth of field (ImageSpaceEffectDepthOfField::Render, inside the post chain) clears the mask once its
			// masked composites read it, so the start stage's mask draws begin the next frame on an empty mask.
			case ColourTarget::kUNDERWATER_MASK:
				return Host::PostChain;
			// Cleared after the water blend every frame, at the deferred+water stage's last ImageSpace step.
			// The post chain clears them again when TAA is turned on, before TAA reads them.
			case ColourTarget::kTEMPORAL_AA_MASK:
			case ColourTarget::kMAIN_ONLY_ALPHA:
				return RenderGraph::Frame::PostChain::InChain() ? Host::PostChain : Host::ImageSpace;
			case ColourTarget::kMOTION_VECTOR:
				return RenderGraph::Frame::PostChain::InChain() ? Host::PostChain : Host::Own;
			default:
				return Host::Own;
			}
		}

		/** @brief The colour targets the engine clears inside the post chain. */
		bool InChainClears(std::uint32_t a_target)
		{
			return a_target == ColourTarget::kTEMPORAL_AA_MASK || a_target == ColourTarget::kMOTION_VECTOR || a_target == ColourTarget::kMAIN_ONLY_ALPHA;
		}

		/** @brief A DCLF epoch, or an anchor (every anchor is a stage's). */
		bool HostAvailable(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			if (a_host == Host::Own)
				return false;
			return info.dclf || info.anchor != Anchor::Count;
		}

		const std::string& HostedClientId(std::size_t a_group, Host a_host)
		{
			// Asked at every staged clear: the ids are made once.
			static const auto ids = [] {
				std::array<std::array<std::string, kHosts>, kGroups> table;
				for (std::size_t group = 0; group < kGroups; ++group)
					for (std::size_t host = 0; host < kHosts; ++host)
						table[group][host] = fmt::format("{}@{}", kGroupIds[group], kHostInfo[host].name);
				return table;
			}();
			return ids[a_group][HostIndex(a_host)];
		}

		struct HostedSet
		{
			std::array<std::array<std::vector<std::shared_ptr<const ClearSpec>>, kHosts>, kGroups> specs;
			std::array<std::uint32_t, kGroups> generation{};
		};

		constexpr const char* kHostedExtensionId = "cs.engine.skeleton.hosted";

		/**
		* @brief Puts the skeleton's clears at the start of each DCLF host's graph, before DCLF's own writes.
		* A tap hands a clear to a host only once that host's graph holds the pass.
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

		/** @brief Rebuilds the DCLF epochs' graphs with every group's newest hosted passes. */
		void PublishHosted()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			auto set = std::make_shared<HostedSet>();
			std::vector<std::uint32_t> epochs;
			for (std::size_t g = 0; g < kGroups; ++g) {
				const auto& builds = Skel().groups[g].builds;
				const auto* newest = builds.empty() ? nullptr : builds.back().get();
				set->generation[g] = newest ? newest->generation : 0;
				for (std::size_t h = 0; h < kHosts; ++h)
					if (kHostInfo[h].dclf) {
						if (newest)
							set->specs[g][h] = newest->hosted[h];
						if (std::ranges::find(epochs, RenderGraphRuntime::EpochOf(kHostInfo[h].segment)) == epochs.end())
							epochs.push_back(RenderGraphRuntime::EpochOf(kHostInfo[h].segment));
					}
			}
			std::shared_ptr<const HostedSet> published = std::move(set);
			host->AddExtension(kHostedExtensionId, [published]() -> std::unique_ptr<org::RenderGraph::IRenderGraphExtension> {
				return std::make_unique<HostedExtension>(published);
			}, epochs);
		}

		/** @brief The build of a_facts, if the group still keeps it. */
		std::shared_ptr<GroupBuild> FindBuild(const GroupState& a_group, const GroupFacts& a_facts)
		{
			for (auto it = a_group.builds.rbegin(); it != a_group.builds.rend(); ++it)
				if ((*it)->facts == a_facts)
					return *it;
			return nullptr;
		}

		/**
		* @brief The build the frame uses: the adopted registration's, from setup's adoption on. Every registered client of a group
		* holds the build of the same facts, adopted at one Begin. Null while none is registered.
		*/
		std::shared_ptr<GroupBuild> AdoptedBuild(Group a_group)
		{
			const auto& group = GroupOf(a_group);
			for (const auto& client : group.hostClients)
				if (auto build = client.Adopted())
					return build;
			for (const auto& client : group.placeClients)
				if (auto build = client.Adopted())
					return build;
			return nullptr;
		}

		/** @brief Writes the clears handed to this DCLF host since its last run into the run, after closing the gate. */
		void CommitHosted(Group a_group, Host a_host)
		{
			auto& group = GroupOf(a_group);
			const auto build = AdoptedBuild(a_group);
			if (!build || !build->gate || !build->colours)
				return;
			const org::RunRef run = RunGate::Current();
			build->gate->Close(run);
			std::uint32_t count = 0;
			std::erase_if(group.hostedPending, [&](const Pending& a_pending) {
				if (a_pending.host != a_host)
					return false;
				if (!a_pending.depth)
					build->colours->WriteValue(run, a_pending.gateEntry * RunGate::kEntryBytes, a_pending.colour);
				build->gate->OpenDraw(run, a_pending.gateEntry);
				++count;
				return true;
			});
			if (count) {
				group.taken += count;
				RenderGraph::Frame::Driver::NoteHosted(kHostInfo[HostIndex(a_host)].name, count);
			}
		}

		/** @brief Splits a build's views by the hosts that run them. DCLF hosts get the hosted extension, anchor hosts a client each. */
		void SplitHosted(GroupBuild& a_build, std::size_t a_group)
		{
			for (const auto& [target, spec] : a_build.specs) {
				std::array<std::vector<ClearDraw>, kHosts> draws;
				for (const auto& draw : spec->draws) {
					if (const auto host = HostFor(target, draw.flags); HostAvailable(host))
						draws[HostIndex(host)].push_back(draw);
					if (const auto host = HostFor(target, draw.flags); kHostInfo[HostIndex(host)].dclf && HostAvailable(StagePlaceFor(host)))
						draws[HostIndex(StagePlaceFor(host))].push_back(draw);
					if (target.kind == Kind::Colour && InChainClears(target.index) && HostAvailable(Host::PostChain))
						draws[HostIndex(Host::PostChain)].push_back(draw);
				}
				for (std::size_t h = 0; h < kHosts; ++h) {
					if (draws[h].empty())
						continue;
					auto hosted = std::make_shared<ClearSpec>(*spec);
					hosted->draws = std::move(draws[h]);
					hosted->name = fmt::format("{}@{}", spec->name, kHostInfo[h].name);
					a_build.hosted[h].push_back(std::move(hosted));
				}
			}
			std::string summary;
			for (std::size_t h = 0; h < kHosts; ++h)
				if (!a_build.hosted[h].empty())
					summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", kHostInfo[h].name, a_build.hosted[h].size());
			if (!summary.empty())
				logger::info("[ORG] engine skeleton: a world frame's {} clears run in their hosts' epochs (views by host: {})", kGroupNames[a_group], summary);
		}

		/**
		* @brief An anchor host's client: its views first at the anchor, since the host's own passes write them after it. Every
		* anchor host is a stage host, so the engine's call stages the entry and colour, and the step has no tap.
		*/
		void HostedClient(const GroupBuild& a_build, Host a_host, FrameAnchors::ClientDesc& a_client)
		{
			const auto h = HostIndex(a_host);
			for (const auto& spec : a_build.hosted[h]) {
				std::shared_ptr<const ClearSpec> pass = spec;
				a_client.passes.push_back({ .name = spec->name,
					.anchor = kHostInfo[h].anchor,
					.order = std::numeric_limits<std::int32_t>::min() + static_cast<std::int32_t>(spec->draws.front().entry),
					.kind = FrameAnchors::PassKind::Render,
					.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
					.atExecution = true,
					// In these stages the engine's call stages the entry before the commit, so the CPU gate answer decides at record time.
					.gpuGated = !(a_host == Host::DeferredPasses || a_host == Host::ImageSpace || a_host == Host::PostChain || a_host == Host::Start ||
								  a_host == Host::PreDepth) });
			}
			a_client.active = [] { return false; };
			a_client.segmentOnly = true;
			a_client.gates = { a_build.gate };
		}

		const std::string& PlaceClientId(std::size_t a_group, std::size_t a_place)
		{
			// Asked at every clear: the ids are made once.
			static const auto ids = [] {
				std::array<std::array<std::string, kPlaces>, std::size(kGroupIds)> table;
				for (std::size_t group = 0; group < table.size(); ++group)
					for (std::size_t place = 0; place < kPlaces; ++place)
						table[group][place] = fmt::format("{}@{}", kGroupIds[group], kPlaceNames[place]);
				return table;
			}();
			return ids[a_group][a_place];
		}

		/** @brief The clears seen in loading screen and main menu frames. Any other clear there keeps its group anchor's ticket. */
		bool PlaceClears(Place, const Target& a_target)
		{
			if (a_target.kind == Kind::Depth)
				return a_target.index == DepthTarget::kMAIN || a_target.index == DepthTarget::kSHADOWMAPS;
			if (a_target.kind != Kind::Colour)
				return false;
			switch (a_target.index) {
			case ColourTarget::kMAIN:
			case ColourTarget::kMOTION_VECTOR:
			case ColourTarget::kNORMAL_TAAMASK_SSRMASK:
			case ColourTarget::kNORMAL_TAAMASK_SSRMASK_SWAP:
			case ColourTarget::kSHADOW_MASK:
				return true;
			default:
				return false;
			}
		}

		bool PlacesOn()
		{
			return FrameAnchors::NamedByStage(PlaceClientId(IndexOf(Group::Frame), 0));
		}

		/** @brief Copies each view's spec once per place that clears it, on that place's gate entries. */
		void SplitPlaces(GroupBuild& a_build, std::size_t a_group)
		{
			if (!PlacesOn() || !a_build.entries)
				return;
			a_build.placeGate = RunGate::Create(fmt::format("{}.places", kGroupIds[a_group]), a_build.entries * static_cast<std::uint32_t>(kPlaces));
			auto* host = RenderGraphRuntime::Get().Host();
			a_build.placeColours = host ? org::LatchBlock::Create(fmt::format("{}.places.colours", kGroupIds[a_group]),
											  a_build.entries * static_cast<std::uint32_t>(kPlaces) * RunGate::kEntryBytes) :
			                              nullptr;
			if (!a_build.placeGate || !a_build.placeColours)
				RenderGraph::Fatal("engine skeleton", fmt::format("the {} clears' places have no run gate", kGroupNames[a_group]));
			std::string summary;
			for (std::size_t place = 0; place < kPlaces; ++place) {
				for (const auto& [target, spec] : a_build.specs) {
					if (!PlaceClears(static_cast<Place>(place), target))
						continue;
					auto placed = std::make_shared<ClearSpec>(*spec);
					placed->name = fmt::format("{}@{}", spec->name, kPlaceNames[place]);
					placed->gate = a_build.placeGate;
					placed->colours = a_build.placeColours;
					for (auto& draw : placed->draws)
						draw.entry += static_cast<std::uint32_t>(place) * a_build.entries;
					a_build.placeSpecs[place].emplace(target, std::move(placed));
				}
				if (!a_build.placeSpecs[place].empty())
					summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", kPlaceNames[place], a_build.placeSpecs[place].size());
			}
			if (!summary.empty())
				logger::info("[ORG] engine skeleton: a menu scene's {} clears run in the UI stage (views by place: {})", kGroupNames[a_group], summary);
		}

		/** @brief A place's client: segment-only, since the engine's call stages the entry and colour and the step has no tap. */
		void PlaceClient(const GroupBuild& a_build, std::size_t a_place, FrameAnchors::ClientDesc& a_client)
		{
			for (const auto& [target, placed] : a_build.placeSpecs[a_place]) {
				const std::shared_ptr<const ClearSpec> pass = placed;
				a_client.passes.push_back({ .name = placed->name,
					.anchor = Anchor::EngineFrameClear,
					.order = static_cast<std::int32_t>(placed->draws.front().entry),
					.kind = FrameAnchors::PassKind::Render,
					.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
					// The engine's call stages the entry before the commit. A clear not asked for skips and folds its barriers.
					.atExecution = true });
			}
			a_client.active = [] { return false; };
			a_client.segmentOnly = true;
			a_client.gates = { a_build.placeGate };
		}

		/** @brief A new build of a group's clears for a_facts, with a new gate and colour latch, so an older recording never clears the wrong view. */
		std::shared_ptr<GroupBuild> MakeBuild(Group a_group, const GroupFacts& a_facts, const RF::Inputs& a_in)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			auto build = std::make_shared<GroupBuild>();
			build->facts = a_facts;
			build->generation = ++group.generation;
			std::vector<std::shared_ptr<ClearSpec>> made;
			for (const auto& fact : a_facts)
				if (auto spec = MakeSpec(fact, a_in)) {
					build->entries += static_cast<std::uint32_t>(spec->draws.size());
					made.push_back(std::move(spec));
				}
			if (made.empty())
				return build;
			build->gate = RunGate::Create(kGroupIds[index], build->entries);
			auto* host = RenderGraphRuntime::Get().Host();
			build->colours = host ? org::LatchBlock::Create(fmt::format("{}.colours", kGroupIds[index]), build->entries * RunGate::kEntryBytes) : nullptr;
			if (!build->gate || !build->colours)
				RenderGraph::Fatal("engine skeleton", fmt::format("the {} clears' run gate could not be made", kGroupNames[index]));
			std::uint32_t entry = 0;
			for (auto& spec : made) {
				for (auto& draw : spec->draws)
					draw.entry = entry++;
				spec->name = fmt::format("{}.{}", kGroupIds[index], TargetName(spec->target));
				spec->generation = build->generation;
				spec->gate = build->gate;
				spec->colours = build->colours;
				build->specs.emplace(spec->target, std::move(spec));
			}
			logger::info("[ORG] engine skeleton: the {} clears made ({} views, {} gate entries, registration {})", kGroupNames[index], build->specs.size(),
				build->entries, build->generation);
			SplitHosted(*build, index);
			SplitPlaces(*build, index);
			return build;
		}

		/**
		* @brief Render thread, in a client's key when a revision starts. The group's build for the revision's facts. Equal facts
		* give the same build, so every client of the group registers the same views and gates.
		*/
		std::shared_ptr<GroupBuild> BuildOf(Group a_group, const RF::Inputs& a_in)
		{
			auto& group = GroupOf(a_group);
			const auto* facts = group.facts ? group.facts->Get(a_in) : nullptr;
			if (!facts)
				return nullptr;
			if (auto found = FindBuild(group, *facts))
				return found;
			auto build = MakeBuild(a_group, *facts, a_in);
			group.builds.push_back(build);
			while (group.builds.size() > kKeptBuilds)
				group.builds.pop_front();
			PublishHosted();
			return build;
		}

		/** @brief The gate entry of a_target's draw for a_flags among a_specs, if one clears it. */
		std::optional<std::uint32_t> EntryIn(const std::vector<std::shared_ptr<const ClearSpec>>& a_specs, const Target& a_target, std::uint32_t a_flags)
		{
			for (const auto& spec : a_specs) {
				if (!(spec->target == a_target))
					continue;
				for (const auto& draw : spec->draws)
					if (draw.flags == a_flags)
						return draw.entry;
			}
			return std::nullopt;
		}

		/**
		* @brief Stages a clear into the UI stage place that FrameAnchors::StagePlace names, on the gate of the registration the
		* stage holds. False when no open stage has a place for it.
		*/
		bool TakeIntoPlace(Group a_group, const Target& a_target, std::uint32_t a_flags, const Pending& a_pending)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			static Util::ReasonLog s_kept("[ORG] engine skeleton: a clear at no place of the UI stage keeps its group anchor's ticket");
			std::array<std::string_view, kPlaces> views;
			for (std::size_t place = 0; place < kPlaces; ++place)
				views[place] = PlaceClientId(index, place);
			const auto place = FrameAnchors::StagePlace(views);
			if (place < 0)
				return !WorldFrame() && s_kept.Report(fmt::format("{}: no open stage has a place for the {} clears", RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind), kGroupNames[index])), false;
			const auto p = static_cast<std::size_t>(place);
			const auto held = group.placeClients[p].Held(Anchor::Count);
			if (!held)
				return s_kept.Report(fmt::format("the stage's schedule does not have the {} place's client on ({})", kPlaceNames[p], FrameAnchors::ReasonName(held.Reason()))), false;
			const auto found = held->placeSpecs[p].find(a_target);
			if (found == held->placeSpecs[p].end())
				return s_kept.Report(fmt::format("the {} place does not clear {}", kPlaceNames[p], TargetName(a_target))), false;
			const auto entry = EntryIn({ found->second }, a_target, a_flags);
			if (!entry)
				return s_kept.Report(fmt::format("the {} place has no clear of {} for flags {}", kPlaceNames[p], TargetName(a_target), a_flags)), false;
			const auto& build = *held;
			const bool staged = FrameAnchors::StageInputs(views[p], [&] {
				const org::RunRef run = RunGate::Current();
				if (!a_pending.depth)
					build.placeColours->WriteValue(run, *entry * RunGate::kEntryBytes, a_pending.colour);
				build.placeGate->OpenDraw(run, *entry);
			});
			if (!staged)
				return s_kept.Report(fmt::format("the stage did not take the {} place's inputs", kPlaceNames[p])), false;
			++group.taken;
			if (group.placed++ == 0)
				logger::info("[ORG] engine skeleton: the first {} clear at a place runs at the UI stage's {} place ({} frame)", kGroupNames[index], kPlaceNames[p],
					RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind));
			RenderGraph::Frame::Driver::NoteHosted(kPlaceNames[p], 1);
			return true;
		}

		/** @brief The same handle, or in native mode the same graph texture behind two handles. */
		bool SameImage(const void* a_engine, const void* a_call)
		{
			if (a_engine == a_call)
				return true;
			// Guest mode: an engine field may hold the guest context's handle, and a call the resource behind it.
			if (a_engine && a_call && Tripwire::UnwrapObject(static_cast<IUnknown*>(const_cast<void*>(a_engine))) == a_call)
				return true;
			if (!a_engine || !a_call || !Native::Active())
				return false;
			DxvkOrgInteropResourceInfo engine{}, call{};
			engine.version = call.version = DXVK_ORG_INTEROP_VERSION;
			return Native::DescribeResource(static_cast<const IUnknown*>(a_engine), engine) && Native::DescribeResource(static_cast<const IUnknown*>(a_call), call) &&
			       engine.image.image && engine.image.image == call.image.image;
		}

		/** @brief Whether a DCLF host's graph holds the adopted build's passes. */
		bool HostReady(Group a_group, const GroupBuild& a_build, Host a_host)
		{
			if (a_host == Host::Own || StageHost(a_host) || !WorldFrame())
				return false;
			const auto h = HostIndex(a_host);
			if (!kHostInfo[h].dclf || a_build.hosted[h].empty())
				return false;
			// DCLF epochs run only once DCLF draws the world. The shadow epoch draws the views only if that frame's coverage decision took them.
			if (!RenderGraph::Frame::Driver::Current().sceneDraws || !DCLF::EpochsRun())
				return false;
			if (a_host == Host::ShadowView && !DCLF::DrawsShadowViews())
				return false;
			// Menu 3D scenes also clear the main depth. Only RenderDepth's clear belongs to the Z-prepass.
			if (a_host == Host::ZPrepass && !DCLF::InDepthPass())
				return false;
			return GroupOf(a_group).hostedBuilt[h].load(std::memory_order_acquire) == a_build.generation;
		}

		/**
		* @brief The view's pass for a_flags in a_build. Null when there is none. Then a_engineKeeps says whether the caller's call
		* stays. Otherwise the clear was a tap error and is dropped. Only a patched engine site (a_engineSite) is a tap error. A
		* clear at the native context may be CS's own, so the context records it.
		*/
		const ClearDraw* DrawFor(Group a_group, const GroupBuild& a_build, const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, bool a_engineSite,
			bool& a_engineKeeps)
		{
			a_engineKeeps = false;
			// Some engine clears have no pass yet. They stay on D3D11, logged once per view, unless CS_ORG_ENGINE_DRAW_TAP=1 makes them
			// tap errors.
			const auto refuse = [&](std::string_view a_why) -> const ClearDraw* {
				static const bool strict = Switches::On("CS_ORG_ENGINE_DRAW_TAP");
				if (a_engineSite && strict) {
					RefuseEngineClear(a_target, a_flags, a_why);
					return nullptr;
				}
				a_engineKeeps = true;
				if (a_engineSite && Skel().keptOnD3D11.insert(a_target).second)
					logger::info("[ORG] engine skeleton: a clear of {} stays on D3D11 ({})", TargetName(a_target), a_why);
				return nullptr;
			};
			const auto found = a_build.specs.find(a_target);
			if (found == a_build.specs.end()) {
				// An engine-owned target stays the engine's D3D11 object (EngineTargetSet), so its clear stays with its draws.
				// An unbacked target's clear goes to the context, which drops it or reports the unbacked use.
				if (a_target.kind == Kind::Colour &&
					(Targets::EngineOwned(Targets::Colour(a_target.index)) || Targets::Unbacked(Targets::Colour(a_target.index)))) {
					a_engineKeeps = true;
					return nullptr;
				}
				std::string_view why = "not a view the skeleton clears";
				if (const auto noted = Skel().noClearPass.find(a_target); noted != Skel().noClearPass.end())
					why = noted->second;
				return refuse(why);
			}
			const auto& spec = *found->second;
			// Another view in the slot means a lent slot or a remade target, so this clear is not the pass's.
			if (spec.view != a_view || spec.texture != TextureOf(a_target)) {
				const bool remade = spec.texture != TextureOf(a_target);
				// Guest mode: Skylighting lends the precipitation occlusion slot, and the engine draws the map into it on D3D11.
				// The clear stays with those draws. Native mode lends no slot.
				if (!Native::Active() && a_target.kind == Kind::Depth && a_target.index == DepthTarget::kPRECIPITATION_OCCLUSION_MAP) {
					if (GroupOf(a_group).lent++ == 0)
						logger::info("[ORG] engine skeleton: a clear of {} found Skylighting's lent view in its slot; it stays with the engine's occlusion draws",
							TargetName(a_target));
					a_engineKeeps = true;
					return nullptr;
				}
				// The next frame start observes the new texture, and the revision after it registers the view again.
				if (remade) {
					if (a_engineSite) {
						static Util::ReasonLog s_remade("[ORG] engine skeleton: a clear lags (its target was made again; it registers with the next revision); nothing clears it this frame");
						s_remade.Report(TargetName(a_target));
					} else
						a_engineKeeps = true;
					return nullptr;
				}
				return refuse("another view of its texture is in its slot");
			}
			for (const auto& draw : spec.draws)
				if (draw.flags == a_flags)
					return &draw;
			return refuse("its clear flags have no pass");
		}

		/** @brief Native mode: a clear with no place is a mismatch and draws nothing. Returns true, the engine's call is taken. */
		bool NoPlace(const Target& a_target, std::uint32_t a_flags, std::string_view a_why)
		{
			const auto& frame = RenderGraph::Frame::Driver::Current();
			RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
				fmt::format("a clear of {} (flags {}) in a {} frame{}: {}", TargetName(a_target), a_flags, RenderGraph::Frame::KindName(frame.kind),
					frame.capture ? " (capture)" : "", a_why));
			return true;
		}

		/** @brief False when the caller makes its call: a clear the skeleton has no pass for at the native context (not a_engineSite). */
		bool Take(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, Pending a_pending, bool a_engineSite)
		{
			const Group group = GroupFor(a_target);
			auto& state = GroupOf(group);
			// In world frames the start stage's face sky clears the faces. Other frame kinds never read them, and the next world frame's
			// sky clears them. Setup leaves the cube group without passes for this.
			if (group == Group::Cube && state.failed) {
				if (state.dropped++ == 0)
					logger::info("[ORG] engine skeleton: reflection face clears are dropped; the start stage's face sky clears each face and its depth");
				return true;
			}
			const auto build = AdoptedBuild(group);
			if (!build || build->specs.empty()) {
				if (!a_engineSite)
					return false;
				RefuseEngineClear(a_target, a_flags, fmt::format("the adopted revision has no {} clear passes", kGroupNames[IndexOf(group)]));
				return true;
			}
			bool engineKeeps = false;
			const auto* draw = DrawFor(group, *build, a_target, a_view, a_flags, a_engineSite, engineKeeps);
			if (!draw)
				return !engineKeeps;
			a_pending.gateEntry = draw->entry;
			a_pending.target = a_target;
			// Bink frames and the session's first frame clear the main depth outside every stage, but nothing reads it there.
			// Every other frame kind clears it before its first read, so drop it. A movie's first tick clears it before its Begin,
			// still in the frame before the movie, and that clear is the Bink frame's too.
			if (a_target.kind == Kind::Depth && a_target.index == DepthTarget::kMAIN && PlacesOn() &&
				(BinkFrame() || RenderGraph::Frame::Driver::Current().frame == 0)) {
				if (state.dropped++ == 0)
					logger::info("[ORG] engine skeleton: a Bink frame's (or frame 0's) clear of the main depth is dropped (no reader before the next kind's own clear)");
				return true;
			}
			// The UI stage opens after every world stage ends, so a world stage's clear never finds a place.
			if (TakeIntoPlace(group, a_target, a_flags, a_pending))
				return true;
			const auto host = HostFor(a_target, a_flags);
			if (HostReady(group, *build, host)) {
				a_pending.host = host;
				state.hostedPending.push_back(a_pending);
				return true;
			}
			const auto unplaced = [&](std::string_view a_why) { return NoPlace(a_target, a_flags, a_why); };
			if (!WorldFrame())
				return unplaced("no open stage has a place for it");
			const auto place = StagePlaceFor(host);
			if (place == Host::Own || build->hosted[HostIndex(place)].empty())
				return unplaced("no stage of the world recipe clears it");
			const auto& info = kHostInfo[HostIndex(place)];
			const auto held = state.hostClients[HostIndex(place)].Held(info.anchor);
			if (!held.Placement().InStage())
				return unplaced(fmt::format("the {} stage place is not open", info.name));
			if (!held) {
				// The stage has no schedule yet, so the clear lags.
				static Util::ReasonLog s_lag("[ORG] engine skeleton: a clear lags (its stage's schedule does not hold the place yet); nothing clears it this frame");
				s_lag.Report(fmt::format("{} at the {} place ({})", TargetName(a_target), info.name, FrameAnchors::ReasonName(held.Reason())));
				return true;
			}
			// The registration the stage holds has its own gate entries.
			const auto entry = EntryIn(held->hosted[HostIndex(place)], a_target, a_flags);
			if (!entry)
				return unplaced(fmt::format("the {} stage place's registration does not clear it", info.name));
			const auto& holder = *held;
			const bool staged = FrameAnchors::StageInputs(HostedClientId(IndexOf(group), place), [&] {
				const org::RunRef run = RunGate::Current();
				if (!a_pending.depth)
					holder.colours->WriteValue(run, *entry * RunGate::kEntryBytes, a_pending.colour);
				holder.gate->OpenDraw(run, *entry);
			});
			if (!staged)
				return NoPlace(a_target, a_flags, fmt::format("the {} stage place did not take it", info.name));
			++state.taken;
			RenderGraph::Frame::Driver::NoteHosted(info.name, 1);
			return true;
		}

		/** @brief Compared to detect a remade target. */
		std::pair<const void*, const void*> EngineTexturesOf(CopyKind a_kind)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			if (a_kind == CopyKind::Depth) {
				const auto& depths = renderer->GetDepthStencilData().depthStencils;
				return { depths[DepthTarget::kMAIN].texture, depths[DepthTarget::kPOST_ZPREPASS_COPY].texture };
			}
			const auto& main = renderer->GetRuntimeData().renderTargets[ColourTarget::kMAIN];
			return { main.texture, main.textureCopy };
		}

		/** @brief kMAIN's copy texture is the published "Engine::kMAIN Copy" while ORG owns kMAIN, else a D3D11 import. */
		std::pair<FrameAnchors::Texture, FrameAnchors::Texture> GraphTexturesOf(CopyKind a_kind)
		{
			if (a_kind == CopyKind::Depth)
				return { FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kMAIN)), FrameAnchors::EngineTexture(FrameAnchors::Depth(DepthTarget::kPOST_ZPREPASS_COPY)) };
			const auto& main = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[ColourTarget::kMAIN];
			auto copy = Targets::Slot(Targets::ColourCopy(ColourTarget::kMAIN));
			if (!copy && main.textureCopy)
				copy = FrameAnchors::ImportTexture("cs.d3d11.engine.kMAIN.copy", { .texture = main.textureCopy, .srv = main.SRVCopy });
			return { FrameAnchors::EngineTexture(FrameAnchors::Colour(ColourTarget::kMAIN)), std::move(copy) };
		}

		const std::string& CopyClientId(CopyKind a_kind, CopyAt a_at)
		{
			// Asked at every copy: the ids are made once.
			static const auto ids = [] {
				std::array<std::array<std::string, 2>, std::size(kCopyIds)> table;
				for (std::size_t kind = 0; kind < table.size(); ++kind) {
					table[kind][0] = kCopyIds[kind];
					table[kind][1] = fmt::format("{}@{}", kCopyIds[kind], kHostInfo[HostIndex(Host::DeferredPasses)].name);
				}
				return table;
			}();
			static const std::string ui = kUiDepthCopyId;
			const auto index = static_cast<std::size_t>(a_kind);
			switch (a_at) {
			case CopyAt::Hosted:
				return ids[index][1];
			case CopyAt::Ui:
				return ui;
			default:
				return ids[index][0];
			}
		}

		/**
		* @brief A copy's client, for its own anchor, first in the deferred passes, or in the UI stage. Each frame's extent comes
		* from the current versions, so a resize needs no new registration.
		* - Own: no run gate entry for the depth copy, since Main::RenderDepth always copies; the kMAIN copy is gated. This anchor
		*   only runs from RunCopy, which always wants the copy.
		* - Hosted: ungated, since Deferred::DeferredPasses copies in every world frame.
		* - Ui: the engine's call opens its entry. A frame with no menu scene skips it.
		*/
		FrameAnchors::ClientDesc CopyClient(CopyKind a_kind, CopyAt a_at)
		{
			const auto index = static_cast<std::size_t>(a_kind);
			auto& copy = CopyOf(a_kind);
			auto [source, target] = GraphTexturesOf(a_kind);
			// The key holds both engine textures, so both must be on the graph.
			if (!source || !target)
				RenderGraph::Fatal("engine skeleton", fmt::format("{}: a texture has no graph texture", kCopyNames[index]));
			if (source.Width() != target.Width() || source.Height() != target.Height() || source.Format() != target.Format())
				RenderGraph::Fatal("engine skeleton", fmt::format("{}: the textures differ in size or format", kCopyNames[index]));
			const auto& hostInfo = kHostInfo[HostIndex(Host::DeferredPasses)];
			const auto& id = CopyClientId(a_kind, a_at);
			auto anchor = kCopyAnchors[index];
			std::int32_t order = 0;
			std::string step = kCopySteps[index];
			std::shared_ptr<RunGate> gate;
			std::optional<std::uint32_t> entry;
			switch (a_at) {
			case CopyAt::Own:
				gate = copy.gate;
				if (a_kind != CopyKind::Depth)
					entry = 0u;
				break;
			case CopyAt::Hosted:
				anchor = hostInfo.anchor;
				order = std::numeric_limits<std::int32_t>::min();
				step = fmt::format("{}@{}", step, hostInfo.name);
				break;
			case CopyAt::Ui:
				anchor = Anchor::EngineDepthCopy;
				gate = copy.uiGate;
				entry = 0u;
				step = "post-zprepass@ui";
				break;
			}
			AnchorPasses::Builder builder(anchor, id, order, {}, gate);
			// A gated copy runs only when the engine copies, whatever later passes read the target.
			builder.Add(AnchorPasses::Copy{
				.name = std::move(step), .source = std::move(source), .target = std::move(target), .width = 0, .height = 0, .gate = entry, .mayNotWrite = entry.has_value() });
			FrameAnchors::ClientDesc client{};
			client.id = id;
			client.passes = builder.Take();
			if (gate)
				client.gates = { gate };
			if (a_at == CopyAt::Own) {
				client.commit = [a_kind](const FrameAnchors::Point&) {
					if (const auto& own = CopyOf(a_kind).gate)
						own->OpenDispatch(0, 1);
				};
				client.active = [a_kind] { return CopyOf(a_kind).wanted; };
				client.executed = [a_kind](const FrameAnchors::Point&) { CopyOf(a_kind).executed = true; };
			} else {
				// The engine's call stages the entry, or the stage runs it every frame, so the step has no tap.
				client.active = [] { return false; };
				client.segmentOnly = true;
			}
			return client;
		}

		/** @brief Whether the open stage declares this depth copy. The engine's call then does nothing more. */
		bool CopyDeclared(CopyKind a_kind, bool a_hostable, const char*& a_where)
		{
			auto& copy = CopyOf(a_kind);
			if (a_kind != CopyKind::Depth)
				return false;
			// The UI stage opens after every world stage ends, so the pre-main copy never finds it open.
			if (copy.uiClient.Registered() && FrameAnchors::StageInputs(kUiDepthCopyId, [&] { copy.uiGate->OpenDispatch(RunGate::Current(), 0, 1); })) {
				a_where = "UI stage";
				return true;
			}
			if (!WorldFrame()) {
				static Util::ReasonLog s_notDeclared("[ORG] engine skeleton: a depth copy outside a world frame is not the UI stage's pass");
				const auto reason = copy.uiClient.Registered() ? fmt::format("{}: the UI stage is not open, or its schedule does not have the copy on",
																	 RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind)) :
				                                                 std::string("the UI stage's copy is not registered");
				s_notDeclared.Report(reason);
				return false;
			}
			a_where = a_hostable ? "deferred stage" : "pre-main stage";
			if (a_hostable)
				return copy.host != Host::Own && copy.hostedClient.Registered() && FrameAnchors::Place(CopyClientId(a_kind, CopyAt::Hosted)).built;
			return copy.client.Registered() && FrameAnchors::Place(kCopyIds[static_cast<std::size_t>(a_kind)]).built;
		}

		/** @brief False when the skeleton does not take the copy and the caller copies. a_hostable makes a world frame's copy wait for its host. */
		bool RunCopy(CopyKind a_kind, bool a_hostable = false)
		{
			const auto index = static_cast<std::size_t>(a_kind);
			auto& copy = CopyOf(a_kind);
			if (!Usable())
				return false;
			// RenderDepth's copy is its last act, so the pre-depth stage commits here at the latest.
			if (a_kind == CopyKind::Depth && !a_hostable && WorldFrame())
				(void)FrameAnchors::TapPoint(RenderGraph::Frame::SegmentId::PreDepth);
			// CS's depth copy is the deferred+water stage's first pass, so the pre-main stage ends here at the latest.
			if (a_kind == CopyKind::Depth && a_hostable && WorldFrame())
				(void)FrameAnchors::TapPoint(RenderGraph::Frame::SegmentId::PreMain);
			// The adopted registration's textures. A texture made again inside the frame registers with the revision after the next frame start.
			const auto adopted = copy.client.Adopted();
			const auto [sourceTexture, targetTexture] = EngineTexturesOf(a_kind);
			if (!adopted || adopted->source != sourceTexture || adopted->target != targetTexture) {
				static Util::ReasonLog s_remade("[ORG] engine skeleton: a copy lags (a texture was made again; it registers with the next revision); nothing copies this frame");
				s_remade.Report(kCopyNames[index]);
				return true;
			}
			const char* where = nullptr;
			if (CopyDeclared(a_kind, a_hostable, where)) {
				RenderGraph::Frame::Driver::NoteHosted(where, 1);
				return true;
			}
			if (!copy.client.Registered()) {
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::EngineWork, RenderGraph::Frame::SegmentId::Count,
					fmt::format("{}: the engine copied a texture the skeleton did not register", kCopyNames[index]));
				return true;
			}
			using RenderGraph::Frame::SegmentId;
			const auto start = static_cast<std::uint32_t>(SegmentId::Start);
			// The reflection cubemap's render copies the main depth while the start stage is open.
			// The pre-main copy overwrites kPOST_ZPREPASS_COPY before anyone reads it, so drop this one.
			if (a_kind == CopyKind::Depth && WorldFrame() && FrameAnchors::Stage(start).InStage()) {
				if (copy.dropped++ == 0)
					logger::info("[ORG] engine skeleton: {} while the start stage is open (the cubemap's render) is dropped: pre-main's copy writes it again before any reader",
						kCopyNames[index]);
				return true;
			}
			// The copy's anchor has no epoch of its own, so every copy is a stage's pass or dropped.
			const auto& frame = RenderGraph::Frame::Driver::Current();
			if (a_kind == CopyKind::Depth) {
				// The holding stage has no schedule yet, so the copy lags.
				auto holder = static_cast<std::uint32_t>(SegmentId::Ui);
				std::string holderId(kUiDepthCopyId);
				if (WorldFrame()) {
					holder = static_cast<std::uint32_t>(a_hostable ? SegmentId::Deferred : SegmentId::PreMain);
					holderId = CopyClientId(a_kind, a_hostable ? CopyAt::Hosted : CopyAt::Own);
				}
				if (FrameAnchors::Stage(holder).reason == FrameAnchors::Reason::Absent || FrameAnchors::Place(holderId).MayLag()) {
					static Util::ReasonLog s_lag("[ORG] engine skeleton: a copy lags (its stage has no schedule yet); nothing copies this frame");
					s_lag.Report(kCopyNames[index]);
					return true;
				}
				const auto isOpen = [](std::uint32_t a_segment) { return FrameAnchors::Stage(a_segment).reason == FrameAnchors::Reason::None; };
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
					fmt::format("{} in a {} frame came while no open stage holds it (open: start {}, pre-depth {}, pre-main {}, deferred+water {}; hostable {})",
						kCopyNames[index], RenderGraph::Frame::KindName(frame.kind), isOpen(start),
						isOpen(static_cast<std::uint32_t>(SegmentId::PreDepth)), isOpen(static_cast<std::uint32_t>(SegmentId::PreMain)),
						isOpen(static_cast<std::uint32_t>(SegmentId::Deferred)), a_hostable));
				return true;
			}
			const auto hold = FrameAnchors::Place(kCopyIds[index], kCopyAnchors[index]);
			if (!hold.InStage()) {
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
					fmt::format("{} in a {} frame came while no open stage holds it", kCopyNames[index], RenderGraph::Frame::KindName(frame.kind)));
				return true;
			}
			// The stage has no schedule yet, so the copy lags.
			if (!hold.draws || !hold.built) {
				static Util::ReasonLog s_lag("[ORG] engine skeleton: a copy lags (its stage's schedule does not hold it yet); nothing copies this frame");
				s_lag.Report(kCopyNames[index]);
				return true;
			}
			copy.wanted = true;
			copy.executed = false;
			auto& state = Skel();
			state.flushing = true;
			const auto result = FrameAnchors::Run(kCopyAnchors[index]);
			state.flushing = false;
			copy.wanted = false;
			const bool done = result && copy.executed;
			if (done && !std::exchange(copy.announced, true))
				logger::info("[ORG] engine skeleton: {} runs on the graph ({} anchor)", kCopyNames[index], FrameAnchors::Name(kCopyAnchors[index]));
			if (!done)
				FailedRun(kCopyNames[index], result.status);
			return true;
		}

		/**
		* @brief At a frame start. Observes the engine's targets and copy textures. A remade one is an event, so the next
		* revision registers its clients again and starts once their builds and any new clear program are built.
		*/
		void ObserveTargets(bool a_setup)
		{
			auto& state = Skel();
			if (!CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer))
				return;
			const auto known = KnownTargets();
			for (std::size_t g = 0; g < kGroups; ++g) {
				auto& group = state.groups[g];
				if (group.failed || !group.facts)
					continue;
				auto facts = ObjectsOf(known, static_cast<Group>(g));
				const auto* latest = group.facts->Latest();
				// A resize is a new version of the same slot, which the clears follow each frame.
				if (latest && SameObjects(*latest, facts))
					continue;
				Describe(facts);
				WantPrograms(facts, a_setup);
				const bool remade = latest != nullptr;
				if (group.facts->Observe(facts) && remade)
					logger::info("[ORG] engine skeleton: an engine target of the {} clears was made again; they register with the next revision", kGroupNames[g]);
			}
			for (std::size_t i = 0; i < kCopies; ++i) {
				auto& copy = state.copies[i];
				if (!copy.facts)
					continue;
				const auto [source, target] = EngineTexturesOf(static_cast<CopyKind>(i));
				const bool had = copy.facts->Latest() != nullptr;
				if (copy.facts->Observe(CopyFacts{ source, target }) && had)
					logger::info("[ORG] engine skeleton: {}: a texture was made again; it registers with the next revision", kCopyNames[i]);
			}
		}

		/** @brief The group's clients: one per anchor host and one per UI stage place, each keyed on the group's build. */
		void DeclareGroup(Group a_group)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			for (std::size_t h = 0; h < kHosts; ++h) {
				const auto host = static_cast<Host>(h);
				if (kHostInfo[h].dclf || host == Host::Own || !HostAvailable(host))
					continue;
				group.hostClients[h] = RF::DeclareClient<BuildKey, GroupBuild>(
					HostedClientId(index, host), RF::DependsOn(group.facts->Var()),
					[a_group, h](const RF::Inputs& a_in) -> std::optional<BuildKey> {
						auto build = BuildOf(a_group, a_in);
						if (!build || build->hosted[h].empty())
							return std::nullopt;
						return BuildKey{ std::move(build) };
					},
					[host](const BuildKey& a_key, FrameAnchors::ClientDesc& a_client) {
						HostedClient(*a_key.build, host, a_client);
						return a_key.build;
					});
			}
			// Every group declares its places, so each new facts value makes its build when a revision starts.
			for (std::size_t place = 0; place < kPlaces; ++place) {
				group.placeClients[place] = RF::DeclareClient<BuildKey, GroupBuild>(
					PlaceClientId(index, place), RF::DependsOn(group.facts->Var()),
					[a_group, place](const RF::Inputs& a_in) -> std::optional<BuildKey> {
						auto build = BuildOf(a_group, a_in);
						if (!build || build->placeSpecs[place].empty())
							return std::nullopt;
						return BuildKey{ std::move(build) };
					},
					[place](const BuildKey& a_key, FrameAnchors::ClientDesc& a_client) {
						PlaceClient(*a_key.build, place, a_client);
						return a_key.build;
					});
			}
		}

		/** @brief A copy's client at a_at, keyed on its engine textures. No client while the engine has either texture missing. */
		RF::DeclaredClientOf<CopyFacts> DeclareCopy(CopyKind a_kind, CopyAt a_at)
		{
			auto client = RF::DeclareClient<CopyFacts, CopyFacts>(
				CopyClientId(a_kind, a_at), RF::DependsOn(CopyOf(a_kind).facts->Var()),
				[a_kind](const RF::Inputs& a_in) -> std::optional<CopyFacts> {
					const auto* facts = CopyOf(a_kind).facts->Get(a_in);
					if (!facts || !facts->source || !facts->target)
						return std::nullopt;
					return *facts;
				},
				[a_kind, a_at](const CopyFacts& a_facts, FrameAnchors::ClientDesc& a_client) {
					a_client = CopyClient(a_kind, a_at);
					return std::make_shared<CopyFacts>(a_facts);
				});
			return client;
		}

		/** @brief A native immediate-context clear outside the patched sites, run on the graph now. False lets the context record it. */
		bool TakeNow(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, const Pending& a_pending)
		{
			return !FrameAnchors::InRun() && Take(a_target, a_view, a_flags, a_pending, false);
		}

		/** @brief The flags a depth clear asks the graph for, 0 for none. A format without stencil clears depth only, whatever the flags ask. */
		std::uint32_t DepthClearFlags(const Target& a_target, UINT a_flags)
		{
			std::uint32_t flags = a_flags & (D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL);
			if (const auto build = AdoptedBuild(GroupFor(a_target))) {
				const auto found = build->specs.find(a_target);
				if (found != build->specs.end() && found->second->draws.size() == 1 && found->second->draws.front().flags == D3D11_CLEAR_DEPTH)
					flags &= D3D11_CLEAR_DEPTH;
			}
			return flags;
		}

		/** @brief The target of a depth clear the graph can make, else empty. The clear programs only write depth 1 and stencil 0, the only values the engine uses. */
		std::optional<Target> DepthClearTarget(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			if (!Usable() || ((a_flags & D3D11_CLEAR_DEPTH) && a_depth != 1.0f) || ((a_flags & D3D11_CLEAR_STENCIL) && a_stencil != 0))
				return std::nullopt;
			return FindDepth(a_view);
		}

		/** @brief A colour clear's pending entry. */
		Pending ColourPending(const FLOAT a_colour[4])
		{
			Pending pending{ .depth = false };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return pending;
		}
	}

	namespace
	{
		/** @brief Before each epoch's submission. Hosted work still waiting for another host is a tap error. */
		void EpochStarting(RenderGraphRuntime::Segment a_segment)
		{
			// A load started this frame abandons its waiting world work at the next Begin.
			if (!FrameAnchors::IsRenderOwner() || RenderGraph::Frame::Driver::LoadStartedThisFrame())
				return;
			// The start stage commits just before the shadow view epoch and the pre-depth stage just before the Z-prepass.
			// Neither touches the targets those epochs host, so the order is safe.
			if (a_segment == RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(RenderGraph::Frame::SegmentId::Start)) ||
				a_segment == RenderGraphRuntime::StageSegment(static_cast<std::uint32_t>(RenderGraph::Frame::SegmentId::PreDepth)))
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

		/** @brief First in a DCLF host's commit. */
		void EpochCommit(RenderGraphRuntime::Segment a_segment)
		{
			if (!FrameAnchors::IsRenderOwner())
				return;
			for (std::size_t h = 0; h < kHosts; ++h) {
				if (!kHostInfo[h].dclf || kHostInfo[h].segment != a_segment)
					continue;
				for (std::size_t g = 0; g < kGroups; ++g) {
					const auto build = AdoptedBuild(static_cast<Group>(g));
					if (build && !build->hosted[h].empty() && Skel().groups[g].hostedBuilt[h].load(std::memory_order_acquire) == build->generation)
						CommitHosted(static_cast<Group>(g), static_cast<Host>(h));
				}
			}
		}
	}

	void BeginFrame()
	{
		auto& state = Skel();
		if (!state.setUp || !FrameAnchors::IsRenderOwner())
			return;
		// Checked at frame start rather than at a clear, because a slot can be lent within a frame.
		ObserveTargets(false);
		// A loading screen cut this frame short, so its waiting work is abandoned.
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

	void Setup()
	{
		auto& state = Skel();
		if (!Engine::GraphMode() || state.setUp)
			return;
		if (!state.installed)
			RenderGraph::Fatal("engine skeleton", "its engine hooks are not installed");
		if (!RenderGraphRuntime::Get().IsActive() || !CS_ENGINE_READ(Renderer, Other, globals::game::renderer))
			RenderGraph::Fatal("engine skeleton", "the render graph is not active");
		// Everything is declared here, so a clear never registers anything. A remade target registers with a revision.
		state.programs = std::make_unique<RF::ProgramSet<ProgramKey, RasterProgram>>("EngineSkeleton.Clears", &DescribeClear);
		for (std::size_t i = 0; i < kGroups; ++i) {
			// The start stage's face sky clears the reflection faces, so the cube group has no anchor and no passes.
			if (static_cast<Group>(i) == Group::Cube) {
				state.groups[i].failed = true;
				continue;
			}
			state.groups[i].facts = std::make_unique<RF::Observed<GroupFacts>>(fmt::format("EngineSkeleton.{}", kGroupNames[i]));
		}
		for (std::size_t i = 0; i < kCopies; ++i) {
			auto& copy = state.copies[i];
			copy.gate = RunGate::Create(kCopyIds[i], 1);
			if (static_cast<CopyKind>(i) == CopyKind::Depth)
				copy.uiGate = RunGate::Create(kUiDepthCopyId, 1);
			copy.facts = std::make_unique<RF::Observed<CopyFacts>>(fmt::format("EngineSkeleton.{}", kCopyIds[i]));
		}
		// The setup's clear programs join CS's setup batch, and the first revision registers every client.
		ObserveTargets(true);
		for (std::size_t i = 0; i < kGroups; ++i)
			if (!state.groups[i].failed)
				DeclareGroup(static_cast<Group>(i));
		for (std::size_t i = 0; i < kCopies; ++i) {
			const auto kind = static_cast<CopyKind>(i);
			auto& copy = state.copies[i];
			copy.client = DeclareCopy(kind, CopyAt::Own);
			// The kMAIN copy keeps its own ticket. Water draws follow it outdoors and the ImageSpace chain indoors, and one host cannot follow both.
			if (kind == CopyKind::Depth && HostAvailable(Host::DeferredPasses)) {
				copy.hostedClient = DeclareCopy(kind, CopyAt::Hosted);
				copy.host = Host::DeferredPasses;
				logger::info("[ORG] engine skeleton: in a world frame {} runs first at the {} anchor", kCopyNames[i],
					FrameAnchors::Name(kHostInfo[HostIndex(Host::DeferredPasses)].anchor));
			}
			if (kind == CopyKind::Depth)
				copy.uiClient = DeclareCopy(kind, CopyAt::Ui);
		}
		state.setUp = true;
		// Native mode answers its own copies before the hook is asked.
		if (const auto* main = state.copies[static_cast<std::size_t>(CopyKind::Main)].facts->Latest(); main && main->source && main->target)
			Tripwire::SetGuestCopyHook(&Sites::TakeContextCopy);
		RenderGraphRuntime::SetEpochHooks({ .starting = &EpochStarting, .commit = &EpochCommit });
		logger::info("[ORG] engine skeleton: on; the engine's clears and its main depth copy are declared and register with the first revision");
	}

	bool CopyPostZPrepassDepth()
	{
		return Engine::GraphMode() && RunCopy(CopyKind::Depth, true);
	}

	void MarkInstalled()
	{
		Skel().installed = true;
	}

	bool ClearColour(RE::RENDER_TARGET a_target, const float a_colour[4])
	{
		auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
		auto* view = renderer ? renderer->GetRuntimeData().renderTargets[a_target].RTV : nullptr;
		return view && Sites::TakeColourClear(globals::d3d::context, view, a_colour);
	}

	bool ClearDepth(RE::RENDER_TARGET_DEPTHSTENCIL a_target, std::uint32_t a_flags)
	{
		auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
		auto* view = renderer ? renderer->GetDepthStencilData().depthStencils[a_target].views[0] : nullptr;
		return view && Sites::TakeDepthClear(globals::d3d::context, view, a_flags, 1.0f, 0);
	}

	bool CopyMain()
	{
		// As the engine's copy reaching the immediate context (Sites::TakeContextCopy).
		return Engine::GraphMode() && CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer) && !FrameAnchors::InRun() && RunCopy(CopyKind::Main, true);
	}

	namespace Sites
	{
		bool TakeColourClear(ID3D11DeviceContext*, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Usable() || !a_colour)
				return false;
			const auto target = FindColour(a_view);
			return target && Take(*target, a_view, 0, ColourPending(a_colour), true);
		}

		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			const auto target = DepthClearTarget(a_view, a_flags, a_depth, a_stencil);
			if (!target)
				return false;
			const auto flags = DepthClearFlags(*target, a_flags);
			// No flags left: the D3D11 call does nothing either.
			return flags && Take(*target, a_view, flags, Pending{ .depth = true, .flags = a_flags }, true);
		}

		bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			if (!renderer || !Engine::GraphMode())
				return false;
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			if (!SameImage(depths[DepthTarget::kPOST_ZPREPASS_COPY].texture, a_destination) || !SameImage(depths[DepthTarget::kMAIN].texture, a_source))
				return false;
			return RunCopy(CopyKind::Depth);
		}

		bool TakeContextColourClear(ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Usable() || !a_colour)
				return false;
			const auto target = FindColour(a_view);
			return target && TakeNow(*target, a_view, 0, ColourPending(a_colour));
		}

		bool TakeContextDepthClear(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			const auto target = DepthClearTarget(a_view, a_flags, a_depth, a_stencil);
			if (!target)
				return false;
			const auto flags = DepthClearFlags(*target, a_flags);
			return flags && TakeNow(*target, a_view, flags, Pending{ .depth = true, .flags = a_flags });
		}

		bool TakeContextCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			if (!Engine::GraphMode() || !CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer) || !a_destination || FrameAnchors::InRun())
				return false;
			for (std::size_t i = 0; i < kCopies; ++i) {
				const auto kind = static_cast<CopyKind>(i);
				const auto [source, target] = EngineTexturesOf(kind);
				if (SameImage(source, a_source) && SameImage(target, a_destination))
					return RunCopy(kind, kind == CopyKind::Main);
			}
			return false;
		}
	}
}

