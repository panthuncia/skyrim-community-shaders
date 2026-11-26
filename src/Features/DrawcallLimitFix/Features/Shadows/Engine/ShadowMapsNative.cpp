// Native shadow maps: a shadow light whose views DCLF draws whole skips its Render. The engine's Render would draw nothing into the
// maps then, but it still does bookkeeping the masks and features read later. This file does that bookkeeping instead, on the render
// thread inside Main::RenderShadowMaps: the descriptors' targets and slices, the slices' clears, the light transforms, the scene
// accumulate array's release and the globals SubmitAccumulator leaves set. It captures each view for the shadow epoch itself.
// Ports of BSShadowDirectionalLight::Render (AE 0x141511d60, SE 0x141324c30), BSShadowFrustumLight::Render (AE 0x14151aac0,
// SE 0x14132d040), BSShadowParabolicLight::Render (AE 0x14151bd10, SE 0x14132e4e0) and BSShadowLight::RenderShadowmap (AE 0x1414f0cf0,
// SE 0x141305610).

#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/ShadowEngineAddresses.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowFrame.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "RenderGraph/Engine/Skeleton/SkeletonSites.h"

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace DCLF::Features::Shadows::Engine
{
	namespace
	{
		using Descriptor = RE::BSShadowLight::ShadowmapDescriptor;
		using DepthTarget = RE::RENDER_TARGETS_DEPTHSTENCIL;

		// Render thread: inside Main::RenderShadowMaps, from DCLF's BeforeShadowMaps to its AfterShadowMaps.
		bool shadowMapsOpen = false;

		/** @brief Why a shadow light's Render went to the engine. Counted for the log line below. */
		enum class Fallback : std::uint8_t
		{
			None,
			NotOpen,        // outside Main::RenderShadowMaps
			NoDraws,        // the light lists no draw
			NotDclf,        // the frame's shadow views are not DCLF's
			NoAccumulator,  // a draw without an accumulator, camera or batch renderer
			NotCovered,     // a view DCLF does not cover
			Kept,           // a caster DCLF does not hold kept its passes for the engine
			Capture,        // a view could not be captured natively
			Count,
		};
		constexpr std::array<const char*, static_cast<std::size_t>(Fallback::Count)> kFallbackNames{ "native", "outside RenderShadowMaps", "no draws",
			"views not DCLF's", "no accumulator", "view not covered", "kept passes", "capture failed" };
		// Render thread. Lights per outcome since the last line, and shadow map frames.
		std::array<std::uint64_t, static_cast<std::size_t>(Fallback::Count)> lightCounts{};
		std::uint64_t shadowFrames = 0;
		constexpr std::uint64_t kLineFrames = 600;

		enum class Kind : std::uint8_t
		{
			Directional,
			Frustum,
			Parabolic,
		};

		/** @brief One RenderShadowmap call of a light's Render, in its order. */
		struct Draw
		{
			Descriptor* descriptor = nullptr;
			bool copy = false;    // the sun's volumetric lighting copy (flags 0x100), which does not advance the light index
			bool counts = false;  // advances the light index RenderShadowMaps walks the light array with
			std::uint32_t view = ~0u;
			std::uint32_t mode = 0;
			std::array<float, 2> parabola{};  // a paraboloid's radius and side, as its Render sets the globals
		};

		template <class T>
		T& Global(const DCLF::Engine::Offset& a_offset)
		{
			return DCLF::Engine::Global<T>(a_offset);
		}

		std::uint32_t FocusCount() { return Global<std::uint32_t>(DCLF::Engine::Address::kFocusTargetCount); }

		/** @brief The light's RenderShadowmap calls, in its Render's order, without its writes. */
		void ListDraws(RE::BSShadowLight& a_light, Kind a_kind, std::vector<Draw>& a_draws)
		{
			auto& data = a_light.GetRuntimeData();
			auto* descriptors = data.shadowmapDescriptors.data();
			const std::uint32_t count = a_light.shadowMapCount;
			const float radius = a_light.light ? a_light.light->GetLightRuntimeData().radius.x : 0.0f;
			switch (a_kind) {
			case Kind::Directional:
				if (Global<std::uint32_t>(Address::kVolumetricCopy) == 2)
					for (std::uint32_t d = 0; d < count; ++d)
						a_draws.push_back({ .descriptor = descriptors + d, .copy = true });
				for (std::uint32_t d = 0; d < count; ++d)
					a_draws.push_back({ .descriptor = descriptors + d, .counts = true });
				if (data.drawFocusShadows)
					for (std::uint32_t i = 0; i < FocusCount() && i < std::size(data.focusShadowmapDescriptors); ++i)
						a_draws.push_back({ .descriptor = data.focusShadowmapDescriptors + i });
				break;
			case Kind::Frustum:
				a_draws.push_back({ .descriptor = descriptors, .counts = true });
				if (data.drawFocusShadows)
					for (std::uint32_t i = 0; i < FocusCount() && i < std::size(data.focusShadowmapDescriptors); ++i)
						if (data.focusShadowmapDescriptors[i].shaderAccumulator)
							a_draws.push_back({ .descriptor = data.focusShadowmapDescriptors + i });
				break;
			case Kind::Parabolic:
				for (std::uint32_t d = 0; d < count; ++d)
					a_draws.push_back({ .descriptor = descriptors + d, .counts = true, .parabola = { radius, d == 1 ? -1.0f : 1.0f } });
				break;
			}
		}

		/** @brief Whether a batch renderer holds a geometry group pass, which the engine would draw whatever registered this frame. */
		bool GroupsEmpty(const RE::BSBatchRenderer& a_batch)
		{
			for (const auto* group : a_batch.geometryGroups)
				if (group && group->passList.head)
					return false;
			return !a_batch.alphaGroup || !a_batch.alphaGroup->passList.head;
		}

		/** @brief Fills each draw's view and mode. Anything but None when one is not a covered view DCLF draws whole, so the engine draws the light. */
		Fallback Whole(std::vector<Draw>& a_draws)
		{
			if (a_draws.empty())
				return Fallback::NoDraws;
			const auto& views = DCLF::ShadowViews::Get();
			if (!views.Valid() || !views.DclfDraws())
				return Fallback::NotDclf;
			const auto& capture = DCLF::PassCapture::Get();
			for (auto& draw : a_draws) {
				auto* accumulator = draw.descriptor->shaderAccumulator.get();
				const auto* data = accumulator ? accumulator->GetRuntimeData() : nullptr;
				if (!data || !draw.descriptor->camera || !data->batchRenderer)
					return Fallback::NoAccumulator;
				draw.view = views.ViewOfAccumulator(accumulator);
				const auto* view = views.At(draw.view);
				if (!view || !view->covered)
					return Fallback::NotCovered;
				draw.mode = static_cast<std::uint32_t>(data->renderMode);
				// A caster DCLF does not hold yet kept its passes for the engine.
				if (capture.CoveredKept(accumulator) || !GroupsEmpty(*data->batchRenderer))
					return Fallback::Kept;
			}
			return Fallback::None;
		}

		/** @brief Clears a slice as RenderShadowmap's ApplyState does: depth to 1, through the engine skeleton's clear pass. */
		void ClearSlice(std::uint32_t a_target, std::uint32_t a_slice)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, Other, globals::game::renderer);
			auto* view = a_slice < 8 ? renderer->GetDepthStencilData().depthStencils[a_target].views[a_slice] : nullptr;
			auto* context = reinterpret_cast<ID3D11DeviceContext*>(renderer->GetRuntimeData().context);
			if (!view || !context)
				return;
			if (!RenderGraph::Engine::Skeleton::Sites::TakeDepthClear(context, view, D3D11_CLEAR_DEPTH, 1.0f, 0))
				context->ClearDepthStencilView(view, D3D11_CLEAR_DEPTH, 1.0f, 0);
		}

		/** @brief The descriptor's target size and port, as RenderShadowmap reads them once its target is set. */
		void TargetOf(const Descriptor& a_descriptor, std::uint32_t& a_width, std::uint32_t& a_height, std::array<std::int32_t, 4>& a_port)
		{
			a_width = a_height = 0;
			const auto target = static_cast<std::uint32_t>(a_descriptor.renderTarget);
			if (auto* texture = target < DepthTarget::kTOTAL ? CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[target].texture : nullptr) {
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				a_width = desc.Width;
				a_height = desc.Height;
			}
			static_assert(sizeof(a_descriptor.port) == sizeof(a_port));
			std::memcpy(a_port.data(), &a_descriptor.port, sizeof(a_port));
		}

		/** @brief The descriptor's light transform, as RenderShadowmap writes it once its target is set. */
		std::array<float, 16> TransformOf(RE::BSShadowLight& a_light, const Descriptor& a_descriptor)
		{
			std::uint32_t width, height;
			std::array<std::int32_t, 4> port;
			TargetOf(a_descriptor, width, height, port);
			return DCLF::ShadowCameras::LightTransform(*a_descriptor.camera, port, a_light.GetIsParabolicLight(), width, height);
		}

		/**
		 * @brief The camera state RenderShadowmap's tail leaves: its SetCameraData from the camera's axes and grown frustum. The world's
		 * EarlyPrepass work reads it before Main::RenderPlayerView sets the main camera again (depth of field differs without it).
		 */
		void LeaveCameraState(RE::BSShadowLight& a_light, const Descriptor& a_descriptor)
		{
			std::uint32_t width, height;
			std::array<std::int32_t, 4> port;
			TargetOf(a_descriptor, width, height, port);
			const auto& camera = *a_descriptor.camera;
			bool toTexture = false;
			const RE::NiFrustum frustum = DCLF::ShadowCameras::ShadowmapFrustum(camera, port, a_light.GetIsParabolicLight(), width, height, toTexture);
			const auto& r = camera.world.rotate.entry;
			const float dir[3] = { r[0][0], r[1][0], r[2][0] };
			const float up[3] = { r[0][1], r[1][1], r[2][1] };
			const float right[3] = { r[0][2], r[1][2], r[2][2] };
			const float viewport[4] = { 0.0f, 1.0f, 1.0f, 0.0f };
			using SetCameraVectors = void (*)(void*, const RE::NiPoint3*, const float*, const float*, const float*, const RE::NiFrustum*, const float*);
			const auto base = REL::Module::get().base();
			reinterpret_cast<SetCameraVectors>(base + Address::kSetCameraVectors)(reinterpret_cast<void*>(base + Address::kGraphicsState), &camera.world.translate, dir,
				up, right, &frustum, viewport);
		}

		/** @brief RenderShadowmap's bookkeeping for one draw, after its Render's writes to the descriptor. */
		void Shadowmap(RE::BSShadowLight& a_light, const Draw& a_draw, std::uint32_t& a_index, bool& a_released)
		{
			// The scene accumulate array is dropped at the light's first draw. Later draws find it empty.
			if (!a_released) {
				a_light.GetRuntimeData().sceneAccumArray.clear();
				a_released = true;
			}
			if (a_draw.counts)
				++a_index;
			auto& descriptor = *a_draw.descriptor;
			if (static_cast<std::int32_t>(descriptor.renderTarget) == -1) {
				descriptor.renderTarget = static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(DepthTarget::kSHADOWMAPS);
				auto& freeMask = Global<std::uint32_t>(Address::kShadowFreeMask);
				std::uint32_t slice = 0;
				if (freeMask) {
					slice = static_cast<std::uint32_t>(std::countr_zero(freeMask));
					freeMask &= ~(1u << slice);
				}
				descriptor.shadowmapIndex = slice;
			}
			if (descriptor.clearRenderTarget)
				ClearSlice(static_cast<std::uint32_t>(descriptor.renderTarget), descriptor.shadowmapIndex);
			const auto transform = TransformOf(a_light, descriptor);
			std::memcpy(&descriptor.lightTransform, transform.data(), sizeof(transform));
		}

		/** @brief The light's Render in full, without drawing. Its writes to the descriptors come first, as in each Render. */
		void Render(RE::BSShadowLight& a_light, Kind a_kind, const std::vector<Draw>& a_draws, std::uint32_t& a_index)
		{
			auto& data = a_light.GetRuntimeData();
			auto* descriptors = data.shadowmapDescriptors.data();
			bool released = false;
			const std::uint32_t focusBase = Global<std::uint32_t>(Address::kFocusTargetBase);
			std::uint32_t local = 0;  // the copy and focus draws' own counter
			for (const auto& draw : a_draws) {
				auto& descriptor = *draw.descriptor;
				const bool focus = &descriptor >= data.focusShadowmapDescriptors && &descriptor < data.focusShadowmapDescriptors + std::size(data.focusShadowmapDescriptors);
				const auto slot = focus ? static_cast<std::uint32_t>(&descriptor - data.focusShadowmapDescriptors) : static_cast<std::uint32_t>(&descriptor - descriptors);
				if (focus) {
					descriptor.renderTarget = static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(DepthTarget::kSHADOWMAPS);
					descriptor.clearRenderTarget = true;
					if (a_kind == Kind::Frustum)
						descriptor.isEnabled = true;
					descriptor.shadowmapIndex = focusBase + slot;
				} else if (a_kind == Kind::Directional) {
					descriptor.renderTarget = static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(draw.copy ? DepthTarget::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM : DepthTarget::kSHADOWMAPS_ESRAM);
					descriptor.shadowmapIndex = slot;
				} else if (a_kind == Kind::Parabolic) {
					// The second hemisphere takes the first's target. The globals are SetupTechnique's ParabolaParam.
					if (slot == 1)
						descriptor.renderTarget = descriptors[0].renderTarget;
					Global<float>(DCLF::Engine::Address::kParabolaSide) = draw.parabola[1];
				}
				Shadowmap(a_light, draw, draw.counts ? a_index : local, released);
			}
			// A spot light's focus descriptors without an accumulator still get target 4.
			if (a_kind == Kind::Frustum && data.drawFocusShadows)
				for (std::uint32_t i = 0; i < FocusCount() && i < std::size(data.focusShadowmapDescriptors); ++i)
					data.focusShadowmapDescriptors[i].renderTarget = static_cast<RE::RENDER_TARGET_DEPTHSTENCIL>(DepthTarget::kSHADOWMAPS);
			// The last draw leaves its camera state, SubmitAccumulator's camera, accumulator and render mode set, and a paraboloid's Render
			// resets the radius.
			const auto& last = *a_draws.back().descriptor;
			LeaveCameraState(a_light, last);
			Global<RE::NiCamera*>(Address::kCurrentCamera) = last.camera.get();
			Global<RE::BSShaderAccumulator*>(Address::kCurrentAccumulator) = last.shaderAccumulator.get();
			RE::BSShaderManager::SetRenderMode(static_cast<RE::BSShaderManager::etRenderMode>(a_draws.back().mode));
			if (a_kind == Kind::Parabolic)
				Global<float>(DCLF::Engine::Address::kParabolaRadius) = -1.0f;
		}

		/** @brief Captures the light's views and does its Render's bookkeeping. Anything but None leaves the light to the engine. */
		Fallback RenderNative(RE::BSShadowLight& a_light, Kind a_kind, std::uint32_t& a_index)
		{
			DCLF_ZONE("CS.DCLF.ShadowMaps.Native");
			thread_local std::vector<Draw> draws;
			draws.clear();
			draws.reserve(16);
			ListDraws(a_light, a_kind, draws);
			if (const auto whole = Whole(draws); whole != Fallback::None)
				return whole;
			// Capture first: a view that cannot be captured hands the whole light back before anything is written.
			auto& indirect = DCLF::IndirectDraws::Get();
			const std::size_t mark = indirect.CapturedShadowViews();
			for (const auto& draw : draws)
				if (!indirect.CaptureNativeShadowView(draw.view, draw.mode, a_kind == Kind::Parabolic ? draw.parabola.data() : nullptr)) {
					indirect.DropShadowViewsFrom(mark);
					static std::atomic<bool> logged{ false };
					if (!logged.exchange(true))
						logger::info("[DCLF] native shadow maps: shadow view {} (mode {:#x}) could not be captured natively; the engine draws its light",
							draw.view, draw.mode);
					return Fallback::Capture;
				}
			Render(a_light, a_kind, draws, a_index);
			return Fallback::None;
		}


		template <Kind K>
		struct LightRender
		{
			static void thunk(RE::BSShadowLight* a_light, std::uint32_t& a_index)
			{
				const auto outcome = !shadowMapsOpen ? Fallback::NotOpen : !a_light ? Fallback::NoDraws :
				                                                                      RenderNative(*a_light, K, a_index);
				++lightCounts[static_cast<std::size_t>(outcome)];
				if (outcome != Fallback::None)
					func(a_light, a_index);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool RenderLightNatively(RE::BSShadowLight& a_light, std::uint32_t& a_index)
	{
		const auto kind = a_light.GetIsDirectionalLight() ? Kind::Directional : a_light.GetIsParabolicLight() ? Kind::Parabolic : Kind::Frustum;
		const auto outcome = shadowMapsOpen ? RenderNative(a_light, kind, a_index) : Fallback::NotOpen;
		++lightCounts[static_cast<std::size_t>(outcome)];
		if (outcome == Fallback::None)
			return true;
		// No engine Render: the light draws nothing this frame, and the walk moves past its slots as its Render would.
		thread_local std::vector<Draw> draws;
		draws.clear();
		ListDraws(a_light, kind, draws);
		for (const auto& draw : draws)
			a_index += draw.counts ? 1u : 0u;
		return false;
	}

	void SetShadowMapsOpen(bool a_open)
	{
		shadowMapsOpen = a_open;
		if (a_open || ++shadowFrames % kLineFrames != 0)
			return;
		std::string reasons;
		for (std::size_t i = 1; i < lightCounts.size(); ++i)
			if (lightCounts[i])
				reasons += fmt::format("{}{} {}", reasons.empty() ? "" : ", ", lightCounts[i], kFallbackNames[i]);
		logger::info("[DCLF] native shadow maps over {} frames: {} shadow lights rendered natively, the engine's Render for the rest ({})", kLineFrames,
			lightCounts[0], reasons.empty() ? "none" : reasons);
		lightCounts = {};
	}

	void InstallShadowMapHooks()
	{
		stl::write_vfunc<0x0A, LightRender<Kind::Directional>>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_vfunc<0x0A, LightRender<Kind::Frustum>>(RE::VTABLE_BSShadowFrustumLight[0]);
		stl::write_vfunc<0x0A, LightRender<Kind::Parabolic>>(RE::VTABLE_BSShadowParabolicLight[0]);
		logger::info("[DCLF] native shadow maps: the shadow lights' Render hooked; a light whose views DCLF draws whole skips it");
	}
}
