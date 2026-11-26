// Captures the shadow and occlusion views at the engine's hooks, on the render thread. For each view it records the target,
// viewport, camera and constants, using DCLF's own camera where it has one. Also decides whether DCLF covers the frame's shadows.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowEpochs.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

namespace DCLF
{
	namespace Draws
	{
		/** @brief DCLF's id for an engine rasterizer table entry, or 0 if the entry is out of range or empty. */
		std::uint32_t EngineRasterStateId(std::uint32_t a_fill, std::uint32_t a_cull, std::uint32_t a_bias, std::uint32_t a_scissor, std::uint32_t a_renderMode)
		{
			if (a_fill >= 2 || a_cull >= 3 || a_bias >= 12 || a_scissor >= 2)
				return 0;
			auto* engineState = EngineRasterStates()[a_fill][a_cull][a_bias][a_scissor];
			if (!engineState)
				return 0;
			D3D11_RASTERIZER_DESC desc{};
			engineState->GetDesc(&desc);
			return DrawPipelines::Get().ShadowRasterStateId(desc, a_renderMode);
		}

		/** @brief Reads the depth format, viewport and eye the engine just drew a view with from the renderer's state. */
		void CaptureViewTarget(PendingView& a_view, std::uint32_t a_target)
		{
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsv->GetDesc(&dsvDesc);
				a_view.dsvFormat = dsvDesc.Format;
			}
			a_view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
			a_view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
			a_view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
			a_view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
			a_view.minDepth = shadowState.viewPort.MinDepth;
			a_view.maxDepth = shadowState.viewPort.MaxDepth;
			a_view.eye = shadowState.posAdjust.getEye();
		}

		/** @brief The depth format alone, for a view the engine did not draw. */
		void CaptureViewFormat(PendingView& a_view, std::uint32_t a_target)
		{
			if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].views[0]) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsv->GetDesc(&dsvDesc);
				a_view.dsvFormat = dsvDesc.Format;
			}
		}

		/** @brief Replaces the captured viewport and eye with DCLF's own camera. Returns false and keeps the captured values if there is none. */
		bool ApplyOwnViewport(PendingView& a_view, const ShadowCameras::Camera* a_cascade, std::uint32_t a_target)
		{
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!a_cascade || !texture)
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const auto port = ShadowCameras::Viewport(a_cascade->port, desc.Width, desc.Height);
			const RE::NiPoint3 eye{ a_cascade->eye[0], a_cascade->eye[1], a_cascade->eye[2] };
			a_view.x = port[0];
			a_view.y = port[1];
			a_view.width = port[2];
			a_view.height = port[3];
			a_view.minDepth = 0.0f;
			a_view.maxDepth = 1.0f;
			a_view.eye = eye;
			return true;
		}

		/** @brief Sets the view's VS_PerFrame block and caster planes from DCLF's own camera. */
		void ApplyOwnCamera(PendingView& a_view, const ShadowCameras::Camera& a_cascade)
		{
			std::array<std::byte, ShadowCameras::kBlockBytes> block{};
			ShadowCameras::Block(a_cascade, block.data());
			const auto* mine = reinterpret_cast<const float*>(block.data());
			std::memcpy(a_view.perFrame.data(), block.data(), block.size());
			a_view.perFrameBytes = static_cast<std::uint32_t>(block.size());
			std::memcpy(a_view.viewProj.data(), mine + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
			for (std::uint32_t q = 0; q < 6; ++q)
				for (std::uint32_t i = 0; i < 4; ++i)
					a_view.cullPlanes[q][i] = a_cascade.caster.plane[q][i];
			a_view.cullPlaneMask = a_cascade.caster.mask;
		}

		/**
		 * @brief Draws a focus view over its whole slice. The engine uses a sub-rectangle that changes most frames, but placements are
		 * per slice. Folding the sub-rectangle into the projection puts casters on the same pixels.
		 */
		void FocusToSlice(PendingView& a_view, const ShadowCameras::Camera& a_cascade, std::uint32_t a_target)
		{
			auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[a_target].texture;
			if (!texture)
				return;
			D3D11_TEXTURE2D_DESC desc{};
			texture->GetDesc(&desc);
			const float w = static_cast<float>(desc.Width), h = static_cast<float>(desc.Height);
			// The same rectangle as Renderer::UpdateViewPort, without rounding.
			const auto& port = a_cascade.port;
			const float sx = port[0] * w, sy = (1.0f - port[2]) * h, sw = (port[1] - port[0]) * w, sh = (port[2] - port[3]) * h;
			const float ax = sw / w, bx = (sw + 2.0f * sx) / w - 1.0f;
			const float ay = sh / h, by = 1.0f - (sh + 2.0f * sy) / h;
			auto* block = reinterpret_cast<float*>(a_view.perFrame.data());
			for (const std::uint32_t c : { 4u, 8u, 12u }) {
				float* row0 = block + c * 4;
				float* row1 = row0 + 4;
				const float* row3 = row0 + 12;
				for (std::uint32_t i = 0; i < 4; ++i) {
					row0[i] = ax * row0[i] + bx * row3[i];
					row1[i] = ay * row1[i] + by * row3[i];
				}
			}
			std::memcpy(a_view.viewProj.data(), block + 32, sizeof(float) * 16);
		}

		/** @brief Asks the view's shadow source for DCLF's own camera. Returns false if no source owns this kind of view. */
		bool OwnViewOf(const ShadowViews::View& a_view, std::uint32_t a_sceneFrame, PendingView& a_pending, const ShadowCameras::Camera*& a_own)
		{
			const auto source = Extensions::ShadowViewsSet().sourceOf[static_cast<std::size_t>(a_view.kind)][a_view.focus ? 1 : 0];
			if (source >= Api::ShadowSource::Count)
				return false;
			Api::ShadowSourceCamera request{ a_view.light, a_view.descriptor, a_sceneFrame };
			a_own = Extensions::SourceCamera(source, request);
			a_pending.portalContext = a_own && a_own->valid ? a_own->portalContext : 0u;
			a_pending.parabolic = a_own && a_own->valid && a_own->parabolic;
			if (a_pending.parabolic) {
				a_pending.parabolicBack = a_own->parabolicBack;
				a_pending.parabolicBothSides = a_own->parabolicBothSides;
				a_pending.parabolicSphere = a_own->parabolicSphere;
				a_pending.parabolicPlane = a_own->parabolicPlane;
			}
			if (source == Api::ShadowSource::Focus) {
				a_pending.focusTarget = request.focusTarget;
				a_pending.focus = true;
			}
			return true;
		}

		/**
		 * @brief Copies the engine's caster volume, which BSShadowDirectionalLight::UpdateCamera builds and FUN_1414f0920 tests.
		 * It is much tighter than the cascade box. Focus views have none, because their descriptor indexes the focus array.
		 */
		void CaptureCasterVolume(PendingView& a_view, const ShadowViews::View& a_shadowView)
		{
			if (auto& lightData = const_cast<RE::BSShadowLight*>(a_shadowView.light)->GetRuntimeData();
				!a_shadowView.focus && a_shadowView.descriptor < lightData.shadowmapDescriptors.size()) {
				const auto* process = lightData.shadowmapDescriptors[a_shadowView.descriptor].cullingProcess;
				if (process && process->doCustomCullPlanes) {
					const auto& planes = process->customCullPlanes;
					for (std::uint32_t p = 0; p < RE::NiFrustumPlanes::Planes::kTotal; ++p) {
						a_view.cullPlanes[p][0] = planes.cullingPlanes[p].normal.x;
						a_view.cullPlanes[p][1] = planes.cullingPlanes[p].normal.y;
						a_view.cullPlanes[p][2] = planes.cullingPlanes[p].normal.z;
						a_view.cullPlanes[p][3] = planes.cullingPlanes[p].constant;
					}
					a_view.cullPlaneMask = planes.activePlanes.underlying() & 0x3Fu;
				}
			}
		}

		/** @brief Checks the block's CameraPosAdjust (c40) against the renderer's eye, and reports every 1000 views. */
		void CheckCapturedEye(const PendingView& a_view, std::uint32_t a_viewId, std::uint32_t a_renderMode)
		{
			static std::uint64_t views = 0, fallback = 0, differ = 0;
			static float largest = 0.0f;
			static std::string first;
			++views;
			const auto* block = reinterpret_cast<const float*>(a_view.perFrame.data());
			if (a_view.perFrameBytes >= 164 * sizeof(float)) {
				const float d = std::max({ std::abs(block[160] - a_view.eye.x), std::abs(block[161] - a_view.eye.y), std::abs(block[162] - a_view.eye.z) });
				if (d > 0.01f) {
					++differ;
					if (first.empty())
						first = fmt::format("view {} mode {:#x}: block ({:.1f} {:.1f} {:.1f}), renderer ({:.1f} {:.1f} {:.1f})", a_viewId, a_renderMode, block[160], block[161],
							block[162], a_view.eye.x, a_view.eye.y, a_view.eye.z);
				}
				largest = std::max(largest, d);
			}
			if (!ConstantMirror::Get().Contents(*globals::game::perFrame.get()).size())
				++fallback;
			if (views == 1000) {
				logger::info("[DCLF] shadow view capture: {} views, {} from the cached copy, {} whose b12 eye is not the renderer's (largest {:.2f}){}{}", views, fallback, differ,
					largest, differ ? " <- CAPTURE" : " <- OK", first.empty() ? "" : "; first " + first);
				views = fallback = differ = 0;
				largest = 0.0f;
				first.clear();
			}
		}

		/**
		 * @brief Captures VS_PerFrame (b12) now, before the next view rewrites it. Uses CS's cached copy until the constant
		 * mirror has seen a write.
		 */
		void CapturePerFrame(PendingView& a_view)
		{
			auto& mirror = ConstantMirror::Get();
			if (auto* perFrame = *globals::game::perFrame.get()) {
				mirror.Watch(perFrame);
				const auto contents = mirror.Contents(perFrame);
				if (contents.size() >= 48 * sizeof(float)) {
					a_view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), a_view.perFrame.size()));
					std::memcpy(a_view.perFrame.data(), contents.data(), a_view.perFrameBytes);
				}
			}
			if (!a_view.perFrameBytes) {
				const auto& cached = globals::game::frameBufferCached.data;
				static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
				a_view.perFrameBytes = sizeof(cached);
				std::memcpy(a_view.perFrame.data(), &cached, sizeof(cached));
			}
			std::memcpy(a_view.viewProj.data(), reinterpret_cast<const float*>(a_view.perFrame.data()) + 32, sizeof(float) * 16);
			a_view.hasViewProj = true;
		}
	}

	std::uint32_t IndirectDraws::ShadowViewCapacity()
	{
		// 0 means the engine draws every view.
		auto& engine = *impl->shadowEngine;
		if (!engine.SetupShadow() || engine.shadowPlacements.empty())
			return 0;
		// The shadow maps were imported when the placements were built. The engine target set keeps their textures for the
		// session, so they are never replaced.
		if (Scene::FrameView::Get().NotDrawingYet())
			return 0;
		// Staged rebuilds do not count, because the attached graph keeps running. An unstaged one leaves the views undrawn.
		if (auto* host = RenderGraphRuntime::Get().Host(); !host || RenderGraphRuntime::Get().RebuildRequestedUnstaged()) {
			LoadHold::NoteUndrawable(LoadHold::View::Shadow, host ? "the shadow views wait for an unstaged graph rebuild" : "the shadow views have no graph host");
			return 0;
		}
		return UINT32_MAX;
	}

	void IndirectDraws::DecideShadowCoverage()
	{
		// Must run before the engine keeps its first shadow light, so every caster registration sees this frame's verdict.
		if (!impl->shadowEngine->shadow || impl->shadowEngine->shadowPlacements.empty())
			return;
		// DCLF owns the views once the shadow epoch is recorded and a shadow payload is installed.
		if (!impl->EpochCovered(EpochOfShape(ShapeKind::Shadow, kShadowShape)) || !impl->installedDraws || !impl->installedDraws->shadow) {
			ShadowViews::Get().SetDrawable(false);
			// A hold waits while the epoch is not recorded yet; outside one it is a tap error. A missing payload alone is
			// neither, since a scene with no casters has none.
			if (!impl->EpochCovered(EpochOfShape(ShapeKind::Shadow, kShadowShape)))
				LoadHold::NoteUndrawable(LoadHold::View::Shadow, "the shadow epoch is not recorded");
		}
	}

	void IndirectDraws::BeginShadowFrame()
	{
		if (ShadowViews::Get().Unrecorded())
			++impl->shadowUnrecorded;
		impl->shadowEngine->pendingViews.clear();
	}

	void IndirectDraws::CaptureShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// Nothing is drawn here. ExecuteShadowFrame draws the captured views later.
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		(void)CaptureView(a_viewId, a_renderMode, nullptr, false);
	}

	bool IndirectDraws::CaptureNativeShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode, const float* a_parabola)
	{
		return CaptureView(a_viewId, a_renderMode, a_parabola, true);
	}

	std::size_t IndirectDraws::CapturedShadowViews() const
	{
		return impl->shadowEngine->pendingViews.size();
	}

	void IndirectDraws::DropShadowViewsFrom(std::size_t a_mark)
	{
		auto& pending = impl->shadowEngine->pendingViews;
		for (std::size_t i = a_mark; i < pending.size(); ++i)
			ShadowViews::Get().UndoCapture(pending[i].viewId);
		if (a_mark < pending.size())
			pending.resize(a_mark);
	}

	bool IndirectDraws::CaptureView(std::uint32_t a_viewId, std::uint32_t a_renderMode, const float* a_parabola, bool a_native)
	{
		const auto start = std::chrono::steady_clock::now();
		auto& engine = *impl->shadowEngine;
		if (!a_native)
			++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		// A native view that fails goes back to the engine, which captures it again, so only engine views count.
		auto notReady = [&](ShadowNotReady a_reason) {
			if (a_native)
				return false;
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
			if (a_renderMode >= PassCapture::kFirstShadowMode)
				engine.ShadowsNotDrawn(shadowStats);
			return false;
		};
		if (!pipelines.Enabled() || !utility || !engine.SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The scene tables are not read here. ExecuteShadowFrame checks them.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// Not covered this frame, so the engine drew it.
		if (!shadowView->covered) {
			++shadowStats.uncovered;
			return false;
		}
		// The target is predicted. A cascade's first capture each frame is its volumetric copy.
		// A native view counts only once it is captured, since a failure hands the light back to the engine.
		const std::uint32_t occurrence = a_native ? ShadowViews::Get().CapturesOf(a_viewId) : ShadowViews::Get().NextCapture(a_viewId);
		const bool copyDraw = shadowView->volumetricCopy && occurrence == 0;
		const std::uint32_t target = copyDraw ? static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM) : shadowView->drawTarget;
		const std::uint32_t slice = shadowView->drawSlice;
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		// The volumetric copy holds only volumetric-only casters, batch group 15 with flag 0x100.
		const bool volumetricCopy = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM;
		if (targetIndex == ~0u || !engine.ImportShadowDepth(targetIndex, target)) {
			// An unexpected target. Logged once per target.
			if (target < 32 && !((engine.shadowLoggedTargets >> target) & 1)) {
				engine.shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		// An engine view: read the table entry now, because CS's ShadowmapCascadeRasterizerFix swaps in per-cascade bias copies only for
		// this window. Use cull mode 1, not the renderer's, which is whatever drew last. Fall back to 0 for views like the volumetric copy.
		// A native view takes its placement's state, which an engine view must match.
		std::uint32_t rasterState = 0;
		if (!a_native) {
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 1, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode, a_renderMode);
			if (rasterState == 0)
				rasterState = EngineRasterStateId(shadowState.rasterStateFillMode, 0, shadowState.rasterStateDepthBiasMode, shadowState.rasterStateScissorMode,
					a_renderMode);
			if (rasterState == 0)
				return notReady(ShadowNotReady::RasterState);
		}

		auto& view = engine.pendingViews.emplace_back();
		const auto drop = [&](ShadowNotReady a_reason) {
			engine.pendingViews.pop_back();
			return notReady(a_reason);
		};
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		view.rasterState = rasterState;
		view.casterClass = volumetricCopy ? 1u : 0u;
		view.sunView = shadowView->kind == ShadowViews::Kind::Directional && !shadowView->focus;
		if (a_native)
			CaptureViewFormat(view, target);
		else
			CaptureViewTarget(view, target);
		const std::uint32_t sceneFrame = Scene::FrameView::Get().SceneStep();
		const ShadowCameras::Camera* ownView = nullptr;
		const bool owned = OwnViewOf(*shadowView, sceneFrame, view, ownView) && ApplyOwnViewport(view, ownView, target);
		// A native view has no engine camera block to fall back to.
		if (a_native && !owned)
			return drop(ShadowNotReady::Setup);
		// A focus view draws over its whole slice. See FocusToSlice.
		if (owned && shadowView->focus)
			if (auto* texture = globals::game::renderer->GetDepthStencilData().depthStencils[target].texture) {
				D3D11_TEXTURE2D_DESC desc{};
				texture->GetDesc(&desc);
				view.x = 0;
				view.y = 0;
				view.width = desc.Width;
				view.height = desc.Height;
			}
		// The view must match a placement's mode and state and be the only view there this frame. Otherwise it is a defect.
		{
			const std::size_t placement = engine.PlacementOf(view);
			if (a_native && placement != SIZE_MAX)
				view.rasterState = rasterState = engine.shadowPlacements[placement].rasterState;
			const std::span<const Draws::PendingView> others(engine.pendingViews.data(), engine.pendingViews.size() - 1);
			std::string defect;
			if (placement == SIZE_MAX)
				defect = "at no placement";
			else if (const auto& p = engine.shadowPlacements[placement]; p.rasterState != rasterState || p.modeIndex != view.modeIndex)
				defect = fmt::format("at placement {} (mode {}, state {}) with mode {}, state {}", placement, p.modeIndex, p.rasterState, view.modeIndex, rasterState);
			else if (std::any_of(others.begin(), others.end(), [&](const PendingView& a_other) { return a_other.slot == p.slot; }))
				defect = fmt::format("at placement {}, which a view of the frame already drew at", placement);
			if (!defect.empty()) {
				if (engine.placementDefectsLogged++ < 16)
					logger::warn("[DCLF] shadow view {} ({}, mode {:#x}) drew into target {} slice {} at ({} {}) {}x{} {} <- PLACEMENT", a_viewId,
						ShadowViews::KindName(shadowView->kind), a_renderMode, targetIndex, slice, view.x, view.y, view.width, view.height, defect);
				return drop(ShadowNotReady::Placement);
			}
			view.slot = engine.shadowPlacements[placement].slot;
		}
		CaptureCasterVolume(view, *shadowView);
		// PerTechnique (b0). ParabolaParam comes from the same two engine globals SetupTechnique reads, or a native paraboloid's values.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(Engine::Address::kParabolaRadius) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(Engine::Address::kParabolaSide) };
			const float radius = a_parabola ? a_parabola[0] : *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = a_parabola ? a_parabola[1] : *parabolaSide.get();
			// c2 is unused. Records are absolute and Utility.hlsl subtracts the view's CameraPosAdjust.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		// With DCLF's own camera, the engine's block is not captured.
		if (!owned)
			CapturePerFrame(view);
		if (owned)
			ApplyOwnCamera(view, *ownView);
		// Must come after ApplyOwnCamera.
		if (owned && shadowView->focus)
			FocusToSlice(view, *ownView, target);
		if (a_native) {
			(void)ShadowViews::Get().NextCapture(a_viewId);
			++shadowStats.views;
		}
		CheckCapturedEye(view, a_viewId, a_renderMode);
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		return true;
	}

	void IndirectDraws::OcclusionView(std::uint32_t a_view, const RE::NiCamera& a_camera)
	{
		// Builds DCLF's view from the camera Skylighting just set up, before RenderMask draws the map. Skylighting swaps its own
		// texture into depth target 10 for the sky map. Runs every frame the map renders, so next frame's build has its pipelines.
		auto& engine = *impl->shadowEngine;
		if (a_view >= kOcclusionViews || !Extensions::OcclusionOn(a_view) || !engine.SetupShadow())
			return;
		constexpr auto target = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP);
		const std::uint32_t rasterState = EngineRasterStateId(0, 1, 0, 0, kOcclusionRenderMode);
		if (!rasterState || !engine.ImportShadowDepth(OcclusionDepthTarget(a_view), target))
			return;
		auto& occlusion = engine.occlusion[a_view];
		const std::uint32_t mode = OcclusionModeOf(a_view);
		if (!engine.readyModes[mode]) {
			engine.readyModes[mode] = true;
			engine.NoteCapability(engine.shadowCatalogBuilt, fmt::format("occlusion map {} set up before its setup", a_view));
		}
		if (const auto& states = engine.readyStates[mode].Of(false); !std::binary_search(states.begin(), states.end(), rasterState)) {
			engine.readyStates[mode].Add(rasterState, false);
			engine.NoteCapability(engine.shadowCatalogBuilt, fmt::format("occlusion map {} draws with rasterizer state {}, not in the catalog", a_view, rasterState));
		}
		const auto& depth = globals::game::renderer->GetDepthStencilData().depthStencils[target];
		if (!depth.texture || !depth.views[0])
			return;
		auto& view = occlusion.view;
		view = {};
		view.viewId = ~0u;
		view.renderMode = kOcclusionRenderMode;
		view.modeIndex = mode;
		view.targetIndex = OcclusionDepthTarget(a_view);
		view.slice = 0;
		view.rasterState = rasterState;
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		depth.views[0]->GetDesc(&dsvDesc);
		view.dsvFormat = dsvDesc.Format;
		D3D11_TEXTURE2D_DESC desc{};
		depth.texture->GetDesc(&desc);
		ShadowCameras::Camera camera;
		ShadowCameras::CameraMatrices(a_camera, camera);
		const auto port = ShadowCameras::Viewport(camera.port, desc.Width, desc.Height);
		view.x = port[0];
		view.y = port[1];
		view.width = port[2];
		view.height = port[3];
		view.minDepth = 0.0f;
		view.maxDepth = 1.0f;
		view.eye = { camera.eye[0], camera.eye[1], camera.eye[2] };
		ShadowCameras::Block(camera, view.perFrame.data());
		view.perFrameBytes = ShadowCameras::kBlockBytes;
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		occlusion.rasterState = rasterState;
		if (view.dsvFormat != occlusion.dsvFormat) {
			engine.NoteCapability(engine.shadowCatalogBuilt && occlusion.dsvFormat != DXGI_FORMAT_UNKNOWN,
				fmt::format("occlusion map {} draws into format {}, not {}", a_view, static_cast<int>(view.dsvFormat), static_cast<int>(occlusion.dsvFormat)));
			occlusion.dsvFormat = view.dsvFormat;
		}
		occlusion.capturedFrame = Scene::FrameView::Get().SceneStep();
		impl->PostRevisionInputs();
	}

	std::vector<ShadowViewLayout> ShadowEngine::PredictedOcclusion(bool a_onlyOn) const
	{
		std::vector<ShadowViewLayout> out;
		for (std::uint32_t v = 0; v < kOcclusionViews; ++v) {
			const auto& o = occlusion[v];
			if ((a_onlyOn && !Extensions::OcclusionOn(v)) || !o.rasterState || o.dsvFormat == DXGI_FORMAT_UNKNOWN)
				continue;
			out.push_back(LayoutOf(o.view, OcclusionSlot(v), OcclusionModeOf(v), OcclusionDepthTarget(v)));
		}
		return out;
	}
}
