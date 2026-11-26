#include "Snapshot/SideEffects.h"

#include "Snapshot/StealthLights.h"

#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"

#include <ORGModuleServices/Cpu/SphereCull.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Snapshot::SideEffects
{
	namespace
	{
		// NiAVObject::flags (+0xF4, the same on SE and AE).
		constexpr std::size_t kObjectFlags = 0xF4;
		constexpr std::uint32_t kFlagPreprocessed = 1u << 12;  // BSFadeNodeCuller::Process1 marks the roots it tested
		constexpr std::uint32_t kFlagNotVisible = 1u << 20;    // the actor pre-cull's "not visible"
		constexpr std::uint32_t kFlagAccumulated = 1u << 26;   // kAccumulated: passed the last cull that reached it
		constexpr std::size_t kCurrentFade = 0x130;            // BSFadeNode::currentFade
		// HighActorCuller::Operate's "fully faded in" threshold (AE 0x1417afdc4, SE 0x14156b7b4: 1e-5).
		constexpr float kFadedIn = 9.99999975e-06f;
		// The pre-cull's bits in the actor (AE +0x27C, SE +0x274): 1 occluder-tested, 2 visible, 4 faded in.
		constexpr std::uint32_t kActorTested = 1u, kActorVisible = 2u, kActorFadedIn = 4u;
		// BSTempEffectSimpleDecal states (BGSDecalManager's update, AE 15220): 3 waits for the test, 4 for its query, 5 places the decal.
		constexpr std::uint32_t kDecalAwaitingTest = 3, kDecalPlace = 5;
		// The fade enable flags come back on after more than this many world frames (Main::Draw's tail, AE 107143, SE 100425).
		constexpr std::int32_t kFadeEnableFrames = 0x14;

		bool installed = false;
		thread_local bool t_renderHalf = false;

		struct Stats
		{
			std::uint64_t updates = 0, skipped = 0, stagesSignalled = 0, engineStagesSkipped = 0, maskClears = 0;
			std::uint32_t maskBefore = 0, maskAfter = 0;  // the last clear's
			std::uint64_t decalsAccepted = 0, decalsStuck = 0, fadeEnables = 0;
			std::uint64_t actors = 0, actorsInView = 0, actorsHidden = 0, actorsFadedIn = 0, actorsNoFadeNode = 0;
			std::uint64_t animatedRoots = 0, animatedByDclf = 0, animatedInView = 0;
		};
		Stats stats;  // game thread

		std::uint32_t& FrameCounter()
		{
			// gFrameCounter, BSGraphics::State+0x4C.
			static REL::Relocation<std::uint32_t*> counter{ REL::RelocationID(525008, 411489) };
			return *counter;
		}

		std::uint32_t& RenderUnsafeMask()
		{
			// The threads that are inside the render half (TaskQueueInterface::ShouldUseTaskQueue reads it).
			static REL::Relocation<std::uint32_t*> mask{ REL::RelocationID(517486, 404075) };
			return *mask;
		}

		std::uint32_t AtomicFlags(void* a_object, std::uint32_t a_set, std::uint32_t a_clear)
		{
			std::atomic_ref<std::uint32_t> flags(*reinterpret_cast<std::uint32_t*>(static_cast<std::byte*>(a_object) + kObjectFlags));
			if (a_set)
				flags.fetch_or(a_set, std::memory_order_relaxed);
			if (a_clear)
				flags.fetch_and(~a_clear, std::memory_order_relaxed);
			return flags.load(std::memory_order_relaxed);
		}

		/** @brief Fatal unless a_site is a CALL rel32 to a_target: the patches below assume the engine's own code. */
		void RequireCall(const char* a_what, std::uintptr_t a_site, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			if (bytes[0] != 0xE8 || a_site + 5 + relative != a_target)
				RenderGraph::Fatal("snapshot side effects", fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
		}

		void RequireBytes(const char* a_what, std::uintptr_t a_site, std::initializer_list<std::uint8_t> a_bytes)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::size_t i = 0;
			for (const auto expected : a_bytes)
				if (bytes[i++] != expected)
					RenderGraph::Fatal("snapshot side effects", fmt::format("{} at {:#x} is not the engine's code; another plugin may have patched it", a_what, a_site));
		}

		/** @brief SetJobTaskEvent (SE 68165, AE 69514). Inside the render half, stages 5 to 9 are left to AfterRenderHalf. */
		struct SetStageEvent
		{
			static void thunk(std::uint32_t a_stage)
			{
				if (t_renderHalf && a_stage >= 5 && a_stage <= 9) {
					++stats.engineStagesSkipped;
					return;
				}
				func(a_stage);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::Draw's clear of the main thread's render-unsafe bit after the world draw (AE 39034 with 0). AfterRenderHalf clears it. */
		struct SkipRenderUnsafeClear
		{
			static void thunk([[maybe_unused]] bool a_set) {}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// The high actor culler object (HighActorCuller, SE 517032, AE 403540). +0x148 is null until its first run initialises it.
		constexpr std::size_t kCullerReady = 0x148;
		constexpr std::size_t kCullerCamera = 0x28;
		constexpr std::size_t kCullerProcess = 0x10;
		constexpr std::size_t kCameraFrustum = 0x150;  // NiCamera::viewFrustum

		std::byte* Culler()
		{
			static REL::Relocation<std::byte*> culler{ REL::RelocationID(517032, 403540) };
			return culler.get();
		}

		/**
		* @brief Main::Draw's camera setup runs the high actor culler (AE 40336, SE 39263): it stores the camera, sets its culling
		* process's frustum, then walks the high actors. AfterCapture does all three, so this runs only the first, initialising call.
		*/
		struct SkipHighActorCuller
		{
			static void thunk(void* a_culler, void* a_camera)
			{
				if (!*reinterpret_cast<void**>(static_cast<std::byte*>(a_culler) + kCullerReady))
					func(a_culler, a_camera);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		* @brief AE 40336's setup: the camera and NiCullingProcess::SetFrustum (SE 69699, AE 71081) with the world camera's frustum.
		* CS reads that frustum's near and far at the next capture (Util::GetCameraData), so it is a side effect too.
		*/
		bool SetCullerFrustum()
		{
			auto* culler = Culler();
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera || !*reinterpret_cast<void**>(culler + kCullerReady))
				return false;
			using SetFrustum = void (*)(void*, const void*);
			static REL::Relocation<SetFrustum> setFrustum{ REL::RelocationID(69699, 71081) };
			*reinterpret_cast<RE::NiCamera**>(culler + kCullerCamera) = camera;
			setFrustum(culler + kCullerProcess, reinterpret_cast<const std::byte*>(camera) + kCameraFrustum);
			return true;
		}

		/**
		* @brief S17 (decision 11). BGSDecalManager's update moved new decals that need an occlusion test to state 3 and queued them
		* in +0x60, which Main::DrawFrame tests (Main::DecalTest, SE 35563, AE 36562). The test only decides placement, so every
		* queued decal is accepted: state 5 with a non-zero count, which the next update places, as a visible answer would.
		*/
		void AcceptQueuedDecals()
		{
			auto* manager = RE::BGSDecalManager::GetSingleton();
			if (!manager)
				return;
			for (auto& decal : manager->simpleDecals) {
				if (!decal || decal->state != kDecalAwaitingTest)
					continue;
				decal->unk40 = 1;
				decal->state = kDecalPlace;
				++stats.decalsAccepted;
			}
			manager->simpleDecals.clear();
			// Nothing should wait for a test in native: a decal in state 3 or 4 here is stuck.
			for (const auto& effect : manager->decals) {
				const auto* simple = effect ? netimmerse_cast<RE::BSTempEffectSimpleDecal*>(effect.get()) : nullptr;
				if (simple && (simple->state == kDecalAwaitingTest || simple->state == kDecalAwaitingTest + 1))
					++stats.decalsStuck;
			}
		}

		/**
		* @brief S5. Main::Draw's tail (AE 107143, SE 100425) turns both fade enable flags back on after 20 world frames with the
		* first clear. A teleport or load (AE 40438) clears it. Counted once per world update here; the engine's count is patched out.
		*/
		void CountFadeEnable(const FrameSnapshot& a_snapshot)
		{
			if (a_snapshot.kind != FrameKind::World)
				return;
			static REL::Relocation<std::uint8_t*> fadeLodUpdates{ REL::RelocationID(513141, 390865) };
			static REL::Relocation<std::uint8_t*> fadesOn{ REL::RelocationID(513142, 390866) };
			static REL::Relocation<std::int32_t*> frames{ REL::RelocationID(527629, 414558) };
			if (*fadeLodUpdates != 0)
				return;
			if (++*frames <= kFadeEnableFrames)
				return;
			*fadeLodUpdates = 1;
			*fadesOn = 1;
			*frames = 0;
			++stats.fadeEnables;
			logger::info("[Snapshot] fade enable flags back on at snapshot {} (counted on the game thread)", a_snapshot.id);
		}

		/** @brief The six planes of the snapshot's main camera, packed for the SIMD sphere cull. False without a valid world view. */
		bool MainPlanes(const FrameSnapshot& a_snapshot, float (&a_active)[6][4], std::uint32_t& a_count)
		{
			const auto& view = a_snapshot.cameras.Current(ViewId::Main);
			a_count = 0;
			if (a_snapshot.kind != FrameKind::World || !view.valid)
				return false;
			RE::NiTransform world;
			world.rotate = view.rotation;
			world.translate = view.position;
			world.scale = 1.0f;
			DCLF::ShadowCameras::Planes planes;
			DCLF::ShadowCameras::FrustumPlanes(view.frustum, world, planes);
			for (std::uint32_t p = 0; p < 6; ++p)
				if ((planes.mask >> p) & 1)
					std::memcpy(a_active[a_count++], planes.plane[p].data(), sizeof(a_active[0]));
			return true;
		}

		/** @brief The SIMD sphere cull (org::cpu) of the spheres against the packed planes. */
		void CullSpheres(const std::vector<float>& a_x, const std::vector<float>& a_y, const std::vector<float>& a_z, const std::vector<float>& a_r,
			const float (&a_planes)[6][4], std::uint32_t a_planeCount, std::vector<std::uint8_t>& a_pass)
		{
			const auto count = static_cast<std::uint32_t>(a_x.size());
			a_pass.assign(count, 0);
			if (!count)
				return;
			const org::cpu::Spheres spheres{ a_x.data(), a_y.data(), a_z.data(), a_r.data() };
			static const auto wide = org::cpu::WideFrustum(org::cpu::Supported());
			if (wide)
				wide(spheres, count, a_planes, a_planeCount, nullptr, a_pass.data());
			else
				org::cpu::FrustumSse(spheres, count, a_planes, a_planeCount, a_pass.data());
		}

		/**
		* @brief S6 (decision 12). HighActorCuller::Operate (AE 40335, SE 39262) for every high actor, with the main camera's frustum
		* tested by the SIMD sphere cull: BSFadeNodeCuller::Process1's six planes on the root's world bound, then the fade threshold.
		* The occluder test (FUN_1414a8ba0) is not repeated: a root in view and faded in counts as tested and not occluded.
		*/
		void DecideActorPreCull(const FrameSnapshot& a_snapshot)
		{
			// bPreCullActors:General gates the engine's culler.
			static REL::Relocation<bool*> preCull{ REL::RelocationID(508891, 380927) };
			auto* lists = RE::ProcessLists::GetSingleton();
			float active[6][4]{};
			std::uint32_t planeCount = 0;
			// Main::Draw runs the culler only when it draws the world.
			if (!*preCull || !lists || !MainPlanes(a_snapshot, active, planeCount) || !SetCullerFrustum())
				return;

			struct Entry
			{
				RE::Actor* actor;
				RE::NiAVObject* root;
				RE::BSFadeNode* fade;
			};
			static std::vector<Entry> entries;
			static std::vector<float> x, y, z, r;
			static std::vector<std::uint8_t> pass;
			entries.clear();
			x.clear();
			y.clear();
			z.clear();
			r.clear();
			for (const auto& handle : lists->highActorHandles) {
				const auto actor = handle.get();
				// The engine's list walk (AE 0x14076bff0) skips references with form flag bit 21.
				if (!actor || ((actor->GetFormFlags() >> 21) & 1))
					continue;
				auto* root = actor->Get3D();
				auto* fade = root ? root->AsFadeNode() : nullptr;
				entries.push_back({ actor.get(), root, fade });
				if (!fade)
					continue;
				x.push_back(fade->worldBound.center.x);
				y.push_back(fade->worldBound.center.y);
				z.push_back(fade->worldBound.center.z);
				r.push_back(fade->worldBound.radius);
			}
			CullSpheres(x, y, z, r, active, planeCount, pass);
			const std::size_t bitsOffset = REL::Relocate(0x274, 0x27C);
			std::uint32_t lane = 0;
			for (const auto& entry : entries) {
				std::uint32_t bits = 0;
				++stats.actors;
				if (entry.root && !entry.fade) {
					AtomicFlags(entry.root, kFlagNotVisible, 0);
					++stats.actorsNoFadeNode;
				} else if (entry.fade) {
					const bool inView = pass[lane++] != 0;
					AtomicFlags(entry.fade, kFlagPreprocessed | (inView ? 0u : kFlagNotVisible), inView ? kFlagNotVisible : 0u);
					const float currentFade = *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(entry.fade) + kCurrentFade);
					bits = kActorVisible | (currentFade >= kFadedIn ? kActorFadedIn : 0u);
					if (inView && (bits & kActorFadedIn))
						bits |= kActorTested;
					stats.actorsInView += inView ? 1 : 0;
					stats.actorsHidden += inView ? 0 : 1;
					stats.actorsFadedIn += (bits & kActorFadedIn) ? 1 : 0;
				}
				std::atomic_ref<std::uint32_t> word(*reinterpret_cast<std::uint32_t*>(reinterpret_cast<std::byte*>(entry.actor) + bitsOffset));
				word.store((word.load(std::memory_order_relaxed) & ~0xFu) | bits, std::memory_order_relaxed);
			}
		}

		/**
		* @brief S7 (decision 13) for the roots DCLF has no verdict for. The animation jobs (UpdateAnimationJob, UpdateManagedNodesJob,
		* UpdateAnimatedRefsJob) ask AE 19384 (SE 18972) whether an animated reference's root passed the last cull (kAccumulated).
		* The roots are the attached cells' animated references (LOADED_CELL_DATA +0x40). Each root DCLF does not list is tested here
		* against the snapshot's main camera; AfterRenderHalf writes the verdicts after the render half's culls, so CS's is the value
		* the next update reads. DCLF writes its own roots' verdicts at Main::Update's entry.
		*/
		struct AnimatedVerdict
		{
			RE::NiPointer<RE::NiAVObject> root;
			bool visible;
		};
		std::vector<AnimatedVerdict> animatedVerdicts;  // game thread, from AfterCapture to AfterRenderHalf

		void DecideAnimatedVisibility(const FrameSnapshot& a_snapshot)
		{
			animatedVerdicts.clear();
			float planes[6][4]{};
			std::uint32_t planeCount = 0;
			auto* tes = RE::TES::GetSingleton();
			if (!tes || !MainPlanes(a_snapshot, planes, planeCount))
				return;
			static std::vector<RE::NiPointer<RE::NiAVObject>> roots;
			static std::vector<float> x, y, z, r;
			static std::vector<std::uint8_t> pass;
			roots.clear();
			x.clear();
			y.clear();
			z.clear();
			r.clear();
			tes->ForEachCell([&](RE::TESObjectCELL* a_cell) {
				auto* loaded = a_cell && a_cell->IsAttached() ? a_cell->GetRuntimeData().loadedData : nullptr;
				if (!loaded)
					return;
				for (const auto& handle : loaded->unk040) {
					const auto reference = handle.get();
					auto* root = reference ? reference->Get3D() : nullptr;
					if (!root)
						continue;
					++stats.animatedRoots;
					bool dclf = false;
					if (DCLF::Extensions::ListedVerdictOf(root, dclf)) {
						++stats.animatedByDclf;
						continue;
					}
					roots.emplace_back(root);
					x.push_back(root->worldBound.center.x);
					y.push_back(root->worldBound.center.y);
					z.push_back(root->worldBound.center.z);
					r.push_back(root->worldBound.radius);
				}
			});
			CullSpheres(x, y, z, r, planes, planeCount, pass);
			animatedVerdicts.reserve(roots.size());
			for (std::size_t i = 0; i < roots.size(); ++i) {
				const bool visible = pass[i] != 0;
				stats.animatedInView += visible ? 1 : 0;
				animatedVerdicts.push_back({ std::move(roots[i]), visible });
			}
		}

		void WriteAnimatedVisibility()
		{
			for (const auto& verdict : animatedVerdicts) {
				AtomicFlags(verdict.root.get(), verdict.visible ? kFlagAccumulated : 0u, verdict.visible ? 0u : kFlagAccumulated);
			}
			animatedVerdicts.clear();
		}
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !RenderGraph::Engine::Native::Requested())
			return;
		// S1: the stage signals inside the render half.
		stl::detour_thunk<SetStageEvent>(REL::RelocationID(68165, 69514));
		if (!SetStageEvent::func.address())
			RenderGraph::Fatal("snapshot side effects", "SetJobTaskEvent could not be detoured");
		// S2: Renderer::End's `inc dword [gFrameCounter]` (6 bytes).
		const auto increment = REL::RelocationID(75461, 77246).address() + REL::Relocate(0x50, 0xBB);
		RequireBytes("Renderer::End's frame counter increment", increment, { 0xFF, 0x05 });
		REL::safe_fill(increment, 0x90, 6);
		// S4: Main::Draw's clear of the render-unsafe bit.
		const auto maskClear = REL::RelocationID(35560, 36559).address() + REL::Relocate(0xA01, 0xA0F);
		RequireCall("Main::Draw's render-unsafe clear", maskClear, REL::RelocationID(38080, 39034).address());
		stl::write_thunk_call<SkipRenderUnsafeClear>(maskClear);
		// S5: Main::Draw's tail counts only while the flag is clear; `jne` over the count becomes `jmp`.
		const auto fadeCount = REL::RelocationID(100425, 107143).address() + 0x3D;
		RequireBytes("the fade enable count's branch", fadeCount, { 0x75 });
		REL::safe_write(fadeCount, std::uint8_t{ 0xEB });
		// S6: the high actor culler in Main::Draw's camera setup.
		const auto culler = REL::RelocationID(35562, 36561).address() + REL::Relocate(0x148, 0x11B);
		RequireCall("Main::Draw's high actor culler", culler, REL::RelocationID(39263, 40336).address());
		stl::write_thunk_call<SkipHighActorCuller>(culler);
		StealthLights::InstallHooks();
		installed = true;
		logger::info(
			"[Snapshot] side effects: render stages 5-9, the frame counter, the render-unsafe bit, the fade enable count, the actor pre-cull "
			"and the decal tests are produced on the game thread (native); the engine's writes are off");
	}

	void AfterCapture(const FrameSnapshot& a_snapshot)
	{
		if (!installed)
			return;
		++stats.updates;
		AcceptQueuedDecals();
		CountFadeEnable(a_snapshot);
		DecideActorPreCull(a_snapshot);
		DecideAnimatedVisibility(a_snapshot);
	}

	void AfterRenderHalf(std::uint32_t a_renders)
	{
		if (!installed)
			return;
		stats.skipped += a_renders == 0 ? 1 : 0;
		// S7: after the render half's culls, before the next update's animation jobs.
		WriteAnimatedVisibility();
		// S4 (decision 14): the main thread's bit, through the engine's own helper (SE 38080, AE 39034).
		using SetRenderUnsafe = void (*)(bool);
		static REL::Relocation<SetRenderUnsafe> setRenderUnsafe{ REL::RelocationID(38080, 39034) };
		stats.maskBefore = RenderUnsafeMask();
		setRenderUnsafe(false);
		stats.maskAfter = RenderUnsafeMask();
		++stats.maskClears;
		// S2: once per game update, where Renderer::End moved it.
		std::atomic_ref<std::uint32_t>(FrameCounter()).fetch_add(1, std::memory_order_relaxed);
		// S1 (decision 9): the stages in the render half's order.
		for (std::uint32_t stage = 5; stage <= 9; ++stage)
			SetStageEvent::func(stage);
		stats.stagesSignalled += 5;
	}

	RenderHalfScope::RenderHalfScope() { t_renderHalf = true; }
	RenderHalfScope::~RenderHalfScope() { t_renderHalf = false; }

	std::string TakeStatsLine()
	{
		if (!installed)
			return {};
		const auto s = std::exchange(stats, {});
		const double perUpdate = s.updates ? 1.0 / static_cast<double>(s.updates) : 0.0;
		return fmt::format(
				   "[Snapshot] side effects over {} updates ({} skipped): stages 5-9 signalled {} times by CS, {} engine signals skipped; frame counter {}; "
				   "render-unsafe bit cleared {} times (last mask {:#x} to {:#x}); decals accepted {} ({} stuck in the test states); fade enables {}; actor pre-cull "
				   "{:.1f} actors an update, {:.1f} in view, {:.1f} not visible, {:.1f} faded in, {:.1f} without a fade node",
				   s.updates, s.skipped, s.stagesSignalled / 5, s.engineStagesSkipped, FrameCounter(), s.maskClears, s.maskBefore, s.maskAfter, s.decalsAccepted,
				   s.decalsStuck, s.fadeEnables, double(s.actors) * perUpdate, double(s.actorsInView) * perUpdate, double(s.actorsHidden) * perUpdate,
				   double(s.actorsFadedIn) * perUpdate, double(s.actorsNoFadeNode) * perUpdate) +
		       fmt::format("; animated roots {:.1f} an update ({:.1f} DCLF's, {:.1f} of the rest in view)",
				   double(s.animatedRoots) * perUpdate, double(s.animatedByDclf) * perUpdate, double(s.animatedInView) * perUpdate) +
		       "; " + StealthLights::TakeStatsLine();
	}
}
