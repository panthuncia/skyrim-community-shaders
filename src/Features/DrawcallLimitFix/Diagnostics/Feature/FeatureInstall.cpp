// Observers of the frame events, all on the render thread. The capture parity and the leak count.
#include "Features/DrawcallLimitFix/Diagnostics/Install.h"

#include "Features/DrawcallLimitFix/Diagnostics/Scene/CaptureParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/PlayerParity.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "RenderGraph/Frame/FrameDriver.h"

namespace DCLF::Reports
{
	LeakStats& Leaks()
	{
		static LeakStats leaks;
		return leaks;
	}
}

namespace DCLF::Diagnostics
{
	namespace
	{
		/**
		 * @brief A main-view native pass of a frame set member is a leak, since its registration should have withheld it. It is a tap
		 * error where it is found. Skipped when the engine draws all.
		 */
		void NoteLeak(const NativePassOffered& a_offered)
		{
			const auto* pass = a_offered.pass;
			if (!pass || !pass->geometry || !Policy::WithholdFromEngine())
				return;
			if (!a_offered.inDepthPass && !globals::deferred->deferredPass)
				return;  // shadows, reflections and cubemaps are not main views
			auto& leaks = Reports::Leaks();
			++leaks.offered;
			const auto set = Scene::FrameView::Get().FrameClaims();
			if (!set || !(set->PhasesOf(pass->geometry) & kSetMain))
				return;
			++(a_offered.inDepthPass ? leaks.leakedInDepth : leaks.leakedInOpaque);
			// The engine drew what DCLF draws too: fatal in development builds. Release counts it and the reports keep their samples.
			::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::EngineLeak, ::RenderGraph::Frame::SegmentId::Count,
				fmt::format("'{}' ({} pass, technique {:08X}, hint {})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
					a_offered.inDepthPass ? "depth" : "opaque", a_offered.technique, static_cast<std::uint32_t>(pass->accumulationHint)));
			// For the compatibility monitor, keyed by the geometry's reference, since another plugin's registration is the likely cause.
			if (PlayerParity::Active()) {
				std::uint32_t form = 0;
				for (const RE::NiAVObject* at = pass->geometry; at && !form; at = at->parent)
					if (const auto* reference = at->GetUserData())
						form = reference->GetFormID();
				PlayerParity::NoteLeak(form, fmt::format("'{}' ({} pass, technique {:08X})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
												 a_offered.inDepthPass ? "depth" : "opaque", a_offered.technique));
			}
			if (leaks.samples.size() < 8)
				leaks.samples.push_back(fmt::format("'{}' ({}, hint {}, technique {:08X})", pass->geometry->name.c_str() ? pass->geometry->name.c_str() : "?",
					a_offered.inDepthPass ? "depth" : "opaque", static_cast<std::uint32_t>(pass->accumulationHint), a_offered.technique));
		}
	}

	void InstallFeature()
	{
		const auto& o = Observe();
		if (o.capture)
			Subscribe<FramePoint::PrepassEnd>([](const On<FramePoint::PrepassEnd>&) { CaptureParity::Get().Report(Scene::FrameView::Get().SceneStep(), 300); });
		if (o.capture)
			Subscribe<NativeLightingDraw>([](const NativeLightingDraw& a_event) {
				// Inside a reflection face render.
				if (PassCapture::Get().OwnViewRendering())
					CaptureParity::Get().OnFaceLightingDraw(a_event.pass, a_event.renderFlags);
				else
					CaptureParity::Get().OnNativeLightingDraw(a_event.pass, a_event.renderFlags);
			});
		Subscribe<NativePassOffered>(&NoteLeak);
	}
}
