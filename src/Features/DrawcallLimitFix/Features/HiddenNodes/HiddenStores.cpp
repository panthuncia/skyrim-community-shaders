// Turns SceneWriterFramework's kHidden stores into mirror updates, pushed on the storing thread when the bit changes.
// The LOD show stores go through the show veto as SWF's show filter.

#include "Features/DrawcallLimitFix/Features/HiddenNodes/HiddenNodes.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneEvents.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Features::HiddenNodes
{
	namespace
	{
		// Fixed for the session. Set before any event arrives.
		const Api::ShowVeto* showVeto = nullptr;
		// The site id for plugins' writes through SWF. It comes right after SWF's own stores.
		std::uint32_t writeSite = 0;

		void OnHidden(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_HiddenEvent*>(a_event);
			Scene::PushHiddenUpdate(event.object, event.site);
		}

		void OnSceneWrite(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			const auto& write = *reinterpret_cast<const SWF_SceneWriteEvent*>(a_event);
			if (write.kind == SWF_WRITE_HIDDEN)
				Scene::PushHiddenUpdate(write.object, writeSite);
		}

		// A vetoed store does not run and sends no event, so the object stays hidden.
		// The open word keeps the common case to a single load.
		std::int32_t VetoShow(const SWF_EventHeader* a_event, void*)
		{
			const auto& event = *reinterpret_cast<const SWF_ShowFilterEvent*>(a_event);
			return showVeto->open->load(std::memory_order_relaxed) != 0 && showVeto->vetoes(event.object) ? 1 : 0;
		}

		bool Subscribe()
		{
			const auto* api = SceneWriter::Api();
			if (!api || !(api->GetCaps() & SWF_CAP_HIDDEN)) {
				logger::warn("[DCLF] hidden events not installed: SceneWriterFramework has no hidden stores (see the [SWF] log lines)");
				return false;
			}
			SWF_Subscription filter = 0;
			if (const auto result = api->AddFilter(SWF_FILTER_SHOW, 0, &VetoShow, nullptr, &filter); result != SWF_OK) {
				logger::warn("[DCLF] hidden events not installed: the show filter was refused (SWF result {})", result);
				return false;
			}
			if (!SceneWriter::Subscribe(SWF_EVENT_HIDDEN, &OnHidden)) {
				api->RemoveFilter(filter);
				return false;
			}
			if (api->abiMinor >= 2 && !SceneWriter::Subscribe(SWF_EVENT_SCENE_WRITE, &OnSceneWrite))
				::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused its scene write events at ABI minor 2");
			logger::info("[DCLF] hidden events: {} SceneWriterFramework stores", api->SiteCount(SWF_EVENT_HIDDEN));
			return true;
		}
	}

	void InstallStores()
	{
		// The cells must be set before the first event, because the filter reads them.
		showVeto = &Extensions::RegisteredShowVeto();
		const auto* api = SceneWriter::Api();
		writeSite = api ? api->SiteCount(SWF_EVENT_HIDDEN) : 0;
		Extensions::NoteHiddenSourceSites(writeSite + 1);
		Scene::hiddenEventsInstalled = Subscribe();
	}
}
