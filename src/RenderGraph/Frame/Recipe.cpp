#include "RenderGraph/Frame/Recipe.h"
#include "RenderGraph/Frame/Transients.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/Frame/RecipeTable.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/RenderGraph/ScheduleTrace.h>

#	include <algorithm>
#	include <map>
#	include <mutex>
#	include <set>
#	include <string>
#	include <vector>

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
					if (stage.epochs[e].role != Table::Role::Placed)
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
			std::uint32_t dclfLater = 0;  // resources DCLF's stages declare and only later stages write
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

		struct Pass
		{
			std::uint32_t stage = 0;
			std::string name;
			std::vector<std::string> reads, writes;
		};

		std::string ResourceName(const org::ScheduleTrace& a_trace, std::uint32_t a_index)
		{
			if (a_index >= a_trace.resources.size())
				return fmt::format("#{}", a_index);
			const auto& resource = a_trace.resources[a_index];
			return resource.name.empty() ? fmt::format("#{}", resource.id) : resource.name;
		}

		/** @brief The passes of a_trace in plan order, by batch then position. */
		void AppendPasses(std::uint32_t a_stage, const org::ScheduleTrace& a_trace, std::vector<Pass>& a_out)
		{
			std::vector<const org::ScheduleTrace::Pass*> ordered;
			for (const auto& pass : a_trace.passes)
				ordered.push_back(&pass);
			std::ranges::stable_sort(ordered, [](const auto* a_left, const auto* a_right) {
				return std::tie(a_left->batch, a_left->position) < std::tie(a_right->batch, a_right->position);
			});
			for (const auto* pass : ordered) {
				Pass out{ .stage = a_stage, .name = pass->name };
				for (const auto& access : pass->accesses) {
					auto name = ResourceName(a_trace, access.resource);
					auto& list = access.write ? out.writes : out.reads;
					if (std::ranges::find(list, name) == list.end())
						list.push_back(std::move(name));
				}
				a_out.push_back(std::move(out));
			}
		}

		/** @brief The recipe's passes in order. Epoch a_epoch's come from a_trace when given. */
		std::vector<Pass> RecipePasses(const State& a_state, std::uint32_t a_epoch, const org::ScheduleTrace* a_trace)
		{
			std::vector<Pass> passes;
			ForCheckedEpochs([&](std::uint32_t a_stage, const Table::Epoch& a_entry) {
				const auto epoch = EpochOfSegment(a_entry.segment);
				if (a_trace && epoch == a_epoch) {
					AppendPasses(a_stage, *a_trace, passes);
					return;
				}
				const auto found = a_state.traces.find(epoch);
				if (found != a_state.traces.end() && found->second.trace)
					AppendPasses(a_stage, *found->second.trace, passes);
			});
			return passes;
		}

		/** @brief Logs each finding once. In a development build the first one stops the game. */
		void Report(State& a_state, const std::vector<std::string>& a_findings, std::string_view a_when)
		{
			for (const auto& finding : a_findings) {
				if (!a_state.loggedFindings.insert(finding).second)
					continue;
				const bool fatal = Switches::Number("CS_ORG_RECIPE_FATAL", Driver::MismatchFatal() ? 1 : 0) != 0;
				logger::error("[ORG] world recipe{}: {}", a_when, finding);
				if (fatal) {
					const auto message = fmt::format("Community Shaders native mode: the world recipe reads a resource before the pass that writes it{}: {}", a_when, finding);
					logger::critical("[ORG] world recipe: {}", message);
					spdlog::default_logger()->flush();
					stl::report_and_fail(message);
				}
			}
		}

		std::string Join(const std::vector<std::string>& a_items)
		{
			std::string text;
			for (const auto& item : a_items)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", item);
			return text;
		}

		/**
		 * @brief Checks read order between stages, since a stage's graph orders its own passes.
		 * A read with no writer comes from outside the graphs. One whose writers are all later must be declared in kPreviousFrame.
		 * A CS transient read with no writer in its own stage is a finding, since absent passes may drop the writer.
		 */
		void Validate(State& a_state, const std::vector<Pass>& a_passes, std::vector<std::string>& a_findings)
		{
			std::map<std::uint32_t, std::set<std::string>> stageWrites;  // what each stage's passes write
			for (const auto& pass : a_passes)
				for (const auto& name : pass.writes)
					stageWrites[pass.stage].insert(name);
			std::set<std::string> unwritten;
			for (const auto& pass : a_passes) {
				if (Table::kStageTable[pass.stage].owner != Owner::Cs)
					continue;
				for (const auto& name : pass.reads)
					if (!stageWrites[pass.stage].contains(name) && GraphResources::IsTransientName(name) && unwritten.insert(name).second)
						a_findings.push_back(fmt::format("{} ({}) reads the transient {}, which no present pass of its stage writes: a pass that writes it is absent "
														 "from the build while its reader is present. Keep the writer present whenever a reader is, or give the "
														 "resource a lifetime across frames",
							pass.name, StageName(static_cast<StageId>(pass.stage)), name));
			}
			std::map<std::string, std::uint32_t> firstWriter;  // the first pass that writes each resource
			for (std::uint32_t i = 0; i < a_passes.size(); ++i)
				for (const auto& name : a_passes[i].writes)
					firstWriter.try_emplace(name, i);
			std::set<std::string> previous, outside, found, dclfLater;
			for (std::uint32_t i = 0; i < a_passes.size(); ++i)
				for (const auto& name : a_passes[i].reads) {
					const auto writer = firstWriter.find(name);
					if (writer == firstWriter.end()) {
						outside.insert(name);
						continue;
					}
					if (a_passes[writer->second].stage <= a_passes[i].stage)
						continue;
					if (PreviousFrame(name)) {
						previous.insert(name);
						continue;
					}
					if (Table::kStageTable[a_passes[i].stage].owner == Owner::Dclf) {
						dclfLater.insert(name);
						continue;
					}
					if (!found.insert(name).second)
						continue;
					a_findings.push_back(fmt::format("{} ({}) reads {}, which no earlier stage writes; its first writer is {} ({}). Declare it as the previous "
													 "frame's (RecipeTable.h, kPreviousFrame) once checked, or move the stage",
						a_passes[i].name, StageName(static_cast<StageId>(a_passes[i].stage)), name, a_passes[writer->second].name,
						StageName(static_cast<StageId>(a_passes[writer->second].stage))));
				}
			// Check ORG's derived frame-transient spans against the traces, since a pass outside the span uses bytes another resource holds.
			// The first pass that uses one must write it, since the bytes hold another resource's data.
			std::set<std::string> groupFindings, groupSeen;
			for (const auto& pass : a_passes) {
				const auto check = [&](const std::string& a_name, bool a_write) {
					const auto transient = Transients::FrameTransientOf(a_name);
					if (!transient)
						return;
					const auto span = transient->second;
					if ((span.Empty() || pass.stage < span.first || pass.stage > span.last) && groupFindings.insert(a_name + "@" + pass.name).second)
						a_findings.push_back(fmt::format("{} ({}) uses {} of {}, whose derived span in the frame arena is {}: other stages' transients may "
														 "hold its memory there. ORG's derivation missed this graph's use (org::GraphHost::AddFrameTransients)",
							pass.name, StageName(static_cast<StageId>(pass.stage)), a_name, transient->first, Transients::Describe(span)));
					if (groupSeen.insert(a_name).second && !a_write)
						a_findings.push_back(fmt::format("{} ({}) reads {} of {} first in the frame: its memory comes in holding another resource's data. "
														 "Its first pass must write it",
							pass.name, StageName(static_cast<StageId>(pass.stage)), a_name, transient->first));
				};
				for (const auto& name : pass.writes)
					check(name, true);
				for (const auto& name : pass.reads)
					check(name, std::ranges::find(pass.writes, name) != pass.writes.end());
			}
			a_state.passes = static_cast<std::uint32_t>(a_passes.size());
			a_state.previousInputs = static_cast<std::uint32_t>(previous.size());
			a_state.outsideInputs = static_cast<std::uint32_t>(outside.size());
			a_state.findings = static_cast<std::uint32_t>(a_findings.size());
			a_state.dclfLater = static_cast<std::uint32_t>(dclfLater.size());
		}
	}

	namespace
	{
		const char* OwnerMark(Owner a_owner) { return a_owner == Owner::Dclf ? "*" : ""; }

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
			if (a_epoch.role == Table::Role::Graph && !FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(a_epoch.segment)))
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

	/** @brief CS_ORG_GRAPH_RESOURCES_FRAME=<n> logs every attached epoch graph's resources at frame n, to match with a memory dump. */
	static void LogGraphResources(org::GraphHost& a_host)
	{
		static const auto at = Switches::Number("CS_ORG_GRAPH_RESOURCES_FRAME", 0);
		if (!at || Driver::Current().frame != at)
			return;
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
		std::uint64_t hash = 1469598103934665603ull;
		for (const auto& pass : passes)
			for (const char c : pass.name)
				hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
		if (hash == state.printedHash)
			return;
		state.printedHash = hash;
		if (++state.prints > 12) {
			++state.unprinted;
			return;
		}
		logger::info("[ORG] world recipe ({} passes; {} inputs from the previous frame, {} from outside the graphs, {} declared by Drawcall Limit Fix's stages "
					 "and written later; {} findings):",
			passes.size(), state.previousInputs, state.outsideInputs, state.dclfLater, findings.size());
		for (std::uint32_t s = 0; s < kStages; ++s) {
			std::string text;
			std::uint32_t count = 0;
			for (const auto& pass : passes) {
				if (pass.stage != s)
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

	void LogStats()
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		logger::info("[ORG] world recipe: {} passes, {} inputs from the previous frame, {} from outside the graphs, {} declared by Drawcall Limit Fix's stages and "
					 "written later, {} findings; {} schedules traced on the render owner; {} changed recipes not printed; {} builds checked before their attach",
			state.passes, state.previousInputs, state.outsideInputs, state.dclfLater, state.findings, state.fallbackTraces, state.unprinted, state.accepted);
	}
}

#else

namespace RenderGraph::Frame::Recipes
{
	const char* StageName(StageId) { return "?"; }
	std::vector<PositionedEpoch> PositionedEpochs() { return {}; }
	bool KindHas(Kind a_kind, bool a_capture, SegmentId) { return a_kind == Kind::World || (a_kind == Kind::FrozenWorld && a_capture); }
	std::string DescribeKind(Kind, bool) { return {}; }
	std::string DiffKinds(Kind, bool, Kind, bool) { return {}; }
	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph&, std::uint32_t) { return {}; }
	bool Traced(std::uint32_t) { return false; }
	void NoteTrace(std::uint32_t, std::uint64_t, std::shared_ptr<const org::ScheduleTrace>) {}
	void Accept(std::uint32_t, const std::shared_ptr<const org::ScheduleTrace>&) {}
	void Check(org::GraphHost&) {}
	void LogStats() {}
}

#endif
