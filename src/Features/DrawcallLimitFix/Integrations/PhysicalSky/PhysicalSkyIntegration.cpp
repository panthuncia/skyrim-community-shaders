// Physical Sky: its LUTs (t61-t63, t106) as frame textures. The render thread's callbacks read the feature live.

#include "Features/DrawcallLimitFix/Integrations/PhysicalSky/PhysicalSky.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Globals.h"

#include "Features/PhysicalSky.h"
#include "Features/PhysicalSky/ORGPhysicalSky.h"

namespace DCLF::Integrations::PhysicalSky
{
	namespace
	{
		// Persistent textures made at setup. Prepass binds them while the feature is on; off, the shaders read none of them.
		template <auto Texture>
		void* LutView()
		{
			const auto& sky = globals::features::physicalSky;
			return sky.loaded && sky.cbData.enabled && sky.*Texture ? (sky.*Texture)->srv.get() : nullptr;
		}

		// On the graph the views are the LUT twins' exports, so commits read them ordered after the LUT passes at EarlyPrepass.
		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "physical sky transmittance", .textureRegister = kTransmittanceRegister,
				.view = &LutView<&::PhysicalSky::texTrLut>, .capturedWhenNull = true, .graphResources = { ORGPhysicalSky::kTransmittanceLutId } });
			a_context.RegisterFrameTexture({ .name = "physical sky sky-view", .textureRegister = kSkyViewRegister,
				.view = &LutView<&::PhysicalSky::texSvLut>, .capturedWhenNull = true, .graphResources = { ORGPhysicalSky::kSkyViewLutId } });
			a_context.RegisterFrameTexture({ .name = "physical sky aerial perspective", .textureRegister = kAerialPerspectiveRegister,
				.view = &LutView<&::PhysicalSky::texApLut>, .capturedWhenNull = true, .graphResources = { ORGPhysicalSky::kApLutId } });
			a_context.RegisterFrameTexture({ .name = "physical sky disk phase", .textureRegister = kDiskPhaseRegister,
				.view = &LutView<&::PhysicalSky::texDiskPhaseLut>, .capturedWhenNull = true, .graphResources = { ORGPhysicalSky::kDiskPhaseLutId } });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "physical sky", .install = &Install, .status = [] { return FeatureStatus(globals::features::physicalSky); } };
	}
}
