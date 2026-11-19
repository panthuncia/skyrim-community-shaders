// The sun's entry exclusion, the registration-skip thunks on its Accumulate, and the sun bits for removed entries.
// Runs on the render thread and on the registration jobs.
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunAccumulation.h"
#include "Features/DrawcallLimitFix/Core/Scene/EngineCasters.h"
#include "Features/DrawcallLimitFix/Core/Handoff/EngineOnlyCull.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Local/LocalLightCull.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PrimaryCull.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Sun/SunViews.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>

#include "State.h"
#include "Features/Skylighting.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"

namespace DCLF::Features::Shadows::Sun
{
	namespace
	{
		using namespace DCLF::Engine;

		// CalculateAndDrawShadowCasterLights calls FUN_141511f30. AE uses a CALL, SE a tail JMP at 0x1412e2f32 inside FUN_1412e2e10.
		constexpr DCLF::Engine::Offset kFullFrustumCallSite{ { 107132, 0x126 }, { 100418, 0x122 } };
		constexpr DCLF::Engine::Offset kFullFrustumCull{ { 108492 }, { 101498 } };
		constexpr DCLF::Engine::Offset kRegisterCallSites[2] = { { { 76558, 0xd3 }, { 74809, 0x73 } }, { { 76558, 0x199 }, { 74809, 0x159 } } };  // FUN_140e28af0 -> FUN_1414b2140
		constexpr DCLF::Engine::Offset kRegister{ { 106567 }, { 99923 } };
		constexpr DCLF::Engine::Offset kCascadeCallSite{ { 108488, 0xa8 }, { 101494, 0xb2 } };  // Accumulate -> FUN_1414f0920, once per cascade
		constexpr DCLF::Engine::Offset kCascade{ { 107602 }, { 100818 } };
		constexpr DCLF::Engine::Offset kCascadeCullCallSite{ { 106921, 0xfe }, { 100213, 0x166 } };  // FUN_1414bf320 (mode 2) -> FUN_140e305c0, once per full-frustum process
		constexpr DCLF::Engine::Offset kCascadeCull{ { 76773 }, { 75010 } };
		constexpr DCLF::Engine::Offset kMaskClearCallSite{ { 36559, 0x8ce }, { 35560, 0x8be }, { 36559, 0x8eb } };  // Main::Draw -> FUN_1414cb640, the activeLightMask clear
		constexpr DCLF::Engine::Offset kMaskClear{ { 107128 }, { 100410 } };
		constexpr DCLF::Engine::Offset kAppendNonAccum{ { 76772 }, { 75009 } };  // BSGeometryListCullingProcess::AppendNonAccum, vtable slot 0x19

		/** @brief Whether the instruction at a_address is a JMP rel32 to a_target. */
		bool JumpsTo(std::uintptr_t a_address, std::uintptr_t a_target)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			if (bytes[0] != 0xE9)
				return false;
			std::int32_t displacement = 0;
			std::memcpy(&displacement, bytes + 1, sizeof(displacement));
			return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
		}
		void (*appendNonAccum)(void*, RE::NiAVObject*, std::uint64_t, std::uint64_t) = nullptr;
		constexpr std::size_t kDescriptorAccumulator = 0x48;  // ShadowmapDescriptor::shaderAccumulator
		// The engine indexes the inner lists by its global count. Detours of the same function also read the outer BSTArray's size.
		using SceneList = RE::BSTArray<RE::NiPointer<RE::NiAVObject>>;
		RE::BSTArray<SceneList> emptySceneLists;
		SceneList emptyExtraNodes;
		// The whole skip's extra list when the frame has engine-only casters. Filled and emptied around the cull.
		SceneList targetedExtraNodes;
		constexpr std::uint32_t kMaxSceneLists = 32;
		// FUN_1414f0920's fourth argument, read as a BSTArray of process pointers with the size at +0x10.
		struct ProcessList
		{
			RE::BSCullingProcess* const* data = nullptr;
			std::uint32_t capacity = 0;
			std::uint32_t pad0C = 0;
			std::uint32_t size = 0;
			std::uint32_t pad14 = 0;
		};
		static_assert(sizeof(ProcessList) == 0x18 && offsetof(ProcessList, size) == 0x10);

		constexpr std::size_t kAccumulatorLightIndex = 0x160;  // shadow-light count + 1; 0xFFFF clears masks; 0 writes none
		constexpr std::size_t kAccumulatorLightBit = 0x164;    // 1 << count
		// BSGeometry (AE) and BSShaderProperty fields FUN_1414b2140 reads.
		constexpr std::size_t kGeometryShaderProperty = 0x128;
		constexpr std::size_t kGeometrySkinInstance = 0x130;
		constexpr std::size_t kGeometryRendererData = 0x138;
		constexpr std::size_t kGeometryType = 0x150;
		constexpr std::size_t kDismemberSkinReady = 0x98;

		/** @brief Whether the registration skip applies this frame. */
		bool M1Active()
		{
			return PassCapture::ShadowWithholdingEnabled() && !PassCapture::Get().Bypassed();
		}

		/** @brief Whether the sun's cascades skip DCLF's excluded entries this frame. */
		bool ExcludeSun(std::uint32_t a_frame)
		{
			// Parity frames keep the engine's work so Diagnostics/SunCasters can watch it.
			return !(Diagnostics::Observe().scene && Diagnostics::ParityDue(a_frame));
		}

		/** @brief Whether a bound lies outside any of the active planes (the engine's sphere test: n.c - d < -r). */
		bool Outside(const std::array<std::array<float, 4>, 6>& a_planes, std::uint32_t a_mask, const RE::NiBound& a_bound)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(a_mask & (1u << p)))
					continue;
				const auto& plane = a_planes[p];
				if (plane[0] * a_bound.center.x + plane[1] * a_bound.center.y + plane[2] * a_bound.center.z - plane[3] < -a_bound.radius)
					return true;
			}
			return false;
		}

		/** @brief The sun's Accumulate in progress on this thread, with the set each accumulator skips registrations for. */
		struct SunCall
		{
			std::vector<const void*> accumulators;
			std::vector<std::shared_ptr<const SetSnapshot>> claims;  // per cascade: the frame's set when DCLF draws its casters
			std::vector<std::uint32_t> descriptors;  // per accumulator: its descriptor index
			std::uint32_t count = 0;
			int cascade = -1;  // the cascade FUN_1414f0920 is culling (index into accumulators), -1 between them
			std::uint64_t skipped = 0;
			std::uint64_t registered = 0;

			int IndexOf(const void* a_accumulator) const
			{
				for (std::uint32_t i = 0; i < count; ++i)
					if (accumulators[i] == a_accumulator)
						return static_cast<int>(i);
				return -1;
			}
		};
		thread_local SunCall* currentCall = nullptr;

		// The sun's accumulators from the last Accumulate, to spot a sun registration made elsewhere. Republished whole on change.
		std::atomic<std::shared_ptr<const std::vector<const void*>>> knownAccumulators;

		/** @brief FUN_1414b2140's early-outs and activeLightMask write, without building any passes. */
		void WriteMaskOnly(void* a_accumulator, void* a_geometry)
		{
			auto* skin = At<RE::NiSkinInstance*>(a_geometry, kGeometrySkinInstance);
			if (skin && netimmerse_cast<RE::BSDismemberSkinInstance*>(skin) && At<std::uint8_t>(skin, kDismemberSkinReady) == 0)
				return;
			void* property = At<void*>(a_geometry, kGeometryShaderProperty);
			if (!property)
				return;
			if (!At<void*>(a_geometry, kGeometryRendererData) && !skin) {
				// The engine checks vfunc 0x10 and type byte 0xB before giving up.
				using Vfunc = void* (*)(void*);
				auto* vtable = *reinterpret_cast<Vfunc**>(a_geometry);
				if (!vtable[0x10](a_geometry) && At<std::uint8_t>(a_geometry, kGeometryType) != 0xB)
					return;
			}
			const std::uint32_t index = At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex);
			if (index == 0)
				return;
			void* lightData = At<void*>(property, kPropertyLightData);
			if (!lightData)
				return;
			auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
			mask = index == 0xFFFF ? 0u : (mask | At<std::uint32_t>(a_accumulator, kAccumulatorLightBit));
		}
	}

	bool SunAccumulation::IsWholeCullInput(const void* a_lists)
	{
		return a_lists == &emptySceneLists;
	}

	SunAccumulation& SunAccumulation::Get()
	{
		static SunAccumulation instance;
		return instance;
	}

	void SunAccumulation::ArmExclusion(RE::BSShadowDirectionalLight* a_light)
	{
		auto exclusion = Local::SunExclusionSlot::Get().Take();
		previousExclusion = std::move(frameState.exclusion);
		frameState.exclusion.reset();
		armedExclusion.reset();
		if (!M1Active() || !a_light)
			return;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node || node->GetRuntimeData().sunShadowDirLight != a_light)
			return;
		if (!exclusion || !exclusion->candidates || !exclusion->removed || !exclusion->cleared) {
			++stats.exclusionMissing;
			return;
		}
		// Without a verdict on this frame's candidates, an entry may hold a caster no epoch has drawn yet. The engine culls everything.
		if (exclusion->candidates != Scene::FrameView::Get().GetSunCandidates()) {
			++stats.exclusionStale;
			return;
		}
		vetoProcesses.clear();
		for (auto& process : a_light->GetShadowDirectionalLightRuntimeData().fullFrustumCullingProcessArray)
			if (process)
				vetoProcesses.push_back({ process.get() });
		vetoStamp = stampCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		armedExclusion = std::move(exclusion);
		vetoExclusion.store(armedExclusion.get(), std::memory_order_release);
	}

	bool SunAccumulation::ArmWhole(RE::BSShadowDirectionalLight* a_light)
	{
		if (!M1Active() || !a_light)
			return false;
		auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
		if (!node || node->GetRuntimeData().sunShadowDirLight != a_light)
			return false;
		// The exclusion needs every sun cascade view to be DCLF's this frame. Parity frames keep it so SunCasters can watch them.
		std::uint32_t cascades = 0;
		bool covered = ExcludeSun(Scene::FrameView::Get().GetFrame()) && Global<std::uint32_t>(Address::kSceneListCount) <= kMaxSceneLists;
		for (const auto& view : ShadowViews::Get().All()) {
			if (view.light != a_light || view.kind != ShadowViews::Kind::Directional || view.focus)
				continue;
			++cascades;
			covered = covered && view.covered && PassCapture::Get().CastersForBatch(view.batch);
		}
		if (!covered || cascades < a_light->GetRuntimeData().shadowmapDescriptors.size()) {
			// During a load hold the whole skip is kept rather than the engine's whole cull.
			if (!LoadHold::Active() || !ExcludeSun(Scene::FrameView::Get().GetFrame())) {
				++stats.wholeDeclined;
				return false;
			}
			LoadHold::NoteHeld(LoadHold::View::Sun);
			++stats.wholeHeld;
		}
		// An engine-only caster has only the engine's cull to draw its shadow, so the whole skip culls those roots alone.
		// The exclusion slot is still taken so its hand-off stays the same.
		previousExclusion = Local::SunExclusionSlot::Get().Take();
		frameState.exclusion.reset();
		armedExclusion.reset();
		vetoStamp = stampCounter.fetch_add(1, std::memory_order_relaxed) + 1;
		wholeCleared.NewRound();
		frameState.stamp = vetoStamp;
		frameState.whole = true;
		frameState.skipExcluded = true;
		++stats.wholeFrames;
		return true;
	}

	void SunAccumulation::WholeApplied()
	{
		Diagnostics::Notify(Diagnostics::SunExclusionApplied{ true });
		exclusionLive.store(true, std::memory_order_release);
	}

	bool SunAccumulation::ClearedThisFrame(const void* a_geometry, bool a_clears)
	{
		const auto found = wholeCleared.Test(a_geometry, a_clears);
		if (found == FrameAddressSet::Found::kFull)
			bitStats.wholeUnstamped.fetch_add(1, std::memory_order_relaxed);
		return found == FrameAddressSet::Found::kYes;
	}

	void SunAccumulation::ExcludeEntries()
	{
		// JobList::Finish has run, so the jobs' appends and counts are visible here.
		if (!vetoExclusion.exchange(nullptr, std::memory_order_acq_rel))
			return;
		const std::int64_t start = Now();
		auto exclusion = std::move(armedExclusion);
		const std::uint32_t stamp = vetoStamp;
		const auto& candidates = *exclusion->candidates;
		for (auto& p : vetoProcesses) {
			stats.entriesSeen += p.seen;
			// A process left empty gets its first entry back, because the cascade cull sets its planes up from the first.
			if (p.vetoed && p.vetoed == p.seen) {
				exclusion->removed[p.firstEntry].store(0, std::memory_order_relaxed);
				appendNonAccum(const_cast<void*>(p.process), p.firstVetoed, 0, 0);
				--p.vetoed;
				p.givenBack = true;
			}
			stats.entriesRemoved += p.vetoed;
		}
		if (Diagnostics::Observe().scene && Diagnostics::ParityDue(stamp)) {
			std::uint32_t left = 0, givenBack = 0;
			for (const auto& p : vetoProcesses) {
				const auto& array = static_cast<const RE::BSCullingProcess*>(p.process)->objectArray;
				for (std::uint32_t r = 0; r < array.size(); ++r)
					if (const auto it = candidates.entries.find(array[r].get()); it != candidates.entries.end() && exclusion->excluded[it->second])
						++left;
				givenBack += p.givenBack ? 1 : 0;
			}
			if (left > givenBack) {
				static std::atomic<std::uint32_t> logged{ 0 };
				if (logged++ < 8)
					logger::warn("[DCLF] sun entry exclusion: {} excluded entries in the full-frustum arrays after the veto ({} given back) <- VETO", left, givenBack);
			}
		}
		++stats.exclusionFrames;
		stats.candidates += candidates.Count();
		stats.excluded += exclusion->excludedCount;
		frameState.exclusion = std::move(exclusion);
		frameState.stamp = stamp;
		frameState.skipExcluded = ExcludeSun(Scene::FrameView::Get().GetFrame());
		Diagnostics::Notify(Diagnostics::SunExclusionApplied{ frameState.skipExcluded });
		exclusionLive.store(true, std::memory_order_release);
		stats.filterTicks += Now() - start;
	}

	void SunAccumulation::ResetFrameSun()
	{
		bitsReady.store(false, std::memory_order_relaxed);
		exclusionLive.store(false, std::memory_order_relaxed);
		frameState.whole = false;
		for (auto& cascade : frameState.cascades)
			cascade = {};
		frameState.cascadeCount = 0;
		frameState.sunBits = 0;
		Diagnostics::Notify(Diagnostics::SunBitsSet{ 0 });
	}

	void SunAccumulation::EndFullFrustumWindow()
	{
		// On a frame without the full-frustum cull, such as an interior, nothing of the last frame stays live.
		if (!std::exchange(fullFrustumRan, false))
			ResetFrameSun();
	}

	std::uint32_t SunAccumulation::RemovedGeometryIndex(const RE::BSGeometry* a_geometry) const
	{
		const auto* exclusion = frameState.exclusion.get();
		if (!exclusion)
			return ~0u;
		const auto& candidates = *exclusion->candidates;
		const auto it = candidates.geometries.find(a_geometry);
		if (it == candidates.geometries.end() || exclusion->removed[candidates.geometryEntry[it->second]].load(std::memory_order_relaxed) != frameState.stamp)
			return ~0u;
		return it->second;
	}

	void SunAccumulation::ApplySunBits(RE::BSGeometry* a_geometry, bool a_clears)
	{
		void* property = At<void*>(a_geometry, kGeometryShaderProperty);
		if (!property)
			return;
		void* lightData = At<void*>(property, kPropertyLightData);
		if (!lightData)
			return;
		const auto& state = frameState;
		const bool whole = state.whole;
		if (whole) {
			if (ClearedThisFrame(a_geometry, a_clears))
				return;
		} else {
			const std::uint32_t index = RemovedGeometryIndex(a_geometry);
			if (index == ~0u)
				return;
			auto& cleared = state.exclusion->cleared[index];
			if (cleared.load(std::memory_order_relaxed) == state.stamp)
				return;
			if (a_clears)
				cleared.store(state.stamp, std::memory_order_relaxed);
		}
		const auto& bound = a_geometry->worldBound;
		std::uint32_t bits = 0;
		for (std::uint32_t c = 0; c < state.cascadeCount && c < state.cascades.size(); ++c) {
			const auto& cascade = state.cascades[c];
			if (cascade.captured && !Outside(cascade.planes, cascade.planeMask, bound) &&
				!(cascade.customMask && Outside(cascade.customPlanes, cascade.customMask, bound)))
				bits |= cascade.bit;
		}
		auto& mask = At<std::uint32_t>(lightData, kLightDataActiveMask);
		mask |= bits;
		(whole ? bitStats.wholeWritten : bitStats.written).fetch_add(1, std::memory_order_relaxed);
		if (bits)
			(whole ? bitStats.wholeWithBits : bitStats.withBits).fetch_add(1, std::memory_order_relaxed);
	}

	struct SunAccumulation::Hooks
	{
		/** @brief BSShadowDirectionalLight::Accumulate (vfunc 9, AE 0x141511c80). */
		struct Accumulate
		{
			static void thunk(RE::BSShadowDirectionalLight* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				if (!node || node->GetRuntimeData().sunShadowDirLight != a_light || !ShellState::Running()) {
					func(a_light, a_count, a_arg2, a_arg3);
					return;
				}
				auto& self = SunAccumulation::Get();
				SunCall call;
				auto& descriptors = a_light->GetRuntimeData().shadowmapDescriptors;
				const bool active = M1Active();
				call.accumulators.reserve(descriptors.size());
				call.claims.reserve(descriptors.size());
				for (std::uint32_t i = 0; i < descriptors.size(); ++i) {
					auto* accumulator = descriptors[i].shaderAccumulator.get();
					if (!accumulator)
						continue;
					call.accumulators.push_back(accumulator);
					call.descriptors.push_back(i);
					const auto* data = active ? accumulator->GetRuntimeData() : nullptr;
					call.claims.push_back(data ? PassCapture::Get().CastersForBatch(data->batchRenderer) : nullptr);
					++call.count;
				}
				if (const auto known = knownAccumulators.load(std::memory_order_acquire); !known || *known != call.accumulators)
					knownAccumulators.store(std::make_shared<const std::vector<const void*>>(call.accumulators), std::memory_order_release);
				// Make room before capture. Registration threads read the cascades only after bitsReady is set, and it is clear here.
				if (self.frameState.cascades.size() < call.count)
					self.frameState.cascades.resize(call.count);
				const std::uint32_t frame = Scene::FrameView::Get().GetFrame();
				Diagnostics::Notify<Diagnostics::EnginePoint::SunAccumulateBegin>();
				// A cascade without a DCLF view stays unknown and its bit unset.
				for (std::uint32_t i = 0; i < call.count; ++i) {
					auto& cascade = self.frameState.cascades[i];
					cascade = {};
					if (const auto* mine = SunBoard().CascadeOf(call.descriptors[i], frame)) {
						cascade.planes = mine->cull.plane;
						cascade.planeMask = mine->cull.mask;
						cascade.customPlanes = mine->caster.plane;
						cascade.customMask = mine->caster.mask;
						cascade.captured = true;
					} else {
						Diagnostics::Notify(Diagnostics::SunCascadeMissing{ frame, call.descriptors[i] });
					}
				}
				currentCall = &call;
				const std::int64_t start = Now();
				func(a_light, a_count, a_arg2, a_arg3);
				const std::int64_t ticks = Now() - start;
				currentCall = nullptr;
				// From here until the next mask clear, removed entries' main registrations take DCLF's bits.
				{
					auto& state = self.frameState;
					state.cascadeCount = call.count;
					state.cascadesFrame = frame;
					state.sunBits = 0;
					for (std::uint32_t i = 0; i < call.count; ++i)
						state.sunBits |= state.cascades[i].captured ? state.cascades[i].bit : 0u;
					Diagnostics::Notify(Diagnostics::SunBitsSet{ state.sunBits });
					if (self.exclusionLive.load(std::memory_order_relaxed))
						self.bitsReady.store(true, std::memory_order_release);
				}

				auto& stats = self.stats;
				++stats.frames;
				stats.activeFrames += active ? 1 : 0;
				stats.skipped += call.skipped;
				stats.registered += call.registered;
				stats.accumulateTicks += ticks;
				stats.accumulateMax = std::max(stats.accumulateMax, ticks);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Leaves an owned geometry's activeLightMask at 0, as the engine would after its main registration read it.
		 * DCLF draws its main pass, so no main registration ever consumes the shadow lights' bits.
		 */
		static void ClearOwnedMask(void* a_geometry)
		{
			if (void* property = At<void*>(a_geometry, kGeometryShaderProperty))
				if (void* lightData = At<void*>(property, kPropertyLightData))
					At<std::uint32_t>(lightData, kLightDataActiveMask) = 0;
		}

		/** @brief FUN_140e28af0's calls of the registration FUN_1414b2140(accumulator, geometry, arg). */
		template <std::uint32_t Site>
		struct Register
		{
			static std::uint64_t thunk(void* a_accumulator, void* a_geometry, std::uint64_t a_arg)
			{
				SunCall* call = currentCall;
				const int cascade = call ? call->IndexOf(a_accumulator) : -1;
				auto& self = SunAccumulation::Get();
				auto* geometry = static_cast<RE::BSGeometry*>(a_geometry);
				if (cascade < 0) {
					if (!call) {
						if (const auto known = knownAccumulators.load(std::memory_order_acquire);
							known && std::find(known->begin(), known->end(), a_accumulator) != known->end())
							self.offThread.fetch_add(1, std::memory_order_relaxed);
					}
					// A shadow light's accumulation sets +0x160 to neither 0 nor 0xFFFF.
					const std::uint32_t lightIndex = At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex);
					const bool owned = PrimaryCull::Get().Owned(*geometry);
					if (owned) {
						const auto result = func(a_accumulator, a_geometry, a_arg);
						if (lightIndex != 0 && lightIndex != 0xFFFF)
							ClearOwnedMask(a_geometry);
						return result;
					}
					// The main camera's registrations read the mask, then clear it (+0x160 = 0xFFFF).
					if (self.bitsReady.load(std::memory_order_acquire))
						self.ApplySunBits(geometry, At<std::uint32_t>(a_accumulator, kAccumulatorLightIndex) == 0xFFFF);
					else if (!call && self.exclusionLive.load(std::memory_order_acquire) && self.UnderRemovedEntry(geometry))
						self.bitStats.notReady.fetch_add(1, std::memory_order_relaxed);
					if (lightIndex == 0xFFFF)
						Local::LocalLightCull::NoteMainRegistration(geometry);
					return func(a_accumulator, a_geometry, a_arg);
				}
				std::uint64_t result = 1;
				const auto& claims = call->claims[cascade];
				const bool claimed = claims && (claims->PhasesOf(geometry) & kSetCaster);
				// A removed entry reaching a cascade was culled through another path. Checked every 64th cull.
				if (self.exclusionLive.load(std::memory_order_relaxed) && (self.frameState.stamp & 63) == 0 && self.UnderRemovedEntry(geometry))
					++self.stats.cascadeRegistrationsUnderRemoved;
				if (claimed) {
					if (PrimaryCull::Get().Owned(*geometry))
						ClearOwnedMask(a_geometry);
					else
						WriteMaskOnly(a_accumulator, a_geometry);
					++call->skipped;
				} else {
					const std::uint32_t passesBefore = PassCapture::PassesOnThisThread();
					result = func(a_accumulator, a_geometry, a_arg);
					// A registration that built no pass draws nothing.
					if (PassCapture::PassesOnThisThread() != passesBefore)
						Diagnostics::Notify(Diagnostics::SunCasterRegistered{ geometry });
					if (PrimaryCull::Get().Owned(*geometry))
						ClearOwnedMask(a_geometry);
					++call->registered;
				}
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief CalculateAndDrawShadowCasterLights' call of the full-frustum cull (FUN_141511f30). */
		struct FullFrustum
		{
			static void thunk(RE::BSShadowDirectionalLight* a_light, RE::BSTArray<SceneList>& a_lists, SceneList& a_nodes)
			{
				auto& self = SunAccumulation::Get();
				self.ResetFrameSun();
				self.fullFrustumRan = true;
				// With DCLF off, view coverage is not decided, so the engine culls everything.
				if (!ShellState::Running()) {
					func(a_light, a_lists, a_nodes);
					return;
				}
				if (self.ArmWhole(a_light)) {
					// Run the engine's cull over empty scene lists so the processes are set up but the cascades cull nothing.
					// A null job list makes the empty culls run inline. Only the render thread reads the job list while the game runs.
					emptySceneLists.resize(Global<std::uint32_t>(Address::kSceneListCount));
					auto& jobs = Global<void*>(Address::kListCullJobs);
					void* const held = jobs;
					const std::int64_t start = Now();
					jobs = nullptr;
					const auto& subset = Scene::FrameView::Get().GetEngineCasters();
					if (const auto* casters = subset ? &subset->Of(Scene::EngineOnlySubset::kCaster) : nullptr; casters && !casters->roots.empty()) {
						for (const auto* root : casters->roots)
							targetedExtraNodes.push_back(RE::NiPointer<RE::NiAVObject>(const_cast<RE::NiAVObject*>(root)));
						++self.stats.wholeTargeted;
						self.stats.wholeTargetedRoots += casters->roots.size();
						func(a_light, emptySceneLists, targetedExtraNodes);
						targetedExtraNodes.clear();
					} else {
						func(a_light, emptySceneLists, emptyExtraNodes);
					}
					jobs = held;
					self.WholeApplied();
					const std::int64_t ticks = Now() - start;
					self.stats.fullFrustumTicks += ticks;
					self.stats.fullFrustumMax = std::max(self.stats.fullFrustumMax, ticks);
					return;
				}
				self.ArmExclusion(a_light);
				// The engine culls the sun whole when a view is not covered yet. Not on parity frames, or when the shadows are not DCLF's.
				if (M1Active() && ExcludeSun(Scene::FrameView::Get().GetFrame()))
					EngineOnlyCull::NoteWholeCull(self.vetoExclusion.load(std::memory_order_relaxed) ? EngineOnlyCull::WholeCull::SunNotCovered :
					                                                                                     EngineOnlyCull::WholeCull::SunNoExclusion);
				const std::int64_t start = Now();
				func(a_light, a_lists, a_nodes);
				const std::int64_t ticks = Now() - start;
				auto& stats = self.stats;
				stats.fullFrustumTicks += ticks;
				stats.fullFrustumMax = std::max(stats.fullFrustumMax, ticks);
				self.ExcludeEntries();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief AppendNonAccum (vtable slot 0x19, AE 0x140e30520), the only push into a full-frustum process's objectArray.
		 * While armed, it drops excluded entries at insertion on the cull's job threads. Other processes of this class pass through.
		 */
		struct AppendNonAccum
		{
			static void thunk(void* a_process, RE::NiAVObject* a_object, std::uint64_t a_2, std::uint64_t a_3)
			{
				auto& self = SunAccumulation::Get();
				if (const auto* exclusion = self.vetoExclusion.load(std::memory_order_acquire); exclusion && a_object) {
					for (auto& p : self.vetoProcesses) {
						if (p.process != a_process)
							continue;
						++p.seen;
						const auto& entries = exclusion->candidates->entries;
						if (const auto it = entries.find(a_object); it != entries.end() && exclusion->excluded[it->second]) {
							exclusion->removed[it->second].store(self.vetoStamp, std::memory_order_relaxed);
							if (!p.vetoed++) {
								p.firstVetoed = a_object;
								p.firstEntry = it->second;
							}
							return;
						}
						break;
					}
				}
				func(a_process, a_object, a_2, a_3);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::Draw's activeLightMask clear (FUN_1414cb640). Until the sun accumulates again, no sun bits are written. */
		struct MaskClear
		{
			static void thunk(std::uint64_t a_1, std::uint64_t a_2, std::uint64_t a_3, std::uint64_t a_4)
			{
				func(a_1, a_2, a_3, a_4);
				SunAccumulation::Get().bitsReady.store(false, std::memory_order_relaxed);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Accumulate's per-cascade call of FUN_1414f0920(light, descriptor, count, processes, arg). */
		struct Cascade
		{
			static void thunk(void* a_light, void* a_descriptor, std::uint32_t* a_count, void* a_processes, std::uint32_t a_arg)
			{
				SunCall* call = currentCall;
				auto& self = SunAccumulation::Get();
				if (call && a_descriptor) {
					void* accumulator = At<void*>(a_descriptor, kDescriptorAccumulator);
					call->cascade = call->IndexOf(accumulator);
					if (call->cascade >= 0) {
						auto& cascade = self.frameState.cascades[call->cascade];
						cascade.bit = At<std::uint32_t>(accumulator, kAccumulatorLightBit);
					}
				}
				// In the whole skip only processes holding entries are culled. An empty one would still cost a cull's set-up and a registration
				// under the context's lock (FUN_140e28af0).
				if (call && self.frameState.whole && a_processes) {
					std::array<RE::BSCullingProcess*, kMaxSceneLists> held{};
					ProcessList kept{ held.data() };
					std::uint32_t left = 0;
					for (auto& process : *static_cast<RE::BSTArray<RE::NiPointer<RE::BSCullingProcess>>*>(a_processes)) {
						if (process && !process->objectArray.empty() && kept.size < held.size())
							held[kept.size++] = process.get();
						else
							++left;
					}
					kept.capacity = kept.size;
					self.stats.wholeProcessesCulled += kept.size;
					self.stats.wholeProcessesLeft += left;
					func(a_light, a_descriptor, a_count, &kept, a_arg);
				} else {
					func(a_light, a_descriptor, a_count, a_processes, a_arg);
				}
				if (call)
					call->cascade = -1;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Process1 (vtable slot 0x16) for the sun's cascade culls. Skips excluded entries still reached through a node above.
		 * Only inside the sun's Accumulate on its own thread, and only while skipExcluded is set.
		 */
		struct CascadeProcess1
		{
			static void thunk(RE::NiCullingProcess* a_process, RE::NiAVObject* a_object, std::int32_t a_arg)
			{
				if (SunCall* call = currentCall; call && call->cascade >= 0 && a_object) {
					auto& self = SunAccumulation::Get();
					const auto& state = self.frameState;
					if (self.exclusionLive.load(std::memory_order_relaxed) && state.skipExcluded && state.exclusion && state.exclusion->candidates) {
						const auto& entries = state.exclusion->candidates->entries;
						if (const auto it = entries.find(a_object); it != entries.end() && state.exclusion->excluded[it->second]) {
							state.exclusion->removed[it->second].store(state.stamp, std::memory_order_relaxed);
							self.skipStats.skipped.fetch_add(1, std::memory_order_relaxed);
							return;
						}
					}
				}
				func(a_process, a_object, a_arg);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief FUN_1414bf320's cascade cull, FUN_140e305c0(full-frustum process, cascade process, arg). */
		struct CascadeCull
		{
			static void thunk(void* a_fullProcess, RE::NiCullingProcess* a_process, std::uint64_t a_arg)
			{
				func(a_fullProcess, a_process, a_arg);
				// The traversal's first call (vfunc 0xB8) set the process up from the cascade's camera. With an empty objectArray it did not,
				// and the planes are not this cascade's.
				SunCall* call = currentCall;
				if (!call || call->cascade < 0 || !a_process)
					return;
				Diagnostics::Notify(Diagnostics::SunCascadeCulled{ a_fullProcess, a_process, static_cast<std::uint32_t>(call->cascade) });
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunAccumulation::Install()
	{
		if (installed)
			return;
		const auto base = REL::Module::get().base();
		const auto fullFrustum = base + kFullFrustumCallSite;
		const auto registerA = base + kRegisterCallSites[0];
		const auto registerB = base + kRegisterCallSites[1];
		const auto cascadeCall = base + kCascadeCallSite;
		const auto cascadeCull = base + kCascadeCullCallSite;
		const auto maskClear = base + kMaskClearCallSite;
		// SE's full-frustum site is a tail JMP, patched as one so the thunk returns to the sun part's caller.
		const bool fullFrustumJumps = REL::Module::IsSE();
		// Preflight checked every site. The cascade cull and the mask clear chain onto whatever they call.
		if (!CallsTo(cascadeCull, base + kCascadeCull))
			logger::info("[DCLF] sun accumulation: the cascade cull's call is hooked by {}; DCLF's thunk chains onto it", HookSites::BranchTargetModule(cascadeCull));
		if (!CallsTo(maskClear, base + kMaskClear))
			logger::info("[DCLF] sun accumulation: the light mask clear's call is hooked by {}; DCLF's thunk chains onto it", HookSites::BranchTargetModule(maskClear));
		stl::write_vfunc<0x19, Hooks::AppendNonAccum>(RE::VTABLE_BSGeometryListCullingProcess[0]);
		appendNonAccum = reinterpret_cast<decltype(appendNonAccum)>(Hooks::AppendNonAccum::func.address());
		stl::write_vfunc<0x9, Hooks::Accumulate>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_thunk_call<Hooks::Register<0>>(registerA);
		stl::write_thunk_call<Hooks::Register<1>>(registerB);
		if (fullFrustumJumps)
			stl::write_thunk_jmp<Hooks::FullFrustum>(fullFrustum);
		else
			stl::write_thunk_call<Hooks::FullFrustum>(fullFrustum);
		stl::write_thunk_call<Hooks::Cascade>(cascadeCall);
		stl::write_thunk_call<Hooks::CascadeCull>(cascadeCull);
		stl::write_thunk_call<Hooks::MaskClear>(maskClear);
		stl::write_vfunc<0x16, Hooks::CascadeProcess1>(RE::VTABLE_BSCullingProcess[0]);
		installed = true;
		logger::info("[DCLF] sun accumulation installed (Accumulate, its cascades and registrations, the full-frustum cull)");
	}

	void SunAccumulation::Preflight()
	{
		const auto base = REL::Module::get().base();
		if (REL::Module::IsSE())
			HookSites::RequireJumpTo("the sun's full-frustum cull", base + kFullFrustumCallSite, base + kFullFrustumCull);
		else
			HookSites::RequireCallTo("the sun's full-frustum cull", base + kFullFrustumCallSite, base + kFullFrustumCull);
		HookSites::RequireCallTo("the sun registration's first call", base + kRegisterCallSites[0], base + kRegister);
		HookSites::RequireCallTo("the sun registration's second call", base + kRegisterCallSites[1], base + kRegister);
		HookSites::RequireCallTo("the sun's per-cascade accumulation", base + kCascadeCallSite, base + kCascade);
		HookSites::RequireCall("the sun's cascade cull", base + kCascadeCullCallSite);
		HookSites::RequireCall("Main::Draw's light mask clear", base + kMaskClearCallSite);
		HookSites::RequireSlot("BSGeometryListCullingProcess::AppendNonAccum", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSGeometryListCullingProcess[0] }.address(), 0x19,
			base + kAppendNonAccum);
		HookSites::RequireSlot("BSCullingProcess::Process1", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSCullingProcess[0] }.address(), 0x16);
	}

	SunAccumulation::Stats SunAccumulation::TakeStats()
	{
		stats.offThread = offThread.exchange(0, std::memory_order_relaxed);
		const auto forward = Local::SunExclusionSlot::Get().TakeForwardCounts();
		stats.exclusionForwarded = forward.forwarded;
		stats.exclusionUnforwarded = forward.unforwarded;
		return std::exchange(stats, {});
	}

	SunAccumulation::ExclusionCounts SunAccumulation::TakeExclusionCounts()
	{
		const auto take = [](std::atomic<std::uint64_t>& a_value) { return a_value.exchange(0, std::memory_order_relaxed); };
		ExclusionCounts c;
		c.written = take(bitStats.written);
		c.withBits = take(bitStats.withBits);
		c.notReady = take(bitStats.notReady);
		c.skipped = take(skipStats.skipped);
		c.wholeWritten = take(bitStats.wholeWritten);
		c.wholeWithBits = take(bitStats.wholeWithBits);
		c.wholeUnstamped = take(bitStats.wholeUnstamped);
		return c;
	}
}
