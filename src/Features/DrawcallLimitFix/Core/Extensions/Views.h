#pragma once

// Core's side of Api/Views.h: each engine view's entry, resolved once at install, and what Core derives from them for per-object paths.
// A view no feature takes has no entry, and every per-view loop skips it. The contexts for views' callbacks and work are defined here.

#include "Features/DrawcallLimitFix/Api/Views.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ViewCommon.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace RE
{
	class NiAVObject;
	class NiNode;
}

namespace DCLF
{
	struct SceneTables;
	struct PipelineCatalog;
}

namespace DCLF::Api
{
	/** @brief What a view's work gets at its point. */
	class ViewPointContext
	{
	public:
		std::uint32_t frame = 0;
		bool failed = false;                         // DCLF claims nothing this session
		const SceneTables* tables = nullptr;         // the frame's installed tables
		const PipelineCatalog* catalog = nullptr;
	};

	/**
	 * @brief Inputs for deciding a view's coverage, computed by Core at the frame's start. Side-effect free.
	 *
	 * - epochsCovered: every epoch of the view has its recording in the adopted revision, for the graph as built now.
	 * - rebuildPending: the view's resources wait for their next import because the main resources changed.
	 * - rebuildRequested: the graph is not built as the revision recorded it.
	 */
	class ViewCoverage
	{
	public:
		bool main = false;
		bool on = false;
		bool epochsCovered = false;
		bool targetImported = false;
		bool rebuildPending = false;
		bool mainResourcesCurrent = false;
		bool rebuildRequested = false;
		std::uint32_t frame = 0;
	};

	/**
	 * @brief A CategoryRoot view's root, filed under itself, or null for none this frame.
	 * The engine pointers it is found from are folded into the category signatures.
	 */
	class ViewRootCapture
	{
	public:
		RE::NiNode* root = nullptr;
		std::array<const void*, 2> inputs{};
		std::uint32_t inputCount = 0;
	};

	/** @brief An InMainRenderers view's engine accumulators. A null one is allowed. */
	class ViewAccumulators
	{
	public:
		std::array<const void*, 4> accumulators{};
		std::uint32_t count = 0;
		bool operator==(const ViewAccumulators&) const = default;
	};

	/** @brief What a view root contribution's owned gets: the view and the active toggles. */
	class ViewRootFrame
	{
	public:
		EngineView view;
		const ToggleSet& toggles;
	};

	/** @brief Where a cull hidden source lists its nodes, and whether they are listed as hidden or with their current bit. */
	class CullHiddenNodes
	{
	public:
		/** @brief Lists a_node with the source's bit. Null is ignored. */
		void Add(const RE::NiAVObject* a_node);

		std::vector<std::pair<const RE::NiAVObject*, bool>>& list;
		bool hidden = false;
	};
}

namespace DCLF::Extensions
{
	inline constexpr std::size_t kEngineViews = static_cast<std::size_t>(Api::EngineView::Count);
	inline constexpr std::size_t kViewPoints = static_cast<std::size_t>(Api::ViewPoint::Count);
	inline constexpr std::size_t kViewRoots = static_cast<std::size_t>(Api::ViewRoot::Count);

	/** @brief A view's resolved entry. Every field keeps its default when no feature registered it. */
	struct ViewEntry
	{
		bool registered = false;
		Api::ToggleId toggle;
		bool forwardDecals = false;
		std::uint8_t phase = 0;      // the feature's own SetPhase bit
		std::uint32_t viewBit = 0;   // the feature's own kView* bit
		Api::ViewMembership membership = Api::ViewMembership::LodTechniques;
		Api::ViewKeyRule keys;
		// The shape's string views, and Core's ViewTraits, point into strings owned here.
		Api::ViewShape shape;
		std::string name, prefix, blocks, target;
		std::array<std::string, 5> labels;  // map, tables, latches, blocks, trees
		Api::ViewShapeLife life = Api::ViewShapeLife::KeepLast;
		Api::ViewArea area = Api::ViewArea::Target;
		Api::ViewWithholding withholding = Api::ViewWithholding::OwnRenderers;
		Api::ViewTargetImport targetImport = Api::ViewTargetImport::ColourWithOwnDepth;
		bool coveredInitially = false, coveredOnFailedGraph = false;
		bool (*covered)(const Api::ViewCoverage&) = nullptr;
		void (*captureRoot)(Api::ViewRootCapture&) = nullptr;
		void (*accumulators)(Api::ViewAccumulators&) = nullptr;
		bool (*residueDue)() = nullptr;
		// Per ViewPoint, the features' work in registration order.
		std::array<std::vector<void (*)(const Api::ViewPointContext&)>, kViewPoints> work;
	};
	/** @brief Every view's entry, and what Core's per-object paths read of them. */
	struct ViewSet
	{
		std::array<ViewEntry, kEngineViews> views;
		std::uint8_t phases = 0;
		std::uint8_t rootPhases = 0;    // exclusive phases
		std::uint8_t lodPhases = 0;     // beside the main phase
		std::uint32_t viewBits = 0;
		std::uint32_t rootViewBits = 0;
		// The CategoryRoot views, in slot order.
		std::array<Api::EngineView, kEngineViews> rootViews{};
		std::uint32_t rootCount = 0;
		// The order the set decision tests views: CategoryRoot views first, main-ready first, then the rest, each in slot order.
		std::array<Api::EngineView, kEngineViews> decideOrder{};
		std::uint32_t decideCount = 0;
		// InMainRenderers views in slot order. Tested inside WithholdMain.
		std::array<Api::EngineView, kEngineViews> mainRendererViews{};
		std::uint32_t mainRendererCount = 0;
		// OwnRenderers views in slot order. Tested after WithholdMain.
		std::array<Api::EngineView, kEngineViews> ownRendererViews{};
		std::uint32_t ownRendererCount = 0;
		// The view whose forward keys carry no view bits. The pipeline lane uses it for keys no other view's bits name.
		Api::EngineView plainKeyView = Api::EngineView::Count;
		// Per root, its view root contribution's owned, or null. A draw family's faces root is owned by the family's entry instead.
		std::array<bool (*)(const Api::ViewRootFrame&), kViewRoots> rootOwned{};
		// Roots of the view root contributions. A residue watch covers these besides the owned ones.
		std::uint32_t rootBits = 0;
		std::vector<Api::CullHiddenSource> cullHiddenSources;
	};

	namespace Detail
	{
		// Written only by ResolveViews on the main thread before DCLF's threads exist. Read-only after, so plain reads are safe.
		inline ViewSet resolvedViews;
	}

	/** @brief Any thread, after InstallFeatures. */
	inline const ViewSet& Views() { return Detail::resolvedViews; }
	/** @brief Any thread, after InstallFeatures. */
	inline const ViewEntry& ViewEntryOf(Api::EngineView a_view) { return Views().views[static_cast<std::size_t>(a_view)]; }
	/** @brief Any thread, after InstallFeatures. Whether a feature took a_view. */
	inline bool ViewRegistered(Api::EngineView a_view) { return ViewEntryOf(a_view).registered; }
	/** @brief Main thread, once, at the end of InstallFeatures. Resolves the views, their work, the view roots and the cull hidden sources. */
	void ResolveViews(std::span<const Api::ViewContribution, kEngineViews> a_views, std::span<const Api::ViewWork> a_work,
		std::span<const Api::ViewRootContribution, kViewRoots> a_roots, std::span<const Api::CullHiddenSource> a_cullHidden);

	/** @brief Render thread, at the frame's start. Appends every cull hidden source's nodes. The caller sorts the list. */
	void CollectCullHidden(std::vector<std::pair<const RE::NiAVObject*, bool>>& a_list);

	/** @brief Whether DCLF draws a_view under a_toggles. */
	inline bool ViewOn(const ToggleSet& a_toggles, Api::EngineView a_view)
	{
		return a_toggles.On(ViewEntryOf(a_view).toggle);
	}

	/**
	 * @brief Any thread, after InstallFeatures. The view a forward key belongs to: the view whose bits it carries, or else the plain key view.
	 * Its targets and culling are that view's.
	 */
	inline Api::EngineView ForwardKeyView(std::uint32_t a_flags)
	{
		const auto& views = Views();
		for (std::size_t v = 0; v < kEngineViews; ++v) {
			const std::uint32_t bits = views.views[v].keys.keyFlags;
			if (views.views[v].registered && bits && (a_flags & bits) == bits)
				return static_cast<Api::EngineView>(v);
		}
		return views.plainKeyView;
	}

	/** @brief Whether decals on a_view's entries draw in its forward part. */
	inline bool ViewForwardDecals(Api::EngineView a_view)
	{
		return ViewEntryOf(a_view).forwardDecals;
	}
}
