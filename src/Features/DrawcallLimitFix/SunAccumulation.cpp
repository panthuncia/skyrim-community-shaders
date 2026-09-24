#include "SunAccumulation.h"

#include "PassCapture.h"
#include "ShadowProbe.h"
#include "Switches.h"
#include "Toggles.h"
#include "VolumetricProbe.h"

#include <array>
#include <cstring>

#include "State.h"

namespace DCLF
{
	namespace
	{
		// AE 1.6.1170 (module offsets).
		constexpr std::uintptr_t kFullFrustumCallSite = 0x14cbcb6;  // CalculateAndDrawShadowCasterLights -> FUN_141511f30
		constexpr std::uintptr_t kFullFrustumCull = 0x1511f30;
		constexpr std::uintptr_t kRegisterCallSites[2] = { 0xe28bc3, 0xe28c89 };  // FUN_140e28af0 -> FUN_1414b2140
		constexpr std::uintptr_t kRegister = 0x14b2140;

		// BSShaderAccumulator fields the sun's Accumulate sets per cascade (FUN_1414b2140 reads them).
		constexpr std::size_t kAccumulatorLightIndex = 0x160;  // shadow-light count + 1; 0xFFFF clears masks; 0 writes none
		constexpr std::size_t kAccumulatorLightBit = 0x164;    // 1 << count
		// BSGeometry (AE) and BSShaderProperty fields FUN_1414b2140 reads.
		constexpr std::size_t kGeometryShaderProperty = 0x128;
		constexpr std::size_t kGeometrySkinInstance = 0x130;
		constexpr std::size_t kGeometryRendererData = 0x138;
		constexpr std::size_t kGeometryType = 0x150;
		constexpr std::size_t kDismemberSkinReady = 0x98;
		constexpr std::size_t kPropertyLightData = 0x70;
		constexpr std::size_t kLightDataActiveMask = 0x1C;
		constexpr std::uint32_t kMaxCascades = 4;

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
		T& At(void* a_base, std::size_t a_offset)
		{
			return *reinterpret_cast<T*>(static_cast<std::byte*>(a_base) + a_offset);
		}

		bool TimingEnabled()
		{
			static const bool enabled = SwitchEnabled("CS_DCLF_SUN_TIMING");
			return enabled;
		}

		/** @brief Diagnostics that need the engine's sun registrations: M1 stays off for the whole run. */
		bool RegistrationsNeeded()
		{
			static const bool needed = PassCapture::CascadeProbeEnabled() || VolumetricProbe::Enabled() || ShadowProbe::Enabled();
			return needed;
		}

		std::int64_t Now()
		{
			LARGE_INTEGER value{};
			QueryPerformanceCounter(&value);
			return value.QuadPart;
		}

		/**
		 * @brief The sun's Accumulate in progress on this thread: its cascade accumulators and, per accumulator, the
		 * claim set its registrations are skipped for (null: registered as usual).
		 */
		struct SunCall
		{
			std::array<const void*, kMaxCascades> accumulators{};
			std::array<std::shared_ptr<const PassCapture::ClaimSet>, kMaxCascades> claims{};
			std::uint32_t count = 0;
			std::int64_t registrationTicks = 0;
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
		// The sun's accumulators as the last Accumulate saw them, for spotting a sun registration made elsewhere.
		std::array<std::atomic<const void*>, kMaxCascades> knownAccumulators{};

		/**
		 * @brief FUN_1414b2140 without the mode's registration: its early-outs (a dismemberment skin not yet set up, no
		 * shader property, nothing to draw) and its activeLightMask write, from the accumulator's fields.
		 */
		void WriteMaskOnly(void* a_accumulator, void* a_geometry)
		{
			auto* skin = At<RE::NiSkinInstance*>(a_geometry, kGeometrySkinInstance);
			if (skin && netimmerse_cast<RE::BSDismemberSkinInstance*>(skin) && At<std::uint8_t>(skin, kDismemberSkinReady) == 0)
				return;
			void* property = At<void*>(a_geometry, kGeometryShaderProperty);
			if (!property)
				return;
			if (!At<void*>(a_geometry, kGeometryRendererData) && !skin) {
				// vfunc 0x10 of the geometry, and the type byte 0xB: what the engine checks before giving up.
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

	SunAccumulation& SunAccumulation::Get()
	{
		static SunAccumulation instance;
		return instance;
	}

	struct SunAccumulation::Hooks
	{
		/** @brief BSShadowDirectionalLight::Accumulate (vfunc 9, AE 0x141511c80). */
		struct Accumulate
		{
			static void thunk(RE::BSShadowDirectionalLight* a_light, std::uint32_t* a_count, std::uint32_t* a_arg2, RE::NiAVObject* a_arg3)
			{
				auto* node = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				if (!node || node->GetRuntimeData().sunShadowDirLight != a_light) {
					func(a_light, a_count, a_arg2, a_arg3);
					return;
				}
				auto& self = SunAccumulation::Get();
				SunCall call;
				auto& descriptors = a_light->GetRuntimeData().shadowmapDescriptors;
				const auto toggles = Toggles::Get().Active();
				const bool active = toggles.skipSunAccumulation && PassCapture::ShadowWithholdingEnabled() && PassCapture::VolumetricClaimsAvailable() &&
				                    !PassCapture::Get().Bypassed() && !RegistrationsNeeded();
				for (std::uint32_t i = 0; i < descriptors.size() && call.count < kMaxCascades; ++i) {
					auto* accumulator = descriptors[i].shaderAccumulator.get();
					if (!accumulator)
						continue;
					call.accumulators[call.count] = accumulator;
					knownAccumulators[call.count].store(accumulator, std::memory_order_relaxed);
					// The claims PassCapture would withhold this accumulator's passes by: its batch renderer's
					// render mode's, when the renderer is a shadow view's.
					if (active)
						if (const auto* data = accumulator->GetRuntimeData())
							call.claims[call.count] = PassCapture::Get().ShadowClaimsForBatch(data->batchRenderer);
					++call.count;
				}
				currentCall = &call;
				const std::int64_t start = Now();
				func(a_light, a_count, a_arg2, a_arg3);
				const std::int64_t ticks = Now() - start;
				currentCall = nullptr;

				auto& stats = self.stats;
				++stats.frames;
				stats.activeFrames += active ? 1 : 0;
				stats.skipped += call.skipped;
				stats.registered += call.registered;
				stats.accumulateTicks += ticks;
				stats.accumulateMax = std::max(stats.accumulateMax, ticks);
				stats.registrationTicks += call.registrationTicks;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief FUN_140e28af0's calls of the accumulator's registration, FUN_1414b2140(accumulator, geometry, arg). */
		template <std::uint32_t Site>
		struct Register
		{
			static std::uint64_t thunk(void* a_accumulator, void* a_geometry, std::uint64_t a_arg)
			{
				SunCall* call = currentCall;
				const int cascade = call ? call->IndexOf(a_accumulator) : -1;
				if (cascade < 0) {
					if (!call) {
						for (const auto& known : knownAccumulators)
							if (known.load(std::memory_order_relaxed) == a_accumulator) {
								SunAccumulation::Get().offThread.fetch_add(1, std::memory_order_relaxed);
								break;
							}
					}
					return func(a_accumulator, a_geometry, a_arg);
				}
				const bool timing = TimingEnabled();
				const std::int64_t start = timing ? Now() : 0;
				std::uint64_t result = 1;
				const auto& claims = call->claims[cascade];
				if (claims && claims->contains(static_cast<const RE::BSGeometry*>(a_geometry))) {
					WriteMaskOnly(a_accumulator, a_geometry);
					++call->skipped;
				} else {
					result = func(a_accumulator, a_geometry, a_arg);
					++call->registered;
				}
				if (timing)
					call->registrationTicks += Now() - start;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief CalculateAndDrawShadowCasterLights' call of the full-frustum cull (FUN_141511f30). */
		struct FullFrustum
		{
			static void thunk(void* a_light, void* a_lists, void* a_arg)
			{
				const std::int64_t start = Now();
				func(a_light, a_lists, a_arg);
				const std::int64_t ticks = Now() - start;
				auto& stats = SunAccumulation::Get().stats;
				stats.fullFrustumTicks += ticks;
				stats.fullFrustumMax = std::max(stats.fullFrustumMax, ticks);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	};

	void SunAccumulation::Install()
	{
		if (installed || !REL::Module::IsAE())
			return;
		const auto base = REL::Module::get().base();
		const auto fullFrustum = base + kFullFrustumCallSite;
		const auto registerA = base + kRegisterCallSites[0];
		const auto registerB = base + kRegisterCallSites[1];
		if (!CallsTo(fullFrustum, base + kFullFrustumCull) || !CallsTo(registerA, base + kRegister) || !CallsTo(registerB, base + kRegister)) {
			logger::warn("[DCLF] sun accumulation: the calls are not where expected; the engine keeps the sun's accumulation");
			return;
		}
		stl::write_vfunc<0x9, Hooks::Accumulate>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_thunk_call<Hooks::Register<0>>(registerA);
		stl::write_thunk_call<Hooks::Register<1>>(registerB);
		stl::write_thunk_call<Hooks::FullFrustum>(fullFrustum);
		installed = true;
		logger::info("[DCLF] sun accumulation installed (Accumulate, its registrations, the full-frustum cull)");
	}

	void SunAccumulation::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if (!installed || (a_frame % a_interval) != 0)
			return;
		stats.offThread = offThread.exchange(0, std::memory_order_relaxed);
		if (stats.frames) {
			LARGE_INTEGER frequency{};
			QueryPerformanceFrequency(&frequency);
			const double toMs = 1000.0 / static_cast<double>(frequency.QuadPart);
			const double frames = stats.frames;
			std::string timing;
			if (TimingEnabled())
				timing = fmt::format("; per frame, render thread: full-frustum cull {:.3f} ms (max {:.3f}), Accumulate {:.3f} ms (max {:.3f}), of which registration {:.3f} ms",
					stats.fullFrustumTicks * toMs / frames, stats.fullFrustumMax * toMs, stats.accumulateTicks * toMs / frames, stats.accumulateMax * toMs,
					stats.registrationTicks * toMs / frames);
			logger::info("[DCLF] sun accumulation: skipping registration on {} of {} frames; {:.0f} claimed geometries a frame not registered, {:.0f} registered by the engine, {} sun registrations outside Accumulate{}",
				stats.activeFrames, stats.frames, stats.skipped / frames, stats.registered / frames, stats.offThread, timing);
		}
		stats = {};
	}
}
