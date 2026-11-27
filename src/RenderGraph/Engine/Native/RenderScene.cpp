// Native's world frame skeleton driven by CS. See RenderScene.h.
// Main::RenderScene (AE 1.6.1170 0x140644360, SE 1.5.97 0x1405b1710): the TLS task tag and a flag around the call, the reflections'
// camera (415299), the clear colour, BSShaderManager::State's and the first-person accumulator's eye, AE 107122 (the first-person
// camera at DrawWorld's eye), the main menu test, then Main::RenderPlayerView (AE 0x1406444b0, SE 0x1405b1860), whose calls the driver
// follows one by one (offsets below, SE then AE).

#include "RenderGraph/Engine/Native/RenderScene.h"

#include "EngineZones.h"
#include "Features/DrawcallLimitFix.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/LightSet.h"
#include "Features/LightLimitFix.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Fatal.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"
#include "Utils/Logging.h"

#include "Globals.h"
#include "State.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace RenderGraph::Engine::Native::Scene
{
	namespace
	{
		bool installed = false;

		// NiAVObject::flags (+0xF4) bit 0: the object is hidden. NiAVObject::world.translate is +0xA0.
		constexpr std::size_t kObjectFlags = 0xF4;
		constexpr std::uint32_t kHidden = 1u;
		constexpr std::size_t kWorldTranslate = 0xA0;
		constexpr std::size_t kCameraFrustum = 0x150;  // NiCamera::viewFrustum

		template <class T>
		T& Global(std::uint64_t a_se, std::uint64_t a_ae)
		{
			return *reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address());
		}

		template <class T>
		T& Field(void* a_object, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_object) + a_offset);
		}

		std::uint32_t& Flags(RE::NiAVObject* a_object) { return Field<std::uint32_t>(a_object, kObjectFlags); }

		std::uintptr_t PlayerView() { return REL::RelocationID(35560, 36559).address(); }

		/**
		* @brief A call of Main::RenderPlayerView (SE offset, AE offset): the function its CALL leads to, past the engine-render detector's
		* stub. Each is CS's hook chain in native, which enters no engine function. A point still leading to the engine's own function (a
		* feature that did not hook it) is logged once: the engine's function then runs, as it did inside RenderPlayerView.
		*/
		template <class F>
		F Point(std::uint32_t a_se, std::uint32_t a_ae, const char* a_what)
		{
			const auto target = EngineZones::CallTarget(PlayerView() + REL::Relocate(a_se, a_ae));
			const auto text = REL::Module::get().segment(REL::Segment::textx);
			if (target >= text.address() && target < text.address() + text.size()) {
				static Util::ReasonLog log("[ORG native] skeleton: a point of Main::RenderPlayerView is not hooked, so the engine's function runs");
				log.Report(a_what);
			}
			return reinterpret_cast<F>(target);
		}

		/** @brief Fatal unless a_site is a CALL rel32 to a_target. */
		void RequireCall(const char* a_what, std::uintptr_t a_site, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			if (bytes[0] != 0xE8 || a_site + 5 + relative != a_target)
				RenderGraph::Fatal("native frame skeleton", fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
		}

		/** @brief A clear the skeleton did not take: nothing else makes it in native. Logged once per what. */
		void Untaken(bool a_taken, const char* a_what)
		{
			if (a_taken)
				return;
			static Util::ReasonLog log("[ORG native] skeleton: a clear before RenderWorld was not taken by the skeleton; it is not made");
			log.Report(a_what);
		}

		/** @brief The game-side reads RenderScene and RenderPlayerView decide their branches with. Main thread. */
		struct Branches
		{
			RE::NiAVObject* firstPersonRoot = nullptr;
			bool firstPerson = false;    // the first-person view is drawn
			bool controlDriven = false;  // the movement controller's vfunc +0x78: RenderDepth's and RenderFirstPersonView's second argument
			bool menuMode = false;       // UI +0x160 above 0: RenderWorld's and RenderFirstPersonView's first argument
		};

		Branches ReadBranches(RE::PlayerCharacter& a_player, const Snapshot::FrameSnapshot& a_snapshot)
		{
			Branches b;
			auto* ui = RE::UI::GetSingleton();
			const auto* strings = RE::InterfaceStrings::GetSingleton();
			// RenderScene's test (AE 82074): the main menu is open.
			const bool mainMenu = ui && strings && ui->IsMenuOpen(strings->mainMenu.c_str());
			b.firstPersonRoot = a_player.Get3D1(true);
			// RenderPlayerView's: the first-person root shown, no main menu, and the camera not in its free state. The root's bit comes
			// from the snapshot, taken before the water update that native runs in the capture (it hides the root in first person).
			const auto* camera = RE::PlayerCamera::GetSingleton();
			const bool freeCamera = camera && camera->currentState.get() == camera->GetRuntimeData().cameraStates[RE::CameraStates::kFree].get();
			b.firstPerson = b.firstPersonRoot && a_snapshot.firstPersonShown && !mainMenu && !freeCamera;
			// Actor::GetMovementController (SE 36776, AE 37792), then the controller's vfunc +0x78.
			using GetController = void** (*)(RE::Actor*);
			static REL::Relocation<GetController> getController{ REL::RelocationID(36776, 37792) };
			if (void** controller = getController(&a_player); controller && *controller) {
				using Driven = bool (*)(void*);
				b.controlDriven = (*reinterpret_cast<Driven**>(*controller))[0x78 / sizeof(void*)](*controller);
			}
			b.menuMode = ui && Field<std::int32_t>(ui, 0x160) != 0;
			return b;
		}

		/**
		* @brief RenderPlayerView's start: the list processes take the world camera and its frustum. Game side: the queued lights' walk
		* (ProcessAllQueuedLights) tests each light against the first process's planes and camera, and DCLF's primary cull and the
		* shadow casters' AfterFullFrustum point read them. Their resets (AE 76557) are not made: nothing fills the processes in native,
		* since the list culls do not run.
		*/
		void SetListProcessCameras(RE::NiCamera* a_camera)
		{
			const auto count = Global<std::uint32_t>(528071, 415016);
			auto** processes = Global<std::byte**>(528077, 415022);
			using SetFrustum = void (*)(void*, const void*);
			static REL::Relocation<SetFrustum> setFrustum{ REL::RelocationID(69699, 71081) };
			for (std::uint32_t i = 0; processes && i < count; ++i) {
				Field<RE::NiCamera*>(processes[i], 0x18) = a_camera;
				setFrustum(processes[i], reinterpret_cast<const std::byte*>(a_camera) + kCameraFrustum);
			}
		}

		/** @brief ShadowSceneNode::ProcessAllQueuedLights (SE 99703, AE 106337) with the first-person root shown, as RenderPlayerView makes it. */
		void ProcessQueuedLights(RE::NiAVObject* a_firstPersonRoot)
		{
			auto** processes = Global<void**>(528077, 415022);
			using ProcessAll = void (*)(void*);
			static REL::Relocation<ProcessAll> processAll{ REL::RelocationID(99703, 106337) };
			if (a_firstPersonRoot)
				Flags(a_firstPersonRoot) &= ~kHidden;
			processAll(processes ? processes[0] : nullptr);
			if (a_firstPersonRoot)
				Flags(a_firstPersonRoot) |= kHidden;
		}

		/**
		* @brief The first-person root shift (decision 70's game half): the player's first-person nodes and magic light moved by the
		* negated root position (SE 39494, AE 40573), then the first-person camera and node shift (SE 100405, AE 107123) before the
		* world's passes, or the shift back (SE 100406, AE 107124) after the first-person view. DCLF reads the first-person camera
		* 107123 places (415007), and the depth of field its near and far.
		*/
		void ShiftFirstPerson(RE::PlayerCharacter& a_player, bool a_back)
		{
			auto* root = a_player.Get3D1(true);
			if (!root)
				return;
			const auto& at = Field<const RE::NiPoint3>(root, kWorldTranslate);
			const RE::NiPoint3 negated{ -at.x, -at.y, -at.z };
			using ShiftPlayer = void (*)(RE::PlayerCharacter*, const RE::NiPoint3*);
			static REL::Relocation<ShiftPlayer> shiftPlayer{ REL::RelocationID(39494, 40573) };
			shiftPlayer(&a_player, &negated);
			using Shift = void (*)();
			static REL::Relocation<Shift> shift{ REL::RelocationID(100405, 107123) };
			static REL::Relocation<Shift> shiftBack{ REL::RelocationID(100406, 107124) };
			(a_back ? shiftBack : shift)();
		}

		/**
		* @brief The sky colours RenderPlayerView copies into renderer objects: the sky's two colours (Sky +0xA8, +0xFC) and the sun light's
		* (Sky +0x80 -> +0x40 -> +0x128). DCLF's material port reads the lighting shader's (415094 +0xD0, +0xE0), the native SAO the SAO
		* helper's (ImageSpaceManager +0x1C8 +0x58, +0x64), the snow pass its helper's (+0x1E8 +0x0C, +0x18).
		*/
		void CopySkyColours()
		{
			auto* tes = RE::TES::GetSingleton();
			auto* sky = tes ? Field<std::byte*>(tes, 0x100) : nullptr;
			auto* lighting = Global<std::byte*>(528150, 415094);
			auto* manager = Global<std::byte*>(527731, 414660);
			if (!sky || !lighting || !manager)
				return;
			const auto& first = Field<const std::array<float, 3>>(sky, 0xA8);
			const auto& second = Field<const std::array<float, 3>>(sky, 0xFC);
			auto* sunObject = Field<std::byte*>(sky, 0x80);
			auto* sun = sunObject ? Field<std::byte*>(sunObject, 0x40) : nullptr;
			if (!sun)
				return;
			const auto& sunColour = Field<const std::array<float, 3>>(sun, 0x128);
			Field<std::array<float, 4>>(lighting, 0xD0) = { first[0], first[1], first[2], 1.0f };
			Field<std::array<float, 4>>(lighting, 0xE0) = { sunColour[0], sunColour[1], sunColour[2], 1.0f };
			auto* snow = Field<std::byte*>(manager, 0x1E8);
			Field<std::array<float, 3>>(snow, 0x18) = second;
			Field<std::array<float, 3>>(snow, 0x0C) = first;
			auto* sao = Field<std::byte*>(manager, 0x1C8);
			Field<std::array<float, 3>>(sao, 0x64) = second;
			Field<std::array<float, 3>>(sao, 0x58) = first;
		}

		/**
		* @brief The clears RenderPlayerView makes before RenderWorld, through the skeleton as Renderer::ClearColor's detour takes them:
		* the normals target to (c, c, 0, 0), kMAIN to RenderScene's clear colour (alpha 1), and with improved snow the snow target (0x70)
		* to zero. The values are the game half's.
		*/
		void ClearWorldTargets(const Prepared& a_frame)
		{
			const float normals[4]{ a_frame.normalsClear, a_frame.normalsClear, 0.0f, 0.0f };
			Untaken(Skeleton::ClearColour(static_cast<RE::RENDER_TARGET>(a_frame.normalsTarget), normals), "the normals target");
			const float main[4]{ a_frame.mainClear[0], a_frame.mainClear[1], a_frame.mainClear[2], 1.0f };
			Untaken(Skeleton::ClearColour(RE::RENDER_TARGET::kMAIN, main), "kMAIN");
			if (a_frame.snowClear) {
				constexpr float zero[4]{};
				Untaken(Skeleton::ClearColour(static_cast<RE::RENDER_TARGET>(0x70), zero), "the snow target");
			}
		}

		/** @brief The first-person camera (415007) as the shift placed it, with Main::DrawWorld's eye (415027, world space). */
		Prepared::FirstPersonCamera TakeFirstPersonCamera()
		{
			Prepared::FirstPersonCamera taken;
			auto* camera = Global<RE::NiCamera*>(528062, 415007);
			if (!camera)
				return taken;
			taken.valid = true;
			taken.world = camera->world;
			const auto& data = camera->GetRuntimeData2();
			taken.frustum = data.viewFrustum;
			std::memcpy(taken.port.data(), &data.port, sizeof(taken.port));  // NiRect<float>'s members are protected
			taken.eye = Global<const std::array<float, 3>>(528082, 415027);
			return taken;
		}

		// The game half's values the render half is recording from (RecordFrame), null outside it. Only the render half reads it.
		const Prepared* g_rendering = nullptr;

		/**
		* @brief The driver's game half (decision 8), on the game thread, in the engine's order: the reflections' camera, the branches, the
		* list processes' camera, the queued lights and the shadow casters with the first-person root and the sky node hidden, the
		* first-person shift with its camera taken, the sky colours, the first-person port, the shift back, and Main::Draw's tail. The shadow
		* casters' point is game-side work (the selection, the light cameras and the maps' bookkeeping) and hands the render half the light
		* set. SetupSunAndSky still runs here because its game work and its recording are not split yet (DCLF's frame start, which takes the
		* first-person rows before the shift, and the water render point). The other points are the render half's.
		*/
		Prepared PrepareFrame(RE::Main* a_main, RE::PlayerCharacter& a_player, RE::NiCamera* a_camera, const Snapshot::FrameSnapshot& a_snapshot)
		{
			ZoneScopedN("CS.Native.RenderScene.Game");
			globals::features::drawcallLimitFix.OnNativeGameHalfStart();
			Prepared p;
			p.valid = true;
			// The reflections' camera (SE 528353, AE 415299): the radial blur modifier's projection reads it on the game side (AE 107753).
			Global<RE::NiCamera*>(528353, 415299) = a_camera;
			const auto b = ReadBranches(a_player, a_snapshot);
			p.firstPerson = b.firstPerson;
			p.controlDriven = b.controlDriven;
			p.menuMode = b.menuMode;
			SetListProcessCameras(a_camera);
			// +0xD3, Main::SetupSunAndSky: DCLF's frame start and the water render point. Its result (the capture's) holds the node the
			// shadow casters skip in first person.
			const auto sky = Point<std::uintptr_t (*)(RE::Main*)>(0xD3, 0xD3, "SetupSunAndSky")(a_main);
			if (b.firstPersonRoot)
				Flags(b.firstPersonRoot) |= kHidden;
			ProcessQueuedLights(b.firstPersonRoot);
			// +0x1C4, the shadow casters, with the sky result's node hidden in first person.
			RE::NiAVObject* skipped = b.firstPerson && sky ? Field<RE::NiAVObject*>(reinterpret_cast<void*>(sky), 0x18) : nullptr;
			const bool skippedHidden = skipped && (Flags(skipped) & kHidden);
			if (skipped)
				Flags(skipped) |= kHidden;
			Point<void (*)(RE::NiCamera*)>(0x1C4, 0x1C4, "CalculateAndDrawShadowCasterLights")(a_camera);
			if (skipped && !skippedHidden)
				Flags(skipped) &= ~kHidden;
			p.shadowLights = DCLF::Features::Shadows::Native::TakeSet();
			// Light Limit Fix's scene lights, after the queued lights (added and removed above) and the casters' mask channels.
			if (globals::features::lightLimitFix.loaded)
				p.pointLights = globals::features::lightLimitFix.CaptureLights();
			// The first-person depth epoch takes the camera the shift placed, before the port below (as RenderDepth saw it).
			if (b.firstPerson) {
				ShiftFirstPerson(a_player, false);
				p.firstPersonCamera = TakeFirstPersonCamera();
			}
			CopySkyColours();
			// The first-person camera's port, when 411491 is set (SE 525010; the constant SE 259744, AE 207173).
			if (Global<const bool>(525010, 411491))
				Field<std::array<float, 4>>(Global<RE::NiCamera*>(528062, 415007), 0x174) = Global<const std::array<float, 4>>(259744, 207173);
			p.normalsClear = Global<const float>(230690, 187405);
			p.normalsTarget = WorldPass::EngineNormalsTarget();
			p.mainClear = Global<const std::array<float, 3>>(527696, 414618);
			p.snowClear = Global<const bool>(513332, 391105);
			if (b.firstPerson) {
				Flags(b.firstPersonRoot) &= ~kHidden;
				ShiftFirstPerson(a_player, true);
			}
			// The end-of-frame ImageSpace (SE 527654 / AE 414573 and gEnableEOFImageSpace SE 513299 / AE 391055).
			p.endOfFrame = Global<const bool>(527654, 414573) && Global<const bool>(513299, 391055);
			// The compute SAO when its helper's flag is set; the screen splatter when ScreenSplatter's root has children.
			auto* sao = Field<std::byte*>(Global<std::byte*>(527731, 414660), 0x1D0);
			p.saoHelper = sao;
			p.saoCompute = sao && Field<bool>(sao, 0x20);
			auto* splatter = RE::ScreenSplatter::GetSingleton();
			auto* splatterRoot = splatter ? Field<std::byte*>(splatter, 0x8) : nullptr;
			p.splatter = splatterRoot && Field<std::uint16_t>(splatterRoot, 0x124) != 0;
			if (auto* dofCamera = Global<RE::NiCamera*>(528062, 415007)) {
				const auto& frustum = dofCamera->GetRuntimeData2().viewFrustum;
				p.dofCamera = true;
				p.dofNear = frustum.fNear;
				p.dofFar = frustum.fFar;
			}
			// DCLF confirms the late poses with the scene graph as the render half sees it.
			globals::features::drawcallLimitFix.OnNativeGameHalfEnd();
			// +0xA78, Main::Draw's tail (SE 100425, AE 107143): the sun's occlusion query flag, the engine's per-frame releases (an
			// accumulator's pass list, a frozen ImageSpace texture, the batch renderer's last state), and the fade enable count (S5, CS's).
			// Nothing in a native world frame fills what it releases, so it ends the game half.
			using Tail = void (*)();
			static REL::Relocation<Tail> tail{ REL::RelocationID(100425, 107143) };
			tail();
			return p;
		}

		/**
		* @brief The driver's render half: CS's points from the shadow maps on, through their call sites' hook chains, with the game half's
		* values. a_camera only goes to the engine's RenderSaoCs, whose dispatches ImageSpace takes as steps.
		*/
		void RecordFrame(const Prepared& a_frame, RE::NiCamera* a_camera, std::uint32_t a_target)
		{
			ZoneScopedN("CS.Native.RenderScene.Render");
			const Snapshot::EngineReads::RenderHalfScope half(Snapshot::EngineReads::Half::Record);
			// The points below read a_frame through Current().
			struct Rendering
			{
				explicit Rendering(const Prepared& a_frame) { g_rendering = &a_frame; }
				~Rendering() { g_rendering = nullptr; }
				Rendering(const Rendering&) = delete;
				Rendering& operator=(const Rendering&) = delete;
			} rendering(a_frame);
			// The shadow maps, masks and lens flares draw from the game half's light set.
			DCLF::Features::Shadows::Native::AdoptSet(a_frame.shadowLights);
			// +0x2EC, the shadow maps.
			Point<void (*)()>(0x2EC, 0x2EC, "RenderShadowMaps")();
			// +0x395, +0x39C, +0x3A1: the depth pass, the masks, the precipitation.
			Point<void (*)(bool, bool)>(0x395, 0x395, "RenderDepth")(a_frame.firstPerson, a_frame.controlDriven);
			Point<void (*)(bool)>(0x39C, 0x39C, "RenderShadowMasks")(true);
			Point<void (*)()>(0x3A1, 0x3A1, "RenderPrecipitation")();
			ClearWorldTargets(a_frame);
			// +0x841, RenderWorld.
			Point<void (*)(bool)>(0x831, 0x841, "RenderWorld")(a_frame.menuMode);
			if (a_frame.firstPerson) {
				// +0x8CE, Main::ClearMasks: DCLF's sun mask hook and the first-person hook (which makes no engine call in native).
				Point<void (*)()>(0x8BE, 0x8CE, "ClearMasks")();
				// +0x954, the first-person view.
				Point<void (*)(bool, bool)>(0x944, 0x954, "RenderFirstPersonView")(a_frame.menuMode, a_frame.controlDriven);
			}
			if (!a_frame.endOfFrame)
				return;
			// +0xA4C, the compute SAO: the engine's function, whose dispatches ImageSpace takes as steps.
			if (a_frame.saoCompute)
				Point<void (*)(void*, RE::NiCamera*)>(0xA3E, 0xA4C, "RenderSaoCs")(a_frame.saoHelper, a_camera);
			// +0xA54, RenderWorldPostProcess: the post chain's detour runs CS's native body.
			using PostProcess = void (*)(std::uint32_t);
			static REL::Relocation<PostProcess> postProcess{ REL::RelocationID(100430, 107148) };
			postProcess(a_target);
			// +0xA73, the screen splatter.
			if (a_frame.splatter)
				Point<void (*)()>(0xA6A, 0xA73, "RenderScreenSplatter")();
		}

		/** @brief Main::RenderScene and Main::RenderPlayerView in native: see RenderScene.h. Main thread, inside Main::DrawFrame. */
		void RenderScene(RE::Main* a_main, std::uint32_t a_target)
		{
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* camera = RE::Main::WorldRootCamera();
			if (!player || !camera)
				RenderGraph::Fatal("native frame skeleton", "Main::DrawFrame's world branch ran without the player or the world camera");
			// The game half reads the update's snapshot, not the render side's adopted one.
			const auto& snapshot = Snapshot::Building();
			if (!snapshot)
				RenderGraph::Fatal("native frame skeleton", "Main::DrawFrame's world branch ran before any game update's snapshot");
			ZoneScopedN("CS.Native.RenderScene");
			globals::state->BeginPerfEvent("Player View");
			const auto prepared = PrepareFrame(a_main, *player, camera, *snapshot);
			RecordFrame(prepared, camera, a_target);
			globals::state->EndPerfEvent();
		}

		/** @brief Main::DrawFrame's world call of Main::RenderScene (SE 35556 +0x2DC, AE 36555 +0x4C8). */
		struct DrawFrame_RenderScene
		{
			static void thunk(RE::Main* a_main, std::uint32_t a_target)
			{
				if (Native::Active())
					RenderScene(a_main, a_target);
				else
					func(a_main, a_target);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !Native::Requested())
			return;
		const auto site = REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2DC, 0x4C8);
		RequireCall("Main::DrawFrame's world call of Main::RenderScene", site, REL::RelocationID(35559, 36558).address());
		stl::write_thunk_call<DrawFrame_RenderScene>(site);
		installed = true;
		logger::info("[ORG native] frame skeleton: Main::DrawFrame's world branch runs CS's driver; Main::RenderScene and Main::RenderPlayerView are not entered");
	}

	bool Owns()
	{
		return installed;
	}

	const Prepared& Current()
	{
		static const Prepared none{};
		return g_rendering ? *g_rendering : none;
	}
}
