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
		// The volumetric lighting's frame state (a static object, made on first use by its getter, SE 100297 / AE 107014): its counter at
		// +0x88 picks the target the VL writes and the composite reads, 43 + (~counter & 1) (RenderWorldPostProcess and 100493 on SE;
		// 107148 on AE).
		constexpr REL::RelocationID kVolumetricState{ 100297, 107014 };
		constexpr std::uintptr_t kVolumetricCounter = 0x88;
		// The depth of field (end-of-frame slot 3, ImageSpaceEffectDepthOfField::Render SE 0x1412f90d0 / AE 107385): its Render runs each
		// of five steps whose flag is set in the bool array at +0x98 (the downsample, the blur, three composites), and the blur takes the
		// compute form (the CS blur helper at manager +0x188, 100488) when this option is set.
		constexpr REL::RelocationID kDofComputeBlur{ 513727, 391684 };
		constexpr std::uintptr_t kDofSteps = 0x98;
		constexpr std::uint32_t kDofSlot = 3;
		constexpr std::uint32_t kRefractionSlot = 1;

		// ImageSpaceManager members, the same on SE and AE (RUNTIME_DATA; raw offsets where CommonLib's names are wrong).
		constexpr std::uintptr_t kBloomRadius = 0xcc;        // ImageSpaceData: the bloom blur radius (float)
		constexpr std::uintptr_t kFade = 0x118;              // the cinematic fade amount: above 0 picks the tonemap's fade form
		constexpr std::uintptr_t kVolumetricEffect = 0x170;  // the VL effect (active at +0x08)
		constexpr std::uintptr_t kLensFlares = 0x180;        // the image-based lens flares (active at +0x30)
		constexpr std::uintptr_t kComposites = 0x190;
		constexpr std::uintptr_t kComputeBlur = 0x188;       // the separable compute blur helper (the VL blur, the depth of field's blur)        // the VL / lens flare composites: VL only, lens flare only, both
		constexpr std::uintptr_t kSil = 0x1d8;               // the SIL composite: composite, raw indirect, active at +0x10, counter at +0x14
		constexpr std::uintptr_t kTemporal = 0x1f0;          // TAA: shader, UI form, water form; enabled +0x18, form +0x2c, history +0x34, water +0x38
		constexpr std::uintptr_t kUpsample = 0x1f8;          // the dynamic-resolution upsample: its shader, active at +0x08
		constexpr std::uintptr_t kSwapTarget = 0x60;         // RenderEndOfFrameEffects' ping-pong partner of the source
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
			PostFields chainFields{};  // the fields read at RenderWorldPostProcess's entry, completed at RenderEndOfFrameEffects' entry
			std::vector<std::uint16_t> predicted;
			std::vector<Draw> predictedDraws;  // predicted, with each draw's row
			std::vector<std::uint16_t> observed;
			// The running match of the observed draws against predictedDraws (CurrentRow): the next predicted draw, whether the group
			// there took a draw already, the row of the last draw, and whether the chain left its prediction.
			std::size_t matchAt = 0;
			bool groupTaken = false;
			bool matchLost = false;
			bool haveRow = false;
			RowKey currentRow{};
			// By frame slot: the fields the slot's last chain was predicted from (SlotFields).
			std::array<PostFields, RenderGraphRuntime::kGameFramesInFlight> chainBySlot{};
			std::array<bool, RenderGraphRuntime::kGameFramesInFlight> haveChain{};
			// RenderEndOfFrameEffects' targets as the last world chain had them (the Begin read's guess).
			std::int8_t lastSource = -1, lastDest = -1, lastSwap = -1;
			std::uint64_t chainFrame = UINT64_MAX;  // the frame of the last world chain that started
			std::size_t eofStart = 0;               // where the end-of-frame part starts in predictedDraws
			bool dofRead = false;                   // the depth of field's Render was read at its first draw this chain
			void (*partHook)(Part, const std::vector<Draw>&) = nullptr;  // SetPartHook
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
			std::uint64_t rowsChecked = 0, rowsMatched = 0;  // declared steps' draws checked against their table rows, and matched
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

		/** @brief The depth of field's step flags now (its bool array at +0x98), a bit per step. */
		std::uint8_t DofSteps()
		{
			std::uint8_t out = 0;
			if (const auto* dof = EffectAt(kDofSlot))
				if (const auto steps = Member<const std::uint8_t*>(reinterpret_cast<std::uintptr_t>(dof), kDofSteps))
					for (std::uint32_t step = 0; step < 5; ++step)
						if (steps[step])
							out |= static_cast<std::uint8_t>(1u << step);
			return out;
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
		void EndOfFrameStart(std::int32_t a_source, std::int32_t a_destination);

		/**
		 * @brief ImageSpaceManager::RenderEndOfFrameEffects (SE 99023, AE 105674): the end-of-frame part's decision event. Inside the
		 * world chain it runs within Upscaling's wrapper (Upscaling::Main_PostProcessing), after the upscaler set TAA and dynamic
		 * resolution for the frame. UI3DSceneManager::Render (51855) calls it for a menu's 3D scene too, outside the chain.
		 */
		struct RenderEndOfFrameEffects
		{
			static void thunk(void* a_manager, std::uint64_t a_source, std::uint64_t a_destination, std::uint64_t a_arg4, std::uint64_t a_arg5)
			{
				EndOfFrameStart(static_cast<std::int32_t>(a_source), static_cast<std::int32_t>(a_destination));
				func(a_manager, a_source, a_destination, a_arg4, a_arg5);
				// The world chain's end-of-frame effects returned: the end of the post end-of-frame segment (a point step).
				const bool world = Get().endOfFrame;
				Get().endOfFrame = false;
				if (world)
					(void)FrameAnchors::TapPoint(static_cast<std::uint32_t>(RenderGraphRuntime::Segment::DeclaredPostEnd));
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
			state.chainFrame = frame.frame;
			state.predictedDraws = PredictDraws(fields, Part::Composites);
			state.predicted.clear();
			for (const auto& draw : state.predictedDraws)
				state.predicted.push_back(draw.effect);
			state.observed.clear();
			state.matchAt = 0;
			state.groupTaken = false;
			state.matchLost = false;
			state.haveRow = false;
			state.eofStart = state.predictedDraws.size();
			state.inChain = true;
			if (state.partHook)
				state.partHook(Part::Composites, state.predictedDraws);
		}

		void EndOfFrameStart(std::int32_t a_source, std::int32_t a_destination)
		{
			auto& state = Get();
			if (!state.inChain || state.endOfFrame)
				return;
			state.endOfFrame = true;
			auto fields = Read();
			fields.eofSource = static_cast<std::int8_t>(a_source);
			fields.eofDest = static_cast<std::int8_t>(a_destination);
			if (auto* manager = RE::ImageSpaceManager::GetSingleton())
				fields.eofSwap = static_cast<std::int8_t>(Member<std::int32_t>(reinterpret_cast<std::uintptr_t>(manager), kSwapTarget));
			state.lastSource = fields.eofSource;
			state.lastDest = fields.eofDest;
			state.lastSwap = fields.eofSwap;
			if (const auto diff = DiffPost(state.chainFields, fields, true); !diff.empty()) {
				const std::lock_guard lock(state.mutex);
				++state.eofDecisionFrames;
				CountFields(state.eofDecisions, diff);
				if (++state.decisionLines <= 8)
					logger::info("[ORG] post description: frame {} decided at the end-of-frame effects, after the chain's entry: {}", Driver::Current().frame, diff);
			}
			const auto rest = PredictDraws(fields, Part::EndOfFrame);
			state.eofStart = state.predictedDraws.size();
			state.dofRead = false;
			state.predictedDraws.insert(state.predictedDraws.end(), rest.begin(), rest.end());
			for (const auto& draw : rest)
				state.predicted.push_back(draw.effect);
			state.chainFields = fields;
			const auto slot = Driver::Current().slot;
			state.chainBySlot[slot] = fields;
			state.haveChain[slot] = true;
			if (state.partHook)
				state.partHook(Part::EndOfFrame, rest);
		}

		/** @brief Advances the running match by one observed draw (Matches, one draw at a time). */
		void MatchDraw(State& a_state, std::uint16_t a_effect)
		{
			a_state.haveRow = false;
			while (!a_state.matchLost) {
				if (a_state.matchAt >= a_state.predictedDraws.size()) {
					a_state.matchLost = true;
					return;
				}
				const auto& want = a_state.predictedDraws[a_state.matchAt];
				if (want.effect < kGroupBase) {
					if (want.effect != a_effect) {
						a_state.matchLost = true;
						return;
					}
					++a_state.matchAt;
					a_state.currentRow = want.row;
					a_state.haveRow = want.row.step != Step::None;
					return;
				}
				const bool next = a_state.matchAt + 1 < a_state.predictedDraws.size() && a_state.predictedDraws[a_state.matchAt + 1].effect == a_effect;
				if (InGroup(want.effect, a_effect) && !(a_state.groupTaken && next)) {
					a_state.groupTaken = true;
					return;
				}
				if (a_state.groupTaken || want.effect == kGroupVolumetricLighting) {
					++a_state.matchAt;
					a_state.groupTaken = false;
					continue;
				}
				a_state.matchLost = true;
			}
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
		if (fields.volumetric) {
			static REL::Relocation<std::uintptr_t (*)()> volumetricState{ kVolumetricState };
			if (const auto vl = volumetricState())
				fields.vlParity = static_cast<std::uint8_t>(~Member<std::uint32_t>(vl, kVolumetricCounter) & 1u);
		}
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
		if (fields.slots >> kDofSlot & 1u)
			fields.dofSteps = DofSteps();
		fields.dofCompute = Global<bool>(kDofComputeBlur);
		// The address mode the shadow state holds for the first pixel-shader sampler now: the VL composite leaves it as it finds it.
		fields.compositeAddress = static_cast<std::uint8_t>(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().PSTextureAddressMode[0].underlying() & 0xf);
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
		fields.dynScaled = Global<std::int32_t>(kDynamicLock) == 0 && (Global<float>(kDynamicScaleX) != 1.0f || Global<float>(kDynamicScaleY) != 1.0f);
		fields.screenshot = Member<bool>(base, kScreenshot);
		// RenderEndOfFrameEffects' targets are its arguments: until its entry this frame, the last chain's.
		const auto& state = Get();
		fields.eofSource = state.lastSource;
		fields.eofDest = state.lastDest;
		fields.eofSwap = state.lastSwap;
		// The normals target of this frame, as Deferred::StartDeferred found it bound (forwardRenderTargets[2]).
		if (globals::deferred)
			fields.normals = static_cast<std::int8_t>(globals::deferred->forwardRenderTargets[2]);
		auto& upscaling = globals::features::upscaling;
		const auto method = upscaling.GetUpscaleMethod();
		fields.upscaler = upscaling.loaded && !Upscaling::IsWindowUnusable() &&
		                  (method == Upscaling::UpscaleMethod::kDLSS || method == Upscaling::UpscaleMethod::kFSR || method == Upscaling::UpscaleMethod::kXeSS);
		return fields;
	}

	std::vector<std::uint16_t> PredictEffects(const PostFields& a_fields, Part a_part)
	{
		std::vector<std::uint16_t> out;
		for (const auto& draw : PredictDraws(a_fields, a_part))
			out.push_back(draw.effect);
		return out;
	}

	std::vector<Draw> PredictDraws(const PostFields& a_fields, Part a_part)
	{
		std::vector<Draw> out;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		if (!a_fields.read || !manager)
			return out;
		auto& state = Get();
		RefreshIndex(state);
		const auto base = reinterpret_cast<std::uintptr_t>(manager);
		const bool composites = a_part != Part::EndOfFrame;
		const bool endOfFrame = a_part != Part::Composites;
		const auto group = [&](std::uint16_t a_group) { out.push_back({ a_group, {} }); };
		const auto step = [&](std::uint16_t a_effect, Step a_step, std::uint32_t a_index = 0, std::uint32_t a_parity = 0, std::int32_t a_in = -1, std::int32_t a_out = -1) {
			out.push_back({ a_effect,
				{ a_step, static_cast<std::uint8_t>(a_index), static_cast<std::uint8_t>(a_parity), static_cast<std::int8_t>(a_in), static_cast<std::int8_t>(a_out) } });
		};
		const auto effectOf = [&](std::uintptr_t a_object) { return IndexOf(state, reinterpret_cast<const void*>(a_object)); };
		// RenderWorldPostProcess: the volumetric lighting.
		if (composites && a_fields.volumetric)
			group(kGroupVolumetricLighting);
		// RenderPreTonemapComposites (99024): SIL (the raw indirect light, then the composite), the lens flares, one composite.
		if (const auto sil = Member<std::uintptr_t>(base, kSil); composites && sil && a_fields.sil) {
			step(effectOf(Member<std::uintptr_t>(sil, 0x08)), Step::SilRaw, 0, a_fields.silParity);
			step(effectOf(Member<std::uintptr_t>(sil, 0x00)), Step::SilComposite, 0, a_fields.silParity);
		}
		if (composites && a_fields.lensFlares)
			group(kGroupLensFlares);
		if (const auto holder = Member<std::uintptr_t>(base, kComposites); composites && holder && (a_fields.volumetric || a_fields.lensFlares)) {
			const std::uintptr_t which = a_fields.volumetric && a_fields.lensFlares ? 2 : a_fields.volumetric ? 0 : 1;
			// Its first sampler's address mode is not set by the composite (100493 sets the second's): it is what the shadow state holds
			// at its call (compositeAddress, read there), part of the row's key.
			step(effectOf(Member<std::uintptr_t>(holder, which * 8)), Step::Composite, static_cast<std::uint32_t>(which) | (a_fields.compositeAddress << 4),
				a_fields.volumetric ? a_fields.vlParity : 0);
		}
		if (!endOfFrame)
			return out;
		// RenderEndOfFrameEffects (99023 / 105674), its targets followed as it follows them: the slots before the tonemap ping-pong
		// between the source and the swap target (+0x60), in slot order up to the last active one. With usesLDR, slots 3 to 7 wait
		// until after the tonemap; the last of those is remembered.
		constexpr std::int32_t kTempCopy = 0x29;   // the HDR effect's output when a step follows it
		constexpr std::int32_t kTempCopy2 = 0x2a;  // its partner after it
		constexpr std::int32_t kScreenshotTarget = 0x4b;
		std::int32_t source = a_fields.eofSource, swap = a_fields.eofSwap;
		const std::int32_t destination = a_fields.eofDest;
		// A slot's effect: one shader (FXAA), the depth of field's steps, or a group the description does not settle yet.
		const auto slotDraws = [&](std::uint32_t a_slot, bool a_afterTonemap, std::int32_t a_in, std::int32_t a_out) {
			const std::uint32_t late = a_afterTonemap ? 0x80u : 0u;
			const auto effect = SlotGroup(a_slot);
			if (effect < kGroupBase) {
				step(effect, Step::Slot, a_slot | late, 0, a_in, a_out);
				return;
			}
			// Refraction (ImageSpaceEffectRefraction::Render, SE 100962 / AE 107744) draws its one shader (its first child) from the
			// source into the destination with the refraction normals.
			if (a_slot == kRefractionSlot) {
				std::vector<std::uint16_t> draws;
				if (const auto* refraction = EffectAt(kRefractionSlot))
					Expand(state, ChildAt(refraction, 0), draws);
				if (draws.size() != 1) {
					group(effect);
					return;
				}
				step(draws[0], Step::Slot, a_slot | late, 0, a_in, a_out);
				return;
			}
			const auto* dof = a_slot == kDofSlot ? EffectAt(kDofSlot) : nullptr;
			if (!dof || !a_fields.dofSteps) {
				group(effect);
				return;
			}
			for (std::uint32_t k = 0; k < 5; ++k) {
				if (!(a_fields.dofSteps >> k & 1u))
					continue;
				std::vector<std::uint16_t> draws;
				if (k == 1 && a_fields.dofCompute) {
					// The compute blur helper (+0x188, 100488) dispatches its horizontal shader (+0x30), then its vertical one (+0x38).
					if (const auto helper = Member<std::uintptr_t>(base, kComputeBlur))
						draws = { effectOf(Member<std::uintptr_t>(helper, 0x30)), effectOf(Member<std::uintptr_t>(helper, 0x38)) };
				} else {
					Expand(state, ChildAt(dof, k), draws);
				}
				for (std::uint32_t i = 0; i < draws.size() && i < 8; ++i)
					step(draws[i], Step::Dof, late | (k * 8 + i), 0, a_in, a_out);
			}
		};
		std::int32_t ldrLast = -1;
		for (std::uint32_t slot = 0; slot < 8; ++slot) {
			if (!(a_fields.slots >> slot & 1u))
				continue;
			if (a_fields.ldr && slot >= 3) {
				ldrLast = static_cast<std::int32_t>(slot);
				continue;
			}
			slotDraws(slot, false, source, swap);
			std::swap(source, swap);
		}
		// The tonemap call writes the destination itself when nothing follows it: no TAA, no LDR slot, no dynamic resolution.
		const bool final = !a_fields.taa && ldrLast < 0 && !a_fields.dynScaled;
		std::int32_t current = final ? destination : kTempCopy;
		if (!a_fields.effects11) {
			if (!a_fields.hdr) {
				step(static_cast<std::uint16_t>(kCopyEffect), Step::Copy, 0, 0, source, current);
			} else if (const auto* hdr = EffectAt(kHdrEffect)) {
				// The steps that read the luminance (the adaptation, the bright pass, the tonemap) name its target as their parity: the last
				// level's, which alternates by lumParity (+2) and moves with the level count (the resolution).
				const auto lum = a_fields.lumLevels ? static_cast<std::uint32_t>(kLuminanceFirstTarget + a_fields.lumLevels - 1 + 2 * a_fields.lumParity) : 0u;
				step(IndexOf(state, ChildAt(hdr, kHdrDownsample)), Step::Downsample, 0, 0, source);
				// The luminance chain: every level but the last when last frame's value exists (the adaptation draws that one). Only the
				// last level's target alternates.
				for (std::uint32_t level = 0; level < a_fields.lumLevels; ++level)
					if (!a_fields.adapt || level + 1 < a_fields.lumLevels)
						step(IndexOf(state, ChildAt(hdr, level == 0 ? kHdrLuminanceFirst : kHdrLuminance)), Step::Luminance, level,
							level + 1 == a_fields.lumLevels ? a_fields.lumParity : 0);
				if (a_fields.adapt)
					step(IndexOf(state, ChildAt(hdr, kHdrAdapt)), Step::Adapt, 0, lum);
				// The bloom composite draws its children in order.
				std::vector<std::uint16_t> children;
				Expand(state, EffectAt(kBloomFirst + a_fields.bloom), children);
				for (std::uint32_t child = 0; child < children.size(); ++child)
					step(children[child], Step::Bloom, a_fields.bloom * 8u + child, child == 0 ? lum : 0u);
				step(IndexOf(state, ChildAt(hdr, a_fields.fade ? kHdrTonemapFade : kHdrTonemap)), Step::Tonemap, a_fields.fade ? 1 : 0, lum, source, current);
			}
		}
		if (final)
			return out;
		// After the tonemap: TAA from the temporary copy into the destination when it is the last step, else into its partner.
		std::int32_t next = kTempCopy2;
		if (const auto taa = Member<std::uintptr_t>(base, kTemporal); taa && a_fields.taa) {
			const std::uintptr_t which = a_fields.taaMode == 2 ? 1 : a_fields.taaWater ? 2 : 0;
			const bool last = ldrLast < 0 && !a_fields.dynScaled;
			const std::int32_t written = last ? destination : kTempCopy2;
			// It reads the frame's normals target, which alternates with its own phase: part of the key (+ 0x10 for the swap).
			// Its sixth sampler is the one it finds (taaAddress, read at its call): + 0x20 per address mode.
			const std::uint32_t normals = a_fields.normals == 5 ? 0x10u : 0u;
			step(effectOf(Member<std::uintptr_t>(taa, which * 8)), Step::Taa, static_cast<std::uint32_t>(which) | normals | ((a_fields.taaAddress & 7u) << 5), a_fields.taaParity,
				current, written);
			if (!last) {
				next = current;
				current = written;
			}
		}
		// The screenshot copy reads the partner (the engine's own choice), into the screenshot target.
		if (a_fields.screenshot)
			step(static_cast<std::uint16_t>(kCopyEffect), Step::Screenshot, 0, 0, next, kScreenshotTarget);
		// The LDR slots ping-pong from the current image; the last writes the destination unless dynamic resolution follows.
		for (std::uint32_t slot = 3; slot < 8 && ldrLast >= 0; ++slot) {
			if (!(a_fields.slots >> slot & 1u))
				continue;
			const bool last = static_cast<std::int32_t>(slot) == ldrLast && !a_fields.dynScaled;
			const std::int32_t written = last ? destination : next;
			slotDraws(slot, true, current, written);
			if (!last) {
				next = current;
				current = written;
			}
		}
		if (const auto upsample = Member<std::uintptr_t>(base, kUpsample); upsample && a_fields.dynres && a_fields.dynScaled)
			step(effectOf(Member<std::uintptr_t>(upsample, 0x00)), Step::Upsample, 0, 0, current, destination);
		return out;
	}

	const char* StepName(Step a_step)
	{
		switch (a_step) {
		case Step::SilRaw:
			return "sil raw";
		case Step::SilComposite:
			return "sil composite";
		case Step::Composite:
			return "composite";
		case Step::Slot:
			return "slot";
		case Step::Copy:
			return "copy";
		case Step::Downsample:
			return "downsample";
		case Step::Luminance:
			return "luminance";
		case Step::Adapt:
			return "adapt";
		case Step::Bloom:
			return "bloom";
		case Step::Tonemap:
			return "tonemap";
		case Step::Taa:
			return "taa";
		case Step::Screenshot:
			return "screenshot";
		case Step::Upsample:
			return "upsample";
		case Step::Dof:
			return "dof";
		default:
			return "none";
		}
	}

	std::string RowName(const RowKey& a_key)
	{
		return fmt::format("{} {} p{} {}->{}", StepName(a_key.step), a_key.index, a_key.parity, a_key.in, a_key.out);
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
		const auto index = IndexOf(state, a_effect);
		state.observed.push_back(index);
		// A step that draws with sampler state it does not set itself (the VL composite's first sampler, TAA's sixth) is decided at its
		// call: the address mode the shadow state holds then is the one its draw takes. When it differs from the description's, the part
		// is predicted again from there (the draws before it are the same).
		if (state.matchAt < state.predictedDraws.size()) {
			const auto& next = state.predictedDraws[state.matchAt];
			const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			auto& fields = state.chainFields;
			bool again = false;
			if (next.row.step == Step::Composite) {
				const auto address = static_cast<std::uint8_t>(shadow.PSTextureAddressMode[0].underlying() & 0xf);
				again = address != fields.compositeAddress;
				fields.compositeAddress = address;
			} else if (next.row.step == Step::Taa) {
				const auto address = static_cast<std::uint8_t>(shadow.PSTextureAddressMode[5].underlying() & 0x7);
				again = address != fields.taaAddress;
				fields.taaAddress = address;
			}
			if (again) {
				const bool late = state.endOfFrame;
				const auto from = late ? state.eofStart : std::size_t{ 0 };
				auto redone = PredictDraws(fields, late ? Part::EndOfFrame : Part::Composites);
				if (!late) {
					// The composites part only: keep the end-of-frame part, if any.
					redone.insert(redone.end(), state.predictedDraws.begin() + static_cast<std::ptrdiff_t>((std::min)(state.eofStart, state.predictedDraws.size())),
						state.predictedDraws.end());
				}
				state.predictedDraws.resize(from);
				state.predictedDraws.insert(state.predictedDraws.end(), redone.begin(), redone.end());
				state.predicted.resize(from);
				for (std::size_t i = from; i < state.predictedDraws.size(); ++i)
					state.predicted.push_back(state.predictedDraws[i].effect);
				state.chainBySlot[Driver::Current().slot] = fields;
			}
		}
		// The depth of field's Render is its own decision event: it picks the composite each step draws (fogged, masked, distant) as it
		// runs. At its first draw the end-of-frame part is predicted again from the effect as it is now; the draws before it are the same.
		if (!state.dofRead && state.endOfFrame && state.matchAt < state.predictedDraws.size() && state.predictedDraws[state.matchAt].row.step == Step::Dof) {
			state.dofRead = true;
			auto& fields = state.chainFields;
			fields.dofSteps = DofSteps();
			state.chainBySlot[Driver::Current().slot] = fields;
			const auto again = PredictDraws(fields, Part::EndOfFrame);
			state.predictedDraws.resize(state.eofStart);
			state.predictedDraws.insert(state.predictedDraws.end(), again.begin(), again.end());
			state.predicted.resize(state.eofStart);
			for (const auto& draw : again)
				state.predicted.push_back(draw.effect);
		}
		MatchDraw(state, index);
	}

	const RowKey* CurrentRow()
	{
		const auto& state = Get();
		return state.inChain && state.haveRow && !state.matchLost ? &state.currentRow : nullptr;
	}

	void NoteRowCheck(bool a_matched)
	{
		auto& state = Get();
		const std::lock_guard lock(state.mutex);
		++state.rowsChecked;
		state.rowsMatched += a_matched;
	}

	Part CurrentPart()
	{
		const auto& state = Get();
		return !state.inChain ? Part::All : state.endOfFrame ? Part::EndOfFrame : Part::Composites;
	}

	void SetPartHook(void (*a_hook)(Part a_part, const std::vector<Draw>& a_draws))
	{
		Get().partHook = a_hook;
	}

	bool InChain()
	{
		return Get().inChain;
	}

	PostFields SlotFields(std::uint32_t a_slot, bool* a_have)
	{
		const auto& state = Get();
		const bool have = a_slot < state.haveChain.size() && state.haveChain[a_slot];
		if (a_have)
			*a_have = have;
		return have ? state.chainBySlot[a_slot] : Driver::Current().post;
	}

	PostFields ChainFields()
	{
		const auto& state = Get();
		return state.inChain ? state.chainFields : Driver::Current().post;
	}

	PostFields DeclaredFields()
	{
		const auto& state = Get();
		const auto& frame = Driver::Current();
		return state.chainFrame == frame.frame ? state.chainFields : SlotFields(frame.slot);
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
					 "{} chains read other fields at their entry than at Begin ({}); {} read other fields at the end-of-frame effects than at the chain's entry ({}); "
					 "{} draws of declared steps checked against the pass-state table, {} matched",
			state.frames, state.changedFrames, list(state.changes), state.chains, state.matched, state.decisionFrames, list(state.decisions),
			state.eofDecisionFrames, list(state.eofDecisions), state.rowsChecked, state.rowsMatched);
		state.rowsChecked = state.rowsMatched = 0;
		state.frames = state.chains = state.matched = state.changedFrames = state.decisionFrames = state.eofDecisionFrames = 0;
		state.changes.clear();
		state.decisions.clear();
		state.eofDecisions.clear();
	}
}
