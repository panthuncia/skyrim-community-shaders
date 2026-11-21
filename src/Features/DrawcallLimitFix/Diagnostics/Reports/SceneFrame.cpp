#include "Scene.h"

#include "Engine.h"
#include "Schedule.h"
#include "Spread.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Reports.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SceneState.h"
#include "Features/DrawcallLimitFix/Diagnostics/Scene/SoleHolds.h"
#include "Features/DrawcallLimitFix/Core/Scene/Coordinator/Internal.h"

// The render thread's scene lines, and the lines at the gate releases' point.

namespace DCLF::Reports
{
	using V = Diagnostics::SceneView;

	std::string SceneFrameLines()
	{
		// Only what the render thread keeps. Nothing of the coordinator's.
		std::string text;
		if (auto& g = V::geometryStats(); g.frames) {
			text += fmt::format("[DCLF] pipeline constants: {:.2f} full geometry evaluations and {:.2f} frame samples a frame, {:.1f} pipelines changed, frame lighting changed {:.2f}\n",
				static_cast<double>(g.full) / g.frames, static_cast<double>(g.samples) / g.frames, static_cast<double>(g.changed) / g.frames,
				static_cast<double>(g.lightingVersions) / g.frames);
			g = {};
		}
		if (auto& ss = V::shadingStats(Scene::Coordinator::Get()); V::shadingFrames()) {
			const double frames = static_cast<double>(std::exchange(V::shadingFrames(), 0));
			text += fmt::format("[DCLF] shading (sampled at the frame's start for the slots its events name): {:.1f} LOD fade events, {:.1f} emittance events, {:.1f} "
								"named a frame\n",
				static_cast<double>(ss.lodFadeEvents) / frames, static_cast<double>(ss.emittanceEvents) / frames, static_cast<double>(ss.resampled) / frames);
			ss = {};
		}
		Extensions::ReportTextAt(Api::ReportPoint::SceneFrame, text);
		auto& captureNs = V::captureNs();
		auto& categoryCaptureNs = V::categoryCaptureNs();
		auto& categoryCapturesMade = V::categoryCapturesMade();
		auto& captureFrames = V::captureFrames();
		auto& categoryMirrorCaptures = V::categoryMirrorCaptures();
		auto& categoryMirrorRecords = V::categoryMirrorRecords();
		if (captureFrames) {
			const auto unscoped = FrameGlobals::TakeUnscopedReads();
			text += fmt::format("[DCLF] frame capture (6e F2): render thread {:.1f} us/frame for the globals, {:.1f} us/frame for the categories ({} captures made; {} subtrees the "
								"mirror lacked captured, {} records); {} reads off the render thread with no frame's capture bound{}\n",
				static_cast<double>(captureNs) / 1000.0 / captureFrames, static_cast<double>(categoryCaptureNs) / 1000.0 / captureFrames, categoryCapturesMade, categoryMirrorCaptures,
				categoryMirrorRecords, unscoped, unscoped ? " <- CAPTURE" : " <- OK");
			captureNs = categoryCaptureNs = categoryCapturesMade = captureFrames = 0;
			categoryMirrorCaptures = categoryMirrorRecords = 0;
			// What only the held captures keep alive.
			Diagnostics::SoleHolds pins, nodes, roots;
			for (const auto& capture : V::categoryCapturesHeld()) {
				for (const auto& pin : capture->pins)
					pins.Add(pin.get());
				for (const auto& node : capture->held)
					nodes.Add(node.get());
				for (const auto& [key, root] : capture->roots)
					roots.Add(root.root.get());
			}
			text += fmt::format("[DCLF] category captures held: {} (generations {}..{}); {}; {}; {}\n", V::categoryCapturesHeld().size(),
				V::categoryCapturesHeld().empty() ? 0 : V::categoryCapturesHeld().front()->generation,
				V::categoryCapturesHeld().empty() ? 0 : V::categoryCapturesHeld().back()->generation, pins.Text("pins"), nodes.Text("category nodes"),
				roots.Text("roots"));
		}
		return text;
	}

	void GateReleases(std::uint32_t a_frame)
	{
		// Gate release lines once a report window. The render thread sees each frame number once.
		static Crossing crossing;
		if (crossing.Due(a_frame))
			Extensions::ReportLinesAt(Api::ReportPoint::GateReleases, a_frame);
	}
}
