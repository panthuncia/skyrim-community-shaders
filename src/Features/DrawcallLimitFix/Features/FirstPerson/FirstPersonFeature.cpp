// FirstPerson feature registration: the view's contribution, hooks and report lines.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPerson.h"

#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"

#include <string>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		Api::ViewContribution Contribution()
		{
			return { .view = Api::EngineView::FirstPerson,
				.forwardDecals = true,
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
				.life = Api::ViewShapeLife::DropWhenNone,
				.area = Api::ViewArea::MainColourViewport,
				.withholding = Api::ViewWithholding::InMainRenderers,
				.targetImport = Api::ViewTargetImport::DepthTarget,
				.coveredInitially = false,
				.covered = &Covered,
				.captureRoot = &CaptureRoot,
				.accumulators = &Accumulators };
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterView(Contribution());
			// The game half reads the engine; the render half's start must run before the coverage, which decides whether the members are withheld.
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::FrameStartGame, .run = &BeginGameFrame });
			RegisterTicketPart(TicketPart::FirstPerson, &TakeTicketPart);
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::FrameStartBeforeCoverage, .run = &BeginFrame });
			a_context.RegisterViewWork({ .view = Api::EngineView::FirstPerson, .point = Api::ViewPoint::DepthCommitted, .run = &ExecuteDepth });
			a_context.RegisterCullHiddenSource({ .bit = Api::CullHiddenBit::Hidden, .nodes = &CullHiddenSkeleton });
			a_context.RegisterReportLines({ .name = "first person", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
			SubscribeRootViewChecks();
			InstallHooks(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { .name = "first person", .install = &Install, .preflight = &PreflightHooks };
	}
}
