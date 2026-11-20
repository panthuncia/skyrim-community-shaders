#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Switches.h"

bool RenderGraph::Engine::Skeleton::Requested()
{
	static const bool requested = RenderGraph::Engine::Native::GraphMode();
	return requested;
}

#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#	include "Globals.h"
#	include "RenderGraph/AnchorPasses.h"
#	include "RenderGraph/BuildManager.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/Frame/EngineTargetUse.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/PostChain.h"
#	include "RenderGraph/Engine/Targets/EngineTargets.h"
#	include "RenderGraph/Engine/Tripwire/Tripwire.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/FrameSequence.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/RunGate.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "RenderGraph/Switches.h"
#	include "Utils/Logging.h"

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
		using ColourTarget = RE::RENDER_TARGETS;
		using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL;

		constexpr const char* kShader = "RenderGraph/EngineClear.hlsl";

		// The views the engine clears get one pass each, registered once at Setup.
		// A clear of any other view keeps the engine's call, so a new view never changes the registration.
		constexpr std::array<std::uint32_t, 8> kColourTargets{ ColourTarget::kMAIN, ColourTarget::kMAIN_COPY, ColourTarget::kMAIN_ONLY_ALPHA,
			ColourTarget::kNORMAL_TAAMASK_SSRMASK, ColourTarget::kNORMAL_TAAMASK_SSRMASK_SWAP, ColourTarget::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED,
			ColourTarget::kMOTION_VECTOR, ColourTarget::kSHADOW_MASK };
		// Cleared views missing from the RE list. Native mode hands their clears over through Sites::TakeContextColourClear.
		constexpr std::array<std::uint32_t, 3> kMoreColourTargets{ ColourTarget::kTEMPORAL_AA_MASK, ColourTarget::kWATER_1, ColourTarget::kWATER_2 };
		constexpr std::array<std::uint32_t, 6> kDepthTargets{ DepthTarget::kMAIN, DepthTarget::kSHADOWMAPS_ESRAM, DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM,
			DepthTarget::kSHADOWMAPS, DepthTarget::kPRECIPITATION_OCCLUSION_MAP, DepthTarget::kCUBEMAP_REFLECTIONS };
		constexpr std::uint32_t kDepthSlices = 8;

		/** @brief Each group has its own anchor, so a run declares only its group's views. */
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
			RenderGraphRuntime::Segment segment;
			Anchor anchor;
		};
		constexpr std::array<HostInfo, kHosts> kHostInfo{ {
			{ "own", false, RenderGraphRuntime::Segment::Count, Anchor::Count },
			{ "shadow-view", true, RenderGraphRuntime::Segment::ShadowView, Anchor::Count },
			{ "z-prepass", true, RenderGraphRuntime::Segment::ZPrepass, Anchor::Count },
			{ "prepass", false, RenderGraphRuntime::Segment::Count, Anchor::Prepass },
			{ "engine-draws", false, RenderGraphRuntime::Segment::Count, Anchor::EngineDraws },
			{ "deferred-passes", false, RenderGraphRuntime::Segment::Count, Anchor::DeferredPasses },
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

		/** @brief A world frame or a frozen world's capture frame. Render owner. */
		bool WorldFrame()
		{
			using RenderGraph::Frame::Kind;
			const auto& frame = RenderGraph::Frame::Driver::Current();
			return frame.kind == Kind::World || (frame.kind == Kind::FrozenWorld && frame.capture);
		}

		std::uint32_t HostSegment(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			return info.dclf ? static_cast<std::uint32_t>(info.segment) : FrameAnchors::RunSegment(info.anchor);
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
		};

		/** @brief Each draw clears only when the commit opened its gate entry. */
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
				// Uses the target's slot so a new version of it needs no new pass.
				const auto& texture = spec->resource;
				if (const auto& slot = texture.Slot()) {
					org::ResourceResolverAndRange range(*slot);
					range.range = org::Subresources(texture.Current(), org::Mip(0, 1), org::Slice(spec->slice, 1)).range;
					return declare(range);
				}
				return declare(org::Subresources(texture.Plain(), org::Mip(0, 1), org::Slice(spec->slice, 1)));
			}

			// The recording depends only on the registration and first gate entry. The gate decides each run.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(spec->generation);
				a_out.push_back(spec->draws.front().entry);
			}

			ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { spec }; }

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

		struct GroupState
		{
			std::map<Target, std::shared_ptr<const ClearSpec>> specs;
			std::shared_ptr<RunGate> gate;
			std::shared_ptr<org::LatchBlock> colours;
			std::vector<Pending> hostedPending;
			std::array<std::vector<std::shared_ptr<const ClearSpec>>, kHosts> hosted;
			std::array<bool, kHosts> hostedRegistered{};
			std::array<std::atomic<std::uint32_t>, kHosts> hostedBuilt{};
			std::uint32_t generation = 0;
			bool registered = false;
			bool stale = false;       // registered again at the next flush with nothing pending
			std::uint64_t lent = 0;   // clears that found a lent slot
			bool failed = false;      // stays on D3D11 for the session
			std::uint64_t taken = 0;
			std::uint64_t dropped = 0;  // clears replaced by a stage's load operations
			std::array<std::map<Target, std::shared_ptr<const ClearSpec>>, kPlaces> placeSpecs;
			std::shared_ptr<RunGate> placeGate;
			std::shared_ptr<org::LatchBlock> placeColours;
			std::array<bool, kPlaces> placeRegistered{};
			std::uint32_t entries = 0;
			std::uint64_t placed = 0;
		};

		struct CopyState
		{
			Host host = Host::Own;
			bool hostedRegistered = false;
			FrameAnchors::Texture source, target;
			std::shared_ptr<RunGate> gate;  // gated so the copy can run in a frame phase
			// Opened by the engine's call in the stage's frame inputs.
			std::shared_ptr<RunGate> uiGate;
			bool uiRegistered = false;
			const void* sourceTexture = nullptr;
			const void* targetTexture = nullptr;
			bool registered = false;
			bool wanted = false;
			bool executed = false;
			bool failed = false;
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
			// Made at registration only, never per clear.
			std::map<std::pair<rhi::Format, std::uint32_t>, std::shared_ptr<const RasterProgram>> programs;
			std::set<Target> refused;  // logged once each
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

		/** @brief kFRAMEBUFFER, the window's, is left to D3D11. */
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

		/** @brief Writable views only. */
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

		/** @brief The published texture when ORG owns the target, else the anchors' import. */
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

		/** @brief Built only at registration, one attempt per key. a_depthFlags is 0 for colour. */
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
					// The engine clears stencil to 0, so zero it whatever the test.
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
			// Kept even when null, so each format is tried once.
			state.programs.emplace(key, program);
			return program;
		}

		/** @brief The engine keeps the clear. Logged once per view. */
		void Refuse(const Target& a_target, std::string_view a_reason)
		{
			if (Skel().refused.emplace(a_target).second)
				logger::info("[ORG] engine skeleton: {} stays on D3D11 ({})", TargetName(a_target), a_reason);
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
			auto* renderer = globals::game::renderer;
			if (!renderer)
				return targets;
			// An unbacked target in native mode gets no clear pass. Its engine clear is declared dropped.
			for (const auto index : kColourTargets)
				if (!Frame::EngineTargetUse::ColourUnbacked(index))
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

		/** @brief Null, logged, when the view cannot be cleared on the graph. */
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
			// The engine clears depth-only formats with 1, and stencil formats with 3 and 2.
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

		/** @brief The epoch that runs next after the engine's call and writes the target first, from the CS_ORG_FRAME_SEQUENCE census. */
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
			case ColourTarget::kWATER_1:
			case ColourTarget::kWATER_2:
				return Host::ImageSpace;
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

		/** @brief Whether the start stage's sky clears the reflection faces with load operations in world frames. */
		bool FacesClearedBySky()
		{
			return FrameAnchors::HeldByStage(Anchor::SkyReflections);
		}

		/** @brief A DCLF epoch in native mode, or an anchor with an epoch. */
		bool HostAvailable(Host a_host)
		{
			const auto& info = kHostInfo[HostIndex(a_host)];
			if (a_host == Host::Own)
				return false;
			return info.dclf || FrameAnchors::HasEpoch(info.anchor);
		}

		std::string HostedClientId(std::size_t a_group, Host a_host)
		{
			return fmt::format("{}@{}", kGroupIds[a_group], kHostInfo[HostIndex(a_host)].name);
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

		/** @brief Rebuilds the DCLF epochs' graphs with every group's hosted passes. */
		void PublishHosted()
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
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

		/** @brief Writes the clears handed to this host since its last run into the run. a_close closes the gate first, for DCLF hosts. */
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

		/** @brief An anchor host gets a client of its own. DCLF hosts get the hosted extension. */
		void RegisterHosted(Group a_group)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			for (auto& specs : group.hosted)
				specs.clear();
			for (const auto& [target, spec] : group.specs) {
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
					group.hosted[h].push_back(std::move(hosted));
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
						// First at the anchor, since the host's own passes write the cleared view after it.
						.order = std::numeric_limits<std::int32_t>::min() + static_cast<std::int32_t>(spec->draws.front().entry),
						.kind = FrameAnchors::PassKind::Render,
						.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
						.atExecution = true,
						// In these stages the engine's call stages the entry before the commit, so the CPU gate answer decides at record time.
						.gpuGated = !(host == Host::DeferredPasses || host == Host::ImageSpace || host == Host::PostChain || host == Host::Start || host == Host::PreDepth) });
				}
				if (StageHost(host)) {
					// The engine's call stages the entry and colour, so the step's tap has nothing to commit.
					client.active = [] { return false; };
					client.segmentOnly = true;
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

		std::string PlaceClientId(std::size_t a_group, std::size_t a_place) { return fmt::format("{}@{}", kGroupIds[a_group], kPlaceNames[a_place]); }

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
			return FrameAnchors::HasEpoch(Anchor::EngineFrameClear) && FrameAnchors::NamedByStage(PlaceClientId(IndexOf(Group::Frame), 0));
		}

		/** @brief Copies each view's spec once per place that clears it, on that place's gate entries. Segment-only. */
		void RegisterPlaces(Group a_group)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			for (auto& specs : group.placeSpecs)
				specs.clear();
			const auto unregisterAll = [&] {
				for (std::size_t place = 0; place < kPlaces; ++place)
					if (std::exchange(group.placeRegistered[place], false))
						FrameAnchors::Unregister(PlaceClientId(index, place));
			};
			if (!PlacesOn() || !group.entries) {
				unregisterAll();
				return;
			}
			group.placeGate = RunGate::Create(fmt::format("{}.places", kGroupIds[index]), group.entries * static_cast<std::uint32_t>(kPlaces));
			auto* host = RenderGraphRuntime::Get().Host();
			group.placeColours = host ? org::LatchBlock::Create(fmt::format("{}.places.colours", kGroupIds[index]), group.entries * static_cast<std::uint32_t>(kPlaces) * RunGate::kEntryBytes) : nullptr;
			if (!group.placeGate || !group.placeColours) {
				unregisterAll();
				logger::warn("[ORG] engine skeleton: the {} clears' places have no run gate; those frames' clears keep the {} anchor's ticket", kGroupNames[index],
					FrameAnchors::Name(kGroupAnchors[index]));
				return;
			}
			std::string summary;
			for (std::size_t place = 0; place < kPlaces; ++place) {
				FrameAnchors::ClientDesc client{};
				client.id = PlaceClientId(index, place);
				for (const auto& [target, spec] : group.specs) {
					if (!PlaceClears(static_cast<Place>(place), target))
						continue;
					auto placed = std::make_shared<ClearSpec>(*spec);
					placed->name = fmt::format("{}@{}", spec->name, kPlaceNames[place]);
					placed->gate = group.placeGate;
					placed->colours = group.placeColours;
					for (auto& draw : placed->draws)
						draw.entry += static_cast<std::uint32_t>(place) * group.entries;
					std::shared_ptr<const ClearSpec> pass = placed;
					client.passes.push_back({ .name = placed->name,
						.anchor = Anchor::EngineFrameClear,
						.order = static_cast<std::int32_t>(placed->draws.front().entry),
						.kind = FrameAnchors::PassKind::Render,
						.make = [pass] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ClearPass>(pass)); },
						// The engine's call stages the entry before the commit. A clear not asked for skips and folds its barriers.
						.atExecution = true });
					group.placeSpecs[place].emplace(target, std::move(pass));
				}
				if (client.passes.empty()) {
					if (std::exchange(group.placeRegistered[place], false))
						FrameAnchors::Unregister(client.id);
					continue;
				}
				// The engine's call stages the entry and colour, so the step has no tap.
				client.active = [] { return false; };
				client.segmentOnly = true;
				client.gates = { group.placeGate };
				const auto views = client.passes.size();
				group.placeRegistered[place] = FrameAnchors::Register(std::move(client));
				if (group.placeRegistered[place])
					summary += fmt::format("{}{} {}", summary.empty() ? "" : ", ", kPlaceNames[place], views);
				else
					group.placeSpecs[place].clear();
			}
			if (!summary.empty())
				logger::info("[ORG] engine skeleton: a menu scene's {} clears run in the UI stage (views by place: {})", kGroupNames[index], summary);
		}

		/**
		* @brief Stages a clear into the UI stage place that FrameAnchors::StagePlace names.
		* False when no open stage has a place for it.
		*/
		bool TakeIntoPlace(Group a_group, const Target& a_target, const Pending& a_pending)
		{
			auto& group = GroupOf(a_group);
			const auto index = IndexOf(a_group);
			static Util::ReasonLog s_kept("[ORG] engine skeleton: a clear at no place of the UI stage keeps its group anchor's ticket");
			if (!group.placeGate)
				return !WorldFrame() && s_kept.Report("the group has no places"), false;
			std::array<std::string, kPlaces> ids;
			std::array<std::string_view, kPlaces> views;
			for (std::size_t place = 0; place < kPlaces; ++place) {
				ids[place] = PlaceClientId(index, place);
				views[place] = ids[place];
			}
			const auto place = FrameAnchors::StagePlace(views);
			if (place < 0)
				return !WorldFrame() && s_kept.Report(fmt::format("{}: no open stage has a place for the {} clears", RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind), kGroupNames[index])), false;
			if (!group.placeRegistered[static_cast<std::size_t>(place)])
				return s_kept.Report(fmt::format("the {} place is not registered", kPlaceNames[static_cast<std::size_t>(place)])), false;
			const auto& specs = group.placeSpecs[static_cast<std::size_t>(place)];
			if (!specs.contains(a_target))
				return s_kept.Report(fmt::format("the {} place does not clear {}", kPlaceNames[static_cast<std::size_t>(place)], TargetName(a_target))), false;
			const std::uint32_t entry = static_cast<std::uint32_t>(place) * group.entries + a_pending.gateEntry;
			const bool staged = FrameAnchors::StageInputs(ids[static_cast<std::size_t>(place)], [&] {
				const org::RunRef run = RunGate::Current();
				if (!a_pending.depth)
					group.placeColours->WriteValue(run, entry * RunGate::kEntryBytes, a_pending.colour);
				group.placeGate->OpenDraw(run, entry);
			});
			if (!staged)
				return s_kept.Report(fmt::format("the stage's schedule does not have the {} place's client on", kPlaceNames[static_cast<std::size_t>(place)])), false;
			++group.taken;
			if (group.placed++ == 0)
				logger::info("[ORG] engine skeleton: the first {} clear at a place runs at the UI stage's {} place ({} frame)", kGroupNames[index],
					kPlaceNames[static_cast<std::size_t>(place)], RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind));
			RenderGraph::Frame::Driver::NoteHosted(kPlaceNames[static_cast<std::size_t>(place)], 1);
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

		/** @brief Whether the host's graph holds this registration's passes. */
		bool HostReady(Group a_group, Host a_host)
		{
			if (a_host == Host::Own || StageHost(a_host) || !WorldFrame())
				return false;
			const auto& group = GroupOf(a_group);
			const auto h = HostIndex(a_host);
			if (group.hosted[h].empty())
				return false;
			// DCLF epochs run only once DCLF draws the world. The shadow epoch draws the views only if that frame's coverage decision took them.
			if (kHostInfo[h].dclf && (!RenderGraph::Frame::Driver::Current().dclfDraws || !DCLF::EpochsRun()))
				return false;
			if (a_host == Host::ShadowView && !DCLF::DrawsShadowViews())
				return false;
			// Menu 3D scenes also clear the main depth. Only RenderDepth's clear belongs to the Z-prepass.
			if (a_host == Host::ZPrepass && !DCLF::InDepthPass())
				return false;
			return kHostInfo[h].dclf && group.hostedBuilt[h].load(std::memory_order_acquire) == group.generation;
		}

		/**
		* @brief Registers the group's passes with a new gate and colour latch, so an older recording can never clear the wrong view.
		* Called at Setup and after a resize.
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
			group.entries = entries;
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
					// A run clears only a few views. The others record nothing.
					.atExecution = true,
					// Every clear is an indirect draw through the gate, so native mode records it ahead.
					.gpuGated = true });
				group.specs.emplace(spec->target, std::move(pass));
			}
			const auto passes = client.passes.size();
			group.registered = true;
			logger::info("[ORG] engine skeleton: the {} clears registered at the {} anchor ({} views, {} gate entries, registration {})", kGroupNames[index],
				FrameAnchors::Name(kGroupAnchors[index]), passes, entries, group.generation);
			RegisterHosted(a_group);
			RegisterPlaces(a_group);
			PublishHosted();
		}

		/** @brief Null when the caller must use D3D11 now: no pass, a remade target, or a registration not yet in the built graph. */
		const ClearDraw* DrawFor(Group a_group, const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags)
		{
			auto& group = GroupOf(a_group);
			const auto found = group.specs.find(a_target);
			if (found == group.specs.end()) {
				Refuse(a_target, "not a view the skeleton clears");
				return nullptr;
			}
			const auto& spec = *found->second;
			// Another view in the slot means a lent slot or a remade target, so this clear is not the pass's.
			// Skylighting lends the precipitation occlusion slot for a while. CheckMadeAgain catches remade targets at the next frame start.
			if (spec.view != a_view || spec.texture != TextureOf(a_target)) {
				if (group.lent++ == 0)
					logger::info("[ORG] engine skeleton: a clear of {} found another view in its slot ({}); it stays on D3D11 while the slot holds it",
						TargetName(a_target), spec.texture != TextureOf(a_target) ? "another texture" : "the same texture, another view");
				return nullptr;
			}
			for (const auto& draw : spec.draws)
				if (draw.flags == a_flags)
					return &draw;
			Refuse(a_target, "its clear flags have no pass");
			return nullptr;
		}

		/** @brief Native mode: a clear with no place is a mismatch and draws nothing. Returns true, the engine's call is taken. */
		bool NoPlace(const Target& a_target, std::uint32_t a_flags, std::string_view a_why)
		{
			const auto& frame = RenderGraph::Frame::Driver::Current();
			RenderGraph::FrameSequence::NoteUnplaced(fmt::format("skeleton clear of {}/{}", TargetName(a_target), a_flags));
			RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
				fmt::format("a clear of {} (flags {}) in a {} frame{}: {}", TargetName(a_target), a_flags, RenderGraph::Frame::KindName(frame.kind),
					frame.capture ? " (capture)" : "", a_why));
			return true;
		}

		bool Take(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, Pending a_pending)
		{
			const Group group = GroupFor(a_target);
			auto& state = GroupOf(group);
			// In world frames the face sky clears the faces. Other frame kinds never read them, and the next world frame's sky clears them.
			if (group == Group::Cube && FacesClearedBySky()) {
				if (state.dropped++ == 0)
					logger::info("[ORG] engine skeleton: reflection face clears are dropped; the start stage's face sky clears each face and its depth");
				return true;
			}
			if (state.failed || state.stale || !state.registered)
				return false;
			const auto* draw = DrawFor(group, a_target, a_view, a_flags);
			if (!draw)
				return false;
			a_pending.gateEntry = draw->entry;
			a_pending.target = a_target;
			// Bink frames and the session's first frame clear the main depth outside every stage, but nothing reads it there.
			// Every other frame kind clears it before its first read, so drop it.
			if (a_target.kind == Kind::Depth && a_target.index == DepthTarget::kMAIN && PlacesOn() &&
				(RenderGraph::Frame::Driver::Current().kind == RenderGraph::Frame::Kind::Bink || RenderGraph::Frame::Driver::Current().frame == 0)) {
				if (state.dropped++ == 0)
					logger::info("[ORG] engine skeleton: a Bink frame's (or frame 0's) clear of the main depth is dropped (no reader before the next kind's own clear)");
				return true;
			}
			// The UI stage opens after every world stage ends, so a world stage's clear never finds a place.
			if (TakeIntoPlace(group, a_target, a_pending))
				return true;
			const auto host = HostFor(a_target, a_flags);
			if (HostReady(group, host)) {
				a_pending.host = host;
				state.hostedPending.push_back(a_pending);
				return true;
			}
			const auto unplaced = [&](std::string_view a_why) { return NoPlace(a_target, a_flags, a_why); };
			if (!WorldFrame())
				return unplaced("no open stage has a place for it");
			const auto place = StagePlaceFor(host);
			if (place == Host::Own || GroupOf(group).hosted[HostIndex(place)].empty())
				return unplaced("no stage of the world recipe clears it");
			const auto id = HostedClientId(IndexOf(group), place);
			const auto hold = FrameAnchors::HeldNow(id, kHostInfo[HostIndex(place)].anchor);
			if (!hold.held)
				return unplaced(fmt::format("the {} stage place is not open", kHostInfo[HostIndex(place)].name));
			if (!hold.scheduled) {
				// The stage's schedule does not hold the place's client yet, so the clear lags.
				static Util::ReasonLog s_lag("[ORG] engine skeleton: a clear lags (its stage's schedule does not hold the place yet); nothing clears it this frame");
				s_lag.Report(fmt::format("{} at the {} place", TargetName(a_target), kHostInfo[HostIndex(place)].name));
				return true;
			}
			const auto* spec = &state;
			const bool staged = FrameAnchors::StageInputs(id, [&] {
				const org::RunRef run = RunGate::Current();
				if (!a_pending.depth)
					spec->colours->WriteValue(run, a_pending.gateEntry * RunGate::kEntryBytes, a_pending.colour);
				spec->gate->OpenDraw(run, a_pending.gateEntry);
			});
			if (!staged)
				return NoPlace(a_target, a_flags, fmt::format("the {} stage place did not take it", kHostInfo[HostIndex(place)].name));
			++state.taken;
			RenderGraph::Frame::Driver::NoteHosted(kHostInfo[HostIndex(place)].name, 1);
			return true;
		}

		/** @brief Compared to detect a remade target. */
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

		/** @brief kMAIN's copy texture is the published "Engine::kMAIN Copy" while ORG owns kMAIN, else a D3D11 import. */
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
		void RegisterUiCopy(CopyKind a_kind);

		/**
		* @brief Registers the copy's client. Called at Setup and after a resize.
		* No run gate: a transfer copy has no indirect form, and this anchor only runs from RunCopy, which always wants the copy.
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
			// Main::RenderDepth always copies, so the depth copy is ungated in the pre-main stage and on its own anchor.
			const bool declared = a_kind == CopyKind::Depth;
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
			client.commit = [a_kind](const FrameAnchors::Point&) {
				if (const auto& gate = CopyOf(a_kind).gate)
					gate->OpenDispatch(0, 1);
			};
			client.active = [a_kind] { return CopyOf(a_kind).wanted; };
			client.executed = [a_kind](const FrameAnchors::Point&) { CopyOf(a_kind).executed = true; };
			copy.registered = FrameAnchors::Register(std::move(client));
			if (!copy.registered) {
				copy.failed = true;
				logger::warn("[ORG] engine skeleton: the {} anchor refused {}; it stays on D3D11", FrameAnchors::Name(kCopyAnchors[index]), kCopyNames[index]);
				return;
			}
			RegisterHostedCopy(a_kind);
			RegisterUiCopy(a_kind);
		}

		/** @brief Native mode with hosting. The engine's call opens its entry. A frame with no menu scene skips it. */
		void RegisterUiCopy(CopyKind a_kind)
		{
			auto& copy = CopyOf(a_kind);
			if (a_kind != CopyKind::Depth)
				return;
			if (!copy.uiGate)
				copy.uiGate = RunGate::Create(kUiDepthCopyId, 1);
			if (!copy.uiGate) {
				logger::error("[ORG] engine skeleton: {}: no run gate for the UI stage's copy", kCopyNames[static_cast<std::size_t>(a_kind)]);
				return;
			}
			AnchorPasses::Builder builder(Anchor::EngineDepthCopy, kUiDepthCopyId, 0, {}, copy.uiGate);
			builder.Add(AnchorPasses::Copy{
				.name = "post-zprepass@ui",
				.source = copy.source,
				.target = copy.target,
				.width = copy.source.Width(),
				.height = copy.source.Height(),
				.gate = 0u });
			FrameAnchors::ClientDesc client{};
			client.id = kUiDepthCopyId;
			client.passes = builder.Take();
			client.gates = { copy.uiGate };
			// The engine's call stages the entry, so the step has no tap.
			client.active = [] { return false; };
			client.segmentOnly = true;
			copy.uiRegistered = FrameAnchors::Register(std::move(client));
			if (!copy.uiRegistered)
				logger::error("[ORG] engine skeleton: the UI stage's {} was refused", kCopyNames[static_cast<std::size_t>(a_kind)]);
		}

		/** @brief CS's own depth copy runs first in the DeferredPasses anchor. */
		void RegisterHostedCopy(CopyKind a_kind)
		{
			auto& copy = CopyOf(a_kind);
			const auto index = static_cast<std::size_t>(a_kind);
			// The kMAIN copy keeps its own ticket. Water draws follow it outdoors and the ImageSpace chain indoors, and one host cannot follow both.
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
			// Ungated, since Deferred::DeferredPasses copies in every world frame.
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
			// Without the stages, the copy runs on its own anchor.
			client.segmentOnly = true;
			copy.hostedRegistered = FrameAnchors::Register(std::move(client));
			copy.host = copy.hostedRegistered ? host : Host::Own;
			if (copy.hostedRegistered)
				logger::info("[ORG] engine skeleton: in a world frame {} runs first at the {} anchor", kCopyNames[index], FrameAnchors::Name(info.anchor));
		}

		/** @brief Whether the open stage declares this depth copy. The engine's call then does nothing more. */
		bool CopyDeclared(CopyKind a_kind, bool a_hostable, const char*& a_where)
		{
			auto& copy = CopyOf(a_kind);
			if (a_kind != CopyKind::Depth)
				return false;
			// The UI stage opens after every world stage ends, so the pre-main copy never finds it open.
			if (copy.uiRegistered && FrameAnchors::StageInputs(kUiDepthCopyId, [&] { copy.uiGate->OpenDispatch(RunGate::Current(), 0, 1); })) {
				a_where = "UI stage";
				return true;
			}
			if (!WorldFrame()) {
				static Util::ReasonLog s_notDeclared("[ORG] engine skeleton: a depth copy outside a world frame is not the UI stage's pass");
				const auto reason = copy.uiRegistered ? fmt::format("{}: the UI stage is not open, or its schedule does not have the copy on",
															RenderGraph::Frame::KindName(RenderGraph::Frame::Driver::Current().kind)) :
				                                        std::string("the UI stage's copy is not registered");
				s_notDeclared.Report(reason);
				return false;
			}
			a_where = a_hostable ? "deferred stage" : "pre-main stage";
			if (a_hostable)
				return copy.host != Host::Own && copy.hostedRegistered &&
				       FrameAnchors::CanTap(fmt::format("{}@{}", kCopyIds[static_cast<std::size_t>(a_kind)], kHostInfo[HostIndex(copy.host)].name));
			return copy.registered && FrameAnchors::CanTap(kCopyIds[static_cast<std::size_t>(a_kind)]);
		}

		/** @brief False when the skeleton does not take the copy and the caller copies. a_hostable makes a world frame's copy wait for its host. */
		bool RunCopy(CopyKind a_kind, bool a_hostable = false)
		{
			const auto index = static_cast<std::size_t>(a_kind);
			auto& copy = CopyOf(a_kind);
			if (copy.failed || !Usable())
				return false;
			// RenderDepth's copy is its last act, so the pre-depth stage commits here at the latest.
			if (a_kind == CopyKind::Depth && !a_hostable && WorldFrame())
				(void)FrameAnchors::TapPoint(RenderGraph::Frame::SegmentId::PreDepth);
			// CS's depth copy is the deferred+water stage's first pass, so the pre-main stage ends here at the latest.
			if (a_kind == CopyKind::Depth && a_hostable && WorldFrame())
				(void)FrameAnchors::TapPoint(RenderGraph::Frame::SegmentId::PreMain);
			const char* where = nullptr;
			if (EngineTexturesOf(a_kind) == std::pair{ copy.sourceTexture, copy.targetTexture } && CopyDeclared(a_kind, a_hostable, where)) {
				RenderGraph::Frame::Driver::NoteHosted(where, 1);
				return true;
			}
			// A texture made again inside the frame: D3D11 copies until the next Begin registers the copy again.
			if (EngineTexturesOf(a_kind) != std::pair{ copy.sourceTexture, copy.targetTexture })
				return false;
			if (!copy.registered)
				return false;
			using RenderGraph::Frame::SegmentId;
			const auto start = static_cast<std::uint32_t>(SegmentId::Start);
			// The reflection cubemap's render copies the main depth while the start stage is open.
			// The pre-main copy overwrites kPOST_ZPREPASS_COPY before anyone reads it, so drop this one.
			if (a_kind == CopyKind::Depth && WorldFrame() && (FrameAnchors::StageOpen(start) || FrameAnchors::StageAbsent(start))) {
				if (copy.dropped++ == 0)
					logger::info("[ORG] engine skeleton: {} while the start stage is open (the cubemap's render) is dropped: pre-main's copy writes it again before any reader",
						kCopyNames[index]);
				return true;
			}
			// The copy's anchor has no epoch of its own, so every copy is a stage's pass or dropped.
			const auto& frame = RenderGraph::Frame::Driver::Current();
			if (a_kind == CopyKind::Depth) {
				// The holding stage has no schedule yet, or a build with this registration is pending, so the copy lags.
				auto holder = static_cast<std::uint32_t>(SegmentId::Ui);
				std::string holderId(kUiDepthCopyId);
				if (WorldFrame()) {
					holder = static_cast<std::uint32_t>(a_hostable ? SegmentId::Deferred : SegmentId::PreMain);
					holderId = a_hostable ? fmt::format("{}@{}", kCopyIds[index], kHostInfo[HostIndex(Host::DeferredPasses)].name) : std::string(kCopyIds[index]);
				}
				if (FrameAnchors::StageAbsent(holder) || FrameAnchors::TapMayLag(holderId)) {
					static Util::ReasonLog s_lag("[ORG] engine skeleton: a copy lags (its stage has no schedule yet); nothing copies this frame");
					s_lag.Report(kCopyNames[index]);
					return true;
				}
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
					fmt::format("{} in a {} frame came while no open stage holds it (open: start {}, pre-depth {}, pre-main {}, deferred+water {}; hostable {})",
						kCopyNames[index], RenderGraph::Frame::KindName(frame.kind), FrameAnchors::StageOpen(start),
						FrameAnchors::StageOpen(static_cast<std::uint32_t>(SegmentId::PreDepth)), FrameAnchors::StageOpen(static_cast<std::uint32_t>(SegmentId::PreMain)),
						FrameAnchors::StageOpen(static_cast<std::uint32_t>(SegmentId::Deferred)), a_hostable));
				return true;
			}
			const auto hold = FrameAnchors::HeldNow(kCopyIds[index], kCopyAnchors[index]);
			if (!hold.held) {
				RenderGraph::FrameSequence::NoteUnplaced(fmt::format("skeleton copy {}", kCopySteps[index]));
				RenderGraph::Frame::Driver::NoteTapError(RenderGraph::Frame::Driver::TapError::NotDeclared, RenderGraph::Frame::SegmentId::Count,
					fmt::format("{} in a {} frame came while no open stage holds it", kCopyNames[index], RenderGraph::Frame::KindName(frame.kind)));
				return true;
			}
			// A newer registration is not in the graph until the stage's next build switches in, so it lags too.
			if (!hold.scheduled || !FrameAnchors::CanTap(kCopyIds[index])) {
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

		/** @brief Runs the clears taken so far. A run that does not draw them is an error. */
		void FlushGroup(std::size_t a_index)
		{
			auto& state = Skel();
			auto& group = state.groups[a_index];
			if (group.stale && !group.failed)
				Build(static_cast<Group>(a_index));
		}

		/**
		* @brief Inside the frame's adoption point. Copies whose textures were made again register again here, never at a copy.
		* Until the stage builds with them switch in, the copies lag.
		*/
		void RegisterRemadeCopies()
		{
			if (!globals::game::renderer)
				return;
			for (std::size_t i = 0; i < kCopies; ++i) {
				const auto kind = static_cast<CopyKind>(i);
				const auto& copy = CopyOf(kind);
				if (copy.failed || EngineTexturesOf(kind) == std::pair{ copy.sourceTexture, copy.targetTexture })
					continue;
				logger::info("[ORG] engine skeleton: {}: a texture was made again; it registers again", kCopyNames[i]);
				RegisterCopy(kind);
			}
		}

		/** @brief Checked at frame start rather than at a clear, because a slot can be lent within a frame. */
		void CheckMadeAgain()
		{
			for (std::size_t g = 0; g < kGroups; ++g) {
				auto& group = Skel().groups[g];
				if (group.failed || group.stale || !group.registered)
					continue;
				for (const auto& [target, spec] : group.specs) {
					if (spec->view == ViewOf(target) && spec->texture == TextureOf(target))
						continue;
					group.stale = true;
					logger::info("[ORG] engine skeleton: {} was made again ({}); the {} clears register again", TargetName(target),
						spec->texture != TextureOf(target) ? "a new texture" : "the same texture, another view", kGroupNames[g]);
					break;
				}
			}
		}

		/** @brief A native immediate-context clear outside the patched sites, run on the graph now. False lets the context record it. */
		bool TakeNow(const Target& a_target, ID3D11View* a_view, std::uint32_t a_flags, const Pending& a_pending)
		{
			if (FrameAnchors::InRun() || !Take(a_target, a_view, a_flags, a_pending))
				return false;
			FlushGroup(IndexOf(GroupFor(a_target)));
			return true;
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
		if (state.setUp && FrameAnchors::IsRenderOwner()) {
			CheckMadeAgain();
			// Inside the frame's adoption point, so a remade target registers again here and never at a clear or copy.
			for (std::size_t g = 0; g < kGroups; ++g)
				if (state.groups[g].stale && !state.groups[g].failed)
					Build(static_cast<Group>(g));
			RegisterRemadeCopies();
		}
		if (!state.setUp || !FrameAnchors::IsRenderOwner())
			return;
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

	void Reserve()
	{
		if (!Requested())
			return;
		bool reserved = true;
		for (const auto anchor : { Anchor::EngineShadowClear, Anchor::EngineFrameClear, Anchor::EngineDepthCopy })
			reserved = FrameAnchors::Reserve(anchor) && reserved;
		// Where the start stage holds the face sky, that sky clears the faces, so the cube group needs no anchor.
		if (!FrameAnchors::StageStepsAt(Anchor::SkyReflections))
			reserved = FrameAnchors::Reserve(Anchor::EngineCubeClear) && reserved;
		if (FrameAnchors::StageStepsAt(Anchor::EngineCopy))
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
		// Everything registers here, so a clear never registers anything during play.
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
			copy.failed = !FrameAnchors::HasEpoch(kCopyAnchors[i]);
			if (!copy.failed)
				RegisterCopy(kind);
		}
		state.setUp = true;
		// Native mode answers its own copies before the hook is asked.
		if (state.copies[static_cast<std::size_t>(CopyKind::Main)].registered)
			Tripwire::SetGuestCopyHook(&Sites::TakeContextCopy);
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
			Pending pending{ .depth = false };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return Take(*target, a_view, 0, pending);
		}

		bool TakeDepthClear(ID3D11DeviceContext*, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			// The clear programs only write depth 1 and stencil 0, the only values the engine uses.
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
				return false;  // the D3D11 call does nothing either
			return Take(*target, a_view, flags, Pending{ .depth = true, .flags = a_flags });
		}

		void Flush()
		{
			auto& state = Skel();
			// Only the render owner takes clears.
			if (!state.setUp || state.flushing || !FrameAnchors::IsRenderOwner())
				return;
			for (std::size_t i = 0; i < kGroups; ++i)
				FlushGroup(i);
		}

		bool TakeDepthCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !Requested())
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
			if (!target)
				return false;
			Pending pending{ .depth = false };
			std::memcpy(pending.colour.data(), a_colour, sizeof(pending.colour));
			return TakeNow(*target, a_view, 0, pending);
		}

		bool TakeContextDepthClear(ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
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
			return TakeNow(*target, a_view, flags, Pending{ .depth = true, .flags = a_flags });
		}

		bool TakeContextCopy(ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			if (!Requested() || !globals::game::renderer || !a_destination || FrameAnchors::InRun())
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
		void Flush() {}
		bool TakeDepthCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
		bool TakeContextColourClear(ID3D11RenderTargetView*, const FLOAT[4]) { return false; }
		bool TakeContextDepthClear(ID3D11DepthStencilView*, UINT, FLOAT, UINT8) { return false; }
		bool TakeContextCopy(ID3D11Resource*, ID3D11Resource*) { return false; }
	}
}

#endif
