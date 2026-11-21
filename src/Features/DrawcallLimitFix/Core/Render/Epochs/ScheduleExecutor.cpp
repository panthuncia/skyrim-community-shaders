// The schedule's executor: the registered passes, the schedule resolved against them, and the gather that adds an
// extension's epoch passes. Apart from Diagnostics' observers, this is the one place DCLF's passes reach the graph.

#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Triggers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "RenderGraph/Fatal.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>

namespace DCLF::Draws
{
	namespace
	{
		/** @brief A registered pass, owning its strings. */
		struct PassEntry
		{
			std::string key, passName, who;
			Api::PassKind kind = Api::PassKind::Compute;
			Api::PassQueue queue = Api::PassQueue::Default;
			decltype(Api::PassDef::make) make = nullptr;
			decltype(Api::PassDef::emit) emit = nullptr;
			decltype(Api::PassDef::setup) setup = nullptr;
			decltype(Api::PassDef::latchWord) latchWord = nullptr;
			decltype(Api::PassDef::animatedRows) animatedRows = nullptr;
			decltype(Api::PassDef::frameRows) frameRows = nullptr;
			bool cpuPipeline = false;
			bool recordAtExecution = false;
			bool gpuLate = false;
			bool viewName = false;  // passName contains "{view}"
			decltype(Api::PassDef::present) present = nullptr;
		};

		/**
		 * @brief A schedule epoch's pass, resolved to its index in Executor::passes. A hosted epoch's pass sits in its host's
		 * list and carries that epoch's row. hosts has a bit per hosted epoch row.
		 */
		struct ResolvedPass
		{
			std::uint32_t pass = 0;
			std::uint32_t hosted = kNoEpoch;
		};
		struct ResolvedEpoch
		{
			std::string id;
			RenderGraphRuntime::Segment segment{};
			Api::EngineView view{};
			std::vector<ResolvedPass> passes;
			std::uint32_t hosts = 0;
		};

		constexpr std::string_view kViewToken = "{view}";
		// The depth commit's build reads the stage word from its latch, so the pass writing it must run first.
		constexpr std::string_view kDepthBuild = "core.z.build-draws";

		// Written only during InstallFeatures, before DCLF's threads exist, and read-only after.
		struct Executor
		{
			std::vector<PassEntry> passes;
			// By shape, in the schedule's order.
			std::array<std::vector<ResolvedEpoch>, 3> epochs;
			decltype(Api::PassDef::latchWord) latchWord = nullptr;  // only one pass may have one
			decltype(Api::PassDef::animatedRows) animatedRows = nullptr;
			decltype(Api::PassDef::frameRows) frameRows = nullptr;
			bool resolved = false;
			// The epochs' definitions by row.
			std::array<EpochTraits, kMaxEpochs> traits{};
			std::array<std::string, kMaxEpochs> ids, names;
			std::uint32_t epochCount = 0;
			std::array<std::uint32_t, 2> mainEpochs{ kNoEpoch, kNoEpoch };
			std::array<std::uint32_t, kShadowShapes> shadowEpochs{ kNoEpoch };
			std::array<ViewEpochs, kViews> views{};
			Extensions::FrameOrders orders;
			std::vector<std::string> triggers;
			std::array<std::uint32_t, kMaxEpochs> epochTriggers{};
			std::array<Api::TriggerId, static_cast<std::size_t>(CoreTrigger::Count)> coreTriggers{};
		};

		// Core's trigger point ids, in CoreTrigger's order. FrameDriver marks them.
		constexpr std::string_view kCoreTriggers[] = { "depth.committed" };
		static_assert(std::size(kCoreTriggers) == static_cast<std::size_t>(CoreTrigger::Count));

		Executor& State()
		{
			static Executor executor;
			return executor;
		}

		/** @brief a_entry's graph name in a_context's epoch, with "{view}" replaced by the view's prefix. */
		std::string NameOf(const PassEntry& a_entry, const Api::PassContext& a_context)
		{
			if (!a_entry.viewName)
				return a_entry.passName;
			auto name = a_entry.passName;
			const std::string_view prefix = a_context.View()->traits->prefix;
			for (auto at = name.find(kViewToken); at != std::string::npos; at = name.find(kViewToken, at + prefix.size()))
				name.replace(at, kViewToken.size(), prefix);
			return name;
		}

		static_assert(static_cast<int>(Api::EpochShape::Main) == static_cast<int>(ShapeKind::Main) &&
		                  static_cast<int>(Api::EpochShape::Shadow) == static_cast<int>(ShapeKind::Shadow) &&
		                  static_cast<int>(Api::EpochShape::View) == static_cast<int>(ShapeKind::View),
			"Api::EpochShape is Core's ShapeKind");

		/** @brief The row of epoch a_id. ~0u for none. */
		std::uint32_t RowOf(const Api::FrameSchedule& a_schedule, std::string_view a_id)
		{
			for (std::uint32_t i = 0; i < a_schedule.epochs.size(); ++i)
				if (a_schedule.epochs[i].id == a_id)
					return i;
			return ~0u;
		}

		/** @brief Whether epoch row a_row is hosted. A hosted epoch is in neither order and uses its host's segment. */
		bool Hosted(const Api::FrameSchedule& a_schedule, std::uint32_t a_row)
		{
			return a_row < a_schedule.epochs.size() && !a_schedule.epochs[a_row].hostedBy.empty();
		}

		/** @brief Fails the install unless every epoch id is set and unique, and every segment is used exactly once. */
		void CheckEpochIds(const Api::FrameSchedule& a_schedule)
		{
			const auto rows = static_cast<std::uint32_t>(a_schedule.epochs.size());
			auto idOf = [&](std::uint32_t a_row) { return a_schedule.epochs[a_row].id; };
			auto segmentOf = [&](std::uint32_t a_row) { return a_schedule.epochs[a_row].segment; };
			if (a_schedule.epochs.size() > kMaxEpochs)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule has {} epochs, more than the revisions hold ({}): grow kMaxEpochs first", a_schedule.epochs.size(), kMaxEpochs));
			for (std::uint32_t row = 0; row < rows; ++row) {
				if (idOf(row).empty() || RowOf(a_schedule, idOf(row)) != row)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} has no id, or one another epoch has", idOf(row)));
				if (segmentOf(row) >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} has no segment", idOf(row)));
				for (std::uint32_t other = 0; other < row; ++other)
					if (segmentOf(other) == segmentOf(row) && !Hosted(a_schedule, row) && !Hosted(a_schedule, other))
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epochs {} and {} share segment {}", idOf(other), idOf(row), segmentOf(row)));
			}
		}

		/** @brief Fails the install unless a_order names every epoch once and no hosted one. Returns its segments in order. */
		std::vector<std::uint32_t> ResolveOrder(const Api::FrameSchedule& a_schedule, std::span<const std::string_view> a_order, std::string_view a_what)
		{
			std::size_t rows = a_schedule.epochs.size();
			std::vector<std::uint32_t> segments;
			std::vector<bool> listed(rows, false);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row)
				rows -= Hosted(a_schedule, row) ? 1 : 0;
			for (const auto id : a_order) {
				const auto row = RowOf(a_schedule, id);
				if (row == ~0u || listed[row])
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's {} names epoch {}, which is unknown or named twice", a_what, id));
				if (Hosted(a_schedule, row))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's {} names epoch {}, which its host runs (hostedBy)", a_what, id));
				listed[row] = true;
				segments.push_back(a_schedule.epochs[row].segment);
			}
			if (segments.size() != rows)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's {} lists {} of its {} epochs", a_what, segments.size(), rows));
			return segments;
		}

		/** @brief Resolves epoch a_row's traits: shape, the epoch it shares and claims. Fails the install on a bad share. */
		void ResolveEpochTraits(Executor& a_state, const Api::FrameSchedule& a_schedule, std::uint32_t a_row)
		{
			const auto& epoch = a_schedule.epochs[a_row];
			const auto shape = static_cast<ShapeKind>(epoch.shape);
			const std::size_t indices = shape == ShapeKind::View ? kViews : shape == ShapeKind::Shadow ? kShadowShapes : 2;
			if (static_cast<std::size_t>(epoch.shape) > static_cast<std::size_t>(ShapeKind::View) || epoch.shapeIndex >= indices)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} records a shape Core has not ({} {})", epoch.id,
					static_cast<std::uint32_t>(epoch.shape), epoch.shapeIndex));
			a_state.ids[a_row] = std::string(epoch.id);
			a_state.names[a_row] = std::string(epoch.label);
			auto& traits = a_state.traits[a_row];
			traits = { static_cast<RenderGraphRuntime::Segment>(epoch.segment), a_state.names[a_row], shape, epoch.shapeIndex, kNoEpoch, epoch.claims };
			if (!epoch.hostedBy.empty()) {
				// A hosted epoch's passes ride in its host's recordings. The host must be an earlier Main epoch at the same trigger and segment.
				const auto host = RowOf(a_schedule, epoch.hostedBy);
				if (host >= a_row || a_state.traits[host].shape != ShapeKind::Main || a_state.traits[host].sharesWith != kNoEpoch || a_state.traits[host].hostedBy != kNoEpoch ||
					shape != ShapeKind::View || !epoch.sharesWith.empty() || !epoch.passes.empty() || a_schedule.epochs[host].trigger != epoch.trigger ||
					a_schedule.epochs[host].segment != epoch.segment)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} is hosted by {}, which is not an earlier Main epoch of its own of its trigger and "
													 "segment, or it is no View epoch of its own with its passes in the host's list",
						epoch.id, epoch.hostedBy));
				traits.hostedBy = host;
				a_state.traits[host].hostsMask |= 1u << a_row;
			}
			if (!epoch.sharesWith.empty()) {
				// The source of a shared shape must come first. A hosted epoch never shares one.
				const auto source = RowOf(a_schedule, epoch.sharesWith);
				if (source >= a_row || a_state.traits[source].shape != shape || a_state.traits[source].index != epoch.shapeIndex || a_state.traits[source].sharesWith != kNoEpoch)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} shares {}'s shape, which is not an earlier epoch recording that shape itself",
						epoch.id, epoch.sharesWith));
				traits.sharesWith = source;
				if (shape == ShapeKind::View) {
					auto& view = a_state.views[epoch.shapeIndex];
					if (view.epochs[1] != kNoEpoch)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} is a second sharing view {}'s shape", epoch.id, epoch.shapeIndex));
					view.epochs[1] = a_row;
					view.segments[1] = traits.segment;
				}
				return;
			}
			auto& owner = shape == ShapeKind::Main ? a_state.mainEpochs[epoch.shapeIndex] : shape == ShapeKind::Shadow ? a_state.shadowEpochs[epoch.shapeIndex] :
			                                                                              a_state.views[epoch.shapeIndex].epochs[0];
			if (owner != kNoEpoch)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epochs {} and {} both record one shape: the second shares it", a_state.ids[owner], epoch.id));
			owner = a_row;
			if (shape == ShapeKind::View)
				a_state.views[epoch.shapeIndex].segments = { traits.segment, traits.segment };
		}

		/** @brief Resolves the schedule's triggers. Fails the install for a missing, duplicated or unused trigger id. */
		void ResolveTriggers(Executor& a_state, const Api::FrameSchedule& a_schedule)
		{
			const auto& triggers = a_schedule.triggers;
			for (std::uint32_t row = 0; row < triggers.size(); ++row) {
				if (triggers[row].id.empty() || FindTrigger(a_schedule, triggers[row].id, "the schedule").index != row)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's trigger {} has no id, or one another trigger has", triggers[row].id));
				a_state.triggers.emplace_back(triggers[row].id);
			}
			std::vector<bool> used(triggers.size(), false);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row) {
				const auto trigger = FindTrigger(a_schedule, a_schedule.epochs[row].trigger, fmt::format("the schedule's epoch {}", a_schedule.epochs[row].id));
				a_state.epochTriggers[row] = trigger.index;
				used[trigger.index] = true;
			}
			for (std::uint32_t row = 0; row < triggers.size(); ++row)
				if (!used[row])
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's trigger {} is named by no epoch", triggers[row].id));
			for (std::size_t i = 0; i < a_state.coreTriggers.size(); ++i)
				a_state.coreTriggers[i] = FindTrigger(a_schedule, kCoreTriggers[i], "Core");
		}

		/** @brief Resolves the epochs' definitions and the frame's orders. Fails the install on any inconsistency or past kMaxEpochs. */
		void ResolveEpochs(Executor& a_state, const Api::FrameSchedule& a_schedule)
		{
			CheckEpochIds(a_schedule);
			for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row)
				ResolveEpochTraits(a_state, a_schedule, row);
			ResolveTriggers(a_state, a_schedule);
			// Core records every main, shadow and view shape.
			for (std::uint32_t i = 0; i < 2; ++i)
				if (a_state.mainEpochs[i] == kNoEpoch || (i < kShadowShapes && a_state.shadowEpochs[i] == kNoEpoch))
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule records no epoch for Core's main or shadow shape {}", i));
			for (std::size_t v = 0; v < kViews; ++v)
				if (a_state.views[v].epochs[0] == kNoEpoch)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule records no epoch for view {}", v));
			a_state.epochCount = static_cast<std::uint32_t>(a_schedule.epochs.size());
			auto& orders = a_state.orders;
			orders.epochOrder = ResolveOrder(a_schedule, a_schedule.frameOrder, "frame order");
			// The host's async epochs run in the frame order. The ones with revision set are revision-driven.
			for (const auto id : a_schedule.frameOrder)
				if (const auto row = RowOf(a_schedule, id); row < a_schedule.epochs.size() && a_schedule.epochs[row].revision)
					orders.revisionEpochs.push_back(a_schedule.epochs[row].segment);
		}
	}

	void RegisterPassDef(const Api::PassDef& a_pass, std::string_view a_who)
	{
		auto& state = State();
		if (a_pass.key.empty() || !a_pass.make == !a_pass.emit || (a_pass.make && a_pass.passName.empty()) || (a_pass.emit && (a_pass.setup || a_pass.latchWord || a_pass.animatedRows || a_pass.frameRows)) ||
			(a_pass.recordAtExecution && !a_pass.cpuPipeline && !a_pass.gpuLate))
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered an incomplete pass {}", a_who, a_pass.key));
		for (const auto& entry : state.passes)
			if (entry.key == a_pass.key)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} registered pass {}, which {} has", a_who, a_pass.key, entry.who));
		state.passes.push_back({ std::string(a_pass.key), std::string(a_pass.passName), std::string(a_who), a_pass.kind, a_pass.queue, a_pass.make, a_pass.emit,
			a_pass.setup, a_pass.latchWord, a_pass.animatedRows, a_pass.frameRows, a_pass.cpuPipeline, a_pass.recordAtExecution, a_pass.gpuLate,
			a_pass.passName.find(kViewToken) != std::string_view::npos, a_pass.present });
	}

	Api::TriggerId FindTrigger(const Api::FrameSchedule& a_schedule, std::string_view a_id, std::string_view a_who)
	{
		std::uint32_t row = 0;
		while (row < a_schedule.triggers.size() && a_schedule.triggers[row].id != a_id)
			++row;
		if (a_id.empty() || row == a_schedule.triggers.size())
			::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{} names trigger {}, which the schedule has not", a_who, a_id));
		return { row };
	}

	void ResolveSchedule(const Api::FrameSchedule& a_schedule)
	{
		auto& state = State();
		if (std::exchange(state.resolved, true))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the schedule was resolved twice");
		ResolveEpochs(state, a_schedule);
		// BuildDrawsLatch has one stage word, the depth segment's.
		std::uint32_t latchPass = ~0u;
		for (std::uint32_t i = 0; i < state.passes.size(); ++i) {
			if (!state.passes[i].latchWord)
				continue;
			if (latchPass != ~0u)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s pass {} asks for the latch's stage word, which {}'s pass {} has: grow BuildDrawsLatch first",
					state.passes[i].who, state.passes[i].key, state.passes[latchPass].who, state.passes[latchPass].key));
			latchPass = i;
		}
		// The latch's animated rows likewise. One pass, before the depth build, since every reader of the main rows runs after it.
		std::uint32_t rowsPass = ~0u;
		for (std::uint32_t i = 0; i < state.passes.size(); ++i) {
			if (!state.passes[i].animatedRows)
				continue;
			if (rowsPass != ~0u)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s pass {} asks for the latch's animated rows, which {}'s pass {} has",
					state.passes[i].who, state.passes[i].key, state.passes[rowsPass].who, state.passes[rowsPass].key));
			rowsPass = i;
		}
		std::uint32_t frameRowsPass = ~0u;
		for (std::uint32_t i = 0; i < state.passes.size(); ++i) {
			if (!state.passes[i].frameRows)
				continue;
			if (frameRowsPass != ~0u)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("{}'s pass {} asks for the latch's frame rows, which {}'s pass {} has",
					state.passes[i].who, state.passes[i].key, state.passes[frameRowsPass].who, state.passes[frameRowsPass].key));
			frameRowsPass = i;
		}
		bool rowsScheduled = false;
		bool latchScheduled = false;
		bool frameRowsScheduled = false;
		for (std::uint32_t row = 0; row < a_schedule.epochs.size(); ++row) {
			const auto& epoch = a_schedule.epochs[row];
			const auto shape = static_cast<std::size_t>(epoch.shape);
			if (shape >= state.epochs.size() || epoch.segment >= static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count))
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} has no shape or segment", epoch.id));
			// A hosted epoch's passes are listed in its host's epoch.
			if (state.traits[row].hostedBy != kNoEpoch)
				continue;
			ResolvedEpoch resolved{ std::string(epoch.id), static_cast<RenderGraphRuntime::Segment>(epoch.segment), static_cast<Api::EngineView>(epoch.shapeIndex), {},
				state.traits[row].hostsMask };
			for (std::size_t k = 0; k < epoch.passes.size(); ++k) {
				auto key = epoch.passes[k];
				std::uint32_t hosted = kNoEpoch;
				if (const auto at = key.find(Api::kHostedKey); at != std::string_view::npos) {
					hosted = EpochById(key.substr(0, at));
					if (hosted == kNoEpoch || state.traits[hosted].hostedBy != row)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} names pass {} of an epoch it does not host", epoch.id, key));
					key = key.substr(at + 1);
				}
				std::uint32_t found = ~0u;
				for (std::uint32_t i = 0; i < state.passes.size() && found == ~0u; ++i)
					if (state.passes[i].key == key)
						found = i;
				if (found == ~0u)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} names pass {}, which nobody registered", epoch.id, key));
				// A setup's state is held with the scene's tables, which a Shadow epoch's context does not reach.
				if (state.passes[found].setup && epoch.shape == Api::EpochShape::Shadow)
					::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} names pass {}, which has a setup, in a shadow epoch", epoch.id, key));
				// The stage word belongs to the depth commit, so its pass runs in the depth epoch before the build that reads the latch.
				if (found == latchPass || found == rowsPass || found == frameRowsPass) {
					const auto later = epoch.passes.subspan(k + 1);
					const bool beforeBuild = epoch.segment == static_cast<std::uint32_t>(RenderGraphRuntime::Segment::ZPrepass) && epoch.shape == Api::EpochShape::Main &&
					                         std::find(later.begin(), later.end(), kDepthBuild) != later.end();
					if (!beforeBuild)
						::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("the schedule's epoch {} names pass {}, which asks for the latch's {}, outside the depth epoch before {}",
							epoch.id, key, found == latchPass ? "stage word" : "animated rows", kDepthBuild));
					latchScheduled |= found == latchPass;
					rowsScheduled |= found == rowsPass;
					frameRowsScheduled |= found == frameRowsPass;
				}
				resolved.passes.push_back({ found, hosted });
			}
			// An epoch whose every pass is absent in this install has nothing to record or submit.
			const bool dormant = !resolved.passes.empty() && std::ranges::all_of(resolved.passes, [&](const ResolvedPass& a_pass) {
				const auto present = state.passes[a_pass.pass].present;
				return present && !present();
			});
			if (dormant) {
				state.traits[row].dormant = true;
				logger::info("[DCLF] schedule: the {} epoch is dormant (none of its passes is made in this install); it is never recorded or submitted", epoch.id);
			}
			state.epochs[shape].push_back(std::move(resolved));
		}
		if (latchPass != ~0u) {
			if (!latchScheduled)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("pass {} asks for the latch's stage word, but the schedule does not name it", state.passes[latchPass].key));
			state.latchWord = state.passes[latchPass].latchWord;
		}
		if (rowsPass != ~0u) {
			if (!rowsScheduled)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("pass {} asks for the latch's animated rows, but the schedule does not name it", state.passes[rowsPass].key));
			state.animatedRows = state.passes[rowsPass].animatedRows;
		}
		if (frameRowsPass != ~0u) {
			if (!frameRowsScheduled)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("pass {} asks for the latch's frame rows, but the schedule does not name it", state.passes[frameRowsPass].key));
			state.frameRows = state.passes[frameRowsPass].frameRows;
		}
	}

	std::vector<std::shared_ptr<const void>> SetUpPassStates(rhi::Device a_device, const SceneBuffers& a_scene)
	{
		const auto& passes = State().passes;
		std::vector<std::shared_ptr<const void>> states(passes.size());
		const Api::PassSetup setup{ a_device, a_scene };
		for (std::size_t i = 0; i < passes.size(); ++i)
			if (passes[i].setup)
				states[i] = passes[i].setup(setup);
		return states;
	}

	std::uint32_t PassLatchWord()
	{
		const auto latchWord = State().latchWord;
		return latchWord ? latchWord() : 0u;
	}

	Api::FrameRows PassAnimatedRows()
	{
		const auto animatedRows = State().animatedRows;
		return animatedRows ? animatedRows() : Api::FrameRows{};
	}

	Api::FrameRows PassFrameRows()
	{
		const auto frameRows = State().frameRows;
		return frameRows ? frameRows() : Api::FrameRows{};
	}

	Api::TriggerId CoreTriggerOf(CoreTrigger a_trigger)
	{
		return State().coreTriggers[static_cast<std::size_t>(a_trigger)];
	}

	std::uint32_t EpochTrigger(std::uint32_t a_epoch)
	{
		return State().epochTriggers[a_epoch];
	}

	std::string_view TriggerName(std::uint32_t a_trigger)
	{
		const auto& triggers = State().triggers;
		return a_trigger < triggers.size() ? std::string_view(triggers[a_trigger]) : std::string_view("none");
	}

	std::uint32_t EpochCount()
	{
		return State().epochCount;
	}

	const EpochTraits& EpochTraitsOf(std::uint32_t a_epoch)
	{
		return State().traits[a_epoch];
	}

	std::uint32_t EpochOfShape(ShapeKind a_shape, std::uint32_t a_index)
	{
		const auto& state = State();
		switch (a_shape) {
		case ShapeKind::Main:
			return a_index < state.mainEpochs.size() ? state.mainEpochs[a_index] : kNoEpoch;
		case ShapeKind::Shadow:
			return a_index < state.shadowEpochs.size() ? state.shadowEpochs[a_index] : kNoEpoch;
		default:
			return a_index < state.views.size() ? state.views[a_index].epochs[0] : kNoEpoch;
		}
	}

	std::uint32_t EpochById(std::string_view a_id)
	{
		const auto& state = State();
		for (std::uint32_t e = 0; e < state.epochCount; ++e)
			if (state.ids[e] == a_id)
				return e;
		return kNoEpoch;
	}

	const ViewEpochs& ViewEpochsOf(ViewKind a_kind)
	{
		return State().views[ViewIndex(a_kind)];
	}

	bool CpuPipelineRecords(std::uint32_t a_epoch)
	{
		const auto& state = State();
		if (a_epoch >= state.epochCount || state.traits[a_epoch].hostedBy != kNoEpoch)
			return false;
		for (const auto& epochs : state.epochs)
			for (const auto& epoch : epochs)
				if (epoch.id == state.ids[a_epoch])
					return std::any_of(epoch.passes.begin(), epoch.passes.end(), [&](const ResolvedPass& a_pass) {
						const auto& entry = state.passes[a_pass.pass];
						return entry.cpuPipeline && entry.make;
					});
		return false;
	}

	bool CpuPipelineSegmentRecords(RenderGraphRuntime::Segment a_segment)
	{
		const auto& state = State();
		for (std::uint32_t e = 0; e < state.epochCount; ++e)
			if (state.traits[e].segment == a_segment && CpuPipelineRecords(e))
				return true;
		return false;
	}

	std::vector<std::uint32_t> CpuPipelineEpochs()
	{
		const auto& state = State();
		std::vector<std::uint32_t> out;
		for (std::uint32_t e = 0; e < state.epochCount; ++e)
			if (CpuPipelineRecords(e))
				out.push_back(RenderGraphRuntime::EpochOf(state.traits[e].segment));
		return out;
	}

	std::vector<std::uint32_t> ExtensionEpochs(Api::EpochShape a_shape, Api::EngineView a_view)
	{
		const auto& state = State();
		std::vector<std::uint32_t> out;
		const auto add = [&](RenderGraphRuntime::Segment a_segment) {
			const auto epoch = RenderGraphRuntime::EpochOf(a_segment);
			if (std::ranges::find(out, epoch) == out.end())
				out.push_back(epoch);
		};
		const auto& shapes = state.epochs[static_cast<std::size_t>(a_shape)];
		for (const auto& epoch : shapes)
			if (a_shape != Api::EpochShape::View || epoch.view == a_view)
				add(epoch.segment);
		// Main epochs hosting a view's epochs draw from its resources too.
		if (a_shape == Api::EpochShape::View)
			for (const auto& host : state.epochs[static_cast<std::size_t>(Api::EpochShape::Main)])
				for (std::uint32_t row = 0; row < state.epochCount; ++row)
					if (((host.hosts >> row) & 1) && state.traits[row].index == static_cast<std::uint32_t>(a_view))
						add(host.segment);
		// Before the schedule is resolved, every DCLF epoch.
		if (out.empty())
			for (std::uint32_t segment = 0; segment < static_cast<std::uint32_t>(RenderGraphRuntime::Segment::Count); ++segment)
				if (DCLF::IsDclfSegment(static_cast<RenderGraphRuntime::Segment>(segment)))
					add(static_cast<RenderGraphRuntime::Segment>(segment));
		return out;
	}

	void GatherScheduled(Api::EpochShape a_shape, Api::EngineView a_view, const Api::PassContext& a_base, std::uint32_t a_graphEpoch,
		std::vector<org::RenderGraph::ExternalPassDesc>& a_out)
	{
		const auto& state = State();
		// Build from the build state's views, pass states and conditions, never the live resources or the attached conditions.
		const auto* build = a_base.build;
		if (!build) {
			static std::atomic<bool> logged{ false };
			if (!logged.exchange(true))
				logger::error("[DCLF] a graph build gathered its passes without a build state; its epochs get no passes");
			return;
		}
		// In the CPU pipeline graph, the direct sites draw at their triggers and only CPU-pipeline passes are kept, all on the graphics queue.
		const auto& conditions = build->Conditions();
		const bool cpu = conditions.cpuPipeline;
		const auto cpuPass = [](const PassEntry& a_entry) { return a_entry.cpuPipeline && a_entry.make; };
		for (const auto& epoch : state.epochs[static_cast<std::size_t>(a_shape)]) {
			if (a_shape == Api::EpochShape::View && epoch.view != a_view)
				continue;
			auto context = a_base;
			context.build = build;
			context.epochId = epoch.id;
			context.segment = epoch.segment;
			context.epoch = RenderGraphRuntime::EpochOf(epoch.segment);
			// Another epoch's graph would drop these passes, so they are not made.
			constexpr std::uint32_t kAllEpochs = UINT32_MAX;  // org::persistent::AllEpochs
			if (a_graphEpoch != kAllEpochs && context.epoch != kAllEpochs && context.epoch != a_graphEpoch)
				continue;
			// The view epochs this epoch hosts. Each is added only while its view's passes can be, and the epoch's own passes see them.
			std::array<std::shared_ptr<ViewResources>, kViews> hostedViews{};
			std::vector<std::shared_ptr<ViewResources>> hosted;
			for (std::uint32_t row = 0; a_shape == Api::EpochShape::Main && row < state.epochCount; ++row) {
				if (!((epoch.hosts >> row) & 1))
					continue;
				const auto view = state.traits[row].index;
				auto resources = build->mainViews[view];
				const auto* viewBuild = resources ? build->ViewOf(resources.get()) : nullptr;
				if (viewBuild && ViewPassesReady(*viewBuild, *context.Main()))
					hosted.push_back(hostedViews[view] = std::move(resources));
			}
			context.hosted = hosted;
			Api::PassOut out(a_out, context.epoch);
			for (const auto& [index, hostedEpoch] : epoch.passes) {
				const auto& entry = state.passes[index];
				context.view = a_base.view;
				if (hostedEpoch != kNoEpoch) {
					const auto& view = hostedViews[state.traits[hostedEpoch].index];
					if (!view)
						continue;
					context.view = &view;
				}
				if (cpu ? !cpuPass(entry) : entry.recordAtExecution && !entry.gpuLate)
					continue;
				if (entry.emit) {
					if (!cpu)
						entry.emit(context, out);
					continue;
				}
				// Only a pass with a setup gets a state.
				context.state = nullptr;
				if (entry.setup) {
					const auto& states = build->passStates;
					if (index >= states.size() || !states[index])
						continue;
					context.state = states[index];
				}
				if (auto pass = entry.make(context))
					AddPass(a_out, entry.kind, NameOf(entry, context), std::move(pass), context.epoch,
						cpu && entry.queue == Api::PassQueue::Compute ? Api::PassQueue::Default : entry.queue, entry.recordAtExecution);
			}
		}
	}
}

namespace DCLF::Extensions
{
	const FrameOrders& ScheduledFrameOrders()
	{
		return Draws::State().orders;
	}
}

namespace DCLF::Api
{
	const Draws::GraphConditions& PassContext::Conditions() const
	{
		return Build().Conditions();
	}

	bool PassContext::PhaseTwo() const
	{
		return Main()->hzb && Conditions().twoPassOcclusion;
	}

	bool PassContext::Interior() const
	{
		return Conditions().interior;
	}

	void PassOut::Add(std::string a_name, PassKind a_kind, std::shared_ptr<org::RenderPass> a_pass, PassQueue a_queue, bool a_recordAtExecution)
	{
		Draws::AddPass(out, a_kind, std::move(a_name), std::move(a_pass), epoch, a_queue, a_recordAtExecution);
	}
}
