// GpuOcclusion feature registration.

#include "Features/DrawcallLimitFix/Features/GpuOcclusion/GpuOcclusion.h"

namespace DCLF::Features::GpuOcclusion
{
	namespace
	{
		void Install(Api::InstallContext& a_context)
		{
			RegisterPyramid(a_context);
		}
	}

	Api::Feature Feature()
	{
		return { "gpu-occlusion", &Install };
	}
}
