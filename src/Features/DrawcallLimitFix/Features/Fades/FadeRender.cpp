// Registers Fades' render side. The draws read root states at t120, root rows at t119 and fade seeds at t117.
// The seeds are what DCLFAlphaFade falls back to. Core's cull reads the same data through the Root* cull bindings.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"

namespace DCLF::Features::Fades
{
	namespace
	{
		// GPU frames read the previous frame's published states. CPU frames read their CPU stage's pool by index.
		void SelectStatesBinding(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread && Direct::CpuRootStatesFor(a_select.frame)) {
				a_select.index = CpuStatesIndex();
				return;
			}
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.buffer = &fades->FadeStatesRead(a_select.frame);
		}

		void SelectRootsBinding(Api::FrameBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.buffer = &fades->fadeRoots;
		}

		// The seeds live in a frame value pool, so only an index is bound here.
		void SelectSeedsBinding(Api::FrameBindingSelect& a_select)
		{
			if (a_select.renderThread && SceneOf(a_select.scene))
				a_select.index = SeedsIndex();
		}

		void SelectRootPrograms(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->fadeRootPrograms);
		}

		void SelectRootRows(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene)) {
				a_select.Versioned(fades->fadeRoots);
				a_select.Rows(fades->fadeRootCount);
			}
		}

		void SelectRootStates(Api::CullBindingSelect& a_select)
		{
			if (Direct::CpuRootStatesFor(a_select.frame)) {
				a_select.Index(CpuStatesIndex());
				return;
			}
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->FadeStatesRead(a_select.frame));
		}

		void SelectRootStepStates(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Versioned(fades->fadeStates);
		}

		void SelectRootSeeds(Api::CullBindingSelect& a_select)
		{
			a_select.Index(SeedsIndex());
		}

		void SelectRootBounds(Api::CullBindingSelect& a_select)
		{
			a_select.Index(RootBoundsIndex());
		}

		void SelectRootFrame(Api::CullBindingSelect& a_select)
		{
			if (const auto* fades = SceneOf(a_select.scene))
				a_select.Buffer(fades->fadeFrameBuffer);
		}
	}

	void RegisterRender(Api::InstallContext& a_context)
	{
		// The order matters. The CPU stage's pool samples after the root bounds and seeds pools, because it reads both.
		RegisterRootBounds(a_context);
		RegisterSeeds(a_context);
		RegisterCpuStage(a_context);
		// After every feature's scene resources, because the visibility readback reads these rows.
		RegisterResources(a_context);
		// After every feature's commit hook, because the root programs include Portals' programs.
		RegisterCommit(a_context);
		RegisterPasses(a_context);
		RegisterPixelRows(a_context);
		// The named-address parity looks these names up.
		a_context.RegisterFrameBinding({ .name = "fade states SRV", .textureRegister = kFadeStatesRegister, .select = &SelectStatesBinding });
		a_context.RegisterFrameBinding({ .name = "fade roots SRV", .textureRegister = kFadeRootsRegister, .select = &SelectRootsBinding });
		a_context.RegisterFrameBinding({ .name = "fade seeds SRV", .textureRegister = kFadeSeedsRegister, .select = &SelectSeedsBinding });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootPrograms, .name = "cs.dclf.fade-root-programs", .select = &SelectRootPrograms });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootRows, .name = "cs.dclf.fade-roots", .select = &SelectRootRows });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStates, .name = "cs.dclf.fade-states-out", .select = &SelectRootStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootStepStates, .name = "cs.dclf.fade-states", .select = &SelectRootStepStates });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootSeeds, .name = "cs.dclf.frame-values.fade-seeds", .select = &SelectRootSeeds });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootBounds, .name = "cs.dclf.frame-values.fade-root-bounds", .select = &SelectRootBounds });
		a_context.RegisterCullBinding({ .slot = Api::CullBindingSlot::RootFrame, .name = "cs.dclf.fade-frame", .select = &SelectRootFrame });
	}
}
