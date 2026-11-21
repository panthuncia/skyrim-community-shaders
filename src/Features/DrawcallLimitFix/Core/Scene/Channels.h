#pragma once

// The scene's channels: every message between the render thread (Scene::FrameView) and the coordinator's passes.
// Each is lock-free with one producer side and one consumer side. Neither thread reads the other's state, only what was sent here.

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "Features/DrawcallLimitFix/Api/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/AncestorValues.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Runtime/PublicationLog.h"
#include "Features/DrawcallLimitFix/Core/Runtime/Retirement.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Publication.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/SharedBindings.h"

namespace DCLF
{
	struct FrameGlobals;
}

namespace DCLF::Scene
{
	/**
	 * @brief What a scene pass takes of the frame, posted latest-wins by the render thread. The post wakes the pump.
	 * A pass reads only what it took, and keeps the last one while nothing newer is posted.
	 */
	struct FrameInputs
	{
		std::shared_ptr<const FrameGlobals> globals;
		std::uint32_t frame = 0;
		std::uint32_t membershipWitness = 0;
		ToggleSet toggles{};
		std::uint32_t togglesGeneration = 0;
		std::uint32_t verdictsGeneration = 0;
	};

	/** @brief Light Limit Fix's room indices, filled by the render thread when its generation moves. */
	struct AncestorMapInput
	{
		std::shared_ptr<const Extensions::AncestorValueMap> map;
		std::uint64_t generation = 0;
	};

	/**
	 * @brief The category nodes and portal roots as the render thread found them, since the scene work reads no TES, cell or portal graph.
	 * Immutable, and holds every node it names. Remade after a detach too, since a detach can take a category node before the signature sees the cell go.
	 */
	struct CategoryCapture
	{
		std::uint64_t generation = 0;
		std::uint64_t signature = 0;
		ankerl::unordered_dense::set<RE::NiNode*> nodes;
		std::vector<RE::NiPointer<RE::NiNode>> held;  // every node but the unheld roots
		struct Root
		{
			RE::NiPointer<RE::NiAVObject> root;  // null for an unheld root
			RE::NiNode* category = nullptr;
			// An unheld root's pointer is only a key. It may be read only while the capture's pins hold it.
			RE::NiAVObject* object = nullptr;
		};
		ankerl::unordered_dense::map<const RE::NiAVObject*, Root> roots;
		ankerl::unordered_dense::map<const RE::NiNode*, std::vector<const RE::NiAVObject*>> children;
		// Holds everything new under the capture until the batch that applied it is done. The coordinator reads them only at the
		// capture's first application.
		mutable std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		// By view, a CategoryRoot view's root or null. It is both a category node and a root.
		std::array<const RE::NiNode*, Extensions::kEngineViews> viewRoots{};
		// Unheld roots such as the particle manager's masters, held by key alone. The capture pins each only until the coordinator applied it.
		ankerl::unordered_dense::set<const RE::NiAVObject*> unheld;
	};

	/**
	 * @brief A fade node the coordinator holds no reference to, as when another instance's clone set it. The render thread checks the
	 * property still names it, takes a reference and posts it back.
	 */
	struct FadeNodeRequest
	{
		RE::NiPointer<RE::BSGeometry> geometry;
		const void* fadeNode = nullptr;
	};
	struct FadeNodeAnswer
	{
		const RE::BSGeometry* geometry = nullptr;  // a key
		const void* fadeNode = nullptr;            // the requested key
		RE::NiPointer<RE::NiAVObject> node;        // null: the property names another node now, or the geometry left the world
	};

	/** @brief Applied only where the slot still holds its key. */
	struct PipelineConstantsPost
	{
		std::uint32_t slot = 0, binding = 0, generation = 0;
		PipelineKey key{};
		GeometryConstants constants{};
	};

	/** @brief What the coordinator tells PrimaryCull, in the order it made them. */
	struct PrimaryNote
	{
		enum class Kind : std::uint8_t
		{
			HiddenKey,
			MemberLost,
			AllMembersLost
		};
		Kind kind = Kind::HiddenKey;
		const void* key = nullptr;  // the hidden store's key, or the member's geometry (keys: nothing is dereferenced)
	};

	/** @brief A join's material with no capture held, for the render thread to capture at EarlyPrepass. */
	struct MaterialRequest
	{
		RE::NiPointer<RE::BSShaderProperty> property;
		const RE::BSShaderMaterial* material = nullptr;
		std::uint32_t pass = 0;
	};
	struct MaterialAnswer
	{
		const RE::BSShaderMaterial* material = nullptr;
		bool captured = false;  // false: the property held another material by then (the request stale)
	};

	/** @brief Each reference is dropped on the render thread after the catch-ups. */
	struct SwitchCatchUp
	{
		std::vector<RE::NiPointer<RE::NiAVObject>> attached;
		std::vector<SwitchEvent> switches;
	};

	struct AccumulateInputCounters
	{
		std::atomic<std::uint64_t> ancestorMapsPosted{ 0 }, ancestorMapsTaken{ 0 }, pipelineFramesPosted{ 0 }, pipelineFramesTaken{ 0 };
		std::atomic<std::uint64_t> ancestorValuesMoved{ 0 };  // object slots a taken map moved (Coordinator::RefreshAncestorValues)
		std::atomic<std::uint64_t> ancestorFound{ 0 }, ancestorNoKey{ 0 }, ancestorNotMapped{ 0 };
		std::atomic<std::uint64_t> pipelineBlocksMade{ 0 }, pipelineBlocksWaited{ 0 };
	};

	struct Channels
	{
		static Channels& Get()
		{
			static Channels channels;
			return channels;
		}

		// Render thread -> coordinator.
		::RenderGraph::Frame::LatestSlot<FrameInputs> frameInputs;
		::RenderGraph::Frame::LatestSlot<std::shared_ptr<const CategoryCapture>> categoryPosted;
		std::atomic<std::shared_ptr<const GeometryPort::PipelineFrame>> pipelineFramePosted;
		std::atomic<std::shared_ptr<const AncestorMapInput>> ancestorMapPosted;
		std::atomic<std::shared_ptr<const SharedBindings::ProjectedCapture>> projectedPosted;
		// PrimaryCull's admitted roots, a whole set, latest wins.
		::RenderGraph::Frame::LatestSlot<std::vector<Api::StandInRoot>> standInRootsPosted;
		RenderGraph::Frame::EventQueue<std::unique_ptr<PipelineConstantsPost>, 256> pipelineConstantsPosted;
		// While a load screen is up the passes run no live check.
		std::atomic<bool> loadingSeen{ false };
		RenderGraph::Frame::EventQueue<MaterialAnswer> materialRequestsAnswered{ &WakeSceneMaterialAnswer };
		RenderGraph::Frame::EventQueue<FadeNodeAnswer, 256> fadeNodeAnswers;
		Retired<ScenePublication> retiredPublications;

		// Coordinator -> render thread.
		PublicationLog<PublicationDeltas> publicationLog;  // every publication's deltas, walked by the frame's start
		RenderGraph::Frame::EventQueue<PrimaryNote> primaryNotes;
		RenderGraph::Frame::EventQueue<MaterialRequest> materialRequests;
		RenderGraph::Frame::EventQueue<FadeNodeRequest, 256> fadeNodeRequests;
		// Tracked geometries the mirror held no record of, captured at the next frame start.
		RenderGraph::Frame::EventQueue<RE::NiPointer<RE::NiAVObject>, 256> mirrorCaptureRequests;
		RenderGraph::Frame::EventQueue<std::shared_ptr<SwitchCatchUp>, 64> switchCatchUps;
		// Tells the render thread which category captures it may let go of.
		std::atomic<std::uint64_t> categoryAppliedGeneration{ 0 };
		// The newest capture whose pins the coordinator is done with.
		std::atomic<std::uint64_t> categoryPinsReleased{ 0 };
		std::atomic<bool> categoryDetachSeen{ false };  // a pass drained a detach: the next frame start's category capture is forced

		AccumulateInputCounters accumulateInputs;
		std::atomic<std::uint64_t> catchUpsBySwitch{ 0 }, catchUpsByAttach{ 0 }, catchUpNs{ 0 };
	};
}
