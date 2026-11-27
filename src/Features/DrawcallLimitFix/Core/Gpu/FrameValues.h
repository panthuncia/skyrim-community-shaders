#pragma once

#include "RenderGraph/BufferRef.h"

#include "Features/DrawcallLimitFix/Api/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <optional>
#include <vector>

#include <rhi.h>

namespace org
{
	class VersionedBuffer;
	class RenderPassBuilder;
}

namespace org::services
{
	struct Backing;
}

namespace RenderGraph
{
	struct DeclaredRefs;
}

namespace DCLF
{
	struct FrameGlobals;
	struct BindlessPlacement;
	struct BindlessShading;
	namespace LatePoses
	{
		struct FramePlan;
	}
	namespace Direct
	{
		struct ViewBatchSet;
		struct SnapshotSpheres;
		struct ForwardSortRow;
	}

	/**
	 * @brief Makes the per-frame values a frame's draws read: placements, skin palettes and shading rows.
	 *
	 * Two kinds of job run on DCLF's pool, one after another in kick order (one chain). The game half kicks a sampling job per ticket:
	 * it applies the new placement plans to the set it keeps, samples only what moved under engine-read leases, and runs the game-side
	 * pools' samples. The render half kicks an upload job per render: it reads only the sampled rows, fills the render's CPU slot (forward
	 * sort rows, live spheres), runs the per-render pools, and uploads the changed rows on the dedicated upload queue. It then signals
	 * the frame-wait timeline, always, even after a failure. The GPU waits for that signal and the render thread never does. Nothing
	 * the render half kicks takes an engine-read lease.
	 *
	 * Each row set lives in pooled backings. A backing is bound to the frame's last submission, so no producer writes a backing a
	 * frame in flight still reads. Feature-registered pools run after the built-in row sets at every step.
	 */
	class FrameValues
	{
	public:
		/** @brief One float4 of a palette. A bone takes three. */
		struct PaletteRow
		{
			float v[4];
		};

		static FrameValues& Get();

		/** @brief The first-person members' rows and palettes, sampled by the game half. KickSample hands them to its job. */
		struct RootViews;

		/** @brief An object's placement row read from the engine now. Without a listed root the bound is unbounded. */
		static void SampleRow(const RE::BSGeometry& a_geometry, const RE::NiAVObject* a_listedRoot, const RE::BSShaderProperty* a_fadeProperty,
			BindlessPlacement& a_out);
		/** @brief Diagnostics, render thread. A slot's row read from the engine now. False for a free slot. */
		static bool SampleSlot(const SceneTables& a_tables, std::uint32_t a_slot, BindlessPlacement& a_out);

		/**
		 * @brief Game half, once per ticket (rendered or not). Runs the pools' frame starts and the game-side pools' kicks, and kicks the
		 * ticket's sampling job with its one-shot inputs. Without a graph or dedicated upload queue the inputs wait for the next one.
		 */
		void KickSample(std::vector<std::shared_ptr<const PlacementPlan>> a_plans, std::vector<ShadingItem> a_shading,
			std::vector<ShadingFieldValue> a_fieldValues, std::shared_ptr<const FrameGlobals> a_globals, std::shared_ptr<RootViews> a_rootViews);
		/**
		 * @brief Render thread, at the scene's start, each render. Leases the render's backings, kicks its upload job after every sampling
		 * job kicked before it, and sets the host's frame wait to its sequence number. Returns false when there is no graph or dedicated
		 * upload queue. The frame then waits for nothing.
		 */
		bool Kick(std::shared_ptr<const FrameGlobals> a_globals);
		/** @brief Render thread, before Kick. Kick also leases late-pose placement and palette backings, filled by the same producer. */
		void SetLatePoses(std::shared_ptr<const LatePoses::FramePlan> a_plan);
		/** @brief Render thread, at the depth commit. Draws from here read the late backings. The shadow epoch keeps the first ones. */
		void UseLatePoses();
		/**
		 * @brief Game half of the frame start, before Main::Draw moves the skeleton into its root's space (FUN_1414cb020). Applies
		 * a_plans to the first-person members and samples those a_claims holds (none when null). The producer never samples them.
		 */
		std::shared_ptr<RootViews> TakeRootViews(const std::vector<std::shared_ptr<const PlacementPlan>>& a_plans, const SetSnapshot* a_claims);
		/**
		 * @brief Render thread, before Kick. The CPU pipeline's view batches, kept until replaced. Producers fill their live sphere column.
		 * The next producer also fills a_forwardRows, the frame slot's forward sort rows, from the Forward and Refraction lists.
		 */
		void SetViewBatches(std::shared_ptr<const Direct::ViewBatchSet> a_set, std::vector<Direct::ForwardSortRow>* a_forwardRows = nullptr)
		{
			viewBatches = std::move(a_set);
			forwardRows = a_forwardRows;
		}
		/** @brief Render thread, once RowsIfDone gave rows. Whether the newest kick's producer filled a_target's forward sort rows. */
		bool ForwardRowsFilled(const std::vector<Direct::ForwardSortRow>* a_target) const;
		/** @brief Any thread. Asks the next producer for every slot's bound. Costly, so made only when asked. Latest wins. */
		void RequestSnapshotSpheres();
		/** @brief The newest bounds posted since the last take, or null. */
		std::unique_ptr<Direct::SnapshotSpheres> TakeSnapshotSpheres();
		/** @brief Any thread. Like RequestSnapshotSpheres, in its own slot for texture streaming. */
		void RequestStreamingSpheres();
		/** @brief The newest streaming bounds since the last take, or null. */
		std::unique_ptr<Direct::SnapshotSpheres> TakeStreamingSpheres();
		/**
		 * @brief First-person particle writes since the last call. The farthest distance from the first-person eye flags a snapshot taken
		 * while the skeleton was in its root's space.
		 */
		struct FirstPersonPaletteStats
		{
			std::uint64_t writes = 0, units = 0;
			float farthest = 0.0f;
		};
		static FirstPersonPaletteStats TakeFirstPersonPaletteStats();
		/** @brief Render thread, before Present. Waits until the last producer's signal is on the copy queue. See the .cpp for why. */
		void WaitSignalSubmitted();
		/** @brief Render thread, after the frame's last submission. Binds the frame's backings to it. */
		void EndFrame();
		/**
		 * @brief Render thread, after Kick. The timeline point the frame's GPU work must wait for, or 0 without a producer.
		 * Work outside ORG's batches must take it at its trigger, before the next Kick moves it.
		 */
		rhi::TimelinePoint FrameWaitPoint() const;
		/** @brief The row sets a pass reads, as bits. */
		enum ReadSets : std::uint32_t
		{
			kReadPlacements = 1u << 0,
			kReadPalettes = 1u << 1,
			kReadShading = 1u << 2,
			kReadPools = 1u << 3,
			kReadAll = kReadPlacements | kReadPalettes | kReadShading | kReadPools,
		};
		/**
		 * @brief Any thread, in a pass's declaration. Declares the frame's value buffers the pass reads.
		 * With a scoped frame wait only declaring batches wait, so every pass that reads a row set must declare it.
		 */
		void DeclareReads(org::RenderPassBuilder& a_builder, std::uint32_t a_sets = kReadAll);
		/** @brief Any thread, in a pass's declaration. Declares a registered pool written through PoolRef, checked again in Prepare. */
		void DeclarePoolWrite(org::RenderPassBuilder& a_builder, Api::FrameValuePoolId a_pool, std::uint32_t a_sets, RenderGraph::DeclaredRefs& a_out);
		/** @brief Render thread. The placement buffer's descriptor for the current frame, 0 before the first. */
		std::uint32_t PlacementsIndex() const { return frameIndex; }
		/** @brief Render thread. The palette buffer's descriptor for the current frame, 0 before the first. */
		std::uint32_t PalettesIndex() const { return paletteIndex; }
		/** @brief Render thread. The shading buffer's descriptor for the current frame, 0 before the first. */
		std::uint32_t ShadingIndex() const { return shadingIndex; }
		/** @brief Render thread. A registered pool's backing descriptor this frame, 0 when there is none. */
		std::uint32_t PoolIndex(Api::FrameValuePoolId a_pool) const;
		/** @brief Render thread. The device address of that backing, 0 for pools without one. */
		std::uint64_t PoolAddress(Api::FrameValuePoolId a_pool) const;
		/** @brief Render thread. The buffer view ref shaders use for that backing. Writers must declare it with DeclarePoolWrite. */
		RenderGraph::BufferRef PoolRef(Api::FrameValuePoolId a_pool) const;
		/** @brief Render thread. That backing's size in bytes. */
		std::uint64_t PoolBytes(Api::FrameValuePoolId a_pool) const;
		/** @brief Diagnostics, render thread. That backing, held by the frame until EndFrame. */
		std::shared_ptr<const org::services::Backing> PoolBacking(Api::FrameValuePoolId a_pool) const;
		/** @brief Render thread. Whether Kick can make values now, which needs a graph host with a dedicated upload queue. */
		bool Available() const;
		/** @brief Diagnostics, render thread. Whether the frame's producer is done, optionally waiting for it. */
		bool ProducerDone(bool a_wait = false) const;
		/**
		 * @brief Any thread. Returns once the newest sampling job has finished reading engine memory.
		 * Engine writers inside the window call this. Uploads may still run.
		 */
		void WaitSampled() const;
		/** @brief A pool's sample counts its refused engine-read lease here. */
		void NoteRefused();

		/** @brief Diagnostics, render thread. The rows the producer left, or null while it runs. Valid until the next Kick. */
		const std::vector<BindlessPlacement>* RowsIfDone(bool a_wait = false) const;
		/** @brief Render thread. The palette rows, likewise. */
		const std::vector<PaletteRow>* PalettesIfDone(bool a_wait = false) const;
		/** @brief Render thread. The shading rows, likewise. */
		const std::vector<BindlessShading>* ShadingIfDone(bool a_wait = false) const;
		/** @brief The producer's counters since the last take. */
		struct Counters
		{
			std::uint64_t frames = 0, sampled = 0, settled = 0, seeded = 0, rowsChanged = 0, skins = 0, paletteRowsChanged = 0, shadingSampled = 0, shadingChanged = 0,
						  wetnessChanged = 0, runs = 0, bytes = 0, sampleUs = 0, uploadUs = 0, maxTotalUs = 0, refused = 0, defects = 0,
						  failures = 0;
			std::uint64_t movers = 0, moversDue = 0, roots = 0, rootsDue = 0, moveKeys = 0, moveNodes = 0;
			// Producers that reused a copy a frame in flight still bound. Their uploads waited for it on the GPU.
			std::uint64_t reusedInFlight = 0;
			std::uint64_t plans = 0, planResets = 0, planItems = 0;
		};
		/** @brief Takes and resets the counters. */
		Counters TakeCounters();
		/** @brief One row set's pool stats. */
		struct PoolStats
		{
			const char* name = "";
			std::uint64_t live = 0, leased = 0, free = 0, liveBytes = 0, allocated = 0, reused = 0, released = 0;
			// Since the last take.
			std::uint64_t acquires = 0, made = 0, runs = 0, bytes = 0;
			// The oldest content any backing holds, 0 if none.
			std::uint64_t floor = 0;
		};
		static constexpr std::size_t kBuiltInPools = 3;  // placements, palettes, shading
		/** @brief Any thread. Takes the pools' stats and resets the since-last-take counts. */
		std::vector<PoolStats> TakePoolStats();

	private:
		FrameValues();
		~FrameValues();
		void DeclareReadsExcept(org::RenderPassBuilder& a_builder, std::uint32_t a_sets, std::size_t a_skip);
		struct Impl;
		std::unique_ptr<Impl> impl;
		std::uint32_t frameIndex = 0, paletteIndex = 0, shadingIndex = 0;
		std::shared_ptr<const Direct::ViewBatchSet> viewBatches;
		std::vector<Direct::ForwardSortRow>* forwardRows = nullptr;
	};
}
