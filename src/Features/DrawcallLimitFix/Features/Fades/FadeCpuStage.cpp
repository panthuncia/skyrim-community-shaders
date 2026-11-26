// The CPU version of FadeStateCS, run as the "fade-states-cpu" frame-value pool.
// The kick gathers the frame's inputs on the render thread. The producer steps every root as FadeStateCS does,
// reports milestones to the write-back, and publishes the frame's CullFrame. The send uploads the states.
//
// Unlike FadeStateCS, which lags a frame, these states are the current frame's because the stage runs before the culls.
// Roots without a portal program use the engine's camera culling planes, not FadeStateCS's view-projection planes.
// The two agree except for rounding at the planes.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramsCpu.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Cull.h"
#include "Features/DrawcallLimitFix/Core/Render/Direct/Sites.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"

#include <Tracy/Tracy.hpp>
#include <Tracy/TracyC.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace DCLF::Features::Fades
{
	namespace
	{
		Api::FrameValuePoolId cpuStatesPool;
		constexpr std::uint32_t kNoObject = ~0u;  // matches FadeStateCS's kNoObject

		/** @brief Drops a handed-off animated drain if the producer never took it. */
		struct AnimatedDrainHold
		{
			bool pending = false;
			AnimatedDrainHold() = default;
			AnimatedDrainHold(const AnimatedDrainHold&) = delete;
			AnimatedDrainHold& operator=(const AnimatedDrainHold&) = delete;
			~AnimatedDrainHold()
			{
				if (pending)
					DropAnimatedDrain();
			}
		};

		/** @brief What a frame's kick hands its producer. It holds no engine references. */
		struct CpuStageInput
		{
			std::shared_ptr<const Direct::ViewBatchSet> set;
			std::uint32_t sceneFrame = 0;
			bool visibilityReset = false;
			std::uint64_t publication = 0;
			std::shared_ptr<const void> retirement;
			std::shared_ptr<const SceneTables> tables;
			std::shared_ptr<const EntrySpace> candidates;
			std::shared_ptr<FadeScene> scene;
			// The producer fills in the animated fields before reading them. Nothing else reads them.
			mutable FadeFrame frame{};
			ShadowCameras::Planes camera;  // no active planes when there is no camera
			std::array<float, 4> fadeEye{};
			std::array<float, 2> treeHeight{};
			mutable std::vector<AnimatedFadeRoot> animatedRoots;
			mutable AnimatedDrainHold animatedDrain;
			std::shared_ptr<const std::vector<std::uint32_t>> rootPrograms;  // by root slot
			std::vector<std::uint32_t> programs;
			// Set on GPU-driven frames. The stage then only reads and steps routedRoots. FadeStateCS stays in charge of fades.
			bool observer = false;
			// Made by the producer from set (RoutedRoots), off the render thread.
			mutable std::shared_ptr<const std::vector<std::uint32_t>> routedRoots;
		};

		// Render thread. Kept apart from FadeCommit.cpp's mirror of the same programs. Frames share the vector, so it is replaced, never edited.
		struct ProgramsMirror
		{
			std::shared_ptr<const std::vector<std::uint32_t>> programs = std::make_shared<std::vector<std::uint32_t>>();
			std::uint64_t key = 0, rootsEpoch = 0, inputVersion = 0;
			bool judged = false;
			std::vector<std::uint32_t> roots, asked, answers;  // kept to reuse the allocations
		};
		ProgramsMirror programsMirror;
		std::uint64_t stateRows = 0;
		std::shared_ptr<const CpuStageInput> idleInput = std::make_shared<CpuStageInput>();

		// Producer only, one at a time. reported holds each root's last reported generation and milestone.
		KeptWholeArray<FadeNodeState> cpuStates;
		std::vector<FadeNodeState> observerStates;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> reported;

		/** @brief Backing for a CullFrame's spans, recycled once no cull holds it. */
		struct FrameStorage
		{
			std::vector<std::array<float, 4>> sunEntry, fadeNode;
			std::vector<std::uint32_t> rootWords, rootGenerations, rootPrograms, programs;
			std::vector<FadeNodeState> rootStates;
			std::vector<FadeRootBound> rootBounds;
			FrameStorage* next = nullptr;
		};
		std::atomic<FrameStorage*> freeStorage{ nullptr };

		FrameStorage* TakeStorage()
		{
			FrameStorage* storage = freeStorage.load(std::memory_order_acquire);
			while (storage && !freeStorage.compare_exchange_weak(storage, storage->next, std::memory_order_acq_rel, std::memory_order_acquire)) {}
			return storage ? storage : new FrameStorage();
		}
		/** @brief Called by the last cull to release the storage. Any thread. */
		void GiveBackStorage(FrameStorage* a_storage)
		{
			a_storage->next = freeStorage.load(std::memory_order_relaxed);
			while (!freeStorage.compare_exchange_weak(a_storage->next, a_storage, std::memory_order_release, std::memory_order_relaxed)) {}
		}

		// ---- Kick (render thread) ----

		/** @brief Roots to judge again. Incrementally, those written since the last judging plus new ones. Otherwise all. */
		void CollectRoots(const SceneTables& a_tables, bool a_incremental, std::size_t a_before, std::size_t a_count, std::vector<std::uint32_t>& a_roots)
		{
			a_roots.clear();
			if (a_incremental) {
				if (const auto* family = Draws::RootFamilyOf(a_tables))
					family->Journal().Take().ForEachRun(programsMirror.rootsEpoch, a_before, [&](std::uint64_t a_first, std::uint64_t a_runs) {
						for (std::uint64_t r = a_first; r < a_first + a_runs && r < a_before; ++r)
							a_roots.push_back(static_cast<std::uint32_t>(r));
					});
				for (std::size_t r = a_before; r < a_count; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
				return;
			}
			for (std::size_t r = 0; r < a_count; ++r)
				a_roots.push_back(static_cast<std::uint32_t>(r));
		}

		/** @brief Each root's portal program. Rejudges only roots that can have changed. */
		std::shared_ptr<const std::vector<std::uint32_t>> RootPrograms(const SceneTables& a_tables)
		{
			auto& m = programsMirror;
			const auto* input = Extensions::MainCullInput();
			if (!input || !input->contribution.judge || !input->contribution.programsVersion)
				return m.programs;
			const auto list = Extensions::RootListOf(a_tables);
			const std::uint64_t inputVersion = input->contribution.programsVersion();
			const std::uint64_t key = list.contentEpoch * 0x9E3779B97F4A7C15ull ^ inputVersion;
			if (m.judged && m.key == key)
				return m.programs;
			const std::size_t count = list.nodes.size();
			auto programs = std::make_shared<std::vector<std::uint32_t>>(*m.programs);
			const bool incremental = m.judged && m.inputVersion == inputVersion && programs->size() <= count;
			if (!incremental)
				programs->clear();
			CollectRoots(a_tables, incremental, programs->size(), count, m.roots);
			m.asked.clear();
			m.answers.assign(m.roots.size(), input->contribution.noProgram);
			for (std::size_t k = 0; k < m.roots.size(); ++k)
				if (list.nodes[m.roots[k]])
					m.asked.push_back(static_cast<std::uint32_t>(k));
			if (!m.asked.empty()) {
				Api::RootProgramsJudge judge{ list.nodes, m.roots, m.asked, m.answers };
				input->contribution.judge(judge);
			}
			programs->resize(count, input->contribution.noProgram);
			for (std::size_t k = 0; k < m.roots.size(); ++k)
				(*programs)[m.roots[k]] = m.answers[k];
			m.programs = std::move(programs);
			m.key = key;
			m.rootsEpoch = list.contentEpoch;
			m.inputVersion = inputVersion;
			m.judged = true;
			return m.programs;
		}

		/** @brief An invalid batch means no updates. */
		void SetAnimated(const CpuStageInput& a_input, const AnimatedFadeInputs& anim)
		{
			auto& roots = a_input.animatedRoots;
			std::copy_n(anim.eye, 3, a_input.frame.animEye);
			a_input.frame.animLodAdjust = anim.lodAdjust;
			a_input.frame.animCounter = anim.counter;
			a_input.frame.animDeltaTime = anim.deltaTime;
			if (!anim.valid)
				roots.clear();
		}

		/**
		 * @brief Render thread. An observer leaves the batch to FadeStateCS and steps a copy. Otherwise the producer drains it,
		 * which keeps a copy of about 1,400 updates off the render thread.
		 */
		void TakeAnimated(CpuStageInput& a_input)
		{
			AnimatedFadeInputs anim{};
			TakeObserverAnimatedBatch(a_input.animatedRoots, anim);
			if (a_input.observer) {
				SetAnimated(a_input, anim);
				return;
			}
			a_input.animatedRoots.clear();
			HandAnimatedDrainToWorker();
			a_input.animatedDrain.pending = true;
		}

		/** @brief Producer. Must run before anything reads the animated fields. */
		void TakeAnimatedOnWorker(const CpuStageInput& a_input)
		{
			if (!a_input.animatedDrain.pending)
				return;
			DCLF_ZONE("CS.DCLF.FadeCpuStage.TakeAnimated");
			AnimatedFadeInputs anim{};
			TakeAnimatedBatchOnWorker(a_input.animatedRoots, anim);
			a_input.animatedDrain.pending = false;
			SetAnimated(a_input, anim);
			ZoneValue(a_input.animatedRoots.size());
		}

		/** @brief Sorted root slots the set's lanes name, cached per set. */
		std::shared_ptr<const std::vector<std::uint32_t>> RoutedRoots(const std::shared_ptr<const Direct::ViewBatchSet>& a_set)
		{
			static std::weak_ptr<const Direct::ViewBatchSet> madeFor;
			static std::shared_ptr<const std::vector<std::uint32_t>> made;
			if (made && madeFor.lock() == a_set)
				return made;
			auto roots = std::make_shared<std::vector<std::uint32_t>>();
			for (const auto& list : a_set->lists)
				for (const auto& batch : list)
					for (std::uint32_t lane = 0; batch.fadeRoot && lane < batch.count; ++lane)
						if (batch.fadeRoot[lane])
							roots->push_back(batch.fadeRoot[lane] - 1);
			std::sort(roots->begin(), roots->end());
			roots->erase(std::unique(roots->begin(), roots->end()), roots->end());
			madeFor = a_set;
			made = std::move(roots);
			return made;
		}

		Api::FrameValueInput CpuStatesKick(const Api::FrameValuePoolKick&)
		{
			DCLF_ZONE("CS.DCLF.FV.Kick.FadeCpuStates");
			TracyCZoneN(fadeArm, "CS.DCLF.FV.Kick.Fade.Arm", true);
			auto arm = Direct::TakeCpuFrameArm();
			const auto& view = Scene::FrameView::Get();
			auto tables = view.AcceptedTables();
			const auto* scene = IndirectDraws::Get().Scene();
			TracyCZoneEnd(fadeArm);
			if (!arm.set || !tables || !scene)
				return { idleInput, stateRows };
			auto input = std::make_shared<CpuStageInput>();
			input->observer = arm.observer;
			input->set = std::move(arm.set);
			input->sceneFrame = arm.sceneFrame;
			input->visibilityReset = arm.visibilityReset;
			input->candidates = view.GetSunCandidates();
			input->publication = view.InstalledSequence();
			input->retirement = view.InstalledRetirement();
			input->scene = SharedSceneOf(*scene);
			// The same inputs the depth commit samples for FadeStateCS.
			TracyCZoneN(fadeSample, "CS.DCLF.FV.Kick.Fade.SampleFrame", true);
			input->frame = FadeState::SampleFrame(nullptr);
			TracyCZoneEnd(fadeSample);
			input->fadeEye = FadeState::FadeEye();
			input->treeHeight = FadeState::TreeHeightTest();
			std::copy_n(input->fadeEye.data(), 3, input->frame.eye);
			input->frame.lodAdjust = input->fadeEye[3];
			if (const auto* camera = FadeState::MainCamera()) {
				ShadowCameras::Camera matrices;
				ShadowCameras::CameraMatrices(*camera, matrices);
				input->camera = matrices.frustum;
			}
			const auto list = Extensions::RootListOf(*tables);
			TakeAnimated(*input);
			{
				DCLF_ZONE("CS.DCLF.FV.Kick.Fade.RootPrograms");
				input->rootPrograms = RootPrograms(*tables);
			}
			if (const auto* cull = Extensions::MainCullInput(); cull && cull->contribution.programWords) {
				DCLF_ZONE("CS.DCLF.FV.Kick.Fade.ProgramWords");
				const auto words = cull->contribution.programWords();
				ZoneValue(words.size());
				input->programs.assign(words.begin(), words.end());
			}
			input->tables = std::move(tables);
			// An observer sends nothing, so it needs no backing.
			if (!input->observer)
				stateRows = std::max<std::uint64_t>(stateRows, list.rows.size());
			return { std::move(input), stateRows };
		}

		// ---- Sample (the producer) ----

		// Producer only. animatedSlots lists the counts to clear next time.
		std::vector<std::uint8_t> animatedCounts;
		std::vector<std::uint32_t> animatedSlots;

		/**
		 * @brief Counts animated updates per root slot, as SampleFadeInputs does. Skips roots the tables no longer list.
		 * Order does not matter, so the batch is never sorted.
		 */
		std::span<const std::uint8_t> CountAnimated(const CpuStageInput& a_input, const Api::RootListView& a_list)
		{
			for (const std::uint32_t slot : animatedSlots)
				animatedCounts[slot] = 0;
			animatedSlots.clear();
			if (animatedCounts.size() < a_list.rows.size())
				animatedCounts.resize(a_list.rows.size(), 0);
			for (const auto& root : a_input.animatedRoots) {
				if (root.slot >= a_list.rows.size() || root.slot >= a_list.nodes.size() || a_list.nodes[root.slot] != root.node ||
					a_list.rows[root.slot].generation != root.generation)
					continue;
				auto& count = animatedCounts[root.slot];
				if (!count)
					animatedSlots.push_back(root.slot);
				count = static_cast<std::uint8_t>(std::min<std::uint32_t>(count + 1u, kFadeAnimatedCountMask));
			}
			return std::span<const std::uint8_t>(animatedCounts.data(), a_list.rows.size());
		}

		/**
		 * @brief Matches FadeStateCS's FadeMilestone. The engine still draws parts of a root that is not stood in,
		 * so while it fades each of 32 fade steps also counts as a milestone.
		 */
		std::uint32_t Milestone(const FadeNodeState& a_state, std::uint32_t a_rootBits)
		{
			const std::uint32_t fade = a_state.currentFade <= 0.0f ? 0u : (a_state.currentFade >= 1.0f ? 2u : 1u);
			const std::uint32_t step = fade == 1u && !(a_rootBits & kFadeRootStoodIn) ? std::min(static_cast<std::uint32_t>(a_state.currentFade * 32.0f), 31u) : 0u;
			return fade | ((a_state.levels & 0xFu) << 2) | (((a_state.levels >> 12) & 0x7u) << 6) | (step << 9);
		}
		// Milestone never returns this, so the next milestone always differs.
		constexpr std::uint32_t kNoMilestone = ~0u;

		bool CameraInView(const CpuStageInput& a_input, std::uint32_t a_bits, const float a_centre[3], float a_radius)
		{
			if (a_bits & kFadeRootAlwaysDraw)
				return true;
			const auto& planes = a_input.camera;
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(planes.mask & (1u << p)))
					continue;
				const auto& n = planes.plane[p];
				if (n[0] * a_centre[0] + n[1] * a_centre[1] + n[2] * a_centre[2] - n[3] < -a_radius)
					return false;
			}
			return true;
		}

		/** @brief Mirrors the GPU fade query's choice between the frustum test and the portal program. */
		bool RootInView(const CpuStageInput& a_input, std::uint32_t a_program, const FadeRootStatic& a_root, const float a_centre[3], float a_radius)
		{
			// Both tests use absolute planes, so a margin keeps rounding from culling. A zero radius stays zero so the portal test culls it.
			const float margin = ::DCLF::Draws::CullPlaneMargin(a_input.fadeEye[0], a_input.fadeEye[1], a_input.fadeEye[2]);
			const float radius = a_radius > 0.0f ? a_radius + margin : a_radius;
			const std::uint32_t count = a_input.programs.size() > kPortalSetBytes / 4 ? a_input.programs[kPortalSetBytes / 4] : 0u;
			if (a_program >= count || (a_root.bits & kFadeRootPreprocessed))
				return CameraInView(a_input, a_root.bits, a_centre, radius);
			const std::uint32_t flags = ((a_root.bits & kFadeRootAlwaysDraw) ? 1u << 11 : 0u) | ((a_root.bits & kFadeRootPreprocessed) ? 1u << 12 : 0u) |
			                            ((a_root.bits & kFadeRootPreprocessHidden) ? 1u << 20 : 0u);
			return ::DCLF::Portals::PortalTestCpu(a_input.programs, a_program, a_centre, radius, flags) == 1;
		}

		/** @brief The verdict word the culls of the root's members read. */
		std::uint32_t RootWordOf(const FadeNodeState& a_effective, const FadeNodeState& a_state, const FadeRootStatic& a_row, std::uint32_t a_sceneFrame)
		{
			std::uint32_t word = Direct::kRootListed;
			if (a_effective.currentFade < 1.0f)
				word |= Direct::kRootFading;
			// An unsettled root recurses only while its fade is above 0 and its fadeAmount is not 0.
			const bool settledIn = (a_effective.flags & kFadeFlagSettled) && a_row.fadeAmount == 1.0f && a_effective.currentFade == 1.0f;
			if (!settledIn && (!(a_effective.currentFade > 0.0f) || a_row.fadeAmount == 0.0f))
				word |= Direct::kRootFadedOutOfOcclusion;
			if (a_state.generation == a_row.generation) {
				word |= (a_state.verdict & kFadeVerdictServiced) ? Direct::kRootServiced : 0u;
				word |= (a_state.verdict & kFadeVerdictDrawn) ? Direct::kRootDrawn : 0u;
				word |= (a_state.verdict & kFadeVerdictInView) ? Direct::kRootInView : 0u;
				word |= a_state.frame == a_sceneFrame ? Direct::kRootUpdated : 0u;
			}
			const std::uint32_t level = std::min(a_effective.levels & 0xFu, 3u);
			word |= level << Direct::kRootLodShift;
			if (((a_effective.levels >> 8) & 0x70u) != 0x20u && level < 3 && static_cast<int>(a_effective.blend * 31.0f) > 0)
				word |= Direct::kRootLodCopy;
			return word;
		}

		/** @brief Per-chunk context for StepRoot. events and marked belong to the chunk alone. */
		struct StepContext
		{
			const CpuStageInput& input;
			const Api::RootListView& list;
			const std::vector<FadeRootStatic>& seeds;
			const std::vector<FadeRootBound>& bounds;
			FrameStorage& storage;
			std::vector<FadeNodeState>& states;
			std::vector<FadeEvent>& events;
			std::vector<std::uint32_t>& marked;      // cpuStates.Mark is not thread-safe, so marks are applied after the chunks
			std::span<const std::uint8_t> animated;
		};

		/** @brief Matches FadeStateCS's ReportMilestone. Unlike the GPU list, this one never overflows. */
		void Report(StepContext& a_context, std::uint32_t a_root, const FadeRootStatic& a_row, const FadeNodeState& a_state)
		{
			auto& last = reported[a_root];
			// A new listing always writes its first update. The engine may still step the node after the seed was copied from it.
			if (last.first != a_row.generation)
				last = { a_row.generation, kNoMilestone };
			const std::uint32_t milestone = Milestone(a_state, a_row.bits);
			if ((a_row.bits & kFadeRootWritten) && last.second != milestone) {
				FadeEvent event;
				event.root = a_root;
				event.generation = a_row.generation;
				event.flags = a_state.flags;
				event.currentFade = a_state.currentFade;
				event.levels = a_state.levels;
				event.blend = a_state.blend;
				a_context.events.push_back(event);
			}
			last = { a_row.generation, milestone };
		}

		/** @brief FadeStateCS's main for one root slot. */
		void StepRoot(StepContext& a_context, std::uint32_t a_slot)
		{
			const auto& input = a_context.input;
			const FadeRootStatic& row = a_context.list.rows[a_slot];
			FadeNodeState& state = a_context.states[a_slot];
			auto& storage = a_context.storage;
			if (!input.observer)
				storage.rootGenerations[a_slot] = row.generation;
			const bool listed = row.object != kNoObject && row.generation != 0;
			FadeRootStatic root = row;
			const bool merged = listed && MergeFadeSeed(row, a_context.seeds.data(), a_context.seeds.size(), a_slot, root);
			if (merged) {
				if (state.generation != root.generation) {
					state = root.initial;
					state.generation = root.generation;
					state.verdict = 0;
					state.frame = 0;
				}
				// The root node's own bound this frame, as OnVisible tests it. A root waits for its first sampled bound like its seed.
				const FadeRootBound* bound = a_slot < a_context.bounds.size() && a_context.bounds[a_slot].generation == root.generation ? &a_context.bounds[a_slot] : nullptr;
				if (state.frame != input.sceneFrame && input.frame.lodAdjust != 0.0f && bound) {
					const float* centre = bound->centre;
					const float radius = bound->radius;
					state.verdict = 0;
					const std::uint32_t updates = a_slot < a_context.animated.size() ? a_context.animated[a_slot] : 0u;
					if (updates)
						FadeState::AnimatedUpdate(state, root, centre, input.frame, updates);
					const std::uint32_t program = a_slot < input.rootPrograms->size() ? (*input.rootPrograms)[a_slot] : kPortalNoProgram;
					if (RootInView(input, program, root, centre, radius))
						state.verdict = FadeState::OnVisible(state, root, centre, input.frame);
					if (updates)
						state.verdict |= kFadeVerdictAnimated | (updates << 5);
					state.frame = input.sceneFrame;
					if (!input.observer) {
						a_context.marked.push_back(a_slot);
						Report(a_context, a_slot, root, state);
					}
				}
			}
			// Same rule as BuildDrawsCS's SeedState. Prefer the root's own state, then its seed's, then its row's.
			const FadeNodeState& effective = state.generation == row.generation ? state : (merged ? root.initial : row.initial);
			if (!input.observer)
				storage.rootStates[a_slot] = effective;
			storage.rootWords[a_slot] = listed ? RootWordOf(effective, state, row, input.sceneFrame) : 0u;
		}

		/** @brief Copies the rows the culls read, because the next producer rewrites the source. */
		void CopyRows(std::span<const BindlessPlacement> a_rows, FrameStorage& a_storage)
		{
			a_storage.sunEntry.resize(a_rows.size());
			a_storage.fadeNode.resize(a_rows.size());
			for (std::size_t o = 0; o < a_rows.size(); ++o) {
				std::memcpy(a_storage.sunEntry[o].data(), a_rows[o].sunEntry, sizeof(float) * 4);
				std::memcpy(a_storage.fadeNode[o].data(), a_rows[o].lodFadeNode, sizeof(float) * 4);
			}
		}

		/** @brief The CullFrame owns a_storage and returns it to the free stack when the last cull lets go. */
		std::shared_ptr<Direct::CullFrame> MakeCullFrame(const CpuStageInput& a_input, std::uint64_t a_seq, FrameStorage* a_storage)
		{
			auto frame = std::make_shared<Direct::CullFrame>();
			auto& s = *a_storage;
			frame->sceneFrame = a_input.sceneFrame;
			frame->stamp = a_input.sceneFrame & 0x0FFFFFFFu;
			frame->frameValuesSeq = a_seq;
			frame->set = a_input.set;
			frame->sunEntry = s.sunEntry;
			frame->fadeNode = s.fadeNode;
			frame->rootWords = s.rootWords;
			frame->rootStates = s.rootStates;
			frame->rootGenerations = s.rootGenerations;
			frame->rootPrograms = s.rootPrograms;
			frame->rootBounds = s.rootBounds;
			frame->programs = s.programs;
			// Portal tests only decide visibility stamps, and an observer's culls stamp nothing.
			frame->reportsVisibility = !a_input.observer;
			frame->waterSites = !a_input.observer;
			if (!a_input.observer) {
				frame->portalTest = &::DCLF::Portals::PortalTestCpu;
				frame->portalTestBatch = &::DCLF::Portals::PortalTestCpuBatch;
			}
			frame->fadeEye = a_input.fadeEye;
			frame->treeHeight = a_input.treeHeight;
			frame->tables = a_input.tables;
			frame->publication = a_input.publication;
			frame->retirement = a_input.retirement;
			frame->candidates = a_input.candidates;
			frame->visibilityReset = a_input.visibilityReset;
			frame->storage = std::shared_ptr<const void>(a_storage, [](const void* a_owned) { GiveBackStorage(static_cast<FrameStorage*>(const_cast<void*>(a_owned))); });
			return frame;
		}

		void CpuStatesSample(Api::FrameValuePoolProducer& a_producer)
		{
			const auto* input = a_producer.Input<CpuStageInput>();
			if (!input || !input->set || !input->tables) {
				// Nothing draws from the last frame any more, so release what it holds.
				if (!Direct::CpuFrame())
					Direct::DropCullFrame();
				return;
			}
			DCLF_ZONE("CS.DCLF.FadeCpuStage");
			TakeAnimatedOnWorker(*input);
			TracyCZoneN(fPrep, "CS.DCLF.FadeCpuStage.Prepare", true);
			const bool observer = input->observer;
			if (!observer)
				a_producer.BeginBuild(cpuStates);
			const auto list = Extensions::RootListOf(*input->tables);
			const std::size_t roots = list.rows.size();
			auto& states = observer ? observerStates : cpuStates.Mutable();
			if (states.size() < roots) {
				const std::size_t first = states.size();
				states.resize(roots, FadeNodeState{});
				if (!observer)
					cpuStates.MarkRange(first, roots - first);
			}
			if (reported.size() < roots)
				reported.resize(roots, { 0u, 0u });
			auto* storage = TakeStorage();
			const auto rows = Direct::SampledRows(a_producer.seq);
			storage->rootWords.assign(roots, 0u);
			// An observer's frame is only read for its root words.
			if (observer) {
				storage->sunEntry.clear();
				storage->fadeNode.clear();
				storage->rootGenerations.clear();
				storage->rootStates.clear();
				storage->rootPrograms.clear();
				storage->programs.clear();
			} else {
				{
					DCLF_ZONE("CS.DCLF.FadeCpuStage.CopyRows");
					ZoneValue(rows.size());
					CopyRows(rows, *storage);
				}
				storage->rootGenerations.assign(roots, 0u);
				storage->rootStates.assign(roots, FadeNodeState{});
				// A program index past this frame's count falls back to the frustum test.
				const std::uint32_t count = input->programs.size() > kPortalSetBytes / 4 ? input->programs[kPortalSetBytes / 4] : 0u;
				storage->rootPrograms.assign(roots, kPortalNoProgram);
				for (std::size_t r = 0; r < roots && r < input->rootPrograms->size(); ++r)
					if ((*input->rootPrograms)[r] < count)
						storage->rootPrograms[r] = (*input->rootPrograms)[r];
				storage->programs = input->programs;
			}
			// Every root, or for an observer the routed roots still listed. Producer only (RoutedRoots' cache).
			if (observer && !input->routedRoots) {
				DCLF_ZONE("CS.DCLF.FadeCpuStage.RoutedRoots");
				input->routedRoots = RoutedRoots(input->set);
			}
			const std::uint32_t* routed = observer ? input->routedRoots->data() : nullptr;
			const std::size_t steps = observer ? static_cast<std::size_t>(std::lower_bound(input->routedRoots->begin(), input->routedRoots->end(), roots) - input->routedRoots->begin()) : roots;
			const auto slotAt = [&](std::size_t a_step) { return routed ? routed[a_step] : static_cast<std::uint32_t>(a_step); };
			// A root's step touches only its own slot, so chunks run in parallel.
			// Each chunk keeps its own events and marks, merged afterwards in root order.
			constexpr std::size_t kRootGrain = 256;
			const std::size_t chunks = (steps + kRootGrain - 1) / kRootGrain;
			std::vector<std::vector<FadeEvent>> chunkEvents(chunks);
			std::vector<std::vector<std::uint32_t>> chunkMarked(chunks);
			const auto& seeds = ProducerSeeds();
			const auto& bounds = ProducerRootBounds();
			storage->rootBounds.assign(bounds.begin(), bounds.end());
			const auto animated = CountAnimated(*input, list);
			TracyCZoneValue(fPrep, steps);
			TracyCZoneEnd(fPrep);
			TracyCZoneN(fSteps, "CS.DCLF.FadeCpuStage.Steps", true);
			::RenderGraph::Frame::SharedExecutor().ParallelFor("CS.DCLF.FadeCpuStage.Roots", steps, kRootGrain, [&](std::size_t a_begin, std::size_t a_end) {
				const std::size_t chunk = a_begin / kRootGrain;
				StepContext context{ *input, list, seeds, bounds, *storage, states, chunkEvents[chunk], chunkMarked[chunk], animated };
				for (std::size_t r = a_begin; r < a_end; ++r)
					StepRoot(context, slotAt(r));
			});
			TracyCZoneEnd(fSteps);
			DCLF_ZONE_NAMED(fTail, "CS.DCLF.FadeCpuStage.Publish", true);
			if (!observer) {
				std::vector<FadeEvent> events;
				for (std::size_t c = 0; c < chunks; ++c) {
					events.insert(events.end(), chunkEvents[c].begin(), chunkEvents[c].end());
					for (const std::uint32_t slot : chunkMarked[c])
						cpuStates.Mark(slot);
				}
				if (input->scene && input->scene->fadeWriteBack)
					PostCpuFadeEvents(*input->scene->fadeWriteBack, input->sceneFrame, std::move(events));
			}
			Direct::PublishCullFrame(MakeCullFrame(*input, a_producer.seq, storage));
		}

		void CpuStatesSend(Api::FrameValuePoolProducer& a_producer)
		{
			a_producer.Send(cpuStates);
		}
	}

	void RegisterCpuStage(Api::InstallContext& a_context)
	{
		// Must register after the seeds' pool, because its sample reads this producer's seeds.
		cpuStatesPool = a_context.RegisterFrameValuePool({ .name = "fade-states-cpu", .stride = sizeof(FadeNodeState), .kick = &CpuStatesKick,
			.sample = &CpuStatesSample, .send = &CpuStatesSend });
	}

	std::uint32_t CpuStatesIndex()
	{
		return FrameValues::Get().PoolIndex(cpuStatesPool);
	}
}
