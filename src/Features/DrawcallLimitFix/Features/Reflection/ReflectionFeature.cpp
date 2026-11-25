// Reflection: DCLF draws the water reflection's cube map faces as a view of the one scene.

// ReflectionView.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionView.h"
#include "Features/DrawcallLimitFix/Features/Reflection/ReflectionFaces.h"

#include "Features/DrawcallLimitFix/Core/Engine/RenderThreadBudget.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include <string>

namespace DCLF::Features::Reflection
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			a_text += Report();
		}

		/**
		 * @brief The view's contribution: the main set's LOD techniques, six faces with tree LOD, the engine's cube with DCLF's own depth.
		 * Withheld in its own renderers, the cube camera's accumulators.
		 */
		Api::ViewContribution Contribution()
		{
			return { .view = kView,
				.phase = kSetReflection,
				.membership = Api::ViewMembership::LodTechniques,
				.keys = { .lodTechniquesOnly = true },
				.shape = { .faces = kFaces,
					.trees = true,
					.viewBits = kViewReflection,
					.name = "reflection",
					.prefix = "cs.dclf.reflection",
					.blocks = "face-blocks",
					.target = "cube",
					.labels = { "reflection bucket map", "reflection bucket tables", "reflection culling latches", "reflection face per-frame data",
						"reflection tree rows" } },
				.life = Api::ViewShapeLife::KeepLast,
				.area = Api::ViewArea::Target,
				.withholding = Api::ViewWithholding::OwnRenderers,
				.targetImport = Api::ViewTargetImport::ColourWithOwnDepth,
				.coveredInitially = true,
				.covered = &Covered };
		}

		/** @brief Installs the water reflection's hooks first of the early hooks, before Core's observers. */
		void InstallHooks()
		{
			ReflectionFaces::Install();
			ReflectionFaces::SetFaceOriented([](const ShadowCameras::Camera& a_camera, std::uint32_t a_face) {
				RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ReflectionFace);
				FaceOriented(a_camera, a_face);
			});
			ReflectionFaces::SetRootsOwned([](bool a_plain) {
				RenderThreadBudget::Hook budget(RenderThreadBudget::Site::ReflectionFace);
				return IndirectDraws::Get().ViewLodRootsOwned(kView, a_plain);
			});
		}

		void RegisterSwitch(Api::InstallContext& a_context)
		{
			const auto id = a_context.RegisterSwitch({ .name = "CS_DCLF_REFLECTION_NATIVE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
				.description = "0: the engine renders the water reflection's cube faces; 1: as 0, and each face's camera and sky passes are also computed "
							   "without it and compared with the engine's (differences logged once each); 2 (default): a plain face whose LOD roots DCLF "
							   "draws is drawn without the engine's face render, its sky computed from the Sky object list "
							   "(perf-reflections.md)" });
			const auto& value = SwitchValue(id);
			const auto mode = value == "0" ? ReflectionFaces::Mode::Engine : value == "1" ? ReflectionFaces::Mode::Observe : ReflectionFaces::Mode::Native;
			ReflectionFaces::SetMode(mode);
			if (mode != ReflectionFaces::Mode::Native)
				logger::info("[DCLF] reflection native: CS_DCLF_REFLECTION_NATIVE={}", value);
		}

		void Install(Api::InstallContext& a_context)
		{
			RegisterSwitch(a_context);
			// DCLF draws the LOD the water's cube map reflects from its own tables on the GPU; the engine draws the sky.
			a_context.RegisterView(Contribution());
			a_context.RegisterViewWork({ .view = kView, .point = Api::ViewPoint::FrameStartAfterTreeLod, .run = &Prepare });
			a_context.RegisterViewWork({ .view = kView, .point = Api::ViewPoint::DepthCommitted, .run = &Execute });
			// The engine's update hides these.
			a_context.RegisterCullHiddenSource({ .bit = Api::CullHiddenBit::AsNow, .nodes = &CullHiddenByUpdate });
			a_context.RegisterReportLines({ .name = "reflection", .point = Api::ReportPoint::AsyncWorkViews, .text = &Lines });
		}
	}

	Api::Feature Feature()
	{
		return { .name = "reflection", .install = &Install, .earlyHooks = &InstallHooks, .preflight = &ReflectionFaces::Preflight };
	}
}
