// A loading model's shadow lights, depth pass and shadow masks in a native menu scene render (roadmap phase 5,
// docs/development/plans/snap5-menus5.md). UI3DSceneManager::Render (AE 52727) with scheme 4 and its depth pass renders the menu scene
// node's shadow lights (FUN_140973210, AE 0x140973210), a depth pass into kMAIN's depth and the menu masks (FUN_140972f70) before its
// colour pass. CS does each from the scene graph: the light loop through the native shadow ports (Shadows/Native), each map and the
// depth pass as Utility draws CS computes, the masks through the shadow mask family. No engine render function runs.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Engine/Engine.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LocalCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"

#include "Globals.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"
#include "Utils/Logging.h"

#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes::Native
{
	namespace
	{
		/** @brief FUN_1414a4030 (AE 106367) on the menu scene node: every kept slot emptied, its light's mask index reset. */
		void ResetKept(RE::ShadowSceneNode& a_node)
		{
			for (auto& light : a_node.GetRuntimeData().shadowLightsAccum)
				if (light) {
					light->GetRuntimeData().maskIndex = 0xFF;
					light = nullptr;
				}
		}

		/**
		 * @brief FUN_1414a3fb0 (AE 106365) with one slot, as the menu's loop calls it. The world's keep (NativeCasters) also tells DCLF's
		 * frame driver; a menu light is no DCLF view, so it does not.
		 */
		void Keep(RE::ShadowSceneNode& a_node, RE::BSShadowLight* a_light, std::uint32_t a_slot)
		{
			auto& kept = a_node.GetRuntimeData().shadowLightsAccum;
			if (a_slot >= kept.size())
				return;
			if (auto* old = kept[a_slot]; old && old != a_light)
				old->GetRuntimeData().maskIndex = 0xFF;
			kept[a_slot] = a_light;
		}

		/** @brief Appends the geometry under a_object a shadow camera sees: hidden subtrees skipped, a switch node's active child, bounds by a_inside. */
		template <class Inside>
		void Casters(RE::NiAVObject* a_object, const Inside& a_inside, std::vector<RE::BSGeometry*>& a_out)
		{
			if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden) || !a_inside(a_object->worldBound))
				return;
			if (auto* geometry = a_object->AsGeometry()) {
				a_out.push_back(geometry);
				return;
			}
			auto* node = a_object->AsNode();
			if (!node)
				return;
			if (auto* switchNode = netimmerse_cast<RE::NiSwitchNode*>(node)) {
				const auto index = switchNode->index;
				if (index >= 0 && static_cast<std::size_t>(index) < node->GetChildren().size())
					Casters(node->GetChildren()[static_cast<std::uint16_t>(index)].get(), a_inside, a_out);
				return;
			}
			for (auto& child : node->GetChildren())
				Casters(child.get(), a_inside, a_out);
		}

		struct ShadowState
		{
			std::uint64_t leftOut = 0;
			std::uint64_t renders = 0;
		};

		ShadowState& GetShadowState()
		{
			static ShadowState state;
			return state;
		}

		/** @brief a_view's draw of each geometry. A draw left out of this render is logged once; any other refusal is fatal (decision 149). */
		void AddDraws(const UtilityView& a_view, const std::vector<RE::BSGeometry*>& a_geometry, Run& a_run)
		{
			for (auto* geometry : a_geometry) {
				std::string detail;
				const char* why = AddUtility(a_view, *geometry, a_run, detail);
				if (!why)
					continue;
				const char* name = geometry->name.c_str() ? geometry->name.c_str() : "?";
				if (LeftOut(why)) {
					if (GetShadowState().leftOut++ == 0)
						logger::info("[DCLF] menu scenes: {} draw {} is left out of this render: {} ({})", NameOf(a_view.view), name, why, detail);
					continue;
				}
				::RenderGraph::Fatal("menu scenes", std::format("{} draw {}: {} ({})", NameOf(a_view.view), name, why, detail));
			}
		}

		/** @brief Hands a run to its view's client, or keeps its imports alive when the client cannot take it now. */
		void SubmitRun(std::shared_ptr<Run> a_run)
		{
			if (a_run->draws.empty() || !SceneDraws::Available() || !SceneDraws::CanTake(a_run->view))
				SceneDraws::Keep(std::move(a_run->owners));
			else
				SceneDraws::Submit(std::move(a_run));
		}

		/**
		 * @brief A kept light's maps, after its Render's bookkeeping: each descriptor's camera draws the menu geometry it sees into the
		 * slice it was given. A paraboloid's back hemisphere draws into the front's slice (RenderShadowmap binds only a descriptor that
		 * clears) and both hemispheres take every caster in the light's sphere; the parabola clip drops the other side.
		 */
		void DrawLight(RE::BSShadowLight& a_light, RE::NiAVObject* a_root, const Inputs& a_in, Run& a_run)
		{
			auto& descriptors = a_light.GetRuntimeData().shadowmapDescriptors;
			const bool parabolic = a_light.GetIsParabolicLight();
			const auto* niLight = a_light.light.get();
			if (!niLight || descriptors.empty())
				::RenderGraph::Fatal("menu scenes", "a kept menu shadow light with no light or no shadow map");
			const float radius = niLight->GetLightRuntimeData().radius.x;
			const auto& depths = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils;
			const std::uint32_t count = (std::min)(a_light.shadowMapCount, static_cast<std::uint32_t>(descriptors.size()));
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& descriptor = descriptors[i];
				const auto& bound = parabolic ? descriptors[0] : descriptor;
				auto* camera = descriptor.camera.get();
				auto* accumulator = descriptor.shaderAccumulator.get();
				const auto* data = accumulator ? accumulator->GetRuntimeData() : nullptr;
				const auto target = static_cast<std::uint32_t>(bound.renderTarget);
				if (!camera || !data || target != static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS) || bound.shadowmapIndex >= kMaxShadowSlices)
					::RenderGraph::Fatal("menu scenes", std::format("a menu shadow map CS cannot draw (camera {}, accumulator {}, target {}, slice {})", camera != nullptr,
															data != nullptr, target, bound.shadowmapIndex));
				UtilityView view;
				view.view = View::MenuShadow;
				view.renderMode = static_cast<std::uint32_t>(data->renderMode);
				view.eye = camera->world.translate;
				if (parabolic)
					view.parabola = { 1.0f / radius, i == 1 ? -1.0f : 1.0f };
				view.slice = bound.shadowmapIndex;
				view.textureTransformBuffer = a_in.material.vanilla.textureTransformBuffer;
				std::uint32_t width = 0, height = 0;
				if (auto* texture = depths[target].texture) {
					D3D11_TEXTURE2D_DESC desc{};
					texture->GetDesc(&desc);
					width = desc.Width;
					height = desc.Height;
				}
				ShadowCameras::Camera matrices;
				ShadowCameras::CameraMatrices(*camera, matrices);
				view.viewport.rect = ShadowCameras::Viewport(matrices.port, width, height);
				view.viewport.depthRange = { 0.0f, 1.0f };
				UtilityFrameRows(*camera, view.frame);
				std::vector<RE::BSGeometry*> casters;
				if (parabolic) {
					const auto& centre = niLight->world.translate;
					Casters(
						a_root, [&](const RE::NiBound& a_bound) {
							const auto d = a_bound.center - centre;
							const float reach = radius + a_bound.radius;
							return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
						},
						casters);
				} else {
					ShadowCameras::Planes planes;
					ShadowCameras::FrustumPlanes(camera->GetRuntimeData2().viewFrustum, camera->world, planes);
					Casters(
						a_root, [&](const RE::NiBound& a_bound) {
							for (std::uint32_t p = 0; p < 6; ++p) {
								const auto& plane = planes.plane[p];
								if ((planes.mask & (1u << p)) &&
									plane[0] * a_bound.center.x + plane[1] * a_bound.center.y + plane[2] * a_bound.center.z - plane[3] < -a_bound.radius)
									return false;
							}
							return true;
						},
						casters);
				}
				AddDraws(view, casters, a_run);
			}
		}
	}

	void RenderShadowsAndDepth(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, RE::NiAVObject* a_root, const Inputs& a_in,
		std::span<const ScenePass> a_passes)
	{
		auto* node = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[1];
		if (!node)
			::RenderGraph::Fatal("menu scenes", "a loading model's render with no menu scene node");

		// FUN_140973210, which scheme 4 runs while iLoadingMenuShadowLightFlags is set: the first four active shadow lights whose camera
		// update passes are kept, each with the next mask channel, its scissor and its Accumulate bookkeeping; the others return their maps.
		static const REL::Relocation<const std::int32_t*> shadowFlags{ RELOCATION_ID(510351, 383327) };
		auto shadows = std::make_shared<Run>();
		shadows->view = View::MenuShadow;
		if (a_manager.currentlightScheme == RE::INTERFACE_LIGHT_SCHEME::kLoading && *shadowFlags.get() != 0) {
			ResetKept(*node);
			std::uint32_t count = 0, kept = 0;
			for (const auto& pointer : node->GetRuntimeData().activeShadowLights) {
				auto* light = pointer.get();
				if (!light)
					continue;
				if (light->GetIsDirectionalLight())
					::RenderGraph::Fatal("menu scenes", "a directional shadow light in a menu scene");
				if (kept < kMaxShadowSlices && Shadows::Native::UpdateLightCamera(*light, a_camera)) {
					Keep(*node, light, count);
					light->GetRuntimeData().maskIndex = kept;
					Shadows::Native::ProjectLightRect(*light, a_camera, count);
					Shadows::Native::AccumulateBookkeeping(*light, count, kept);
					++kept;
				} else
					Shadows::Native::ReturnShadowmaps(*light);
			}
			// Each kept light's Render, walked as the engine's loop walks the kept array, then its maps.
			const auto& accumulated = node->GetRuntimeData().shadowLightsAccum;
			std::uint32_t index = 0;
			while (index < accumulated.size() && accumulated[index]) {
				auto* light = accumulated[index];
				const std::uint32_t before = index;
				Shadows::Engine::RenderMenuLight(*light, index);
				if (index == before)
					++index;
				DrawLight(*light, a_root, a_in, *shadows);
			}
		}
		SubmitRun(std::move(shadows));

		// The depth pass (StartAccumulating with flags | 0x20) into kMAIN's depth, with no colour target bound.
		auto depth = std::make_shared<Run>();
		depth->view = View::MenuDepth;
		UtilityView view;
		view.view = View::MenuDepth;
		view.eye = a_in.eye;
		view.textureTransformBuffer = a_in.material.vanilla.textureTransformBuffer;
		view.viewport = MenuViewport(kMainTarget, DrawState{});
		FrameRows(a_in, view.frame);
		std::vector<RE::BSGeometry*> geometry;
		for (const auto& pass : a_passes)
			if (!pass.membrane)
				geometry.push_back(pass.geometry);
		AddDraws(view, geometry, *depth);
		SubmitRun(std::move(depth));

		// The menu masks (FUN_140972f70) from the kept lights, the depth pass's depth and the menu camera.
		ShadowMask::DrawMenuMasks(a_in.eye);
		if (GetShadowState().renders++ == 0)
			logger::info("[DCLF] menu scenes: the first native loading model render with its shadow maps and depth pass");
	}
}
