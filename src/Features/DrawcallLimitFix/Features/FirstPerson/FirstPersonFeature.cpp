// FirstPerson feature registration: switches, toggle, the view's contribution, hooks and report lines.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

#include <string>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		Api::ViewContribution Contribution(Api::ToggleId a_toggle, bool a_forwardDecals)
		{
			return { .view = Api::EngineView::FirstPerson,
				.toggle = a_toggle,
				.forwardDecals = a_forwardDecals,
				.phase = kSetFirstPerson,
				.membership = Api::ViewMembership::CategoryRoot,
				.keys = { .forwardRasterToForwardPart = true, .keyFlags = kForwardKeyFirstPerson, .cullBackFaces = true },
				.shape = { .faces = 1,
					.forwardPart = true,
					.depthPass = true,
					.viewBits = kViewFirstPerson,
					.name = "first-person",
					.prefix = "cs.dclf.first-person",
					.blocks = "block",
					.target = "depth-copy",
					.labels = { "first-person bucket map", "first-person bucket table", "first-person culling latch", "first-person per-frame data",
						"first-person tree rows" },
					.depthRange = 0.1f,
					// The frustum test dropped the weapon at some angles because of its skinned bound.
					// The view holds a handful of objects that are always in front of the camera, so it skips the test.
					.frustumCull = false },
				.life = Api::ViewShapeLife::DropWhenOff,
				.area = Api::ViewArea::MainColourViewport,
				.withholding = Api::ViewWithholding::InMainRenderers,
				.targetImport = Api::ViewTargetImport::DepthTarget,
				.coveredInitially = false,
				.coveredOnFailedGraph = false,
				.covered = &Covered,
				.captureRoot = &CaptureRoot,
				.accumulators = &Accumulators };
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto firstPersonSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::UnlessOne,
				.description = "default on: the player's first-person arms, weapons and magic hands drawn by DCLF as a view of the one scene (T7e: kSetFirstPerson members, the view bit kViewFirstPerson at the GPU cull, its depth in the Z-prepass epoch and a colour epoch at Main::RenderFirstPersonView); 0 they stay native (live toggle's seed)",
				.before = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto decalsSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_FIRST_PERSON_DECALS", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Zero,
				.description = "default on (T7f item 7): a decal on a first-person entry (an effect's skinned blood decal on the arms, a weapon's BloodLighting) drawn by the first-person forward part as a forward object with its decal group's depth bias, test and blend (needs CS_DCLF_FIRST_PERSON and CS_DCLF_DECALS); 0 it stays the engine's",
				.before = "CS_DCLF_OPAQUE_EFFECTS" });
			const auto firstPerson = a_context.RegisterToggle({ .name = "first person", .label = "Draw the first-person arms and weapons",
				.tooltip = "T7e: DCLF draws the first-person skeleton's geometry (arms, weapons, shield, magic hands) as a view of its scene, with the first-person camera, in the engine's depth and first-person passes. Off: the engine draws them.",
				.section = "First person", .seed = firstPersonSwitch, .entersClassification = true, .after = "shadows", .needs = { "skinned", "actors" } });
			// The decals switch is read once, here.
			a_context.RegisterView(Contribution(firstPerson, SwitchValue(decalsSwitch) != "0"));
			// Must run before the coverage, which decides whether the members are withheld.
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::FrameStartBeforeCoverage, .run = &BeginFrame });
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::DepthCommitted, .run = &ExecuteDepth });
			// Listed whatever the toggle says.
			a_context.RegisterCullHiddenSource({ .bit = Api::CullHiddenBit::Hidden, .nodes = &CullHiddenSkeleton });
			a_context.RegisterReportLines({ .name = "first person", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			if (Diagnostics::Observe().claims)
				SubscribeSkeletonParity();
			SubscribeRootViewChecks();
			a_context.RegisterReportLines({ .name = "first person filing", .point = Api::ReportPoint::SceneCoordinatorSet, .lines = &SetLines });
			InstallHooks(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "first person", .install = &Install, .preflight = &PreflightHooks };
	}
}
