#include "Snapshot/SideEffects.h"

#include "Features/SubsurfaceScattering.h"
#include "Globals.h"
#include "Snapshot/EngineReads.h"
#include "State.h"

#include "Snapshot/FaceTint.h"
#include "Snapshot/StealthLights.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/EngineEvents.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeState.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadeWriters.h"
#include "Features/DrawcallLimitFix/Features/Fades/FadesAddresses.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
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
			std::uint64_t animatedRoots = 0, animatedByDclf = 0, animatedInView = 0, fadesStepped = 0;
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

		/** @brief A short jump from a_from to a_to over engine code, after checking the bytes it replaces. */
		void JumpOver(const char* a_what, std::uintptr_t a_from, std::initializer_list<std::uint8_t> a_bytes, std::uintptr_t a_to)
		{
			RequireBytes(a_what, a_from, a_bytes);
			const auto distance = static_cast<std::intptr_t>(a_to) - static_cast<std::intptr_t>(a_from + 2);
			if (distance < 0 || distance > 0x7F)
				RenderGraph::Fatal("snapshot side effects", fmt::format("{}: no short jump reaches {:#x} from {:#x}", a_what, a_to, a_from));
			const std::uint8_t jump[2]{ 0xEB, static_cast<std::uint8_t>(distance) };
			REL::safe_write(a_from, jump, sizeof(jump));
		}

		/**
		* @brief The main camera's list culls (decisions 12 and 13): Main::Draw (SE 35560, AE 36559) queues DrawWorld_BuildSceneLists and
		* waits for it around Main::RenderWaterEffects, runs Main::FirstPersonCulling after the shadow caster selection, and clears the
		* scene lists in jobs around Main::RenderWorld. Native draws nothing from the lists or the first-person cull, and the culls'
		* game-visible effects are produced here (S7, S8) and by DCLF, so Main::Draw jumps over all of it. The list accumulation jobs
		* are not queued either (NativeCasters.cpp). Offsets are the same on SE and AE up to the last one.
		*/
		void SkipListCulls()
		{
			const auto draw = REL::RelocationID(35560, 36559).address();
			// The lock, QueueJob(BuildSceneLists), unlock and Begin, up to RenderWaterEffects; then its Finish.
			JumpOver("Main::Draw's scene list build", draw + 0x12A, { 0x48, 0x8B, 0x15 }, draw + 0x16F);
			JumpOver("Main::Draw's scene list build's Finish", draw + 0x177, { 0x48, 0x8B, 0x0D }, draw + 0x183);
			// Main::FirstPersonCulling(main, firstPerson) and its argument setup.
			JumpOver("Main::Draw's first-person cull", draw + 0x1EB, { 0x41, 0x0F, 0xB6, 0xD6 }, draw + 0x1F7);
			// The ClearLists job list's creation, its jobs and Begin. R15D is the function's zero register, set inside the block.
			const auto clear = draw + 0x3CB;
			RequireBytes("Main::Draw's scene list clears", clear, { 0x48, 0x8B, 0x05 });
			const auto clearEnd = draw + 0x519;
			std::uint8_t skip[8]{ 0x45, 0x31, 0xFF, 0xE9 };  // xor r15d, r15d; jmp rel32
			const auto relative = static_cast<std::int32_t>(static_cast<std::intptr_t>(clearEnd) - static_cast<std::intptr_t>(clear + sizeof(skip)));
			std::memcpy(skip + 4, &relative, sizeof(relative));
			REL::safe_write(clear, skip, sizeof(skip));
			// The ClearLists Finish after Main::RenderWorld.
			JumpOver("Main::Draw's scene list clears' Finish", draw + REL::Relocate(0x836, 0x846), { 0x48, 0x8B, 0x0D }, draw + REL::Relocate(0x842, 0x852));
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
			std::uint32_t plan;  // the fade plan (FadeState::PlanOf), kFadeRootOther for a root S8 does not step
		};
		std::vector<AnimatedVerdict> animatedVerdicts;  // game thread, from AfterCapture to AfterRenderHalf

		// BSFadeNode fields, the same on SE and AE.
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kLastVisible = 0x13C;  // the fade frame counter an OnVisible last stamped
		constexpr std::size_t kLodBlend = 0x14C;
		constexpr std::size_t kLodLevels = 0x152;
		constexpr std::size_t kLodType = 0x153;
		constexpr std::size_t kTreeLodSwitch = 0x180;  // BSTreeNode: its LOD switch, with a count at +0x12C
		constexpr std::uint32_t kFlagFadedIn = 1u << 14;
		constexpr std::uint32_t kFlagSettled = 1u << 15;
		constexpr std::size_t kCameraLodAdjust = 0x184;

		/** @brief The camera an OnVisible of the main camera's cull reads: the world camera's position and LOD adjust. */
		bool FadeCamera(float (&a_camera)[4])
		{
			const auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return false;
			a_camera[0] = camera->world.translate.x;
			a_camera[1] = camera->world.translate.y;
			a_camera[2] = camera->world.translate.z;
			a_camera[3] = *reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(camera) + kCameraLodAdjust);
			return true;
		}

		/**
		* @brief S8 (decisions 13, 39, 46): what the main camera's cull did to a fade root in view, without the children. BSLeafAnimNode's
		* and BSTreeNode's LOD step (AE 105588, 105604), then BSFadeNode::OnVisible's fade part (SE 98848, AE 105485): the LOD type 6
		* step or the fade state machine (SE 98849, AE 105486), and the +0x13C stamp. The main camera's list processes have
		* cameraRelatedUpdates set and the tree height test off. CPU only: the engine's own fade functions, as AE 19384 calls them.
		*/
		void StepFade(std::byte* a_node, std::uint32_t a_plan, const float (&a_camera)[4])
		{
			using namespace DCLF::Engine;
			namespace FadeAddress = DCLF::Features::Fades::Address;
			const std::int32_t counter = Global<std::int32_t>(FadeAddress::kFadeFrameCounter);
			if ((a_plan == DCLF::kFadeRootLeaf || a_plan == DCLF::kFadeRootTree) && Global<std::uint8_t>(Address::kFadeLodUpdates)) {
				using Distance = float (*)(void*, const float*, std::int32_t*);
				using LevelUpdate = void (*)(void*, std::int32_t);
				std::int32_t level = 0;
				reinterpret_cast<Distance>(REL::Module::get().base() + FadeAddress::kFadeDistance)(a_node, a_camera, &level);
				reinterpret_cast<LevelUpdate>(REL::Module::get().base() + Address::kLodLevelUpdate)(a_node, level);
				At<std::int32_t>(a_node, kLastVisible) = counter;
			}
			[&] {
				if (!Global<std::uint8_t>(Address::kFadesOn))
					return;
				const float one = Global<float>(FadeAddress::kFloatOne);
				const float amount = At<float>(a_node, kFadeAmount);
				auto& current = At<float>(a_node, kCurrentFade);
				if ((At<std::uint32_t>(a_node, kObjectFlags) & kFlagSettled) && one == amount && one == current)
					return;
				if ((At<std::uint8_t>(a_node, kLodType) & 0xF) == 6 && !(Global<float>(Address::kFadeSpecialA) != Global<float>(Address::kFadeSpecialB))) {
					if (!Global<std::uint8_t>(Address::kFadeLodUpdates)) {
						AtomicFlags(a_node, kFlagFadedIn, 0);
						current = 1.0f;
					}
					if (!(At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadedIn)) {
						if (counter - At<std::int32_t>(a_node, kLastVisible) < 2) {
							float step = Global<float>(FadeAddress::kFadeStepTime) / Global<float>(FadeAddress::kFadeStepDivisor);
							if (Global<float>(FadeAddress::kFadeStepMax) <= step)
								step = Global<float>(FadeAddress::kFadeStepMax);
							float fade = step + current;
							if (one <= fade)
								fade = one;
							current = fade;
							if (!(fade < one))
								AtomicFlags(a_node, kFlagFadedIn, 0);
						} else {
							current = 1.0f;
							AtomicFlags(a_node, kFlagFadedIn, 0);
						}
					}
				} else {
					using FadeUpdate = std::uint64_t (*)(void*, float, const float*);
					reinterpret_cast<FadeUpdate>(REL::Module::get().base() + Address::kFadeUpdate)(a_node, amount, a_camera);
				}
				At<std::int32_t>(a_node, kLastVisible) = counter;
			}();
			// BSTreeNode::OnVisible's LOD fix-up, after BSLeafAnimNode's.
			if (a_plan == DCLF::kFadeRootTree)
				if (const auto* lod = At<const std::byte*>(a_node, kTreeLodSwitch); lod && At<std::int32_t>(lod, 0x12C) > 0) {
					At<std::uint8_t>(a_node, kLodType) = static_cast<std::uint8_t>((At<std::uint8_t>(a_node, kLodType) & 0xAF) | 0x20);
					At<float>(a_node, kLodBlend) = 0.0f;
				}
		}

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
				// A root DCLF lists is DCLF's to fade even before its first visibility verdict.
				const std::uint32_t plan = roots[i]->AsFadeNode() && !DCLF::Features::Fades::FadeWrittenByDclf(roots[i].get()) ?
				                               DCLF::FadeState::PlanOf(*roots[i]) :
				                               DCLF::kFadeRootOther;
				animatedVerdicts.push_back({ std::move(roots[i]), visible, plan });
			}
		}

		/** @brief S7, then S8 for the roots in view: once per game update, as the engine's cull ran once per rendered update. */
		void WriteAnimatedVisibility()
		{
			float camera[4]{};
			const bool haveCamera = FadeCamera(camera);
			for (const auto& verdict : animatedVerdicts) {
				AtomicFlags(verdict.root.get(), verdict.visible ? kFlagAccumulated : 0u, verdict.visible ? 0u : kFlagAccumulated);
				if (verdict.visible && verdict.plan != DCLF::kFadeRootOther && haveCamera) {
					StepFade(reinterpret_cast<std::byte*>(verdict.root.get()), verdict.plan, camera);
					++stats.fadesStepped;
				}
			}
			animatedVerdicts.clear();
		}
	}

	namespace
	{
		/**
		* @brief CS's engine writes, once per game update before the render half (guest: State::Reset at each frame's end). They
		* read and write game state, so they belong to the game thread's publish, not the render side's frame end.
		*/
		void WriteCsEngineState(const FrameSnapshot& a_snapshot)
		{
			State::WriteCsEngineState(a_snapshot.gameDelta);
			// The subsurface character light is native's only.
			auto& sss = globals::features::subsurfaceScattering;
			if (sss.loaded)
				sss.ApplyCharacterLight(sss.settings.EnableCharacterLighting.Adopted() != 0, sss.settings.CharacterLightingStrength.Adopted());
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
		// S7/S8 and DCLF produce what the main camera's list culls wrote, so they do not run.
		SkipListCulls();
		// Main::Draw's VL block (gVolumetricLightingEnabled: SetCameraData, the VL descriptor, VolumetricLighting::Render): CS computes the
		// VL render's constants from the snapshot in its RenderPostProcess driver (ORGVolumetricLighting::RenderNative). Same offsets on SE and AE.
		const auto draw = REL::RelocationID(35560, 36559).address();
		JumpOver("Main::Draw's volumetric lighting render", draw + 0x2F1, { 0x80, 0x3D }, draw + 0x33A);
		StealthLights::InstallHooks();
		RenderGraph::Engine::Native::WorldPass::InstallCapture();
		installed = true;
		logger::info(
			"[Snapshot] side effects: render stages 5-9, the frame counter, the render-unsafe bit, the fade enable count, the actor pre-cull, "
			"the decal tests and the animated roots' visibility and fades are produced on the game thread (native); the engine's writes and "
			"the main camera's list culls are off");
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
		WriteCsEngineState(a_snapshot);
	}

	bool Installed()
	{
		return installed;
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
		// The current camera (SE 527667 / AE 414580): the world camera, as RenderPostProcess left it in the engine's world frame. Game
		// code reads it: ImageSpaceModifierInstanceDOF's constructor and Apply take a target's distance from it (AE 0x141482cd0).
		static REL::Relocation<RE::NiCamera**> currentCamera{ REL::RelocationID(527667, 414580) };
		*currentCamera.get() = RE::Main::WorldRootCamera();
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
		       fmt::format("; {:.1f} of their fades stepped (S8)", double(s.fadesStepped) * perUpdate) +
		       "; " + StealthLights::TakeStatsLine() + "; " + FaceTint::TakeStatsLine();
	}
}
