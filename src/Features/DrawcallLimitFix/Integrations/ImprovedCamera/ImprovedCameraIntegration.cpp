// Improved Camera, an SKSE plugin. In first person it hides the player's head, arms and back weapons right after the shadow maps,
// by shrinking those nodes, so shadows keep the whole body. DCLF samples rows once at the frame's start,
// so it names the player's third-person root as a late pose, and views drawn after the shadows leave the hidden parts out too.

#include "Features/DrawcallLimitFix/Integrations/ImprovedCamera/ImprovedCamera.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

namespace DCLF::Integrations::ImprovedCamera
{
	namespace
	{
		Api::SwitchId latePoseSwitch;

		bool PluginLoaded()
		{
			// Read at the first frame, long after the plugins loaded.
			static const bool loaded = ::GetModuleHandleW(L"ImprovedCameraSE.dll") != nullptr;
			return loaded;
		}

		// Render thread. The player's third-person root while the camera is in first person.
		const void* Root()
		{
			const auto& value = SwitchValue(latePoseSwitch);
			if (value == "0" || (value != "1" && !PluginLoaded()))
				return nullptr;
			const auto* camera = CS_ENGINE_READ(Camera, RecordTime, RE::PlayerCamera::GetSingleton());
			auto* player = CS_ENGINE_READ(Player, RecordTime, RE::PlayerCharacter::GetSingleton());
			if (!camera || !camera->IsInFirstPerson() || !player)
				return nullptr;
			return player->Get3D(false);
		}

		// Any thread. Whether the plugin is loaded, so the late pose is looked for.
		std::string Status()
		{
			const auto& value = SwitchValue(latePoseSwitch);
			const bool on = value != "0" && (value == "1" || PluginLoaded());
			return fmt::format("plugin {}, late pose {}{}", PluginLoaded() ? "loaded" : "not loaded", on ? "on" : "off",
				value.empty() ? "" : fmt::format(" (CS_DCLF_LATE_POSE={})", value));
		}

		void Install(Api::InstallContext& a_context)
		{
			latePoseSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_LATE_POSE", .kind = Api::SwitchKind::Test, .reduced = Api::SwitchReduced::Never,
				.description = "0: the player's parts Improved Camera hides after the shadow maps are not looked for; 1: they are looked for in first person without the plugin too" });
			a_context.RegisterLatePose({ .name = "improved camera first person", .root = &Root });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "improved camera", .install = &Install, .status = &Status };
	}
}
