// The CalculateActiveShadowCasterLights detour and DCLF's port of its light selection. Render thread.

#include "Features/DrawcallLimitFix/Features/LocalLights/LightSelection.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContextAtlas.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"

#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"

#include "State.h"

namespace DCLF
{
	namespace
	{
		using Engine::At;
		using Engine::Global;

		constexpr Engine::Offset kCalculateActive{ { 107137 }, { 100419 } };  // CalculateActiveShadowCasterLights
		constexpr std::size_t kPortalEntry = 0x30190;           // A culling process's portal-graph entry.
		constexpr std::size_t kShadowMapCount = 0x140;          // BSShadowLight's descriptors in use.
		constexpr std::uint32_t kMaxLights = 4;

		// Port of vfunc 0x10 (BSShadowFrustumLight 0x14151ac50, BSShadowParabolicLight 0x14151b620). Directional lights always pass.
		bool Visible(const RE::BSShadowLight& a_light, const RE::NiCamera& a_camera, const ShadowCameras::Planes& a_cameraPlanes)
		{
			auto& light = const_cast<RE::BSShadowLight&>(a_light);
			const auto* niLight = light.light.get();
			if (light.GetIsDirectionalLight())
				return true;
			if (!niLight || (niLight->GetFlags().underlying() & 1))
				return false;
			const bool frustum = light.GetIsFrustumLight();
			if (!frustum && !light.GetIsParabolicLight())
				return false;
			const auto& position = niLight->world.translate;
			const float radius = const_cast<RE::NiLight*>(niLight)->GetLightRuntimeData().radius.x;
			bool visible;
			if (frustum) {
				const auto& descriptors = light.GetRuntimeData().shadowmapDescriptors;
				const auto* camera = descriptors.empty() ? nullptr : descriptors[0].camera.get();
				visible = camera && ShadowCameras::FrustaIntersect(a_camera, *camera);
			} else {
				const float centre[3] = { position.x, position.y, position.z };
				visible = ShadowCameras::SphereInside(centre, radius, a_cameraPlanes);
			}
			if (visible && light.lodFade && ShadowCameras::LodFaded(a_camera, position, radius, frustum))
				visible = false;
			return visible;
		}

		RE::ShadowSceneNode* SceneNode() { return Global<RE::ShadowSceneNode*>(Engine::Address::kShadowSceneNode); }
		const RE::NiCamera* WorldCamera()
		{
			const auto* graph = Global<const std::byte*>(Engine::Address::kWorldSceneGraph);
			return graph ? At<const RE::NiCamera*>(graph, 0x128) : nullptr;
		}
	}

	LightSelection& LightSelection::Get()
	{
		static LightSelection instance;
		return instance;
	}

	bool LightSelection::SharesRoomWithCamera(const RE::BSShadowLight* a_light)
	{
		// The first check is the engine's own. Then compare the light's rooms from this frame's walk with the camera's.
		auto* light = const_cast<RE::BSShadowLight*>(a_light);
		const auto& descriptors = light->GetRuntimeData().shadowmapDescriptors;
		const auto* process = descriptors.empty() ? nullptr : descriptors[0].cullingProcess;
		if (!process || !At<const void*>(process, kPortalEntry))
			return false;
		auto& portals = PortalViews::Get();
		auto& rooms = lightRooms[a_light];
		portals.WalkLight(a_light, rooms);
		const bool shares = portals.SharesRoom(rooms);
		roomVerdicts[a_light] = shares;
		return shares;
	}

	const LightSelection::Frame* LightSelection::Current(std::uint32_t a_sceneFrame) const
	{
		if (const auto* boards = Features::Shadows::Native::AdoptedBoards())
			return static_cast<const Frame*>(boards->selection.get());
		return frame.valid && frame.sceneFrame == a_sceneFrame ? &frame : nullptr;
	}

	void LightSelection::SelectNative()
	{
		Select(true);
	}

	void LightSelection::Select(bool a_native)
	{
		// Invalidate first, so an early return leaves no light views this frame.
		ShadowViews::LightSlots().Invalidate();
		const std::uint32_t sceneFrame = Scene::FrameView::Get().SceneStep();
		std::vector<FrameLight> frameLights = frame.sceneFrame == sceneFrame ? std::move(frame.frameLights) : std::vector<FrameLight>{};
		const std::uint32_t selections = frame.sceneFrame == sceneFrame ? frame.selections + 1 : 1;
		frame = {};
		frame.sceneFrame = sceneFrame;
		frame.selections = selections;
		frame.frameLights = std::move(frameLights);
		auto* node = SceneNode();
		const auto* camera = WorldCamera();
		if (!node || !camera)
			return;
		auto& runtime = node->GetRuntimeData();
		std::uint32_t kept = 0, slot = 0, mask = 0;
		// focusFound mirrors the engine's 0x14338c912.
		const bool focusOn = Global<std::uint8_t>(Engine::Address::kFocusShadows) != 0 && Global<std::uint32_t>(Engine::Address::kFocusTargetCount) != 0;
		bool focusFound = false;
		if (!Global<std::uint8_t>(Engine::Address::kSunOff) && runtime.sunShadowDirLight) {
			frame.sun = runtime.sunShadowDirLight;
			frame.sunSlots = At<std::uint32_t>(frame.sun, kShadowMapCount);
			frame.slots.assign(frame.sunSlots, frame.sun);
			kept = 1;
			slot = frame.sunSlots;
			mask = 1;
			focusFlags[frame.sun] = focusOn;
			if (focusOn) {
				focusFound = true;
				lastFocusHost = nullptr;
			}
		}
		// Last frame's host keeps the role while it is a candidate. This copies the engine's loop exactly, odd as it is.
		if (focusOn && !focusFound && !runtime.activeShadowLights.empty()) {
			for (const auto& pointer : runtime.activeShadowLights) {
				if (const auto* light = pointer.get()) {
					focusFound = light == lastFocusHost && !focusFound;
					focusFlags[light] = focusFound;
					if (a_native)
						const_cast<RE::BSShadowLight*>(light)->GetRuntimeData().drawFocusShadows = focusFound;
				}
			}
		}
		lastFocusHost = nullptr;
		roomVerdicts.clear();
		ShadowCameras::Planes cameraPlanes;
		ShadowCameras::FrustumPlanes(const_cast<RE::NiCamera*>(camera)->GetRuntimeData2().viewFrustum, camera->world, cameraPlanes);
		for (const auto& pointer : runtime.activeShadowLights) {
			const auto* light = pointer.get();
			if (!light)
				continue;
			// Native mode does the engine's UpdateCamera itself (its result is the visibility) and releases a light it does not keep.
			const bool keep = kept < kMaxLights && (a_native ? Features::Shadows::Native::UpdateLightCamera(*const_cast<RE::BSShadowLight*>(light), *camera) : Visible(*light, *camera, cameraPlanes)) &&
			                  SharesRoomWithCamera(light);
			if (!keep) {
				if (a_native)
					Features::Shadows::Native::ReleaseLight(*const_cast<RE::BSShadowLight*>(light));
				continue;
			}
			// The engine walks a kept light here (FUN_1414a2530) and leaves the walk's edge flags on the shapes.
			PortalViews::Get().CommitLightWalk();
			const auto& rooms = lightRooms.at(light);
			frame.locals.push_back({ light, slot, mask, rooms.context, ~0u, rooms.walk });
			// Without a host yet, the first directional or spot light takes the role.
			{
				auto& flag = focusFlags[light];
				auto* candidate = const_cast<RE::BSShadowLight*>(light);
				if (flag || (!focusFound && (candidate->GetIsDirectionalLight() || candidate->GetIsFrustumLight()))) {
					flag = true;
					focusFound = true;
					lastFocusHost = light;
					if (a_native)
						Features::Shadows::Native::FocusHost(*candidate, *camera);
				}
			}
			if (a_native)
				Features::Shadows::Native::KeepLocal(*const_cast<RE::BSShadowLight*>(light), *camera, kept);
			// The engine writes the light into one slot but its Accumulate takes one per hemisphere or cascade.
			// The extra slots keep stale contents, so they are null here.
			auto* shadowLight = const_cast<RE::BSShadowLight*>(light);
			const std::uint32_t maps = At<std::uint32_t>(light, kShadowMapCount);
			const std::uint32_t taken = shadowLight->GetIsParabolicLight() ? (maps == 2 ? 2u : 1u) : shadowLight->GetIsDirectionalLight() ? maps :
			                                                                                                                                1u;
			frame.slots.push_back(light);
			frame.slots.resize(frame.slots.size() + (taken ? taken - 1 : 0), nullptr);
			slot += taken;
			++mask;
			++kept;
		}
		frame.focusCount = std::min<std::uint32_t>(Global<std::uint32_t>(Engine::Address::kFocusTargetCount), 4u);
		if (frame.sun && focusFlags[frame.sun])
			frame.focusHosts.push_back(frame.sun);
		for (const auto& selected : frame.locals)
			if (selected.light != frame.sun && focusFlags[selected.light])
				frame.focusHosts.push_back(selected.light);
		// Prune lights that are gone, because a freed light's address may be reused.
		if (focusFlags.size() > 64) {
			std::unordered_map<const RE::BSShadowLight*, bool> live;
			for (const auto& pointer : runtime.activeShadowLights)
				if (const auto it = focusFlags.find(pointer.get()); it != focusFlags.end())
					live.insert(*it);
			if (frame.sun)
				live.insert_or_assign(frame.sun, focusFlags[frame.sun]);
			focusFlags = std::move(live);
		}
		if (lightRooms.size() > 64) {
			std::unordered_map<const RE::BSShadowLight*, PortalViews::LightRooms> live;
			for (const auto& pointer : runtime.activeShadowLights)
				if (const auto it = lightRooms.find(pointer.get()); it != lightRooms.end())
					live.insert(std::move(*it));
			lightRooms = std::move(live);
		}
		// A light kept again takes this walk's context.
		for (const auto& selected : frame.locals) {
			auto it = std::find_if(frame.frameLights.begin(), frame.frameLights.end(), [&](const auto& a_entry) { return a_entry.light == selected.light; });
			if (it == frame.frameLights.end())
				it = frame.frameLights.insert(frame.frameLights.end(), FrameLight{ selected.light });
			it->portalContext = selected.portalContext;
			it->portalWalk = selected.portalWalk;
		}
		std::vector<Portals::ContextHold> contexts;
		if (auto cameraContext = PortalViews::Get().MainContext(); cameraContext && cameraContext->frame == frame.sceneFrame)
			contexts.push_back(std::move(cameraContext));
		for (auto& entry : frame.frameLights) {
			entry.portalContextIndex = ~0u;
			if (entry.portalContext) {
				entry.portalContextIndex = static_cast<std::uint32_t>(contexts.size());
				contexts.push_back(entry.portalContext);
			}
		}
		for (auto& selected : frame.locals)
			if (const auto* entry = frame.LightOf(selected.light))
				selected.portalContextIndex = entry->portalContextIndex;
		// Changes on every selection, even twice in one frame. Zero is never used.
		static std::uint64_t publication = 0;
		if (++publication == 0)
			++publication;
		// This runs inside the engine's detour, so a failed encode publishes nothing instead of throwing.
		try {
			frame.portalContexts = Portals::EncodeContexts(contexts, publication);
		} catch (const std::exception& e) {
			frame.portalContexts.clear();
			for (auto& selected : frame.locals)
				selected.portalContextIndex = ~0u;
			for (auto& entry : frame.frameLights)
				entry.portalContextIndex = ~0u;
			logger::error("[DCLF][structural-gap] point-light context atlas not encoded at frame {}: {}", frame.sceneFrame, e.what());
		}
		frame.valid = true;
		// The board holds spans into frame, valid until the next Select.
		ShadowViews::LightSlots().Publish(frame.sceneFrame, frame.slots, frame.focusHosts, frame.focusCount);
		TellKeptLights(frame);
	}

	void LightSelection::TellKeptLights(const Frame& a_frame)
	{
		const auto& spot = Extensions::ShadowSourceOf(Api::ShadowSource::Spot);
		const auto& point = Extensions::ShadowSourceOf(Api::ShadowSource::Point);
		for (const auto& selected : a_frame.locals) {
			auto* light = const_cast<RE::BSShadowLight*>(selected.light);
			const auto* source = light->GetIsFrustumLight() ? &spot : light->GetIsParabolicLight() ? &point :
			                                                                                         nullptr;
			if (source && source->lightKept)
				source->lightKept({ selected.light, a_frame.sceneFrame });
		}
	}

	struct LightSelection::Hooks
	{
		struct CalculateActive
		{
			static void thunk()
			{
				auto& selection = LightSelection::Get();
				func();
				selection.Select();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void LightSelection::Install()
	{
		if (installed)
			return;
		if (!kCalculateActive.Known()) {
			logger::warn("[DCLF] light selection: CalculateActiveShadowCasterLights is not known for this runtime; the engine selects the lights");
			return;
		}
		stl::detour_thunk<Hooks::CalculateActive>(REL::Offset(kCalculateActive.Get()).address());
		HookSites::NoteDetour("the light selection's CalculateActiveShadowCasterLights detour", Hooks::CalculateActive::func.address());
		installed = true;
		logger::info("[DCLF] light selection: CalculateActiveShadowCasterLights detoured (T3b)");
	}
}
