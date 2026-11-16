#include "RenderGraph/Frame/Recipe.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/FrameAnchors.h"
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

		constexpr std::uint32_t kMaxEpochs = 8;

		/** @brief One stage of the world recipe: its owner, its epochs in frame order, and where it commits. */
		struct StageDesc
		{
			StageId id;
			Owner owner;
			std::array<Segment, kMaxEpochs> epochs;
			std::uint32_t epochCount;
			const char* commit;
		};

		// The world recipe. Recipe order is pass order. The CS stages are FrameAnchors' declared segments; their commit points and why
		// each is safe are in FrameAnchors.cpp (kDeclaredInfo). Epochs of anchors not moved yet keep their tickets at their hooks.
		constexpr std::array<StageDesc, kStages> kWorld{ {
			// The decal tests keep their anchor's own epoch, before the stage's first tap (several a frame, each with its own query range).
			// The reflection faces' clears outside world frames keep the cube clear anchor's ticket.
			{ StageId::Start, Owner::Cs, { Segment::AnchorDecalQueries, Segment::DeclaredStart, Segment::AnchorEngineCubeClear }, 3,
				"just before DCLF's shadow view epoch (the last face's sky)" },
			{ StageId::Shadows, Owner::Dclf, { Segment::AnchorEngineShadowClear, Segment::ShadowView, Segment::SkyOcclusion }, 3, "Drawcall Limit Fix's shadow views" },
			{ StageId::PreDepth, Owner::Cs, { Segment::DeclaredPreDepth }, 1, "the EarlyPrepass hook (Main::RenderShadowMaps returned)" },
			// Drawcall Limit Fix's water reflection faces come after the Z-prepass (TESWaterReflections::Update).
			{ StageId::Depth, Owner::Dclf, { Segment::ZPrepass, Segment::FirstPersonDepth, Segment::Reflection }, 3, "Main::RenderDepth" },
			{ StageId::PreMain, Owner::Cs, { Segment::DeclaredPreMain }, 1, "just before DCLF's main opaque epoch (the main pass's sky)" },
			{ StageId::Opaque, Owner::Dclf, { Segment::MainOpaque }, 1, "the main pass's opaque batches" },
			// The EngineDraws, ImageSpace and EngineCopy anchors are not listed: their epochs hold work of several places in the frame
			// (menus, other frame kinds), so no one place in the recipe is theirs.
			{ StageId::Deferred, Owner::Cs, { Segment::DeclaredDeferred }, 1, "just before DCLF's forward epoch (the water blend's end)" },
			// The EngineDraws anchor's own epoch keeps the clients no stage holds (blood splatter, precipitation, the menu scenes): in a
			// world frame they run after the water.
			{ StageId::Forward, Owner::Dclf, { Segment::AnchorEngineDraws, Segment::AnchorEngineFrameClear, Segment::Forward, Segment::AnchorEngineQueries }, 4,
				"the engine's blended draw" },
			{ StageId::FirstPerson, Owner::Dclf, { Segment::FirstPerson }, 1, "Main::RenderFirstPersonView" },
			{ StageId::Post, Owner::Cs,
				{ Segment::DeclaredPost, Segment::AnchorTonemap, Segment::AnchorFrameGeneration },
				3, "RenderEndOfFrameEffects' return" },
			// The UI stage holds Scaleform's displays (fixed instances; the Interface anchor keeps its epoch for the other frame kinds) and
			// the overlay (the Overlay anchor has no epoch of its own). The movie tiles and the present's clients come before it and keep their
			// tickets; HDR output and the capture follow its commit.
			{ StageId::Ui, Owner::Cs,
				{ Segment::AnchorMovieTiles, Segment::AnchorInterface, Segment::AnchorPresent, Segment::DeclaredUi, Segment::AnchorHDROutput, Segment::AnchorCapture },
				6, "just before the present (or HDR output, or the capture)" },
		} };

		/**
		 * @brief Resources a stage reads before the stage that first writes them in the frame: the previous frame's contents, each
		 * checked by hand against the stage that writes it. Exact graph resource names.
		 * - written by forward or post and read earlier in the next frame: the refraction normals, the underwater mask, the water
		 *   displacement pair and the water flow map (the water surface), kMAIN_COPY (the water surface);
		 * - written by the deferred stages and read by the start stage's face sky: the sky's IBL and Terrain Blending's blended depth;
		 * - the depth copy Volumetric Lighting makes in post, read by the menu scenes' local map fog (the EngineDraws anchor's own epoch);
		 * - the dynamic cubemaps the deferred stage compresses, read by the start stage's face sky.
		 * Drawcall Limit Fix's stages are not held to it: their graphs declare the whole binding table their shaders may read (the light
		 * lists, the shadow mask, the luminance chain), so they "read" much that CS writes later; the check counts those apart.
		 */
		constexpr std::array<std::string_view, 11> kPreviousFrame{ "DynamicCubemaps::EnvTextureBC6H", "DynamicCubemaps::EnvReflectionsBC6H", "Engine::kREFRACTION_NORMALS", "Engine::kUNDERWATER_MASK", "Engine::kWATER_DISPLACEMENT",
			"Engine::kWATER_DISPLACEMENT_SWAP", "Engine::Texture2D 320x320", "Engine::kMAIN_COPY", "IBL::SkyIBL",
			"TerrainBlending::BlendedDepth16", "Engine::Depth::kMAIN_COPY" };

		const char* OwnerName(Owner a_owner)
		{
			switch (a_owner) {
			case Owner::Cs:
				return "CS, one ticket";
			case Owner::Dclf:
				return "Drawcall Limit Fix";
			default:
				return "tickets at hooks";
			}
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
			std::map<std::uint32_t, std::uint64_t> checked;  // by epoch: the generation the last check saw
			std::uint64_t printedHash = 0;
			std::uint32_t prints = 0, unprinted = 0;
			std::uint32_t passes = 0, previousInputs = 0, outsideInputs = 0, findings = 0, fallbackTraces = 0;
			std::uint32_t dclfLater = 0;  // resources Drawcall Limit Fix's stages declare and only later stages write
			std::set<std::string> loggedFindings;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool PreviousFrame(std::string_view a_name)
		{
			return std::ranges::find(kPreviousFrame, a_name) != kPreviousFrame.end();
		}

		std::uint32_t EpochOfSegment(Segment a_segment) { return RenderGraphRuntime::EpochOf(a_segment); }

		/** @brief One pass of the recipe as the check reads it. */
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

		/** @brief The passes of a_trace in their plan order (batch, then position). */
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

		std::string Join(const std::vector<std::string>& a_items)
		{
			std::string text;
			for (const auto& item : a_items)
				text += fmt::format("{}{}", text.empty() ? "" : ", ", item);
			return text;
		}

		/**
		 * @brief The check (BasicRenderer's PipelineRecipe::Validate): walks the passes in recipe order. Within one stage the stage's
		 * graph orders its own passes by what they declare, so the check is between stages. A read whose resource a stage written
		 * earlier in the recipe (or the reading stage itself) writes is fine. One with no writer anywhere in the frame comes from outside
		 * the graphs (uploads, files, the engine). One whose writers are all in later stages is the previous frame's: it must be declared
		 * so (kPreviousFrame), else it is a finding.
		 */
		void Validate(State& a_state, const std::vector<Pass>& a_passes, std::vector<std::string>& a_findings)
		{
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
					if (kWorld[a_passes[i].stage].owner == Owner::Dclf) {
						dclfLater.insert(name);
						continue;
					}
					if (!found.insert(name).second)
						continue;
					a_findings.push_back(fmt::format("{} ({}) reads {}, which no earlier stage writes; its first writer is {} ({}). Declare it as the previous "
													 "frame's (Recipe.cpp, kPreviousFrame) once checked, or move the stage",
						a_passes[i].name, StageName(static_cast<StageId>(a_passes[i].stage)), name, a_passes[writer->second].name,
						StageName(static_cast<StageId>(a_passes[writer->second].stage))));
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
		/** @brief One stage of a frame kind's recipe: a name, its owner, and the CS stage it is (SegmentId::Count for none). */
		struct KindStage
		{
			const char* name;
			Owner owner;
			SegmentId segment;
		};

		constexpr std::uint32_t kKindStages = 11;

		/** @brief A frame kind's recipe: its stages in frame order. */
		struct KindRecipe
		{
			std::array<KindStage, kKindStages> stages;
			std::uint32_t count;
		};

		// The world recipe (kWorld above), by CS stage. A frozen world's capture frame draws the world once, with the same recipe.
		constexpr KindRecipe kWorldKind{ { {
			{ "start", Owner::Cs, SegmentId::Start },
			{ "shadows", Owner::Dclf, SegmentId::Count },
			{ "pre-depth", Owner::Cs, SegmentId::PreDepth },
			{ "depth", Owner::Dclf, SegmentId::Count },
			{ "pre-main", Owner::Cs, SegmentId::PreMain },
			{ "opaque", Owner::Dclf, SegmentId::Count },
			{ "deferred+water", Owner::Cs, SegmentId::Deferred },
			{ "forward", Owner::Dclf, SegmentId::Count },
			{ "first person", Owner::Dclf, SegmentId::Count },
			{ "post", Owner::Cs, SegmentId::Post },
			{ "UI", Owner::Cs, SegmentId::Ui },
		} }, 11 };
		// A menu over a frozen world (after its capture frame): the engine composites the frozen frame (ImageSpace), a menu's 3D scene
		// (the EngineDraws and Tonemap anchors' own epochs), then the UI.
		constexpr KindRecipe kFrozenKind{ { {
			{ "frozen composite and menu scene", Owner::Tickets, SegmentId::Count },
			{ "UI", Owner::Cs, SegmentId::Ui },
		} }, 2 };
		// The main menu (and any frame before the player has 3D): the menu's 3D scene, then the UI.
		constexpr KindRecipe kMainMenuKind{ { {
			{ "menu scene", Owner::Tickets, SegmentId::Count },
			{ "UI", Owner::Cs, SegmentId::Ui },
		} }, 2 };
		// A loading screen's frame: its 3D model (the menu scene's anchors), then the UI.
		constexpr KindRecipe kLoadingKind{ { {
			{ "loading model", Owner::Tickets, SegmentId::Count },
			{ "UI", Owner::Cs, SegmentId::Ui },
		} }, 2 };
		// A Bink movie's own frame: the movie's tiles (the Present anchor's Bink client), then the UI.
		constexpr KindRecipe kBinkKind{ { {
			{ "movie", Owner::Tickets, SegmentId::Count },
			{ "UI", Owner::Cs, SegmentId::Ui },
		} }, 2 };

		const KindRecipe& RecipeOf(Kind a_kind, bool a_capture)
		{
			switch (a_kind) {
			case Kind::World:
				return kWorldKind;
			case Kind::FrozenWorld:
				return a_capture ? kWorldKind : kFrozenKind;
			case Kind::MainMenu:
				return kMainMenuKind;
			case Kind::Loading:
				return kLoadingKind;
			case Kind::Bink:
			default:
				return kBinkKind;
			}
		}

		const char* OwnerMark(Owner a_owner) { return a_owner == Owner::Dclf ? "*" : a_owner == Owner::Tickets ? " (tickets)" : ""; }
	}

	bool KindHas(Kind a_kind, bool a_capture, SegmentId a_segment)
	{
		const auto& recipe = RecipeOf(a_kind, a_capture);
		for (std::uint32_t i = 0; i < recipe.count; ++i)
			if (recipe.stages[i].segment == a_segment)
				return true;
		return false;
	}

	std::string DescribeKind(Kind a_kind, bool a_capture)
	{
		const auto& recipe = RecipeOf(a_kind, a_capture);
		std::string line;
		for (std::uint32_t i = 0; i < recipe.count; ++i)
			line += fmt::format("{}{}{}", i ? " | " : "", recipe.stages[i].name, OwnerMark(recipe.stages[i].owner));
		return line;
	}

	std::string DiffKinds(Kind a_from, bool a_fromCapture, Kind a_to, bool a_toCapture)
	{
		const auto& from = RecipeOf(a_from, a_fromCapture);
		const auto& to = RecipeOf(a_to, a_toCapture);
		if (&from == &to)
			return {};
		const auto names = [](const KindRecipe& a_recipe, const KindRecipe& a_other) {
			std::string list;
			for (std::uint32_t i = 0; i < a_recipe.count; ++i) {
				bool shared = false;
				for (std::uint32_t j = 0; j < a_other.count; ++j)
					shared = shared || std::string_view(a_recipe.stages[i].name) == a_other.stages[j].name;
				if (!shared)
					list += fmt::format("{}{}", list.empty() ? "" : ", ", a_recipe.stages[i].name);
			}
			return list.empty() ? std::string("none") : list;
		};
		return fmt::format("recipe {} -> {}; stages added: {}; removed: {}", DescribeKind(a_from, a_fromCapture), DescribeKind(a_to, a_toCapture), names(to, from),
			names(from, to));
	}

	const char* StageName(StageId a_stage)
	{
		switch (a_stage) {
		case StageId::Start:
			return "start";
		case StageId::Shadows:
			return "shadows";
		case StageId::PreDepth:
			return "pre-depth";
		case StageId::Depth:
			return "depth";
		case StageId::PreMain:
			return "pre-main";
		case StageId::Opaque:
			return "opaque";
		case StageId::Deferred:
			return "deferred+water";
		case StageId::Forward:
			return "forward";
		case StageId::FirstPerson:
			return "first person";
		case StageId::Post:
			return "post";
		case StageId::Ui:
			return "UI";
		default:
			return "?";
		}
	}

	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph& a_graph, std::uint32_t a_epoch)
	{
		// A host epoch's own graph holds that epoch's passes alone: its whole executable is the epoch's when it has no split for it.
		if (auto trace = a_graph.TraceSelectedSchedule(a_epoch))
			return trace;
		return a_graph.TraceSelectedSchedule(UINT32_MAX);
	}

	bool Traced(std::uint32_t a_epoch)
	{
		for (const auto& stage : kWorld)
			for (std::uint32_t e = 0; e < stage.epochCount; ++e)
				if (EpochOfSegment(stage.epochs[e]) == a_epoch && !FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(stage.epochs[e])))
					return true;
		return false;
	}

	void NoteTrace(std::uint32_t a_epoch, std::uint64_t a_generation, std::shared_ptr<const org::ScheduleTrace> a_trace)
	{
		if (!a_trace)
			return;
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		state.traces[a_epoch] = { a_generation, std::move(a_trace) };
	}

	void Check(org::GraphHost& a_host)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		// The epochs whose build changed since the last check. One without a trace of its build (a build the host made itself, at
		// setup) is traced here, at most two a frame.
		bool changed = false;
		bool complete = true;  // every epoch's current build is traced: the recipe is checked only whole
		std::uint32_t traced = 0;
		for (const auto& stage : kWorld)
			for (std::uint32_t e = 0; e < stage.epochCount; ++e) {
				if (FrameAnchors::IsUnusedSegment(static_cast<std::uint32_t>(stage.epochs[e])))
					continue;
				const auto epoch = EpochOfSegment(stage.epochs[e]);
				auto* graph = a_host.Graph(epoch);
				if (!graph)
					continue;
				const auto generation = a_host.BuildGeneration(epoch);
				if (const auto seen = state.checked.find(epoch); seen != state.checked.end() && seen->second == generation)
					continue;
				auto& entry = state.traces[epoch];
				if (entry.generation != generation || !entry.trace) {
					auto trace = traced < 2 ? TraceEpochGraph(*graph, epoch) : nullptr;
					if (!trace) {
						complete = false;  // no schedule selected yet, or two traced this frame already: the next frame tries again
						continue;
					}
					++traced;
					++state.fallbackTraces;
					entry = { generation, std::move(trace) };
				}
				state.checked[epoch] = generation;
				changed = true;
			}
		if (!changed || !complete)
			return;
		std::vector<Pass> passes;
		for (std::uint32_t s = 0; s < kWorld.size(); ++s)
			for (std::uint32_t e = 0; e < kWorld[s].epochCount; ++e) {
				const auto found = state.traces.find(EpochOfSegment(kWorld[s].epochs[e]));
				if (found != state.traces.end() && found->second.trace)
					AppendPasses(s, *found->second.trace, passes);
			}
		std::vector<std::string> findings;
		Validate(state, passes, findings);
		for (const auto& finding : findings) {
			if (!state.loggedFindings.insert(finding).second)
				continue;
			const bool fatal = Switches::Number("CS_ORG_RECIPE_FATAL", Driver::MismatchFatal() ? 1 : 0) != 0;
			logger::error("[ORG] world recipe: {}", finding);
			if (fatal) {
				const auto message = fmt::format("Community Shaders native mode: the world recipe reads a resource before the pass that writes it: {}", finding);
				logger::critical("[ORG] world recipe: {}", message);
				stl::report_and_fail(message);
			}
		}
		// Printed when its passes changed: every stage, its passes in order, what each reads and writes.
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
		for (std::uint32_t s = 0; s < kWorld.size(); ++s) {
			std::string text;
			std::uint32_t count = 0;
			for (const auto& pass : passes) {
				if (pass.stage != s)
					continue;
				++count;
				text += fmt::format("{}{} [reads {}; writes {}]", text.empty() ? "" : "; ", pass.name, Join(pass.reads), Join(pass.writes));
			}
			logger::info("[ORG] world recipe: {} {} ({}; commits at {}; {} passes): {}", s + 1, StageName(kWorld[s].id), OwnerName(kWorld[s].owner), kWorld[s].commit, count,
				text);
		}
	}

	void LogStats()
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		logger::info("[ORG] world recipe: {} passes, {} inputs from the previous frame, {} from outside the graphs, {} declared by Drawcall Limit Fix's stages and "
					 "written later, {} findings; {} schedules traced on the render owner; {} changed recipes not printed",
			state.passes, state.previousInputs, state.outsideInputs, state.dclfLater, state.findings, state.fallbackTraces, state.unprinted);
	}
}

#else

namespace RenderGraph::Frame::Recipes
{
	const char* StageName(StageId) { return "?"; }
	bool KindHas(Kind a_kind, bool a_capture, SegmentId) { return a_kind == Kind::World || (a_kind == Kind::FrozenWorld && a_capture); }
	std::string DescribeKind(Kind, bool) { return {}; }
	std::string DiffKinds(Kind, bool, Kind, bool) { return {}; }
	std::shared_ptr<const org::ScheduleTrace> TraceEpochGraph(org::RenderGraph&, std::uint32_t) { return {}; }
	bool Traced(std::uint32_t) { return false; }
	void NoteTrace(std::uint32_t, std::uint64_t, std::shared_ptr<const org::ScheduleTrace>) {}
	void Check(org::GraphHost&) {}
	void LogStats() {}
}

#endif
