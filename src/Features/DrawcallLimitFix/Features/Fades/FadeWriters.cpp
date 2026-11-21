// Fade writer hooks. Threads involved: the main thread at Main::Update's entry, animation job threads,
// cull threads in the OnVisible detours, and the render thread when the list jobs end.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"  // WriteFadeStores
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRoots.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesAddresses.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "RenderGraph/Async/EventQueue.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <utility>

namespace DCLF::Features::Fades
{
	namespace
	{
		using namespace Engine;

		// Field offsets are the same on SE and AE. These are fields BSFadeNode::OnVisible (AE 0x141479f50) reads.
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::uint32_t kFlagFadeTargetReached = 1u << 14;
		constexpr DCLF::Engine::Offset kAnimatedFadeCallSite{ { 19384, 0xe4 }, { 18972, 0xe4 } };  // FUN_1402cff60 (UpdateAnimationJob) -> FUN_14147a160
		constexpr DCLF::Engine::Offset kNodeOnVisible{ { 70307 }, { 68956 } };  // NiNode::OnVisible
		constexpr std::size_t kNodeWorldZ = 0xEC;                 // NiAVObject::world.translate.z
		constexpr std::size_t kProcessCameraUpdates = 0x11C;      // byte: NiCullingProcess::cameraRelatedUpdates
		constexpr std::size_t kFadeLodType = 0x153;               // byte, LOD type in the low nibble
		constexpr std::size_t kFadeNear = 0x128;                  // float, on the fade node
		constexpr std::size_t kFadeFar = 0x12C;                   // float
		constexpr std::size_t kFadeAlwaysOut = 0x109;             // byte, bit 0 fades the node out at any distance

		// Lock-free queue of the animation job's fade updates. The inputs are the same for every call in one animation update.
		// The first caller claims and publishes them. Later callers compare and set animatedVaried if they differ.
		RenderGraph::Frame::EventQueue<AnimatedFadeRoot> animatedNodes;
		std::vector<AnimatedFadeRoot> animatedBatch;  // render thread
		enum : std::uint32_t
		{
			kAnimatedEmpty,
			kAnimatedWriting,
			kAnimatedPublished
		};
		std::atomic<std::uint32_t> animatedState{ kAnimatedEmpty };
		AnimatedFadeInputs animatedInputs{};  // written by the claimer only, read once published
		std::atomic<std::uint32_t> animatedVaried{ 0 };
		RenderGraph::Frame::EventQueue<AnimatedFadeInputs, 64> animatedUnpublished;  // inputs seen before the claimer published, compared at drain
		AnimatedFadeInputs batchInputs{};  // render thread, or the CPU stage's producer while a drain is handed off
		std::atomic<bool> animatedDrainHanded{ false };
		void EndAnimatedDrain()
		{
			animatedDrainHanded.store(false, std::memory_order_release);
			animatedDrainHanded.notify_all();
		}
		// Immutable tables published at Main::Update's entry and held until the next entry, when no OnVisible or gate runs.
		std::atomic<const void*> fadeWriterTables{ nullptr };
		std::shared_ptr<const void> fadeWriterTablesHeld;
		std::atomic<std::uint64_t> onVisibleFadeSkipped{ 0 };
		std::atomic<std::uint64_t> gateFadeSkipped{ 0 };

		/** @brief Null before the family's first write. */
		const Scene::FadeRootTables* RootsOf(const void* a_tables)
		{
			return static_cast<const SceneTables*>(a_tables)->families.Columns<Scene::FadeRootTables>(Scene::FadeRootFamilyId());
		}

		void PublishWriterTables()
		{
			// Safe to release the old tables here because the last job cycle has finished, so nothing still reads them.
			std::shared_ptr<const void> tables = Scene::FrameView::Get().AcceptedTables();
			fadeWriterTables.store(tables.get(), std::memory_order_release);
			fadeWriterTablesHeld = std::move(tables);
		}

		/** @brief Queues the update and returns whether DCLF writes the root. */
		bool NoteAnimatedFade(const void* a_node, const float* a_camera)
		{
			// An unlisted node is left to the engine. If it is listed later, its seed already includes this update.
			bool written = false;
			if (const auto* tables = fadeWriterTables.load(std::memory_order_acquire)) {
				if (const auto* roots = RootsOf(tables)) {
					if (const auto it = roots->fadeRootIndex.find(a_node); it != roots->fadeRootIndex.end() && it->second < roots->fadeRoots.size()) {
						const auto& row = roots->fadeRoots[it->second];
						written = (row.bits & kFadeRootWritten) != 0;
						animatedNodes.Push({ a_node, it->second, row.generation });
					}
				}
			}
			// a_camera is the world root camera's position and lodAdjust.
			if (!a_camera)
				return written;
			const AnimatedFadeInputs inputs{ { a_camera[0], a_camera[1], a_camera[2] }, a_camera[3], Global<std::int32_t>(Address::kFadeFrameCounter), Global<float>(Address::kFadeStepTime), 1u };
			std::uint32_t state = animatedState.load(std::memory_order_acquire);
			if (state == kAnimatedEmpty && animatedState.compare_exchange_strong(state, kAnimatedWriting, std::memory_order_acquire)) {
				animatedInputs = inputs;
				animatedState.store(kAnimatedPublished, std::memory_order_release);
				return written;
			}
			if (state == kAnimatedPublished) {
				if (std::memcmp(&inputs, &animatedInputs, offsetof(AnimatedFadeInputs, valid)) != 0)
					animatedVaried.store(1, std::memory_order_relaxed);
				return written;
			}
			// Claimed but not yet published. Compare at drain time.
			animatedUnpublished.Push(inputs);
			return written;
		}

		/**
		 * @brief FUN_1402cff60's call of FUN_14147a160. The animation job updates the fade of animated nodes the cull did not reach.
		 * FadeStateCS repeats it on the GPU, so the engine's store is skipped on DCLF-written roots.
		 */
		struct AnimatedFade
		{
			static void thunk(void* a_node, float a_amount, const float* a_camera)
			{
				if (NoteAnimatedFade(a_node, a_camera)) {
					gateFadeSkipped.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				func(a_node, a_amount, a_camera);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSFadeNode::OnVisible (AE 0x141479f50) with the same control flow but none of its stores.
		 * The fade update, the type-6 step and the +0x13C store are skipped.
		 */
		void WrittenFadeOnVisible(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
		{
			using NodeOnVisible = void (*)(void*, RE::NiCullingProcess*, std::int32_t);
			static const auto children = reinterpret_cast<NodeOnVisible>(REL::Module::get().base() + kNodeOnVisible);
			const float one = Global<float>(Address::kFloatOne);
			const float amount = At<float>(a_node, kFadeAmount);
			const bool settled = (At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadeSettled) && amount == one && At<float>(a_node, kCurrentFade) == one;
			if (!Global<std::uint8_t>(Engine::Address::kFadesOn) || settled) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<std::uint8_t>(a_process, kProcessCameraUpdates) && (At<std::uint8_t>(a_node, kFadeLodType) & 0xF) == 6 &&
				!(Global<float>(Engine::Address::kFadeSpecialA) != Global<float>(Engine::Address::kFadeSpecialB))) {
				children(a_node, a_process, a_arg);
				return;
			}
			if (At<float>(a_node, kCurrentFade) <= 0.0f || amount == 0.0f)
				return;
			children(a_node, a_process, a_arg);
		}

		struct FadeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSLeafAnimNode::OnVisible (AE 0x14147c9c0). Skips the leaf LOD step and the +0x13C store. */
		struct LeafOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSTreeNode::OnVisible (AE 0x14147d3c0). Keeps the height test and skips the LOD stores at +0x153 and +0x14C. */
		struct TreeOnVisible
		{
			static void thunk(void* a_node, RE::NiCullingProcess* a_process, std::int32_t a_arg)
			{
				if (!FadeWrittenByDclf(a_node)) {
					func(a_node, a_process, a_arg);
					return;
				}
				onVisibleFadeSkipped.fetch_add(1, std::memory_order_relaxed);
				if (At<std::uint8_t>(a_process, Address::kProcessTreeHeightTest) && Global<std::uint8_t>(Address::kTreeHeightTestOn) &&
					At<float>(a_node, kNodeWorldZ) - Global<float>(Address::kTreeHeightBase) > Global<float>(Address::kTreeHeightLimit))
					return;
				WrittenFadeOnVisible(a_node, a_process, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief All or none. A root whose engine writer still ran would have two writers. */
		void InstallWriterHooks()
		{
			const auto base = REL::Module::get().base();
			stl::write_thunk_call<AnimatedFade>(base + kAnimatedFadeCallSite);
			// Detours rather than vtable patches, so subclass vtables and FadeState::PlanOf still see the engine's addresses.
			stl::detour_thunk<FadeOnVisible>(base + Address::kFadeOnVisible);
			stl::detour_thunk<LeafOnVisible>(base + Address::kLeafOnVisible);
			stl::detour_thunk<TreeOnVisible>(base + Engine::Address::kTreeOnVisible);
			HookSites::NoteDetour("BSFadeNode::OnVisible's detour", FadeOnVisible::func.address());
			HookSites::NoteDetour("BSLeafAnimNode::OnVisible's detour", LeafOnVisible::func.address());
			HookSites::NoteDetour("BSTreeNode::OnVisible's detour", TreeOnVisible::func.address());
		}
	}

	void PreflightWriters()
	{
		const auto base = REL::Module::get().base();
		HookSites::RequireCallTo("the animation job's fade update", base + kAnimatedFadeCallSite, base + Engine::Address::kFadeUpdate);
		HookSites::RequireKnown("the fade OnVisibles", Address::kFadeOnVisible.Known() && Address::kLeafOnVisible.Known() && Engine::Address::kTreeOnVisible.Known());
		if (Address::kFadeOnVisible.Known() && Address::kLeafOnVisible.Known() && Engine::Address::kTreeOnVisible.Known()) {
			HookSites::RequireEntry("BSFadeNode::OnVisible", base + Address::kFadeOnVisible);
			HookSites::RequireEntry("BSLeafAnimNode::OnVisible", base + Address::kLeafOnVisible);
			HookSites::RequireEntry("BSTreeNode::OnVisible", base + Engine::Address::kTreeOnVisible);
		}
	}

	void WriteFadeWriters()
	{
		// Main::Update's jobs start pushing again soon, so a drain handed off last frame must end first. It is normally long done.
		WaitAnimatedDrain();
		// Publish the writer set before writing, so the engine writers skip the same roots the write-back covers.
		PublishWriterTables();
		WriteFadeStores([](const void* a_root) { return FadeWrittenByDclf(a_root); });
	}

	bool FadeWrittenByDclf(const void* a_node)
	{
		const auto* tables = fadeWriterTables.load(std::memory_order_acquire);
		return tables && WrittenByTables(tables, a_node);
	}

	bool WrittenByTables(const void* a_tables, const void* a_node)
	{
		const auto* roots = RootsOf(a_tables);
		if (!roots)
			return false;
		const auto it = roots->fadeRootIndex.find(a_node);
		return it != roots->fadeRootIndex.end() && it->second < roots->fadeRoots.size() && (roots->fadeRoots[it->second].bits & kFadeRootWritten);
	}

	FadeWriterCounts TakeFadeWriterCounts()
	{
		FadeWriterCounts c;
		c.onVisibleSkipped = onVisibleFadeSkipped.exchange(0, std::memory_order_relaxed);
		c.gateSkipped = gateFadeSkipped.exchange(0, std::memory_order_relaxed);
		if (const auto* tables = fadeWriterTables.load(std::memory_order_acquire))
			if (const auto* roots = RootsOf(tables))
				c.roots = roots->fadeRootIndex.size();
		return c;
	}

	FadeWriterCounts PeekFadeWriterCounts()
	{
		FadeWriterCounts c;
		c.onVisibleSkipped = onVisibleFadeSkipped.load(std::memory_order_relaxed);
		c.gateSkipped = gateFadeSkipped.load(std::memory_order_relaxed);
		if (const auto* tables = fadeWriterTables.load(std::memory_order_acquire))
			if (const auto* roots = RootsOf(tables))
				c.roots = roots->fadeRootIndex.size();
		return c;
	}

	// No hand-off check. Used by the producer, which holds the hand-off.
	std::size_t DrainAnimatedFadesNow();

	void WaitAnimatedDrain()
	{
		if (!animatedDrainHanded.load(std::memory_order_acquire))
			return;
		DCLF_ZONE("CS.DCLF.Fade.WaitAnimatedDrain");
		while (animatedDrainHanded.load(std::memory_order_acquire))
			animatedDrainHanded.wait(true, std::memory_order_acquire);
	}

	void HandAnimatedDrainToWorker()
	{
		WaitAnimatedDrain();
		animatedDrainHanded.store(true, std::memory_order_release);
	}

	void TakeAnimatedBatchOnWorker(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs)
	{
		DrainAnimatedFadesNow();
		a_roots.assign(animatedBatch.begin(), animatedBatch.end());
		animatedBatch.clear();
		a_inputs = std::exchange(batchInputs, {});
		EndAnimatedDrain();
	}

	void DropAnimatedDrain()
	{
		DrainAnimatedFadesNow();
		animatedBatch.clear();
		batchInputs = {};
		EndAnimatedDrain();
	}

	void TakeAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs)
	{
		WaitAnimatedDrain();
		// Copied so the batch keeps its storage for the next drain.
		a_roots.assign(animatedBatch.begin(), animatedBatch.end());
		animatedBatch.clear();
		a_inputs = std::exchange(batchInputs, {});
	}

	namespace
	{
		std::vector<AnimatedFadeRoot> observerBatch;  // render thread
		AnimatedFadeInputs observerInputs{};
	}

	void KeepAnimatedBatchForObserver(const std::vector<AnimatedFadeRoot>& a_roots, const AnimatedFadeInputs& a_inputs)
	{
		observerBatch.assign(a_roots.begin(), a_roots.end());
		observerInputs = a_inputs;
	}

	void TakeObserverAnimatedBatch(std::vector<AnimatedFadeRoot>& a_roots, AnimatedFadeInputs& a_inputs)
	{
		a_roots.swap(observerBatch);
		observerBatch.clear();
		a_inputs = std::exchange(observerInputs, {});
	}

	void DrainAnimatedFades()
	{
		// The producer owns the queue during a hand-off. Nothing is pushed before the list jobs end, so skipping loses nothing.
		if (animatedDrainHanded.load(std::memory_order_acquire))
			return;
		DrainAnimatedFadesNow();
	}

	std::size_t DrainAnimatedFadesNow()
	{
		// The animation job runs between two culls, so its updates are complete by now.
		animatedBatch.clear();
		const std::size_t drained = animatedNodes.Drain([&](AnimatedFadeRoot&& a_root) { animatedBatch.push_back(a_root); });
		batchInputs = {};
		if (animatedState.load(std::memory_order_acquire) == kAnimatedPublished) {
			batchInputs = animatedInputs;
			batchInputs.varied = animatedVaried.load(std::memory_order_relaxed);
		}
		animatedUnpublished.Drain([&](AnimatedFadeInputs&& a_inputs) {
			if (batchInputs.valid && std::memcmp(&a_inputs, &batchInputs, offsetof(AnimatedFadeInputs, valid)) != 0)
				batchInputs.varied = 1;
		});
		animatedVaried.store(0, std::memory_order_relaxed);
		animatedState.store(kAnimatedEmpty, std::memory_order_release);
		return drained;
	}

	float FadeDistanceOf(const SceneCapture::NodeRecord& a_root)
	{
		// Reads captured globals, never live engine memory.
		const auto& g = FrameGlobals::Current();
		// The fade value has no distance term here (FUN_14147b110 returns 1).
		if (!g.fadesOn || !g.fadeLodUpdates)
			return 0.0f;
		const std::uint32_t type = a_root.fadeType;
		// Type 8 fades even out of view, and the type-6 branch never fades out. The engine draws both as soon as they come into view.
		if (type == 8 || (type == 6 && g.fadeSpecialA == g.fadeSpecialB))
			return 0.0f;
		const float mult = g.fadeDistanceMult;
		const float nearDistance = mult * a_root.fadeNear;
		const float farDistance = mult * a_root.fadeFar;
		if (!(farDistance > nearDistance))
			return 0.0f;  // the fade value never falls below 1
		// The fade value 1 - (x - near) / (far - near), x the scaled distance, reaches the threshold at x = limit.
		const float limit = nearDistance + (1.0f - g.fadeOutThreshold) * (farDistance - nearDistance);
		const float divisor = g.fadeTypeDivisors[type];
		// x = distance * lodFactor / divisor, or distance * the default scale when the divisor is not positive.
		const float distance = divisor > 0.0f ? limit * divisor : -(limit / g.fadeDefaultScale);
		return std::isfinite(distance) ? distance : 0.0f;
	}

	void RegisterWriters(Api::InstallContext& a_context)
	{
		// Must be the last observer, so the writer tables publish after the other observers' updateEntry.
		// Without the primary cull no hooks go in.
		if (!a_context.RegisterStandInObserver({ .updateEntry = &WriteFadeWriters, .listJobsDone = &DrainAnimatedFades }))
			return;
		InstallWriterHooks();
	}
}
