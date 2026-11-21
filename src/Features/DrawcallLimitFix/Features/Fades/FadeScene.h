#pragma once

// Fades' render side. FadeScene holds the per-scene GPU resources for fade roots.
// FadeWriteBack carries fade events read back from the GPU to the main thread, which writes them into engine nodes.

#include "Features/DrawcallLimitFix/Api/Feature.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Readback/Readbacks.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeLayout.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeRecords.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <Resources/Buffers/Buffer.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct SceneBuffers;
	}
}

namespace DCLF::Features::Fades
{
	struct FadeWriteBack;
	/** @brief Drops a dying write-back from the census. Any thread. */
	void ForgetWriteBackCensus(const FadeWriteBack* a_writeBack);

	/**
	 * @brief Host side of the fade write-back. Event batches reach the write-back task through a lock-free stack.
	 * A frame whose epoch did not run loses nothing, because FadeStateCS did not run either.
	 */
	struct FadeWriteBack
	{
		// The event list's readback. Its regions are the armed slots.
		std::shared_ptr<org::RunReadback> list;
		struct Batch
		{
			std::vector<FadeEvent> events;
			std::uint32_t frame = 0;  // scene frame whose FadeStateCS appended these events
			Batch* next = nullptr;
		};
		// Render thread only.
		struct Armed
		{
			bool armed = false;
			std::uint32_t frame = 0;
		};
		std::vector<Armed> armed;
		std::atomic<Batch*> batches{ nullptr };
		// Owned by the write-back task. Per root slot, the scene frame of the last applied event, so a late older list cannot overwrite it.
		std::vector<std::uint32_t> appliedFrame;
		std::atomic<bool> running{ false };
		/**
		 * @brief Stores a task judged, in event order, for the main thread to write. The task touches no engine memory.
		 * The retirement handle keeps the nodes alive until WriteFadeStores runs.
		 */
		struct Stores
		{
			struct Store
			{
				void* node = nullptr;
				std::uint32_t flags = 0;  // fade bits only (kFadeFlagMask)
				float fade = 0.0f;
				// LOD state of a leaf or tree root at +0x152, +0x153, +0x14C. Plain fade roots keep their own levels.
				bool lod = false;
				std::uint32_t levels = 0;
				float blend = 0.0f;
			};
			std::uint64_t publication = 0;          // oldest publication among the stores' tables
			std::shared_ptr<const void> retirement;  // the oldest publication's retirement node, which also holds every newer one
			std::shared_ptr<const SceneTables> tables;
			std::vector<Store> stores;
		};
		std::atomic<Stores*> ready{ nullptr };
		// Counters since the last report. notWritten counts roots the engine's cull owns, so DCLF must not also write them.
		std::atomic<std::uint64_t> applied{ 0 }, stale{ 0 }, busy{ 0 }, storeNs{ 0 }, notWritten{ 0 }, dropped{ 0 };
		std::atomic<std::uint64_t> harvested{ 0 }, harvestedWithEvents{ 0 }, events{ 0 };
		std::atomic<std::uint64_t> pendingBatches{ 0 }, readyStores{ 0 };

		void Push(Batch* a_batch)
		{
			pendingBatches.fetch_add(1, std::memory_order_relaxed);
			a_batch->next = batches.load(std::memory_order_relaxed);
			while (!batches.compare_exchange_weak(a_batch->next, a_batch, std::memory_order_release, std::memory_order_relaxed)) {}
		}
		~FadeWriteBack()
		{
			ForgetWriteBackCensus(this);
			delete ready.exchange(nullptr);
			for (Batch* b = batches.exchange(nullptr); b;) {
				Batch* following = b->next;
				delete b;
				b = following;
			}
		}
		static std::uint64_t BytesFor(std::uint32_t a_events) { return (kFadeEventHeaderWords + std::uint64_t(a_events) * kFadeEventWords) * sizeof(std::uint32_t); }
	};

	/** @brief Fades' per-scene resources, made with the scene's tables when the fade state program loads. */
	struct FadeScene
	{
		Draws::Versioned fadeRoots, fadeStates;
		std::shared_ptr<org::Buffer> fadeFrameBuffer;
		Draws::Versioned fadeRootPrograms;
		Draws::Versioned fadeAnimated;  // per root slot, the scene frame whose animation batch last updated it
		org::services::FilledVersion fadeAnimatedFilled;
		// CPU mirrors. Only runs that differ are uploaded.
		std::vector<std::uint32_t> fadeAnimatedMirror;
		std::vector<std::uint32_t> fadeAnimatedCounts;
		std::vector<std::uint32_t> fadeRootProgramsMirror;
		org::services::FilledContents<> fadeRootProgramsFilled;
		// Inputs the programs mirror was last judged against.
		struct ProgramsMirrorOf
		{
			std::uint64_t key = 0, rootsEpoch = 0, programs = 0;
		} fadeRootProgramsMirrorOf;
		// Kept between commits to reuse the allocations.
		struct RootProgramsBatch
		{
			std::vector<std::uint32_t> roots, asked, programs;
		} fadeRootProgramsBatch;
		// Indexed by frame parity. Every version is zero-filled, so builds fall back to the static row's state.
		std::array<Draws::Versioned, 2> fadeStatesOut;
		/** @brief The states a frame's builds read, which are the previous frame's. */
		const Draws::Versioned& FadeStatesRead(std::uint32_t a_frame) const { return fadeStatesOut[(a_frame + 1) & 1]; }
		org::services::FilledContents<> fadeRootsFilled;
		/** @brief Content epoch of the fadeRoots version named in a_sealed, or ~0 if none. */
		std::uint64_t FadeRootsContentEpoch(const Draws::NamedVersions& a_sealed) const { return fadeRootsFilled.Since(a_sealed.Of(fadeRoots)).value_or(~0ull); }
		std::uint32_t fadeRootCount = 0;
		std::uint32_t fadeFrameNumber = 0;  // the scene frame of fadeFrame
		FadeFrame fadeFrame{};
		std::shared_ptr<const ComputeProgram> fadeState;
		std::shared_ptr<const ComputeProgram> portalQueries;
		std::shared_ptr<Portals::Batch> portalBatch;
		// Every depth commit zeroes the event count. fadeReported is zero-filled, and a zero generation never appears in a listing.
		Draws::Versioned fadeEvents, fadeReported;
		std::shared_ptr<FadeWriteBack> fadeWriteBack;
	};

	/** @brief Write-back state for the diagnostic capture. Counters run since the last report. */
	struct WriteBackCensus
	{
		bool present = false;  // false if no write-back was kicked yet or its scene went away
		std::uint64_t applied = 0, stale = 0, busy = 0, notWritten = 0, dropped = 0, harvested = 0, harvestedWithEvents = 0, events = 0;
		std::uint64_t overflowed = 0, missed = 0, setsAdded = 0;
		std::uint64_t pendingBatches = 0, readyStores = 0;
		std::size_t appliedFrameSize = 0;
		std::vector<std::uint32_t> appliedFrame;  // filled only when asked
	};
	/** @brief Reads the last kicked write-back under the census lock. Any thread. */
	WriteBackCensus PeekWriteBackCensus(bool a_frames);

	/** @brief Seed copies waiting for a kick, and those a refused lease left behind. */
	struct SeedCounts
	{
		std::size_t unsent = 0, owed = 0;
	};
	/** @brief Any thread. */
	SeedCounts PeekSeedCounts();

	/** @brief Fades' resources in a_scene, or null if none were made. */
	FadeScene* SceneOf(const Draws::SceneBuffers& a_scene);
	/** @brief Like SceneOf, but shares ownership so a pass can hold it. */
	std::shared_ptr<FadeScene> SharedSceneOf(const Draws::SceneBuffers& a_scene);

	/**
	 * @brief Render thread. Descriptor of the fade seeds this frame reads, two rows per root slot, or 0 before the first frame.
	 * A seed is the root's node as it was at the start of the frame.
	 */
	std::uint32_t SeedsIndex();
	/** @brief The seed rows this producer just made. Producer thread only. */
	const std::vector<FadeRootStatic>& ProducerSeeds();

	/**
	 * @brief Render thread, at the start of the frame. Hands fade events read back since the last kick to a task for WriteFadeStores.
	 * Without it, engine readers such as tree LOD crossfade and the occlusion cull would see stale node fades.
	 */
	void KickFadeWriteBack();
	/**
	 * @brief Main thread, at Main::Update's entry while no cull or animation job runs. Writes the task's stores to roots DCLF owns.
	 * Roots a_written rejects are left to the engine's cull.
	 */
	void WriteFadeStores(const std::function<bool(const void*)>& a_written);

	// RegisterRender's parts, in the order FadeRender.cpp calls them.
	void RegisterSeeds(Api::InstallContext& a_context);
	void RegisterPixelRows(Api::InstallContext& a_context);
	void RegisterResources(Api::InstallContext& a_context);
	void RegisterCommit(Api::InstallContext& a_context);
	void RegisterPasses(Api::InstallContext& a_context);
	/** @brief The CPU pipeline's fade stage. It steps every root on the CPU in FadeStateCS's order. */
	void RegisterCpuStage(Api::InstallContext& a_context);
	/** @brief Render thread. Descriptor of the CPU stage's states for this frame, or 0 if none. */
	std::uint32_t CpuStatesIndex();
	/** @brief Pushes CPU stage events to the write-back the same way the GPU harvest does. Any thread. */
	void PostCpuFadeEvents(FadeWriteBack& a_writeBack, std::uint32_t a_frame, std::vector<FadeEvent> a_events);
}
