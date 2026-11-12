// The draw families' frame decisions (IndirectDraws::DecideDrawFamilies): each taken family's decide, published as the frame's ownership
// (the depth commit's, a trees view's, PassCapture's), and the pipeline lane its decide asks for its slot's pipelines
// (Api::DrawFamilyFrame::Pipelines). Render thread.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"

#include <algorithm>

namespace DCLF::Api
{
	bool DrawFamilyFrame::Pipelines(bool a_drawable) const
	{
		// The lane builds the slot's pipelines from the family's program source (TreeInstances': the DistantTree shader); until the
		// frame's catalog has them the family waits, withheld (nobody draws it).
		auto& drawPipelines = DrawPipelines::Get();
		if (auto* shader = static_cast<RE::BSShader*>(const_cast<void*>(Extensions::DrawFamilyOf(family).contribution.frame.programSource()))) {
			drawPipelines.SetFamilyInputs(family, *shader);  // the lane only reads it
			drawPipelines.RequestFamily(family);
		}
		const auto* catalog = FrameCatalog();
		FamilyPipelines pipelines;
		const bool built = CatalogFamilyPipelines(catalog, family, pipelines);
		// The adopted revision's recordings draw the family with these pipelines (its shapes', PassFrame::families): until a revision made
		// from a catalog that has them is adopted, the family waits, withheld: no frame claims it with nothing recorded to draw it.
		const auto adopted = IndirectDraws::Get().AdoptedFamilyPipelines(family);
		const bool recorded = adopted && Draws::SameHandle(adopted->depth, pipelines.depth) && Draws::SameHandle(adopted->colour, pipelines.colour) &&
		                      Draws::SameHandle(adopted->colour2, pipelines.colour2);
		const bool owned = built && a_drawable && recorded;
		if (owned) {
			auto& published = scene->familyPipelines[static_cast<std::size_t>(family)];
			const auto current = published.load(std::memory_order_acquire);
			if (!current || !Draws::SameHandle(current->depth, pipelines.depth) || !Draws::SameHandle(current->colour, pipelines.colour) ||
				!Draws::SameHandle(current->colour2, pipelines.colour2))
				published.store(std::make_shared<const FamilyPipelines>(pipelines), std::memory_order_release);
		}
		const bool failed = catalog && catalog->families[static_cast<std::size_t>(family)].failed;
		drawPipelines.NoteFamilyWait(family, !built && !failed);
		// Drawable, not ready, nothing failed: the family waits (its passes withheld). A failed build leaves it the engine's.
		if (waits)
			*waits = a_drawable && !owned && !failed;
		return owned;
	}
}

namespace DCLF
{
	void IndirectDraws::DecideDrawFamilies(std::uint32_t a_frame)
	{
		auto* scene = impl->scene.get();
		const auto toggles = ActiveToggles();
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f) {
			const auto& entry = Extensions::DrawFamilies()[f];
			if (!entry.taken)
				continue;
			const auto family = static_cast<Api::DrawFamily>(f);
			// A frame without claims is the engine's whole, the family's draws included; so is one whose family is not set up or off.
			bool owned = false, waits = false;
			// The CPU pipeline draws a family through its main.direct (its main draw's row and arguments outside ORG); one without it is
			// the engine's there.
			if (scene && Draws::FamilySetUp(*scene, family) && Extensions::DrawFamilyOn(toggles, family) && !Scene::FrameView::Get().NotDrawingYet() &&
				(!Direct::CpuFrame() || entry.contribution.main.direct))
				owned = entry.contribution.frame.decide({ a_frame, scene, family, &waits });
			impl->familyOwned[f] = owned;
			impl->familyWaiting[f] = !owned && waits;
			// Owned, or waiting to be: the engine's passes the family replaces are withheld either way.
			PassCapture::Get().SetFamilyWithheld(family, owned || waits);
			if (entry.contribution.frame.owned)
				entry.contribution.frame.owned(owned);
		}
	}

	bool IndirectDraws::DrawFamilyOwned(Api::DrawFamily a_family) const
	{
		return impl && impl->familyOwned[static_cast<std::size_t>(a_family)];
	}

	std::uint32_t IndirectDraws::DrawFamiliesWaiting() const
	{
		return impl ? static_cast<std::uint32_t>(std::ranges::count(impl->familyWaiting, true)) : 0u;
	}
}
