// The views' entries: the features' contributions and work, and what the per-object paths read of them.

// Internal.h first, because volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Extensions
{
	namespace
	{
		ViewEntry& EntryOf(ViewSet& a_set, Api::EngineView a_view) { return a_set.views[static_cast<std::size_t>(a_view)]; }
		auto& WorkAt(ViewEntry& a_entry, Api::ViewPoint a_point) { return a_entry.work[static_cast<std::size_t>(a_point)]; }

		/** @brief Copies a shape into an entry with its strings. PointShape points the views at them once the entry is in place. */
		void SetShape(ViewEntry& a_entry, const Api::ViewShape& a_shape)
		{
			a_entry.shape = a_shape;
			a_entry.name = a_shape.name;
			a_entry.prefix = a_shape.prefix;
			a_entry.blocks = a_shape.blocks;
			a_entry.target = a_shape.target;
			a_entry.labels = { std::string(a_shape.labels.map), std::string(a_shape.labels.tables), std::string(a_shape.labels.latches),
				std::string(a_shape.labels.blocks), std::string(a_shape.labels.trees) };
		}

		void PointShape(ViewEntry& a_entry)
		{
			auto& shape = a_entry.shape;
			shape.name = a_entry.name;
			shape.prefix = a_entry.prefix;
			shape.blocks = a_entry.blocks;
			shape.target = a_entry.target;
			shape.labels = { a_entry.labels[0], a_entry.labels[1], a_entry.labels[2], a_entry.labels[3], a_entry.labels[4] };
		}

		/** @brief Copies a feature's contribution into its view's entry. */
		void FillRegistered(ViewEntry& a_entry, const Api::ViewContribution& a_view)
		{
			a_entry.registered = true;
			a_entry.forwardDecals = a_view.forwardDecals;
			a_entry.phase = a_view.phase;
			a_entry.viewBit = a_view.shape.viewBits;
			a_entry.membership = a_view.membership;
			a_entry.keys = a_view.keys;
			SetShape(a_entry, a_view.shape);
			a_entry.life = a_view.life;
			a_entry.area = a_view.area;
			a_entry.withholding = a_view.withholding;
			a_entry.targetImport = a_view.targetImport;
			a_entry.coveredInitially = a_view.coveredInitially;
			a_entry.covered = a_view.covered;
			a_entry.captureRoot = a_view.captureRoot;
			a_entry.accumulators = a_view.accumulators;
		}

		/** @brief The set's phases and view bits from its entries. */
		void DeriveViewPhases(ViewSet& a_set)
		{
			for (const auto& entry : a_set.views) {
				const bool root = entry.membership == Api::ViewMembership::CategoryRoot;
				a_set.phases |= entry.phase;
				(root ? a_set.rootPhases : a_set.lodPhases) |= entry.phase;
				a_set.viewBits |= entry.viewBit;
				a_set.rootViewBits |= root ? entry.viewBit : 0u;
			}
		}

		/** @brief The root views, the decision order with CategoryRoot views first, and the renderer view lists, in slot order. */
		void DeriveOrders(ViewSet& a_set)
		{
			for (std::size_t v = 0; v < a_set.views.size(); ++v) {
				const auto& entry = a_set.views[v];
				const auto view = static_cast<Api::EngineView>(v);
				if (!entry.phase)
					continue;
				if (entry.membership == Api::ViewMembership::CategoryRoot) {
					a_set.rootViews[a_set.rootCount++] = view;
					a_set.decideOrder[a_set.decideCount++] = view;
				}
				if (entry.withholding == Api::ViewWithholding::InMainRenderers)
					a_set.mainRendererViews[a_set.mainRendererCount++] = view;
				else
					a_set.ownRendererViews[a_set.ownRendererCount++] = view;
			}
			for (std::size_t v = 0; v < a_set.views.size(); ++v)
				if (a_set.views[v].phase && a_set.views[v].membership != Api::ViewMembership::CategoryRoot)
					a_set.decideOrder[a_set.decideCount++] = static_cast<Api::EngineView>(v);
		}

		/** @brief The view for forward keys without view bits. Fails the install for bits that clash, or a second view with none. */
		void DeriveForwardKeyViews(ViewSet& a_set)
		{
			constexpr std::uint32_t coreBits = ForwardPipelineKey::kForwardTwoSided | ForwardPipelineKey::kForwardTreeLod;
			std::uint32_t claimed = coreBits;
			for (std::size_t v = 0; v < a_set.views.size(); ++v) {
				const auto& entry = a_set.views[v];
				if (!entry.registered)
					continue;
				const std::uint32_t bits = entry.keys.keyFlags;
				if (bits & claimed)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("view {}'s forward key flags {:#x} overlap Core's or another view's", static_cast<std::uint32_t>(v), bits));
				claimed |= bits;
				if (bits)
					continue;
				if (a_set.plainKeyView != Api::EngineView::Count)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("views {} and {} both have forward keys without key flags", static_cast<std::uint32_t>(a_set.plainKeyView), static_cast<std::uint32_t>(v)));
				a_set.plainKeyView = static_cast<Api::EngineView>(v);
			}
		}
	}

	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work,
		std::span<const Api::ViewRootContribution, kViewRoots> a_roots, std::span<const Api::CullHiddenSource> a_cullHidden)
	{
		static bool once = false;
		if (std::exchange(once, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the views were resolved twice");
		ViewSet set;
		for (const auto& view : a_views)
			if (view.view < Api::EngineView::Count)
				FillRegistered(EntryOf(set, view.view), view);
		for (const auto& work : a_work) {
			auto& entry = EntryOf(set, work.view);
			if (!entry.registered)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("work was registered for view {}, which no feature contributes", static_cast<std::uint32_t>(work.view)));
			WorkAt(entry, work.point).push_back(work.run);
		}
		DeriveViewPhases(set);
		DeriveOrders(set);
		DeriveForwardKeyViews(set);
		for (std::size_t r = 0; r < kViewRoots; ++r) {
			set.rootOwned[r] = a_roots[r].root < Api::ViewRoot::Count ? a_roots[r].owned : nullptr;
			set.rootBits |= set.rootOwned[r] ? Api::ViewRootBit(static_cast<Api::ViewRoot>(r)) : 0u;
		}
		set.cullHiddenSources.assign(a_cullHidden.begin(), a_cullHidden.end());
		Detail::resolvedViews = std::move(set);
		// Point the shapes' views at the strings where they now live.
		for (auto& entry : Detail::resolvedViews.views)
			PointShape(entry);
	}

	void CollectCullHidden(std::vector<std::pair<const RE::NiAVObject*, bool>>& a_list)
	{
		for (const auto& source : Views().cullHiddenSources) {
			Api::CullHiddenNodes nodes{ a_list, source.bit == Api::CullHiddenBit::Hidden };
			source.nodes(nodes);
		}
	}
}

namespace DCLF::Api
{
	void CullHiddenNodes::Add(const RE::NiAVObject* a_node)
	{
		// A Hidden source's bit is not read, as for FirstPerson's skeleton, which is hidden for every view the scene serves.
		if (a_node)
			list.emplace_back(a_node, hidden || Engine::IsHidden(a_node));
	}
}
