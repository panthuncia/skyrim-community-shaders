#pragma once

// Engine views that Core's view machinery can draw in the engine's place, but only once a feature takes them.
// Also the feature's work at a view's engine points, and the engine roots a covered view's faces draw instead of the engine.

#include "Features/DrawcallLimitFix/Api/Settings.h"
#include "Features/DrawcallLimitFix/Api/ViewCommon.h"

#include <cstdint>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/Views.h.
	class ViewCoverage;
	class ViewRootCapture;
	class ViewAccumulators;

	/** @brief An engine view Core can draw once a feature takes it. The order is Core's slot order. */
	enum class EngineView : std::uint8_t
	{
		// The water reflection's cube faces, drawn once per frame, with tree LOD per face.
		Reflection,
		// The player's first-person skeleton, with a depth epoch, a colour epoch and a forward part.
		FirstPerson,
		Count,
	};

	/** @brief How a view's entries join its set phase. */
	enum class ViewMembership : std::uint8_t
	{
		CategoryRoot,   // only entries under the view's category root, in a phase of their own
		LodTechniques,  // main-eligible slots using a LOD Lighting technique, alongside the main phase
	};
	/** @brief How a view's pipeline slots map to its forward keys. */
	struct ViewKeyRule
	{
		bool lodTechniquesOnly = false;           // only LOD Lighting technique slots get a key
		bool forwardRasterToForwardPart = false;  // forward slots get no key because the view's forward part draws them
		// The view's own ForwardPipelineKey bits, which the pipeline lane uses to find the key's view. At most one view may have none.
		std::uint32_t keyFlags = 0;
		// Cube faces mirror the image, so they cull front faces instead of back faces.
		bool cullBackFaces = false;
	};
	/**
	 * @brief A view's shape, which tells its epochs apart.
	 *
	 * - faces: at most Core's kMaxViewFaces.
	 * - forwardPart: a forward range sorted by the depth epoch and drawn after the buckets by the colour epoch.
	 * - depthPass: the depth epoch draws the buckets' depth pipelines into the main depth.
	 * - depthRange: the viewport's maximum depth until the view's work sets one.
	 * - frustumCull: false draws every member untested. First person needs this because its skinned bounds do not follow its camera.
	 */
	struct ViewShape
	{
		std::uint32_t faces = 1;
		bool trees = false;
		bool forwardPart = false;
		bool depthPass = false;
		std::uint32_t viewBits = 0;
		std::string_view name, prefix, blocks, target;
		struct Labels
		{
			std::string_view map, tables, latches, blocks, trees;
		} labels;
		float depthRange = 1.0f;
		bool frustumCull = true;
	};
	/** @brief Whether a view keeps its last shape when no new one is made, or drops it from the revision. */
	enum class ViewShapeLife : std::uint8_t
	{
		KeepLast,
		DropWhenNone,
	};
	/** @brief A view's render area. */
	enum class ViewArea : std::uint8_t
	{
		Target,
		MainColourViewport,
	};
	/** @brief Where the engine registers a view's members' passes that DCLF withholds. */
	enum class ViewWithholding : std::uint8_t
	{
		InMainRenderers,
		OwnRenderers,
	};
	/** @brief What a view's epochs draw into. */
	enum class ViewTargetImport : std::uint8_t
	{
		ColourWithOwnDepth,
		DepthTarget,
	};

	/**
	 * @brief A view that is DCLF's. Its callbacks run once per frame.
	 *
	 * - forwardDecals: decals on the view's entries draw in the view's forward part instead of the engine.
	 * - membership: CategoryRoot also stops the hidden-chain test at the root, whose hidden bit belongs to the view.
	 * - covered: render thread at the frame's start. Returns whether the view's epochs draw its members this frame.
	 * - captureRoot: CategoryRoot only. Returns the view's root and the engine pointers it is found from.
	 * - accumulators: InMainRenderers only. Lists the engine accumulators its members register into.
	 */
	struct ViewContribution
	{
		EngineView view = EngineView::Count;
		bool forwardDecals = false;
		std::uint8_t phase = 0;
		ViewMembership membership = ViewMembership::LodTechniques;
		ViewKeyRule keys;
		ViewShape shape;
		ViewShapeLife life = ViewShapeLife::KeepLast;
		ViewArea area = ViewArea::Target;
		ViewWithholding withholding = ViewWithholding::OwnRenderers;
		ViewTargetImport targetImport = ViewTargetImport::ColourWithOwnDepth;
		bool coveredInitially = false;
		bool (*covered)(const ViewCoverage& a_coverage) = nullptr;
		void (*captureRoot)(ViewRootCapture& a_capture) = nullptr;
		void (*accumulators)(ViewAccumulators& a_accumulators) = nullptr;
	};

	/** @brief A feature's work for a view at an engine point. Runs once per frame on the render thread. */
	struct ViewWork
	{
		EngineView view = EngineView::Count;
		ViewPoint point = ViewPoint::Count;
		void (*run)(const ViewPointContext& a_context) = nullptr;
	};

	/**
	 * @brief An engine root a covered view's faces can draw instead of the engine. The engine's add-root call is skipped for it.
	 * Each root has one owner, either a draw family's faces or a view root contribution.
	 */
	enum class ViewRoot : std::uint8_t
	{
		LodLandAndObjects,
		LodTrees,
		Count,
	};
	/** @brief a_root's bit in a view's owned roots word. */
	inline constexpr std::uint32_t ViewRootBit(ViewRoot a_root) { return 1u << static_cast<std::uint32_t>(a_root); }

	// Defined in Core/Extensions/Views.h.
	class ViewRootFrame;

	/** @brief Says whether a covered view's faces draw a root's members. owned reads only registered data, never the engine. */
	struct ViewRootContribution
	{
		ViewRoot root = ViewRoot::Count;
		bool (*owned)(const ViewRootFrame& a_frame) = nullptr;
	};

	// Defined in Core/Extensions/Views.h.
	class CullHiddenNodes;

	/** @brief Whether a cull hidden source lists its nodes with their current hidden bit, or always as hidden. */
	enum class CullHiddenBit : std::uint8_t
	{
		AsNow,
		Hidden,
	};

	/**
	 * @brief Engine nodes whose hidden bit the engine toggles around the views within a frame.
	 * The frame capture records the bit the views will see, and membership reads that instead of the live bit.
	 */
	struct CullHiddenSource
	{
		CullHiddenBit bit = CullHiddenBit::AsNow;
		void (*nodes)(CullHiddenNodes& a_nodes) = nullptr;
	};
}
