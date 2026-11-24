#include "RenderGraph/Engine/Native/Native.h"

#include "DxvkLoader.h"
#include "Features/RenderDoc.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Switches.h"
#include "Utils/SwitchFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <ranges>
#include <string>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		/** @brief Switches that select a DXVK path native mode does not have. Each one set is a startup error. */
		struct Conflict
		{
			const char* name;
			const char* value;
			const char* why;
		};
		constexpr std::array kConflicts{
			Conflict{ "CS_NATIVE_D3D11", "1", "it runs the game on the system D3D11 runtime, not on ORG" },
			Conflict{ "CS_ORG", "0", "native mode is the render graph: it cannot be off" },
			Conflict{ "CS_ORG", "features", "native mode is the render graph: it cannot be off" },
		};

		std::atomic<int> g_stage{ 0 };
	}

	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_NATIVE");
		return requested;
	}

	void Stage(std::string_view a_stage, std::string_view a_detail)
	{
		const int number = g_stage.fetch_add(1, std::memory_order_relaxed) + 1;
		if (a_detail.empty())
			logger::info("[ORG native] stage {}: {}", number, a_stage);
		else
			logger::info("[ORG native] stage {}: {}: {}", number, a_stage, a_detail);
	}

	void Fail(std::string_view a_dependency, std::string_view a_why)
	{
		RenderGraph::Fatal("native mode (CS_ORG_NATIVE=1)",
			fmt::format("{} still needs DXVK ({}). Native mode has no DXVK and does not fall back to it. Fix the cause, or turn CS_ORG_NATIVE off "
						"to run on DXVK.",
				a_dependency, a_why));
	}

	namespace
	{
		/** @brief An environment switch read directly, since Switches caches its reads. Empty when unset or too long. */
		std::string Environment(const char* a_name)
		{
			char value[32]{};
			const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length != 0 && length < sizeof(value) ? std::string(value) : std::string();
		}

		/** @brief Whether this is guest mode, DXVK with the render graph on. Read from the environment because this runs before the first cached read. */
		bool GuestMode()
		{
			const auto org = Environment("CS_ORG");
			if (org == "0" || org == "features")
				return false;
			return Environment("CS_NATIVE_D3D11") != "1";
		}
	}

	void RequireDclf(std::string_view a_cause)
	{
		if (!GraphMode())
			return;
		RenderGraph::Fatal(Requested() ? "native mode (CS_ORG_NATIVE=1)" : "guest mode (DXVK with the render graph)",
			fmt::format("it cannot run without Drawcall Limit Fix: {}. This mode withholds the engine's draws and DCLF draws the world, so the "
						"world would be empty. Turn DCLF on, or run without the render graph (CS_ORG=0).",
				a_cause));
	}

	void ApplySwitches()
	{
		// DCLF draws the world's geometry in native and guest mode, and the engine's draws are dropped there.
		if (std::ranges::contains(Switches::List("CS_DISABLE_FEATURES"), std::string_view("DrawcallLimitFix")))
			RequireDclf("CS_DISABLE_FEATURES names DrawcallLimitFix");
		if (Util::SwitchFile::Value("CS_DCLF", false) == "0")
			RequireDclf("CS_DCLF=0");
		if (!Requested())
			return;
		for (const auto& conflict : kConflicts) {
			if (Switches::Equals(conflict.name, conflict.value))
				Fail(fmt::format("{}={}", conflict.name, conflict.value), conflict.why);
		}
		Stage("switches", "no conflicting switch is set");
		// D3D12 sets no DCLF switch. DCLF reads the device's indirect-command features at setup: without pipeline sets it draws
		// colour with plain draws only (DrawPipelines::DeviceGenerated).
	}

	void CheckStartup()
	{
		if (!Requested())
			return;
		if (DxvkLoader::IsLoaded())
			Fail("DXVK", "dxvk_d3d11.dll was loaded");
		if (::GetModuleHandleW(L"dxvk_d3d11.dll") || ::GetModuleHandleW(L"dxvk_dxgi.dll"))
			Fail("another plugin", "a DXVK DLL is in the process (another mod loads DXVK)");
		// RenderDoc captures the native device (its Vulkan or D3D12 layer), not the engine's D3D11.
		if (RenderDoc::IsLoaded())
			logger::info("[ORG native] RenderDoc is loaded: its captures hold the native device's frames");
		// Streamline stands down until it runs on the native device, so upscaling falls back to TAA.
		Stage("checks", "DXVK is not loaded, and no feature that needs it is on");
	}

	std::filesystem::path RuntimeDirectory()
	{
		return DxvkLoader::GetRuntimeDir();
	}

	void AfterInitD3D()
	{
		if (!Requested())
			return;
		auto* renderer = RE::BSGraphics::Renderer::GetSingleton();
		if (!renderer)
			Fail("the engine's renderer", "it does not exist after InitD3D");
		auto& data = renderer->GetRuntimeData();
		REL::Relocation<ID3D11Device**> deviceGlobal{ REL::RelocationID(524729, 411348) };
		REL::Relocation<ID3D11DeviceContext**> contextGlobal{ REL::RelocationID(524771, 411391) };
		auto* device = reinterpret_cast<ID3D11Device*>(data.forwarder);
		auto* context = reinterpret_cast<ID3D11DeviceContext*>(data.context);
		auto* swapChain = reinterpret_cast<IDXGISwapChain*>(data.renderWindows[0].swapChain);
		const bool handles = Tripwire::IsHandle(device) && Tripwire::IsHandle(context) && Tripwire::IsHandle(swapChain) && *deviceGlobal == device &&
		                     *contextGlobal == context;
		if (!handles)
			Fail("the engine's device fields",
				fmt::format("RendererData +0x38/+0x40, window 0's swap chain or the device and context globals do not hold native mode's handles "
							"(device {} / {}, context {} / {}, swap chain {})",
					fmt::ptr(device), fmt::ptr(*deviceGlobal), fmt::ptr(context), fmt::ptr(*contextGlobal), fmt::ptr(swapChain)));
		Stage("engine init", fmt::format("InitD3D ran on the handles (device {}, context {}, swap chain {}); window 0's views {} / {}", fmt::ptr(device),
								 fmt::ptr(context), fmt::ptr(swapChain), fmt::ptr(data.renderWindows[0].renderView), fmt::ptr(data.renderWindows[0].resourceView)));
	}
}

namespace RenderGraph::Engine
{
	bool GraphMode()
	{
		// VR's engine loop was not checked, so it never runs on the graph.
		static const bool on = !REL::Module::IsVR() && (Native::Requested() || Native::GuestMode());
		return on;
	}
}
