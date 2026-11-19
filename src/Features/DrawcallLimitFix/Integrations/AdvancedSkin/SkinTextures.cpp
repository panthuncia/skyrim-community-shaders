// Advanced Skin's per-draw buffer. Its textures are resolved per geometry now, and DCLF's shading row does not carry them yet,
// so DCLF draws bind none and Skin.hlsli reads zero material flags.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"

namespace DCLF::Integrations::AdvancedSkin
{
	void InstallMaterialTextures(Api::InstallContext& a_context)
	{
		// SkinPerGeometry (b7), bound per draw. DCLF's draws take it from the shading row instead.
		a_context.DeclarePerDrawBuffer({ .name = "SkinPerGeometry", .stage = Api::ShaderStage::Pixel, .constantRegister = kSkinPerGeometryRegister });
	}
}
