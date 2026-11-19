// Hooks the engine's LOD segment writers. After each write the shape is queued for the coordinator and its mirror update is pushed.

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Runtime/EventQueue.h"
#include "Features/DrawcallLimitFix/Features/Lod/LodSegmentEvents.h"

namespace DCLF::Features::Lod
{
	namespace
	{
		// No wake. Writes happen constantly as the camera moves and mostly leave the ranges unchanged, so the next pass takes them.
		EventQueue<const void*> segmentEvents;
		bool segmentHooksInstalled = false;

		void OnSegmentWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			auto* shape = SceneWriter::Arg<RE::BSGeometry*>(a_event, 0);
			segmentEvents.Push(shape);
			Extensions::PushRangesUpdate(shape);
		}

		bool SubscribeSegmentWriters()
		{
			// All three writers or none.
			if (!SceneWriter::Has(SWF_CAP_LOD_SEGMENT_CALLS)) {
				logger::warn("[DCLF] object LOD: the scene writer framework has no LOD segment calls (a call site differs); object LOD stays native");
				return false;
			}
			for (const std::uint32_t hook : { SWF_HOOK_LOD_SEGMENT_SHOW, SWF_HOOK_LOD_SEGMENT_HIDE, SWF_HOOK_LOD_SEGMENT_SHOW_ALL })
				if (!SceneWriter::SubscribeCall(hook, &OnSegmentWrite, SWF_PHASE_POST))
					stl::report_and_fail("Drawcall Limit Fix: the scene writer framework refused a LOD segment writer after reporting it installed");
			return true;
		}
	}

	void InstallSegmentHooks()
	{
		segmentHooksInstalled = SubscribeSegmentWriters();
	}

	bool SegmentHooksInstalled()
	{
		return segmentHooksInstalled;
	}

	std::size_t DrainSegmentEvents(Api::KeySink& a_sink)
	{
		return segmentEvents.Drain(a_sink);
	}
}
