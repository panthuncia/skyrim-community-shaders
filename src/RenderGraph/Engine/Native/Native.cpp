#include "RenderGraph/Engine/Native/Native.h"

#include "DxvkLoader.h"
#include "Features/RenderDoc.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
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
		/** @brief Switches that select a DXVK path native mode does not have: each one set is a startup error. */
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

		/**
		 * @brief Switches that narrow a port to part of its work: set to anything but every part, each one is a startup error. The parts
		 * left out stay the engine's draws, and native mode drops engine draws without a word.
		 */
		struct Narrowing
		{
			const char* name;
			std::string_view all;  // the parts, comma separated, that cover the whole port; unset covers it too
			bool anyCase;          // the reader ignores case
			const char* why;
		};
		constexpr std::array kNarrowings{
			Narrowing{ "CS_ORG_WATER_PARTS", "mask,surface", false, "it leaves the water views it does not name to the engine" },
			Narrowing{ "CS_ORG_SHADOW_MASK_VIEWS", "world,menu", false, "it leaves the shadow mask views it does not name to the engine" },
			Narrowing{ "CS_ORG_IMAGESPACE_EFFECTS", "all", true, "it leaves the ImageSpace effects it does not name to the engine" },
		};

		/** @brief Whether a narrowing switch names every part (or is unset), compared as its reader compares them. */
		bool CoversAll(const Narrowing& a_switch)
		{
			const auto items = Switches::List(a_switch.name);
			if (items.empty())
				return true;
			for (const auto part : std::views::split(a_switch.all, ',')) {
				const std::string_view wanted(part.begin(), part.end());
				const bool named = std::ranges::any_of(items, [&](const std::string& a_item) {
					if (!a_switch.anyCase)
						return a_item == wanted;
					return a_item.size() == wanted.size() && _strnicmp(a_item.data(), wanted.data(), wanted.size()) == 0;
				});
				if (!named)
					return false;
			}
			return true;
		}

		/**
		 * @brief Drawcall Limit Fix on D3D12: what has no D3D12 port yet, kept off, and the plain draws D3D12 needs kept on. DCLF's D3D12
		 * port draws only with its pulled builds and plain indirect draws (DrawPipelines, DxilStages). A value the environment sets
		 * already is kept, so a run can try a part.
		 */
		struct DclfSwitch
		{
			const char* name;
			const char* value;
			const char* why;
		};
		constexpr std::array kD3D12DclfSwitches{
			DclfSwitch{ "CS_DCLF_MAIN_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_TERRAIN_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_DECAL_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_FORWARD_PLAIN", "1", "D3D12 draws colour with plain draws only" },
		};

		std::string g_d3d12DclfOff;

		std::atomic<int> g_stage{ 0 };
	}

	std::string D3D12DclfOff()
	{
		return g_d3d12DclfOff;
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
		const auto message = fmt::format(
			"Community Shaders native mode (CS_ORG_NATIVE=1) cannot run: {} still needs DXVK ({}). Native mode has no DXVK and does not fall "
			"back to it. Fix the cause, or turn CS_ORG_NATIVE off to run on DXVK.",
			a_dependency, a_why);
		logger::critical("[ORG native] {}", message);
		stl::report_and_fail(message);
	}

	namespace
	{
		/** @brief The value of an environment switch, read directly (Switches caches its reads); empty when unset or too long. */
		std::string Environment(const char* a_name)
		{
			char value[32]{};
			const DWORD length = ::GetEnvironmentVariableA(a_name, value, sizeof(value));
			return length != 0 && length < sizeof(value) ? std::string(value) : std::string();
		}

		/**
		 * @brief Guest mode is DXVK with the render graph on: not native, not CS_ORG=0 or CS_ORG=features (no graph), not the system
		 * D3D11 runtime. Read from the environment: this runs before the first cached read.
		 */
		bool GuestMode()
		{
#if defined(CS_HAS_RENDER_GRAPH)
			const auto org = Environment("CS_ORG");
			if (org == "0" || org == "features")
				return false;
			return Environment("CS_NATIVE_D3D11") != "1";
#else
			return false;
#endif
		}
	}

	bool GraphMode()
	{
		static const bool on = Requested() || GuestMode();
		return on;
	}

	void ApplySwitches()
	{
		if (!Requested())
			return;
		for (const auto& conflict : kConflicts) {
			if (Switches::Equals(conflict.name, conflict.value))
				Fail(fmt::format("{}={}", conflict.name, conflict.value), conflict.why);
		}
		for (const auto& narrowing : kNarrowings) {
			if (!CoversAll(narrowing))
				Fail(fmt::format("{}={}", narrowing.name, Switches::Value(narrowing.name)), narrowing.why);
		}
		// Drawcall Limit Fix draws the world's geometry in native mode: without it the engine's draws would be dropped.
		if (std::ranges::contains(Switches::List("CS_DISABLE_FEATURES"), std::string_view("DrawcallLimitFix")))
			Fail("CS_DISABLE_FEATURES naming DrawcallLimitFix", "the world's draws it takes would stay the engine's");
		if (Util::SwitchFile::Value("CS_DCLF", false) == "0")
			Fail("CS_DCLF=0", "the world's draws Drawcall Limit Fix takes would stay the engine's");
		Stage("switches", "no conflicting switch is set");
		// D3D12: Drawcall Limit Fix's parts without a D3D12 port, by their own switches (read later, at DCLF's install).
		// GraphicsAPI::ApplyAtBoot set CS_ORG_NATIVE_BACKEND already, so the cached read sees its value.
		if (!Switches::EqualsNoCase("CS_ORG_NATIVE_BACKEND", "d3d12"))
			return;
		std::string set, kept;
		for (const auto& entry : kD3D12DclfSwitches) {
			char value[32]{};
			const DWORD length = ::GetEnvironmentVariableA(entry.name, value, sizeof(value));
			if (length != 0 && length < sizeof(value)) {
				if (std::string_view(value) != entry.value)
					kept += fmt::format("{}{}={} (D3D12 wants {}: {})", kept.empty() ? "" : ", ", entry.name, value, entry.value, entry.why);
				continue;
			}
			::SetEnvironmentVariableA(entry.name, entry.value);
			_putenv_s(entry.name, entry.value);
			set += fmt::format("{}{}={} ({})", set.empty() ? "" : ", ", entry.name, entry.value, entry.why);
		}
		g_d3d12DclfOff = set.empty() ? std::string("nothing") : set;
		if (!kept.empty())
			g_d3d12DclfOff += fmt::format("; kept from the environment: {}", kept);
		logger::info("[ORG native] D3D12: Drawcall Limit Fix switches set: {}", g_d3d12DclfOff);
	}

	void CheckStartup()
	{
		if (!Requested())
			return;
#if !defined(CS_HAS_RENDER_GRAPH)
		Fail("this build", "it was built without OpenRenderGraph (CS_ORG_ROOT missing)");
#else
		if (DxvkLoader::IsLoaded())
			Fail("DXVK", "dxvk_d3d11.dll was loaded");
		if (::GetModuleHandleW(L"dxvk_d3d11.dll") || ::GetModuleHandleW(L"dxvk_dxgi.dll"))
			Fail("another plugin", "a DXVK DLL is in the process (another mod loads DXVK)");
		if (RenderDoc::IsLoaded())
			Fail("the RenderDoc feature", "it captures the game's D3D11, which runs on DXVK");
		// Upscaling runs: its TAA and depth upscale are graph passes (ORGUpscaling). Streamline stands down until it runs on the native
		// device, so its upscalers and frame generation are unavailable and the method falls back to TAA (Upscaling::GetUpscaleMethod).
		Stage("checks", "DXVK is not loaded, and no feature that needs it is on");
#endif
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
