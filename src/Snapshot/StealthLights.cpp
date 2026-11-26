#include "Snapshot/StealthLights.h"

#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Fatal.h"

#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace Snapshot::StealthLights
{
	namespace
	{
		// The sums' colour average (SE 247774, AE 198129: 1/3).
		constexpr float kThird = 1.0f / 3.0f;
		// The attach walk skips a light whose NiLight fade is at or below this (AE 0x1417743ec: 0.05).
		constexpr float kAttachMinFade = 0.05f;
		// The player branch's mask has one bit per shadow light.
		constexpr std::uint32_t kMaskBits = 32;

		bool installed = false;

		struct Stats
		{
			std::uint64_t playerCalls = 0, npcCalls = 0, npcNoGeometry = 0, pending = 0;
			float lastPlayer = 0.0f;
		};
		Stats stats;  // diagnostics; NPC calls may come from AI jobs

		/** @brief The lights ProcessQueuedLights (SE 99701, AE 106335) would leave in a ShadowSceneNode, without applying them. */
		struct Membership
		{
			std::vector<RE::NiPointer<RE::BSLight>> active, shadow;
			bool pending = false;  // the queues were not empty
		};

		/**
		* @brief The queued adds join the shadow or active list (a light whose NiLight is already there keeps the existing entry),
		* the queued removes leave it, then lights whose NiLight is gone or held only by the light drop out of the active list.
		*/
		void BuildMembership(RE::ShadowSceneNode* a_node, Membership& a_out)
		{
			auto& data = a_node->GetRuntimeData();
			a_out.active.clear();
			a_out.shadow.clear();
			RE::BSSpinLockGuard guard(data.lightQueueLock);
			for (const auto& light : data.activeLights)
				if (light)
					a_out.active.emplace_back(light.get());
			for (const auto& light : data.activeShadowLights)
				if (light)
					a_out.shadow.emplace_back(light.get());
			a_out.pending = !data.lightQueueAdd.empty() || !data.lightQueueRemove.empty();
			for (const auto& added : data.lightQueueAdd) {
				if (!added)
					continue;
				auto& list = added->IsShadowLight() ? a_out.shadow : a_out.active;
				bool present = false;
				for (const auto& light : list)
					present |= light->light.get() == added->light.get();
				if (!present)
					list.emplace_back(added.get());
			}
			for (const auto& removed : data.lightQueueRemove) {
				if (!removed)
					continue;
				auto& list = removed->IsShadowLight() ? a_out.shadow : a_out.active;
				for (auto it = list.begin(); it != list.end(); ++it)
					if (it->get() == removed.get()) {
						list.erase(it);
						break;
					}
			}
			if (!data.disableLightUpdate)
				std::erase_if(a_out.active, [](const RE::NiPointer<RE::BSLight>& a_light) {
					const auto* niLight = a_light->light.get();
					return !niLight || niLight->GetRefCount() == 1;
				});
		}

		/** @brief BSLight's luminance at a point (SE 101303, AE 108292); 0 for the excluded NiLight. */
		float Luminance(RE::BSLight* a_light, const RE::NiPoint3* a_point, RE::NiLight* a_exclude)
		{
			using func_t = float (*)(RE::BSLight*, const RE::NiPoint3*, RE::NiLight*);
			static REL::Relocation<func_t> func{ REL::RelocationID(101303, 108292) };
			return func(a_light, a_point, a_exclude);
		}

		/** @brief Whether a shadow light reaches the point unblocked (SE 41661, AE 42744: a pick from the light). */
		bool LightReaches(RE::BSLight* a_light, RE::Actor* a_actor, const RE::NiPoint3* a_point)
		{
			using func_t = bool (*)(void*, RE::BSLight*, RE::Actor*, const RE::NiPoint3*);
			static REL::Relocation<func_t> func{ REL::RelocationID(41661, 42744) };
			static REL::Relocation<void**> context{ REL::RelocationID(518074, 404596) };
			return func(*context, a_light, a_actor, a_point);
		}

		thread_local Membership t_membership;
		thread_local RE::Actor* t_actor = nullptr;

		/** @brief Releases the membership's references as soon as a sum is done. */
		struct MembershipScope
		{
			Membership& m = t_membership;
			~MembershipScope()
			{
				m.active.clear();
				m.shadow.clear();
			}
		};

		/**
		* @brief CalculateLightValue's LOS location call (SE 38900 +0x123, AE 39946 +0x123): the one place the actor is in hand
		* before the sums, kept for the NPC sum.
		*/
		struct LosLocation
		{
			static RE::NiPoint3* thunk(RE::Actor* a_actor, RE::NiPoint3* a_out, std::uint32_t a_location)
			{
				t_actor = a_actor;
				return func(a_actor, a_out, a_location);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		* @brief The caller's mask loop over the engine's shadow lights (SE 38900 +0x161, AE 39946 +0x16B). PlayerSum builds the mask
		* over CS's membership itself, so the engine list's picks are skipped.
		*/
		struct SkipEngineMaskPick
		{
			static bool thunk(void*, RE::BSLight*, RE::Actor*, const RE::NiPoint3*) { return true; }
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		* @brief The player branch's sum (ShadowSceneNode::GetLuminanceAtPoint, SE 99725, AE 106362) over CS's membership: the
		* active lights, then the shadow lights that reach the player, then the sun light's ambient and diffuse terms. The caller's
		* mask, built from the engine's list, is not used.
		*/
		struct PlayerSum
		{
			static float thunk(RE::ShadowSceneNode* a_node, const RE::NiPoint3* a_point, std::int32_t* a_count, float* a_ambient, float* a_sun,
				RE::NiLight* a_exclude, [[maybe_unused]] std::uint32_t a_engineMask)
			{
				MembershipScope scope;
				auto& m = scope.m;
				BuildMembership(a_node, m);
				auto* player = RE::PlayerCharacter::GetSingleton();
				std::uint32_t reaches = ~0u;
				for (std::uint32_t i = 0; i < m.shadow.size(); ++i)
					if (!LightReaches(m.shadow[i].get(), player, a_point))
						reaches &= ~(1u << (i % kMaskBits));
				float sum = 0.0f;
				for (const auto& light : m.active) {
					sum += Luminance(light.get(), a_point, a_exclude);
					*a_count += sum > 0.0f ? 1 : 0;
				}
				for (std::uint32_t i = 0; i < m.shadow.size() && i < kMaskBits; ++i) {
					if (!((reaches >> i) & 1))
						continue;
					sum += Luminance(m.shadow[i].get(), a_point, a_exclude);
					*a_count += sum > 0.0f ? 1 : 0;
				}
				*a_ambient = *a_sun = 0.0f;
				if (const auto* sunLight = a_node->GetRuntimeData().sunLight; sunLight && sunLight->light) {
					const auto& sun = sunLight->light->GetLightRuntimeData();
					*a_ambient = (sun.ambient.red + sun.ambient.green + sun.ambient.blue) * kThird;
					*a_sun = (sun.diffuse.red + sun.diffuse.green + sun.diffuse.blue) * sun.fade * kThird;
				}
				++stats.playerCalls;
				stats.pending += m.pending ? 1 : 0;
				stats.lastPlayer = sum;
				return sum;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The NPC's skin geometry: the third-person geometry that carries the process's lighting property. */
		RE::BSGeometry* SkinGeometry(RE::Actor* a_actor, const RE::BSShaderProperty* a_property)
		{
			auto* root = a_actor ? a_actor->Get3D(false) : nullptr;
			RE::BSGeometry* found = nullptr;
			if (root)
				RE::BSVisit::TraverseScenegraphGeometries(root, [&](RE::BSGeometry* a_geometry) {
					if (a_geometry->GetGeometryRuntimeData().shaderProperty.get() != a_property)
						return RE::BSVisit::BSVisitControl::kContinue;
					found = a_geometry;
					return RE::BSVisit::BSVisitControl::kStop;
				});
			return found;
		}

		/** @brief Whether the attach walk would give the light to geometry with this bound: lit, and its radius reaches the bound. */
		bool Reaches(RE::BSLight* a_light, const RE::NiBound& a_bound)
		{
			const auto* niLight = a_light->light.get();
			if (!niLight || niLight->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return false;
			const auto& data = niLight->GetLightRuntimeData();
			if (data.fade <= kAttachMinFade)
				return false;
			const auto d = niLight->world.translate - a_bound.center;
			const float reach = data.radius.x + a_bound.radius;
			return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
		}

		/**
		* @brief The NPC branch's sum (SE 100989, AE 107776) over the active lights of CS's membership that reach the NPC's skin
		* geometry, as the render half's attach walk would assign them (radius against the geometry bound; no portal test). Shadow
		* lights are never in geometry light lists.
		*/
		struct NpcSum
		{
			static float thunk([[maybe_unused]] RE::BSShaderPropertyLightData* a_list, const RE::NiPoint3* a_point, RE::NiLight* a_exclude,
				[[maybe_unused]] std::uint32_t a_first)
			{
				auto* node = RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0];
				auto* actor = std::exchange(t_actor, nullptr);
				auto* process = actor ? actor->GetMiddleHighProcess() : nullptr;
				auto* geometry = process ? SkinGeometry(actor, process->lightingProperty.get()) : nullptr;
				++stats.npcCalls;
				if (!node || !geometry) {
					++stats.npcNoGeometry;
					return 0.0f;
				}
				MembershipScope scope;
				BuildMembership(node, scope.m);
				float sum = 0.0f;
				for (const auto& light : scope.m.active)
					if (Reaches(light.get(), geometry->worldBound))
						sum += Luminance(light.get(), a_point, a_exclude);
				return sum;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void RequireCall(const char* a_what, std::uintptr_t a_site, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_site);
			std::int32_t relative = 0;
			std::memcpy(&relative, bytes + 1, sizeof(relative));
			if (bytes[0] != 0xE8 || a_site + 5 + relative != a_target)
				RenderGraph::Fatal("stealth lights", fmt::format("{} at {:#x} is not the engine's call; another plugin may have patched it", a_what, a_site));
		}
	}

	void InstallHooks()
	{
		if (REL::Module::IsVR() || !RenderGraph::Engine::Native::Requested())
			return;
		const auto calculate = REL::RelocationID(38900, 39946).address();
		const auto los = calculate + 0x123;
		const auto maskPick = calculate + REL::Relocate(0x161, 0x16B);
		const auto player = calculate + REL::Relocate(0x1C9, 0x1D3);
		const auto npc = calculate + REL::Relocate(0x247, 0x25C);
		RequireCall("CalculateLightValue's LOS location call", los, REL::RelocationID(36755, 37771).address());
		RequireCall("CalculateLightValue's shadow light pick", maskPick, REL::RelocationID(41661, 42744).address());
		RequireCall("CalculateLightValue's player light sum", player, REL::RelocationID(99725, 106362).address());
		RequireCall("CalculateLightValue's NPC light sum", npc, REL::RelocationID(100989, 107776).address());
		stl::write_thunk_call<LosLocation>(los);
		stl::write_thunk_call<SkipEngineMaskPick>(maskPick);
		stl::write_thunk_call<PlayerSum>(player);
		stl::write_thunk_call<NpcSum>(npc);
		installed = true;
		logger::info("[Snapshot] stealth light level: the player's and NPCs' light sums use CS's light membership (native)");
	}

	std::string TakeStatsLine()
	{
		if (!installed)
			return {};
		const auto s = std::exchange(stats, {});
		return fmt::format("stealth light level {} player sums ({} with queued lights counted early, last {:.4f}), {} NPC sums ({} without skin geometry)",
			s.playerCalls, s.pending, s.lastPlayer, s.npcCalls, s.npcNoGeometry);
	}
}
