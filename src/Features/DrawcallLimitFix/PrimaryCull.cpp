#include "PrimaryCull.h"

#include "LightingDescriptors.h"
#include "PassCapture.h"
#include "SceneStore.h"
#include "IndirectDraws.h"
#include "SunAccumulation.h"
#include "Switches.h"
#include "Toggles.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace DCLF
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kAfterFullFrustumCallSite = 0x14cbcea;  // CalculateAndDrawShadowCasterLights -> FUN_1414a0840, after either branch
		constexpr std::uintptr_t kAfterFullFrustum = 0x14a0840;
		constexpr std::uintptr_t kListJobsFinishCallSite = 0x14cbf4d;  // ... -> JobList::Finish(scene list culling)
		constexpr std::uintptr_t kListJobsFinish = 0xcf6810;
		constexpr std::uintptr_t kSceneLists = 0x338c870;      // BSTArray<NiPointer<NiAVObject>>*
		constexpr std::uintptr_t kSceneListCount = 0x338c868;  // std::uint32_t
		constexpr std::uintptr_t kExtraList = 0x338c888;       // BSTArray<NiPointer<NiAVObject>>, culled by the first job only
		constexpr std::uintptr_t kMainAccumulator = 0x338c830;   // BSShaderAccumulator*, render mode 0
		constexpr std::uintptr_t kDepthAccumulator = 0x338c828;  // BSShaderAccumulator*, render mode 0xC
		constexpr std::size_t kAccumulatorDeferredShadow = 0x178;  // byte: the accumulator draws the deferred shadow mask
		constexpr std::uintptr_t kNoSunShadowDir = 0x20330a4;      // byte: GetRenderPasses gives no pass ShadowDir
		constexpr std::uintptr_t kScreenDoorFades = 0x2033468;     // byte: screen-door fades are on
		constexpr std::uintptr_t kListProcesses = 0x338c8a0;       // BSGeometryListCullingProcess**, one per scene list
		constexpr std::uintptr_t kFadesOn = 0x2032dfd;             // byte: BSFadeNode::OnVisible updates fades
		// NiAVObject / BSFadeNode fields BSFadeNode::OnVisible (AE 0x141479f50) reads before its fade update.
		constexpr std::size_t kObjectFlags = 0xF4;
		constexpr std::uint32_t kFlagFadeSettled = 1u << 15;
		constexpr std::size_t kFadeAmount = 0x100;
		constexpr std::size_t kCurrentFade = 0x130;
		constexpr std::size_t kPropertyLightData = 0x70;
		constexpr std::size_t kLastVisibleFrame = 0x13C;
		constexpr std::uint32_t kFlagFadeTargetReached = 1u << 14;
		constexpr std::size_t kCameraLodAdjust = 0x184;
		constexpr std::uintptr_t kFadeFrameCounter = 0x2032e50;   // std::int32_t: the fade code's frame counter
		constexpr std::uintptr_t kFadeLodUpdates = 0x2032dfc;     // byte
		constexpr std::uintptr_t kFadeStepTime = 0x2033084;       // float: the frame time
		constexpr std::uintptr_t kFadeStepDivisor = 0x2032e2c;    // float
		constexpr std::uintptr_t kFadeStepMax = 0x2032e4c;        // float
		constexpr std::uintptr_t kFadeSpecialA = 0x332a254;       // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch
		constexpr std::uintptr_t kFadeSpecialB = 0x1769578;       // float
		constexpr std::uintptr_t kFadeUpdate = 0x147a160;         // FUN_14147a160(node, fadeAmount, camera): the fade state machine
		constexpr std::uintptr_t kFadeDistance = 0x147b110;       // FUN_14147b110(node, camera, out): the distance and LOD level
		constexpr std::uintptr_t kLeafLodUpdate = 0x147a430;      // FUN_14147a430(node, level): the leaf and tree LOD transition
		constexpr std::size_t kLightDataActiveMask = 0x1C;

		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;

		bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] != 0xE8)
				return false;
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}

		template <class T>
		T& Global(std::uintptr_t a_offset)
		{
			return *reinterpret_cast<T*>(REL::Module::get().base() + a_offset);
		}

		template <class T>
		T& At(const void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(const_cast<std::byte*>(static_cast<const std::byte*>(a_base)) + a_offset);
		}

		std::int64_t Now()
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		bool RttiIs(const RE::NiAVObject* a_object, const char* a_name)
		{
			const auto* rtti = a_object->GetRTTI();
			return rtti && rtti->name && std::strcmp(rtti->name, a_name) == 0;
		}

		/** @brief BSFadeNode::OnVisible's early out: nothing but the recursion when fades are off or the node is settled. */
		bool FadeSettled(const RE::NiAVObject* a_node)
		{
			if (!Global<std::uint8_t>(kFadesOn))
				return true;
			return (At<std::uint32_t>(a_node, kObjectFlags) & kFlagFadeSettled) && At<float>(a_node, kFadeAmount) == 1.0f &&
			       At<float>(a_node, kCurrentFade) == 1.0f;
		}

		/** @brief The engine's sphere test against the active planes (outside when n.c - d < -r). */
		bool Outside(const RE::NiFrustumPlanes& a_planes, const RE::NiBound& a_bound)
		{
			const std::uint32_t mask = a_planes.activePlanes.underlying() & 0x3Fu;
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(mask & (1u << p)))
					continue;
				const auto& plane = a_planes.cullingPlanes[p];
				if (plane.normal.Dot(a_bound.center) - plane.constant < -a_bound.radius)
					return true;
			}
			return false;
		}

		std::string NameOf(const RE::NiAVObject* a_object)
		{
			if (!a_object)
				return "null";
			const char* name = a_object->name.c_str();
			return fmt::format("{}:{}", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?", name && *name ? name : "-");
		}
	}

	PrimaryCull& PrimaryCull::Get()
	{
		static PrimaryCull instance;
		return instance;
	}

	bool PrimaryCull::Probe()
	{
		static const bool probe = SwitchValue("CS_DCLF_PRIMARY_EXCLUDE") == "probe";
		return probe;
	}

	bool PrimaryCull::ServiceFade(RE::NiAVObject* a_node, bool a_leaf, const RE::NiCamera& a_camera)
	{
		using FadeUpdate = void (*)(RE::NiAVObject*, float, const float*);
		using FadeDistance = float (*)(RE::NiAVObject*, const float*, std::uint32_t*);
		using LeafLodUpdate = void (*)(RE::NiAVObject*, std::uint32_t);
		const auto base = REL::Module::get().base();
		const float camera[4] = { a_camera.world.translate.x, a_camera.world.translate.y, a_camera.world.translate.z, At<float>(&a_camera, kCameraLodAdjust) };
		auto& lastVisible = At<std::int32_t>(a_node, kLastVisibleFrame);
		const std::int32_t counter = Global<std::int32_t>(kFadeFrameCounter);
		// BSLeafAnimNode::OnVisible: its LOD step first, then BSFadeNode::OnVisible.
		if (a_leaf && Global<std::uint8_t>(kFadeLodUpdates)) {
			std::uint32_t level = 0;
			reinterpret_cast<FadeDistance>(base + kFadeDistance)(a_node, camera, &level);
			reinterpret_cast<LeafLodUpdate>(base + kLeafLodUpdate)(a_node, level);
			lastVisible = counter;
		}
		if (!Global<std::uint8_t>(kFadesOn) || FadeSettled(a_node))
			return true;
		auto& flags = At<std::uint32_t>(a_node, kObjectFlags);
		auto& currentFade = At<float>(a_node, kCurrentFade);
		const float fadeAmount = At<float>(a_node, kFadeAmount);
		if ((At<std::uint8_t>(a_node, 0x153) & 0xF) == 6 && Global<float>(kFadeSpecialA) == Global<float>(kFadeSpecialB)) {
			std::uint32_t level = 0;
			reinterpret_cast<FadeDistance>(base + kFadeDistance)(a_node, camera, &level);
			std::uint32_t value = flags;
			if (!Global<std::uint8_t>(kFadeLodUpdates)) {
				value |= kFlagFadeTargetReached;
				currentFade = 1.0f;
				flags = value;
			}
			if (!(value & kFlagFadeTargetReached)) {
				bool reached = true;
				if (counter - lastVisible <= 1) {
					const float step = std::min(Global<float>(kFadeStepTime) / Global<float>(kFadeStepDivisor), Global<float>(kFadeStepMax));
					currentFade = std::min(step + currentFade, 1.0f);
					reached = !(currentFade < 1.0f);
				} else {
					currentFade = 1.0f;
				}
				if (reached)
					flags = value | kFlagFadeTargetReached;
			}
			lastVisible = counter;
			return true;
		}
		reinterpret_cast<FadeUpdate>(base + kFadeUpdate)(a_node, fadeAmount, camera);
		lastVisible = counter;
		return currentFade > 0.0f && fadeAmount != 0.0f;
	}

	PrimaryCull::EntryPlan PrimaryCull::PlanOf(std::uint32_t a_entry, const RE::NiAVObject* a_root)
	{
		auto& plan = cut.plans[a_entry];
		if (plan != EntryPlan::Unknown)
			return plan;
		const auto& candidates = *cut.candidates;
		plan = EntryPlan::Rejected;
		if (a_entry >= candidates.primary.size() || !candidates.primary[a_entry]) {
			// [TEMP] the first geometry the verdict fails on, by its table state and reason.
			std::string why = "snapshot verdict";
			for (std::uint32_t g = cut.geometryOffsets[a_entry]; g < cut.geometryOffsets[a_entry + 1]; ++g) {
				const auto* geometry = cut.geometryIndices[g];
				Ineligible reason = Ineligible::None;
				const int state = SceneStore::Get().ProbeTableState(geometry, reason);
				const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
				const auto* alpha = geometry->GetGeometryRuntimeData().alphaProperty.get();
				const bool decal = property && (property->flags.underlying() & 0xc000000ull);
				const bool blended = alpha && (alpha->alphaFlags & 1);
				if (state != 2 || reason != Ineligible::None || decal || blended) {
					why = fmt::format("snapshot verdict: state {} {}{}{}", state, kIneligibleNames[static_cast<std::size_t>(reason)], decal ? " decal" : "", blended ? " blended" : "");
					break;
				}
			}
			++cutStats.planReasons[why];
			return plan;
		}
		std::string cause;
		// Only nodes whose OnVisible is the plain recursion (NiNode, and a multibound's frustum cache), under a root that
		// may also be a fade node; and every geometry one of the entry's tracked ones. Anything else keeps it in.
		bool rejected = false;
		std::uint32_t geometries = 0;
		const auto visit = [&](const auto& a_self, const RE::NiAVObject* a_object, bool a_isRoot) -> void {
			if (rejected || !a_object)
				return;
			if (const auto* geometry = const_cast<RE::NiAVObject*>(a_object)->AsGeometry()) {
				++geometries;
				if (!candidates.geometries.contains(geometry)) {
					rejected = true;
					cause = fmt::format("untracked geometry {}", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?");
				}
				return;
			}
			auto* node = const_cast<RE::NiAVObject*>(a_object)->AsNode();
			if (!node || !(RttiIs(a_object, "NiNode") || RttiIs(a_object, "BSMultiBoundNode") ||
							  (a_isRoot && (RttiIs(a_object, "BSFadeNode") || RttiIs(a_object, "BSLeafAnimNode"))))) {
				rejected = true;
				cause = fmt::format("{} node {}", a_isRoot ? "root" : "inner", a_object->GetRTTI() ? a_object->GetRTTI()->name : "?");
				return;
			}
			for (const auto& child : node->GetChildren())
				a_self(a_self, child.get(), false);
		};
		visit(visit, a_root, true);
		const std::uint32_t tracked = cut.geometryOffsets[a_entry + 1] - cut.geometryOffsets[a_entry];
		if (rejected || !geometries || geometries != tracked) {
			++cutStats.planReasons[rejected ? cause : fmt::format("geometry count {} vs {} tracked", geometries < tracked ? "below" : "above", tracked)];
			return plan;
		}
		plan = RttiIs(a_root, "BSFadeNode") ? EntryPlan::FadeRoot : RttiIs(a_root, "BSLeafAnimNode") ? EntryPlan::LeafRoot : EntryPlan::Plain;
		return plan;
	}

	void PrimaryCull::RestoreLists()
	{
		if (!cut.filtered)
			return;
		cut.filtered = false;
		auto* lists = Global<SceneList*>(kSceneLists);
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		// Back into their positions, by moves alone: the kept entries slide up from the end, the removed ones drop into
		// their slots, and no reference count changes.
		for (std::uint32_t l = 0; l < count && l + 1 < cut.removedBegin.size(); ++l) {
			std::uint32_t next = cut.removedBegin[l + 1];
			const std::uint32_t first = cut.removedBegin[l];
			if (next == first)
				continue;  // nothing was taken out of this one
			auto& list = lists[l];
			std::uint32_t kept = list.size();
			list.resize(cut.originalSize[l]);
			for (std::uint32_t i = cut.originalSize[l]; i-- > 0;) {
				if (next > first && cut.removed[next - 1].first == i)
					list[i] = std::move(cut.removed[--next].second);
				else
					list[i] = std::move(list[--kept]);
			}
		}
		cut.removed.clear();
	}

	void PrimaryCull::AfterFullFrustum()
	{
		cut.excluded.clear();
		frameVisible.clear();
		if (Toggles::Get().Active().excludePrimaryEntries)
			FilterLists();
		if (!Probe())
			return;
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
		if (!lists || !count)
			return;
		auto candidates = SceneStore::Get().GetSunCandidates();
		frameCandidates = candidates;
		listed.assign(candidates ? candidates->entries.size() : 0, 0);
		ankerl::unordered_dense::set<const RE::NiAVObject*> entrySet;
		auto& c = census;
		++c.frames;
		c.lists += count;
		c.sunCandidates += candidates ? candidates->entries.size() : 0;
		for (std::uint32_t l = 0; l < count; ++l) {
			const auto& list = lists[l];
			c.firstEntries += list.empty() ? 0 : 1;
			for (const auto& entry : list) {
				const auto* object = entry.get();
				if (!object)
					continue;
				++c.entries;
				entrySet.insert(object);
				const auto* reference = object->GetUserData();
				if (reference && reference->IsActor())
					++c.actorEntries;
				if (candidates) {
					const auto it = candidates->entries.find(object);
					if (it != candidates->entries.end()) {
						++c.candidates;
						listed[it->second] = 1;
						continue;
					}
				}
				if (c.others.size() < 200)
					++c.others[fmt::format("{} under {}{}", NameOf(object), NameOf(object->parent), reference && reference->IsActor() ? " (actor)" : "")];
				else
					++c.others["(more)"];
			}
		}
		if (candidates)
			for (std::uint32_t g = 0; g < candidates->geometryEntry.size(); ++g)
				c.candidateGeometries += listed[candidates->geometryEntry[g]];
		c.extraEntries += Global<SceneList>(kExtraList).size();

		// Where the player's third-person 3D reaches the cull: its chain of parents, marking the list entries.
		if (playerChain.empty() || (c.frames % 300) == 1) {
			std::string chain;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				for (auto* node = player->Get3D(false); node; node = node->parent)
					chain += fmt::format(" <- {}{}", NameOf(node), entrySet.contains(node) ? " [LIST ENTRY]" : "");
			playerChain = chain;
		}
		counting.store(true, std::memory_order_release);
	}

	void PrimaryCull::FilterLists()
	{
		++cutStats.frames;
		auto candidates = SceneStore::Get().GetSunCandidates();
		const std::uint32_t count = Global<std::uint32_t>(kSceneListCount);
		auto* lists = Global<SceneList*>(kSceneLists);
		// A synthetic pass has no point-light shadow: any local light that cast shadows last frame keeps everything in.
		bool localShadows = false;
		if (auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr)
			for (const auto& light : node->GetRuntimeData().activeShadowLights)
				localShadows = localShadows || (light && light.get() != node->GetRuntimeData().sunShadowDirLight);
		if (!candidates || candidates->generation != SceneStore::Get().GetSunCandidatesGeneration()) {
			++cutStats.skippedStale;
			return;
		}
		if (!lists || !count || !SunAccumulation::Get().ExclusionLive() || localShadows) {
			++cutStats.skippedPreconditions;
			return;
		}
		const std::int64_t start = Now();
		if (cut.candidates != candidates) {
			// A new snapshot: the plans, and each entry's geometries, again.
			cut.candidates = candidates;
			const std::uint32_t entries = static_cast<std::uint32_t>(candidates->entries.size());
			cut.plans.assign(entries, EntryPlan::Unknown);
			cut.geometryOffsets.assign(entries + 2, 0);
			for (const auto entry : candidates->geometryEntry)
				++cut.geometryOffsets[entry + 2];
			for (std::size_t e = 2; e < cut.geometryOffsets.size(); ++e)
				cut.geometryOffsets[e] += cut.geometryOffsets[e - 1];
			cut.geometryIndices.assign(candidates->geometryEntry.size(), nullptr);
			for (const auto& [geometry, index] : candidates->geometries)
				cut.geometryIndices[cut.geometryOffsets[candidates->geometryEntry[index] + 1]++] = geometry;
			cut.geometryOffsets.pop_back();
			cut.roots.assign(entries, nullptr);
			for (const auto& [root, index] : candidates->entries)
				cut.roots[index] = root;
			// Admission is kept, by node, for the entries still in the snapshot.
			std::erase_if(cut.admitted, [&](const RE::NiAVObject* a_root) { return !candidates->entries.contains(a_root); });
		}
		const auto frame = SceneStore::Get().GetFrame();
		const auto& draws = IndirectDraws::Get();
		cut.originalSize.assign(count, 0);
		cut.removedBegin.assign(count + 1, 0);
		cut.removed.clear();
		for (std::uint32_t l = 0; l < count; ++l) {
			auto& list = lists[l];
			std::uint32_t kept = 0;
			const std::uint32_t size = list.size();
			cut.originalSize[l] = size;
			cut.removedBegin[l] = static_cast<std::uint32_t>(cut.removed.size());
			for (std::uint32_t r = 0; r < size; ++r) {
				auto* object = list[r].get();
				bool remove = false;
				// Entry 0 stays: the job's first entry sets its process's frustum up (Process2).
				if (r > 0 && object) {
					if (const auto it = candidates->entries.find(object); it != candidates->entries.end()) {
						const std::uint32_t e = it->second;
						const auto plan = PlanOf(e, object);
						if (plan == EntryPlan::Rejected) {
							++cutStats.rejectedPlan;
						} else if ((plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot) && (At<float>(object, kCurrentFade) != 1.0f || At<float>(object, kFadeAmount) != 1.0f)) {
							// Fading: the engine's cull keeps it (its fade and the registration's fade handling are the engine's).
							++cutStats.notSettled;
							if (cutStats.fadeStates.size() < 40)
								++cutStats.fadeStates[fmt::format("fadeAmount {:.2f}, currentFade {:.2f}, +0x153 {:#x}", At<float>(object, kFadeAmount),
									At<float>(object, kCurrentFade), At<std::uint8_t>(object, 0x153))];
							cut.admitted.erase(object);
						} else if (cut.admitted.contains(object)) {
							remove = true;
						} else {
							remove = true;
							for (std::uint32_t g = cut.geometryOffsets[e]; g < cut.geometryOffsets[e + 1] && remove; ++g)
								remove = draws.DrewLastFrame(cut.geometryIndices[g], frame);
							if (remove)
								cut.admitted.insert(object);
							else
								++cutStats.notAdmitted;
						}
						if (remove)
							cut.excluded.push_back(e);
					}
				}
				++cutStats.entries;
				if (remove) {
					cut.removed.emplace_back(r, std::move(list[r]));
					continue;
				}
				if (kept != r)
					list[kept] = std::move(list[r]);
				++kept;
			}
			if (kept != size) {
				list.resize(kept);
				cut.filtered = true;
			}
		}
		cut.removedBegin[count] = static_cast<std::uint32_t>(cut.removed.size());
		cutStats.removed += cut.excluded.size();
		++cutStats.appliedFrames;
		cutStats.filterTicks += Now() - start;
	}

	void PrimaryCull::AfterListJobs()
	{
		RestoreLists();
		if (cut.excluded.empty() || !cut.candidates)
			return;
		const std::int64_t start = Now();
		// The main camera's planes as the list jobs used them (each job's first entry had Process2 set them up).
		auto** processes = Global<RE::NiCullingProcess**>(kListProcesses);
		const auto* process = processes ? processes[0] : nullptr;
		if (!process)
			return;
		const auto& planes = process->planes;
		const auto* camera = process->camera;
		const std::uint32_t sunBits = SunAccumulation::Get().SunBits();
		auto& handBack = At<RE::BSTArray<RE::NiPointer<RE::NiAVObject>>>(process, 0x128);  // BSCullingProcess::objectArray
		for (const std::uint32_t e : cut.excluded) {
			const auto* root = cut.roots[e];
			if (!root || Outside(planes, root->worldBound))
				continue;
			++cutStats.visibleEntries;
			// What OnVisible would have done to the root: its fade and LOD state. A root that faded out is not drawn; one
			// that has started to fade is the engine's this frame: its geometries go into the list process's output, which
			// the registration jobs walk as they walk what the cull found (and the next frame's filter keeps it in).
			const auto plan = cut.plans[e];
			bool engineDraws = false;
			if ((plan == EntryPlan::FadeRoot || plan == EntryPlan::LeafRoot) && camera) {
				++cutStats.fadeServiced;
				auto* node = const_cast<RE::NiAVObject*>(root);
				if (!ServiceFade(node, plan == EntryPlan::LeafRoot, *camera)) {
					++cutStats.fadedOut;
					continue;
				}
				engineDraws = At<float>(node, kCurrentFade) != 1.0f;
			}
			for (std::uint32_t g = cut.geometryOffsets[e]; g < cut.geometryOffsets[e + 1]; ++g) {
				const auto* geometry = cut.geometryIndices[g];
				// NiAVObject::Cull skips an app-culled object and everything under it.
				bool hidden = false;
				for (const RE::NiAVObject* object = geometry; object && !hidden; object = object == root ? nullptr : object->parent)
					hidden = object->GetFlags().any(RE::NiAVObject::Flag::kHidden);
				if (hidden) {
					++cutStats.hiddenSkipped;
					continue;
				}
				if (engineDraws) {
					handBack.push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::BSGeometry*>(geometry)));
					++cutStats.handedBack;
					continue;
				}
				frameVisible.push_back(geometry);
				// The main registration reads the mask and clears it (+0x160 = 0xFFFF), so every later registration in
				// the frame reads 0.
				if (const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get())
					if (void* lightData = At<void*>(property, kPropertyLightData)) {
						auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
						cutStats.localShadowed += (mask & ~sunBits) ? 1 : 0;
						mask = 0;
					}
			}
		}
		cutStats.afterTicks += Now() - start;
	}

	const std::vector<std::pair<const RE::BSGeometry*, AccumulatedPass>>& PrimaryCull::BuildSyntheticPasses()
	{
		synthetic.clear();
		if (frameVisible.empty())
			return synthetic;
		const std::int64_t start = Now();
		for (const auto* geometry : frameVisible) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
			if (!lighting) {
				++cutStats.unmodelled;
				continue;
			}
			auto& cached = derivedCache[geometry];
			const std::uint8_t fadeState = FadeStateOf(property);
			if (cached.property != property || cached.material != lighting->material || cached.flags != lighting->flags.underlying() ||
				cached.fadeState != fadeState) {
				LightingDescriptors descriptors;
				const auto reason = DeriveLightingDescriptors(*lighting, *geometry, nullptr, descriptors, true);
				cached = { property, lighting->material, lighting->flags.underlying(), fadeState, reason == Ineligible::None ? descriptors.derivedPass : kNotDerived };
			}
			AccumulatedPass pass;
			if (!SyntheticPass(*geometry, cached.derivedPass, pass)) {
				++cutStats.unmodelled;
				continue;
			}
			synthetic.emplace_back(geometry, pass);
		}
		cutStats.synthetic += synthetic.size();
		cutStats.synthTicks += Now() - start;
		return synthetic;
	}

	void PrimaryCull::NoteRegistration(const void* a_accumulator, const RE::BSGeometry* a_geometry)
	{
		const bool main = a_accumulator == Global<void*>(kMainAccumulator);
		const bool depth = !main && a_accumulator == Global<void*>(kDepthAccumulator);
		if (!main && !depth) {
			registrations.other.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		bool under = false;
		if (const auto* candidates = frameCandidates.get()) {
			const auto it = candidates->geometries.find(a_geometry);
			under = it != candidates->geometries.end() && listed[candidates->geometryEntry[it->second]];
		}
		(main ? registrations.main : registrations.depth).fetch_add(1, std::memory_order_relaxed);
		if (under)
			(main ? registrations.mainUnder : registrations.depthUnder).fetch_add(1, std::memory_order_relaxed);
	}

	std::uint32_t PrimaryCull::SunShadowBits(const RE::BSGeometry& a_geometry)
	{
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return 0;
		const auto inCascades = SunAccumulation::Get().InSunCascades(a_geometry.worldBound);
		if (!inCascades)
			return ~0u;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* fadeNode = lighting->fadeNode;
		const float fade = fadeNode ? fadeNode->GetRuntimeData().currentFade : 1.0f;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const float alpha = (material ? material->materialAlpha : 1.0f) * fade;
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		// GetRenderPasses (AE 0x1414adfb0): local_164, local_167, local_165/local_168 and local_158.
		const bool translucent = alpha < 1.0f || blended;
		const bool screenDoor = Global<std::uint8_t>(kScreenDoorFades) && fadeNode && fadeNode->GetRuntimeData().unk154 && fade < 1.0f &&
		                        !(flags & (1ull << 19));
		constexpr std::uint64_t kOpaqueClasses = 0x800c000000ull;
		const bool eligible = !translucent || (screenDoor && !blended) || (flags & kOpaqueClasses);
		const bool shadowDirAllowed = eligible && !Global<std::uint8_t>(kNoSunShadowDir);
		bool deferred = Global<std::uint8_t*>(kMainAccumulator)[kAccumulatorDeferredShadow] != 0;
		if (!screenDoor || blended)
			deferred = deferred && !((alpha < 1.0f || blended || (flags & (1ull << 33))) && !(flags & kOpaqueClasses));
		// FUN_1414fcf80: ShadowDir when a mask bit names the sun (any cascade the bound meets).
		bool shadowDir = shadowDirAllowed && *inCascades;
		bool defShadow = deferred;
		if (!deferred)
			shadowDir = false;
		else if (!shadowDir)
			defShadow = false;  // no shadowed point light either (the caller's precondition)
		if (!(flags & 0x800c000100ull) && !lighting->shadowMapOrMaskPasses.head)
			shadowDir = defShadow = false;
		return (shadowDir ? 0x2000u : 0u) | (defShadow ? 0x4000u : 0u);
	}

	bool PrimaryCull::SyntheticPass(const RE::BSGeometry& a_geometry, std::uint32_t a_derivedPass, AccumulatedPass& a_out)
	{
		if (a_derivedPass == kNotDerived)
			return false;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = netimmerse_cast<const RE::BSLightingShaderProperty*>(property);
		if (!lighting)
			return false;
		const std::uint64_t flags = lighting->flags.underlying();
		const auto* fadeNode = lighting->fadeNode;
		const float fade = fadeNode ? fadeNode->GetRuntimeData().currentFade : 1.0f;
		const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(lighting->material);
		const auto* alphaProperty = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blended = alphaProperty && (alphaProperty->alphaFlags & 1);
		const bool translucent = (material ? material->materialAlpha : 1.0f) * fade < 1.0f || blended;
		// Fading and translucent objects take GetRenderPasses' other hints (1, 9, 10) and the screen-door bit; not modelled.
		if (fade < 1.0f)
			return false;
		const std::uint32_t sun = SunShadowBits(a_geometry);
		if (sun == ~0u)
			return false;
		std::uint32_t hint = 0;
		if (flags & 0xc000000ull)
			hint = translucent ? 3 : 2;  // the decal groups
		else if (translucent)
			return false;
		else if (flags & 0x8000000600000000ull)
			hint = (~static_cast<std::uint32_t>(flags >> 33) & 1u) | 6u;
		else if (flags & (1ull << 61))
			hint = 11;  // TreeAnim
		else if (!sun)
			hint = 15;  // opaque with no shadow work (no shadowed point light: the caller's precondition)
		a_out = {};
		a_out.subPass = PassCapture::SubPassOf(&a_geometry, flags);
		a_out.technique = DrawnPassDescriptor((a_derivedPass & ~0x6000u) | sun, a_out.subPass);
		a_out.passEnum = a_out.technique + 0x4800002Du;
		a_out.hint = hint;
		a_out.lodRow = SceneStore::LodRowOf(a_geometry, property);
		return true;
	}

	bool PrimaryCull::UnderListedCandidate(const RE::BSGeometry* a_geometry) const
	{
		const auto* candidates = frameCandidates.get();
		if (!candidates)
			return false;
		const auto it = candidates->geometries.find(a_geometry);
		return it != candidates->geometries.end() && listed[candidates->geometryEntry[it->second]];
	}

	void PrimaryCull::NoteDerived(const RE::BSGeometry& a_geometry, const LightingDescriptors& a_descriptors, const AccumulatedPass& a_accumulated,
		Ineligible a_reason, std::uint32_t a_derivedLodRow)
	{
		auto& d = derived;
		++d.objects;
		++d.hints[std::min<std::uint32_t>(a_accumulated.hint, 31)];
		d.fading += a_accumulated.fading ? 1 : 0;
		d.alphaMask += (a_accumulated.technique & kPassAdditionalAlphaMask) ? 1 : 0;
		if (const auto* pass = a_accumulated.pass; pass && pass->numShadowLights)
			++d.shadowLights;
		if (a_geometry.GetGeometryRuntimeData().skinInstance && a_derivedLodRow != a_accumulated.lodRow)
			++d.lodRowDiffer;
		if (a_reason != Ineligible::None) {
			++d.ineligible;
			++d.byReason[std::min<std::size_t>(static_cast<std::size_t>(a_reason), d.byReason.size() - 1)];
			return;
		}
		if (a_descriptors.derivedPass == kNotDerived) {
			++d.notDerived;
			return;
		}
		constexpr std::uint32_t kSunBits = 0x6000u;
		const std::uint32_t differing = a_descriptors.derivedPass ^ a_descriptors.pass;
		if (differing & ~kSunBits)
			++d.differ;
		else if (differing)
			++d.sunOnly;
		for (std::uint32_t remaining = differing; remaining; remaining &= remaining - 1)
			++d.bits[std::countr_zero(remaining)];
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* lighting = static_cast<const RE::BSLightingShaderProperty*>(property);
		if (std::abs(a_descriptors.derivedSpecularLODFade - lighting->specularLODFade) > 1e-3f)
			++d.specularFadeDiffer;
		if (std::abs(a_descriptors.derivedEnvmapLODFade - lighting->envmapLODFade) > 1e-3f)
			++d.envmapFadeDiffer;
		if (const std::uint32_t sun = SunShadowBits(a_geometry); sun == ~0u) {
			++d.sunUnknown;
		} else {
			const std::uint32_t engine = a_descriptors.pass & kSunBits;
			if (sun == engine)
				++d.sunAgree;
			else if (!sun)
				++d.sunEngineOnly;
			else if (!engine)
				++d.sunDclfOnly;
			else
				++d.sunOther;
			if (sun != engine && d.sunSamples.size() < 24) {
				const auto& bound = a_geometry.worldBound;
				++d.sunSamples[fmt::format("'{}' under '{}': engine {:#x} DCLF {:#x}, bound ({:.0f} {:.0f} {:.0f}) r {:.0f}, hint {}",
					a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?",
					engine, sun, bound.center.x, bound.center.y, bound.center.z, bound.radius, a_accumulated.hint)];
			}
		}
		if (AccumulatedPass built; !SyntheticPass(a_geometry, a_descriptors.derivedPass, built)) {
			++d.synthUnmodeled;
		} else {
			const bool technique = built.technique != a_accumulated.technique, subPass = built.subPass != a_accumulated.subPass;
			const bool hint = built.hint != a_accumulated.hint, lodRow = built.lodRow != a_accumulated.lodRow;
			d.synthTechnique += technique, d.synthSubPass += subPass, d.synthHint += hint, d.synthLodRow += lodRow;
			if (!technique && !subPass && !hint && !lodRow)
				++d.synthAgree;
			else if (d.synthSamples.size() < 24)
				++d.synthSamples[fmt::format("'{}' under '{}': technique {:08X}/{:08X} subPass {}/{} hint {}/{} lodRow {}/{} (engine/DCLF)",
					a_geometry.name.c_str() ? a_geometry.name.c_str() : "?", a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?",
					a_accumulated.technique, built.technique, a_accumulated.subPass, built.subPass, a_accumulated.hint, built.hint, a_accumulated.lodRow, built.lodRow)];
		}
		if ((differing & ~kSunBits) && d.samples.size() < 40)
			++d.samples[fmt::format("'{}' under '{}': registered {:08X} derived {:08X} (hint {}, subPass {})", a_geometry.name.c_str() ? a_geometry.name.c_str() : "?",
				a_geometry.parent && a_geometry.parent->name.c_str() ? a_geometry.parent->name.c_str() : "?", a_descriptors.pass, a_descriptors.derivedPass,
				a_accumulated.hint, a_accumulated.subPass)];
	}

	struct PrimaryCull::Hooks
	{
		/** @brief CalculateAndDrawShadowCasterLights' call of FUN_1414a0840, right after the full-frustum cull. */
		struct AfterFullFrustum
		{
			static std::uint64_t thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				PrimaryCull::Get().AfterFullFrustum();
				return func(a_1, a_2, a_3, a_4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Its JobList::Finish of the scene list culling jobs. */
		struct ListJobsFinish
		{
			static std::uint64_t thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				const auto result = func(a_1, a_2, a_3, a_4);
				PrimaryCull::Get().AfterListJobs();
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void PrimaryCull::Install()
	{
		if (installed || !REL::Module::IsAE())
			return;
		const auto base = REL::Module::get().base();
		const auto afterFullFrustum = base + kAfterFullFrustumCallSite;
		const auto finish = base + kListJobsFinishCallSite;
		if (!CallsTo(afterFullFrustum, base + kAfterFullFrustum) || !CallsTo(finish, base + kListJobsFinish)) {
			logger::warn("[DCLF] primary cull: the calls are not where expected; the engine keeps the primary's cull");
			return;
		}
		stl::write_thunk_call<Hooks::AfterFullFrustum>(afterFullFrustum);
		stl::write_thunk_call<Hooks::ListJobsFinish>(finish);
		installed = true;
		logger::info("[DCLF] primary cull installed (the scene lists after the full-frustum cull, the list jobs' Finish){}", Probe() ? "; census on" : "");
	}

	void PrimaryCull::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if (!installed || (a_frame % a_interval) != 0)
			return;
		if (const auto& s = cutStats; s.frames) {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
			const double applied = std::max<double>(static_cast<double>(s.appliedFrames), 1.0);
			logger::info("[DCLF] primary exclusion: applied on {} of {} frames ({} stale, {} preconditions); per frame {:.0f} of {:.0f} list entries left out "
						 "(kept in: {:.0f} by their plan, {:.1f} fading, {:.1f} not yet drawn), {:.0f} of them visible; {:.0f} synthetic passes, {:.1f} not modelled, "
				"{:.1f} hidden, {:.1f} with a local light's shadow bit; {} holes; fades serviced {:.0f}, faded out {:.1f}, handed back {:.1f}; "
						 "render thread: filter {:.3f} ms, after the jobs {:.3f} ms, synthetic passes {:.3f} ms",
				s.appliedFrames, s.frames, s.skippedStale, s.skippedPreconditions, s.removed / applied, s.entries / applied, s.rejectedPlan / applied,
				s.notSettled / applied, s.notAdmitted / applied, s.visibleEntries / applied, s.synthetic / applied, s.unmodelled / applied,
				s.hiddenSkipped / applied, s.localShadowed / applied, s.holes, s.fadeServiced / applied, s.fadedOut / applied, s.handedBack / applied,
				s.filterTicks * toMs / applied, s.afterTicks * toMs / applied,
				s.synthTicks * toMs / applied);
			for (const auto& [cause, count] : s.planReasons)
				logger::info("[DCLF][TEMP] primary exclusion plan rejects: {} x {}", count, cause);
			for (const auto& [state, count] : s.fadeStates)
				logger::info("[DCLF][TEMP] primary exclusion fade root not settled: {:.1f}/frame {}", count / applied, state);
			cutStats = {};
		}
		if (!Probe() || !census.frames)
			return;
		const auto& c = census;
		const double f = static_cast<double>(c.frames);
		const auto take = [](std::atomic<std::uint64_t>& a_value) { return a_value.exchange(0, std::memory_order_relaxed); };
		const auto main = take(registrations.main), mainUnder = take(registrations.mainUnder);
		const auto depth = take(registrations.depth), depthUnder = take(registrations.depthUnder), other = take(registrations.other);
		logger::info("[DCLF] primary census, per frame: {:.0f} lists, {:.0f} entries ({:.0f} first), {:.0f} extra; {:.0f} are sun candidates ({:.0f} of the candidates' {:.0f}; {:.0f} tracked geometries under them), {:.0f} actor entries; "
					 "registrations: main {:.0f} ({:.0f} under a listed candidate), depth {:.0f} ({:.0f}), other {:.0f}",
			c.lists / f, c.entries / f, c.firstEntries / f, c.extraEntries / f, c.candidates / f, c.candidates / f, c.sunCandidates / f, c.candidateGeometries / f,
			c.actorEntries / f, main / f, mainUnder / f, depth / f, depthUnder / f, other / f);
		std::vector<std::pair<std::uint64_t, std::string>> top;
		for (const auto& [key, count] : c.others)
			top.emplace_back(count, key);
		std::sort(top.rbegin(), top.rend());
		for (std::size_t i = 0; i < top.size() && i < 30; ++i)
			logger::info("[DCLF] primary census, not a candidate: {:.2f}/frame {}", top[i].first / f, top[i].second);
		logger::info("[DCLF] primary census, the player's 3D:{}", playerChain);
		census = {};
		if (derived.objects) {
			const double n = static_cast<double>(derived.objects) / f;
			auto& d = derived;
			std::string bits, hints, reasons;
			for (std::uint32_t b = 0; b < 32; ++b)
				if (d.bits[b])
					bits += fmt::format(" {}={:.1f}", b, d.bits[b] / f);
			for (std::uint32_t h = 0; h < 32; ++h)
				if (d.hints[h])
					hints += fmt::format(" {}={:.1f}", h, d.hints[h] / f);
			for (std::size_t r = 0; r < d.byReason.size(); ++r)
				if (d.byReason[r])
					reasons += fmt::format(" {}={:.1f}", r < kIneligibleNames.size() ? kIneligibleNames[r] : "?", d.byReason[r] / f);
			logger::info("[DCLF] primary derivation, per frame: {:.0f} registered objects under listed candidates: {:.1f} ineligible ({}), {:.1f} not derived; "
						 "descriptor: {:.1f} differ outside the sun bits, {:.1f} only in them; bits:{}; {:.1f} with shadowed point lights, {:.1f} LOD rows differ, "
						 "{:.1f} fading, {:.1f} screen-door; LOD fades differ: specular {:.1f}, envmap {:.1f}; hints:{}",
				n, d.ineligible / f, reasons, d.notDerived / f, d.differ / f, d.sunOnly / f, bits, d.shadowLights / f, d.lodRowDiffer / f, d.fading / f, d.alphaMask / f,
				d.specularFadeDiffer / f, d.envmapFadeDiffer / f, hints);
			for (const auto& [sample, count] : d.samples)
				logger::info("[DCLF] primary derivation differs: {:.2f}/frame {}", count / f, sample);
			logger::info("[DCLF] primary sun bits, per frame: {:.1f} agree, {:.1f} engine only, {:.1f} DCLF only, {:.1f} otherwise, {:.1f} unknown",
				d.sunAgree / f, d.sunEngineOnly / f, d.sunDclfOnly / f, d.sunOther / f, d.sunUnknown / f);
			for (const auto& [sample, count] : d.sunSamples)
				logger::info("[DCLF] primary sun bits differ: {:.2f}/frame {}", count / f, sample);
			logger::info("[DCLF] primary synthetic pass, per frame: {:.1f} agree in every field, {:.1f} not modelled; differ: technique {:.1f}, subPass {:.1f}, hint {:.1f}, LOD row {:.1f}",
				d.synthAgree / f, d.synthUnmodeled / f, d.synthTechnique / f, d.synthSubPass / f, d.synthHint / f, d.synthLodRow / f);
			for (const auto& [sample, count] : d.synthSamples)
				logger::info("[DCLF] primary synthetic pass differs: {:.2f}/frame {}", count / f, sample);
			derived = {};
		}
	}
}
