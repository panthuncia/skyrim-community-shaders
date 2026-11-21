// The LOD gates' report lines. GateLines runs on the scene lane, ReleaseLines on the render thread.

#include "Features/DrawcallLimitFix/Features/Lod/LodGateComponent.h"

#include "Features/DrawcallLimitFix/Core/Extensions/CoordinatorComponents.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Spread.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodGates.h"

#include <algorithm>
#include <array>
#include <string>

namespace DCLF::Features::Lod::Gates
{
	namespace
	{
		using Reports::Percentiles;

		// Empty while not installed.
		std::string EngineLine(std::uint32_t a_frames)
		{
			const auto t = LodGates::Take();
			if (!t.installed)
				return {};
			std::string flushes;
			for (std::size_t i = 0; i < t.flushes.size(); ++i)
				if (const auto count = t.flushes[i])
					flushes += fmt::format("{}{} {}", flushes.empty() ? "" : ", ", LodGates::FlushName(i), count);
			const auto ms = LodGates::TicksToMilliseconds;
			return fmt::format(
				"[DCLF] LOD gates (T6b5, over {} frames): {} opened ({} initial loads; {} initial loads under a load screen passed through), {} flipped ({} flips "
				"missed), {} forced ({} superseded; flushes: {}); {} blocks held, {} released ({} reused, {} handed off, {} deferred past a flip), {} reattached "
				"before their flip, {} kept notices to the scene, {} reused still hidden by the flip with no gate pending, {} orphans shown, {} shows vetoed; {} "
				"open, the oldest {} frames {:.1f} ms; request to flip {:.1f} frames {:.2f} ms average, {:.2f} ms max; flip to release {:.2f} ms average, {:.2f} ms max",
				a_frames, t.opened, t.initialOpened, t.initialLoadScreen, t.flipped, t.flipMisses, t.forced, t.superseded, flushes.empty() ? "none" : flushes, t.held,
				t.released, t.reused, t.handedOff, t.deferred, t.reattached, t.kept, t.reusedHidden, t.orphansShown, t.vetoes, t.open, t.oldestFrames, ms(t.oldestTicks),
				t.flipped ? static_cast<double>(t.flipFrames) / static_cast<double>(t.flipped) : 0.0, t.flipped ? ms(t.flipTicks) / static_cast<double>(t.flipped) : 0.0,
				ms(t.flipTicksMax), t.releases ? ms(t.releaseTicks) / static_cast<double>(t.releases) : 0.0, ms(t.releaseTicksMax));
		}
	}

	void GateLines(const Api::CoordinatorComponentContext& a_context, std::string& a_text)
	{
		auto& state = a_context.State<GateState>();
		auto& s = state.gateStats;
		const auto& lodGates = state.lodGates;
		const std::uint32_t gateHeldOutCount = a_context.VetoCount(Api::kVetoHold);
		if (!s.opened && !s.retiredFlipped && !s.retiredForced && lodGates.empty() && !gateHeldOutCount && !s.unknownOutcomes && !s.keptNotices)
			return;
		// Count what each open gate's pending geometries are waiting for.
		const std::uint64_t now = GateNowNs();
		std::uint64_t oldestNs = 0;
		std::uint32_t open = 0, flipped = 0, unmirrored = 0, noIncoming = 0;
		std::uint64_t unclassified = 0, noRecord = 0, unbound = 0, partial = 0;
		std::array<std::uint64_t, 16> waitingBy{};
		for (const auto& [token, gate] : lodGates) {
			if (gate.state != LodGate::State::Open) {
				++flipped;
				continue;
			}
			++open;
			if (!oldestNs || gate.stampNs < oldestNs)
				oldestNs = gate.stampNs;
			unmirrored += gate.unmirrored;
			noIncoming += gate.incoming.empty() ? 1u : 0u;
			for (auto* geometry : gate.incomingGeometries) {
				const Api::TrackedState entry = a_context.TrackedStateOf(geometry);
				if (!entry.tracked || GeometryReady(geometry, entry, [&](std::uint32_t a_slot) { return a_context.ObservedSlot(a_slot); }))
					continue;
				const std::uint32_t slot = entry.slot != kNoSlot ? entry.slot : entry.layerSlot;
				if (entry.leafWaiting || !entry.classified)
					++unclassified;
				else if (slot == kNoSlot)
					++noRecord;
				else if (entry.waitCause)
					++waitingBy[(std::min<std::size_t>)(entry.waitWhy, waitingBy.size() - 1)];
				else if (!entry.resident)
					++unbound;
				else
					++partial;
			}
		}
		std::string waiting;
		for (std::size_t why = 0; why < waitingBy.size(); ++why)
			if (waitingBy[why])
				waiting += fmt::format("{}{} {}", waiting.empty() ? "" : ", ", why, waitingBy[why]);
		auto text = fmt::format("[DCLF] LOD gates (T6b5): {} opened, {} flipped, {} retired flipped, {} forced ({} before their flip), {} outcomes for no gate, {} roots taken "
								 "by a later gate, {} roots detached, {} with no incoming root; {} incoming and {} outgoing geometries tagged, {} held out at flips ({} held "
								 "out now); request to flip: ms (p50/p95/max) {}, frames (p50/p95/max) {}; now {} open (oldest {:.1f} ms), {} flipped awaiting their "
								 "release's outcome; blockers: unmirrored roots {}, unclassified {}, no record {}, not bound {}, waiting (by the set's waiting-for index) {}, "
								 "a phase short or its layer partner {}",
			s.opened, s.flipped, s.retiredFlipped, s.retiredForced, s.forcedOpen, s.unknownOutcomes, s.superseded, s.rootsDetached, s.emptyIncoming, s.taggedIncoming,
			s.taggedOutgoing, s.heldOut, gateHeldOutCount, Percentiles(s.requestToFlipMs), Percentiles(s.requestToFlipFrames), open, GateMs(oldestNs, now), flipped,
			unmirrored, unclassified, noRecord, unbound, waiting.empty() ? "-" : waiting, partial);
		if (noIncoming)
			text += fmt::format("; {} open gates lost every incoming root (waiting for a forced release)", noIncoming);
		text += fmt::format("; reused blocks: {} kept notices, {} geometries released from their "
							"hold-out (committed again), {} outgoing tags dropped before their flip",
			s.keptNotices, s.keptCleared, s.keptUntagged);
		s = {};
		a_text += text;
	}

	void ReleaseLines([[maybe_unused]] std::uint32_t a_frame)
	{
		auto& rs = Releases();
		if (rs.any)
			logger::info("[DCLF] LOD gate releases (T6b5): {} released, {} refused (forced or unknown by then), {} in a frame before DCLF draws (no claims installed yet); flip to release: ms "
						 "(p50/p95/max) {}, frames (p50/p95/max) {}; request to release ms (p50/p95/max) {}",
				rs.released, rs.refused, rs.beforeDrawing, Percentiles(rs.flipToReleaseMs), Percentiles(rs.flipToReleaseFrames),
				Percentiles(rs.requestToReleaseMs));
		if (LodGates::Enabled())
			if (const auto engine = EngineLine(Reports::Interval()); !engine.empty())
				logger::info("{}", engine);
		rs = {};
	}
}
