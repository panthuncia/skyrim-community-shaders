// The frame's schedule: the engine points DCLF's epochs are submitted at, every epoch DCLF adds passes to, and its passes in order.
//
// A trigger is the engine point an epoch is submitted at. Its owner's hook marks it active while its DCLF work runs.
// An epoch is a RenderGraphRuntime::Segment plus the shape its passes are made from. ORG orders epochs, and passes run in list order.
// A hosted epoch has a shape and passes but no epoch of its own. Its keys sit in the host's list as "<id>/<key>" and the host commits it.
// A pass key's prefix names its owner, such as core.*, view.*, or a feature's prefix. A pass's own conditions decide whether it is added.
// The diag.* anchors only exist for diagnostics. A key nobody registered fails the install.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Schedule/Schedule.h"

#include "RenderGraph/RenderGraphRuntime.h"

namespace DCLF::Schedule
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		constexpr std::uint32_t SegmentOf(Segment a_segment) { return static_cast<std::uint32_t>(a_segment); }
		constexpr std::uint32_t ViewOf(Api::EngineView a_view) { return static_cast<std::uint32_t>(a_view); }
		constexpr std::uint32_t MainOf(Api::MainShape a_shape) { return static_cast<std::uint32_t>(a_shape); }

		// The engine points the epochs are submitted at, in the order a frame reaches them.
		constexpr Api::TriggerDef kTriggers[] = {
			{ .id = "shadow-maps.after", .owner = "Features/Shadows/Engine", .where = "Main::RenderShadowMaps returned, the shadow views drawn and the occlusion maps set up before it (Deferred's Main_RenderShadowMaps hook)" },
			{ .id = "depth.committed", .owner = "Core", .where = "Main::RenderDepth's world draw done (the shell's Main_RenderDepth_WorldDrawn hook), the views' work there with it" },
			{ .id = "opaque.after", .owner = "Features/MainView", .where = "the main pass's opaque batches done (Deferred's Main_RenderWorld_BlendedDecals hook)" },
			{ .id = "water.mask-batch", .owner = "Features/Water", .where = "the main pass's water mask batch, before the engine runs it (WaterCapture.cpp's WaterBatch, FinishAccumulating)" },
			{ .id = "water.surface-batch", .owner = "Features/Water", .where = "the main pass's water batch, before the engine runs it (WaterCapture.cpp's WaterBatch, FinishAccumulatingPostResolveDepth)" },
			{ .id = "blended.draw", .owner = "Features/Forward", .where = "the main pass's blended draw for the main accumulator (both its call sites, ForwardHooks.cpp)" },
			{ .id = "fp.colour-render", .owner = "Features/FirstPerson", .where = "Main::RenderFirstPersonView's accumulator render (FirstPersonHooks.cpp)" },
		};

		// The depth epoch. Ordering constraints that are not obvious from the list:
		// - Portal queries run on the graphics queue, because phase 1's build reads their results at once.
		// - The next frame's state runs on the compute queue beside phase 1's depth draw, since nothing of this frame reads it.
		// - The reflection faces' draws sit between first person's cull and its depth draw, which they keep apart.
		// - First person's depth goes into the main depth after the HZB, so the HZB leaves the arms out.
		//   The epoch is submitted before the engine copies kPOST_ZPREPASS_COPY, which the first-person colour epoch tests against.
		// - The visibility events and the fade write-back go last, because ORG starts a graphics batch at the first pass that needs the compute queue.
		constexpr std::string_view kMainDepth[] = {
			"core.z.latched-copies",
			"texture-transforms.apply",
			"fades.pixel-rows",
			"core.z.portal-queries",
			"portals.member-evaluate",
			"core.z.build-draws",
			"core.z.index-pool",
			"tree-lod.cull",
			"grass.cull",
			"grass.args",
			"tree-wind.next-frame",
			"fades.portal-queries",
			"fades.portal-evaluate",
			"fades.state",
			"main-view.z.depth",
			"reflection/view.build-draws",
			"reflection/tree-lod.faces-cull",
			"gpu-occlusion.hzb",
			"core.build-draws-phase2",
			"tree-lod.cull-phase2",
			"grass.cull-phase2",
			"grass.args-phase2",
			"main-view.depth-phase2",
			"grass.args-colour",
			"fp.depth/view.build-draws",
			"fp.depth/tree-lod.faces-cull",
			"reflection/view.faces",
			"fp.depth/view.depth",
			"tree-lod.counts",
			"grass.counts",
			"core.z.visibility-events",
			"core.z.visibility-readback",
			"fades.events",
			"diag.after-depth",
			"core.z.direct",
		};
		// The colour epoch: the build, the sort by pipeline, the decal buckets' sort and the colour draws.
		constexpr std::string_view kMainColour[] = {
			"core.latched-copies",
			"core.build-draws",
			"core.sort-scan",
			"core.sort-scatter",
			"core.forward-sort",
			"diag.before-colour",
			"main-view.colour",
			"diag.after-colour",
			"core.direct",
		};
		// The water epochs: the deferred ranges the colour segment culled. Range 0 is the water mask, range 1 the water surface.
		constexpr std::string_view kWaterMask[] = {
			"core.deferred-range0",
			"core.water-mask-direct",
		};
		constexpr std::string_view kWaterSurface[] = {
			"core.deferred-range1",
			"core.water-surface-direct",
		};
		// The forward epoch: the CPU pipeline's forward site sorts and draws the blended draws and the refraction normals, in either pipeline.
		constexpr std::string_view kForward[] = {
			"core.forward-direct",
		};
		// The shadow epoch: every shadow view's and occlusion map's latched copies, culling and draws in one dispatch.
		// The maps are drawn here, not at the engine's occlusion point after RenderDepth, because Skylighting sets them up before the shadow maps
		// and nothing reads a map before the Prepass. That saves an epoch per exterior frame.
		constexpr std::string_view kShadow[] = {
			"shadows.latched-copies",
			"shadows.index-pool",
			"shadows.caster-rows",
			"shadows.build-draws",
			"shadows.view",
			"shadows.cull-counters",
			"core.shadow-direct",
		};
		// First person's colour epoch: the colour draws, then the CPU pipeline's site with the forward range. Its depth work lives in the depth epoch.
		constexpr std::string_view kFirstPersonColour[] = {
			"view.colour",
			"view.direct",
		};

		// In revision slot order, where an epoch's index is its row. The frame runs them in kFrameOrder.
		// The main epochs decide the frame's claims, and the forward epoch too while its class is on, because only it draws its members.
		constexpr Api::EpochDef kEpochs[] = {
			{ .id = "main.depth", .label = "Z-prepass", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Depth), .claims = true, .passes = kMainDepth },
			{ .id = "main.colour", .label = "colour", .trigger = "opaque.after", .segment = SegmentOf(Segment::MainOpaque), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .claims = true, .passes = kMainColour },
			{ .id = "shadow", .label = "shadow", .trigger = "shadow-maps.after", .segment = SegmentOf(Segment::ShadowView), .shape = Api::EpochShape::Shadow, .shapeIndex = 0, .passes = kShadow },
			// The reflection's faces, hosted by the depth epoch. That saves an epoch per frame with its barriers and latched copies.
			{ .id = "reflection", .label = "reflection", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::Reflection), .hostedBy = "main.depth" },
			{ .id = "forward", .label = "forward", .trigger = "blended.draw", .segment = SegmentOf(Segment::Forward), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .sharesWith = "main.colour", .claims = true, .passes = kForward },
			// First person's depth, hosted by the depth epoch and committed there. The first-person camera is set in Main::Draw before RenderDepth.
			// That saves an epoch per first-person frame. Its colour epoch keeps sharing its shape.
			{ .id = "fp.depth", .label = "first-person depth", .trigger = "depth.committed", .segment = SegmentOf(Segment::ZPrepass), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .hostedBy = "main.depth" },
			{ .id = "fp.colour", .label = "first-person colour", .trigger = "fp.colour-render", .segment = SegmentOf(Segment::FirstPerson), .shape = Api::EpochShape::View, .shapeIndex = ViewOf(Api::EngineView::FirstPerson), .sharesWith = "fp.depth", .passes = kFirstPersonColour },
			// The water's mask and surface. They draw what the colour commit culled, and only they draw their members, so they decide
			// the frame's claims too. Without water their passes do nothing.
			{ .id = "water.mask", .label = "water mask", .trigger = "water.mask-batch", .segment = SegmentOf(Segment::WaterMask), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .sharesWith = "main.colour", .claims = true, .passes = kWaterMask },
			{ .id = "water.surface", .label = "water surface", .trigger = "water.surface-batch", .segment = SegmentOf(Segment::WaterSurface), .shape = Api::EpochShape::Main, .shapeIndex = MainOf(Api::MainShape::Colour), .sharesWith = "main.colour", .claims = true, .passes = kWaterSurface },
		};

		// The order a frame runs them, and the host's async ticket loop's: shadows at AfterShadowMaps, the Z-prepass at the depth pass,
		// colour where the opaque batches end, the water mask and surface at their batches, forward at the blended draw after the
		// composite, then first person's colour. Under water the blended draw comes before the water surface; the order stays this one.
		// Hosted epochs are not in it.
		constexpr std::string_view kFrameOrder[] = {
			"shadow",
			"main.depth",
			"main.colour",
			"water.mask",
			"water.surface",
			"forward",
			"fp.colour",
		};
	}

	const Api::FrameSchedule& Frame()
	{
		static constexpr Api::FrameSchedule kFrame{ kTriggers, kEpochs, kFrameOrder };
		return kFrame;
	}
}
