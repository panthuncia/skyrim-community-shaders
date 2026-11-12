#include "RenderGraph/Engine/Native/Native.h"

#include "DxvkLoader.h"
#include "Features/RenderDoc.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"

#include <array>
#include <atomic>
#include <string>

namespace RenderGraph::Engine::Native
{
	namespace
	{
		/**
		 * @brief The port switches native mode turns on: every engine subsystem and CS feature on ORG (the set of
		 * tools/cs-ab/scenarios/latch-census-short.env, without its diagnostics), and the flush latch, so no engine draw is left to
		 * D3D11. Without one of them its subsystem would call the native handles, which do nothing.
		 */
		constexpr std::array kPortSwitches{
			"CS_ORG_ENGINE_FLUSH_LATCH",
			"CS_ORG_SHADOW_MASK",
			"CS_ORG_COMPOSITE",
			"CS_ORG_IBL",
			"CS_ORG_SCREEN_SPACE_SHADOWS",
			"CS_ORG_TERRAIN_SHADOWS",
			"CS_ORG_VOLUMETRIC_SHADOWS",
			"CS_ORG_SKYLIGHTING",
			"CS_ORG_GRASS_COLLISION",
			"CS_ORG_LIGHT_LIMIT_FIX_SHADOWS",
			"CS_ORG_SCREEN_SPACE_GI",
			"CS_ORG_SUBSURFACE_SCATTERING",
			"CS_ORG_EXPONENTIAL_HEIGHT_FOG",
			"CS_ORG_DYNAMIC_CUBEMAPS",
			"CS_ORG_CLOUD_SHADOWS",
			"CS_ORG_POST_PROCESSING",
			"CS_ORG_HDR_DISPLAY",
			"CS_ORG_UPSCALING",
			"CS_ORG_SCREENSHOT",
			"CS_ORG_TERRAIN_BLENDING",
			"CS_ORG_GLINTS",
			"CS_ORG_E11_RAYS",
			"CS_ORG_FX",
			"CS_ORG_IMGUI",
			"CS_ORG_ENGINE_TARGETS",
			"CS_ORG_ENGINE_TARGET_HANDLES",
			"CS_ORG_ENGINE_BUFFERS",
			"CS_ORG_ENGINE_TEXTURES",
			"CS_ORG_ENGINE_SHADERS",
			"CS_ORG_TEXTURE_STREAMING",
			"CS_ORG_ENGINE_CONSTANTS",
			"CS_ORG_ENGINE_SKELETON",
			"CS_ORG_SKY",
			"CS_ORG_SKY_CLOUDS",
			"CS_ORG_OCCLUSION_QUERIES",
			"CS_ORG_VOLUMETRIC_LIGHTING",
			"CS_ORG_WATER",
			"CS_ORG_BLOOD_SPLATTER",
			"CS_ORG_PRECIPITATION",
			"CS_ORG_IMAGESPACE",
			"CS_ORG_MENU3D",
			"CS_ORG_MAP",
			"CS_ORG_SCALEFORM",
			"CS_ORG_BINK",
			"CS_ORG_LOADING_THREAD",
		};

		/** @brief Switches that select a DXVK path native mode does not have: each one set is a startup error. */
		struct Conflict
		{
			const char* name;
			const char* value;
			const char* why;
		};
		constexpr std::array kConflicts{
			Conflict{ "CS_NATIVE_D3D11", "1", "it runs the game on the system D3D11 runtime, not on ORG" },
			Conflict{ "CS_ORG_OWNED_FRAME", "1", "the owned frame runs DXVK as a guest on BasicRHI's device; native mode has no DXVK" },
			Conflict{ "CS_ORG", "0", "native mode is the render graph: it cannot be off" },
			Conflict{ "CS_ORG", "features", "native mode is the render graph: it cannot be off" },
			Conflict{ "CS_ORG_SCALEFORM_D3D11_PARTS", "1", "it draws Scaleform's remaining parts through D3D11" },
		};

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
			DclfSwitch{ "CS_DCLF_PIPELINE", "gpu", "the CPU pipeline records Vulkan command buffers through external commands" },
			DclfSwitch{ "CS_DCLF_FORWARD_DIRECT", "0", "the forward family's CPU sites record Vulkan command buffers" },
			DclfSwitch{ "CS_DCLF_MAIN_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_TERRAIN_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_DECAL_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_FORWARD_PLAIN", "1", "D3D12 draws colour with plain draws only" },
			DclfSwitch{ "CS_DCLF_SHADOWS", "0", "the shadow views' Utility builds read pushed addresses, not ported" },
			DclfSwitch{ "CS_DCLF_FORWARD", "0", "the forward epoch is not checked on D3D12 yet" },
			DclfSwitch{ "CS_DCLF_REFRACTION", "0", "the refraction draw uses the shadow views' layout" },
			DclfSwitch{ "CS_DCLF_EFFECTS", "0", "the Effect program family's builds are not ported" },
			DclfSwitch{ "CS_DCLF_OPAQUE_EFFECTS", "0", "the Effect program family's builds are not ported" },
			DclfSwitch{ "CS_DCLF_BILLBOARDS", "0", "the Effect program family's builds are not ported" },
			DclfSwitch{ "CS_DCLF_PARTICLES", "0", "the particle path is not ported" },
			DclfSwitch{ "CS_DCLF_MASTER_PARTICLES", "0", "the particle path is not ported" },
			DclfSwitch{ "CS_DCLF_BILLBOARD_PARTICLES", "0", "the particle path is not ported" },
			DclfSwitch{ "CS_DCLF_GRASS", "0", "the grass draw family (RunGrass) is not ported" },
			DclfSwitch{ "CS_DCLF_LOD_TREES", "0", "the tree LOD draw family (DistantTree) is not ported" },
			DclfSwitch{ "CS_DCLF_FIRST_PERSON", "0", "first person's sites are not checked on D3D12 yet" },
			DclfSwitch{ "CS_DCLF_REFLECTIONS", "0", "the water reflection's views are not ported" },
			DclfSwitch{ "CS_DCLF_SKYLIGHT", "0", "Skylighting's occlusion view uses the Utility builds" },
			DclfSwitch{ "CS_DCLF_PRECIPITATION", "0", "the precipitation occlusion view uses the Utility builds" },
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

	void ApplySwitches()
	{
		if (!Requested())
			return;
		for (const auto& conflict : kConflicts) {
			if (Switches::Equals(conflict.name, conflict.value))
				Fail(fmt::format("{}={}", conflict.name, conflict.value), conflict.why);
		}
		std::string turnedOn;
		for (const char* name : kPortSwitches) {
			// Read the environment directly: a switch read through Switches is cached, and this must run before the first read.
			char value[16]{};
			const DWORD length = ::GetEnvironmentVariableA(name, value, sizeof(value));
			if (length != 0 && length < sizeof(value)) {
				if (std::string_view(value) == "0")
					Fail(fmt::format("{}=0", name), "its subsystem would draw through D3D11");
				continue;
			}
			::SetEnvironmentVariableA(name, "1");
			// std::getenv reads the C runtime's own copy, which SetEnvironmentVariable does not change.
			_putenv_s(name, "1");
			turnedOn += turnedOn.empty() ? name : fmt::format(", {}", name);
		}
		// A switch some code read before this point is cached without the value set here: that subsystem would stay on D3D11.
		for (const char* name : kPortSwitches) {
			if (Switches::Value(name).empty())
				Fail(name, "it was read before native mode could turn it on");
		}
		Stage("switches", turnedOn.empty() ? std::string("every port switch was set already") : fmt::format("turned on {}", turnedOn));
		// D3D12: Drawcall Limit Fix's parts without a D3D12 port, by their own switches (read later, at DCLF's install).
		char backend[16]{};
		const DWORD backendLength = ::GetEnvironmentVariableA("CS_ORG_NATIVE_BACKEND", backend, sizeof(backend));
		if (backendLength == 0 || backendLength >= sizeof(backend) || _stricmp(backend, "d3d12") != 0)
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
		if (globals::features::upscaling.loaded)
			Fail("the Upscaling feature",
				"Streamline's upscalers and frame generation take their inputs from DXVK's images (DXVKInterop). Add Upscaling to "
				"CS_DISABLE_FEATURES for native runs");
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
