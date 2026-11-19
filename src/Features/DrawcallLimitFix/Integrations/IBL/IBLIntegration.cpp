// Image Based Lighting: its SH (t76, t77) and static cubemaps (t78, t79) as frame textures. Callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/IBL/IBL.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "State.h"

#include "Features/IBL.h"
#include "Features/IBL/ORGIBL.h"

namespace DCLF::Integrations::IBL
{
	namespace
	{
		// The SH Prepass binds at t76 and t77 unless IBL is off for the scene. They are persistent textures made at setup.
		// Off for the scene, Prepass binds nothing, so the captured views stand.
		bool ShBinds()
		{
			const auto& ibl = globals::features::ibl;
			return ibl.loaded && !ibl.IsDisabledForCurrentScene();
		}

		template <ID3D11ShaderResourceView* (::IBL::*View)() const>
		void* ShView()
		{
			return ShBinds() ? (globals::features::ibl.*View)() : nullptr;
		}

		// The static cubemaps ReflectionsPrepass binds at t78 and t79, only on a frame whose water reflection ran it.
		// Otherwise the bindings stand from an earlier frame or were never made, so the captured views.
		template <auto Texture>
		void* StaticView()
		{
			const auto& ibl = globals::features::ibl;
			return ibl.loaded && globals::state->activeReflections && ibl.*Texture ? (ibl.*Texture)->srv.get() : nullptr;
		}

		void Install(Api::InstallContext& a_context)
		{
			// With CS_ORG_IBL=1 the SH views are the graph's exports, so commits read them ordered after the projection at the Prepass anchor.
			a_context.RegisterFrameTexture({ .name = "ibl environment", .textureRegister = kEnvironmentRegister, .view = &ShView<&::IBL::EnvIBLSRV>,
				.capturedWhenNull = true, .graphResources = { ORGIBL::kEnvId } });
			a_context.RegisterFrameTexture({ .name = "ibl sky", .textureRegister = kSkyRegister, .view = &ShView<&::IBL::SkyIBLSRV>,
				.capturedWhenNull = true, .graphResources = { ORGIBL::kSkyId } });
			a_context.RegisterFrameTexture({ .name = "ibl static diffuse", .textureRegister = kStaticDiffuseRegister,
				.view = &StaticView<&::IBL::staticDiffuseIBLTexture>, .capturedWhenNull = true });
			a_context.RegisterFrameTexture({ .name = "ibl static specular", .textureRegister = kStaticSpecularRegister,
				.view = &StaticView<&::IBL::staticSpecularIBLTexture>, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "image based lighting", .install = &Install, .status = [] { return FeatureStatus(globals::features::ibl); } };
	}
}
