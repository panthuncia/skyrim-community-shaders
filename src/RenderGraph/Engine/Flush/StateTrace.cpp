// The state readers' trace. DXVK keeps D3D11's hazard rules, so an SRV bind of a resource bound as a target binds null, and a target
// bind unbinds SRVs of the same resource. With the flush latch on, DXVK keeps stale targets, so CS binds of their views can be dropped.
// This trace logs those drops and which reader then read null, and checks flushes leave DXVK matching the shadow state.
#include "RenderGraph/Engine/Flush/StateTrace.h"

#include "Globals.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <intrin.h>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>

namespace RenderGraph::Engine::Flush
{
	namespace
	{
		constexpr std::uint32_t kInputSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;  // 128
		constexpr std::uint32_t kEngineSlots = 16;                                      // the shadow state's PSTexture[]
		constexpr std::uint32_t kTargets = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT;
		constexpr std::uint32_t kComputeUavs = 8;
		constexpr auto kTargetTotal = static_cast<std::uint32_t>(RE::RENDER_TARGET::kTOTAL);
		constexpr auto kDepthTotal = static_cast<std::uint32_t>(RE::RENDER_TARGET_DEPTHSTENCIL::kTOTAL);
		constexpr auto kCubeTotal = static_cast<std::uint32_t>(RE::RENDER_TARGET_CUBEMAP::kTOTAL);

		enum class Stage : std::uint8_t
		{
			Vertex,
			Pixel,
			Compute,
		};

		const char* StageName(Stage a_stage)
		{
			switch (a_stage) {
			case Stage::Vertex:
				return "VS";
			case Stage::Pixel:
				return "PS";
			default:
				return "CS";
			}
		}

		std::atomic<bool> s_installed{ false };

		struct Counters
		{
			std::atomic<std::uint64_t> droppedBinds{ 0 };     // SRV binds DXVK turned into null
			std::atomic<std::uint64_t> unbound{ 0 };          // PS SRVs a target bind unbound
			std::atomic<std::uint64_t> lostReads{ 0 };        // a reader read null where a bind was dropped or unbound
			std::atomic<std::uint64_t> appliedMismatch{ 0 };  // flushes that left a slot, target or viewport off the shadow state
			std::atomic<std::uint64_t> applied{ 0 };          // flushes checked
		};

		Counters& GetCounters()
		{
			static Counters s_counters;
			return s_counters;
		}

		/** @brief Per PS slot, why the last bind there did not hold. Render owner only, locked anyway. */
		struct Lost
		{
			bool lost = false;
			std::string how;
		};

		struct Table
		{
			std::mutex lock;
			std::array<Lost, kInputSlots> pixel{};
		};

		Table& GetTable()
		{
			static Table s_table;
			return s_table;
		}

		// Kind is 0-2 for a dropped bind by stage, 3 for an unbind, 4 for a flush off the shadow state, 5 for a lost read.
		using Key = std::tuple<int, std::uint32_t, const void*, const void*>;

		bool FirstTime(const Key& a_key)
		{
			static std::mutex s_lock;
			static std::set<Key> s_seen;
			std::lock_guard guard(s_lock);
			return s_seen.insert(a_key).second;
		}

		/** @brief Builds an event's text only the first time, since naming resources costs and a site repeats every frame. */
		template <class Build>
		std::string Described(const Key& a_key, Build&& a_build, bool& a_first)
		{
			static std::mutex s_lock;
			static std::map<Key, std::string> s_texts;
			a_first = false;
			{
				std::lock_guard guard(s_lock);
				if (const auto found = s_texts.find(a_key); found != s_texts.end())
					return found->second;
			}
			std::string text = a_build();
			std::lock_guard guard(s_lock);
			const auto [at, inserted] = s_texts.emplace(a_key, std::move(text));
			a_first = inserted;
			return at->second;
		}

		struct Callers
		{
			std::array<void*, 4> frames{};

			static Callers Capture(void* a_caller)
			{
				Callers callers;
				callers.frames[0] = a_caller;
				std::array<void*, 24> stack{};
				const std::size_t count = RtlCaptureStackBackTrace(0, static_cast<DWORD>(stack.size()), stack.data(), nullptr);
				std::size_t at = count;
				for (std::size_t i = 0; i < count; ++i) {
					if (stack[i] == a_caller) {
						at = i;
						break;
					}
				}
				for (std::size_t k = 1; k < callers.frames.size() && at + k < count; ++k)
					callers.frames[k] = stack[at + k];
				return callers;
			}

			std::string Describe() const
			{
				std::string out;
				for (const void* frame : frames)
					if (frame)
						out += fmt::format("{}{}", out.empty() ? "" : " <- ", Tripwire::DescribeCaller(frame));
				return out.empty() ? std::string("?") : out;
			}
		};

		template <class T>
		ID3D11Resource* Raw(T* a_object)
		{
			return a_object ? static_cast<ID3D11Resource*>(Tripwire::Unwrap(a_object)) : nullptr;
		}

		std::string EngineTargetName(ID3D11Resource* a_resource)
		{
			auto* renderer = globals::game::renderer;
			if (!renderer || !a_resource)
				return {};
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < kTargetTotal; ++i) {
				if (Raw(targets[i].texture) == a_resource || Raw(targets[i].textureCopy) == a_resource) {
					const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGET>(i));
					return fmt::format("engine target {}{}", name.empty() ? std::to_string(i) : std::string(name),
						Raw(targets[i].textureCopy) == a_resource ? " (copy)" : "");
				}
			}
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			for (std::uint32_t i = 0; i < kDepthTotal; ++i) {
				if (Raw(depths[i].texture) == a_resource) {
					const auto name = magic_enum::enum_name(static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(i));
					return fmt::format("engine depth {}", name.empty() ? std::to_string(i) : std::string(name));
				}
			}
			const auto& cubes = renderer->GetRendererData().cubemapRenderTargets;
			for (std::uint32_t i = 0; i < kCubeTotal; ++i)
				if (Raw(cubes[i].texture) == a_resource)
					return fmt::format("engine cube {}", i);
			return {};
		}

		std::string ResourceName(ID3D11Resource* a_resource)
		{
			if (!a_resource)
				return "null";
			if (auto name = EngineTargetName(a_resource); !name.empty())
				return name;
			char name[128]{};
			UINT size = sizeof(name) - 1;
			if (SUCCEEDED(a_resource->GetPrivateData(WKPDID_D3DDebugObjectName, &size, name)) && size)
				return fmt::format("'{}'", std::string_view(name, std::min<UINT>(size, sizeof(name) - 1)));
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			a_resource->GetType(&dimension);
			if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				D3D11_TEXTURE2D_DESC desc{};
				static_cast<ID3D11Texture2D*>(a_resource)->GetDesc(&desc);
				return fmt::format("{}x{} texture, format {}, {} layers, at {}", desc.Width, desc.Height, static_cast<std::uint32_t>(desc.Format), desc.ArraySize,
					fmt::ptr(a_resource));
			}
			if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				D3D11_BUFFER_DESC desc{};
				static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&desc);
				return fmt::format("buffer of {} bytes at {}", desc.ByteWidth, fmt::ptr(a_resource));
			}
			return fmt::format("resource at {}", fmt::ptr(a_resource));
		}

		template <class View>
		winrt::com_ptr<ID3D11Resource> ResourceOf(View* a_view)
		{
			winrt::com_ptr<ID3D11Resource> resource;
			if (a_view)
				a_view->GetResource(resource.put());
			return resource;
		}

		template <class View>
		std::string ViewName(View* a_view)
		{
			return a_view ? ResourceName(ResourceOf(a_view).get()) : std::string("null");
		}

		/** @brief Whether the shadow state's current targets hold a_resource, meaning the engine still draws into it. */
		bool ShadowHolds(ID3D11Resource* a_resource)
		{
			auto* renderer = globals::game::renderer;
			auto* shadow = globals::game::shadowState;
			if (!renderer || !shadow || !a_resource)
				return false;
			const auto& state = shadow->GetRuntimeData();
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < kTargets; ++i) {
				const auto index = static_cast<std::uint32_t>(state.renderTargets[i]);
				if (index < kTargetTotal && Raw(targets[index].texture) == a_resource)
					return true;
			}
			if (state.depthStencil < kDepthTotal &&
				Raw(renderer->GetDepthStencilData().depthStencils[state.depthStencil].texture) == a_resource)
				return true;
			return state.cubeMapRenderTarget < kCubeTotal &&
			       Raw(renderer->GetRendererData().cubemapRenderTargets[state.cubeMapRenderTarget].texture) == a_resource;
		}

		/** @brief What a_resource is bound as on DXVK that made it drop an SRV bind, and whether that is stale. */
		std::string ConflictOf(ID3D11DeviceContext* a_context, Stage a_stage, ID3D11Resource* a_resource)
		{
			std::string out;
			const auto note = [&](const char* a_what, std::uint32_t a_slot, ID3D11Resource* a_bound) {
				if (a_bound != a_resource)
					return;
				out += fmt::format("{}{} {}", out.empty() ? "" : ", ", a_what, a_slot);
			};
			if (a_stage == Stage::Compute) {
				std::array<ID3D11UnorderedAccessView*, kComputeUavs> uavs{};
				a_context->CSGetUnorderedAccessViews(0, kComputeUavs, uavs.data());
				for (std::uint32_t i = 0; i < kComputeUavs; ++i) {
					note("bound as CS UAV", i, ResourceOf(uavs[i]).get());
					if (uavs[i])
						uavs[i]->Release();
				}
			} else {
				std::array<ID3D11RenderTargetView*, kTargets> rtvs{};
				ID3D11DepthStencilView* dsv = nullptr;
				a_context->OMGetRenderTargets(kTargets, rtvs.data(), &dsv);
				for (std::uint32_t i = 0; i < kTargets; ++i) {
					note("bound as render target", i, ResourceOf(rtvs[i]).get());
					if (rtvs[i])
						rtvs[i]->Release();
				}
				note("bound as depth stencil", 0, ResourceOf(dsv).get());
				if (dsv)
					dsv->Release();
			}
			if (out.empty())
				return "bound as an output DXVK did not report (an OM UAV?)";
			return out + (ShadowHolds(a_resource) ? "; the engine's shadow state still targets it" :
			                                         "; the engine's shadow state has moved off it (a stale bind: the latch dropped the engine's target bind)");
		}

		std::string ShadowTargets()
		{
			auto* shadow = globals::game::shadowState;
			if (!shadow)
				return "?";
			const auto& state = shadow->GetRuntimeData();
			const auto name = [](auto a_enum, std::uint32_t a_index, std::uint32_t a_total) {
				if (a_index >= a_total)
					return std::string("none");
				const auto text = magic_enum::enum_name(static_cast<decltype(a_enum)>(a_index));
				return text.empty() ? std::to_string(a_index) : std::string(text);
			};
			return fmt::format("RT {} DS {} cube {}", name(RE::RENDER_TARGET{}, static_cast<std::uint32_t>(state.renderTargets[0]), kTargetTotal),
				name(RE::RENDER_TARGET_DEPTHSTENCIL{}, state.depthStencil, kDepthTotal), name(RE::RENDER_TARGET_CUBEMAP{}, state.cubeMapRenderTarget, kCubeTotal));
		}

		void AfterBind(ID3D11DeviceContext* a_context, Stage a_stage, UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_views, void* a_caller)
		{
			if (!a_views)
				return;
			const UINT end = std::min<UINT>(a_start + a_count, kInputSlots);
			for (UINT slot = a_start; slot < end; ++slot) {
				auto* wanted = a_views[slot - a_start];
				ID3D11ShaderResourceView* got = nullptr;
				if (wanted) {
					switch (a_stage) {
					case Stage::Vertex:
						a_context->VSGetShaderResources(slot, 1, &got);
						break;
					case Stage::Pixel:
						a_context->PSGetShaderResources(slot, 1, &got);
						break;
					default:
						a_context->CSGetShaderResources(slot, 1, &got);
						break;
					}
					if (got)
						got->Release();  // identity only
				}
				const bool dropped = wanted && got != wanted;
				if (a_stage == Stage::Pixel) {
					auto& table = GetTable();
					std::lock_guard guard(table.lock);
					table.pixel[slot] = {};
				}
				if (!dropped)
					continue;
				GetCounters().droppedBinds.fetch_add(1, std::memory_order_relaxed);
				const auto callers = Callers::Capture(a_caller);
				bool first = false;
				const std::string text = Described(
					{ static_cast<int>(a_stage), slot, callers.frames[0], callers.frames[1] },
					[&] {
						const auto resource = ResourceOf(wanted);
						return fmt::format("{} t{} bind of {} (the view's resource is {}) at {}", StageName(a_stage), slot, ResourceName(resource.get()),
							ConflictOf(a_context, a_stage, resource.get()), callers.Describe());
					},
					first);
				if (a_stage == Stage::Pixel) {
					auto& table = GetTable();
					std::lock_guard guard(table.lock);
					table.pixel[slot] = { true, "DXVK dropped its " + text };
				}
				if (first)
					logger::warn("[ORG] state readers: DXVK dropped a {}; latch {}, the engine's shadow state: {}", text, Latched() ? "on" : "off", ShadowTargets());
			}
		}

		/** @brief Counts the PS views a target bind unbound. Releases a_before's references. */
		void AfterTargets(ID3D11DeviceContext* a_context, std::array<ID3D11ShaderResourceView*, kInputSlots>& a_before, void* a_caller)
		{
			std::array<ID3D11ShaderResourceView*, kInputSlots> after{};
			a_context->PSGetShaderResources(0, kInputSlots, after.data());
			std::optional<Callers> callers;
			for (std::uint32_t slot = 0; slot < kInputSlots; ++slot) {
				if (a_before[slot] && !after[slot]) {
					GetCounters().unbound.fetch_add(1, std::memory_order_relaxed);
					if (!callers)
						callers = Callers::Capture(a_caller);
					bool first = false;
					const std::string text = Described(
						{ 3, slot, callers->frames[0], callers->frames[1] },
						[&] { return fmt::format("PS t{} ({}) was unbound by a target bind at {}", slot, ViewName(a_before[slot]), callers->Describe()); }, first);
					{
						auto& table = GetTable();
						std::lock_guard guard(table.lock);
						table.pixel[slot] = { true, text };
					}
					if (first)
						logger::info("[ORG] state readers: {}; latch {}, the engine's shadow state: {}", text, Latched() ? "on" : "off", ShadowTargets());
				}
				if (a_before[slot])
					a_before[slot]->Release();
				if (after[slot])
					after[slot]->Release();
			}
		}

		template <Stage S>
		struct SetShaderResources
		{
			static void thunk(ID3D11DeviceContext* a_context, UINT a_start, UINT a_count, ID3D11ShaderResourceView* const* a_views)
			{
				func(a_context, a_start, a_count, a_views);
				AfterBind(a_context, S, a_start, a_count, a_views, _ReturnAddress());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct OMSetRenderTargets
		{
			static void thunk(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth)
			{
				std::array<ID3D11ShaderResourceView*, kInputSlots> before{};
				a_context->PSGetShaderResources(0, kInputSlots, before.data());
				func(a_context, a_count, a_views, a_depth);
				AfterTargets(a_context, before, _ReturnAddress());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct OMSetRenderTargetsAndUnorderedAccessViews
		{
			static void thunk(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth, UINT a_uavStart,
				UINT a_uavCount, ID3D11UnorderedAccessView* const* a_uavs, const UINT* a_counts)
			{
				std::array<ID3D11ShaderResourceView*, kInputSlots> before{};
				a_context->PSGetShaderResources(0, kInputSlots, before.data());
				func(a_context, a_count, a_views, a_depth, a_uavStart, a_uavCount, a_uavs, a_counts);
				AfterTargets(a_context, before, _ReturnAddress());
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace StateTrace
	{
		bool On()
		{
			static const bool on = CensusRequested() && Tripwire::GuestRequested();
			return on;
		}

		void CheckApplied(void* a_caller)
		{
			if (!s_installed.load(std::memory_order_relaxed))
				return;
			auto* context = globals::d3d::context;
			auto* renderer = globals::game::renderer;
			auto* shadow = globals::game::shadowState;
			if (!context || !renderer || !shadow)
				return;
			auto& counters = GetCounters();
			counters.applied.fetch_add(1, std::memory_order_relaxed);
			const auto& state = shadow->GetRuntimeData();
			const auto mismatch = [&](std::uint32_t a_key, auto&& a_what) {
				counters.appliedMismatch.fetch_add(1, std::memory_order_relaxed);
				// Slot 16 is the viewport and 17 is render target 0.
				if (FirstTime({ 4, a_key, a_caller, nullptr }))
					logger::warn("[ORG] state readers: after a flush from {} (latch {}): {}", Tripwire::DescribeCaller(a_caller), Latched() ? "on" : "off", a_what());
			};

			std::array<ID3D11ShaderResourceView*, kEngineSlots> bound{};
			context->PSGetShaderResources(0, kEngineSlots, bound.data());
			for (std::uint32_t slot = 0; slot < kEngineSlots; ++slot) {
				auto* expected = Tripwire::Unwrap(state.PSTexture[slot]);
				// Dirty again means something rebound the slot on purpose after the flush, such as VanillaBinding's reflections at t4.
				const bool dirty = (state.PSResourceModifiedBits >> slot) & 1;
				if (!dirty && bound[slot] != expected)
					mismatch(slot, [&] { return fmt::format("PS t{} on DXVK is {}, the shadow state holds {}", slot, ViewName(bound[slot]), ViewName(expected)); });
				if (bound[slot])
					bound[slot]->Release();
			}

			const auto flags = static_cast<std::uint32_t>(state.stateUpdateFlags.underlying());
			if (!(flags & RE::BSGraphics::DIRTY_VIEWPORT)) {
				D3D11_VIEWPORT viewport{};
				UINT count = 1;
				context->RSGetViewports(&count, &viewport);
				const auto& want = state.viewPort;
				if (count != 1 || viewport.TopLeftX != want.TopLeftX || viewport.TopLeftY != want.TopLeftY || viewport.Width != want.Width ||
					viewport.Height != want.Height || viewport.MinDepth != want.MinDepth || viewport.MaxDepth != want.MaxDepth)
					mismatch(16, [&] {
						return fmt::format("the viewport on DXVK is {}x{} at ({}, {}) depth {} to {}, the shadow state's {}x{} at ({}, {}) depth {} to {}", viewport.Width,
							viewport.Height, viewport.TopLeftX, viewport.TopLeftY, viewport.MinDepth, viewport.MaxDepth, want.Width, want.Height, want.TopLeftX, want.TopLeftY,
							want.MinDepth, want.MaxDepth);
					});
			}

			// Skip cube mode, where SetDirtyStates binds the face alone, and targets that are dirty again.
			const auto target = static_cast<std::uint32_t>(state.renderTargets[0]);
			if (!(flags & RE::BSGraphics::DIRTY_RENDERTARGET) && state.cubeMapRenderTarget >= kCubeTotal && target < kTargetTotal) {
				ID3D11RenderTargetView* rtv = nullptr;
				context->OMGetRenderTargets(1, &rtv, nullptr);
				auto* expected = Tripwire::Unwrap(renderer->GetRuntimeData().renderTargets[target].RTV);
				if (rtv != expected)
					mismatch(17, [&] { return fmt::format("render target 0 on DXVK is {}, the shadow state's is {}", ViewName(rtv), ViewName(expected)); });
				if (rtv)
					rtv->Release();
			}
		}

		void Report(double a_frames)
		{
			if (!s_installed.load(std::memory_order_relaxed) || a_frames <= 0.0)
				return;
			auto& counters = GetCounters();
			logger::info(
				"[ORG] state readers: {:.0f} frames: {:.1f} shader resource binds a frame dropped by DXVK (the view's resource bound as an output), {:.1f} PS views a frame "
				"unbound by target binds, {:.1f} reads a frame found such a view null; {:.1f} flushes a frame bound their state, {:.1f} of their slots a frame differ "
				"from the shadow state (latch {})",
				a_frames, static_cast<double>(counters.droppedBinds.exchange(0)) / a_frames, static_cast<double>(counters.unbound.exchange(0)) / a_frames,
				static_cast<double>(counters.lostReads.exchange(0)) / a_frames, static_cast<double>(counters.applied.exchange(0)) / a_frames,
				static_cast<double>(counters.appliedMismatch.exchange(0)) / a_frames, Latched() ? "on" : "off");
		}
	}

	void InstallStateTrace(ID3D11DeviceContext* a_context)
	{
		if (!a_context || !StateTrace::On() || s_installed.exchange(true))
			return;
		stl::detour_vfunc<8, SetShaderResources<Stage::Pixel>>(a_context);
		stl::detour_vfunc<25, SetShaderResources<Stage::Vertex>>(a_context);
		stl::detour_vfunc<67, SetShaderResources<Stage::Compute>>(a_context);
		stl::detour_vfunc<33, OMSetRenderTargets>(a_context);
		stl::detour_vfunc<34, OMSetRenderTargetsAndUnorderedAccessViews>(a_context);
		logger::info(
			"[ORG] state readers: tracing DXVK's dropped shader resource binds, the PS views target binds unbind, the readers that read them, and flushes that leave "
			"DXVK off the shadow state (CS_ORG_RESIDUE_CENSUS=1)");
	}

	void NoteRead(const char* a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view)
	{
		if (a_view || a_register >= kInputSlots || !s_installed.load(std::memory_order_relaxed))
			return;
		std::string how;
		{
			auto& table = GetTable();
			std::lock_guard guard(table.lock);
			if (!table.pixel[a_register].lost)
				return;
			how = table.pixel[a_register].how;
		}
		GetCounters().lostReads.fetch_add(1, std::memory_order_relaxed);
		if (FirstTime({ 5, a_register, a_reader, nullptr }))
			logger::warn("[ORG] state readers: {} read PS t{} as null: {}; latch {}", a_reader ? a_reader : "?", a_register, how, Latched() ? "on" : "off");
	}
}
