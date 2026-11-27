// The depth commit hook for fades. It uploads root rows and FadeStateCS's per-frame inputs,
// harvests and re-arms the write-back readback, and uploads each root's portal program.

#include <Tracy/Tracy.hpp>

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/TableFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Uploads.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/SceneBuffers.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <algorithm>
#include <numeric>

namespace DCLF::Features::Fades
{
	namespace
	{
		// Once a frame.
		void SampleFadeInputs(const Draws::NamedVersions& a_adopted, Draws::CommitUploads& a_uploads, FadeScene& a_buffers, std::uint32_t a_rootSlots,
			FadeFrame& a_inputs)
		{
			a_inputs = FadeState::SampleFrame(nullptr);
			const auto eye = FadeState::FadeEye();
			std::copy_n(eye.data(), 3, a_inputs.eye);
			a_inputs.lodAdjust = eye[3];
			// Kept for their storage, which the batches rotate through. Render thread.
			static std::vector<AnimatedFadeRoot> roots;
			static AnimatedStamps stamps;
			AnimatedFadeInputs anim{};
			TakeAnimatedBatch(roots, anim);
			std::copy_n(anim.eye, 3, a_inputs.animEye);
			a_inputs.animLodAdjust = anim.lodAdjust;
			a_inputs.animCounter = anim.counter;
			a_inputs.animDeltaTime = anim.deltaTime;
			// Normally counted before the list jobs' wait.
			if (!TakeAnimatedStamps(stamps))
				CountAnimatedRoots(roots, stamps);
			TracyCZoneN(stampZone, "CS.DCLF.Fade.Stamp", true);
			TracyCZoneValue(stampZone, stamps.slots.size());
			static std::vector<std::uint32_t> stamped;  // render thread, kept for its storage
			stamped.clear();
			auto& animatedWords = a_buffers.fadeAnimatedMirror;
			if (animatedWords.size() < a_rootSlots)
				animatedWords.resize(a_rootSlots, 0u);
			if (a_buffers.fadeAnimated && anim.valid) {
				const std::uint32_t sceneStep = Scene::FrameView::Get().SceneStep();
				for (std::size_t i = 0; i < stamps.slots.size(); ++i)
					if (const std::uint32_t root = stamps.slots[i]; root < a_rootSlots) {
						animatedWords[root] = FadeAnimatedWord(sceneStep, stamps.counts[i]);
						stamped.push_back(root);
					}
			}
			TracyCZoneEnd(stampZone);
			KeepAnimatedBatchForObserver(roots, anim);
			// A version not filled yet gets the whole mirror.
			if (const auto& animated = a_adopted.Of(a_buffers.fadeAnimated); !a_buffers.fadeAnimatedFilled.Holds(animated)) {
				stamped.resize(std::min<std::size_t>(animatedWords.size(), animated.units));
				std::iota(stamped.begin(), stamped.end(), 0u);
				a_buffers.fadeAnimatedFilled.Filled(animated);
			}
			Draws::SendWordRuns(stamped, [&](std::uint32_t a_first, std::uint32_t a_count) {
				a_uploads(a_buffers.fadeAnimated, animatedWords.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
			});
			if (anim.varied)
				Diagnostics::Notify(Diagnostics::FadeInputsVaried{ Scene::FrameView::Get().SceneStep() });
		}

		/** @brief Uploads the static rows the sealed version is missing, and the frame row. */
		void UploadFadeRoots(const Draws::NamedVersions& a_sealed, const SceneTables& a_tables, std::uint32_t a_frame, const FadeFrame& a_inputs,
			FadeScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			if (!a_scene.fadeState || !a_scene.fadeRoots)
				return;
			const auto list = Extensions::RootListOf(a_tables);
			const auto* family = Draws::RootFamilyOf(a_tables);
			const std::uint64_t current = list.contentEpoch;
			const auto& roots = a_sealed.Of(a_scene.fadeRoots);
			const std::uint64_t contentEpoch = a_scene.fadeRootsFilled.Since(roots).value_or(~0ull);
			if (contentEpoch != current && list.rows.size() <= roots.units) {
				const auto* rows = list.rows.data();
				if (family)
					family->Journal().Take().ForEachRun(contentEpoch, list.rows.size(), [&](std::uint64_t a_first, std::uint64_t a_count) {
						a_uploads(a_scene.fadeRoots, rows + a_first, static_cast<std::size_t>(a_count * sizeof(FadeRootStatic)), a_first * sizeof(FadeRootStatic));
					});
				a_scene.fadeRootsFilled.Filled(roots, current);
			}
			a_scene.fadeRootCount = a_scene.FadeRootsContentEpoch(a_sealed) == current ? static_cast<std::uint32_t>(list.rows.size()) : 0u;
			if (a_scene.fadeFrameNumber != a_frame) {
				a_scene.fadeFrameNumber = a_frame;
				a_scene.fadeFrame = a_inputs;
			}
			a_scene.fadeFrame.rootCount = a_scene.fadeRootCount;
			a_scene.fadeFrame.sceneFrame = a_scene.fadeFrameNumber;
			a_scene.fadeFrame.outSlot = static_cast<std::uint32_t>(Scene::FrameView::Get().RenderSerial() & 1);
			a_uploads(a_scene.fadeFrameBuffer, &a_scene.fadeFrame, sizeof(FadeFrame), 0);
		}

		// Incrementally, roots written since the last judging plus new ones. Otherwise every root.
		void CollectRootsToJudge(const Extensions::TableFamilyColumns* a_family, bool a_incremental, std::uint64_t a_judgedEpoch, std::size_t a_before,
			std::size_t a_rootCount, std::vector<std::uint32_t>& a_roots)
		{
			a_roots.clear();
			if (a_incremental) {
				if (a_family)
					a_family->Journal().Take().ForEachRun(a_judgedEpoch, a_before, [&](std::uint64_t a_first, std::uint64_t a_count) {
						for (std::uint64_t r = a_first; r < a_first + a_count && r < a_before; ++r)
							a_roots.push_back(static_cast<std::uint32_t>(r));
					});
				for (std::size_t r = a_before; r < a_rootCount; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
			} else {
				for (std::size_t r = 0; r < a_rootCount; ++r)
					a_roots.push_back(static_cast<std::uint32_t>(r));
			}
		}

		// A root without a node gets noProgram without asking.
		void JudgeRootPrograms(const Extensions::MainCullInputEntry& a_input, std::span<const void* const> a_nodes, FadeScene::RootProgramsBatch& a_batch)
		{
			a_batch.asked.clear();
			a_batch.programs.assign(a_batch.roots.size(), a_input.contribution.noProgram);
			for (std::size_t k = 0; k < a_batch.roots.size(); ++k)
				if (a_nodes[a_batch.roots[k]])
					a_batch.asked.push_back(static_cast<std::uint32_t>(k));
			if (a_batch.asked.empty())
				return;
			Api::RootProgramsJudge judge{ a_nodes, a_batch.roots, a_batch.asked, a_batch.programs };
			a_input.contribution.judge(judge);
		}

		// Each root's portal program. Rejudges only changed roots unless the input changed or the version does not hold the mirror.
		// Without a main cull input nothing reads the programs.
		void UploadFadeRootPrograms(const Draws::NamedVersions& a_adopted, Draws::CommitUploads& a_uploads, FadeScene& a_buffers)
		{
			const auto* input = Extensions::MainCullInput();
			if (!input)
				return;
			const auto& rootTables = Scene::FrameView::Get().GetTables();
			const auto rootList = Extensions::RootListOf(rootTables);
			const std::uint64_t rootsEpoch = rootList.contentEpoch, inputVersion = input->contribution.programsVersion();
			const std::uint64_t programsKey = rootsEpoch * 0x9E3779B97F4A7C15ull ^ inputVersion;
			const auto& programsVersion = a_adopted.Of(a_buffers.fadeRootPrograms);
			const auto programsFilledKey = a_buffers.fadeRootProgramsFilled.Since(programsVersion);
			const std::size_t rootCount = rootList.nodes.size();
			if (programsFilledKey != programsKey && rootCount <= programsVersion.units) {
				auto& sent = a_buffers.fadeRootProgramsMirror;
				const auto judged = a_buffers.fadeRootProgramsMirrorOf;
				const bool incremental = programsFilledKey && *programsFilledKey == judged.key && judged.programs == inputVersion && sent.size() <= rootCount;
				if (!programsFilledKey)
					sent.clear();
				if (!incremental)
					sent.resize(std::min(sent.size(), rootCount));
				auto& batch = a_buffers.fadeRootProgramsBatch;
				CollectRootsToJudge(Draws::RootFamilyOf(rootTables), incremental, judged.rootsEpoch, sent.size(), rootCount, batch.roots);
				JudgeRootPrograms(*input, rootList.nodes, batch);
				// Upload only words that differ from the mirror.
				std::vector<std::uint32_t> differing;
				for (std::size_t k = 0; k < batch.roots.size(); ++k) {
					const std::size_t root = batch.roots[k];
					const std::uint32_t program = batch.programs[k];
					if (root < sent.size() && sent[root] == program)
						continue;
					if (root >= sent.size())
						sent.resize(root + 1, ~program);
					sent[root] = program;
					differing.push_back(static_cast<std::uint32_t>(root));
				}
				if (incremental) {
					std::sort(differing.begin(), differing.end());
					differing.erase(std::unique(differing.begin(), differing.end()), differing.end());
				}
				Draws::SendWordRuns(differing, [&](std::uint32_t a_first, std::uint32_t a_count) {
					a_uploads(a_buffers.fadeRootPrograms, sent.data() + a_first, std::size_t(a_count) * sizeof(std::uint32_t), std::uint64_t(a_first) * sizeof(std::uint32_t));
				});
				a_buffers.fadeRootProgramsFilled.Filled(programsVersion, programsKey);
				a_buffers.fadeRootProgramsMirrorOf = { programsKey, rootsEpoch, inputVersion };
			}
		}

		/** @brief Harvests the range's last fade events for the write-back task, then resets the list and re-arms the range. */
		void ArmFadeReadback(FadeScene& a_scene, Draws::CommitUploads& a_uploads)
		{
			if (!a_scene.fadeWriteBack || !a_scene.fadeEvents)
				return;
			auto& writeBack = *a_scene.fadeWriteBack;
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return;
			// If the epoch did not run for the armed run, FadeStateCS did not report either, so the next run reports those changes.
			const auto commit = writeBack.list->CommitList(host->CurrentRun(), [&](const org::RunReadback::ListRead& a_read) {
				if (a_read.region >= writeBack.armed.size() || !std::exchange(writeBack.armed[a_read.region].armed, false) || !a_read.words)
					return;
				if (const std::uint32_t events = std::min(a_read.count, a_read.capacity)) {
					auto* batch = new FadeWriteBack::Batch();
					batch->frame = writeBack.armed[a_read.region].frame;
					const auto* list = reinterpret_cast<const FadeEvent*>(a_read.words + kFadeEventHeaderWords);
					batch->events.assign(list, list + events);
					writeBack.Push(batch);
					writeBack.harvestedWithEvents.fetch_add(1, std::memory_order_relaxed);
					writeBack.events.fetch_add(events, std::memory_order_relaxed);
				}
				writeBack.harvested.fetch_add(1, std::memory_order_relaxed);
			});
			const std::uint32_t frameSlot = commit.region;
			if (frameSlot >= writeBack.armed.size())
				return;
			// Header with a zero count and the run's stamp, which the read matches.
			const std::uint32_t header[kFadeEventHeaderWords]{ 0u, commit.stamp, 0u, 0u };
			a_uploads(a_scene.fadeEvents, header, sizeof(header), 0);
			auto& armed = writeBack.armed[frameSlot];
			armed.armed = true;
			armed.frame = Scene::FrameView::Get().SceneStep();
		}

		void CommitFadeRoots(const Api::CommitContext& a_commit)
		{
			if (a_commit.kind != Api::CommitKind::Depth)
				return;
			auto* fades = SceneOf(a_commit.scene);
			if (!fades || !fades->fadeState)
				return;
			const auto& adopted = a_commit.adopted;
			auto& uploads = a_commit.uploads;
			auto& buffers = *fades;
			const auto rootSlots = static_cast<std::uint32_t>(adopted.Of(buffers.fadeRoots).units);
			FadeFrame inputs = buffers.fadeFrame;
			if (buffers.fadeFrameNumber != a_commit.frame) {
				ZoneScopedN("CS.DCLF.Fade.SampleInputs");
				SampleFadeInputs(adopted, uploads, buffers, rootSlots, inputs);
			}
			{
				ZoneScopedN("CS.DCLF.Fade.UploadRoots");
				UploadFadeRoots(adopted, a_commit.tables, a_commit.frame, inputs, buffers, uploads);
			}

			Extensions::TableFamilySent(Extensions::RootStatesFamily(), buffers.FadeRootsContentEpoch(adopted));
			{
				ZoneScopedN("CS.DCLF.Fade.ArmReadback");
				ArmFadeReadback(buffers, uploads);
			}
			ZoneScopedN("CS.DCLF.Fade.UploadPrograms");
			UploadFadeRootPrograms(adopted, uploads, buffers);
		}
	}

	void RegisterCommit(Api::InstallContext& a_context)
	{
		// Must run after every other feature's hook, including Portals', which sends the programs.
		a_context.RegisterCommitHook({ .name = "fade roots", .commit = &CommitFadeRoots });
	}
}
