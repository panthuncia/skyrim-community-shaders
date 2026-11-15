#include "RenderGraph/Frame/PostChain.h"

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
		// The engine's globals the chain reads (Ghidra, SE 1.5.97 and AE 1.6.1170; docs, "The declared frame, stage 4"). Each is the
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

		// ImageSpaceManager members, the same on SE and AE (RUNTIME_DATA; raw offsets where CommonLib's names are wrong).
		constexpr std::uintptr_t kBloomRadius = 0xcc;        // ImageSpaceData: the bloom blur radius (float)
		constexpr std::uintptr_t kFade = 0x118;              // the cinematic fade amount: above 0 picks the tonemap's fade form
		constexpr std::uintptr_t kVolumetricEffect = 0x170;  // the VL effect (active at +0x08)
		constexpr std::uintptr_t kLensFlares = 0x180;        // the image-based lens flares (active at +0x30)
		constexpr std::uintptr_t kComposites = 0x190;        // the VL / lens flare composites: VL only, lens flare only, both
		constexpr std::uintptr_t kSil = 0x1d8;               // the SIL composite: composite, raw indirect, active at +0x10, counter at +0x14
		constexpr std::uintptr_t kTemporal = 0x1f0;          // TAA: shader, UI form, water form; enabled +0x18, form +0x2c, history +0x34, water +0x38
		constexpr std::uintptr_t kUpsample = 0x1f8;          // the dynamic-resolution upsample: its shader, active at +0x08
		constexpr std::uintptr_t kUsesLdr = 0x210;
		constexpr std::uintptr_t kScreenshot = 0x211;

		// ImageSpaceManager::effects indices the chain names directly.
		constexpr std::uint32_t kHdrEffect = 30;
		constexpr std::uint32_t kCopyEffect = 34;
		constexpr std::uint32_t kBloomFirst = 23;  // BlurBrightPass3: the bloom composite is effects[23 + radius index]
		// The HDR effect's children (its effects list), as its Render picks them.
		constexpr std::uint32_t kHdrTonemap = 2, kHdrTonemapFade = 3, kHdrDownsample = 4, kHdrLuminanceFirst = 5, kHdrLuminance = 6, kHdrAdapt = 8;
		constexpr std::uint32_t kLuminanceFirstTarget = 27;  // kHDR_DOWNSAMPLE1
		constexpr std::uint32_t kDownsampleTarget = 26;      // kHDR_DOWNSAMPLE0

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
			// The world chain now: from RenderWorldPostProcess's entry to its return.
			bool inChain = false;
			bool endOfFrame = false;  // RenderEndOfFrameEffects of the world chain is running
			PostFields chainFields{};  // the fields read at RenderWorldPostProcess's entry
			std::vector<std::uint16_t> predicted;
			std::vector<std::uint16_t> observed;
			// ImageSpaceManager::effects by object, both the ImageSpaceEffect and the BSImagespaceShader it sits in (+0x90 before it).
			std::unordered_map<const void*, std::uint16_t> indexOf;
			std::uintptr_t mapData = 0;
			std::uint32_t mapCount = 0;
			// Each slot's description from four frames before: a change against it is a new recording for that slot's copy.
			std::array<PostFields, RenderGraphRuntime::kGameFramesInFlight> bySlot{};
			std::array<bool, RenderGraphRuntime::kGameFramesInFlight> haveSlot{};
			PostFields lastBegin{};
			// The census since the last report.
			std::mutex mutex;
			std::uint64_t frames = 0, chains = 0, matched = 0, changedFrames = 0, decisionFrames = 0;
			std::map<std::string, std::uint64_t> changes;    // fields that changed against the same slot four frames before, by name
			std::map<std::string, std::uint64_t> decisions;  // fields that differed at the chain's entry from Begin's read
			std::map<std::string, std::uint64_t> eofDecisions;  // fields that differed at RenderEndOfFrameEffects' entry from the chain entry's
			std::uint64_t eofDecisionFrames = 0;
			std::uint32_t decisionLines = 0;
			std::uint32_t mismatchLines = 0;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		/** @brief The effects' index map, rebuilt when the manager's array moved or grew. */
		void RefreshIndex(State& a_state)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager)
				return;
			const std::uint32_t count = manager->effects.free_idx();
			if (count == 0)
				return;
			const auto data = reinterpret_cast<std::uintptr_t>(&manager->effects[0]);
			if (data == a_state.mapData && count == a_state.mapCount)
				return;
			a_state.indexOf.clear();
			for (std::uint32_t i = 0; i < count && i < manager->effects.capacity(); ++i) {
				const auto* effect = manager->effects[static_cast<std::uint16_t>(i)];
				if (!effect)
					continue;
				a_state.indexOf.emplace(effect, static_cast<std::uint16_t>(i));
				// A BSImagespaceShader's ImageSpaceEffect base sits at +0x90 (Render's this - 0x90, SE and AE).
				a_state.indexOf.emplace(reinterpret_cast<const std::byte*>(effect) - 0x90, static_cast<std::uint16_t>(i));
			}
			a_state.mapData = data;
			a_state.mapCount = count;
		}

		/** @brief An effect object's index, or kGroupBase - 1 when it is not in the manager's array. */
		std::uint16_t IndexOf(State& a_state, const void* a_object)
		{
			const auto found = a_state.indexOf.find(a_object);
			return found != a_state.indexOf.end() ? found->second : static_cast<std::uint16_t>(kGroupBase - 1);
		}

		RE::ImageSpaceEffect* EffectAt(std::uint32_t a_index)
		{
			auto* manager = RE::ImageSpaceManager::GetSingleton();
			if (!manager || a_index >= manager->effects.free_idx() || a_index >= manager->effects.capacity())
				return nullptr;
			return manager->effects[static_cast<std::uint16_t>(a_index)];
		}

		RE::ImageSpaceEffect* ChildAt(const RE::ImageSpaceEffect* a_effect, std::uint32_t a_child)
		{
			if (!a_effect || a_child >= a_effect->effects.free_idx() || a_child >= a_effect->effects.capacity())
				return nullptr;
			return a_effect->effects[static_cast<std::uint16_t>(a_child)];
		}

		/** @brief The luminance chain's levels from kHDR_DOWNSAMPLE0's size, as ImageSpaceEffectHDR::Render loops over them. */
		std::uint32_t LuminanceLevels()
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

		/** @brief The group an end-of-frame slot's effect draws as. */
		std::uint16_t SlotGroup(std::uint32_t a_slot)
		{
			switch (a_slot) {
			case 0:
				return kGroupWorldMap;
			case 1:
				return kGroupRefraction;
			case 2:
				return 2;  // ISFXAA: the slot's effect is the shader itself
			case 3:
				return kGroupDepthOfField;
			case 5:
				return kGroupRadialBlur;
			case 6:
				return kGroupFullScreenBlur;
			default:
				return kGroupGetHit;
			}
		}

		/** @brief Whether an effect index can be one of a group's draws. */
		bool InGroup(std::uint16_t a_group, std::uint16_t a_effect)
		{
			const auto in = [&](std::initializer_list<std::pair<std::uint16_t, std::uint16_t>> a_ranges) {
				return std::ranges::any_of(a_ranges, [&](const auto& a_range) { return a_effect >= a_range.first && a_effect <= a_range.second; });
			};
			switch (a_group) {
			case kGroupVolumetricLighting:
				return in({ { 97, 97 }, { 103, 103 }, { 106, 107 } });
			case kGroupLensFlares:
				return in({ { 105, 105 }, { 119, 124 }, { 34, 38 } });
			case kGroupDepthOfField:
				return in({ { 9, 22 }, { 45, 51 }, { 57, 58 }, { 65, 78 }, { 112, 113 }, { 34, 38 } });
			case kGroupFullScreenBlur:
			case kGroupGetHit:
				return in({ { 9, 22 }, { 34, 41 }, { 65, 78 } });
			case kGroupWorldMap:
				return in({ { 43, 44 }, { 34, 38 } });
			case kGroupRadialBlur:
				return in({ { 52, 54 } });
			case kGroupRefraction:
				return in({ { 39, 39 }, { 34, 38 } });
			default:
				return false;
			}
		}

		/** @brief The composite's draws: a shader is one draw, a composite with the generic Render draws its children in order. */
		void Expand(State& a_state, const RE::ImageSpaceEffect* a_effect, std::vector<std::uint16_t>& a_out, std::uint32_t a_depth = 0)
		{
			if (!a_effect)
				return;
			if (a_effect->effects.free_idx() == 0 || a_depth > 4) {
				a_out.push_back(IndexOf(a_state, a_effect));
				return;
			}
			for (std::uint32_t i = 0; i < a_effect->effects.free_idx(); ++i)
				Expand(a_state, ChildAt(a_effect, i), a_out, a_depth + 1);
		}

		/** @brief Matches the observed draws against the prediction (a group takes one or more draws of its set). */
		bool Matches(const std::vector<std::uint16_t>& a_predicted, const std::vector<std::uint16_t>& a_observed)
		{
			std::size_t at = 0;
			for (std::size_t p = 0; p < a_predicted.size(); ++p) {
				const auto want = a_predicted[p];
				if (want < kGroupBase) {
					if (at >= a_observed.size() || a_observed[at] != want)
						return false;
					++at;
					continue;
				}
				// A group: at least one draw of its set (the volumetric lighting's may have none), then as many as follow until the next
				// predicted draw.
				if (at >= a_observed.size() || !InGroup(want, a_observed[at])) {
					if (want == kGroupVolumetricLighting)
						continue;
					return false;
				}
				++at;
				const bool nextConcrete = p + 1 < a_predicted.size() && a_predicted[p + 1] < kGroupBase;
				while (at < a_observed.size() && InGroup(want, a_observed[at]) && !(nextConcrete && a_observed[at] == a_predicted[p + 1]))
					++at;
			}
			return at == a_observed.size();
		}

		std::string Names(const std::vector<std::uint16_t>& a_effects)
		{
			std::string out;
			for (const auto effect : a_effects)
				out += fmt::format("{}{}", out.empty() ? "" : " ", EffectName(effect));
			return out.empty() ? std::string("nothing") : out;
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

		void ChainStart();
		void ChainEnd();
		void EndOfFrameStart();

		/**
		 * @brief ImageSpaceManager::RenderEndOfFrameEffects (SE 99023, AE 105674): the end-of-frame part's decision event. Inside the
		 * world chain it runs within Upscaling's wrapper (Upscaling::Main_PostProcessing), after the upscaler set TAA and dynamic
		 * resolution for the frame. UI3DSceneManager::Render (51855) calls it for a menu's 3D scene too, outside the chain.
		 */
		struct RenderEndOfFrameEffects
		{
			static void thunk(void* a_manager, std::uint64_t a_source, std::uint64_t a_destination, std::uint64_t a_arg4, std::uint64_t a_arg5)
			{
				EndOfFrameStart();
				func(a_manager, a_source, a_destination, a_arg4, a_arg5);
				Get().endOfFrame = false;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148): the world post chain. Its entry is the post
		 * description's decision event: the game's update and the menus have set the frame's ImageSpace data by then.
		 */
		struct RenderWorldPostProcess
		{
			static void thunk(std::uint32_t a_target)
			{
				ChainStart();
				func(a_target);
				ChainEnd();
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void ChainStart()
		{
			auto& state = Get();
			if (!On() || !FrameAnchors::IsRenderOwner())
				return;
			const auto& frame = Driver::Current();
			const auto fields = Read();
			if (const auto diff = DiffPost(frame.post, fields, true); !diff.empty()) {
				const std::lock_guard lock(state.mutex);
				++state.decisionFrames;
				CountFields(state.decisions, diff);
				if (++state.decisionLines <= 8)
					logger::info("[ORG] post description: frame {} decided at the chain's entry, after Begin: {}", frame.frame, diff);
			}
			RefreshIndex(state);
			state.chainFields = fields;
			state.predicted = PredictEffects(fields, Part::Composites);
			state.observed.clear();
			state.inChain = true;
		}

		void EndOfFrameStart()
		{
			auto& state = Get();
			if (!state.inChain || state.endOfFrame)
				return;
			state.endOfFrame = true;
			const auto fields = Read();
			if (const auto diff = DiffPost(state.chainFields, fields, true); !diff.empty()) {
				const std::lock_guard lock(state.mutex);
				++state.eofDecisionFrames;
				CountFields(state.eofDecisions, diff);
				if (++state.decisionLines <= 8)
					logger::info("[ORG] post description: frame {} decided at the end-of-frame effects, after the chain's entry: {}", Driver::Current().frame, diff);
			}
			const auto rest = PredictEffects(fields, Part::EndOfFrame);
			state.predicted.insert(state.predicted.end(), rest.begin(), rest.end());
		}

		void ChainEnd()
		{
			auto& state = Get();
			if (!state.inChain)
				return;
			state.inChain = false;
			const bool match = Matches(state.predicted, state.observed);
			{
				const std::lock_guard lock(state.mutex);
				++state.chains;
				state.matched += match;
			}
			if (!match) {
				Driver::NoteTapError(Driver::TapError::PostNotDeclared, SegmentId::Count,
					fmt::format("the world post chain drew [{}], the description declared [{}]", Names(state.observed), Names(state.predicted)));
			}
		}
	}

	void Install()
	{
		auto& state = Get();
		if (state.installed || REL::Module::IsVR() || !Engine::ImageSpace::Requested())
			return;
		if (Switches::Equals("CS_ORG_SEGMENT_POST", "0")) {
			logger::info("[ORG] post chain: the description check is off (CS_ORG_SEGMENT_POST=0)");
			return;
		}
		stl::detour_thunk<RenderWorldPostProcess>(REL::RelocationID(100430, 107148));
		stl::detour_thunk<RenderEndOfFrameEffects>(REL::RelocationID(99023, 105674));
		state.installed = true;
		logger::info("[ORG] post chain: each world post chain is checked against the frame's post description (CS_ORG_SEGMENT_POST=0 turns it off)");
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
		if (const auto sil = Member<std::uintptr_t>(base, kSil)) {
			fields.sil = Member<bool>(sil, 0x10);
			// The composite adds one to its counter and wraps it at 2 before it draws.
			const auto counter = Member<std::int32_t>(sil, 0x14) + 1;
			fields.silParity = static_cast<std::uint8_t>(counter > 1 ? 0 : counter);
		}
		if (const auto lens = Member<std::uintptr_t>(base, kLensFlares))
			fields.lensFlares = Member<bool>(lens, 0x30);
		fields.eof = Global<bool>(kEnableEndOfFrame);
		fields.ldr = Member<bool>(base, kUsesLdr);
		if (fields.eof)
			for (std::uint32_t slot = 0; slot < 8; ++slot)
				if (Active(EffectAt(slot)))
					fields.slots |= static_cast<std::uint8_t>(1u << slot);
		fields.hdr = Global<bool>(kHdrEnabled);
		fields.effects11 = globals::state && globals::state->PeekTonemapOwner() == ::State::TonemapOwner::kEffects11;
		fields.bloom = static_cast<std::uint8_t>(std::clamp(static_cast<std::int32_t>(Member<float>(base, kBloomRadius)) - 1, 0, 6));
		fields.fade = Member<float>(base, kFade) > 0.0f;
		fields.adapt = Global<std::int32_t>(kAdaptTarget) >= 0 && !Global<bool>(kAdaptReset);
		fields.lumLevels = static_cast<std::uint8_t>(LuminanceLevels());
		fields.lumParity = static_cast<std::uint8_t>(1 - Global<std::int32_t>(kLuminanceParity));
		if (const auto taa = Member<std::uintptr_t>(base, kTemporal)) {
			fields.taa = Member<bool>(taa, 0x18);
			fields.taaMode = static_cast<std::uint8_t>(Member<std::int32_t>(taa, 0x2c));
			fields.taaWater = Member<bool>(taa, 0x38);
			const auto history = Member<std::int32_t>(taa, 0x34);
			fields.taaParity = static_cast<std::uint8_t>(fields.taaMode != 2 ? 1 - history : history);
		}
		fields.dynres = Global<std::int32_t>(kDynamicLock) == 0 && (Global<float>(kDynamicScaleX) != 1.0f || Global<float>(kDynamicScaleY) != 1.0f);
		if (const auto upsample = Member<std::uintptr_t>(base, kUpsample); !upsample || !Member<bool>(upsample, 0x08))
			fields.dynres = false;
		fields.screenshot = Member<bool>(base, kScreenshot);
		auto& upscaling = globals::features::upscaling;
		const auto method = upscaling.GetUpscaleMethod();
		fields.upscaler = upscaling.loaded && !Upscaling::IsWindowUnusable() &&
		                  (method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR || method == Upscaling::UpscaleMethod::kXeSS);
		return fields;
	}

	std::vector<std::uint16_t> PredictEffects(const PostFields& a_fields, Part a_part)
	{
		std::vector<std::uint16_t> out;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (!a_fields.read || !manager)
			return out;
		auto& state = Get();
		RefreshIndex(state);
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		const bool composites = a_part != Part::EndOfFrame;
		const bool endOfFrame = a_part != Part::Composites;
		// RenderWorldPostProcess: the volumetric lighting.
		if (composites && a_fields.volumetric)
			out.push_back(kGroupVolumetricLighting);
		// RenderPreTonemapComposites (99024): SIL (the raw indirect light, then the composite), the lens flares, one composite.
		if (const auto sil = Member<std::uintptr_t>(base, kSil); composites && sil && a_fields.sil) {
			out.push_back(IndexOf(state, reinterpret_cast<const void*>(Member<std::uintptr_t>(sil, 0x08))));
			out.push_back(IndexOf(state, reinterpret_cast<const void*>(Member<std::uintptr_t>(sil, 0x00))));
		}
		if (composites && a_fields.lensFlares)
			out.push_back(kGroupLensFlares);
		if (const auto holder = Member<std::uintptr_t>(base, kComposites); composites && holder && (a_fields.volumetric || a_fields.lensFlares)) {
			const std::uintptr_t which = a_fields.volumetric && a_fields.lensFlares ? 2 : a_fields.volumetric ? 0 : 1;
			out.push_back(IndexOf(state, reinterpret_cast<const void*>(Member<std::uintptr_t>(holder, which * 8))));
		}
		if (!endOfFrame)
			return out;
		// RenderEndOfFrameEffects (99023): the slots before the tonemap, in slot order up to the last active one. With usesLDR, slots 3
		// to 7 wait until after the tonemap.
		bool ldrSkipped = false;
		for (std::uint32_t slot = 0; slot < 8; ++slot) {
			if (!(a_fields.slots >> slot & 1u))
				continue;
			if (a_fields.ldr && slot >= 3) {
				ldrSkipped = true;
				continue;
			}
			out.push_back(SlotGroup(slot));
		}
		// The tonemap call: Effects11 replaces it whole; otherwise the HDR effect, or a copy when HDR is off.
		if (!a_fields.effects11) {
			if (!a_fields.hdr) {
				out.push_back(static_cast<std::uint16_t>(kCopyEffect));
			} else if (const auto* hdr = EffectAt(kHdrEffect)) {
				out.push_back(IndexOf(state, ChildAt(hdr, kHdrDownsample)));
				// The luminance chain: every level but the last when last frame's value exists (the adaptation draws that one).
				for (std::uint32_t level = 0; level < a_fields.lumLevels; ++level)
					if (!a_fields.adapt || level + 1 < a_fields.lumLevels)
						out.push_back(IndexOf(state, ChildAt(hdr, level == 0 ? kHdrLuminanceFirst : kHdrLuminance)));
				if (a_fields.adapt)
					out.push_back(IndexOf(state, ChildAt(hdr, kHdrAdapt)));
				Expand(state, EffectAt(kBloomFirst + a_fields.bloom), out);
				out.push_back(IndexOf(state, ChildAt(hdr, a_fields.fade ? kHdrTonemapFade : kHdrTonemap)));
			}
		}
		// After the tonemap, unless it wrote the frame's target itself: TAA, the screenshot copy, the LDR slots, the upsample.
		const bool final = !a_fields.taa && !ldrSkipped && !a_fields.dynres;
		if (!final) {
			if (const auto taa = Member<std::uintptr_t>(base, kTemporal); taa && a_fields.taa) {
				const std::uintptr_t which = a_fields.taaMode == 2 ? 1 : a_fields.taaWater ? 2 : 0;
				out.push_back(IndexOf(state, reinterpret_cast<const void*>(Member<std::uintptr_t>(taa, which * 8))));
			}
			if (a_fields.screenshot)
				out.push_back(static_cast<std::uint16_t>(kCopyEffect));
			if (ldrSkipped)
				for (std::uint32_t slot = 3; slot < 8; ++slot)
					if (a_fields.slots >> slot & 1u)
						out.push_back(SlotGroup(slot));
			if (const auto upsample = Member<std::uintptr_t>(base, kUpsample); upsample && a_fields.dynres)
				out.push_back(IndexOf(state, reinterpret_cast<const void*>(Member<std::uintptr_t>(upsample, 0x00))));
		}
		return out;
	}

	std::string EffectName(std::uint16_t a_effect)
	{
		switch (a_effect) {
		case kGroupVolumetricLighting:
			return "(volumetric lighting)";
		case kGroupLensFlares:
			return "(lens flares)";
		case kGroupDepthOfField:
			return "(depth of field)";
		case kGroupFullScreenBlur:
			return "(full-screen blur)";
		case kGroupGetHit:
			return "(get hit)";
		case kGroupWorldMap:
			return "(world map)";
		case kGroupRadialBlur:
			return "(radial blur)";
		case kGroupRefraction:
			return "(refraction)";
		default:
			break;
		}
		if (a_effect >= kGroupBase - 1)
			return "(not an effect)";
		const auto name = RE::ImageSpaceManager::GetImageSpaceEffectName(static_cast<RE::ImageSpaceManager::ImageSpaceEffectEnum>(a_effect));
		return name == "Unknown" ? std::to_string(a_effect) : name;
	}

	void OnBegin(Description& a_frame)
	{
		auto& state = Get();
		if (!On() || (a_frame.kind != Kind::World && !(a_frame.kind == Kind::FrozenWorld && a_frame.capture)))
			return;
		a_frame.post = Read();
		const std::lock_guard lock(state.mutex);
		++state.frames;
		// A slot's copy is recorded again when its description changes: compare with the same slot's last one. The parity fields
		// alternate with the frame, so they stay equal for a slot while the engine renders one chain a frame.
		auto& kept = state.bySlot[a_frame.slot];
		if (state.haveSlot[a_frame.slot]) {
			if (const auto diff = DiffPost(kept, a_frame.post, true); !diff.empty()) {
				++state.changedFrames;
				CountFields(state.changes, diff);
			}
		}
		kept = a_frame.post;
		state.haveSlot[a_frame.slot] = true;
	}

	void NoteShader(const void* a_effect)
	{
		auto& state = Get();
		if (!state.inChain || !a_effect)
			return;
		state.observed.push_back(IndexOf(state, a_effect));
	}

	void LogStats()
	{
		auto& state = Get();
		if (!On())
			return;
		const std::lock_guard lock(state.mutex);
		const auto list = [](const std::map<std::string, std::uint64_t>& a_counts) {
			std::string out;
			for (const auto& [name, count] : a_counts)
				out += fmt::format("{}{} {}", out.empty() ? "" : ", ", name, count);
			return out.empty() ? std::string("none") : out;
		};
		logger::info("[ORG] post description: {} world frames, {} changed against their slot's last description ({}); {} chains checked, {} matched; "
					 "{} chains read other fields at their entry than at Begin ({}); {} read other fields at the end-of-frame effects than at the chain's entry ({})",
			state.frames, state.changedFrames, list(state.changes), state.chains, state.matched, state.decisionFrames, list(state.decisions),
			state.eofDecisionFrames, list(state.eofDecisions));
		state.frames = state.chains = state.matched = state.changedFrames = state.decisionFrames = state.eofDecisionFrames = 0;
		state.changes.clear();
		state.decisions.clear();
		state.eofDecisions.clear();
	}
}
