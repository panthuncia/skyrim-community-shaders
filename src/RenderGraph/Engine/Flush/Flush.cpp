#include "RenderGraph/Engine/Flush/Flush.h"
#include "Snapshot/EngineReads.h"

#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <array>
#include <atomic>
#include <intrin.h>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

namespace RenderGraph::Engine::Flush
{
	namespace
	{
		// More than ID3D11DeviceContext's 115 vtable slots.
		constexpr std::size_t kSlots = 128;

		enum class SlotClass : std::uint8_t
		{
			Other,     // not counted
			Draw,      // counted
			Dispatch,  // counted
		};

		struct Config
		{
			bool installed = false;
			std::array<SlotClass, kSlots> slots{};
			std::array<const char*, kSlots> names{};
		};

		Config& GetConfig()
		{
			static Config s_config;
			return s_config;
		}

		/** @brief An engine function that flushes the shadow state and then draws or dispatches. */
		struct DrawFunction
		{
			std::uint64_t se;
			std::uint64_t ae;
			const char* name;
		};

		// Each calls SetDirtyStates before drawing. SE 75488 only calls 75489, while AE 77275 inlines that call and draws itself.
		constexpr std::array<DrawFunction, 11> kDrawFunctions{ {
			{ 75477, 77263, "DrawTriShape" },
			{ 75478, 77264, "DrawTriShape (alternate index buffer)" },
			{ 75479, 77265, "DrawInstancedTriShape" },
			{ 75488, 77275, "DrawDynamicTriShape (DrawData copy)" },
			{ 75489, 77276, "DrawDynamicTriShape" },
			{ 75490, 77277, "DrawParticleShaderTriShape" },
			{ 75491, 77278, "DrawStripParticles" },
			{ 75494, 77282, "DrawLineShape" },
			{ 75499, 77289, "DrawDynamicLineShape" },
			{ 75500, 77290, "DrawDynamicLineShape (offset)" },
			{ 75532, 77329, "DispatchCSShader" },
		} };

		const DrawFunction* DrawFunctionOf(std::uint64_t a_id)
		{
			const bool ae = REL::Module::IsAE();
			for (const auto& function : kDrawFunctions)
				if ((ae ? function.ae : function.se) == a_id)
					return &function;
			return nullptr;
		}

		/** @brief Whether a_caller is inside one of kDrawFunctions. The last answer per thread is cached, since draws repeat their site. */
		bool InDrawFunction(void* a_caller)
		{
			thread_local void* lastCaller = nullptr;
			thread_local bool lastResult = false;
			if (a_caller == lastCaller)
				return lastResult;
			std::uint64_t id = 0, within = 0;
			const bool result = Tripwire::GameFunction(a_caller, id, within) && DrawFunctionOf(id) != nullptr;
			lastCaller = a_caller;
			lastResult = result;
			return result;
		}

		/** @brief DispatchCSShader's address. Its Dispatch is a tail jump, so the D3D11 call's return address is its caller's. */
		void* DispatchFunction()
		{
			static void* const address = reinterpret_cast<void*>(REL::RelocationID(75532, 77329).address());
			return address;
		}

		thread_local int t_dispatchScope = 0;

		struct DispatchScope
		{
			DispatchScope() { ++t_dispatchScope; }
			~DispatchScope() { --t_dispatchScope; }
			DispatchScope(const DispatchScope&) = delete;
			DispatchScope& operator=(const DispatchScope&) = delete;
		};

		// The counts are logged once every this many main-thread frames.
		constexpr std::uint64_t kReportFrames = 600;

		struct Counters
		{
			std::atomic<std::uint64_t> dropped{ 0 };
			std::atomic<std::uint64_t> outside{ 0 };
			std::uint64_t frames = 0;
		};

		Counters& GetCounters()
		{
			static Counters s_counters;
			return s_counters;
		}

		// ---- A dropped draw's description -------------------------------------------------------------------------------------

		struct DrawKey
		{
			std::array<void*, 3> up{};
			void* viaCs = nullptr;         // set when CS called the draw function
			const char* shader = nullptr;  // the technique's file name, or the ImageSpace shader's name
			std::int32_t shaderType = -1;  // the last technique's BSShader::Type
			std::uint32_t vertex = 0;      // RSS.currentVertexShader->id
			std::uint32_t pixel = 0;       // RSS.currentPixelShader->id
			std::int32_t target = -1;      // RSS renderTargets[0]
			std::int32_t depth = -1;       // RSS depthStencil
		};

		/** @brief Fills the game frames above the draw's site, and the first CS frame among them. */
		void Walk(void* a_site, DrawKey& a_key)
		{
			std::array<void*, 32> frames{};
			const USHORT count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
			USHORT i = 0;
			while (i < count && frames[i] != a_site)
				++i;
			if (i < count) {
				++i;
			} else {
				// DispatchCSShader tail-jumps into the context, so walk from the first frame outside CS.
				i = 0;
				while (i < count && Tripwire::IsOwnCode(frames[i]))
					++i;
			}
			std::size_t up = 0;
			for (; i < count && up < a_key.up.size(); ++i) {
				void* frame = frames[i];
				if (Tripwire::IsGameCode(frame))
					a_key.up[up++] = frame;
				else if (!a_key.viaCs && t_dispatchScope == 0 && Tripwire::IsOwnCode(frame))
					a_key.viaCs = frame;
			}
		}

		void Technique(DrawKey& a_key)
		{
			if (const auto* state = globals::state; state && state->currentShader) {
				const auto* shader = state->currentShader;
				const auto type = shader->shaderType.get();
				a_key.shaderType = static_cast<std::int32_t>(type);
				a_key.shader = type == RE::BSShader::Type::ImageSpace ? static_cast<const RE::BSImagespaceShader*>(shader)->name : shader->fxpFilename;
			}
			if (CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentVertexShader) && *CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentVertexShader))
				a_key.vertex = (*CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentVertexShader))->id;
			if (CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentPixelShader) && *CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentPixelShader))
				a_key.pixel = (*CS_ENGINE_READ(Shaders, RecordTime, globals::game::currentPixelShader))->id;
		}

		/** @brief The view from the shadow state at +0x18 and +0x38. */
		void View(DrawKey& a_key)
		{
			const auto* rss = reinterpret_cast<const std::uint8_t*>(CS_ENGINE_READ(ShadowState, RecordTime, globals::game::shadowState));
			if (!rss)
				return;
			a_key.target = *reinterpret_cast<const std::int32_t*>(rss + 0x18);
			a_key.depth = *reinterpret_cast<const std::int32_t*>(rss + 0x38);
		}

		std::string FunctionName(const void* a_site)
		{
			std::uint64_t id = 0, within = 0;
			if (Tripwire::GameFunction(a_site, id, within)) {
				if (const auto* function = DrawFunctionOf(id))
					return fmt::format("{} ({}/{})", function->name, function->se, function->ae);
				return fmt::format("ID {}", id);
			}
			return Tripwire::DescribeCaller(a_site);
		}

		const char* SlotName(std::uint16_t a_slot)
		{
			const auto& config = GetConfig();
			return a_slot < kSlots && config.names[a_slot] ? config.names[a_slot] : "?";
		}

		/** @brief Closes the frame and logs the counts every kReportFrames frames. Main thread. */
		void Tick()
		{
			auto& counters = GetCounters();
			if (++counters.frames < kReportFrames)
				return;
			const auto frames = counters.frames;
			counters.frames = 0;
			logger::info("[ORG] engine draws: {} frames: {} engine draws and dispatches dropped in the draw functions, {} outside them", frames,
				counters.dropped.exchange(0), counters.outside.exchange(0));
		}

		// ---- The census ---------------------------------------------------------------------------------------------------

		/**
		* @brief Whether an engine draw the context drops is a tap error. Off by default: loading screens and some world draws are still
		* dropped until their sources are ported. CS_ORG_ENGINE_DRAW_TAP=1 turns it on.
		*/
		bool EngineDrawTap()
		{
			static const bool on = Switches::On("CS_ORG_ENGINE_DRAW_TAP");
			return on;
		}

		/** @brief Whether a game draw outside every draw function is a tap error. CS_ORG_ENGINE_DRAW_TAP=0 turns it off. */
		bool EngineCensusTap()
		{
			static const bool on = !Switches::Off("CS_ORG_ENGINE_DRAW_TAP");
			return on;
		}

		/**
		* @brief An engine draw the context dropped in a draw function, which no port took. An EngineDraw tap error with
		* CS_ORG_ENGINE_DRAW_TAP=1. The full caller chain is logged once per site and chain, since DrawTriShape serves many callers.
		*/
		void NoteDropped(std::uint16_t a_slot, void* a_caller)
		{
			GetCounters().dropped.fetch_add(1, std::memory_order_relaxed);
			void* const site = t_dispatchScope > 0 ? DispatchFunction() : a_caller;
			const auto tapError = [&] {
				if (!EngineDrawTap())
					return;
				::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::EngineDraw, ::RenderGraph::Frame::SegmentId::Count,
					fmt::format("engine {} in {} at {}, dropped by the engine's context", SlotName(a_slot), FunctionName(site), Tripwire::DescribeCaller(site)));
			};
			DrawKey key;
			Walk(a_caller, key);
			static std::mutex s_lock;
			static std::set<std::array<void*, 4>> s_seen;
			{
				std::lock_guard guard(s_lock);
				if (!s_seen.insert({ site, key.up[0], key.up[1], key.up[2] }).second) {
					tapError();
					return;
				}
			}
			std::string chain;
			for (const void* frame : key.up)
				if (frame)
					chain += fmt::format(" <- {}", Tripwire::DescribeCaller(frame));
			if (key.viaCs)
				chain += fmt::format(" (via CS {})", Tripwire::DescribeCaller(key.viaCs));
			{
				std::array<void*, 48> frames{};
				const USHORT count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
				std::size_t game = 0;
				std::string further;
				for (USHORT i = 0; i < count; ++i)
					if (Tripwire::IsGameCode(frames[i]) && game++ >= 4 && game <= 12)
						further += fmt::format(" <- {}", Tripwire::DescribeCaller(frames[i]));
				if (!further.empty())
					chain += fmt::format("; further up{}", further);
			}
			Technique(key);
			View(key);
			chain += fmt::format("; technique {} (type {}, vertex {:#x}, pixel {:#x}), target {} depth {}", key.shader ? key.shader : "none", key.shaderType, key.vertex,
				key.pixel, key.target, key.depth);
			logger::error("[ORG] engine draws: engine {} in {} at {}{} was dropped by the engine's context (first from these callers)", SlotName(a_slot),
				FunctionName(site), Tripwire::DescribeCaller(site), chain);
			tapError();
		}

		/**
		* @brief Development builds: a game draw or dispatch outside every draw function. An EngineDraw tap error. Only game code counts:
		* another plugin's call is logged by the tripwire itself.
		*/
		void NoteOutside(std::uint16_t a_slot, void* a_caller)
		{
			GetCounters().outside.fetch_add(1, std::memory_order_relaxed);
#if defined(CS_RELEASE_PROFILE)
			(void)a_slot;
			(void)a_caller;
#else
			if (!EngineCensusTap() || !Tripwire::IsGameCode(a_caller))
				return;
			::RenderGraph::Frame::Driver::NoteTapError(::RenderGraph::Frame::Driver::TapError::EngineDraw, ::RenderGraph::Frame::SegmentId::Count,
				fmt::format("engine {} at {} came from outside every draw function, and the engine's context dropped it", SlotName(a_slot),
					Tripwire::DescribeCaller(a_caller)));
#endif
		}

		void Observe(std::uint16_t a_slot, void* a_caller)
		{
			const auto& config = GetConfig();
			const SlotClass type = a_slot < kSlots ? config.slots[a_slot] : SlotClass::Other;
			if (type == SlotClass::Other)
				return;
			if ((type == SlotClass::Dispatch && t_dispatchScope > 0) || InDrawFunction(a_caller))
				NoteDropped(a_slot, a_caller);
			else
				NoteOutside(a_slot, a_caller);
		}

		// ---- The detour -----------------------------------------------------------------------------------------------------

		/** @brief Renderer::DispatchCSShader (75532 / 77329), so a dropped dispatch names its engine site. Wraps the engine's code only. */
		struct DispatchCSShader
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::ComputeShader* a_shader, std::uint32_t a_x, std::uint32_t a_y,
				std::uint32_t a_z)
			{
				DispatchScope dispatch;
				func(a_renderer, a_shader, a_x, a_y, a_z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		SlotClass ClassOf(std::string_view a_name)
		{
			if (a_name == "Draw" || a_name == "DrawIndexed" || a_name == "DrawInstanced" || a_name == "DrawIndexedInstanced" || a_name == "DrawAuto" ||
				a_name == "DrawIndexedInstancedIndirect" || a_name == "DrawInstancedIndirect")
				return SlotClass::Draw;
			if (a_name == "Dispatch" || a_name == "DispatchIndirect")
				return SlotClass::Dispatch;
			return SlotClass::Other;
		}
	}

	bool EngineDrawsDropped()
	{
		return GetConfig().installed && Tripwire::GuestContextInstalled();
	}

	void Install()
	{
		auto& config = GetConfig();
		if (!GraphMode())
			return;
		const auto names = Tripwire::SlotNames(Tripwire::Kind::DeviceContext);
		for (std::size_t i = 0; i < names.size() && i < kSlots; ++i) {
			config.names[i] = names[i];
			config.slots[i] = ClassOf(names[i]);
		}
		// Before ImageSpace's and Hooks.cpp's DispatchCSShader detours.
		stl::detour_thunk<DispatchCSShader>(REL::RelocationID(75532, 77329));
		if (!DispatchCSShader::func.address())
			logger::error("[ORG] engine draws: the DispatchCSShader detour did not attach; a dropped dispatch names its caller instead");
		Tripwire::SetContextObserver(&Observe);
		MainTick::Add("Engine draw census", [] { Tick(); });
		config.installed = true;
		logger::info("[ORG] engine draws: the engine's context keeps binds and drops draws (graph mode), reports every {} frames", kReportFrames);
		logger::info("[ORG] engine draws: a dropped engine draw is {}, and a game draw outside the draw functions is {} (CS_ORG_ENGINE_DRAW_TAP=1 or 0)",
			EngineDrawTap() ? "an EngineDraw tap error" : "logged", EngineCensusTap() ? "an EngineDraw tap error" : "logged");
	}

	void BindVertexShader(ID3D11VertexShader* a_shader)
	{
		if (!GraphMode()) {
			globals::d3d::context->VSSetShader(a_shader, nullptr, 0);
			return;
		}
		// The engine's context handle keeps the bind for its readers. Nothing draws with it.
		if (!Tripwire::GuestContextInstalled())
			RenderGraph::Fatal("engine flush", "a vertex shader bind for an engine draw came before the engine's context handle was in place");
		Tripwire::EngineContext()->VSSetShader(a_shader, nullptr, 0);
	}

	void BindPixelShader(ID3D11PixelShader* a_shader)
	{
		if (!GraphMode()) {
			globals::d3d::context->PSSetShader(a_shader, nullptr, 0);
			return;
		}
		if (!Tripwire::GuestContextInstalled())
			RenderGraph::Fatal("engine flush", "a pixel shader bind for an engine draw came before the engine's context handle was in place");
		Tripwire::EngineContext()->PSSetShader(a_shader, nullptr, 0);
	}
}
