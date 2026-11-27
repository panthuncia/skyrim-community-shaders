// SunViews: the UpdateCamera hook that writes the sun's board, and what Core reads of it. Render thread.

#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SunVolume.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include <algorithm>

#include "State.h"

namespace DCLF::Features::Shadows::Sun
{
	SunFrame& SunBoard()
	{
		static SunFrame frame;
		return frame;
	}

	SunViews& SunViews::Get()
	{
		static SunViews instance;
		return instance;
	}

	void SunViews::Update(const void* a_light)
	{
		auto* node = CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState) ? CS_ENGINE_READ(ShaderManagerState, RecordTime, globals::game::smState)->shadowSceneNode[0] : nullptr;
		const auto* sun = node ? node->GetRuntimeData().sunShadowDirLight : nullptr;
		// The shadow scene node's caster list holds a second directional light whose descriptors are never drawn. Take the sun alone.
		if (!sun || a_light != sun)
			return;
		auto& light = *const_cast<RE::BSShadowDirectionalLight*>(sun);
		// Written in place on this thread. Registration threads read the cascades only after SunAccumulation's bitsReady release.
		SunFrame& next = SunBoard();
		next.sceneFrame = Scene::FrameView::Get().SceneStep();
		next.valid = false;
		next.cascades = {};
		const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
		next.count = std::min<std::uint32_t>(descriptors.size(), SunFrame::kMaxCascades);
		for (std::uint32_t d = 0; d < next.count; ++d) {
			const auto& descriptor = descriptors[d];
			const auto* camera = descriptor.camera.get();
			if (!camera)
				continue;
			auto& cascade = next.cascades[d];
			ShadowCameras::CameraMatrices(*camera, cascade);
			ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cascade.frustum);
			const auto* process = static_cast<const RE::NiCullingProcess*>(descriptor.cullingProcess);
			if (process && process->doCustomCullPlanes)
				ShadowCameras::CullPlanes(process, *camera, cascade.caster);
			ShadowCameras::CullPlanes(process, *camera, cascade.cull);
			cascade.valid = true;
		}
		auto& runtime = light.GetShadowDirectionalLightRuntimeData();
		next.fullFrusta.clear();
		if (const auto* camera = runtime.fullFrustumCamera.get())
			for (const auto& process : runtime.fullFrustumCullingProcessArray)
				if (process)
					ShadowCameras::CullPlanes(process.get(), *camera, next.fullFrusta.emplace_back());
		next.valid = true;
	}

	namespace
	{
		/** @brief The board a reader reads: native's render side the copy its light set carries (with its own step), else the board. */
		const SunFrame& ReadBoard(std::uint32_t& a_frame)
		{
			if (const auto* boards = Native::AdoptedBoards(); boards && boards->sun) {
				a_frame = boards->sun->sceneFrame;
				return *boards->sun;
			}
			return SunBoard();
		}
	}

	void SunCamera(Api::ShadowSourceCamera& a_camera)
	{
		std::uint32_t frame = a_camera.sceneFrame;
		const auto& board = ReadBoard(frame);
		a_camera.camera = board.CascadeOf(a_camera.descriptor, frame);
	}

	void SampleSunCascades(Api::SunCascadeSink& a_sink)
	{
		// Every frame's cascades, whether or not the exclusion ran. Without them every pass with the sun's bits would draw unshadowed.
		// None when UpdateCamera did not run for the sun, as in an interior, where no pass has the sun's bits either.
		std::uint32_t frame = Scene::FrameView::Get().SceneStep();
		const auto& views = ReadBoard(frame);
		if (!views.valid || views.sceneFrame != frame)
			return;
		for (std::uint32_t d = 0; d < views.count && d < views.cascades.size(); ++d)
			if (const auto* cascade = views.CascadeOf(d, frame))
				a_sink.Put(*cascade);
	}

	bool SampleSunEntryPlanes(Api::SunEntryPlanesSink& a_sink, std::uint32_t a_frame)
	{
		// a_frame's planes at the shadow epoch, or the frame before's for the build ahead, posted before this frame's UpdateCamera.
		const auto& views = ReadBoard(a_frame);
		if (!views.valid || (views.sceneFrame != a_frame && views.sceneFrame + 1 != a_frame) || views.fullFrusta.empty())
			return false;
		for (const auto& planes : views.fullFrusta)
			a_sink.Put(planes);
		return true;
	}

	struct SunViews::Hooks
	{
		/** @brief BSShadowDirectionalLight::UpdateCamera (vtable slot 0x10, 0x141512230): DCLF's views from what it set up. */
		struct UpdateCamera
		{
			static bool thunk(RE::BSShadowDirectionalLight* a_light, RE::NiCamera* a_camera)
			{
				const bool result = func(a_light, a_camera);
				SunViews::Get().Update(a_light);
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunViews::Install()
	{
		if (installed)
			return;
		stl::write_vfunc<0x10, Hooks::UpdateCamera>(RE::VTABLE_BSShadowDirectionalLight[0]);
		installed = true;
		logger::info("[DCLF] sun views: BSShadowDirectionalLight::UpdateCamera hooked (T2a)");
	}
}
