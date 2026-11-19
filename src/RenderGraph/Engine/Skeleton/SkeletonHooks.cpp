// The engine call sites of the frame skeleton. Each patch replaces a vtable call on the immediate context. The thunk either
// hands the work to the skeleton or makes the same D3D11 call.

#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#include "Globals.h"

#include <array>
#include <cstring>
#include <tuple>
#include <type_traits>

namespace RenderGraph::Engine::Skeleton
{
#if defined(CS_HAS_RENDER_GRAPH)
	namespace
	{
		// Inside ApplyState and SetDirtyStates the clears collect, and the bind that follows runs the graph first.
		void ClearTargetSite(ID3D11DeviceContext* a_context, ID3D11RenderTargetView* a_view, const FLOAT a_colour[4])
		{
			if (!Sites::TakeColourClear(a_context, a_view, a_colour))
				a_context->ClearRenderTargetView(a_view, a_colour);
		}

		void ClearDepthSite(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			if (!Sites::TakeDepthClear(a_context, a_view, a_flags, a_depth, a_stencil))
				a_context->ClearDepthStencilView(a_view, a_flags, a_depth, a_stencil);
		}

		void BindTargetsSite(ID3D11DeviceContext* a_context, UINT a_count, ID3D11RenderTargetView* const* a_views, ID3D11DepthStencilView* a_depth)
		{
			Sites::Flush();
			a_context->OMSetRenderTargets(a_count, a_views, a_depth);
		}

		// Renderer::ClearDepthStencil (75466 / 77252). An immediate clear with no bind after it.
		void ClearDepthNowSite(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_view, UINT a_flags, FLOAT a_depth, UINT8 a_stencil)
		{
			if (Sites::TakeDepthClear(a_context, a_view, a_flags, a_depth, a_stencil))
				Sites::Flush();
			else
				a_context->ClearDepthStencilView(a_view, a_flags, a_depth, a_stencil);
		}

		void CopyDepthSite(ID3D11DeviceContext* a_context, ID3D11Resource* a_destination, ID3D11Resource* a_source)
		{
			if (!Sites::TakeDepthCopy(a_destination, a_source))
				a_context->CopyResource(a_destination, a_source);
		}

		/** @brief Renderer::ClearColor (75465 / 77251). Its vtable call is a tail jump, so the whole function is detoured. */
		struct Renderer_ClearColor
		{
			static void thunk(RE::BSGraphics::Renderer* a_renderer)
			{
				if (a_renderer && globals::game::shadowState) {
					// As the original reads it (AE 0x140e4fcf0). kNONE is left to it.
					const auto index = static_cast<std::uint32_t>(globals::game::shadowState->GetRuntimeData().renderTargets[0]);
					if (index < RE::RENDER_TARGET::kTOTAL) {
						auto* view = a_renderer->GetRuntimeData().renderTargets[index].RTV;
						auto* context = reinterpret_cast<ID3D11DeviceContext*>(a_renderer->GetRuntimeData().context);
						if (view && context && Sites::TakeColourClear(context, view, a_renderer->GetRendererData().clearColor)) {
							Sites::Flush();
							return;
						}
					}
				}
				func(a_renderer);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Site
		{
			const char* name;
			REL::RelocationID function;
			std::size_t seOffset, aeOffset;
			std::array<std::uint8_t, 7> bytes;
			std::size_t length;  // 7 has a REX prefix, and the seventh byte becomes a NOP
			void* thunk;
		};

		constexpr std::array<std::uint8_t, 7> kClearTarget{ 0xFF, 0x90, 0x90, 0x01, 0x00, 0x00 };         // call [rax+0x190] ClearRenderTargetView
		constexpr std::array<std::uint8_t, 7> kClearDepth{ 0xFF, 0x90, 0xA8, 0x01, 0x00, 0x00 };          // call [rax+0x1a8] ClearDepthStencilView
		constexpr std::array<std::uint8_t, 7> kBindTargets{ 0xFF, 0x90, 0x08, 0x01, 0x00, 0x00 };         // call [rax+0x108] OMSetRenderTargets
		constexpr std::array<std::uint8_t, 7> kCopy{ 0x41, 0xFF, 0x91, 0x78, 0x01, 0x00, 0x00 };          // call [r9+0x178] CopyResource
		constexpr std::array<std::uint8_t, 7> kClearDepthR9{ 0x41, 0xFF, 0x91, 0xA8, 0x01, 0x00, 0x00 };  // call [r9+0x1a8] ClearDepthStencilView

		const std::array<Site, 11>& PatchSites()
		{
			static const std::array<Site, 11> sites{ {
				// Renderer::ApplyState (SE 0x140d6a330, AE 0x140e44850).
				{ "ApplyState cube clear", REL::RelocationID(75462, 77247), 0x85, 0x85, kClearTarget, 6, reinterpret_cast<void*>(&ClearTargetSite) },
				{ "ApplyState target clear", REL::RelocationID(75462, 77247), 0xE1, 0xE1, kClearTarget, 6, reinterpret_cast<void*>(&ClearTargetSite) },
				{ "ApplyState depth clear", REL::RelocationID(75462, 77247), 0x184, 0x184, kClearDepth, 6, reinterpret_cast<void*>(&ClearDepthSite) },
				{ "ApplyState bind", REL::RelocationID(75462, 77247), 0x1A8, 0x1A8, kBindTargets, 6, reinterpret_cast<void*>(&BindTargetsSite) },
				// BSGraphics::SetDirtyStates (SE 0x140d705b0, AE 0x140e4b5b0).
				{ "SetDirtyStates cube clear", REL::RelocationID(75580, 77386), 0x8F, 0x8F, kClearTarget, 6, reinterpret_cast<void*>(&ClearTargetSite) },
				{ "SetDirtyStates target clear", REL::RelocationID(75580, 77386), 0xF1, 0xF1, kClearTarget, 6, reinterpret_cast<void*>(&ClearTargetSite) },
				{ "SetDirtyStates depth clear", REL::RelocationID(75580, 77386), 0x1B5, 0x1BC, kClearDepth, 6, reinterpret_cast<void*>(&ClearDepthSite) },
				{ "SetDirtyStates bind", REL::RelocationID(75580, 77386), 0x1D9, 0x1E0, kBindTargets, 6, reinterpret_cast<void*>(&BindTargetsSite) },
				// Renderer::ClearDepthStencil (SE 0x140d6a810, AE 0x140e44d30).
				{ "ClearDepthStencil", REL::RelocationID(75466, 77252), 0x48, 0x48, kClearDepthR9, 7, reinterpret_cast<void*>(&ClearDepthNowSite) },
				// Main::RenderDepth's end (SE 0x1412e3a43, AE 0x1414cd56f) and FinishAccumulating's copy (SE 0x1412cb07e, AE 0x1414b312d).
				{ "RenderDepth copy", REL::RelocationID(100421, 107139), 0x523, 0xA3F, kCopy, 7, reinterpret_cast<void*>(&CopyDepthSite) },
				{ "FinishAccumulating copy", REL::RelocationID(99938, 106583), 0x3AE, 0x39D, kCopy, 7, reinterpret_cast<void*>(&CopyDepthSite) },
			} };
			return sites;
		}

		std::uintptr_t AddressOf(const Site& a_site) { return a_site.function.address() + (REL::Module::IsAE() ? a_site.aeOffset : a_site.seOffset); }

		/** @brief False if another plugin patched the site. */
		bool Matches(const Site& a_site)
		{
			return std::memcmp(reinterpret_cast<const void*>(AddressOf(a_site)), a_site.bytes.data(), a_site.length) == 0;
		}

		void Patch(const Site& a_site)
		{
			const auto address = AddressOf(a_site);
			// write_call writes six bytes and would refuse a seven-byte call at the boundary check. The bytes were already checked, so skip it and NOP the seventh.
			SKSE::GetTrampoline().write_call<6>(address, reinterpret_cast<std::uintptr_t>(a_site.thunk), a_site.length != 6);
			if (a_site.length == 7) {
				constexpr std::uint8_t nop = 0x90;
				REL::safe_write(address + 6, &nop, 1);
			}
		}
	}

	void Install()
	{
		if (!Requested())
			return;
		// A function's clears run at its bind, so its sites are patched all together or not at all.
		// One site patched by another plugin leaves the whole function to the engine.
		const auto& sites = PatchSites();
		std::array<bool, std::tuple_size_v<std::remove_cvref_t<decltype(sites)>>> matches{};
		for (std::size_t i = 0; i < sites.size(); ++i)
			matches[i] = Matches(sites[i]);
		std::uint32_t patched = 0;
		for (const auto& site : sites) {
			bool whole = true;
			for (std::size_t i = 0; i < sites.size(); ++i)
				whole = whole && (sites[i].function.address() != site.function.address() || matches[i]);
			if (!whole) {
				logger::warn("[ORG] engine skeleton: {} at 0x{:X} is left to the engine: a call in its function is not the expected one (another plugin may patch it)",
					site.name, AddressOf(site));
				continue;
			}
			Patch(site);
			++patched;
		}
		stl::detour_thunk<Renderer_ClearColor>(REL::RelocationID(75465, 77251));
		MarkInstalled();
		logger::info("[ORG] engine skeleton: {} of {} engine call sites patched, Renderer::ClearColor {}", patched, sites.size(),
			Renderer_ClearColor::func.address() ? "detoured" : "not detoured");
	}
#else
	void Install()
	{
		if (Requested())
			logger::warn("[ORG] engine skeleton: CS_ORG_ENGINE_SKELETON=1, but this build has no render graph");
	}
#endif
}
