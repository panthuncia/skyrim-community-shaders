// CS_DCLF_FADE_PARITY checks FadeStateCS against the C++ port in FadeState.h.
// Every 30th frame the pass logs a window of roots. The log is read back three frames later,
// and the port replays each logged update from its starting state with the GPU's frustum verdict.
// It also checks the root rows' inputs against the live nodes.

#include "Features/DrawcallLimitFix/Features/Fades/FadeScene.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesInternal.h"
#include "Features/DrawcallLimitFix/Features/Fades/Diagnostics/FadeDiagnostics.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CullInputs.h"
#include "Features/DrawcallLimitFix/Core/Extensions/RootStates.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/DrawsState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

namespace DCLF::Features::Fades
{
	namespace
	{
		struct FadeReadback
		{
			winrt::com_ptr<ID3D11Buffer> log;
			std::uint32_t frame = 0, base = 0, framesLeft = 0;
			std::vector<FadeRootStatic> roots;  // static rows from base, as uploaded that frame
			FadeFrame inputs{};
			// For roots the engine also draws, the node after the engine's OnVisible. FadeStateCS must match it.
			std::vector<FadeNodeState> nodes;
			std::vector<std::uint8_t> engine;
			std::array<float, 4> worldCamera{};  // position and lodAdjust when the nodes were read
			std::vector<std::array<float, 3>> nodeCentres;
			std::vector<std::string> nodeNames;
			std::vector<std::uint32_t> nodeFlags;
			std::vector<std::uint32_t> programs;
			std::vector<std::uint32_t> rootPrograms;
			std::vector<float> radii, nodeRadii;
		};
		struct FadeLogParity
		{
			std::uint64_t logs = 0, updates = 0, inView = 0, serviced = 0, exact = 0, rounding = 0, differ = 0;
			std::uint64_t engineChecked = 0, engineExact = 0, engineRounding = 0, engineDiffer = 0;
			std::uint64_t skippedRoot = 0, skippedFrame = 0, skippedGeneration = 0;
			// A steady node whose row differs means some writer changed it without an event.
			std::uint64_t inputChecks = 0, inputRoots = 0, inputMoving = 0, inputDiffer = 0;
			std::string first, engineFirst, skippedFirst, inputFirst;
		};

		// Render thread. Every other window focuses on a root the set parity saw wrongly fade-rejected.
		std::optional<FadeReadback> fadeReadback;
		std::uint32_t fadeLogCursor = 0;
		FadeLogParity fadeParity;
		ankerl::unordered_dense::map<const void*, std::pair<float, std::uint8_t>> fadeInputsSeen;
		std::uint32_t fadeLogTurn = 0;

		bool Within(float a_a, float a_b)
		{
			return std::abs(a_a - a_b) <= 1e-5f * std::max(1.0f, std::abs(a_a));
		}
	}

	static void CheckFadeInputs(const SceneTables& a_tables)
	{
		// The tables can lag the engine by an event or two, so only nodes that held their value since the last check are judged.
		auto& p = fadeParity;
		++p.inputChecks;
		const Api::RootListView list = Extensions::RootListOf(a_tables);
		ankerl::unordered_dense::map<const void*, std::pair<float, std::uint8_t>> seen;
		seen.reserve(list.rows.size());
		// Roots whose seed has not arrived yet are skipped.
		const auto* seeds = SeedsIfDone(true);
		for (std::size_t r = 0; r < list.rows.size() && r < list.nodes.size(); ++r) {
			FadeRootStatic row;
			if (!seeds || !MergeFadeSeed(list.rows[r], seeds->data(), seeds->size(), static_cast<std::uint32_t>(r), row))
				continue;
			const auto* node = static_cast<const RE::NiAVObject*>(list.nodes[r]);
			if (!node || row.generation == 0)
				continue;
			++p.inputRoots;
			const float amount = Engine::At<float>(node, 0x100);
			const std::uint8_t flags109 = Engine::At<std::uint8_t>(node, 0x109) & 0x3;
			seen.emplace(node, std::make_pair(amount, flags109));
			// +0x109 bit 0 is always fade out. Bit 1 scales the LOD thresholds by radius.
			const std::uint8_t rowFlags109 = static_cast<std::uint8_t>(row.bits >> kFadeRootBitsShift) & 0x3;
			if (std::memcmp(&amount, &row.fadeAmount, sizeof(amount)) == 0 && flags109 == rowFlags109)
				continue;
			const auto previous = fadeInputsSeen.find(node);
			if (previous == fadeInputsSeen.end() || std::memcmp(&previous->second.first, &amount, sizeof(amount)) != 0 || previous->second.second != flags109) {
				++p.inputMoving;
				continue;
			}
			++p.inputDiffer;
			if (p.inputFirst.empty())
				p.inputFirst = fmt::format("root {} '{}' (plan {}, bits {:#x}): fadeAmount {} (the node's {}), +0x109 {:#x} (the node's {:#x})", r,
					node->name.c_str() ? node->name.c_str() : "", row.bits & kFadeRootPlanMask, row.bits, row.fadeAmount, amount, rowFlags109, flags109);
		}
		fadeInputsSeen = std::move(seen);
		if ((p.inputChecks % 10) == 0) {
			logger::info("[DCLF] fade inputs parity (the root rows' fadeAmount and +0x109 against the nodes): {} checks, {} roots, {} still moving, {} differ{}{}",
				p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputDiffer ? " <- FADE INPUT" : " <- OK", p.inputFirst.empty() ? "" : "; first: " + p.inputFirst);
			p.inputRoots = p.inputMoving = p.inputDiffer = 0;
			p.inputFirst.clear();
		}
	}

	static void ReadWindowPrograms(FadeReadback& a_readback, const Api::RootListView& a_list, const SceneTables& a_tables)
	{
		const auto count = a_readback.roots.size();
		const auto* input = Extensions::MainCullInput();
		a_readback.programs.clear();
		if (input && input->contribution.programWords) {
			const auto words = input->contribution.programWords();
			a_readback.programs.assign(words.begin(), words.end());
		}
		a_readback.rootPrograms.assign(count, input ? input->contribution.noProgram : ~0u);
		a_readback.radii.assign(count, 0.0f);
		a_readback.nodeRadii.assign(count, 0.0f);
		a_readback.nodeFlags.assign(count, 0u);
		if (input && input->contribution.judge) {
			std::vector<std::uint32_t> roots(count), asked;
			asked.reserve(count);
			for (std::uint32_t i = 0; i < count; ++i) {
				roots[i] = a_readback.base + i;
				if (a_list.NodeOf(a_readback.base + i))
					asked.push_back(i);
			}
			Api::RootProgramsJudge judge;
			judge.nodes = a_list.nodes;
			judge.roots = roots;
			judge.asked = asked;
			judge.programs = a_readback.rootPrograms;
			input->contribution.judge(judge);
		}
		for (std::size_t i = 0; i < count; ++i) {
			const auto object = a_readback.roots[i].object;
			const auto* entryNode = object < a_tables.listedRoot.size() ? a_tables.listedRoot[object] : nullptr;
			const float entry = entryNode ? entryNode->worldBound.radius : -1.0f;
			a_readback.radii[i] = entry >= 0.0f && entry < 1e30f ? entry : a_readback.roots[i].radius;
			if (const auto* node = static_cast<const RE::NiAVObject*>(a_list.nodes[a_readback.base + i])) {
				a_readback.nodeRadii[i] = node->worldBound.radius;
				a_readback.nodeFlags[i] = node->GetFlags().underlying();
			}
		}
	}

	static std::uint32_t NextFadeLogBase(std::uint32_t a_frame, const SceneTables& a_tables, const FadeFrame& a_inputs)
	{
		auto& fadeLogFocus = Diagnostics::DrawsState::Get().fadeLogFocus;
		auto& turn = fadeLogTurn;
		if (!FadeParity() || (a_frame % 30) != 0)
			return ~0u;
		const Api::RootListView list = Extensions::RootListOf(a_tables);
		if (list.rows.empty())
			return ~0u;
		CheckFadeInputs(a_tables);
		if (fadeReadback)
			return ~0u;
		const auto count = static_cast<std::uint32_t>(list.rows.size());
		// Start the window at a live root. FadeStateCS logs nothing for empty rows, so such a window would check nothing.
		const auto live = [&](std::uint32_t a_row) { return a_row < count && list.rows[a_row].generation != 0 && list.rows[a_row].object != ~0u; };
		std::uint32_t base = ~0u;
		if (const std::uint32_t focus = fadeLogFocus.exchange(~0u, std::memory_order_relaxed); (++turn & 1) && live(focus))
			base = focus;
		for (std::uint32_t step = 0, row = fadeLogCursor < count ? fadeLogCursor : 0u; base == ~0u && step < count; ++step, row = row + 1 < count ? row + 1 : 0u)
			if (live(row)) {
				base = row;
				fadeLogCursor = row + kFadeLogEntries;
			}
		if (base == ~0u)
			return ~0u;
		FadeReadback readback;
		readback.frame = a_frame;
		readback.base = base;
		readback.roots.assign(list.rows.begin() + base, list.rows.begin() + std::min(count, base + kFadeLogEntries));
		// Merge the seeds so the rows match what FadeStateCS used.
		if (const auto* seeds = SeedsIfDone(true))
			for (std::size_t i = 0; i < readback.roots.size(); ++i)
				MergeFadeSeed(readback.roots[i], seeds->data(), seeds->size(), static_cast<std::uint32_t>(base + i), readback.roots[i]);
		readback.inputs = a_inputs;
		readback.nodes.resize(readback.roots.size());
		readback.engine.assign(readback.roots.size(), 0);
		ReadWindowPrograms(readback, list, a_tables);
		if (const auto* camera = RE::Main::WorldRootCamera())
			readback.worldCamera = { camera->world.translate.x, camera->world.translate.y, camera->world.translate.z, Engine::At<float>(camera, 0x184) };
		// Nodes as the list jobs left them, before the next animation job runs beside the render thread.
		const auto* snapshot = NodeSnapshot(a_frame);
		for (std::size_t i = 0; i < readback.roots.size(); ++i) {
			const auto bits = readback.roots[i].bits;
			const auto* node = static_cast<const RE::NiAVObject*>(list.nodes[base + i]);
			// Roots DCLF writes have no engine OnVisible to compare against.
			if (node && snapshot && base + i < snapshot->size() && !(bits & kFadeRootWritten)) {
				readback.nodes[i] = (*snapshot)[base + i];
				readback.engine[i] = 1;
				readback.nodeCentres.resize(readback.roots.size());
				readback.nodeNames.resize(readback.roots.size());
				readback.nodeCentres[i] = { node->worldBound.center.x, node->worldBound.center.y, node->worldBound.center.z };
				readback.nodeNames[i] = node->name.c_str() ? node->name.c_str() : "";
			}
		}
		fadeReadback = std::move(readback);
		return base;
	}

	static void NextFadeLog(const Diagnostics::FadeLogWindow& a_window)
	{
		a_window.base = NextFadeLogBase(a_window.frame, a_window.tables, a_window.inputs);
	}

	/** @brief Copies the log to a staging buffer to read three frames later. False drops the window. */
	static bool CopyFadeLog(FadeReadback& a_readback, const FadeScene* a_fades)
	{
		if (!a_fades || !a_fades->fadeLog || a_fades->fadeLogBase != a_readback.base || a_fades->fadeFrameNumber != a_readback.frame)
			return false;
		constexpr UINT bytes = kFadeLogEntries * sizeof(FadeLogEntry);
		D3D11_BUFFER_DESC sourceDesc{};
		sourceDesc.ByteWidth = bytes;
		sourceDesc.Usage = D3D11_USAGE_DEFAULT;
		sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		sourceDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
		sourceDesc.StructureByteStride = sizeof(std::uint32_t);
		const auto source = RenderGraphRuntime::Get().WrapBuffer(*a_fades->fadeLog, sourceDesc);
		D3D11_BUFFER_DESC desc{};
		desc.ByteWidth = bytes;
		desc.Usage = D3D11_USAGE_STAGING;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		if (!source || FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, a_readback.log.put())))
			return false;
		ScopedPerfEvent event("CS DCLF: fade state readback");
		const D3D11_BOX box{ 0, 0, 0, bytes, 1, 1 };
		globals::d3d::context->CopySubresourceRegion(a_readback.log.get(), 0, 0, 0, 0, source.get(), 0, &box);
		a_readback.framesLeft = 3;
		return true;
	}

	/** @brief Whether the entry is this frame's update for this row. Otherwise counts it as skipped. */
	static bool EntryCurrent(const FadeReadback& a_done, std::uint32_t a_i, const FadeLogEntry& a_entry, const FadeRootStatic& a_root, FadeLogParity& a_p)
	{
		if (a_entry.root == a_done.base + a_i && a_entry.after.frame == a_done.frame && a_entry.after.generation == a_root.generation && a_root.generation)
			return true;
		const bool rootSkip = a_entry.root != a_done.base + a_i, frameSkip = !rootSkip && a_entry.after.frame != a_done.frame;
		++(rootSkip ? a_p.skippedRoot : frameSkip ? a_p.skippedFrame : a_p.skippedGeneration);
		if (a_p.skippedFirst.empty())
			a_p.skippedFirst = fmt::format("row {}: entry root {}, its update's frame {} (the log's {}), generation {} (the row's {})", a_done.base + a_i, a_entry.root,
				a_entry.after.frame, a_done.frame, a_entry.after.generation, a_root.generation);
		return false;
	}

	/** @brief Detail for the first engine difference, to show which camera each side measured from. */
	static std::string DescribeEngineDifference(const FadeReadback& a_done, std::uint32_t a_i, const FadeLogEntry& a_entry, const FadeRootStatic& a_root,
		const std::string& a_differences)
	{
		const auto& n = a_done.nodes[a_i];
		const auto& g = a_entry.after;
		const auto& in = a_done.inputs;
		const auto distanceTo = [&](const float* a_eye) {
			const float dx = a_entry.centre[0] - a_eye[0], dy = a_entry.centre[1] - a_eye[1], dz = a_entry.centre[2] - a_eye[2];
			return std::sqrt(dx * dx + dy * dy + dz * dz);
		};
		const float perMetric = g.metric != 0.0f ? distanceTo(in.eye) / g.metric : 0.0f;
		std::string text = fmt::format("root {} (plan {}, verdict {:#x}): {}; centre ({:.0f} {:.0f} {:.0f}), fade eye ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}, "
									   "world camera ({:.0f} {:.0f} {:.0f}) lodAdjust {} at {:.0f}; the engine's metric implies {:.0f} at the fade eye's scale",
			a_entry.root, a_root.bits & kFadeRootPlanMask, g.verdict, a_differences, a_entry.centre[0], a_entry.centre[1], a_entry.centre[2], in.eye[0], in.eye[1],
			in.eye[2], in.lodAdjust, distanceTo(in.eye), a_done.worldCamera[0], a_done.worldCamera[1], a_done.worldCamera[2], a_done.worldCamera[3],
			distanceTo(a_done.worldCamera.data()), n.metric * perMetric);
		// Run the port at this frame's eye to see which side is current.
		FadeNodeState now = a_entry.before;
		FadeState::OnVisible(now, a_root, a_entry.centre, in);
		text += fmt::format("; at this frame's eye the metric is {}; FadeStateCS's before {} lastVisible {} (after {}), the engine's lastVisible {}",
			now.metric, a_entry.before.metric, a_entry.before.lastVisible, g.lastVisible, n.lastVisible);
		{
			// Repeat FadeStateCS's cull test on the inputs it had.
			const std::uint32_t list = a_done.rootPrograms[a_i];
			std::string why;
			const auto* input = Extensions::MainCullInput();
			const int port = input && input->contribution.test ? input->contribution.test(a_done.programs, list, a_entry.centre, a_done.radii[a_i], a_done.nodeFlags[a_i], why) : -1;
			{
				// Depth below the listed entry matters because the engine culls a child only after its parents.
				const auto* node = static_cast<const RE::NiAVObject*>(Extensions::RootListOf(Scene::FrameView::Get().GetTables()).nodes[a_entry.root]);
				std::uint32_t depth = 0;
				const RE::NiAVObject* at = node;
				while (at && !PrimaryCull::Get().IsEntry(at))
					at = at->parent, ++depth;
				text += fmt::format("; {} below its entry '{}'", at ? fmt::format("{} levels", depth) : std::string("no entry"), at && at->name.c_str() ? at->name.c_str() : "");
			}
			text += fmt::format("; its portal program {}, radius read {} (the node's {}), the test on those {} ({})", static_cast<std::int32_t>(list), a_done.radii[a_i],
				a_done.nodeRadii[a_i], port, why);
		}
		if (a_i < a_done.nodeCentres.size()) {
			const auto& c = a_done.nodeCentres[a_i];
			const float dx = c[0] - in.eye[0], dy = c[1] - in.eye[1], dz = c[2] - in.eye[2];
			text += fmt::format("; the node '{}' centre ({:.0f} {:.0f} {:.0f}) at {:.0f}", a_done.nodeNames[a_i], c[0], c[1], c[2], std::sqrt(dx * dx + dy * dy + dz * dz));
		}
		return text;
	}

	static void CompareWithEngine(const FadeReadback& a_done, std::uint32_t a_i, const FadeLogEntry& a_entry, const FadeRootStatic& a_root, FadeLogParity& a_p)
	{
		++a_p.engineChecked;
		const auto& n = a_done.nodes[a_i];
		const auto& g = a_entry.after;
		const auto engineDifferences = FadeState::Differences(n, g);
		if (engineDifferences.empty())
			++a_p.engineExact;
		else if (n.flags == g.flags && n.lastVisible == g.lastVisible && (n.levels & 0xFFFF) == (g.levels & 0xFFFF) && Within(n.currentFade, g.currentFade) &&
				 Within(n.snapRadius, g.snapRadius) && Within(n.amountFade, g.amountFade) && Within(n.metric, g.metric) && Within(n.previousMetric, g.previousMetric) &&
				 Within(n.blend, g.blend))
			++a_p.engineRounding;
		else {
			++a_p.engineDiffer;
			if (a_p.engineFirst.empty())
				a_p.engineFirst = DescribeEngineDifference(a_done, a_i, a_entry, a_root, engineDifferences);
		}
	}

	static void CheckPortUpdate(const FadeReadback& a_done, const FadeLogEntry& a_entry, const FadeRootStatic& a_root, FadeLogParity& a_p)
	{
		++a_p.updates;
		FadeNodeState port = a_entry.before;
		std::uint32_t verdict = 0;
		if (a_entry.after.verdict & kFadeVerdictAnimated) {
			const std::uint32_t updates = (a_entry.after.verdict >> 5) & kFadeAnimatedCountMask;
			FadeState::AnimatedUpdate(port, a_root, a_entry.centre, a_done.inputs, updates);
			verdict |= kFadeVerdictAnimated | (updates << 5);
		}
		if (a_entry.after.verdict & kFadeVerdictInView) {
			++a_p.inView;
			verdict |= FadeState::OnVisible(port, a_root, a_entry.centre, a_done.inputs);
		}
		a_p.serviced += (verdict & kFadeVerdictServiced) ? 1 : 0;
		const auto differences = FadeState::Differences(port, a_entry.after);
		if (differences.empty() && verdict == a_entry.after.verdict) {
			++a_p.exact;
			return;
		}
		// The GPU's division and square root may round differently from SSE, so allow small float differences.
		const auto& a = a_entry.after;
		const bool rounding = verdict == a.verdict && port.flags == a.flags && port.lastVisible == a.lastVisible && (port.levels & 0xFFFF) == (a.levels & 0xFFFF) &&
		                      Within(port.currentFade, a.currentFade) && Within(port.snapRadius, a.snapRadius) && Within(port.amountFade, a.amountFade) &&
		                      Within(port.metric, a.metric) && Within(port.previousMetric, a.previousMetric) && Within(port.blend, a.blend);
		++(rounding ? a_p.rounding : a_p.differ);
		if (!rounding && a_p.first.empty())
			a_p.first = fmt::format("root {} (plan {}, verdict {:#x}, expected {:#x}): {}", a_entry.root, a_root.bits & kFadeRootPlanMask, a.verdict, verdict,
				differences.empty() ? "the verdict" : differences);
	}

	/** @brief Every 10th log. Resets the counts except the input checks, which CheckFadeInputs reports. */
	static void LogFadeParity(FadeLogParity& a_p)
	{
		auto& p = a_p;
		if ((p.logs % 10) != 0)
			return;
		logger::info("[DCLF] fade state parity (FadeStateCS against the port): {} logs, {} root updates ({} in view, {} serviced); {} exact, {} within rounding, {} differ{}{}",
			p.logs, p.updates, p.inView, p.serviced, p.exact, p.rounding, p.differ, p.differ ? " <- FADE STATE" : " <- OK", p.first.empty() ? "" : "; first: " + p.first);
		logger::info("[DCLF] fade state parity: log entries skipped: {} not the row asked for, {} another frame's update, {} another listing's{}", p.skippedRoot,
			p.skippedFrame, p.skippedGeneration, p.skippedFirst.empty() ? "" : "; first: " + p.skippedFirst);
		// Informational only. The engine's own cull may visit a root differently, and DCLF's state is the one that counts.
		logger::info("[DCLF] fade state of roots with engine-drawn parts (FadeStateCS against the engine's node, informational): {} checked, {} exact, {} within rounding, {} differ{}{}",
			p.engineChecked, p.engineExact, p.engineRounding, p.engineDiffer, p.engineDiffer ? " (the engine's own cull; DCLF's state stands)" : "",
			p.engineFirst.empty() ? "" : "; first: " + p.engineFirst);
		const auto inputs = std::make_tuple(p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputFirst);
		p = {};
		std::tie(p.inputChecks, p.inputRoots, p.inputMoving, p.inputDiffer, p.inputFirst) = inputs;
	}

	static void ReadFadeLog(const Diagnostics::MainEpochDone& a_epoch)
	{
		if (!a_epoch.depthOnly)
			return;
		const auto& a_resources = a_epoch.resources;
		if (!fadeReadback)
			return;
		auto* context = globals::d3d::context;
		auto& readback = *fadeReadback;
		if (!readback.log) {
			if (!CopyFadeLog(readback, SceneOf(*a_resources->scene)))
				fadeReadback.reset();
			return;
		}
		if (--readback.framesLeft)
			return;
		const auto done = std::move(*fadeReadback);
		fadeReadback.reset();
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context->Map(done.log.get(), 0, D3D11_MAP_READ, 0, &mapped)))
			return;
		const auto* entries = static_cast<const FadeLogEntry*>(mapped.pData);
		auto& p = fadeParity;
		++p.logs;
		for (std::uint32_t i = 0; i < done.roots.size(); ++i) {
			const auto& entry = entries[i];
			const auto& root = done.roots[i];
			if (!EntryCurrent(done, i, entry, root, p))
				continue;
			if (i < done.engine.size() && done.engine[i])
				CompareWithEngine(done, i, entry, root, p);
			CheckPortUpdate(done, entry, root, p);
		}
		context->Unmap(done.log.get(), 0);
		LogFadeParity(p);
	}

	void SubscribeFadeLog()
	{
		Diagnostics::Subscribe<Diagnostics::FadeLogWindow>(&NextFadeLog);
		Diagnostics::Subscribe<Diagnostics::MainEpochDone>(&ReadFadeLog);
	}
}
