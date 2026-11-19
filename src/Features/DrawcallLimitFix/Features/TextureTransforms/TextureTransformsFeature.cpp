// Texture transforms: the materials' TexcoordOffset and the ports' declared frame variables, patched into the installed payload's
// material rows every frame, since the rows hold their records' captured value.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"

namespace DCLF::Features::TextureTransforms
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			RegisterPool(a_context);
			RegisterPass(a_context);
			RegisterProduct(a_context);
			// So a capture or controller write that moved only these leaves the records as they are.
			a_context.RegisterFrameVSVariablePatch();
		}
	}

	Api::Feature Feature()
	{
		return { "texture-transforms", &Install };
	}
}
