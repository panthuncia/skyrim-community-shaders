#include "RenderGraph/Engine/Flush/Flush.h"

#include "Globals.h"
#include "RenderGraph/Engine/MainTick.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace RenderGraph::Engine::Flush
{
	namespace
	{
		// More than ID3D11DeviceContext's 115 vtable slots (Tripwire::WatchSet::kMaxSlots).
		constexpr std::size_t kSlots = 128;

		enum class SlotClass : std::uint8_t
		{
			Other,     // forwarded always (Map, Unmap, clears, queries, getters, copies)
			Setter,    // IASet*, VSSet*, PSSet*, GSSet*, HSSet*, DSSet*, CSSet*, OMSet*, RSSet*, SOSet*: dropped inside a latch scope
			Draw,      // Draw*, counted and possibly skipped
			Dispatch,  // Dispatch*, the same
		};

		struct Config
		{
			bool census = false;
			bool latch = false;
			bool installed = false;
			std::uint64_t frames = 600;
			std::size_t top = 25;
			std::array<SlotClass, kSlots> slots{};
			std::array<const char*, kSlots> names{};
		};

		Config& GetConfig()
		{
			static Config s_config;
			return s_config;
		}

		/** @brief An engine function that flushes the shadow state and then draws or dispatches (engine-d3d11-surface.md R1). */
		struct DrawFunction
		{
			std::uint64_t se;
			std::uint64_t ae;
			const char* name;
		};

		// Every BSGraphics draw function, and the one dispatch. Each calls SetDirtyStates before its draw, so with the latch on its
		// draw has no state bound. SE 75488 only calls 75489; AE 77275 inlines that call and draws itself.
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

		/** @brief Whether a_caller is inside one of kDrawFunctions. The last answer per thread is kept: draws repeat their site. */
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

		/** @brief DispatchCSShader's address: its Dispatch is a tail jump, so the D3D11 call's return address is its caller's. */
		void* DispatchFunction()
		{
			static void* const address = reinterpret_cast<void*>(REL::RelocationID(75532, 77329).address());
			return address;
		}

		// Inside one of the four functions with the latch on: the guest context drops their binds.
		thread_local int t_latchScope = 0;
		// Inside DispatchCSShader: its Dispatch is attributed to it.
		thread_local int t_dispatchScope = 0;
		// Inside an EngineDraws scope: the reason's id, else -1. The engine draws here on purpose.
		thread_local int t_engineReason = -1;

		// A draw family's rest of a batch left to the engine (BeginEngineRest): not part of the scopes' nesting, so a pass's own scope
		// (RenderPassImmediately's for a vouched pass) restoring its previous reason never ends it.
		thread_local int t_restReason = -1;

		/** @brief The reason the engine draws now: the innermost scope's, else a family's rest of a batch; -1 for none. */
		int CurrentReason() { return t_engineReason >= 0 ? t_engineReason : t_restReason; }

		bool InEngineDraws() { return CurrentReason() >= 0; }

		struct LatchScope
		{
			LatchScope() { ++t_latchScope; }
			~LatchScope() { --t_latchScope; }
			LatchScope(const LatchScope&) = delete;
			LatchScope& operator=(const LatchScope&) = delete;
		};

		struct DispatchScope
		{
			DispatchScope() { ++t_dispatchScope; }
			~DispatchScope() { --t_dispatchScope; }
			DispatchScope(const DispatchScope&) = delete;
			DispatchScope& operator=(const DispatchScope&) = delete;
		};

		// RSS +0x00 stateUpdateFlags, +0x04 PS resources, +0x08 PS samplers, +0x0C CS resources, +0x10 CS samplers, +0x14 CS UAVs
		// (SE 0x143027eb0, AE 0x14202ab70; engine-d3d11-surface.md R2). SetDirtyStates clears each word it applies.
		constexpr std::size_t kWords = 6;
		constexpr std::uint32_t kVertexDescBit = 0x400;

		std::uint32_t* ShadowWords()
		{
			return reinterpret_cast<std::uint32_t*>(globals::game::shadowState);
		}

		/** @brief Whether a flush would apply anything (isCompute keeps the input layout pending, so its bit does not count). */
		bool NonEmpty(const std::uint32_t* a_words, bool a_isCompute)
		{
			std::uint32_t any = a_isCompute ? (a_words[0] & ~kVertexDescBit) : a_words[0];
			for (std::size_t i = 1; i < kWords; ++i)
				any |= a_words[i];
			return any != 0;
		}

		/**
		 * @brief What latched calls did not bind: the dirty bits their flushes consumed and the shaders they would have bound. Only
		 * the render owner flushes (one thread at a time, under Renderer::Lock when it is the loading screen), so no lock.
		 */
		struct Owed
		{
			std::array<std::uint32_t, kWords> words{};
			ID3D11VertexShader* vertexShader = nullptr;
			ID3D11PixelShader* pixelShader = nullptr;
			bool vertexOwed = false;
			bool pixelOwed = false;
		};

		Owed& GetOwed()
		{
			static Owed s_owed;
			return s_owed;
		}

		struct LatchCounters
		{
			std::atomic<std::uint64_t> latchedFlushes{ 0 };
			std::atomic<std::uint64_t> realFlushes{ 0 };
			std::atomic<std::uint64_t> allowedFlushes{ 0 };
			std::atomic<std::uint64_t> latchedBinds{ 0 };
			std::atomic<std::uint64_t> dropped{ 0 };
			std::atomic<std::uint64_t> skipped{ 0 };
			std::uint64_t frames = 0;
		};

		LatchCounters& GetLatch()
		{
			static LatchCounters s_latch;
			return s_latch;
		}

		/** @brief The allowed-draw reasons (Reason): their names, and per reason the draws let through and the passes vouched for. */
		constexpr std::size_t kReasons = 32;

		struct Reasons
		{
			std::mutex lock;  // interning only
			std::array<const char*, kReasons> names{};
			std::size_t count = 0;
			std::array<std::atomic<std::uint64_t>, kReasons> draws{};
			std::array<std::atomic<std::uint64_t>, kReasons> passes{};
		};

		Reasons& GetReasons()
		{
			static Reasons s_reasons;
			return s_reasons;
		}

		std::uint8_t Intern(const char* a_name)
		{
			auto& reasons = GetReasons();
			std::lock_guard guard(reasons.lock);
			for (std::size_t i = 0; i < reasons.count; ++i)
				if (reasons.names[i] == a_name || std::strcmp(reasons.names[i], a_name) == 0)
					return static_cast<std::uint8_t>(i);
			if (reasons.count == kReasons - 1)
				reasons.names[reasons.count++] = "more reasons";
			if (reasons.count == kReasons)
				return static_cast<std::uint8_t>(kReasons - 1);
			reasons.names[reasons.count] = a_name;
			return static_cast<std::uint8_t>(reasons.count++);
		}

		/**
		 * @brief The vouched passes (VouchPass), one entry per pass object. The engine's passes are 0x48-byte slots of one arena (65535
		 * of them), so the slot index modulo the table size keeps them apart; a pass from elsewhere can collide, and the pass and
		 * geometry checks then read it as not vouched for (skipped and logged, never let through by mistake). Writers store the pass
		 * last; the batch walk reads it on the render thread after the registration jobs.
		 */
		struct Mark
		{
			std::atomic<const void*> pass{ nullptr };
			std::atomic<const void*> geometry{ nullptr };
			std::atomic<std::uint32_t> stamp{ 0 };  // the frame (Tick's count, 24 bits) << 8 | the reason
		};
		constexpr std::size_t kMarks = 1u << 17;
		constexpr std::size_t kPassBytes = 0x48;  // sizeof(RE::BSRenderPass)

		std::unique_ptr<Mark[]>& GetMarks()
		{
			static std::unique_ptr<Mark[]> s_marks;
			return s_marks;
		}

		// Tick's frame count, for the marks' age: a mark stands for the frame it was made in and the next.
		std::atomic<std::uint32_t> s_markFrame{ 0 };

		Mark& MarkOf(const void* a_pass, Mark* a_marks)
		{
			return a_marks[(reinterpret_cast<std::uintptr_t>(a_pass) / kPassBytes) & (kMarks - 1)];
		}

		/** @brief The reason a_pass was vouched for, or -1. */
		int VouchedReason(const RE::BSRenderPass* a_pass)
		{
			Mark* marks = GetMarks().get();
			if (!marks || !a_pass)
				return -1;
			const Mark& mark = MarkOf(a_pass, marks);
			if (mark.pass.load(std::memory_order_acquire) != a_pass || mark.geometry.load(std::memory_order_relaxed) != a_pass->geometry)
				return -1;
			const std::uint32_t stamp = mark.stamp.load(std::memory_order_relaxed);
			const std::uint32_t age = ((s_markFrame.load(std::memory_order_relaxed) & 0xFFFFFFu) - (stamp >> 8)) & 0xFFFFFFu;
			return age <= 1 ? static_cast<int>(stamp & 0xFFu) : -1;
		}

		/**
		 * @brief A flush CS asked for, or one inside an EngineDraws scope: the owed bits back in the shadow state and the owed shaders
		 * bound, so it applies everything.
		 */
		void PayOwed(std::uint32_t* a_words)
		{
			auto& owed = GetOwed();
			for (std::size_t i = 0; i < kWords; ++i)
				a_words[i] |= owed.words[i];
			owed.words = {};
			if (auto* context = globals::d3d::context) {
				if (owed.vertexOwed)
					context->VSSetShader(Tripwire::Unwrap(owed.vertexShader), nullptr, 0);
				if (owed.pixelOwed)
					context->PSSetShader(Tripwire::Unwrap(owed.pixelShader), nullptr, 0);
			}
			owed.vertexOwed = false;
			owed.pixelOwed = false;
			auto& latch = GetLatch();
			(InEngineDraws() ? latch.allowedFlushes : latch.realFlushes).fetch_add(1, std::memory_order_relaxed);
		}

		// ---- The census -----------------------------------------------------------------------------------------------------

		/** @brief One class of engine draw: where it was issued from, the technique and the view. */
		struct DrawKey
		{
			void* site = nullptr;          // the return address in the draw function (DispatchCSShader for a dispatch)
			std::array<void*, 3> up{};     // the next game frames above it
			void* viaCs = nullptr;         // the first CS frame between them, when CS called the draw function (DCLF's native draws)
			const char* shader = nullptr;  // the last technique's file name, or the ImageSpace shader's name
			std::int32_t shaderType = -1;  // the last technique's BSShader::Type
			std::uint32_t vertex = 0;      // RSS.currentVertexShader->id
			std::uint32_t pixel = 0;       // RSS.currentPixelShader->id
			std::int32_t target = -1;      // RSS renderTargets[0]
			std::int32_t depth = -1;       // RSS depthStencil
			std::int32_t cube = -1;        // RSS cubeMapRenderTarget
			std::uint16_t slot = 0;        // the context method
			bool skipped = false;          // skipped by the latch
			bool operator==(const DrawKey&) const = default;
		};

		struct DrawKeyHash
		{
			std::size_t operator()(const DrawKey& a_key) const noexcept
			{
				std::size_t hash = std::hash<const void*>{}(a_key.site);
				const auto mix = [&](std::size_t a_value) { hash ^= a_value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2); };
				for (const void* frame : a_key.up)
					mix(std::hash<const void*>{}(frame));
				mix(std::hash<const void*>{}(a_key.viaCs));
				mix(std::hash<const void*>{}(a_key.shader));
				mix((static_cast<std::size_t>(a_key.vertex) << 32) | a_key.pixel);
				mix((static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.target)) << 32) | static_cast<std::uint32_t>(a_key.depth));
				mix((static_cast<std::size_t>(static_cast<std::uint32_t>(a_key.cube)) << 32) | (static_cast<std::size_t>(a_key.slot) << 8) |
					(static_cast<std::size_t>(static_cast<std::uint8_t>(a_key.shaderType)) << 1) | (a_key.skipped ? 1u : 0u));
				return hash;
			}
		};

		struct SiteCount
		{
			std::uint64_t calls = 0;
			std::uint64_t orphans = 0;  // followed by another flush or bind of the same kind, with no draw between
		};

		struct Window
		{
			std::unordered_map<DrawKey, std::uint64_t, DrawKeyHash> draws;
			std::unordered_map<void*, SiteCount> flushes;
			std::unordered_map<void*, SiteCount> vertexBinds;
			std::unordered_map<void*, SiteCount> pixelBinds;
			std::array<std::uint64_t, kSlots> perSlot{};
			std::uint64_t drawCount = 0;
			std::uint64_t skipped = 0;
			std::uint64_t flushCount = 0;
			std::uint64_t emptyFlushes = 0;
			std::uint64_t csFlushes = 0;
			std::uint64_t flushOrphans = 0;
			std::uint64_t vertexCount = 0;
			std::uint64_t vertexOrphans = 0;
			std::uint64_t pixelCount = 0;
			std::uint64_t pixelOrphans = 0;
			std::uint64_t frames = 0;
			std::uint64_t zeroFrames = 0;
		};

		struct Census
		{
			std::mutex lock;
			Window window;
			std::uint64_t frameDraws = 0;
			std::uint64_t totalDraws = 0;
			std::uint64_t totalFrames = 0;
			std::uint64_t totalZeroFrames = 0;
		};

		Census& GetCensus()
		{
			static Census s_census;
			return s_census;
		}

		/** @brief Per thread: the last non-empty flush and the last binds no draw has followed yet. */
		struct Pending
		{
			void* flush = nullptr;
			void* vertex = nullptr;
			void* pixel = nullptr;
		};
		thread_local Pending t_pending;

		/** @brief The game frames above the draw's site (and the first CS frame among them), from the stack. */
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
				// A tail jump into the context (DispatchCSShader): walk from the first frame outside CS.
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

		/** @brief The last technique and the bound shaders' descriptors. */
		void Technique(DrawKey& a_key)
		{
			if (const auto* state = globals::state; state && state->currentShader) {
				const auto* shader = state->currentShader;
				const auto type = shader->shaderType.get();
				a_key.shaderType = static_cast<std::int32_t>(type);
				a_key.shader = type == RE::BSShader::Type::ImageSpace ? static_cast<const RE::BSImagespaceShader*>(shader)->name : shader->fxpFilename;
			}
			if (globals::game::currentVertexShader && *globals::game::currentVertexShader)
				a_key.vertex = (*globals::game::currentVertexShader)->id;
			if (globals::game::currentPixelShader && *globals::game::currentPixelShader)
				a_key.pixel = (*globals::game::currentPixelShader)->id;
		}

		/** @brief The view from the shadow state: render target 0 (+0x18), the depth stencil (+0x38), the cube target (+0x40). */
		void View(DrawKey& a_key)
		{
			const auto* rss = reinterpret_cast<const std::uint8_t*>(globals::game::shadowState);
			if (!rss)
				return;
			a_key.target = *reinterpret_cast<const std::int32_t*>(rss + 0x18);
			a_key.depth = *reinterpret_cast<const std::int32_t*>(rss + 0x38);
			a_key.cube = *reinterpret_cast<const std::int32_t*>(rss + 0x40);
		}

		void NoteDraw(std::uint16_t a_slot, void* a_caller, bool a_skipped)
		{
			DrawKey key;
			key.slot = a_slot;
			key.skipped = a_skipped;
			key.site = t_dispatchScope > 0 ? DispatchFunction() : a_caller;
			Walk(a_caller, key);
			Technique(key);
			View(key);
			auto& census = GetCensus();
			std::lock_guard guard(census.lock);
			auto& window = census.window;
			++window.draws[key];
			++window.drawCount;
			++window.perSlot[a_slot];
			if (a_skipped)
				++window.skipped;
			++census.frameDraws;
			t_pending = {};
		}

		void NoteFlush(void* a_caller, bool a_nonEmpty, bool a_fromCs)
		{
			auto& census = GetCensus();
			std::lock_guard guard(census.lock);
			auto& window = census.window;
			++window.flushes[a_caller].calls;
			++window.flushCount;
			if (a_fromCs)
				++window.csFlushes;
			if (!a_nonEmpty) {
				++window.emptyFlushes;
				return;
			}
			if (t_pending.flush) {
				++window.flushes[t_pending.flush].orphans;
				++window.flushOrphans;
			}
			t_pending.flush = a_caller;
		}

		void NoteBind(bool a_vertex, void* a_caller)
		{
			auto& census = GetCensus();
			std::lock_guard guard(census.lock);
			auto& window = census.window;
			auto& sites = a_vertex ? window.vertexBinds : window.pixelBinds;
			void*& pending = a_vertex ? t_pending.vertex : t_pending.pixel;
			++sites[a_caller].calls;
			++(a_vertex ? window.vertexCount : window.pixelCount);
			if (pending) {
				++sites[pending].orphans;
				++(a_vertex ? window.vertexOrphans : window.pixelOrphans);
			}
			pending = a_caller;
		}

		// ---- The report -----------------------------------------------------------------------------------------------------

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

		std::string TargetName(std::int32_t a_index)
		{
			if (a_index < 0)
				return "none";
			const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGET>(a_index));
			return name.empty() ? std::to_string(a_index) : std::string(name);
		}

		std::string DepthName(std::int32_t a_index)
		{
			if (a_index < 0)
				return "none";
			const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(a_index));
			return name.empty() ? std::to_string(a_index) : std::string(name);
		}

		std::string ShaderTypeName(std::int32_t a_type)
		{
			if (a_type < 0)
				return "no technique";
			const auto name = magic_enum::enum_name(static_cast<RE::BSShader::Type>(a_type));
			return name.empty() ? std::to_string(a_type) : std::string(name);
		}

		std::string ViewName(const DrawKey& a_key)
		{
			std::string out = fmt::format("RT {} DS {}", TargetName(a_key.target), DepthName(a_key.depth));
			if (a_key.cube >= 0)
				out += fmt::format(" cube {}", a_key.cube);
			return out;
		}

		/** @brief "a 12.0, b 3.5": a_counts by key, the biggest first, at most a_limit. */
		std::string Breakdown(const std::map<std::string, std::uint64_t>& a_counts, double a_frames, std::size_t a_limit)
		{
			std::vector<std::pair<std::string, std::uint64_t>> sorted(a_counts.begin(), a_counts.end());
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			std::string out;
			for (std::size_t i = 0; i < sorted.size() && i < a_limit; ++i)
				out += fmt::format("{}{} {:.1f}", i ? ", " : "", sorted[i].first, static_cast<double>(sorted[i].second) / a_frames);
			if (sorted.size() > a_limit)
				out += fmt::format(", {} more", sorted.size() - a_limit);
			return out.empty() ? "none" : out;
		}

		void ReportSites(const char* a_what, const std::unordered_map<void*, SiteCount>& a_sites, double a_frames, std::size_t a_top)
		{
			std::vector<std::pair<void*, SiteCount>> sorted;
			for (const auto& [site, count] : a_sites)
				if (count.orphans)
					sorted.emplace_back(site, count);
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.orphans > b.second.orphans; });
			for (std::size_t i = 0; i < sorted.size() && i < a_top; ++i)
				logger::info("[ORG] residue census: {} with no draw #{}: {:.2f} a frame of {:.2f} at {}", a_what, i + 1,
					static_cast<double>(sorted[i].second.orphans) / a_frames, static_cast<double>(sorted[i].second.calls) / a_frames,
					Tripwire::DescribeCaller(sorted[i].first));
		}

		void Report(const Window& a_window, std::uint64_t a_totalDraws, std::uint64_t a_totalFrames, std::uint64_t a_totalZero)
		{
			const auto& config = GetConfig();
			const double frames = static_cast<double>(std::max<std::uint64_t>(a_window.frames, 1));
			std::map<std::string, std::uint64_t> bySlot, byFunction, byShader, byView;
			for (const auto& [key, count] : a_window.draws) {
				bySlot[config.names[key.slot] ? config.names[key.slot] : "?"] += count;
				byFunction[FunctionName(key.site)] += count;
				byShader[ShaderTypeName(key.shaderType)] += count;
				byView[ViewName(key)] += count;
			}
			logger::info("[ORG] residue census: {} frames, {} with no engine draw reaching D3D11: {:.1f} engine draws and dispatches a frame reach D3D11 ({}){}; since the start {} over {} frames, {} frames with none",
				a_window.frames, a_window.zeroFrames, static_cast<double>(a_window.drawCount) / frames, Breakdown(bySlot, frames, 8),
				a_window.skipped ? fmt::format(", {:.1f} of them skipped by the flush latch", static_cast<double>(a_window.skipped) / frames) : std::string(),
				a_totalDraws, a_totalFrames, a_totalZero);
			logger::info("[ORG] residue census: by draw function: {}", Breakdown(byFunction, frames, 16));
			logger::info("[ORG] residue census: by last technique: {}", Breakdown(byShader, frames, 16));
			logger::info("[ORG] residue census: by view: {}", Breakdown(byView, frames, 16));
			logger::info(
				"[ORG] residue census: flushes {:.1f} a frame ({:.1f} with nothing to apply, {:.1f} from CS code, {:.1f} followed by no draw); "
				"vertex shader binds {:.1f} ({:.1f} followed by no draw); pixel shader binds {:.1f} ({:.1f} followed by no draw)",
				static_cast<double>(a_window.flushCount) / frames, static_cast<double>(a_window.emptyFlushes) / frames,
				static_cast<double>(a_window.csFlushes) / frames, static_cast<double>(a_window.flushOrphans) / frames,
				static_cast<double>(a_window.vertexCount) / frames, static_cast<double>(a_window.vertexOrphans) / frames,
				static_cast<double>(a_window.pixelCount) / frames, static_cast<double>(a_window.pixelOrphans) / frames);

			std::vector<std::pair<const DrawKey*, std::uint64_t>> sorted;
			sorted.reserve(a_window.draws.size());
			for (const auto& [key, count] : a_window.draws)
				sorted.emplace_back(&key, count);
			const std::size_t top = std::min(config.top, sorted.size());
			std::partial_sort(sorted.begin(), sorted.begin() + top, sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			for (std::size_t i = 0; i < top; ++i) {
				const auto& key = *sorted[i].first;
				std::string chain;
				for (const void* frame : key.up)
					if (frame)
						chain += fmt::format(" <- {}", Tripwire::DescribeCaller(frame));
				if (key.viaCs)
					chain += fmt::format(" (via CS {})", Tripwire::DescribeCaller(key.viaCs));
				logger::info("[ORG] residue census: draw #{}: {:.2f} a frame, {}{} in {} at {}{}; {} \"{}\" vs {:#x} ps {:#x}; {}", i + 1,
					static_cast<double>(sorted[i].second) / frames, config.names[key.slot] ? config.names[key.slot] : "?",
					key.skipped ? " (skipped by the latch)" : "", FunctionName(key.site), Tripwire::DescribeCaller(key.site), chain,
					ShaderTypeName(key.shaderType), key.shader ? key.shader : "", key.vertex, key.pixel, ViewName(key));
			}
			if (sorted.size() > top)
				logger::info("[ORG] residue census: {} more draw classes", sorted.size() - top);
			ReportSites("flush", a_window.flushes, frames, config.top);
			ReportSites("vertex shader bind", a_window.vertexBinds, frames, config.top);
			ReportSites("pixel shader bind", a_window.pixelBinds, frames, config.top);
		}

		/** @brief Main thread, after each frame's jobs: closes the frame, and every CS_ORG_RESIDUE_CENSUS_FRAMES frames reports. */
		void Tick()
		{
			const auto& config = GetConfig();
			if (config.census) {
				auto& census = GetCensus();
				std::unique_lock guard(census.lock);
				auto& window = census.window;
				++window.frames;
				++census.totalFrames;
				if (census.frameDraws == 0) {
					++window.zeroFrames;
					++census.totalZeroFrames;
				}
				census.totalDraws += census.frameDraws;
				census.frameDraws = 0;
				if (window.frames >= config.frames) {
					const Window closed = std::exchange(census.window, Window{});
					const auto totalDraws = census.totalDraws;
					const auto totalFrames = census.totalFrames;
					const auto totalZero = census.totalZeroFrames;
					guard.unlock();
					Report(closed, totalDraws, totalFrames, totalZero);
				}
			}
			if (config.latch) {
				s_markFrame.fetch_add(1, std::memory_order_relaxed);
				auto& latch = GetLatch();
				if (++latch.frames >= config.frames) {
					const double frames = static_cast<double>(latch.frames);
					latch.frames = 0;
					auto& reasons = GetReasons();
					std::size_t count = 0;
					{
						std::lock_guard guard(reasons.lock);
						count = reasons.count;
					}
					std::string allowed;
					for (std::size_t i = 0; i < count; ++i) {
						const auto draws = reasons.draws[i].exchange(0, std::memory_order_relaxed);
						const auto passes = reasons.passes[i].exchange(0, std::memory_order_relaxed);
						if (draws || passes)
							allowed += fmt::format("{}{} {:.1f} ({:.1f} passes vouched)", allowed.empty() ? "" : ", ", reasons.names[i],
								static_cast<double>(draws) / frames, static_cast<double>(passes) / frames);
					}
					logger::info(
						"[ORG] flush latch: {:.0f} frames: {:.1f} flushes and {:.1f} shader binds a frame latched, {:.1f} D3D11 binds a frame dropped, "
						"{:.1f} flushes a frame from CS code applied the owed state, {:.1f} flushes a frame of allowed engine draws applied it; "
						"allowed engine draws a frame: {}; {} engine draws and dispatches skipped",
						frames, static_cast<double>(latch.latchedFlushes.exchange(0)) / frames, static_cast<double>(latch.latchedBinds.exchange(0)) / frames,
						static_cast<double>(latch.dropped.exchange(0)) / frames, static_cast<double>(latch.realFlushes.exchange(0)) / frames,
						static_cast<double>(latch.allowedFlushes.exchange(0)) / frames, allowed.empty() ? "none" : allowed, latch.skipped.exchange(0));
				}
			}
		}

		// ---- The latch ------------------------------------------------------------------------------------------------------

		/**
		 * @brief A draw or dispatch the latch skipped: counted, and logged once per site and caller chain (DrawTriShape alone serves
		 * the batch walk, the cloud pass list, ImageSpace and Bink's tiles).
		 */
		void NoteSkipped(std::uint16_t a_slot, void* a_caller)
		{
			GetLatch().skipped.fetch_add(1, std::memory_order_relaxed);
			void* const site = t_dispatchScope > 0 ? DispatchFunction() : a_caller;
			DrawKey key;
			Walk(a_caller, key);
			static std::mutex s_lock;
			static std::set<std::array<void*, 4>> s_seen;
			{
				std::lock_guard guard(s_lock);
				if (!s_seen.insert({ site, key.up[0], key.up[1], key.up[2] }).second)
					return;
			}
			std::string chain;
			for (const void* frame : key.up)
				if (frame)
					chain += fmt::format(" <- {}", Tripwire::DescribeCaller(frame));
			if (key.viaCs)
				chain += fmt::format(" (via CS {})", Tripwire::DescribeCaller(key.viaCs));
			const auto& config = GetConfig();
			logger::error(
				"[ORG] flush latch: engine {} in {} at {}{} reached D3D11 with the engine's state latched and nobody vouched for it "
				"(Flush::EngineDraws, Flush::VouchPass): skipped (first from these callers; CS_ORG_RESIDUE_CENSUS=1 counts them)",
				a_slot < kSlots && config.names[a_slot] ? config.names[a_slot] : "?", FunctionName(site), Tripwire::DescribeCaller(site), chain);
		}

		/** @brief The guest context's filter: the census's draw count, and the latch's dropped binds and skipped draws. */
		bool ContextFilter(std::uint16_t a_slot, void* a_caller)
		{
			const auto& config = GetConfig();
			const SlotClass type = a_slot < kSlots ? config.slots[a_slot] : SlotClass::Other;
			if (type == SlotClass::Other)
				return true;
			if (type == SlotClass::Setter) {
				if (t_latchScope == 0)
					return true;
				GetLatch().dropped.fetch_add(1, std::memory_order_relaxed);
				return false;
			}
			// A draw or a dispatch. Inside a latch scope (DispatchCSShader's dispatch) or from a draw function, its state was latched, unless
			// it is inside an EngineDraws scope: then its flush was real and the draw is the engine's on purpose.
			const bool engine = config.latch && (t_latchScope > 0 || (type == SlotClass::Dispatch && t_dispatchScope > 0) || InDrawFunction(a_caller));
			const bool skip = engine && !InEngineDraws();
			if (engine && !skip)
				GetReasons().draws[static_cast<std::size_t>(CurrentReason())].fetch_add(1, std::memory_order_relaxed);
			if (config.census)
				NoteDraw(a_slot, a_caller, skip);
			if (skip)
				NoteSkipped(a_slot, a_caller);
			return !skip;
		}

		void OweVertexShader(ID3D11VertexShader* a_shader)
		{
			auto& owed = GetOwed();
			owed.vertexShader = a_shader;
			owed.vertexOwed = true;
			GetLatch().latchedBinds.fetch_add(1, std::memory_order_relaxed);
		}

		void OwePixelShader(ID3D11PixelShader* a_shader)
		{
			auto& owed = GetOwed();
			owed.pixelShader = a_shader;
			owed.pixelOwed = true;
			GetLatch().latchedBinds.fetch_add(1, std::memory_order_relaxed);
		}

		// ---- The detours ----------------------------------------------------------------------------------------------------

		/**
		 * @brief BSGraphics::SetDirtyStates (75580 / 77386), outside Hooks.cpp's detour (installed after it), so _ReturnAddress is the
		 * real caller. Latched, the engine's code runs as before inside a latch scope; the words it cleared are owed. A flush CS calls
		 * itself is real and pays what is owed first.
		 */
		struct SetDirtyStates
		{
			static void thunk(bool a_isCompute)
			{
				void* const caller = _ReturnAddress();
				std::uint32_t* words = ShadowWords();
				const bool fromCs = Tripwire::IsOwnCode(caller);
				if (GetConfig().census && words)
					NoteFlush(caller, NonEmpty(words, a_isCompute), fromCs);
				if (!words || !Latched()) {
					func(a_isCompute);
					return;
				}
				if ((fromCs || InEngineDraws()) && t_latchScope == 0) {
					PayOwed(words);
					func(a_isCompute);
					return;
				}
				std::array<std::uint32_t, kWords> before{};
				std::copy_n(words, kWords, before.begin());
				{
					LatchScope scope;
					func(a_isCompute);
				}
				auto& owed = GetOwed();
				for (std::size_t i = 0; i < kWords; ++i)
					owed.words[i] |= before[i] & ~words[i];
				GetLatch().latchedFlushes.fetch_add(1, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Renderer::SetShader (75550 / 77351): `RSS.flags |= 0x400; RSS.currentVertexShader = vs; VSSetShader(vs->shader)`. */
		struct SetShader
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::VertexShader* a_shader)
			{
				if (GetConfig().census)
					NoteBind(true, _ReturnAddress());
				if (!Latched()) {
					func(a_renderer, a_shader);
					return;
				}
				if (InEngineDraws()) {
					func(a_renderer, a_shader);
					GetOwed().vertexOwed = false;
					return;
				}
				{
					LatchScope scope;
					func(a_renderer, a_shader);
				}
				OweVertexShader(a_shader ? reinterpret_cast<ID3D11VertexShader*>(a_shader->shader) : nullptr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Renderer::SetPixelShader (75555 / 77356): `RSS.currentPixelShader = ps; PSSetShader(ps ? ps->shader : null)`. */
		struct SetPixelShader
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::PixelShader* a_shader)
			{
				if (GetConfig().census)
					NoteBind(false, _ReturnAddress());
				if (!Latched()) {
					func(a_renderer, a_shader);
					return;
				}
				if (InEngineDraws()) {
					func(a_renderer, a_shader);
					GetOwed().pixelOwed = false;
					return;
				}
				{
					LatchScope scope;
					func(a_renderer, a_shader);
				}
				OwePixelShader(a_shader ? reinterpret_cast<ID3D11PixelShader*>(a_shader->shader) : nullptr);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Renderer::DispatchCSShader (75532 / 77329): `SetDirtyStates(true); CSSetShader(cs+0x60); CSSetConstantBuffers(0, 1,
		 * &computeCB); Dispatch` (the dispatch a tail jump). Installed before the other detours, so it wraps the engine's code only.
		 */
		struct DispatchCSShader
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer, RE::BSGraphics::ComputeShader* a_shader, std::uint32_t a_x, std::uint32_t a_y,
				std::uint32_t a_z)
			{
				DispatchScope dispatch;
				if (!Latched() || InEngineDraws()) {
					func(a_renderer, a_shader, a_x, a_y, a_z);
					return;
				}
				LatchScope scope;
				func(a_renderer, a_shader, a_x, a_y, a_z);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSBatchRenderer::RenderPassImmediately (100854 / 107644), the batch walk's draw of one pass (the geometry-type dispatcher
		 * and the skin path are inside it): a pass vouched for at registration (VouchPass) draws in an EngineDraws scope of its reason.
		 * Installed with the latch only.
		 */
		struct RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_alphaTest, std::uint32_t a_renderFlags)
			{
				const int reason = Latched() ? VouchedReason(a_pass) : -1;
				if (reason < 0) {
					func(a_pass, a_technique, a_alphaTest, a_renderFlags);
					return;
				}
				const int previous = std::exchange(t_engineReason, reason);
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				t_engineReason = previous;
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
			constexpr std::array<std::string_view, 10> kStages{ "IA", "VS", "PS", "GS", "HS", "DS", "CS", "OM", "RS", "SO" };
			if (a_name.size() > 5 && a_name.substr(2, 3) == "Set" && std::find(kStages.begin(), kStages.end(), a_name.substr(0, 2)) != kStages.end())
				return SlotClass::Setter;
			return SlotClass::Other;
		}
	}

	bool CensusRequested()
	{
		static const bool requested = Switches::On("CS_ORG_RESIDUE_CENSUS");
		return requested;
	}

	bool LatchRequested()
	{
		static const bool requested = Switches::On("CS_ORG_ENGINE_FLUSH_LATCH");
		return requested;
	}

	bool Latched()
	{
		const auto& config = GetConfig();
		return config.latch && config.installed && Tripwire::GuestContextInstalled();
	}

	void Install()
	{
		auto& config = GetConfig();
		config.census = CensusRequested();
		config.latch = LatchRequested();
		if (!config.census && !config.latch)
			return;
		// Native mode sees the engine's calls on the native context, whose calls the filter gets too.
		if (!Tripwire::GuestRequested() && !Native::Requested()) {
			logger::error(
				"[ORG] engine flush: CS_ORG_RESIDUE_CENSUS and CS_ORG_ENGINE_FLUSH_LATCH need CS_ORG_TRIPWIRE_GUEST=1 (the guest context sees "
				"the engine's D3D11 calls); both stay off");
			config.census = false;
			config.latch = false;
			return;
		}
		config.frames = std::max<std::uint64_t>(Switches::Number("CS_ORG_RESIDUE_CENSUS_FRAMES", 600), 1);
		config.top = static_cast<std::size_t>(Switches::Number("CS_ORG_RESIDUE_CENSUS_TOP", 25));
		const auto names = Tripwire::SlotNames(Tripwire::Kind::DeviceContext);
		for (std::size_t i = 0; i < names.size() && i < kSlots; ++i) {
			config.names[i] = names[i];
			config.slots[i] = ClassOf(names[i]);
		}
		// CS_ORG_ENGINE_FLUSH_LATCH_KEEP=PSSetShaderResources,OMSetRenderTargets: binds that still reach D3D11 inside the latched
		// functions, for CS readers that still ask D3D11 what the engine bound (flush.md "Readers of the D3D11 state").
		std::string kept;
		for (const auto& name : Switches::List("CS_ORG_ENGINE_FLUSH_LATCH_KEEP")) {
			bool found = false;
			for (std::size_t i = 0; i < names.size() && i < kSlots; ++i) {
				if (name == names[i] && config.slots[i] == SlotClass::Setter) {
					config.slots[i] = SlotClass::Other;
					found = true;
				}
			}
			if (found)
				kept += fmt::format("{}{}", kept.empty() ? "" : ", ", name);
			else
				logger::warn("[ORG] engine flush: CS_ORG_ENGINE_FLUSH_LATCH_KEEP names {}, which is not an ID3D11DeviceContext bind; ignored", name);
		}
		if (config.latch && !kept.empty())
			logger::info("[ORG] engine flush: the latch still forwards {} to D3D11 (CS_ORG_ENGINE_FLUSH_LATCH_KEEP)", kept);

		// SetDirtyStates after Hooks.cpp's detour (outside it); DispatchCSShader before ImageSpace's and Hooks.cpp's (inside them).
		stl::detour_thunk<SetDirtyStates>(REL::RelocationID(75580, 77386));
		stl::detour_thunk<SetShader>(REL::RelocationID(75550, 77351));
		stl::detour_thunk<SetPixelShader>(REL::RelocationID(75555, 77356));
		stl::detour_thunk<DispatchCSShader>(REL::RelocationID(75532, 77329));
		if (config.latch) {
			// Outside Hooks.cpp's prefetch detour. The vouched passes' table exists only with the latch.
			GetMarks() = std::make_unique<Mark[]>(kMarks);
			stl::detour_thunk<RenderPassImmediately>(REL::RelocationID(100854, 107644));
		}
		if (!SetDirtyStates::func.address() || !SetShader::func.address() || !SetPixelShader::func.address() || !DispatchCSShader::func.address() ||
			(config.latch && !RenderPassImmediately::func.address())) {
			logger::error(
				"[ORG] engine flush: a detour did not attach (SetDirtyStates {}, SetShader {}, SetPixelShader {}, DispatchCSShader {}, RenderPassImmediately {}); "
				"the latch stays off",
				SetDirtyStates::func.address() != 0, SetShader::func.address() != 0, SetPixelShader::func.address() != 0, DispatchCSShader::func.address() != 0,
				RenderPassImmediately::func.address() != 0);
			config.latch = false;
			GetMarks().reset();
		}
		Tripwire::SetContextFilter(&ContextFilter);
		MainTick::Add("Engine flush census", [] { Tick(); });
		config.installed = true;
		logger::info("[ORG] engine flush: {}{}{}, reports every {} frames", config.census ? "residue census on (CS_ORG_RESIDUE_CENSUS=1)" : "",
			config.census && config.latch ? ", " : "", config.latch ? "flush latch on (CS_ORG_ENGINE_FLUSH_LATCH=1): the engine's flushes and shader binds bind nothing" : "",
			config.frames);
	}

	void BindVertexShader(ID3D11VertexShader* a_shader, void* a_caller)
	{
		if (GetConfig().census)
			NoteBind(true, a_caller);
		const bool latched = Latched();
		if (latched && !InEngineDraws()) {
			OweVertexShader(a_shader);
			return;
		}
		if (latched)
			GetOwed().vertexOwed = false;
		globals::d3d::context->VSSetShader(a_shader, nullptr, 0);
	}

	void BindPixelShader(ID3D11PixelShader* a_shader, void* a_caller)
	{
		if (GetConfig().census)
			NoteBind(false, a_caller);
		const bool latched = Latched();
		if (latched && !InEngineDraws()) {
			OwePixelShader(a_shader);
			return;
		}
		if (latched)
			GetOwed().pixelOwed = false;
		globals::d3d::context->PSSetShader(a_shader, nullptr, 0);
	}

	Reason::Reason(const char* a_name) :
		id(Intern(a_name ? a_name : "unnamed"))
	{}

	EngineDraws::EngineDraws(const Reason& a_reason)
	{
		if (!Latched())
			return;
		previous = std::exchange(t_engineReason, static_cast<int>(a_reason.Id()));
		active = true;
	}

	EngineDraws::~EngineDraws()
	{
		if (active)
			t_engineReason = previous;
	}

	void BeginEngineRest(const Reason& a_reason)
	{
		t_restReason = static_cast<int>(a_reason.Id());
	}

	void EndEngineRest()
	{
		t_restReason = -1;
	}

	bool Vouching()
	{
		return GetMarks() != nullptr;
	}

	void VouchPass(const RE::BSRenderPass* a_pass, const Reason& a_reason)
	{
		Mark* marks = GetMarks().get();
		if (!marks || !a_pass)
			return;
		Mark& mark = MarkOf(a_pass, marks);
		mark.geometry.store(a_pass->geometry, std::memory_order_relaxed);
		mark.stamp.store(((s_markFrame.load(std::memory_order_relaxed) & 0xFFFFFFu) << 8) | a_reason.Id(), std::memory_order_relaxed);
		mark.pass.store(a_pass, std::memory_order_release);
		GetReasons().passes[a_reason.Id()].fetch_add(1, std::memory_order_relaxed);
	}
}
