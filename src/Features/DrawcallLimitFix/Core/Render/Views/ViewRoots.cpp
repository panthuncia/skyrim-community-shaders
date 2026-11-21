// Which engine roots a covered view's faces draw in the engine's place. Render thread.
// See dclf-architecture.md, "Water reflections".

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	std::uint32_t IndirectDraws::ViewLodRootsOwned(Api::EngineView a_view, bool a_plain)
	{
		// A root is owned when DCLF's faces draw its family, or when its contribution's rootOwned says so.
		// An unowned root stays in the engine with its members withheld.
		if (!a_plain || !ViewDrawable(a_view) || !impl->ViewCovered(a_view) || !Policy::WithholdFromEngine())
			return 0;
		std::uint32_t owned = 0;
		const auto& facesOwned = impl->View(a_view).facesOwned;
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f) {
			const auto root = Extensions::DrawFamilies()[f].contribution.faces.root;
			owned |= (facesOwned[f] && root < Api::ViewRoot::Count) ? Api::ViewRootBit(root) : 0u;
		}
		const auto& views = Extensions::Views();
		const Api::ViewRootFrame frame{ a_view };
		for (std::size_t r = 0; r < Extensions::kViewRoots; ++r)
			if (const auto rootOwned = views.rootOwned[r]; rootOwned && rootOwned(frame))
				owned |= Api::ViewRootBit(static_cast<Api::ViewRoot>(r));
		return owned;
	}
}
