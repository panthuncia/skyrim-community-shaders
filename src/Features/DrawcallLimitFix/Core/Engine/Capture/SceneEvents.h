#pragma once

// The scene capture's events. The hooked engine writers push them on any thread, and the coordinator's scene passes and the mirror drain them.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"

namespace DCLF::Scene
{
	/** @brief Render thread, PostPostLoad. Installs every scene event hook below. */
	void InstallSceneEvents();
	/** @brief Whether the hidden events' source is installed. */
	bool HiddenStoresLive();

	/**
	 * @brief Pushes the values a hooked writer changed, captured after the write on its thread. They apply in order with the
	 * attaches and detaches.
	 */
	inline void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields)
	{
		if (a_node)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureNodeFields(*a_node, a_fields) });
	}

	inline void PushPropertyUpdate(const RE::BSShaderProperty* a_property, std::uint32_t a_fields)
	{
		if (a_property)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureProperty(*a_property) });
	}

	inline void PushAlphaUpdate(const RE::NiAlphaProperty* a_alpha, std::uint32_t a_fields)
	{
		if (a_alpha)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureAlpha(*a_alpha) });
	}

	inline void PushGeometryUpdate(const RE::BSGeometry* a_geometry, std::uint32_t a_fields)
	{
		if (a_geometry)
			SceneTracker::Get().PushUpdate(SceneCapture::Update{ a_fields, SceneCapture::CaptureGeometry(*a_geometry) });
	}

	/**
	 * @brief Pushes a geometry whose property or alpha was swapped, applied as a capture. Only in the world: outside it the pins could
	 * hold an object no owner references yet, such as the projectile pool's master clone at refcount 0, and releasing it would free it.
	 */
	inline void PushLeafUpdate(const RE::BSGeometry& a_geometry)
	{
		if (!SceneCapture::InWorld(&a_geometry))
			return;
		using G = SceneCapture::GeometryRecord;
		// A capture too, numbered at its start.
		const auto sequence = SceneCapture::NextSequence();
		SceneCapture::Update update{ G::kProperty | G::kAlpha | G::kLayer, SceneCapture::CaptureGeometry(a_geometry) };
		auto leaf = std::make_shared<SceneCapture::Records>();
		// Its properties, held by the event.
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		SceneCapture::CaptureLeaf(a_geometry, *leaf, &pins);
		leaf->sequence = sequence;
		update.leaf = std::move(leaf);
		update.sequence = sequence;
		SceneTracker::Get().PushUpdate(std::move(update), std::move(pins));
	}

	// A fade node's statics: near, far, +0x109 and the LOD type.
	constexpr std::uint32_t kFadeStatics = SceneCapture::NodeRecord::kFadeNear | SceneCapture::NodeRecord::kFadeFar |
	                                       SceneCapture::NodeRecord::kFade109 | SceneCapture::NodeRecord::kFadeType;
	// Its per-frame state: currentFade, the LOD level and the screen-door byte.
	constexpr std::uint32_t kFadeState = SceneCapture::NodeRecord::kFadeCurrent | SceneCapture::NodeRecord::kFadeLevel | SceneCapture::NodeRecord::kFadeDoor;

	inline float CurrentFade(RE::BSFadeNode* a_node)
	{
		return a_node->GetRuntimeData().currentFade;
	}

	/**
	 * @brief Structural events, pushed on any thread and drained by the scene pass. Property events are keys only, never dereferenced.
	 * Node events hold a reference, because the drain walks the node's subtree and ancestors.
	 */
	inline RenderGraph::Frame::EventQueue<const void*> propertyEvents{ &WakeScenePropertyEvent };
	// No wake, since Havok writes movers' transforms every frame. The next pass takes them.
	inline RenderGraph::Frame::EventQueue<RE::NiPointer<RE::NiAVObject>> nodeEvents;

	inline void PushProperty(const void* a_property) { propertyEvents.Push(a_property); }

	inline void PushNode(RE::NiAVObject* a_node)
	{
		if (a_node)
			nodeEvents.Push(RE::NiPointer<RE::NiAVObject>(a_node));
	}

	/**
	 * @brief Lighting properties whose LOD fades (+0x100, +0x104) changed. GetRenderPasses writes them whenever any view registers
	 * the object, possibly after the patch was sampled. Pushed from cull and accumulation jobs.
	 */
	// Per frame, so no wake.
	inline RenderGraph::Frame::EventQueue<const void*> lodFadeEvents;
	inline bool lodFadeEventsInstalled = false;

	inline void DrainLodFadeEvents(std::vector<const void*>& a_out)
	{
		lodFadeEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	/**
	 * @brief A shared colour that external emittance points at changed: a light's, a region's or the sky's.
	 * Both updates write through Sky::SetColor (0x14040d970), whose detour pushes on change.
	 */
	// Per frame, so no wake.
	inline RenderGraph::Frame::EventQueue<const void*> emittanceEvents;

	inline void DrainEmittanceEvents(std::vector<const void*>& a_out)
	{
		emittanceEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	inline void DrainPropertyEvents(std::vector<const void*>& a_out)
	{
		propertyEvents.Drain([&](const void* a_key) { a_out.push_back(a_key); });
	}

	inline void DrainNodeEvents(std::vector<RE::NiPointer<RE::NiAVObject>>& a_out)
	{
		nodeEvents.Drain([&](RE::NiPointer<RE::NiAVObject>&& a_node) { a_out.push_back(std::move(a_node)); });
	}

	/**
	 * @brief Keys of a reference whose 3D an update pass moved, a category node whose subtree one did, or a particle system with a new
	 * snapshot. Movers are placed every frame regardless. The keys gate the scene pass's light path and which roots the plan lists.
	 * Writers include the animation paths, TESObjectREFR::Update3DPosition, cell update passes, Projectile::Update3D and reserved
	 * feature writers. Pushed after the write from the animation and cell jobs.
	 */
	// Thousands a frame, so no wake.
	inline RenderGraph::Frame::EventQueue<const void*> moveEvents;
	// Calls per writer, for the report.
	inline std::array<std::atomic<std::uint64_t>, Extensions::kMoveWriterSlots> moveWriterCalls{};
	// Core's writers at their slots. Empty slots belong to reserved writers, named at their reservation.
	inline std::array<std::string_view, Extensions::kMoveWriterSlots> moveWriterNames{ "animation", "reference 3D position", {}, {}, {}, "graph animation",
		"update pass", {}, {}, "projectile 3D" };
	inline void CountMove(std::size_t a_writer) { moveWriterCalls[a_writer].fetch_add(1, std::memory_order_relaxed); }

	/**
	 * @brief A node whose kHidden bit a patched store changed, riding in its mirror update. The source is Features/HiddenNodes.
	 * Without it the walk retakes every verdict each frame, and neither the LOD gates nor the kept scene lists engage.
	 */
	struct HiddenEvent
	{
		const void* key = nullptr;
		std::uint32_t site = 0;  // the patched store's index
		bool hidden = false;  // the bit's value after the store
	};
	inline bool hiddenEventsInstalled = false;
	/**
	 * @brief On the storing thread after the store. Pushes the mirror's bit and the hidden event in one update, so both land in the same batch.
	 */
	void PushHiddenUpdate(const void* a_object, std::uint32_t a_site);

	inline void PushMove(const void* a_key)
	{
		if (a_key)
			moveEvents.Push(a_key);
	}

	/**
	 * @brief An NiSwitchNode's selection may have changed. NiSwitchNode::index (+0x12C) has no setter. Outside construction its only
	 * stores (AE 1.6.1170) are BSTreeManager's LOD selection, the local map's, and harvesting. All go through SWF_EVENT_SWITCH_INDEX.
	 * The tree manager can store twice in a pass, so events are judged by net change.
	 * AttachChild, DetachChild and SetAt leave the selected child stale without an index store, so they push a structural event.
	 */
	struct SwitchEvent
	{
		RE::NiPointer<RE::NiAVObject> node;
		std::int32_t before = 0;
		bool structural = false;
	};
	inline RenderGraph::Frame::EventQueue<SwitchEvent> switchEvents{ &WakeSceneSwitchEvent };
	constexpr std::size_t kSwitchIndex = 0x12C;

	inline std::int32_t& SwitchIndexOf(RE::NiAVObject* a_switch)
	{
		return *reinterpret_cast<std::int32_t*>(reinterpret_cast<std::byte*>(a_switch) + kSwitchIndex);
	}

	// The render thread, recorded at every Present and frame start. Switch events are taken only there, because a reference to a
	// node a loader is still assembling is unsafe (a QueuedTree load crashed on a freed child under a tree's switch).
	inline std::atomic<std::uint32_t> switchEventThread{ 0 };

	inline void PushSwitch(RE::NiAVObject* a_switch, std::int32_t a_before, bool a_structural)
	{
		if (a_switch && ::GetCurrentThreadId() == switchEventThread.load(std::memory_order_relaxed))
			switchEvents.Push(SwitchEvent{ RE::NiPointer<RE::NiAVObject>(a_switch), a_before, a_structural });
	}

	/** @brief After one of NiSwitchNode's own child edits. */
	inline void PushSwitchStructural(RE::NiNode* a_switch)
	{
		PushNodeUpdate(a_switch, SceneCapture::NodeRecord::kSwitch);
		PushSwitch(a_switch, SwitchIndexOf(a_switch), true);
	}

	inline bool switchEventsInstalled = false;
	inline bool moveEventsInstalled = false;
	inline std::uint32_t moveWritersPending = 0;

	/** @brief Whether Core's and every reserved move writer are hooked. */
	inline bool MoveEventsLive() { return moveEventsInstalled && moveWritersPending == 0; }
	/** @brief Whether the switch events are installed. False when the engine's writers are not the expected code. */
	inline bool SwitchEventsLive() { return switchEventsInstalled; }
}
