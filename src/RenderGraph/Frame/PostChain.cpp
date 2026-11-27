#include "RenderGraph/Frame/PostChain.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"

#include "Deferred.h"
#include "Features/Upscaling.h"
#include "Features/VolumetricLighting/ORGVolumetricLighting.h"
#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpaceBlock.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "State.h"

#include <algorithm>
#include <functional>
#include <array>
#include <map>
#include <mutex>
#include <unordered_map>

namespace RenderGraph::Frame::PostChain
{
	namespace
	{
		// Engine globals the chain reads, each the value the engine's own test reads (SE 1.5.97 / AE 1.6.1170).
		// RenderEndOfFrameEffects (99023 / 105674) counts the active end-of-frame slots only when this is set.
		constexpr REL::RelocationID kEnableEndOfFrame{ 513299, 391055 };
		// When set the HDR effect (effects[30]) renders, else a copy (effects[34]).
		constexpr REL::RelocationID kHdrEnabled{ 513463, 391301 };
		// Tested with the VL effect's active flag by RenderWorldPostProcess (100430 / 107148) and the VL composite (100493).
		constexpr REL::RelocationID kVolumetricEnabled{ 527940, 414913 };
		// ImageSpaceEffectHDR::Render (100465 / 107183) state. The adapt target is below 0 when there is none.
		// The last-level parity flips on each render.
		constexpr REL::RelocationID kAdaptTarget{ 513465, 391303 };
		constexpr REL::RelocationID kAdaptReset{ 528108, 415053 };
		constexpr REL::RelocationID kLuminanceStopAt2{ 513488, 391336 };
		constexpr REL::RelocationID kLuminanceStopOverride{ 528106, 415051 };
		constexpr REL::RelocationID kLuminanceParity{ 528109, 415054 };
		// The VL frame state (SE 100297 / AE 107014). Its counter at +0x88 picks the VL target as 43 + (~counter & 1).
		constexpr REL::RelocationID kVolumetricState{ 100297, 107014 };
		constexpr std::uintptr_t kVolumetricCounter = 0x88;
		// When set the depth of field blurs through the compute helper at manager +0x188 (100488).
		constexpr REL::RelocationID kDofComputeBlur{ 513727, 391684 };

		// RenderWorld scope state. The minify loop in RenderSSAO (100520 / 107245) runs levels 1 to count - 1.
		constexpr REL::RelocationID kSaoLevels{ 528105, 415050 };
		constexpr REL::RelocationID kSaoContrast{ 528124, 415069 };
		// When set every SAO target index is one higher.
		constexpr REL::RelocationID kSaoHalf{ 528122, 415067 };
		// Tested by the composite together with GetPortalGraph (98984 / 105637).
		constexpr REL::RelocationID kSaoFog{ 528125, 415070 };
		constexpr REL::RelocationID kGetPortalGraph{ 98984, 105637 };
		constexpr REL::RelocationID kPortalIndex{ 513241, 390982 };
		// ISSAORawAO when set, else ISSAORawAONoTemporal.
		constexpr REL::RelocationID kSaoTemporal{ 513569, 391450 };
		// When set the SSR pass (100428 / 107146) blurs through the compute helper (100487).
		// The SAO composite then reads kSSR_BLURRED0 instead of kSSR_RAW.
		constexpr REL::RelocationID kSsrBlur{ 528114, 415059 };
		// The normal mask is downsampled when this is set and the mode is above 1. Mode 4 is the compute form.
		constexpr REL::RelocationID kSsrDownsample{ 513568, 391448 };
		constexpr REL::RelocationID kSsrDownsampleMode{ 513454, 391287 };
		// gFrameCounter: the normals target (WorldPass::NormalsTargetOf) and the water history follow its parity.
		constexpr REL::RelocationID kFrameCounter{ 525008, 411489 };
		// Main::RenderWorld (100424 / 107142) runs the snow pass when this and the helper are on, else copies kMAIN.
		constexpr REL::RelocationID kSnow{ 513332, 391105 };
		constexpr std::uintptr_t kReflections = 0x1c0;  // ray march at +0x00, active at +0x10
		// The SAO helper. +0x50 is the composites' flag and +0x54 its counter.
		constexpr std::uintptr_t kSao = 0x1c8;
		constexpr std::uintptr_t kSnowHelper = 0x1e8;     // ISSnowSSS at +0x00, active at +0x08

		// ImageSpaceManager members, the same on SE and AE. Raw offsets where CommonLib's names are wrong.
		constexpr std::uintptr_t kBloomRadius = 0xcc;        // float
		constexpr std::uintptr_t kFade = 0x118;              // above 0 picks the tonemap's fade form
		constexpr std::uintptr_t kVolumetricEffect = 0x170;  // active at +0x08
		constexpr std::uintptr_t kLensFlares = 0x180;        // active at +0x30
		constexpr std::uintptr_t kSil = 0x1d8;               // active at +0x10, counter at +0x14
		constexpr std::uintptr_t kTemporal = 0x1f0;          // enabled +0x18, form +0x2c, history +0x34, water +0x38
		constexpr std::uintptr_t kUsesLdr = 0x210;
		constexpr std::uintptr_t kScreenshot = 0x211;

		constexpr std::uint32_t kDownsampleTarget = 26;  // kHDR_DOWNSAMPLE0, whose size sets the luminance chain's level count

		template <class T>
		T Member(std::uintptr_t a_object, std::uintptr_t a_offset)
		{
			return *reinterpret_cast<const T*>(a_object + a_offset);
		}

		template <class T>
		T Global(const REL::RelocationID& a_id)
		{
			return *reinterpret_cast<const T*>(a_id.address());
		}

		struct State
		{
			bool installed = false;
			bool inChain = false;     // from RenderWorldPostProcess's entry to its return
			bool endOfFrame = false;
			// Each slot's fields from four frames before. A change is a new recording for that slot.
			std::array<PostFields, RenderGraphRuntime::kGameFramesInFlight> bySlot{};
			std::array<bool, RenderGraphRuntime::kGameFramesInFlight> haveSlot{};
			std::mutex mutex;
			std::uint64_t frames = 0, changedFrames = 0;
			std::map<std::string, std::uint64_t> changes;  // fields that changed against the same slot, by name
			// UpdateJitter's index into the State tables, and the frame's final jitter (NoteJitter).
			std::uint32_t jitterIndex = 0;
			bool jitterIndexRead = false;
			std::array<float, 2> jitter{};
		};

		State& Get()
		{
			static State state;
			return state;
		}

		RE::ImageSpaceEffect* EffectAt(std::uint32_t a_index)
		{
			auto* manager = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
			if (!manager || a_index >= manager->effects.free_idx() || a_index >= manager->effects.capacity())
				return nullptr;
			return manager->effects[static_cast<std::uint16_t>(a_index)];
		}

		/** @brief The luminance chain's levels from kHDR_DOWNSAMPLE0's size, as ImageSpaceEffectHDR::Render loops over them. */
		std::uint32_t Levels()
		{
			// The manager is a static object at this ID, not the pointer CommonLib's GetSingleton reads.
			static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> manager{ REL::RelocationID(524970, 411451) };
			const auto& size = manager.get()->renderTargetData[kDownsampleTarget];
			const std::int32_t least = Global<bool>(kLuminanceStopAt2) && !Global<bool>(kLuminanceStopOverride) ? 2 : 1;
			std::int32_t width = static_cast<std::int32_t>(size.width), height = static_cast<std::int32_t>(size.height);
			std::uint32_t levels = 0;
			while ((least < width || least < height) && levels < 32) {
				width = std::max((width + 3) / 4, least);
				height = std::max((height + 3) / 4, least);
				++levels;
			}
			return levels;
		}

		bool Active(const RE::ImageSpaceEffect* a_effect)
		{
			return a_effect && const_cast<RE::ImageSpaceEffect*>(a_effect)->IsActive();
		}

		void CountFields(std::map<std::string, std::uint64_t>& a_counts, const std::string& a_diff)
		{
			std::size_t start = 0;
			while (start < a_diff.size()) {
				const auto end = a_diff.find(", ", start);
				const auto item = a_diff.substr(start, end == std::string::npos ? std::string::npos : end - start);
				const auto arrow = item.find(" -> ");
				++a_counts[arrow == std::string::npos || arrow == 0 ? item : item.substr(0, item.rfind(' ', arrow - 1))];
				if (end == std::string::npos)
					break;
				start = end + 2;
			}
		}

		/**
		 * @brief ImageSpaceManager::RenderEndOfFrameEffects (SE 99023, AE 105674). In the world chain its return ends the post stage.
		 * UI3DSceneManager::Render (51855) also calls it for a menu's 3D scene, outside the chain.
		 */
		struct RenderEndOfFrameEffects
		{
			static void thunk(void* a_manager, std::uint64_t a_source, std::uint64_t a_final, std::uint64_t a_arg4, std::uint64_t a_arg5)
			{
				auto& state = Get();
				const bool world = state.inChain && !state.endOfFrame && FrameAnchors::IsRenderOwner();
				if (world) {
					state.endOfFrame = true;
					Engine::ImageSpace::OnEndOfFrame(true, static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_final));
				}
				// A menu scene's call. Its draws are the menu composites' steps.
				const bool menu = !world && !state.endOfFrame && FrameAnchors::IsRenderOwner();
				if (menu)
					Engine::ImageSpace::OnMenuEndOfFrame(true, static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_final));
				if (world)
					Engine::ImageSpace::WorldEndOfFrame(static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_final),
						[&] { func(a_manager, a_source, a_final, a_arg4, a_arg5); });
				else
					func(a_manager, a_source, a_final, a_arg4, a_arg5);
				if (menu)
					Engine::ImageSpace::OnMenuEndOfFrame(false, 0, 0);
				if (world) {
					Engine::ImageSpace::OnEndOfFrame(false, 0, 0);
					state.endOfFrame = false;
					(void)FrameAnchors::TapPoint(SegmentId::Post);
				}
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148), the world post chain. */
		struct RenderWorldPostProcess
		{
			static void thunk(std::uint32_t a_target)
			{
				auto& state = Get();
				const bool owner = FrameAnchors::IsRenderOwner();
				if (owner) {
					state.inChain = true;
					// End the sun stages here at the latest, before the chain's D3D11 work reaches the GPU. Sun water follows sun.
					(void)FrameAnchors::TapPoint(SegmentId::Sun);
					(void)FrameAnchors::TapPoint(SegmentId::SunWater);
				}
				if (owner)
					Engine::ImageSpace::WorldPostProcess(a_target, [&] { func(a_target); });
				else
					func(a_target);
				if (owner)
					state.inChain = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install()
	{
		auto& state = Get();
		if (state.installed || !Engine::GraphMode())
			return;
		stl::detour_thunk<RenderWorldPostProcess>(REL::RelocationID(100430, 107148));
		stl::detour_thunk<RenderEndOfFrameEffects>(REL::RelocationID(99023, 105674));
		state.installed = true;
		logger::info("[ORG] post chain: the world post chain's entries are hooked");
	}

	bool On()
	{
		return Get().installed;
	}

	bool OwnsJitter()
	{
		// SE's State layout was matched statically (perf-post-slots.md); untested at runtime.
		return Engine::GraphMode();
	}

	std::array<float, 2> UpdateJitter(RE::BSGraphics::State* a_state)
	{
		// Native keeps the jitter render-side (Snapshot/FrameCamera.h) and leaves the engine's State unjittered.
		const bool write = !Snapshot::FrameCamera::Owned();
		// BSGraphics::State (AE 411479): the table index at +0xC0, x and y tables of eight at +0xC4 and +0xE4 (SE 8 bytes lower), the
		// screen size at +0x24 / +0x28. The engine's UpdateJitter (AE 77518, SE 75709) reads the same.
		auto& state = Get();
		auto* base = reinterpret_cast<std::byte*>(a_state);
		const auto at = [&](std::uintptr_t a_offset) { return reinterpret_cast<float*>(base + a_offset); };
		const std::uintptr_t table = REL::Relocate(0xB8, 0xC0);
		auto* index = reinterpret_cast<std::uint32_t*>(base + table);
		// CS's own index from the first call on. The engine's starts it, so the sequence continues where the engine left it.
		if (!std::exchange(state.jitterIndexRead, true))
			state.jitterIndex = *index;
		auto* manager = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
		const auto taa = manager ? Member<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(manager), kTemporal) : 0;
		std::array<float, 2> jitter{};
		const bool on = taa && Member<bool>(taa, 0x18);
		if (on) {
			state.jitterIndex = (state.jitterIndex + 1) & 7;
			const auto width = static_cast<float>(*reinterpret_cast<const std::uint32_t*>(base + 0x24));
			const auto height = static_cast<float>(*reinterpret_cast<const std::uint32_t*>(base + 0x28));
			const float x = *at(table + 4 + state.jitterIndex * 4);
			const float y = *at(table + 0x24 + state.jitterIndex * 4);
			jitter = { (x + x - 1.0f) / width, (y + y - 1.0f) / height };
		}
		if (write) {
			if (on)
				*index = state.jitterIndex;
			a_state->projectionPosScaleX = jitter[0];
			a_state->projectionPosScaleY = jitter[1];
		}
		return jitter;
	}

	void NoteJitter(float a_x, float a_y)
	{
		Get().jitter = { a_x, a_y };
	}

	std::array<float, 2> Jitter()
	{
		return Get().jitter;
	}

	PostFields Read()
	{
		PostFields fields;
		auto* manager = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
		if (!manager)
			return fields;
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		fields.read = true;
		const auto volumetric = Member<std::uintptr_t>(base, kVolumetricEffect);
		fields.volumetric = Engine::Native::Active() ? ORGVolumetricLighting::NativeActive() :
		                                               Global<bool>(kVolumetricEnabled) && volumetric && Member<bool>(volumetric, 0x08);
		if (const auto sil = Member<std::uintptr_t>(base, kSil))
			fields.sil = Member<bool>(sil, 0x10);
		if (const auto lens = Member<std::uintptr_t>(base, kLensFlares))
			fields.lensFlares = Member<bool>(lens, 0x30);
		fields.eof = Global<bool>(kEnableEndOfFrame);
		fields.ldr = Member<bool>(base, kUsesLdr);
		if (fields.eof)
			for (std::uint32_t slot = 0; slot < 8; ++slot)
				if (Active(EffectAt(slot)))
					fields.slots |= static_cast<std::uint8_t>(1u << slot);
		fields.dofCompute = Global<bool>(kDofComputeBlur);
		fields.hdr = Global<bool>(kHdrEnabled);
		fields.effects11 = globals::state && globals::state->PeekTonemapOwner() == ::State::TonemapOwner::kEffects11;
		fields.bloom = static_cast<std::uint8_t>(std::clamp(static_cast<std::int32_t>(Member<float>(base, kBloomRadius)) - 1, 0, 6));
		fields.fade = Member<float>(base, kFade) > 0.0f;
		fields.adapt = Global<std::int32_t>(kAdaptTarget) >= 0 && !Global<bool>(kAdaptReset);
		fields.lumLevels = static_cast<std::uint8_t>(Levels());
		if (const auto taa = Member<std::uintptr_t>(base, kTemporal)) {
			fields.taa = Member<bool>(taa, 0x18);
			fields.taaMode = static_cast<std::uint8_t>(Engine::Native::Active() ? Engine::Native::WorldPass::TaaForm() : Member<std::int32_t>(taa, 0x2c));
			fields.taaWater = Member<bool>(taa, 0x38);
		}
		// The dynamic-resolution upsample (100548) is not read: a ratio is no build input (decision 6a). Its group is always on, gated.
		fields.screenshot = Member<bool>(base, kScreenshot);
		// Native mode draws the effects from the snapshot's ImageSpace inputs, so the description takes them too. Refraction's slot stays
		// a read (its geometry turns it on), and the adaptation resets by the snapshot's count, not the engine's flag.
		if (const auto* in = Engine::ImageSpace::SnapshotInputs(); in && in->read) {
			fields.sil = in->sil;
			fields.lensFlares = in->iblf;
			fields.eof = in->eof;
			fields.ldr = in->usesLdr;
			fields.slots = in->eof ? static_cast<std::uint8_t>(in->gameSlots | (Active(EffectAt(1)) ? 2u : 0u)) : std::uint8_t(0);
			fields.dofCompute = in->dofCompute;
			fields.hdr = in->hdr;
			fields.bloom = static_cast<std::uint8_t>(std::clamp(static_cast<std::int32_t>(in->At(kBloomRadius)) - 1, 0, 6));
			fields.fade = in->At(kFade) > 0.0f;
			fields.adapt = Global<std::int32_t>(kAdaptTarget) >= 0;
			fields.screenshot = in->screenshot;
		}
		// Upscaling turns the engine's TAA on just before the end-of-frame effects and off after, so the flag at Begin says nothing.
		auto& upscaling = globals::features::upscaling;
		// The adopted revision's method, so the evaluate switches in with its build and textures.
		const auto method = upscaling.FrameUpscaleMethod();
		if (upscaling.loaded)
			fields.taa = !Upscaling::IsWindowUnusable() && method == Upscaling::UpscaleMethod::kTAA;
		fields.upscaler = upscaling.loaded && !Upscaling::IsWindowUnusable() &&
		                  (method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR || method == Upscaling::UpscaleMethod::kXeSS);
		return fields;
	}

	bool InChain()
	{
		return Get().inChain;
	}

	std::uint32_t LuminanceLevels()
	{
		return Levels();
	}

	std::uint32_t LuminanceParity()
	{
		return static_cast<std::uint32_t>(Global<std::int32_t>(kLuminanceParity)) & 1u;
	}

	std::uint32_t VolumetricParity()
	{
		// Native: CS's VL counter (the engine's VL render does not run).
		if (Engine::Native::Active())
			return ORGVolumetricLighting::NativeParity();
		static REL::Relocation<std::uintptr_t (*)()> volumetricState{ kVolumetricState };
		const auto vl = volumetricState();
		return vl ? (~Member<std::uint32_t>(vl, kVolumetricCounter) & 1u) : 0u;
	}

	void OnBegin(Description& a_frame)
	{
		auto& state = Get();
		if (!On() || (a_frame.kind != Kind::World && !(a_frame.kind == Kind::FrozenWorld && a_frame.capture)))
			return;
		a_frame.post = Read();
		const std::lock_guard lock(state.mutex);
		++state.frames;
		// A slot's copy is recorded again when its fields change from the same slot's last ones.
		auto& kept = state.bySlot[a_frame.slot];
		if (state.haveSlot[a_frame.slot]) {
			if (const auto diff = DiffPost(kept, a_frame.post); !diff.empty()) {
				++state.changedFrames;
				CountFields(state.changes, diff);
			}
		}
		kept = a_frame.post;
		state.haveSlot[a_frame.slot] = true;
	}

	namespace
	{
		/** @brief The first SAO level that draws with the contrast form: the first whose height is at most 16 (heights halve from kMAIN's). */
		std::uint8_t SaoContrastLevel(std::uint32_t a_levels)
		{
			// A static object, not the pointer CommonLib's GetSingleton reads.
			static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> targets{ REL::RelocationID(524970, 411451) };
			const auto height = targets.get()->renderTargetData[RE::RENDER_TARGET::kMAIN].height;
			for (std::uint32_t level = 1; level < a_levels; ++level)
				if ((height >> (level - 1)) <= 16)
					return static_cast<std::uint8_t>(level);
			return 0;
		}

		/** @brief The SAO parity: RenderSSAO increments its counter (helper +0x54) and wraps it at 2 before drawing. Render state. */
		std::uint8_t SaoParity()
		{
			const auto* manager = RE::ImageSpaceManager::GetSingleton();
			const auto sao = manager ? Member<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(manager), kSao) : 0;
			if (!sao)
				return 0;
			const auto counter = Member<std::int32_t>(sao, 0x54) + 1;
			return static_cast<std::uint8_t>(counter > 1 ? 0 : counter);
		}

		/** @brief ReadWorld from the snapshot's world settings and frame counter (native). */
		WorldFields ReadWorldFrom(Scope a_scope, WorldFields a_fields, const Engine::ImageSpace::WorldSettings& a_world, std::uint32_t a_frameCounter)
		{
			a_fields.read = true;
			a_fields.normals = static_cast<std::uint8_t>(Engine::Native::WorldPass::NormalsTargetOf(a_world.ssrOn, a_frameCounter));
			a_fields.ssrBlur = a_world.ssrBlur;
			a_fields.saoHalf = a_world.saoHalf;
			switch (a_scope) {
			case Scope::Reflections:
				a_fields.ssr = a_world.ssrOn;
				a_fields.ssrDownsample = 0;
				if (a_world.ssrDownsample && a_world.ssrDivider > 1)
					a_fields.ssrDownsample = a_world.ssrDivider == 4 ? 2 : 1;
				break;
			case Scope::Ssao:
				a_fields.saoLevels = static_cast<std::uint8_t>(a_world.saoLevels);
				a_fields.saoTemporal = a_world.saoTemporal;
				a_fields.saoParity = SaoParity();
				a_fields.saoContrast = a_world.saoContrastOn ? SaoContrastLevel(a_world.saoLevels) : std::uint8_t(0);
				break;
			case Scope::Snow:
				a_fields.snow = a_world.improvedSnow && a_world.snowOn;
				break;
			case Scope::SsaoComposite:
				{
					const bool fog = a_world.saoFog && a_world.portalFog;
					a_fields.saoForm = static_cast<std::uint8_t>(a_world.saoOn ? (fog ? 1 : 0) : (fog ? 2 : 3));
					break;
				}
			case Scope::WaterBlend:
				a_fields.waterHistory = static_cast<std::uint8_t>(a_frameCounter & 1u);
				break;
			default:
				break;
			}
			return a_fields;
		}
	}

	WorldFields ReadWorld(Scope a_scope, const WorldFields& a_base)
	{
		WorldFields fields = a_base;
		// Native: the snapshot's values, which the native scopes draw from. Only the SAO counter and kMAIN's height, render state, are
		// read from the engine there.
		if (const auto* world = Engine::ImageSpace::SnapshotWorld()) {
			if (const auto* inputs = Engine::ImageSpace::SnapshotInputs(); inputs && inputs->read)
				return ReadWorldFrom(a_scope, fields, *world, inputs->frameCounter);
		}
		auto* manager = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
		if (!manager)
			return fields;
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		fields.read = true;
		const auto reflections = Member<std::uintptr_t>(base, kReflections);
		const bool ssr = reflections && Member<bool>(reflections, 0x10);
		fields.normals = static_cast<std::uint8_t>(Engine::Native::WorldPass::NormalsTargetOf(ssr, Global<std::uint32_t>(kFrameCounter)));
		fields.ssrBlur = Global<bool>(kSsrBlur);
		fields.saoHalf = Global<bool>(kSaoHalf);
		switch (a_scope) {
		case Scope::Reflections:
			fields.ssr = ssr;
			fields.ssrDownsample = 0;
			if (Global<bool>(kSsrDownsample) && Global<std::int32_t>(kSsrDownsampleMode) > 1)
				fields.ssrDownsample = Global<std::int32_t>(kSsrDownsampleMode) == 4 ? 2 : 1;
			break;
		case Scope::Ssao:
			{
				fields.saoLevels = static_cast<std::uint8_t>(Global<std::uint32_t>(kSaoLevels));
				fields.saoTemporal = Global<bool>(kSaoTemporal);
				// The raw AO target is 62 + 2 * the parity.
				fields.saoParity = SaoParity();
				fields.saoContrast = Global<bool>(kSaoContrast) ? SaoContrastLevel(fields.saoLevels) : std::uint8_t(0);
				break;
			}
		case Scope::Snow:
			{
				const auto snow = Member<std::uintptr_t>(base, kSnowHelper);
				fields.snow = Global<bool>(kSnow) && snow && Member<bool>(snow, 0x08);
				break;
			}
		case Scope::SsaoComposite:
			{
				const auto sao = Member<std::uintptr_t>(base, kSao);
				const bool form = sao && Member<bool>(sao, 0x50);
				static REL::Relocation<std::uintptr_t (*)(std::int32_t)> portalGraph{ kGetPortalGraph };
				const bool fog = Global<bool>(kSaoFog) && portalGraph(Global<std::int32_t>(kPortalIndex)) != 0;
				fields.saoForm = static_cast<std::uint8_t>(form ? (fog ? 1 : 0) : (fog ? 2 : 3));
				break;
			}
		case Scope::WaterBlend:
			{
				fields.waterHistory = static_cast<std::uint8_t>(Global<std::uint32_t>(kFrameCounter) & 1u);
				break;
			}
		default:
			break;
		}
		return fields;
	}
}
