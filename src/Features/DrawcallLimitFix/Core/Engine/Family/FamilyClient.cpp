// The engine draw families' shared client lifecycle: the parts that need the runtime.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Family/FamilyClient.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Utils/Logging.h"

namespace DCLF::Family::Detail
{
	bool GraphActive()
	{
		return RenderGraphRuntime::Get().IsActive();
	}

	bool CanSetUp()
	{
		return RenderGraphRuntime::Get().Host() && FrameAnchors::IsRenderOwner();
	}

	void LogDeclared(std::string_view a_family, std::string_view a_id, bool a_declared, std::string_view a_refusal)
	{
		if (a_declared)
			logger::info("[DCLF] {}: the {} client declared at setup; it registers with the next frame revision", a_family, a_id);
		else
			logger::warn("[DCLF] {}: the {} client could not be declared; {}", a_family, a_id, a_refusal);
	}
}
