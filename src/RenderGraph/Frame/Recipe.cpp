#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/Fatal.h"
#include "Utils/Hash.h"
#include "RenderGraph/Frame/Transients.h"

#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Frame/RecipeTable.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameClock.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/ReadOrderCheck.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Render/RenderGraph/ScheduleTrace.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace RenderGraph::Frame::Recipes
{
	namespace
	{
		using Segment = RenderGraphRuntime::Segment;

		namespace Table = Recipes::Table;

		/** @brief Visits every stage's own graphs. */
		template <class F>
		void ForCheckedEpochs(F&& a_visit)
		{
			for (std::uint32_t s = 0; s < kStages; ++s) {
				const auto& stage = Table::kStageTable[s];
				for (std::uint32_t e = 0; e < Table::EpochCount(stage); ++e)
					a_visit(s, stage.epochs[e]);
			}
		}

		const char* OwnerName(Owner a_owner)
		{
			return a_owner == Owner::Cs ? "CS, one ticket" : "Drawcall Limit Fix";
		}

		struct Traced
		{
			std::uint64_t generation = 0;
			std::shared_ptr<const org::ScheduleTrace> trace;
		};

		struct State
		{
			std::mutex mutex;
			std::map<std::uint32_t, Traced> traces;  // by epoch
			std::map<std::uint32_t, std::uint64_t> checked;  // by epoch, the generation the last check saw
			std::uint64_t printedHash = 0;
			std::uint32_t prints = 0, unprinted = 0;
			std::uint32_t passes = 0, previousInputs = 0, outsideInputs = 0, findings = 0, fallbackTraces = 0;
			std::uint32_t sourceLater = 0;  // resources the fragment sources' stages declare and only later stages write
			std::uint32_t accepted = 0;   // builds the check accepted before their attach
			std::set<std::string> loggedFindings;
			bool complete = false;  // a check has seen every epoch's build traced
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool PreviousFrame(std::string_view a_name)
		{
			return std::ranges::find(Table::kPreviousFrame, a_name) != Table::kPreviousFrame.end();
		}

		std::uint32_t EpochOfSegment(Segment a_segment) { return RenderGraphRuntime::EpochOf(a_segment); }

		using Pass = org::ReadOrderPass;

		/** @brief The recipe's passes in order. Epoch a_epoch's come from a_trace when given. */
		std::vector<Pass> RecipePasses(const State& a_state, std::uint32_t a_epoch, const org::ScheduleTrace* a_trace)
		{
			std::vector<Pass> passes;
			ForCheckedEpochs([&](std::uint32_t a_stage, const Table::Epoch& a_entry) {
				const auto epoch = EpochOfSegment(a_entry.segment);
				if (a_trace && epoch == a_epoch) {
					org::AppendReadOrderPasses(a_stage, *a_trace, passes);
					return;
				}
				const auto found = a_state.traces.find(epoch);
				if (found != a_state.traces.end() && found->second.trace)
					org::AppendReadOrderPasses(a_stage, *found->second.trace, passes);
			});
			return passes;
		}

		/** @brief Logs each finding once. In a development build the first one stops the game. */
		void Report(State& a_state, const std::vector<std::string>& a_findings, std::string_view a_when)
		{
			for (const auto& finding : a_findings) {
				if (!a_state.loggedFindings.insert(finding).second)
					continue;
				const bool fatal = Driver::MismatchFatal();
				logger::error("[ORG] world recipe{}: {}", a_when, finding);
				if (fatal)
					RenderGraph::Fatal("world recipe", fmt::format("{}: the world recipe reads a resource before the pass that writes it{}: {}", Driver::ModeName(), a_when, finding));
				Driver::RaiseViolationWarning("world recipe order");
			}
		}

		std::string Join(const std::vector<std::string>& a_items)
		{
			std::string text;
			for (const auto& item : a_items)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", item);
			return text;
		}

		const char* PassStage(const Pass& a_pass) { return StageName(static_cast<StageId>(a_pass.position)); }

		/**
		 * @brief Checks read order between stages with ORG's read-order check, since a stage's graph orders its own passes.
		 * A read with no writer comes from outside the graphs. One whose writers are all later must be declared in kPreviousFrame.
		 * A CS transient read with no writer in its own stage is a finding, since absent passes may drop the writer.
		 */
		void Validate(State& a_state, const std::vector<Pass>& a_passes, std::vector<std::string>& a_findings)
		{
			org::ReadOrderInputs inputs;
			inputs.requiresLocalWriter = [](std::uint32_t a_stage) { return Table::kStageTable[a_stage].owner == Owner::Cs; };
			// The fragment sources' stages declare resources that later stages write.
			inputs.allowsLaterWriter = [](std::uint32_t a_stage) { return Table::kStageTable[a_stage].owner == Owner::Fragment; };
			inputs.previousFrame = [](std::string_view a_name) { return PreviousFrame(a_name); };
			inputs.transient = [](std::string_view a_name) { return GraphResources::IsTransientName(a_name); };
			// ORG's derived frame-transient spans, checked against the traces.
			inputs.frameTransient = [](std::string_view a_name) -> std::optional<org::ReadOrderTransient> {
				const auto transient = Transients::FrameTransientOf(a_name);
				if (!transient)
					return std::nullopt;
				return org::ReadOrderTransient{ transient->first, { transient->second.first, transient->second.last } };
			};
			const auto report = org::CheckReadOrder(a_passes, inputs);
			using Kind = org::ReadOrderFinding::Kind;
			for (const auto& finding : report.findings) {
				const auto& pass = a_passes[finding.pass];
				const auto& name = finding.resource;
				switch (finding.kind) {
				case Kind::UnwrittenTransient:
					a_findings.push_back(fmt::format("{} ({}) reads the transient {}, which no present pass of its stage writes: a pass that writes it is absent "
													 "from the build while its reader is present. Keep the writer present whenever a reader is, or give the "
													 "resource a lifetime across frames",
						pass.name, PassStage(pass), name));
					break;
				case Kind::LaterWriter:
					a_findings.push_back(fmt::format("{} ({}) reads {}, which no earlier stage writes; its first writer is {} ({}). Declare it as the previous "
													 "frame's (RecipeTable.h, kPreviousFrame) once checked, or move the stage",
						pass.name, PassStage(pass), name, a_passes[finding.writer].name, PassStage(a_passes[finding.writer])));
					break;
				case Kind::OutsideSpan:
					// A pass outside the span uses bytes another resource holds.
					a_findings.push_back(fmt::format("{} ({}) uses {} of {}, whose derived span in the frame arena is {}: other stages' transients may "
													 "hold its memory there. ORG's derivation missed this graph's use (org::GraphHost::AddFrameTransients)",
						pass.name, PassStage(pass), name, finding.transient.group,
						Transients::Describe(Transients::Span{ finding.transient.span.first, finding.transient.span.last })));
					break;
				case Kind::FirstUseReads:
					a_findings.push_back(fmt::format("{} ({}) reads {} of {} first in the frame: its memory comes in holding another resource's data. "
													 "Its first pass must write it",
						pass.name, PassStage(pass), name, finding.transient.group));
					break;
				}
			}
			a_state.passes = static_cast<std::uint32_t>(a_passes.size());
			a_state.previousInputs = report.previousInputs;
			a_state.outsideInputs = report.outsideInputs;
			a_state.findings = static_cast<std::uint32_t>(a_findings.size());
			a_state.sourceLater = report.laterDeclared;
		}
	}

	namespace
	{
		const char* OwnerMark(Owner a_owner) { return a_owner == Owner::Fragment ? "*" : ""; }

		bool SameStages(const Table::KindRecipe& a_left, const Table::KindRecipe& a_right)
		{
			if (a_left.count != a_right.count)
				return false;
			for (std::uint32_t i = 0; i < a_left.count; ++i)
				if (a_left.stages[i] != a_right.stages[i])
					return false;
			return true;
		}
	}

	bool KindHas(Kind a_kind, bool a_capture, SegmentId a_segment)
	{
		const auto& recipe = Table::KindOf(a_kind, a_capture);
		for (std::uint32_t i = 0; i < recipe.count; ++i)
			if (Table::StageOf(recipe.stages[i]).segment == a_segment)
				return true;
		return false;
	}

	std::string DescribeKind(Kind a_kind, bool a_capture)
	{
		const auto& recipe = Table::KindOf(a_kind, a_capture);
		std::string line;
		for (std::uint32_t i = 0; i < recipe.count; ++i) {
			const auto& stage = Table::StageOf(recipe.stages[i]);
			line += fmt::format("{}{}{}", i ? " | " : "", stage.name, OwnerMark(stage.owner));
		}
		return line;
	}

	std::string DiffKinds(Kind a_from, bool a_fromCapture, Kind a_to, bool a_toCapture)
	{
		const auto& from = Table::KindOf(a_from, a_fromCapture);
		const auto& to = Table::KindOf(a_to, a_toCapture);
		if (SameStages(from, to))
			return {};
		const auto names = [](const Table::KindRecipe& a_recipe, const Table::KindRecipe& a_other) {
			std::string list;
			for (std::uint32_t i = 0; i < a_recipe.count; ++i) {
				bool shared = false;
				for (std::uint32_t j = 0; j < a_other.count; ++j)
					shared = shared || a_recipe.stages[i] == a_other.stages[j];
				if (!shared)
					list += fmt::format("{}{}", list.empty() ? "" : ", ", Table::StageOf(a_recipe.stages[i]).name);
			}
			return list.empty() ? std::string("none") : list;
		};
		return fmt::format("recipe {} -> {}; stages added: {}; removed: {}", DescribeKind(a_from, a_fromCapture), DescribeKind(a_to, a_toCapture), names(to, from),
			names(from, to));
	}

	const char* StageName(StageId a_stage)
	{
		// The table's names are string literals, so their data is terminated.
		return static_cast<std::uint32_t>(a_stage) < kStages ? Table::StageOf(a_stage).name.data() : "?";
	}

	std::vector<PositionedEpoch> PositionedEpochs()
	{
		std::vector<PositionedEpoch> out;
		// A stage's own graphs only. HDR output and frame generation keep their own tickets and stay outside the frame arena.
		ForCheckedEpochs([&](std::uint32_t a_stage, const Table::Epoch& a_epoch) {
			if (!FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(a_epoch.segment)))
				out.push_back({ EpochOfSegment(a_epoch.segment), Table::kStageTable[a_stage].id, Table::kStageTable[a_stage].owner });
		});
		return out;
	}

	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph& a_graph, std::uint32_t a_epoch)
	{
		// Without a split, a host epoch's whole executable is the epoch's.
		if (auto trace = a_graph.TraceSelectedSchedule(a_epoch))
			return trace;
		return a_graph.TraceSelectedSchedule(UINT32_MAX);
	}

	bool Traced(std::uint32_t a_epoch)
	{
		bool traced = false;
		ForCheckedEpochs([&](std::uint32_t, const Table::Epoch& a_entry) {
			traced = traced || (EpochOfSegment(a_entry.segment) == a_epoch && !FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(a_entry.segment)));
		});
		return traced;
	}

	void NoteTrace(std::uint32_t a_epoch, std::uint64_t a_generation, std::shared_ptr<const org::ScheduleTrace> a_trace)
	{
		if (!a_trace)
			return;
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		state.traces[a_epoch] = { a_generation, std::move(a_trace) };
	}

	/**
	 * @brief CS_ORG_GRAPH_RESOURCES_FRAME=<n> logs every attached epoch graph's resources at frame n, to match with a memory dump.
	 * It also writes each epoch's whole schedule (passes in order, their accesses, barriers and waits) to
	 * graph-trace-<n>/epoch-<epoch>.txt in the log folder, to see which pass writes what another epoch reads.
	 */
	static std::atomic_bool graphTraceRequested{ false };

	void RequestGraphTrace() { graphTraceRequested.store(true, std::memory_order_relaxed); }

	static void LogGraphResources(org::GraphHost& a_host)
	{
		static const auto at = Switches::Number("CS_ORG_GRAPH_RESOURCES_FRAME", 0);
		const auto frame = RenderGraph::FrameClock::BeginFrame();
		const bool requested = graphTraceRequested.exchange(false, std::memory_order_relaxed);
		if (!requested && (!at || frame != at))
			return;
		std::filesystem::path traceDir;
		if (const auto logDir = logger::log_directory()) {
			traceDir = *logDir / fmt::format("graph-trace-{}", frame);
			std::error_code error;
			std::filesystem::create_directories(traceDir, error);
			if (error)
				traceDir.clear();
		}
		for (std::size_t i = 0; i < a_host.GraphCount(); ++i) {
			auto* graph = a_host.GraphAt(i);
			const auto epoch = a_host.EpochAt(i);
			if (!graph)
				continue;
			const auto trace = TraceEpochGraph(*graph, epoch);
			if (!trace)
				continue;
			std::set<std::string> names;
			for (const auto& resource : trace->resources)
				names.insert(resource.name.empty() ? fmt::format("#{}", resource.id) : resource.name);
			std::string text;
			for (const auto& name : names)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", name);
			logger::info("[ORG] graph resources: epoch {} build {} ({} passes, {} resources): {}", epoch, a_host.BuildGeneration(epoch), trace->passes.size(), names.size(),
				text);
			if (!traceDir.empty()) {
				std::ofstream file(traceDir / fmt::format("epoch-{}.txt", epoch), std::ios::binary);
				file << org::ScheduleTraceToText(*trace);
			}
		}
	}

	void Check(org::GraphHost& a_host)
	{
		LogGraphResources(a_host);
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		// Epochs whose build changed since the last check. A build the host made itself is traced here, at most two a frame.
		bool changed = false;
		bool complete = true;  // the recipe is checked only whole
		std::uint32_t traced = 0;
		ForCheckedEpochs([&](std::uint32_t, const Table::Epoch& a_entry) {
			if (FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(a_entry.segment)))
				return;
			const auto epoch = EpochOfSegment(a_entry.segment);
			auto* graph = a_host.Graph(epoch);
			if (!graph)
				return;
			const auto generation = a_host.BuildGeneration(epoch);
			if (const auto seen = state.checked.find(epoch); seen != state.checked.end() && seen->second == generation)
				return;
			auto& entry = state.traces[epoch];
			if (entry.generation != generation || !entry.trace) {
				auto trace = traced < 2 ? TraceEpochGraph(*graph, epoch) : nullptr;
				if (!trace) {
					complete = false;  // the next frame tries again
					return;
				}
				++traced;
				++state.fallbackTraces;
				entry = { generation, std::move(trace) };
			}
			state.checked[epoch] = generation;
			changed = true;
		});
		if (!changed || !complete)
			return;
		state.complete = true;
		const auto passes = RecipePasses(state, UINT32_MAX, nullptr);
		std::vector<std::string> findings;
		Validate(state, passes, findings);
		Report(state, findings, "");
		// Printed when its passes changed.
		Util::Fnv1a fnv;
		for (const auto& pass : passes)
			fnv.Bytes(pass.name);
		const std::uint64_t hash = fnv.Value();
		if (hash == state.printedHash)
			return;
		state.printedHash = hash;
		if (++state.prints > 12) {
			++state.unprinted;
			return;
		}
		logger::info("[ORG] world recipe ({} passes; {} inputs from the previous frame, {} from outside the graphs, {} declared by the fragment sources' stages "
					 "and written later; {} findings):",
			passes.size(), state.previousInputs, state.outsideInputs, state.sourceLater, findings.size());
		for (std::uint32_t s = 0; s < kStages; ++s) {
			std::string text;
			std::uint32_t count = 0;
			for (const auto& pass : passes) {
				if (pass.position != s)
					continue;
				++count;
				text += fmt::format("{}{} [reads {}; writes {}]", text.empty() ? "" : "; ", pass.name, Join(pass.reads), Join(pass.writes));
			}
			const auto& stage = Table::kStageTable[s];
			logger::info("[ORG] world recipe: {} {} ({}; commits at {}; {} passes): {}", s + 1, stage.name, OwnerName(stage.owner), stage.commit, count, text);
		}
	}

	void Accept(std::uint32_t a_epoch, const std::shared_ptr<const org::ScheduleTrace>& a_trace)
	{
		if (!a_trace)
			return;
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		// Before the first whole check a later stage's writer could look like the first one.
		if (!state.complete)
			return;
		State scratch;
		std::vector<std::string> findings;
		Validate(scratch, RecipePasses(state, a_epoch, a_trace.get()), findings);
		++state.accepted;
		Report(state, findings, fmt::format(" (epoch {}'s new build, before its attach)", a_epoch));
	}
}

