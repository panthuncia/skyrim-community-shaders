#include "RenderGraph/Frame/PostChain.h"

#include "Deferred.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"
#include "State.h"

#include <algorithm>
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
		// GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP (100460) returns (~counter & 1) | 4 while SSR is active, else 4.
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
		};

		State& Get()
		{
			static State state;
			return state;
		}

		RE::ImageSpaceEffect* EffectAt(std::uint32_t a_index)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
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

	PostFields Read()
	{
		PostFields fields;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (!manager)
			return fields;
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		fields.read = true;
		const auto volumetric = Member<std::uintptr_t>(base, kVolumetricEffect);
		fields.volumetric = Global<bool>(kVolumetricEnabled) && volumetric && Member<bool>(volumetric, 0x08);
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
			fields.taaMode = static_cast<std::uint8_t>(Member<std::int32_t>(taa, 0x2c));
			fields.taaWater = Member<bool>(taa, 0x38);
		}
		// The dynamic-resolution upsample (100548) is not read: a ratio is no build input (decision 6a). Its group is always on, gated.
		fields.screenshot = Member<bool>(base, kScreenshot);
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


	WorldFields ReadWorld(Scope a_scope, const WorldFields& a_base)
	{
		WorldFields fields = a_base;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (!manager)
			return fields;
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		fields.read = true;
		const auto reflections = Member<std::uintptr_t>(base, kReflections);
		const bool ssr = reflections && Member<bool>(reflections, 0x10);
		// Follows the frame counter (100460).
		fields.normals = static_cast<std::uint8_t>(ssr ? ((~Global<std::uint32_t>(kFrameCounter) & 1u) | 4u) : 4u);
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
				// RenderSSAO increments its counter and wraps it at 2 before drawing. The raw AO target is 62 + 2 * counter.
				if (const auto sao = Member<std::uintptr_t>(base, kSao)) {
					const auto counter = Member<std::int32_t>(sao, 0x54) + 1;
					fields.saoParity = static_cast<std::uint8_t>(counter > 1 ? 0 : counter);
				}
				// The first level whose height is at most 16 draws with the contrast form. Heights halve from kMAIN's.
				fields.saoContrast = 0;
				if (Global<bool>(kSaoContrast)) {
					// A static object, not the pointer CommonLib's GetSingleton reads.
					static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> targets{ REL::RelocationID(524970, 411451) };
					const auto height = targets.get()->renderTargetData[RE::RENDER_TARGET::kMAIN].height;
					for (std::uint32_t level = 1; level < fields.saoLevels; ++level)
						if ((height >> (level - 1)) <= 16) {
							fields.saoContrast = static_cast<std::uint8_t>(level);
							break;
						}
				}
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
