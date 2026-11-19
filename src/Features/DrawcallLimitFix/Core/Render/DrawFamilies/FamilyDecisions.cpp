// The draw families' frame decisions on the render thread. Each taken family decides, the result is published as the
// frame's ownership, and the pipeline lane is asked for the slot's pipelines.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"

#include <algorithm>

namespace DCLF::Api
{
	bool DrawFamilyFrame::Pipelines(bool a_drawable) const
	{
		// Until the frame's catalog has the family's pipelines, the family waits and nobody draws it.
		auto& drawPipelines = DrawPipelines::Get();
		if (auto* shader = static_cast<RE::BSShader*>(const_cast<void*>(Extensions::DrawFamilyOf(family).contribution.frame.programSource()))) {
			drawPipelines.SetFamilyInputs(family, *shader);  // the lane only reads it
			drawPipelines.RequestFamily(family);
		}
		const auto* catalog = FrameCatalog();
		FamilyPipelines pipelines;
		const bool built = CatalogFamilyPipelines(catalog, family, pipelines);
		// Until a revision made from a catalog with these pipelines is adopted, the family waits, so no frame claims it with nothing to draw it.
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
		// Drawable but not ready, and nothing failed, so the family waits. A failed build leaves it to the engine.
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
			// A frame without claims is entirely the engine's, and so is a family that is off or not set up.
			bool owned = false, waits = false;
			// The CPU pipeline draws a family only through its main.direct. One without it is the engine's there.
			if (scene && Draws::FamilySetUp(*scene, family) && Extensions::DrawFamilyOn(toggles, family) && !Scene::FrameView::Get().NotDrawingYet() &&
				(!Direct::CpuFrame() || entry.contribution.main.direct))
				owned = entry.contribution.frame.decide({ a_frame, scene, family, &waits });
			impl->familyOwned[f] = owned;
			impl->familyWaiting[f] = !owned && waits;
			// The engine passes the family replaces are withheld whether it is owned or still waiting.
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
