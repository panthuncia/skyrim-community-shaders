// The frame trace, a tool for corrupt frames. Every frame logs one line per point of what it drew with: the adopted snapshot,
// epoch coverage, each main epoch's outcome, colour culling counts and the bound retirement point. F10 logs the user's marker.
// A line off the steady state carries its reason, and a binding the clock already passed is flagged with "<-".
// tools/dclf-corrupt-frames.py maps captures from tools/dclf-capture-burst.ps1 to these lines by timestamp.

#include "DrawsView.h"
#include "Events.h"
#include "Observers.h"

#include <string>

namespace DCLF::Diagnostics
{
	namespace
	{
		/** @brief The last frame's bound point, and whether F10 was down at it. Render thread. */
		struct FrameTraceState
		{
			std::uint64_t lastPoint = 0;
			bool markDown = false;
		};
		FrameTraceState& TraceState()
		{
			static FrameTraceState state;
			return state;
		}
	}

	void TraceSnapshotAdopted(const SnapshotAdopted& a_adopted)
	{
		std::string parts;
		if (const auto* p = a_adopted.payload)
			for (std::size_t k = 0; k < Draws::kPayloadParts; ++k)
				parts += fmt::format(" p{}[units {}]", k, p->units[k]);
		logger::info("[DCLF] frame trace: frame {} adopts snapshot {} (versions {}, set {:p}{}), payload source {}{}", a_adopted.frame, a_adopted.snapshot,
			a_adopted.set ? "yes" : "none", static_cast<const void*>(a_adopted.set), a_adopted.growth ? " GROWTH" : "", a_adopted.payload ? a_adopted.payload->serial : 0,
			parts);
	}

	void TraceEpochCoverage(const EpochCoverageDecided& a_decided)
	{
		std::string bits;
		bits.reserve(a_decided.covered.size());
		for (const bool covered : a_decided.covered)
			bits += covered ? '1' : '0';
		logger::info("[DCLF] frame trace: frame {} coverage {} (snapshot {} drawsComplete {} stale {} payload source {} versions {} current {})", a_decided.frame, bits,
			a_decided.snapshot, a_decided.drawsComplete, a_decided.stale, a_decided.payload ? a_decided.payload->serial : 0, a_decided.versions ? "yes" : "none",
			a_decided.current);
	}

	void TraceMainEpoch(const MainEpochOutcome& a_outcome)
	{
		const char* epoch = a_outcome.depthOnly ? "Z-prepass" : "colour";
		if (!a_outcome.executed) {
			logger::info("[DCLF] frame trace: frame {} {} epoch: {} (set {})", a_outcome.frame, epoch, a_outcome.outcome, a_outcome.notDrawingYet ? "not drawing yet" : "CLAIMS INSTALLED");
			return;
		}
		const auto* payload = a_outcome.payload;
		logger::info("[DCLF] frame trace: frame {} {} epoch: {} (payload {}, inputs {}, snapshot {})", a_outcome.frame, epoch, a_outcome.outcome, payload ? payload->serial : 0,
			payload ? (*payload)[Draws::PayloadPart::Inputs].units : 0, a_outcome.snapshot);
	}

	void TraceAdoptedBound(const AdoptedBound& a_bound)
	{
		auto& state = TraceState();
		logger::info("[DCLF] frame trace: frame {} binds point {} (clock {}){}{}", a_bound.frame, a_bound.point, a_bound.completed,
			a_bound.point <= a_bound.completed ? " <- BOUND AT A RETIRED POINT" : "", a_bound.point < state.lastPoint ? " <- POINT WENT BACK" : "");
		state.lastPoint = a_bound.point;
		// F10 marks a corrupt frame the user saw, once per press.
		const bool down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
		if (down && !state.markDown)
			logger::info("[DCLF] frame trace: ===== USER MARK (F10) at frame {} =====", a_bound.frame);
		state.markDown = down;
	}

	void TraceColourCounts(const Draws::CounterReadback& a_readback, std::uint32_t a_slot)
	{
		// The range's last colour execution has retired. Its words name their execution by the stamp they carry.
		if (a_slot >= a_readback.buffers.size())
			return;
		auto resource = a_readback.buffers[a_slot]->GetAPIResource();
		void* mapped = nullptr;
		resource.Map(&mapped);
		if (!mapped)
			return;
		const auto* w = static_cast<const std::uint32_t*>(mapped);
		logger::info("[DCLF] frame trace: colour exec stamp {} (range {}): drawn {} rejected {} tested {} occluded {} phase2 drawn {} rescued {}; terrain groups {} {}; fade tested {} hidden {}; class unplanned {}",
			w[Draws::kCountWords], a_slot, w[0], w[1], w[2], w[6], w[17], w[18], w[Draws::kCountTerrainGroupWord], w[Draws::kCountTerrainGroupWord + 1],
			w[Draws::kCountFadeTestedWord], w[Draws::kCountFadeHiddenWord], w[Draws::kCountClassUnplannedWord]);
		resource.Unmap(0, 0);
	}
}
