// Portals' report line, self-scheduled: room visibility every 300 published VisibleSets.

#include "Features/DrawcallLimitFix/Features/Portals/Portals.h"
#include "Features/DrawcallLimitFix/Features/Portals/RoomVisibility.h"

namespace DCLF::Features::Portals
{
	void RoomVisibilityLines()
	{
		if (!RoomVisibility::Installed())
			return;
		const auto a = RoomVisibility::Take();
		logger::info("[DCLF] room visibility (T7f checklist item 3): {} calls answered by DCLF's VisibleSet, {} by the engine (DCLF off, or no set yet)", a.dclf, a.engine);
	}
}
