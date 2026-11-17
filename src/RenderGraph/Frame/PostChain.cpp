#include "RenderGraph/Frame/PostChain.h"

#include "Deferred.h"
#include "Features/Upscaling.h"
#include "Globals.h"
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
		// The engine's globals the chain reads (Ghidra, SE 1.5.97 and AE 1.6.1170; docs, "The post stage"). Each is the
		// value the engine's own test reads.
		// gEnableEOFImageSpace: RenderEndOfFrameEffects (99023 / 105674) counts the active end-of-frame slots only when it is set.
		constexpr REL::RelocationID kEnableEndOfFrame{ 513299, 391055 };
		// The HDR switch: RenderEndOfFrameEffects renders the HDR effect (effects[30]) when it is set, else a copy (effects[34]).
		constexpr REL::RelocationID kHdrEnabled{ 513463, 391301 };
		// The volumetric lighting switch: RenderWorldPostProcess (100430 / 107148) and the VL composite (100493) test it with the VL
		// effect's active flag.
		constexpr REL::RelocationID kVolumetricEnabled{ 527940, 414913 };
		// Dynamic resolution: its lock and the two scale factors. The upsample (100548) and RenderEndOfFrameEffects run it when
		// unlocked and a scale is not 1.
		constexpr REL::RelocationID kDynamicLock{ 525027, 411509 };
		constexpr REL::RelocationID kDynamicScaleX{ 525023, 411505 };
		constexpr REL::RelocationID kDynamicScaleY{ 525024, 411506 };
		// ImageSpaceEffectHDR::Render (100465 / 107183): last frame's adapted luminance (its target index, below 0 when there is none),
		// the flag that drops it at the next render, the option and override that stop the luminance chain at 2x2, and the chain's
		// last-level parity, which each render flips.
		constexpr REL::RelocationID kAdaptTarget{ 513465, 391303 };
		constexpr REL::RelocationID kAdaptReset{ 528108, 415053 };
		constexpr REL::RelocationID kLuminanceStopAt2{ 513488, 391336 };
		constexpr REL::RelocationID kLuminanceStopOverride{ 528106, 415051 };
		constexpr REL::RelocationID kLuminanceParity{ 528109, 415054 };
		// The volumetric lighting's frame state (a static object, made on first use by its getter, SE 100297 / AE 107014): its counter at
		// +0x88 picks the target the VL writes and the composite reads, 43 + (~counter & 1) (RenderWorldPostProcess and 100493 on SE;
		// 107148 on AE).
		constexpr REL::RelocationID kVolumetricState{ 100297, 107014 };
		constexpr std::uintptr_t kVolumetricCounter = 0x88;
		// The depth of field's blur takes the compute form (the CS blur helper at manager +0x188, 100488) when this option is set.
		constexpr REL::RelocationID kDofComputeBlur{ 513727, 391684 };

		// The RenderWorld scopes' state (Ghidra, SE 1.5.97 and AE 1.6.1170; docs, "The declared frame, stage 5"). Each is what the engine's
		// own test reads.
		// The camera-Z chain's level count (the minify loop in RenderSSAO, 100520 / 107245, runs levels 1 to count - 1).
		constexpr REL::RelocationID kSaoLevels{ 528105, 415050 };
		// The option that draws the first level at most 16 pixels high with ISMinifyContrast.
		constexpr REL::RelocationID kSaoContrast{ 528124, 415069 };
		// The SAO targets' half-resolution set: every SAO target index + 1.
		constexpr REL::RelocationID kSaoHalf{ 528122, 415067 };
		// The fog flag the composite tests with the portal graph (GetPortalGraph, 98984 / 105637, of the shader state index below).
		constexpr REL::RelocationID kSaoFog{ 528125, 415070 };
		constexpr REL::RelocationID kGetPortalGraph{ 98984, 105637 };
		constexpr REL::RelocationID kPortalIndex{ 513241, 390982 };
		// The temporal raw AO setting (its value byte): ISSAORawAO, else ISSAORawAONoTemporal.
		constexpr REL::RelocationID kSaoTemporal{ 513569, 391450 };
		// The reflections' blur: the reflections pass (100428 / 107146) blurs its ray march through the compute blur helper (manager +0x188,
		// 100487) when it is set, and the SAO composite then reads kSSR_BLURRED0 instead of kSSR_RAW.
		constexpr REL::RelocationID kSsrBlur{ 528114, 415059 };
		// The normal mask's downsample before the ray march: on with this setting when the mode is above 1; mode 4 is the compute form.
		constexpr REL::RelocationID kSsrDownsample{ 513568, 391448 };
		constexpr REL::RelocationID kSsrDownsampleMode{ 513454, 391287 };
		// gFrameCounter: GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP (100460) returns (~counter & 1) | 4 while the reflections helper
		// is active, else 4 (kNORMAL_TAAMASK_SSRMASK).
		constexpr REL::RelocationID kFrameCounter{ 525008, 411489 };
		// The snow setting Main::RenderWorld (100424 / 107142) tests with the snow helper's active flag: the snow pass, else the copy of
		// kMAIN into its copy.
		constexpr REL::RelocationID kSnow{ 513332, 391105 };
		constexpr std::uintptr_t kReflections = 0x1c0;  // the reflections helper: the ray march at +0x00, active at +0x10
		// The SAO helper: blur H, blur V, camera Z, the composites (SAO, fog, SAO and fog), minify, minify contrast, raw AO, raw AO without
		// temporal; +0x50 the SAO composites' flag, +0x54 its counter.
		constexpr std::uintptr_t kSao = 0x1c8;
		constexpr std::uintptr_t kSnowHelper = 0x1e8;     // the snow pass: ISSnowSSS at +0x00, active at +0x08

		// ImageSpaceManager members, the same on SE and AE (RUNTIME_DATA; raw offsets where CommonLib's names are wrong).
		constexpr std::uintptr_t kBloomRadius = 0xcc;        // ImageSpaceData: the bloom blur radius (float)
		constexpr std::uintptr_t kFade = 0x118;              // the cinematic fade amount: above 0 picks the tonemap's fade form
		constexpr std::uintptr_t kVolumetricEffect = 0x170;  // the VL effect (active at +0x08)
		constexpr std::uintptr_t kLensFlares = 0x180;        // the image-based lens flares (active at +0x30)
		constexpr std::uintptr_t kSil = 0x1d8;               // the SIL composite: composite, raw indirect, active at +0x10, counter at +0x14
		constexpr std::uintptr_t kTemporal = 0x1f0;          // TAA: shader, UI form, water form; enabled +0x18, form +0x2c, history +0x34, water +0x38
		constexpr std::uintptr_t kUpsample = 0x1f8;          // the dynamic-resolution upsample: its shader, active at +0x08
		constexpr std::uintptr_t kUsesLdr = 0x210;
		constexpr std::uintptr_t kScreenshot = 0x211;

		constexpr std::uint32_t kDownsampleTarget = 26;  // kHDR_DOWNSAMPLE0: the luminance chain's level count follows its size

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
			bool inChain = false;     // the world chain now: from RenderWorldPostProcess's entry to its return
			bool endOfFrame = false;  // RenderEndOfFrameEffects of the world chain is running
			// Each slot's description from four frames before: a change against it is a new recording for that slot's copy.
			std::array<PostFields, RenderGraphRuntime::kGameFramesInFlight> bySlot{};
			std::array<bool, RenderGraphRuntime::kGameFramesInFlight> haveSlot{};
			// The census since the last report.
			std::mutex mutex;
			std::uint64_t frames = 0, changedFrames = 0;
			std::map<std::string, std::uint64_t> changes;  // fields that changed against the same slot four frames before, by name
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
			// The manager is a static object at this ID (CommonLib's GetSingleton reads it as a pointer, which it is not).
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

		/** @brief Counts each field a DiffPost names ("bloom 2 -> 3" counts as "bloom"). */
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
		 * @brief ImageSpaceManager::RenderEndOfFrameEffects (SE 99023, AE 105674). Inside the world chain it runs within Upscaling's
		 * wrapper (Upscaling::Main_PostProcessing), after the upscaler set TAA and dynamic resolution for the frame: ImageSpace plans the
		 * chain's targets at its entry, and its return ends the post stage (a point step). UI3DSceneManager::Render (51855)
		 * calls it for a menu's 3D scene too, outside the chain: its draws are the menu composites' steps there (ImageSpace's MenuSteps.inl).
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
				// A menu scene's call: its draws are the menu composites' steps.
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

		/** @brief ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148): the world post chain. */
		struct RenderWorldPostProcess
		{
			static void thunk(std::uint32_t a_target)
			{
				auto& state = Get();
				const bool owner = FrameAnchors::IsRenderOwner();
				if (owner)
					state.inChain = true;
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
		if (state.installed || REL::Module::IsVR() || !Engine::ImageSpace::Requested())
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
		fields.dynres = Global<std::int32_t>(kDynamicLock) == 0 && (Global<float>(kDynamicScaleX) != 1.0f || Global<float>(kDynamicScaleY) != 1.0f);
		if (const auto upsample = Member<std::uintptr_t>(base, kUpsample); !upsample || !Member<bool>(upsample, 0x08))
			fields.dynres = false;
		fields.screenshot = Member<bool>(base, kScreenshot);
		// TAA is CS's decision: Upscaling turns the engine's TAA on for its TAA method just before the end-of-frame effects
		// (Util::SetTemporal in Upscaling::Main_PostProcessing) and off after them, so the engine flag at Begin says nothing.
		auto& upscaling = globals::features::upscaling;
		const auto method = upscaling.GetUpscaleMethod();
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
		// A slot's copy is recorded again when its description changes: compare with the same slot's last one.
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

	const char* ScopeName(Scope a_scope)
	{
		switch (a_scope) {
		case Scope::Reflections:
			return "reflections";
		case Scope::Ssao:
			return "SAO";
		case Scope::Snow:
			return "snow";
		case Scope::SsaoComposite:
			return "SAO composite";
		case Scope::WaterBlend:
			return "water blend";
		default:
			return "none";
		}
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
		// The SSR mask target every scope reads (GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP, 100460): it follows the frame counter.
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
				// RenderSSAO adds one to its counter and wraps it at 2 before it draws: the raw AO's target is 62 + 2 * counter.
				if (const auto sao = Member<std::uintptr_t>(base, kSao)) {
					const auto counter = Member<std::int32_t>(sao, 0x54) + 1;
					fields.saoParity = static_cast<std::uint8_t>(counter > 1 ? 0 : counter);
				}
				// The minify loop draws the first level whose height (kMAIN's, halved per level) is at most 16 with the contrast form.
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

	void LogStats()
	{
		auto& state = Get();
		if (!On())
			return;
		const std::lock_guard lock(state.mutex);
		std::string list;
		for (const auto& [name, count] : state.changes)
			list += fmt::format("{}{} {}", list.empty() ? "" : ", ", name, count);
		logger::info("[ORG] post description: {} world frames, {} changed against their slot's last description ({})", state.frames, state.changedFrames,
			list.empty() ? std::string("none") : list);
		state.frames = state.changedFrames = 0;
		state.changes.clear();
	}
}
