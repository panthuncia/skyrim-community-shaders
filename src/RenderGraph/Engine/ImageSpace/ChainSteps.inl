// The world post chain as CS passes, in graph mode. Included by ImageSpace.cpp after WorldSteps.inl, whose step helpers it uses.
//
// CS declares a pass for each step of RenderWorldPostProcess (SE 100430 / AE 107148), in the engine's order.
// The engine's draw is withheld. Its hook only stages the constants the engine computed (TakeChainStep).
//
// The chain works in place on two images: kMAIN before the tonemap and the final target after it. Every step reads and writes
// fixed targets. An effect that cannot write the image it reads draws into a scratch target, and a copy puts the result back.
// So a missing or skipped effect leaves the image as the step before it left it.

/** @brief Groups a frame's description turns on or off together. */
namespace ChainGroup
{
	constexpr std::uint8_t kComposite = 0;
	constexpr std::uint8_t kPreSlot = 1;   // + the slot, 0 to 7
	constexpr std::uint8_t kLdrSlot = 9;   // + the slot - 3, slots 3 to 7
	constexpr std::uint8_t kHdrCopy = 14;  // the copy in the HDR effect's place
	constexpr std::uint8_t kHdr = 15;      // downsample and all luminance levels but the last
	constexpr std::uint8_t kHdrLum = 16;   // last level by the luminance shader, no adapted luminance yet
	constexpr std::uint8_t kHdrAdapt = 17; // last level by the light adaptation
	constexpr std::uint8_t kBloom = 18;    // + the radius index, 0 to 6
	constexpr std::uint8_t kTonemap = 25;
	constexpr std::uint8_t kTonemapFade = 26;
	constexpr std::uint8_t kTaa = 27;
	constexpr std::uint8_t kScreenshot = 28;
	constexpr std::uint8_t kUpsample = 29;
	constexpr std::uint8_t kCapture = 30;  // a frozen world's capture frame copies the final target into kMENUBG
	constexpr std::uint8_t kLensFlare = 31;  // the lens flares' visibility and sprite passes
	constexpr std::uint8_t kIblf = 32;       // the image-based lens flares' downsamples, flares, blurs and sums
	constexpr std::uint8_t kCount = 33;
}

constexpr std::array<PostPart, 3> kChainParts{ PostPart::Composites, PostPart::Hdr, PostPart::Final };

/**
* @brief What the chain's registration is a function of. Its three clients are declared on these variables (Frame/Declare.h), so
* a change registers them again when a revision starts, and the revision switches the new builds in.
*/
struct ChainKey
{
	Frame::Vars::Extent2D display{};
	Frame::Vars::TargetIdentity hdr{};  // HDR Display's scene texture, the final target while HDR is on
	std::uint32_t levels = 0, effects = 0, states = 0;
	friend bool operator==(const ChainKey&, const ChainKey&) = default;
};

/** @brief The chain's build for a key. Its three parts share the post stage's rows, in order. */
struct ChainBuild : StepBuild<ChainKey, 3>
{
	std::uint32_t levels = 0;
	// Part 2's frozen background copy and the routed build's pass-through copy, or kNoCopy.
	std::uint16_t captureEntry = kNoCopy;
	std::uint16_t throughEntry = kNoCopy;
};

struct ChainSteps
{
	StepGroup<ChainBuild> group;
	// The newest build's sizes, final target and HDR scene texture (null while HDR is off). Its specs and the hooks read them.
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
	std::int32_t final = RE::RENDER_TARGET::kFRAMEBUFFER;
	ID3D11Texture2D* hdrTarget = nullptr;
	DXGI_FORMAT hdrFormat = DXGI_FORMAT_UNKNOWN;
	OwnedTarget bloom, bloomSwap, dofDown, dofSwap;
	std::array<OwnedTarget, 2> taaHistory;
	OwnedTarget getHitA, getHitB, getHitSwap, fullBlurDown, fullBlurSwap;
	// Each scratch matches its image's size and format, so the copy back is a plain texel copy.
	OwnedTarget hdrScratch, ldrScratch;
	// The adapted luminance pair, published for DCLF's character light, which reads it in place of the engine's target.
	std::array<OwnedTarget, 2> adapt;
	// From ImageSpaceEffectBlur's HDR flag at +0x95. True for ISBlurN, false for ISNonHDRBlurN.
	std::array<bool, 7> blurHdr{};
	bool inComposites = false, inEndOfFrame = false, inHdr = false, hdrCopy = false, copyDrawn = false;
	std::uint32_t lumLevel = 0;
	// Slot 6 or 7 while inside the full-screen blur or get-hit effect, -1 elsewhere.
	std::int32_t slotScope = -1;
	std::map<Effects::ImageSpaceEffectEnum, std::uint8_t> slotDraws;
	bool captureFrame = false;
	bool scratchWritten = false, taaRouted = false;
	// The frame's lens flare sprites so far and the vertices each draws (StageLensFlareSprite). Render owner.
	std::uint64_t lensFrame = UINT64_MAX;
	std::uint32_t lensSprites = 0, lensVerticesPerSprite = 0;
	// Whether the engine's target and blend at each lens flare pass were checked against the step's. Once a session.
	bool lensVisibilityChecked = false, lensSpriteChecked = false;
	// The image-based flares' draws this frame so far, by effect, t0 and output target (IblfVariantNow). Cleared as the composites start.
	std::map<std::tuple<std::int32_t, std::int32_t, std::int32_t>, std::uint32_t> iblfDraws;
};

/**
* @brief TAA cannot write the image it reads. Direct, TAA draws into the LDR scratch and a copy puts it back.
* Routed, the tonemap draws into the scratch and TAA reads it into the final target. Each build holds one routing.
*/
namespace ChainRoute
{
	constexpr std::uint8_t kAny = 0;
	constexpr std::uint8_t kDirect = 1;
	constexpr std::uint8_t kRouted = 2;
	constexpr std::uint16_t kTonemapBit = 1u << 14;  // bit 15 is the parity
	constexpr std::uint16_t kTaaBit = 1u << 13;      // bits 15 and 14 are the history and the normals target
}

ChainSteps& Chain()
{
	static ChainSteps chain;
	return chain;
}

ID3D11Texture2D* ChainHdrTarget()
{
	return Chain().hdrTarget;
}

Frame::SegmentId SegmentOfPart(std::size_t)
{
	return Frame::SegmentId::Post;
}

/** @brief Whether any of the group's rows are on in the post stage, in the builds the frame runs. Render owner. */
bool GroupScheduled(std::uint8_t a_group)
{
	const auto& chain = Chain();
	for (std::size_t p = 0; p < kChainParts.size(); ++p) {
		const auto held = chain.group.Held(p);
		if (!held)
			continue;
		const auto& part = held->table->parts[p];
		for (std::uint32_t entry = 0; entry < part.count; ++entry)
			if (part.group[entry] == a_group && RowOn(SegmentOfPart(p), part.RowOf(entry)))
				return true;
	}
	return false;
}

/** @brief ISHDRDownSample4 outside the HDR effect belongs to the depth of field. */
std::int32_t SlotOfEffect(Effects::ImageSpaceEffectEnum a_effect)
{
	using E = Effects;
	switch (a_effect) {
	case E::ISWorldMap:
	case E::ISWorldMapNoSkyBlur:
		return 0;
	case E::ISRefraction:
		return 1;
	case E::ISFXAA:
		return 2;
	case E::ISDepthOfField:
	case E::ISDepthOfFieldFogged:
	case E::ISDepthOfFieldMaskedFogged:
	case E::ISDistantBlur:
	case E::ISDistantBlurFogged:
	case E::ISDistantBlurMaskedFogged:
	case E::ISDepthOfFieldBlurHCS:
	case E::ISDepthOfFieldBlurVCS:
	case E::ISHDRDownSample4:
		return 3;
	case E::ISRadialBlur:
	case E::ISRadialBlurMedium:
	case E::ISRadialBlurHigh:
		return 5;
	case E::ISDoubleVision:
		return 7;
	default:
		return -1;
	}
}

std::uint8_t SlotGroup(std::uint32_t a_slot, bool a_ldr)
{
	return static_cast<std::uint8_t>(a_ldr ? ChainGroup::kLdrSlot + (a_slot - 3) : ChainGroup::kPreSlot + a_slot);
}

/** @brief usesLDR (manager +0x210) and a slot from 3 to 7. */
bool SlotIsLdr(std::uint32_t a_slot)
{
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	const bool ldr = manager && *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(manager) + 0x210);
	return ldr && a_slot >= 3 && a_slot <= 7;
}

/** @brief Packs a variant's read and write targets (RE::RENDER_TARGET below 256). The menu composites key their copies by it. */
constexpr std::uint16_t InOut(std::int32_t a_in, std::int32_t a_out)
{
	return static_cast<std::uint16_t>(((a_in & 0x7f) << 8) | (a_out & 0xff));
}

/** @brief How often the image-based flares' blur loops may repeat a draw (the engine's count is 3 by default). A later repeat has no pass. */
constexpr std::uint32_t kIblfRepeats = 4;

/** @brief No image-based flare step has this variant: it names target 127, which does not exist. */
constexpr std::uint16_t kIblfNoStep = 0xffff;

/**
* @brief An image-based flare step's variant: its t0 and output targets (RE::RENDER_TARGET below 127), and which repeat of that pair in
* the frame it is. The blur loops draw the same pair several times, and each repeat is its own pass.
*/
constexpr std::uint16_t IblfVariant(std::int32_t a_in, std::int32_t a_out, std::uint32_t a_repeat)
{
	return static_cast<std::uint16_t>((a_out & 0x7f) | ((a_in & 0x7f) << 7) | ((a_repeat & 3u) << 14));
}

std::array<bool, ChainGroup::kCount> ChainGroupsOn(const Frame::PostFields& a_post);

std::string ChainGroupName(std::uint8_t a_group)
{
	using namespace ChainGroup;
	if (a_group >= kPreSlot && a_group < kLdrSlot)
		return fmt::format("slot {} before the tonemap", a_group - kPreSlot);
	if (a_group >= kLdrSlot && a_group < kHdrCopy)
		return fmt::format("slot {} after the tonemap", a_group - kLdrSlot + 3);
	if (a_group >= kBloom && a_group < kTonemap)
		return fmt::format("bloom radius index {}", a_group - kBloom);
	switch (a_group) {
	case kComposite:
		return "the VL composite";
	case kHdrCopy:
		return "the HDR copy";
	case kHdr:
		return "the HDR effect";
	case kHdrLum:
		return "the last luminance level";
	case kHdrAdapt:
		return "the light adaptation";
	case kTonemap:
		return "the tonemap";
	case kTonemapFade:
		return "the tonemap's fade form";
	case kTaa:
		return "TAA";
	case kScreenshot:
		return "the screenshot copy";
	case kUpsample:
		return "the dynamic-resolution upsample";
	case kCapture:
		return "the frozen background's copy";
	case kLensFlare:
		return "the lens flares";
	case kIblf:
		return "the image-based lens flares";
	default:
		return fmt::format("group {}", a_group);
	}
}

/**
* @brief Replays the engine's end-of-frame loop (SE 0x141297410) only to check it against CS's description.
* An active effect the schedule lacks is a mismatch unless the frame's description at Begin has it, which a later build brings.
*/
void CheckEndOfFrame(std::int32_t a_source, std::int32_t a_final)
{
	auto& chain = Chain();
	static REL::Relocation<const bool*> hdrSwitch{ REL::RelocationID(513463, 391301) };
	chain.hdrCopy = !*hdrSwitch.get();
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	if (!manager)
		return;
	constexpr auto segment = Frame::SegmentId::Post;
	// A frozen world's capture frame runs the chain into kMENUBG. CS's chain still ends in the final target and its last pass copies to kMENUBG.
	const auto& frame = Frame::Driver::Current();
	const bool capture = frame.kind == Frame::Kind::FrozenWorld && frame.capture;
	chain.captureFrame = capture && a_final == RE::RENDER_TARGET::kMENUBG;
	const auto expected = capture ? RE::RENDER_TARGET::kMENUBG : RE::RENDER_TARGET::kFRAMEBUFFER;
	if (a_source != RE::RENDER_TARGET::kMAIN || a_final != expected)
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, segment,
			fmt::format("the engine's end-of-frame effects run from target {} into target {}; CS's chain runs from kMAIN into {}", a_source, a_final,
				capture ? "the final target and copies it into kMENUBG (the frozen world's capture frame)" : "the final target"));
	const auto base = reinterpret_cast<std::uintptr_t>(manager);
	const auto member = [&](std::uintptr_t a_offset) { return reinterpret_cast<const std::byte*>(base + a_offset); };
	const auto wanted = ChainGroupsOn(Frame::Driver::Current().post);
	const auto check = [&](std::uint8_t a_group) {
		if (GroupScheduled(a_group) || wanted[a_group])
			return;
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, segment,
			fmt::format("{} runs in the engine's end-of-frame effects, and the frame's description at Begin does not have it", ChainGroupName(a_group)));
	};
	const auto count = (std::min)(*reinterpret_cast<const std::int32_t*>(member(0x40)), 7);
	for (std::int32_t slot = 0; slot <= count; ++slot) {
		auto* effect = slot < static_cast<std::int32_t>(manager->effects.free_idx()) ? manager->effects[static_cast<std::uint16_t>(slot)] : nullptr;
		if (effect && effect->IsActive())
			check(SlotGroup(static_cast<std::uint32_t>(slot), SlotIsLdr(static_cast<std::uint32_t>(slot))));
	}
	check(chain.hdrCopy ? ChainGroup::kHdrCopy : ChainGroup::kHdr);
	static REL::Relocation<const std::int32_t*> dynamicLock{ REL::RelocationID(525027, 411509) };
	static REL::Relocation<const float*> scaleX{ REL::RelocationID(525023, 411505) };
	static REL::Relocation<const float*> scaleY{ REL::RelocationID(525024, 411506) };
	const bool dynres = *dynamicLock.get() == 0 && (*scaleX.get() != 1.0f || *scaleY.get() != 1.0f);
	if (const auto taa = *reinterpret_cast<const std::uintptr_t*>(member(0x1f0)); taa && *reinterpret_cast<const bool*>(taa + 0x18))
		check(ChainGroup::kTaa);
	if (const auto upsample = *reinterpret_cast<const std::uintptr_t*>(member(0x1f8)); upsample && *reinterpret_cast<const bool*>(upsample + 0x08) && dynres)
		check(ChainGroup::kUpsample);
}

constexpr std::array<std::string_view, 2> kAdaptedIds{ "ImageSpace::AdaptedLuminance0", "ImageSpace::AdaptedLuminance1" };

/** @brief Manager +0x128. The effect picks its blur step from it and blends below 1. */
float FullBlurRadius()
{
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	return manager ? *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(manager) + 0x128) : 0.0f;
}

/**
* @brief Which draw this is inside slot 6 or 7, by occurrence of its shader in the slot, and whether the last blur draw blends.
* CS draws the full-screen blur in place, so the engine's copy of its input has nothing to do.
*/
std::uint16_t InnerVariant(std::int32_t a_slot, Effects::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, bool a_blended)
{
	const bool blends = a_slot == 6 && a_blended && a_effect == Effects::ISHDRDownSample4 && a_occurrence == 1;
	// Bit 10 marks an inner draw, so none shares a key with a slot's own draws.
	return static_cast<std::uint16_t>((blends ? 0x8000u : 0u) | (a_occurrence & 7u) << 12 | (a_slot == 7 ? 0x800u : 0u) | 0x400u);
}

/**
* @brief Read from the render target manager, a static at 524970 / 411451, not CommonLib's GetSingleton pointer.
* The renderer's own entry holds no texture in native mode.
*/
std::pair<std::uint32_t, std::uint32_t> FinalSize()
{
	static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> manager{ REL::RelocationID(524970, 411451) };
	const auto& data = manager.get()->renderTargetData[RE::RENDER_TARGET::kFRAMEBUFFER];
	return { data.width, data.height };
}

/** @brief The registration's HDR scene texture while HDR is on, else kFRAMEBUFFER. */
std::int32_t FinalName()
{
	return Chain().hdrTarget ? kHdrSceneOutput : static_cast<std::int32_t>(RE::RENDER_TARGET::kFRAMEBUFFER);
}

/** @brief The engine target the adapted luminance of a_parity replaces for a_levels luminance levels, or -1 for none. */
std::int32_t AdaptTargetOf(std::uint32_t a_levels, std::uint32_t a_parity)
{
	using T = RE::RENDER_TARGET;
	constexpr auto kFirst = static_cast<std::int32_t>(T::kHDR_DOWNSAMPLE1), kLast = static_cast<std::int32_t>(T::kHDR_DOWNSAMPLE13);
	if (a_levels == 0 || kFirst + static_cast<std::int32_t>(a_levels) + 1 > kLast)
		return -1;
	return kFirst + static_cast<std::int32_t>(a_levels) - 1 + 2 * static_cast<std::int32_t>(a_parity);
}

/** @brief The chain's key at a revision's values, or none while the engine is not ready for it. */
template <class Values>
std::optional<ChainKey> ChainKeyOf(const Values& a_values)
{
	const auto shape = ReadyShape(a_values);
	if (!shape || !IS().on)
		return std::nullopt;
	return ChainKey{ a_values.Get(Frame::Vars::DisplaySize()), a_values.Get(Frame::Vars::HdrOutputTarget()), shape->luminanceLevels, shape->effects,
		shape->states };
}

void BuildChainSteps(ChainBuild& a_build);

/** @brief Setup. The kept chain textures, the adapted pair published once, and the three clients declared on their variables. */
void DeclareChainSteps()
{
	using T = RE::RENDER_TARGET;
	auto& chain = Chain();
	// Kept across frames so a frame whose bloom lags reads the last bloom, not an unwritten transient.
	DeclareKept(chain.bloom, T::kHDR_BLOOM, "ImageSpace::Bloom");
	DeclareKept(chain.taaHistory[0], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory0");
	DeclareKept(chain.taaHistory[1], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory1");
	// The pair follows the luminance chain's last level. One texel like the first level while the levels have no pair.
	for (std::uint32_t parity = 0; parity < 2; ++parity)
		DeclareKept(chain.adapt[parity], kAdaptedIds[parity], Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::EngineImageSpace()),
			[parity](const Frame::Inputs& a_in, D3D11_TEXTURE2D_DESC& a_desc) {
				const auto target = AdaptTargetOf(a_in.Get(Frame::Vars::EngineImageSpace()).luminanceLevels, parity);
				if (target >= 0)
					return OwnedDescLike(target, a_desc);
				if (!OwnedDescLike(T::kHDR_DOWNSAMPLE1, a_desc))
					return false;
				a_desc.Width = a_desc.Height = 1;
				return true;
			});
	// Published once, so DCLF's graphs read it by id. A new level count is a new version of each slot and publishes nothing.
	for (const auto& target : chain.adapt)
		if (!PublishedResources::Publish(*target.texture))
			RenderGraph::Fatal("ImageSpace", "the adapted luminance pair could not be published");
	// The ratios are no dependency: the VL composite's render-size viewport is latched at each draw.
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::HdrOutputTarget(), Frame::Vars::EngineImageSpace());
	chain.group.Declare(kChainParts, dependencies, [](const Frame::Inputs& a_in) { return ChainKeyOf(a_in); },
		[](ChainBuild& a_build) { BuildChainSteps(a_build); });
}

/** @brief The chain groups a description has on, from its post fields read at Begin. */
std::array<bool, ChainGroup::kCount> ChainGroupsOn(const Frame::PostFields& a_post)
{
	const auto& post = a_post;
	std::array<bool, ChainGroup::kCount> on{};
	// The engine may start the VL composite after Begin read its fields. Gated, so frames without it skip it.
	on[ChainGroup::kComposite] = true;
	// The engine draws lens flares only for the lights it sees this frame, which Begin cannot know. Gated, so frames without them skip.
	on[ChainGroup::kLensFlare] = true;
	// CS turns the image-based flares off at init, and only the console turns them on, at any frame. Gated, so frames without them skip.
	on[ChainGroup::kIblf] = true;
	for (std::uint32_t slot = 0; slot < 8; ++slot) {
		if (!(post.slots >> slot & 1u))
			continue;
		const bool ldr = post.ldr && slot >= 3 && slot <= 7;
		on[SlotGroup(slot, ldr)] = true;
	}
	on[ChainGroup::kHdrCopy] |= !post.hdr;
	on[ChainGroup::kHdr] |= post.hdr;
	on[post.adapt ? ChainGroup::kHdrAdapt : ChainGroup::kHdrLum] |= post.hdr;
	on[ChainGroup::kBloom + (std::min)(post.bloom, std::uint8_t{ 6 })] |= post.hdr;
	on[post.fade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap] |= post.hdr && !post.effects11;
	on[ChainGroup::kTaa] |= post.taa;
	// The engine's own dynamic-resolution upsample runs when its ratio is not 1 and unlocked. A ratio is no build input
	// (decision 6a), so the upsample stays on. Gated, so frames without it skip it.
	on[ChainGroup::kUpsample] = true;
	// A save asks for the screenshot with at most a frame of notice, after Begin may have read the flag. Gated, so idle frames skip it.
	on[ChainGroup::kScreenshot] = true;
	// The capture frame is only known at its own Begin, too late for a build. Gated, so every other frame skips it.
	on[ChainGroup::kCapture] = true;
	// A background-freezing menu starts its radial blur in the capture frame itself, so slot 5 stays on. Gated.
	on[SlotGroup(5, post.ldr)] = true;
	// The engine turns refraction on mid-frame when it draws a refracting object. Gated.
	on[SlotGroup(1, false)] = true;
	// The engine picks the tonemap form and the last luminance form late, so both stay on and the unused one skips.
	on[ChainGroup::kHdrLum] |= post.hdr;
	on[ChainGroup::kHdrAdapt] |= post.hdr;
	on[ChainGroup::kTonemap] |= post.hdr && !post.effects11;
	on[ChainGroup::kTonemapFade] |= post.hdr && !post.effects11;
	return on;
}

/** @brief A key routes when the frame has TAA, the HDR effect and CS's tonemap. A frame uses the routing its build holds. */
bool ChainRouted(const StepPart& a_part, const Frame::PostFields& a_post, bool a_key)
{
	const bool wanted = a_post.taa && a_post.hdr && !a_post.effects11;
	if (a_key)
		return wanted;
	const auto& part = a_part;
	std::optional<bool> routed, direct;
	for (std::uint32_t entry = 0; entry < part.count && (!routed || !direct); ++entry) {
		if (part.route[entry] == ChainRoute::kAny)
			continue;
		auto& known = part.route[entry] == ChainRoute::kRouted ? routed : direct;
		if (!known)
			known = FrameAnchors::DescribedBuildHasRow(part.rowBase + entry);
	}
	if (!routed || !direct)
		return wanted;
	return *routed && (wanted || !*direct);
}

/** @brief Per-frame variants such as the parities are all on. TAA only gets the history its frame slot's parity selects. */
void ChainRows(const StepPart& a_part, std::array<std::uint64_t, 4>& a_rows)
{
	const auto& frame = Frame::Driver::Current();
	const bool key = FrameAnchors::DescribingKey();
	const auto on = ChainGroupsOn(frame.post);
	const auto& part = a_part;
	const bool routed = ChainRouted(part, frame.post, key);
	for (std::uint32_t entry = 0; entry < part.count; ++entry) {
		const auto group = part.group[entry];
		if (!on[group])
			continue;
		if (part.route[entry] != ChainRoute::kAny && (part.route[entry] == ChainRoute::kRouted) != routed)
			continue;
		// TAA's history pair is CS's. A frame slot writes one history and reads the other.
		if (group == ChainGroup::kTaa && !key && !part.copy[entry] && ((part.keys[entry].variant >> 15) & 1u) != (frame.parity & 1u))
			continue;
		const auto row = part.rowBase + entry;
		if (row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
	}
}

/**
* @brief A registration's transient texture with the engine target's size and RTV format, so a plain texel copy works. Also works
* for the final target, which has only a view in native mode. Fatal when it cannot be made.
*/
void MakeTransientLike(OwnedTarget& a_out, std::int32_t a_like, std::string_view a_name)
{
	const auto index = a_like;
	const auto import = ImportOf(ImportOfName(index));
	if (!import || !import.Width() || !import.Height())
		RenderGraph::Fatal("ImageSpace", fmt::format("the texture {} is made like target {}, which has no import", a_name, index));
	D3D11_RENDER_TARGET_VIEW_DESC view{};
	if (index & kHdrSceneOutput)
		view.Format = Chain().hdrFormat;
	else if (auto* rtv = globals::game::renderer->GetRuntimeData().renderTargets[index].RTV)
		rtv->GetDesc(&view);
	else
		RenderGraph::Fatal("ImageSpace", fmt::format("the texture {} is made like target {}, which has no RTV", a_name, index));
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = import.Width();
	desc.Height = import.Height();
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = view.Format;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	// A texel copy needs texels of the same size.
	const auto bytes = rhi::FormatByteSize(rhi::helpers::ToRHI(view.Format));
	if (!bytes || bytes != rhi::FormatByteSize(import.Format()))
		RenderGraph::Fatal("ImageSpace", fmt::format("{}: target {}'s view format {} and its import's format {} differ in size; no copy between them",
			a_name, index, static_cast<std::uint32_t>(view.Format), static_cast<std::uint32_t>(import.Format())));
	MakeTransient(a_out, desc, a_name);
	a_out.format = RtvFormatOf(index, import);
}

/**
* @brief One texel load per pixel, built once per format on the programs lane. Null while the build's revision has none: the
* chain's build is then dropped, and the next revision makes it again (StepGroup::For). Render thread.
*/
std::shared_ptr<const RasterProgram> CaptureProgram(rhi::Format a_format)
{
	return SpecProgram(CapturePrograms(), a_format);
}

/** @brief The bloom's bright passes and blurs by radius index (3 to 15 pixels). */
constexpr std::array<Effects::ImageSpaceEffectEnum, 7> kBrightPasses{ Effects::ISBrightPassBlur3, Effects::ISBrightPassBlur5, Effects::ISBrightPassBlur7,
	Effects::ISBrightPassBlur9, Effects::ISBrightPassBlur11, Effects::ISBrightPassBlur13, Effects::ISBrightPassBlur15 };
constexpr std::array<Effects::ImageSpaceEffectEnum, 7> kBloomBlurs{ Effects::ISBlur3, Effects::ISBlur5, Effects::ISBlur7, Effects::ISBlur9, Effects::ISBlur11,
	Effects::ISBlur13, Effects::ISBlur15 };

/** @brief The radius index of a bloom bright pass or blur, or -1. a_bright says which of the two it is. */
std::int32_t BloomRadiusOf(Effects::ImageSpaceEffectEnum a_effect, bool& a_bright)
{
	for (std::int32_t r = 0; r < static_cast<std::int32_t>(kBrightPasses.size()); ++r) {
		if (a_effect == kBrightPasses[r] || a_effect == kBloomBlurs[r]) {
			a_bright = a_effect == kBrightPasses[r];
			return r;
		}
	}
	return -1;
}

/** @brief A luminance level's variant. The last of a_levels levels carries a_parity, which picks the adapted target it writes. */
constexpr std::uint16_t LuminanceVariant(std::uint32_t a_level, std::uint32_t a_levels, std::uint32_t a_parity)
{
	return static_cast<std::uint16_t>(a_level + 1 == a_levels ? (a_level | (1u << 8) | (a_parity << 9)) : a_level);
}

/** @brief The engine's target for the last of a_levels luminance levels of a_parity. */
constexpr std::int32_t HdrLastTarget(std::uint32_t a_levels, std::uint32_t a_parity)
{
	return RE::RENDER_TARGET::kHDR_DOWNSAMPLE1 + static_cast<std::int32_t>(a_levels) - 1 + 2 * static_cast<std::int32_t>(a_parity);
}

/** @brief Whether the last level's targets of both parities exist for a_levels levels. */
constexpr bool HdrLevelsFit(std::uint32_t a_levels)
{
	return a_levels > 0 && HdrLastTarget(a_levels, 1) <= RE::RENDER_TARGET::kHDR_DOWNSAMPLE13;
}

/** @brief Where the HDR effect's steps after the downsample write: engine targets in a menu scene, CS's own in the world chain. */
struct HdrStepTargets
{
	std::array<StepTexture, 2> adapted;  // the last level's adapted luminance of each parity
	StepTexture bloomSwap, bloom;
};

/**
* @brief The HDR effect's steps after its downsample, in the engine's order: the luminance levels, the last level and its light
* adaptation of each parity while HdrLevelsFit, then the bloom of each radius. a_add(group, effect, variant, desc) takes each,
* with its ChainGroup.
*/
template <class Add>
void AddHdrSteps(std::uint32_t a_levels, const HdrStepTargets& a_targets, Add&& a_add)
{
	using E = Effects;
	using T = RE::RENDER_TARGET;
	const auto engine = [](std::int32_t a_target) { return StepTexture::Engine(a_target); };
	constexpr StepSampler kPoint{ 0, 0 }, kLinear{ 0, 1 };
	constexpr std::int32_t kDownsample = T::kHDR_DOWNSAMPLE0, kFirstLevel = T::kHDR_DOWNSAMPLE1;
	const auto levelEffect = [](std::uint32_t a_level) { return a_level == 0 ? E::ISHDRDownSample4RGB2Lum : E::ISHDRDownSample16Lum; };
	const auto levelInput = [&](std::uint32_t a_level) { return a_level == 0 ? kDownsample : kFirstLevel + static_cast<std::int32_t>(a_level) - 1; };
	for (std::uint32_t level = 0; level + 1 < a_levels; ++level) {
		StepDesc d{ .name = fmt::format("hdr.luminance{}", level), .output = engine(kFirstLevel + static_cast<std::int32_t>(level)) };
		d.inputs[0] = engine(levelInput(level));
		d.samplers[0] = level == 0 ? kLinear : kPoint;
		a_add(ChainGroup::kHdr, levelEffect(level), static_cast<std::uint16_t>(level), std::move(d));
	}
	if (HdrLevelsFit(a_levels)) {
		const auto lastLevel = a_levels - 1;
		for (std::uint16_t p = 0; p < 2; ++p) {
			const auto variant = LuminanceVariant(lastLevel, a_levels, p);
			StepDesc l{ .name = fmt::format("hdr.luminance{}.p{}", lastLevel, p), .output = a_targets.adapted[p] };
			l.inputs[0] = engine(levelInput(lastLevel));
			l.samplers[0] = lastLevel == 0 ? kLinear : kPoint;
			a_add(ChainGroup::kHdrLum, levelEffect(lastLevel), variant, std::move(l));
			StepDesc a{ .name = fmt::format("hdr.adapt.p{}", p), .output = a_targets.adapted[p] };
			a.inputs[0] = engine(levelInput(lastLevel));
			a.inputs[1] = a_targets.adapted[1 - p];
			a.samplers[0] = kLinear;
			a_add(ChainGroup::kHdrAdapt, E::ISHDRDownSample16LightAdapt, variant, std::move(a));
		}
	}
	for (std::uint8_t r = 0; r < 7; ++r) {
		const auto group = static_cast<std::uint8_t>(ChainGroup::kBloom + r);
		for (std::uint16_t p = 0; p < 2; ++p) {
			StepDesc b{ .name = fmt::format("bloom{}.bright.p{}", r, p), .output = a_targets.bloomSwap };
			b.inputs[0] = engine(kDownsample);
			b.inputs[1] = a_targets.adapted[p];
			a_add(group, kBrightPasses[r], p, std::move(b));
		}
		StepDesc d{ .name = fmt::format("bloom{}.blur", r), .output = a_targets.bloom };
		d.inputs[0] = a_targets.bloomSwap;
		a_add(group, kBloomBlurs[r], 0, std::move(d));
	}
}

/**
* @brief The chain's three clients' registrations for a_build's key. The passes are a fixed list in the engine's order. Their
* declarations register them when the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildChainSteps(ChainBuild& a_build)
{
	auto& state = IS();
	auto& chain = Chain();
	const auto& a_key = a_build.key;
	const auto& table = a_build.table;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	chain.width = main.Width;
	chain.height = main.Height;
	std::tie(chain.finalWidth, chain.finalHeight) = FinalSize();
	// The key's HDR target, not the live one, so the registration matches the revision that switches it in.
	chain.hdrTarget = a_key.hdr.id ? reinterpret_cast<ID3D11Texture2D*>(a_key.hdr.id) : nullptr;
	chain.hdrFormat = static_cast<DXGI_FORMAT>(a_key.hdr.format);
	chain.final = FinalName();
	chain.levels = a_key.levels;
	a_build.levels = a_key.levels;
	using T = RE::RENDER_TARGET;
	if (AdaptTargetOf(chain.levels, 1) < 0) {
		// The kept pair is one texel then, and no step could write it.
		logger::error("[ORG] ImageSpace: the luminance chain has {} levels, which the adapted pair cannot follow; the post chain has no CS passes", chain.levels);
		return;
	}
	for (std::uint32_t parity = 0; parity < 2; ++parity)
		KeptExtentLike(chain.adapt[parity], AdaptTargetOf(chain.levels, parity));
	KeptExtentLike(chain.bloom, T::kHDR_BLOOM);
	for (auto& history : chain.taaHistory)
		KeptExtentLike(history, T::kTEMPORAL_AA_ACCUMULATION_1);
	MakeTransient(chain.bloomSwap, T::kHDR_BLURSWAP, "ImageSpace::BloomSwap");
	MakeTransient(chain.dofDown, T::kLDR_DOWNSAMPLE0, "ImageSpace::DofDownsample");
	MakeTransient(chain.dofSwap, T::kLDR_BLURSWAP, "ImageSpace::DofBlurSwap");
	MakeTransient(chain.getHitA, T::kGETHIT_BUFFER, "ImageSpace::GetHitA");
	MakeTransient(chain.getHitB, T::kGETHIT_BUFFER, "ImageSpace::GetHitB");
	MakeTransient(chain.getHitSwap, T::kGETHIT_BLURSWAP, "ImageSpace::GetHitSwap");
	MakeTransient(chain.fullBlurDown, T::kHDR_BLURSWAP, "ImageSpace::FullBlurDown");
	MakeTransient(chain.fullBlurSwap, T::kHDR_DOWNSAMPLE0, "ImageSpace::FullBlurSwap");
	MakeTransientLike(chain.hdrScratch, T::kMAIN, "ImageSpace::HdrScratch");
	MakeTransientLike(chain.ldrScratch, chain.final, "ImageSpace::LdrScratch");

	using E = Effects;
	// effects[9] to [15] draw ISBlurN with the HDR flag at +0x95, else ISNonHDRBlurN. None is a bright pass.
	if (auto* manager = RE::ImageSpaceManager::GetSingleton())
		for (std::uint32_t r = 0; r < 7; ++r) {
			const auto index = 9 + r;
			auto* blur = index < manager->effects.free_idx() ? manager->effects[static_cast<std::uint16_t>(index)] : nullptr;
			chain.blurHdr[r] = !blur || *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(blur) + 0x95);
			if (blur && *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(blur) + 0x94))
				logger::warn("[ORG] ImageSpace: the blur effect of radius {} is a bright pass; the full-screen blur and get-hit passes assume it is not", 3 + 2 * r);
		}
	const auto engine = [](std::int32_t a_target, std::uint32_t a_mip = AnchorPasses::kAllMips) {
		return StepTexture::Engine(a_mip == AnchorPasses::kAllMips ? a_target : WithMip(a_target, a_mip));
	};
	const auto owned = [](const OwnedTarget& a_target) { return StepTexture::Owned(a_target); };
	const auto mainDepth = StepTexture::Engine(kDepthInput | 0);
	constexpr StepSampler kPoint{ 0, 0 }, kLinear{ 0, 1 }, kWrapPoint{ 3, 0 };
	const std::int32_t kFinal = chain.final;
	const auto image = [&](bool a_ldr) { return engine(a_ldr ? kFinal : T::kMAIN); };
	const auto scratch = [&](bool a_ldr) { return owned(a_ldr ? chain.ldrScratch : chain.hdrScratch); };

	struct Pending
	{
		std::uint8_t part = 0;
		std::uint8_t group = 0;
		StepKey key;
		StepDesc desc;
		bool final = false;  // it writes the scratch target: the next copy back of its group follows it
		bool copy = false;   // a copy back of the scratch target into its image (desc.name only)
		bool ldr = false;    // a copy back's side: the final target, else kMAIN
		bool capture = false;  // the frozen background's copy: the final target into kMENUBG (desc.name only)
		bool through = false;  // the pass-through copy of a routed build (ChainRoute)
		std::uint8_t route = ChainRoute::kAny;
	};
	std::vector<Pending> steps;
	const auto add = [&](std::uint8_t a_part, std::uint8_t a_group, WorldScope a_scope, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc,
						 bool a_final = false) {
		a_desc.effect = a_effect;
		steps.push_back({ .part = a_part, .group = a_group, .key = { a_scope, a_effect, a_variant }, .desc = std::move(a_desc), .final = a_final });
	};
	const auto copyBack = [&](std::uint8_t a_part, std::uint8_t a_group, bool a_ldr, std::string a_name) {
		Pending pending{ .part = a_part, .group = a_group, .key = { WorldScope::None, E::ISCopy, 0 }, .copy = true, .ldr = a_ldr };
		pending.desc.name = std::move(a_name);
		steps.push_back(std::move(pending));
	};

	// Lens flares (SE 100274), which RenderWorldPostProcess draws before the composites. The visibility pass writes a texel per
	// light. One sprite pass draws all the frame's sprites additively into kMAIN, each from its own latch record (StageLensFlareSprite).
	{
		// The engine draws it with write mode 5 (seen at the draw).
		StepDesc v{ .name = "lensflare.visibility", .output = engine(T::kLENSFLAREVIS), .blend = PackBlend(0, 0, 5, 0) };
		v.inputs[0] = mainDepth;
		v.samplers[0] = kPoint;
		add(0, ChainGroup::kLensFlare, WorldScope::LensFlare, E::ISLensFlareVisibility, 0, std::move(v));
		// The sprite texture is clamped and trilinear (BSLensFlare's constructor). t1 is read by Load, with no sampler.
		StepDesc s{ .name = "lensflare.sprites", .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 1, 0), .latched = 0b01 };
		s.inputs[1] = engine(T::kLENSFLAREVIS);
		s.samplers[0] = { 0, 2 };
		add(0, ChainGroup::kLensFlare, WorldScope::LensFlare, E::ISLensFlare, 0, std::move(s));
	}

	// The image-based lens flares (AE 0x1414d21b0), first in RenderPreTonemapComposites. CS turns them off at init (Hooks.cpp), so only
	// the console's ToggleIBLF reaches them. Every step reads and writes the engine's targets, as the engine does. Each downsample
	// blends in last frame's result from the other target of its ping-pong pair (by frame counter), moved by the motion vectors.
	// The sum ends in kHDR_BLURSWAP, which the lens flare forms of the composite read. All viewports are the whole target.
	{
		constexpr std::uint32_t kAdd = PackBlend(5, 0, 2, 0);
		const auto ping = [](std::int32_t a_first, std::uint32_t a_parity) { return a_first + static_cast<std::int32_t>(a_parity); };
		// a_history is the downsample's other ping-pong target, or -1 for a step without it. The engine samples t0 linear with a border.
		const auto iblf = [&](E::ImageSpaceEffectEnum a_effect, std::string a_name, std::int32_t a_in, std::int32_t a_out, std::uint32_t a_repeat,
							  std::int32_t a_history = -1, StepSampler a_sampler = StepSampler{ 4, 1 }, std::uint32_t a_blend = PackBlend(0, 0, 2, 0)) {
			StepDesc d{ .name = std::move(a_name), .output = engine(a_out), .blend = a_blend };
			d.inputs[0] = engine(a_in);
			d.samplers[0] = a_sampler;
			if (a_history >= 0) {
				d.inputs[1] = engine(a_history);
				d.inputs[2] = engine(T::kMOTION_VECTOR);
				d.samplers[1] = kLinear;
				d.samplers[2] = kLinear;
			}
			add(0, ChainGroup::kIblf, WorldScope::Iblf, a_effect, IblfVariant(a_in, a_out, a_repeat), std::move(d));
		};
		constexpr std::int32_t kLights = T::kIBLENSFLARES_LIGHTS_FILTER, k4x4 = T::kIBLENSFLARES_DOWNSAMPLE_4X_4X_PING,
							   k16x4 = T::kIBLENSFLARES_DOWNSAMPLE_16X_4Y_PING, k16x4Blur = T::kIBLENSFLARES_DOWNSAMPLE_16X_4Y_BLUR,
							   k16x4Swap = T::kIBLENSFLARES_DOWNSAMPLE_16X_4Y_BLUR_SWAP, k32x4 = T::kIBLENSFLARES_DOWNSAMPLE_32X_4Y_PING,
							   k32x4Blur = T::kIBLENSFLARES_DOWNSAMPLE_32X_4Y_BLUR, k32x4Swap = T::kIBLENSFLARES_DOWNSAMPLE_32X_4Y_BLUR_SWAP,
							   k16x16 = T::kIBLENSFLARES_DOWNSAMPLE_16X_16X_PING, k16x16Swap = T::kIBLENSFLARES_DOWNSAMPLE_16X_16X_SWAP;
		// The first downsample, of kMAIN. The compute form runs while bEnableDownsampleComputeShader is on (the default), with a linear
		// clamp sampler at s0. The raster form samples with a border.
		for (std::uint32_t p = 0; p < 2; ++p) {
			iblf(E::ISDownsample, fmt::format("iblf.downsample0.p{}", p), T::kMAIN, ping(k4x4, p), 0, ping(k4x4, 1 - p));
			iblf(E::ISDownsampleCS, fmt::format("iblf.downsample0.cs.p{}", p), T::kMAIN, ping(k4x4, p), 0, ping(k4x4, 1 - p), kLinear);
		}
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISIBLensFlares, fmt::format("iblf.flares0.p{}", p), ping(k4x4, p), kLights, 0);
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISDownsample, fmt::format("iblf.downsample1.p{}", p), kLights, ping(k16x4, p), 0, ping(k16x4, 1 - p));
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISIBLensFlares, fmt::format("iblf.flares1.p{}", p), ping(k16x4, p), k16x4Blur, 0);
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISDownsample, fmt::format("iblf.downsample2.p{}", p), k16x4Blur, ping(k32x4, p), 0, ping(k32x4, 1 - p));
		// The lights' blur goes through two of the HDR effect's targets, which the HDR effect writes again later in the frame.
		iblf(E::ISBlur, "iblf.blur.lights.h", kLights, T::kHDR_DOWNSAMPLE0, 0);
		iblf(E::ISBlur, "iblf.blur.lights.v", T::kHDR_DOWNSAMPLE0, T::kHDR_BLURSWAP, 0);
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISDownsample, fmt::format("iblf.downsample3.p{}", p), T::kHDR_BLURSWAP, ping(k16x16, p), 0, ping(k16x16, 1 - p));
		for (std::uint32_t p = 0; p < 2; ++p)
			iblf(E::ISBlur, fmt::format("iblf.blur.16x16.h.p{}", p), ping(k16x16, p), k16x16Swap, 0);
		iblf(E::ISBlur, "iblf.blur.16x16.v", k16x16Swap, T::kHDR_DOWNSAMPLE1, 0);
		// Two blur loops of the engine's count. The second loop's first blur reads the 32x4 downsample, its later ones the swap.
		for (std::uint32_t i = 0; i < kIblfRepeats; ++i) {
			iblf(E::ISBlur, fmt::format("iblf.blur.16x4.h{}", i), k16x4Blur, k16x4Swap, i);
			iblf(E::ISBlur, fmt::format("iblf.blur.16x4.v{}", i), k16x4Swap, k16x4Blur, i);
		}
		for (std::uint32_t i = 0; i < kIblfRepeats; ++i) {
			if (i == 0)
				for (std::uint32_t p = 0; p < 2; ++p)
					iblf(E::ISBlur, fmt::format("iblf.blur.32x4.v0.p{}", p), ping(k32x4, p), k32x4Blur, 0);
			else
				iblf(E::ISBlur, fmt::format("iblf.blur.32x4.v{}", i), k32x4Swap, k32x4Blur, i - 1);
			iblf(E::ISBlur, fmt::format("iblf.blur.32x4.h{}", i), k32x4Blur, k32x4Swap, i);
		}
		// The sum: three additive copies (ImageSpaceManager::RenderEffect with effect 35), linear at s0.
		iblf(E::ISCopyDynamicFetchDisabled, "iblf.sum.16x16", T::kHDR_DOWNSAMPLE1, T::kHDR_BLURSWAP, 0, -1, kLinear, kAdd);
		iblf(E::ISCopyDynamicFetchDisabled, "iblf.sum.32x4", k32x4Swap, k16x4Blur, 0, -1, kLinear, kAdd);
		iblf(E::ISCopyDynamicFetchDisabled, "iblf.sum.16x4", k16x4Blur, T::kHDR_BLURSWAP, 0, -1, kLinear, kAdd);
	}

	// The composite at the render size (AE 0x1414d45f0), additive into kMAIN: t0 the VL of the frame's parity, t1 the image-based
	// flares' sum. The engine picks the form from what is on: VL alone, the flares alone, or both.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("composite.vl.p{}", p), .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[0] = engine(T::kIMAGESPACE_VOLUMETRIC_LIGHTING + p);
		d.samplers[0] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeVolumetricLighting, p, std::move(d));
	}
	{
		StepDesc d{ .name = "composite.lf", .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[1] = engine(T::kHDR_BLURSWAP);
		d.samplers[1] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeLensFlare, 0, std::move(d));
	}
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("composite.lfvl.p{}", p), .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[0] = engine(T::kIMAGESPACE_VOLUMETRIC_LIGHTING + p);
		d.inputs[1] = engine(T::kHDR_BLURSWAP);
		d.samplers[0] = kLinear;
		d.samplers[1] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeLensFlareVolumetricLighting, p, std::move(d));
	}

	const auto single = [&](std::uint8_t a_part, std::uint32_t a_slot, bool a_ldr, E::ImageSpaceEffectEnum a_effect, const char* a_name, StepSampler a_sampler,
							StepTexture a_second = {}, StepSampler a_secondSampler = {}) {
		StepDesc d{ .name = a_name, .output = scratch(a_ldr) };
		d.inputs[0] = image(a_ldr);
		d.samplers[0] = a_sampler;
		if (a_second) {
			d.inputs[1] = a_second;
			d.samplers[1] = a_secondSampler;
		}
		add(a_part, SlotGroup(a_slot, a_ldr), a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, 0, std::move(d), true);
	};
	const auto slotCopy = [&](std::uint8_t a_part, std::uint32_t a_slot, bool a_ldr, const char* a_name) {
		copyBack(a_part, SlotGroup(a_slot, a_ldr), a_ldr, fmt::format("{}.copyback", a_name));
	};
	// Depth of field with its compute blur. The masked composites need a mask CS lacks, and the raster blur has no CS pass.
	const auto dof = [&](std::uint8_t a_part, bool a_ldr) {
		const auto scope = a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
		const auto group = SlotGroup(3, a_ldr);
		{
			StepDesc d{ .name = "dof.downsample", .output = owned(chain.dofDown) };
			d.inputs[0] = image(a_ldr);
			d.samplers[0] = kLinear;
			add(a_part, group, scope, E::ISHDRDownSample4, 0, std::move(d));
		}
		{
			StepDesc h{ .name = "dof.blur.h", .output = owned(chain.dofSwap) };
			h.inputs[0] = owned(chain.dofDown);
			h.inputs[1] = mainDepth;
			add(a_part, group, scope, E::ISDepthOfFieldBlurHCS, 0, std::move(h));
			StepDesc v{ .name = "dof.blur.v", .output = owned(chain.dofDown) };
			v.inputs[0] = owned(chain.dofSwap);
			v.inputs[1] = mainDepth;
			add(a_part, group, scope, E::ISDepthOfFieldBlurVCS, 0, std::move(v));
		}
		constexpr std::array<E::ImageSpaceEffectEnum, 4> kComposites{ E::ISDepthOfField, E::ISDepthOfFieldFogged, E::ISDistantBlur, E::ISDistantBlurFogged };
		for (const auto effect : kComposites) {
			StepDesc d{ .name = fmt::format("dof.composite{}", static_cast<std::uint32_t>(effect)), .output = scratch(a_ldr) };
			d.inputs[0] = image(a_ldr);
			d.inputs[1] = owned(chain.dofDown);
			d.inputs[2] = mainDepth;
			d.inputs[3] = engine(T::kSAO_CAMERAZ, 10);  // the camera depth's mip 10, as the engine binds it
			d.samplers[1] = kLinear;
			add(a_part, group, scope, effect, 0, std::move(d), true);
		}
		copyBack(a_part, group, a_ldr, "dof.copyback");
	};
	const auto radial = [&](std::uint8_t a_part, bool a_ldr) {
		single(a_part, 5, a_ldr, E::ISRadialBlur, "radialblur", kPoint);
		single(a_part, 5, a_ldr, E::ISRadialBlurMedium, "radialblur.medium", kPoint);
		single(a_part, 5, a_ldr, E::ISRadialBlurHigh, "radialblur.high", kPoint);
		slotCopy(a_part, 5, a_ldr, "radialblur");
	};
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlurHdr{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlurLdr{ E::ISNonHDRBlur3, E::ISNonHDRBlur5, E::ISNonHDRBlur7, E::ISNonHDRBlur9, E::ISNonHDRBlur11,
		E::ISNonHDRBlur13, E::ISNonHDRBlur15 };
	const auto blurOf = [&](std::uint32_t a_radiusIndex) { return chain.blurHdr[a_radiusIndex] ? kBlurHdr[a_radiusIndex] : kBlurLdr[a_radiusIndex]; };
	const auto inner = [&](std::uint8_t a_part, std::int32_t a_slot, bool a_ldr, E::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, bool a_blended,
						   StepDesc a_desc, bool a_final = false) {
		const auto variant = InnerVariant(a_slot, a_effect, a_occurrence, a_blended);
		add(a_part, SlotGroup(static_cast<std::uint32_t>(a_slot), a_ldr), a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, variant, std::move(a_desc),
			a_final);
	};
	const auto getHit = [&](std::uint8_t a_part, bool a_ldr) {
		{
			StepDesc d{ .name = "gethit.copy", .output = owned(chain.getHitA) };
			d.inputs[0] = image(a_ldr);
			inner(a_part, 7, a_ldr, E::ISCopy, 0, false, std::move(d));
		}
		const auto blur = blurOf(1);
		const std::array<std::pair<const OwnedTarget*, const OwnedTarget*>, 4> kBlurs{ { { &chain.getHitA, &chain.getHitB }, { &chain.getHitB, &chain.getHitSwap },
			{ &chain.getHitSwap, &chain.getHitB }, { &chain.getHitB, &chain.getHitA } } };
		for (std::uint32_t k = 0; k < kBlurs.size(); ++k) {
			StepDesc d{ .name = fmt::format("gethit.blur{}", k), .output = owned(*kBlurs[k].second) };
			d.inputs[0] = owned(*kBlurs[k].first);
			inner(a_part, 7, a_ldr, blur, k, false, std::move(d));
		}
		{
			StepDesc d{ .name = "gethit.doublevision", .output = scratch(a_ldr) };
			d.inputs[0] = image(a_ldr);
			d.inputs[1] = owned(chain.getHitA);
			inner(a_part, 7, a_ldr, E::ISDoubleVision, 0, false, std::move(d), true);
		}
		slotCopy(a_part, 7, a_ldr, "gethit");
	};
	// In place. The engine's copy of the input before the blend is the image itself here, so no pass.
	const auto fullBlur = [&](std::uint8_t a_part, bool a_ldr) {
		{
			StepDesc d{ .name = "fullblur.down", .output = owned(chain.fullBlurDown) };
			d.inputs[0] = image(a_ldr);
			d.samplers[0] = kLinear;
			inner(a_part, 6, a_ldr, E::ISHDRDownSample4, 0, false, std::move(d));
		}
		for (std::uint32_t r = 0; r < 7; ++r) {
			StepDesc h{ .name = fmt::format("fullblur.blur{}.h", r), .output = owned(chain.fullBlurSwap) };
			h.inputs[0] = owned(chain.fullBlurDown);
			inner(a_part, 6, a_ldr, blurOf(r), 0, false, std::move(h));
			StepDesc v{ .name = fmt::format("fullblur.blur{}.v", r), .output = owned(chain.fullBlurDown) };
			v.inputs[0] = owned(chain.fullBlurSwap);
			inner(a_part, 6, a_ldr, blurOf(r), 1, false, std::move(v));
		}
		for (const bool blended : { false, true }) {
			// Below radius 1 the engine alpha blends (mode 1) over the copy.
			StepDesc d{ .name = fmt::format("fullblur.up{}", blended ? ".blend" : ""), .output = image(a_ldr), .blend = PackBlend(blended ? 1 : 0, 0, 2, 0) };
			d.inputs[0] = owned(chain.fullBlurDown);
			d.samplers[0] = kLinear;
			inner(a_part, 6, a_ldr, E::ISHDRDownSample4, 1, blended, std::move(d));
		}
	};

	// Slot 4 has no effect.
	single(1, 0, false, E::ISWorldMap, "worldmap", kLinear, mainDepth, kPoint);
	single(1, 0, false, E::ISWorldMapNoSkyBlur, "worldmap.nosky", kLinear, mainDepth, kPoint);
	slotCopy(1, 0, false, "worldmap");
	single(1, 1, false, E::ISRefraction, "refraction", kLinear, engine(T::kREFRACTION_NORMALS), kPoint);
	slotCopy(1, 1, false, "refraction");
	single(1, 2, false, E::ISFXAA, "fxaa", kLinear);
	slotCopy(1, 2, false, "fxaa");
	dof(1, false);
	radial(1, false);
	fullBlur(1, false);
	getHit(1, false);

	// Used when the HDR switch 513463 / 391301 is off.
	{
		StepDesc d{ .name = "hdr.copy", .output = engine(kFinal) };
		d.inputs[0] = engine(T::kMAIN);
		add(1, ChainGroup::kHdrCopy, WorldScope::HdrCopy, E::ISCopy, 0, std::move(d));
	}

	// The HDR effect: downsample, luminance levels, last level, then the bloom of the frame's radius.
	// The luminance targets stay the engine's because DCLF's character light reads them.
	{
		StepDesc d{ .name = "hdr.downsample", .output = engine(T::kHDR_DOWNSAMPLE0) };
		d.inputs[0] = engine(T::kMAIN);
		d.samplers[0] = kLinear;
		add(1, ChainGroup::kHdr, WorldScope::Hdr, E::ISHDRDownSample4, 0, std::move(d));
	}
	const auto levels = chain.levels;
	AddHdrSteps(levels, { .adapted = { owned(chain.adapt[0]), owned(chain.adapt[1]) }, .bloomSwap = owned(chain.bloomSwap), .bloom = owned(chain.bloom) },
		[&](std::uint8_t a_group, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
			add(1, a_group, WorldScope::Hdr, a_effect, a_variant, std::move(a_desc));
		});

	// t0 bloom, t1 kMAIN, t2 adapted luminance of the frame's parity.
	for (const bool routed : { false, true })
		for (const bool fade : { false, true })
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc d{ .name = fmt::format("tonemap{}.p{}{}", fade ? ".fade" : "", p, routed ? ".taa" : ""), .output = routed ? scratch(true) : engine(kFinal) };
				d.inputs[0] = owned(chain.bloom);
				d.inputs[1] = engine(T::kMAIN);
				d.inputs[2] = owned(chain.adapt[p]);
				d.samplers[0] = kLinear;
				d.samplers[2] = kLinear;
				add(2, fade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap, WorldScope::Tonemap, fade ? E::ISHDRTonemapBlendCinematicFade : E::ISHDRTonemapBlendCinematic,
					static_cast<std::uint16_t>((p << 15) | (routed ? ChainRoute::kTonemapBit : 0)), std::move(d));
				steps.back().route = routed ? ChainRoute::kRouted : ChainRoute::kDirect;
			}
	// CS's history pair replaces kTEMPORAL_AA_ACCUMULATION_1/2. t2 motion vectors, t3 depth, t4 TAA mask, t5 the frame's normals target.
	for (const bool routed : { false, true }) {
		for (std::uint16_t h = 0; h < 2; ++h)
			for (std::uint16_t n = 0; n < 2; ++n) {
				StepDesc d{ .name = fmt::format("taa.h{}.n{}{}", h, n, routed ? ".routed" : ""), .output = routed ? engine(kFinal) : scratch(true),
					.output1 = owned(chain.taaHistory[h]) };
				d.inputs[0] = routed ? scratch(true) : engine(kFinal);
				d.inputs[1] = owned(chain.taaHistory[1 - h]);
				d.inputs[2] = engine(T::kMOTION_VECTOR);
				d.inputs[3] = mainDepth;
				d.inputs[4] = engine(T::kTEMPORAL_AA_MASK);
				d.inputs[5] = engine(n ? T::kNORMAL_TAAMASK_SSRMASK_SWAP : T::kNORMAL_TAAMASK_SSRMASK);
				d.samplers[1] = kLinear;
				d.samplers[4] = kWrapPoint;
				d.samplers[5] = kWrapPoint;
				add(2, ChainGroup::kTaa, WorldScope::Taa, E::ISTemporalAA, static_cast<std::uint16_t>((h << 15) | (n << 14) | (routed ? ChainRoute::kTaaBit : 0)),
					std::move(d), !routed);
				steps.back().route = routed ? ChainRoute::kRouted : ChainRoute::kDirect;
			}
		if (!routed) {
			copyBack(2, ChainGroup::kTaa, true, "taa.copyback");
			steps.back().route = ChainRoute::kDirect;
		}
	}
	{
		// Used in a routed build's frame whose tonemap wrote the scratch but drew no TAA.
		Pending pending{ .part = 2, .group = ChainGroup::kTonemap, .key = { WorldScope::None, E::ISCopy, 2 }, .copy = true, .ldr = true, .through = true,
			.route = ChainRoute::kRouted };
		pending.desc.name = "taa.through";
		steps.push_back(std::move(pending));
	}
	// Manager +0x211. Copies the final target as TAA left it, before the post-tonemap slots, as the engine does.
	{
		StepDesc d{ .name = "screenshot", .output = engine(T::kSAVE_GAME_SCREENSHOT) };
		d.inputs[0] = engine(kFinal);
		add(2, ChainGroup::kScreenshot, WorldScope::Screenshot, E::ISCopy, 0, std::move(d));
	}
	dof(2, true);
	radial(2, true);
	fullBlur(2, true);
	getHit(2, true);
	{
		StepDesc d{ .name = "upsample", .output = scratch(true) };
		d.inputs[0] = engine(kFinal);
		d.samplers[0] = kLinear;
		add(2, ChainGroup::kUpsample, WorldScope::Upsample, E::ISUpsampleDynamicResolution, 0, std::move(d), true);
		copyBack(2, ChainGroup::kUpsample, true, "upsample.copyback");
	}
	// Last, since the image is final here. Opened at RenderEndOfFrameEffects' return in a capture frame.
	{
		Pending pending{ .part = 2, .group = ChainGroup::kCapture, .key = { WorldScope::None, E::ISCopy, 1 }, .capture = true };
		pending.desc.name = "capture.menubg";
		steps.push_back(std::move(pending));
	}

	std::uint32_t rowBase = 0;
	for (std::uint8_t p = 0; p < 3; ++p) {
		const auto partIndex = static_cast<std::size_t>(kChainParts[p]);
		auto& client = state.post[partIndex];
		auto& part = table->parts[p];
		client.batch.clear();
		part.rowBase = rowBase;
		std::vector<FrameAnchors::PassDesc> passes;
		// A copy pass made here, at its entry: always made.
		const auto addCopy = [&](const Pending& a_step, AnchorPasses::Builder& a_builder) -> std::optional<std::uint32_t> {
			auto made = a_builder.Take();
			if (a_builder.Failed() || made.size() != 1)
				return std::nullopt;
			const auto entry = AddStep(*table, p, a_step.key, nullptr, false);
			part.group[entry] = a_step.group;
			part.copy[entry] = true;
			part.route[entry] = a_step.route;
			part.made[entry].store(true, std::memory_order_release);
			auto pass = std::move(made.front());
			pass.order = static_cast<std::int32_t>(entry);
			pass.row = static_cast<std::int16_t>(part.RowOf(entry));
			passes.push_back(std::move(pass));
			return entry;
		};
		std::vector<std::uint32_t> finals;
		for (auto& step : steps) {
			if (step.part != p)
				continue;
			const auto entry = part.count;
			if (entry >= kGateEntries || part.RowOf(entry) >= 256) {
				logger::error("[ORG] ImageSpace: the post chain has more steps than a stage's rows hold; {} has no pass", step.desc.name);
				continue;
			}
			if (step.capture) {
				// A texel load per pixel, since the formats may differ. The session has no copy if the sizes differ.
				const auto source = ImportOf(ImportOfName(kFinal));
				const auto target = ImportOf(ImportOfName(T::kMENUBG));
				const auto program = source && target ? CaptureProgram(RtvFormatOf(T::kMENUBG, target)) : nullptr;
				// Not in this revision yet: this build is dropped and made again by the next revision (StepGroup::For).
				if (source && target && !program)
					continue;
				if (!program || source.Width() != target.Width() || source.Height() != target.Height()) {
					logger::error("[ORG] ImageSpace: the final target and kMENUBG differ in size, or the copy's program could not be built; a frozen "
								  "world's capture frame has no frozen background");
					continue;
				}
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, static_cast<std::int32_t>(entry), client.latch, client.gate);
				builder.Add(AnchorPasses::Draw{ .name = step.desc.name,
					.program = program,
					.bindings = { AnchorPasses::Bind::Srv(source) },
					.target = target,
					// A zero viewport is the whole target of the version bound at preparation.
					.gate = entry });
				const auto added = addCopy(step, builder);
				if (!added) {
					logger::error("[ORG] ImageSpace: post step {} has no pass ({}); a frozen world's capture frame has no frozen background", step.desc.name,
						builder.Failure());
					continue;
				}
				a_build.captureEntry = static_cast<std::uint16_t>(*added);
				continue;
			}
			if (step.copy) {
				// The draw that writes the scratch opens this copy's gate entry.
				const auto& from = step.ldr ? chain.ldrScratch : chain.hdrScratch;
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, static_cast<std::int32_t>(entry), client.latch, client.gate);
				builder.Add(AnchorPasses::Copy{ .name = step.desc.name,
					.source = AnchorPasses::TextureOf(*from.texture),
					.target = ImportOf(ImportOfName(step.ldr ? kFinal : T::kMAIN)),
					// Sized at preparation: the smaller of the versions bound. On a resize the image's new version
					// can be adopted while the scratch is still at the old size.
					.gate = entry });
				const auto added = addCopy(step, builder);
				if (!added) {
					logger::error("[ORG] ImageSpace: post step {} has no pass ({}); the effects before it draw nothing", step.desc.name, builder.Failure());
					finals.clear();
					continue;
				}
				if (step.through)
					a_build.throughEntry = static_cast<std::uint16_t>(*added);
				else
					for (const auto writer : finals)
						if (part.group[writer] == step.group)
							part.copyBack[writer] = static_cast<std::uint16_t>(*added);
				finals.clear();
				continue;
			}
			std::string error;
			auto spec = MakeStepSpec(step.desc, entry, error);
			if (!spec) {
				logger::info("[ORG] ImageSpace: post step {} has no pass ({})", step.desc.name, error);
				continue;
			}
			if (table->entries.contains(step.key))
				logger::error("[ORG] ImageSpace: post step {} has the key of an earlier step (variant {:#x}); the hook finds only the earlier one", step.desc.name,
					step.key.variant);
			AddStep(*table, p, step.key, spec);
			part.group[entry] = step.group;
			part.route[entry] = step.route;
			if (step.final)
				finals.push_back(entry);
			passes.push_back(StepPassDesc(table, p, entry, client, spec));
		}
		rowBase += part.count;
		auto desc = PartClientDesc(partIndex, std::move(passes));
		if (p == 1)
			// Only the targets kept across frames are registered. A registered resource gets a backing in every graph that holds the client.
			desc.registerResources = [](org::RenderGraph& a_graph) {
				const auto& current = Chain();
				for (const auto* target : { &current.bloom, &current.taaHistory[0], &current.taaHistory[1], &current.adapt[0], &current.adapt[1] })
					if (*target)
						target->texture->Register(a_graph);
			};
		// Rows read the registration's own table, never a newer one's.
		desc.rows = [rows = std::shared_ptr<const StepTable<3>>(table), p](std::array<std::uint64_t, 4>& a_rows) { ChainRows(rows->parts[p], a_rows); };
		desc.repeats = true;
		desc.segmentOnly = true;
		// An empty client is no client. Its declaration then registers none.
		if (!desc.passes.empty())
			a_build.descs[p] = std::move(desc);
	}
	logger::info("[ORG] ImageSpace: the post chain is CS passes: {} composite, {} pre-tonemap and {} post-tonemap passes ({} luminance levels), for {}x{} "
				 "into {}x{} (render-size viewports latched per draw), built for the revision starting now",
		table->parts[0].count, table->parts[1].count, table->parts[2].count, chain.levels, chain.width, chain.height, chain.finalWidth, chain.finalHeight);
}

bool ChainStepNow()
{
	const auto& chain = Chain();
	return IS().on && chain.group.Registered() && (chain.inComposites || chain.inEndOfFrame);
}

enum class ChainDraw : std::uint8_t
{
	Unknown,
	Step,
	Nothing,
};

/**
* @brief An image-based flare draw's variant (IblfVariant) from the targets the engine bound for it: t0 and the render target, or
* u0 for the compute downsample. Counts the frame's repeats of the pair. kIblfNoStep for a target no step names.
*/
std::uint16_t IblfVariantNow(Effects::ImageSpaceEffectEnum a_effect)
{
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	const bool compute = kEffects[static_cast<std::size_t>(EffectIndexOf(a_effect))].compute;
	const auto input = InputOfView(compute ? shadow.CSTexture[0] : shadow.PSTexture[0]);
	const auto output = compute ? TargetOfUav(shadow.CSUAV[0]) : static_cast<std::int32_t>(shadow.renderTargets[0]);
	const auto plain = [](std::int32_t a_name) { return a_name >= 0 && a_name < 0x7f; };
	if (!plain(input) || !plain(output))
		return kIblfNoStep;
	const auto repeat = Chain().iblfDraws[{ static_cast<std::int32_t>(a_effect), input, output }]++;
	return repeat < kIblfRepeats ? IblfVariant(input, output, repeat) : kIblfNoStep;
}

/** @brief A chain draw's key from the engine's parity globals and the frame state. Only the image-based flares' variants name targets. */
ChainDraw ChainKeyOf(Effects::ImageSpaceEffectEnum a_effect, StepKey& a_key, std::uint8_t& a_group)
{
	using E = Effects;
	auto& chain = Chain();
	if (chain.inComposites && !chain.inEndOfFrame) {
		switch (a_effect) {
		case E::ISCompositeVolumetricLighting:
		case E::ISCompositeLensFlareVolumetricLighting:
			a_group = ChainGroup::kComposite;
			a_key = { WorldScope::Composite, a_effect, static_cast<std::uint16_t>(Frame::PostChain::VolumetricParity()) };
			return ChainDraw::Step;
		case E::ISCompositeLensFlare:
			a_group = ChainGroup::kComposite;
			a_key = { WorldScope::Composite, a_effect, 0 };
			return ChainDraw::Step;
		case E::ISDownsample:
		case E::ISDownsampleCS:
		case E::ISIBLensFlares:
		case E::ISBlur:
		case E::ISCopyDynamicFetchDisabled:
			a_group = ChainGroup::kIblf;
			a_key = { WorldScope::Iblf, a_effect, IblfVariantNow(a_effect) };
			return ChainDraw::Step;
		default:
			return ChainDraw::Unknown;
		}
	}
	if (!chain.inEndOfFrame)
		return ChainDraw::Unknown;
	if (chain.slotScope >= 0) {
		const auto slot = chain.slotScope;
		const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
		a_group = SlotGroup(static_cast<std::uint32_t>(slot), ldr);
		const auto occurrence = chain.slotDraws[a_effect]++;
		// In place, the full-screen blur's copy of its input is the image itself.
		if (slot == 6 && (a_effect == E::ISCopy || a_effect == E::ISCopyDynamicFetchDisabled))
			return ChainDraw::Nothing;
		a_key = { ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, InnerVariant(slot, a_effect, occurrence, slot == 6 && FullBlurRadius() < 1.0f) };
		return ChainDraw::Step;
	}
	const auto parity = static_cast<std::uint16_t>(Frame::PostChain::LuminanceParity());
	if (chain.inHdr) {
		switch (a_effect) {
		case E::ISHDRDownSample4:
			a_group = ChainGroup::kHdr;
			a_key = { WorldScope::Hdr, a_effect, 0 };
			return ChainDraw::Step;
		case E::ISHDRDownSample4RGB2Lum:
		case E::ISHDRDownSample16Lum:
		case E::ISHDRDownSample16LightAdapt:
			{
				// Count against the build the frame holds for the HDR part, not the newest. With none held the draw is not drawn.
				const auto held = chain.group.Held(1);
				const auto level = chain.lumLevel++;
				if (!held) {
					Frame::Driver::NoteLaggedWork(Frame::SegmentId::Post);
					return ChainDraw::Nothing;
				}
				const auto levels = held->levels;
				const bool isLast = level + 1 == levels;
				a_group = isLast ? (a_effect == E::ISHDRDownSample16LightAdapt ? ChainGroup::kHdrAdapt : ChainGroup::kHdrLum) : ChainGroup::kHdr;
				a_key = { WorldScope::Hdr, a_effect, LuminanceVariant(level, levels, parity) };
				return ChainDraw::Step;
			}
		case E::ISHDRTonemapBlendCinematic:
		case E::ISHDRTonemapBlendCinematicFade:
			a_group = a_effect == E::ISHDRTonemapBlendCinematicFade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap;
			a_key = { WorldScope::Tonemap, a_effect, static_cast<std::uint16_t>(parity << 15) };
			return ChainDraw::Step;
		default:
			break;
		}
		bool bright = false;
		const auto radius = BloomRadiusOf(a_effect, bright);
		if (radius < 0)
			return ChainDraw::Unknown;
		a_group = static_cast<std::uint8_t>(ChainGroup::kBloom + radius);
		a_key = { WorldScope::Hdr, a_effect, static_cast<std::uint16_t>(bright ? parity : 0) };
		return ChainDraw::Step;
	}
	switch (a_effect) {
	case E::ISTemporalAA:
		{
			a_group = ChainGroup::kTaa;
			const auto history = static_cast<std::uint16_t>(Frame::Driver::Current().parity & 1u);
			const auto normals = static_cast<std::uint16_t>(globals::deferred && globals::deferred->forwardRenderTargets[2] == RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_SWAP);
			a_key = { WorldScope::Taa, a_effect, static_cast<std::uint16_t>((history << 15) | (normals << 14)) };
			return ChainDraw::Step;
		}
	case E::ISUpsampleDynamicResolution:
		a_group = ChainGroup::kUpsample;
		a_key = { WorldScope::Upsample, a_effect, 0 };
		return ChainDraw::Step;
	case E::ISCopy:
		if (chain.hdrCopy && !chain.copyDrawn) {
			chain.copyDrawn = true;
			a_group = ChainGroup::kHdrCopy;
			a_key = { WorldScope::HdrCopy, a_effect, 0 };
			return ChainDraw::Step;
		}
		a_group = ChainGroup::kScreenshot;
		a_key = { WorldScope::Screenshot, a_effect, 0 };
		return ChainDraw::Step;
	default:
		break;
	}
	const auto slot = SlotOfEffect(a_effect);
	if (slot < 0)
		return ChainDraw::Unknown;
	const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
	a_group = SlotGroup(static_cast<std::uint32_t>(slot), ldr);
	a_key = { ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, 0 };
	return ChainDraw::Step;
}

/**
* @brief Captures the constants the engine bound and stages them for the step's pass in the build the frame runs, then withholds
* the engine's draw. A draw with no pass, or of a group the frame's description at Begin has off, is a mismatch.
*/
bool TakeChainStep(std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	auto& chain = Chain();
	const auto id = kEffects[a_effect].id;
	const auto segment = Frame::SegmentId::Post;
	StepKey key{};
	std::uint8_t group = 0;
	const auto draw = ChainKeyOf(id, key, group);
	if (draw == ChainDraw::Nothing)
		return true;
	if (draw == ChainDraw::Unknown) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} in the world post chain{} has no CS pass; it draws nothing", kEffects[a_effect].effectName, chain.inHdr ? " (the HDR effect)" : ""));
		return true;
	}
	// The tonemap and TAA have a routed form. The held build's schedule says which one runs.
	const auto heldOn = [&](const StepKey& a_key) {
		const auto found = chain.group.Find(a_key);
		return found.found == StepFound::Draw && RowOn(segment, found.Part().RowOf(found.at.entry));
	};
	bool routedTonemap = false, routedTaa = false;
	if (key.scope == WorldScope::Tonemap) {
		auto routedKey = key;
		routedKey.variant |= ChainRoute::kTonemapBit;
		if (heldOn(routedKey)) {
			key = routedKey;
			routedTonemap = true;
		}
	} else if (key.scope == WorldScope::Taa) {
		auto routedKey = key;
		routedKey.variant |= ChainRoute::kTaaBit;
		// Without the routed tonemap's scratch this frame, draw no TAA.
		if (heldOn(routedKey)) {
			if (!chain.scratchWritten) {
				Frame::Driver::NoteLaggedWork(segment);
				return true;
			}
			key = routedKey;
			routedTaa = true;
		}
	}
	const auto what = fmt::format("post step {} (variant {:#x}, {})", kEffects[a_effect].effectName, key.variant, ChainGroupName(group));
	const auto find = chain.group.Find(key);
	// Behind: the engine draws for variables the build the frame runs was not made for, such as new luminance levels.
	if (!chain.group.Draws(find, segment, what, "in the world post chain"))
		return true;
	const auto entry = find.at.entry;
	const auto& part = find.Part();
	auto& client = state.post[static_cast<std::size_t>(chain.group.PartOf(find.at.part))];
	if (!RowOn(segment, part.RowOf(entry))) {
		// The description at Begin has the group on, so the build the frame runs was made for a key without it: lag. Else a mismatch.
		if (ChainGroupsOn(Frame::Driver::Current().post)[group])
			Frame::Driver::NoteStepLag(segment, client.id, what);
		else
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, segment,
				fmt::format("{} is drawn, and the frame's description at Begin has its group off", what));
		return true;
	}
	if (!part.Made(entry)) {
		// Its graph build left the pass out (MakePass says why).
		Frame::Driver::NoteStepLag(segment, client.id, what);
		return true;
	}
	const auto staged = client.batch.size();
	const bool taken = StageStep(client, entry, part.SpecOf(entry), a_effect, a_compute, a_groups);
	// A step whose constants could not be staged opens nothing.
	if (client.batch.size() > staged && part.copyBack[entry] != kNoCopy)
		client.batch.push_back({ .entry = part.copyBack[entry] });
	// Only a routed tonemap that drew wrote the scratch.
	if (client.batch.size() > staged) {
		chain.scratchWritten |= routedTonemap;
		chain.taaRouted |= routedTaa;
	}
	return taken;
}

/**
* @brief The lens flare sprites' records: latch entries of the composites client past its passes, one per sprite. The sprite pass reads
* them. Their gate entries stay closed.
*/
constexpr std::uint32_t kLensFlareSprites = 128;
constexpr std::uint32_t kLensFlareRecordFirst = kGateEntries - kLensFlareSprites;

/** @brief Mirrors LensFlareSpritesAt in ISLensFlare.hlsl: the sprite pass's own latch region. */
struct LensFlareSpritesAt
{
	std::uint32_t recordOffset = 0;  // bytes from this region to the first sprite's record
	std::uint32_t recordStride = kLatchStride;
	std::uint32_t sprites = 0;
	std::uint32_t verticesPerSprite = 0;
	std::array<float, 4> viewport{ 1.0f, 1.0f, 0.0f, 0.0f };  // the draw's viewport in kMAIN as fractions: scale xy, offset xy
	std::array<std::uint32_t, 20> unused{};
};
static_assert(sizeof(LensFlareSpritesAt) == kLatchStride);

/** @brief Mirrors LensFlareSpriteRecord in ISLensFlare.hlsl: one sprite's captured constants, texture and geometry. */
struct LensFlareSpriteRecord
{
	std::uint32_t pixelSrv = 0, pixelOffset = 0, pixelSize = 0, texture = 0;  // PS b2, then t0's heap index
	std::uint32_t vertexSrv = 0, vertexOffset = 0, vertexSize = 0, firstIndex = 0;  // VS b2, then the first index
	std::uint32_t vertexBuffer = 0, vertexBufferOffset = 0, indexBuffer = 0, indexBufferOffset = 0;  // buffer view refs
	std::uint32_t layoutLow = 0, layoutHigh = 0, stride = 0, unusedWord = 0;
	std::array<std::uint32_t, 12> unused{};
};
static_assert(sizeof(LensFlareSpriteRecord) == kLatchStride);

/** @brief Whether a draw is a lens flare step: BSLensFlare's frame function runs inside RenderWorldPostProcess. Render owner. */
bool LensFlareStepNow(std::size_t a_effect)
{
	const auto id = kEffects[a_effect].id;
	return (id == Effects::ISLensFlareVisibility || id == Effects::ISLensFlare) && IS().on && Chain().group.Registered() && Frame::PostChain::InChain();
}

/**
* @brief Logs once a session when the engine's target or blend at a lens flare pass differs from its step's. The step's are from the
* engine's code (SE 100274 and its callees), so a difference means that reading is wrong.
*/
void CheckLensFlareState(Effects::ImageSpaceEffectEnum a_effect)
{
	auto& chain = Chain();
	const bool sprite = a_effect == Effects::ISLensFlare;
	auto& checked = sprite ? chain.lensSpriteChecked : chain.lensVisibilityChecked;
	if (checked)
		return;
	checked = true;
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	const auto target = static_cast<std::int32_t>(shadow.renderTargets[0]);
	const std::int32_t plannedTarget = sprite ? RE::RENDER_TARGET::kMAIN : RE::RENDER_TARGET::kLENSFLAREVIS;
	const std::uint32_t plannedMode = sprite ? 5 : 0, plannedWrite = sprite ? 1 : 5;
	if (target != plannedTarget || shadow.alphaBlendMode != plannedMode || shadow.alphaBlendWriteMode != plannedWrite)
		logger::error("[ORG] ImageSpace: the engine's {} writes rt{} with blend mode {} and write mode {}; CS's step writes rt{} with {} and {}",
			kEffects[static_cast<std::size_t>(EffectIndexOf(a_effect))].effectName, target, shadow.alphaBlendMode, shadow.alphaBlendWriteMode, plannedTarget,
			plannedMode, plannedWrite);
}

/**
* @brief Stages one lens flare sprite: its record in the next record entry, and the sprite pass opened for every sprite so far.
* The batch stays open across the sprites' scopes (LeaveScope), so the pass opens once a run with all of them.
*/
void StageLensFlareSprite(PostClient& a_client, std::uint32_t a_entry, const Spec& a_spec, std::uint32_t a_passes)
{
	auto& state = IS();
	auto& chain = Chain();
	constexpr auto segment = Frame::SegmentId::Post;
	const auto frame = Frame::Driver::Current().frame;
	if (chain.lensFrame != frame) {
		chain.lensFrame = frame;
		chain.lensSprites = 0;
		chain.lensVerticesPerSprite = 0;
	}
	const auto isPass = [a_entry](const Pending& a_pending) { return a_pending.entry == a_entry && a_pending.open; };
	// A sprite after the batch with this frame's sprite pass ran has no pass left to draw in.
	if (chain.lensSprites > 0 && std::ranges::none_of(a_client.batch, isPass)) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("lens flare sprite {} of the frame came after the batch with its sprite pass ran; it draws nothing", chain.lensSprites));
		return;
	}
	const auto sprite = chain.lensSprites;
	if (sprite >= kLensFlareSprites || a_passes > kLensFlareRecordFirst) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("lens flare sprite {} of the frame is past the sprite pass's {} records; it draws nothing", sprite, kLensFlareSprites));
		return;
	}
	static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
	setDirtyStates(false);
	// Both stages read their own PerGeometry: the vertex stage places the sprite, the pixel stage tints it.
	const auto use = Constants::Capture(Constants::kDrawStages);
	const auto* pixel = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 2) : nullptr;
	const auto* vertex = use.Valid() ? Constants::Find(use, Constants::Stage::Vertex, 2) : nullptr;
	if (!pixel || !vertex) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("a lens flare sprite's capture holds no PerGeometry for its {} stage; it draws nothing", pixel ? "vertex" : "pixel"));
		return;
	}
	const auto pixelAt = Constants::Where(*pixel);
	const auto vertexAt = Constants::Where(*vertex);
	if (!pixelAt.owner || !vertexAt.owner)
		RenderGraph::Fatal("ISLensFlare", "its captured constants have no ring chunk");
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	DCLF::Family::Geometry geometry;
	auto owners = std::make_shared<DCLF::Family::Owners>();
	std::string detail;
	const auto result = DCLF::Family::LatchGeometry(static_cast<const DCLF::Family::TriShape*>(state.drawShape), state.drawStart, state.drawTriangles,
		~std::uint64_t(0), static_cast<std::uint32_t>(shadow.topology), geometry, *owners, &detail);
	if (result != DCLF::Family::GeometryResult::Ok) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("a lens flare sprite's quad could not be staged: {} ({}); it draws nothing", DCLF::Family::ReasonOf(result), detail));
		return;
	}
	// The vertex stage finds a vertex's sprite by dividing by one count, so every sprite must draw as many vertices.
	if (chain.lensVerticesPerSprite && geometry.indexCount != chain.lensVerticesPerSprite) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("lens flare sprite {} draws {} vertices, and the frame's first sprite drew {}; it draws nothing", sprite, geometry.indexCount,
				chain.lensVerticesPerSprite));
		return;
	}
	// The sprite's loaded texture by heap index. The engine binds none for a flare without one, and D3D11 then reads zero.
	auto& textures = ::DCLF::GpuTextures::Get();
	auto* view = shadow.PSTexture[0];
	const auto input = view ? InputOfView(view) : -1;
	const bool target = input >= 0 && !(input & kDefaultTextureInput);
	auto binding = view && !target ? textures.ResolveBinding(view, kImageSpaceTag) : ::DCLF::GpuTextures::Binding{};
	if (!binding)
		binding = textures.NullOf(::DCLF::GpuTextures::ViewKind::Texture2D);
	if (!binding)
		RenderGraph::Fatal("ISLensFlare", "the zero Texture2D view could not be made");
	if (binding.owner)
		owners->push_back(std::move(binding.owner));
	const LensFlareSpriteRecord record{ .pixelSrv = pixelAt.srvIndex,
		.pixelOffset = pixelAt.offset,
		.pixelSize = pixelAt.size,
		.texture = binding.index,
		.vertexSrv = vertexAt.srvIndex,
		.vertexOffset = vertexAt.offset,
		.vertexSize = vertexAt.size,
		.firstIndex = geometry.firstIndex,
		.vertexBuffer = geometry.vertexRef.viewSlot,
		.vertexBufferOffset = geometry.vertexRef.byteOffset,
		.indexBuffer = geometry.indexRef.viewSlot,
		.indexBufferOffset = geometry.indexRef.byteOffset,
		.layoutLow = static_cast<std::uint32_t>(geometry.layout),
		.layoutHigh = static_cast<std::uint32_t>(geometry.layout >> 32),
		.stride = geometry.stride };
	a_client.batch.push_back(
		Pending{ .entry = kLensFlareRecordFirst + sprite, .source = std::move(owners), .record = std::bit_cast<LatchWords>(record), .open = false });
	chain.lensSprites = sprite + 1;
	chain.lensVerticesPerSprite = geometry.indexCount;
	// The engine's draw keeps the dynamic-resolution viewport while the effect's flag is set (ImageSpaceEffect +0x88).
	const auto* shader = state.current ? state.current->shader : nullptr;
	const bool dynamic = shader && static_cast<const RE::ImageSpaceEffect*>(shader)->unk88;
	const LensFlareSpritesAt at{ .recordOffset = (kLensFlareRecordFirst - a_entry) * kLatchStride,
		.sprites = chain.lensSprites,
		.verticesPerSprite = chain.lensVerticesPerSprite,
		.viewport = dynamic ? RenderViewportOf(a_spec) : std::array<float, 4>{ 1.0f, 1.0f, 0.0f, 0.0f } };
	Pending pass{ .entry = a_entry, .vertices = chain.lensSprites * chain.lensVerticesPerSprite, .record = std::bit_cast<LatchWords>(at) };
	if (const auto staged = std::ranges::find_if(a_client.batch, isPass); staged != a_client.batch.end())
		*staged = std::move(pass);
	else
		a_client.batch.push_back(std::move(pass));
	state.postKeep.push_back(pixelAt.owner);
	state.postKeep.push_back(vertexAt.owner);
	++state.taken;
}

/**
* @brief Matches a lens flare draw to its pass in the build the frame runs. The visibility pass stages like any step. Each sprite
* stages a record for the one sprite pass. Withholds the engine's draw.
*/
bool TakeLensFlareStep(std::size_t a_effect)
{
	auto& state = IS();
	auto& chain = Chain();
	const auto id = kEffects[a_effect].id;
	constexpr auto segment = Frame::SegmentId::Post;
	const StepKey key{ WorldScope::LensFlare, id, 0 };
	const auto what = fmt::format("lens flare step {}", kEffects[a_effect].effectName);
	const auto find = chain.group.Find(key);
	// Behind: the engine draws for variables the build the frame runs was not made for, such as a new size.
	if (!chain.group.Draws(find, segment, what, "in the world post chain"))
		return true;
	const auto entry = find.at.entry;
	const auto& part = find.Part();
	auto& client = state.post[static_cast<std::size_t>(chain.group.PartOf(find.at.part))];
	// The group is always on, so a row off is a build made without it, and a pass not made was left out by its graph build (MakePass).
	if (!RowOn(segment, part.RowOf(entry)) || !part.Made(entry) || !part.SpecOf(entry)) {
		Frame::Driver::NoteStepLag(segment, client.id, what);
		return true;
	}
	CheckLensFlareState(id);
	if (id == Effects::ISLensFlareVisibility)
		return StageStep(client, entry, part.SpecOf(entry), a_effect, false, {});
	StageLensFlareSprite(client, entry, *part.SpecOf(entry), part.count);
	return true;
}

/** @brief In a routed frame with no TAA, copies the scratch into the final target. Render owner. */
void OpenThrough()
{
	auto& chain = Chain();
	const bool written = std::exchange(chain.scratchWritten, false);
	const bool taa = std::exchange(chain.taaRouted, false);
	if (!written || taa)
		return;
	constexpr std::size_t p = 2;
	auto& client = IS().post[static_cast<std::size_t>(kChainParts[p])];
	// The routed tonemap drew from the held build, so that build has the copy. A schedule without it is a mismatch.
	const auto held = chain.group.Held(p);
	const auto entry = held ? held->throughEntry : kNoCopy;
	if (entry == kNoCopy || !RowOn(Frame::SegmentId::Post, held->table->parts[p].RowOf(entry)) || !held->table->parts[p].Made(entry)) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, Frame::SegmentId::Post,
			"the routed tonemap wrote the LDR scratch, and the pass-through copy is not in the frame's schedule");
		return;
	}
	client.batch.push_back({ .entry = entry });
	if (Usable())
		Flush();
}

/** @brief Runs the frozen background copy last in the post stage. Without it in the schedule this capture is lost and logged. Render owner. */
void OpenCapture()
{
	auto& chain = Chain();
	auto& state = IS();
	constexpr auto segment = Frame::SegmentId::Post;
	constexpr std::size_t p = 2;
	auto& client = state.post[static_cast<std::size_t>(kChainParts[p])];
	const auto held = chain.group.Held(p);
	const auto entry = held ? held->captureEntry : kNoCopy;
	const auto row = entry == kNoCopy ? 256u : held->table->parts[p].RowOf(entry);
	const bool made = entry != kNoCopy && held->table->parts[p].Made(entry);
	if (!RowOn(segment, row) || !made) {
		logger::warn("[ORG] ImageSpace: the frozen background's copy is not in frame {}'s schedule (made {}, row {}); kMENUBG keeps its last image",
			Frame::Driver::Current().frame, made, row);
		Frame::Driver::NoteLaggedWork(segment);
		return;
	}
	client.batch.push_back({ .entry = entry });
	if (Usable())
		Flush();
	static std::uint64_t logged = 0;
	if (logged++ < 4)
		logger::info("[ORG] ImageSpace: frame {} is a frozen world's capture frame: the post stage copies the final target into kMENUBG",
			Frame::Driver::Current().frame);
}

struct ChainCompositesGuard
{
	ChainCompositesGuard() : active(IS().on && Frame::PostChain::InChain() && FrameAnchors::IsRenderOwner())
	{
		if (active) {
			Chain().inComposites = true;
			Chain().iblfDraws.clear();
		}
	}
	~ChainCompositesGuard()
	{
		if (!active)
			return;
		Chain().inComposites = false;
		// Tap now, before the PostProcessing and Upscale steps. A scope still open around the composites would tap it after them.
		if (!IS().post[static_cast<std::size_t>(PostPart::Composites)].batch.empty())
			Flush();
	}
	bool active = false;
};

struct ChainHdrGuard
{
	ChainHdrGuard() : active(Chain().inEndOfFrame)
	{
		if (active) {
			Chain().inHdr = true;
			Chain().lumLevel = 0;
		}
	}
	~ChainHdrGuard()
	{
		if (active)
			Chain().inHdr = false;
	}
	bool active = false;
};

/** @brief Hooks the Render of the full-screen blur and get-hit effects (vtable slot 1). */
struct ChainSlotGuard
{
	explicit ChainSlotGuard(std::int32_t a_slot) : active(Chain().inEndOfFrame && !Chain().inHdr)
	{
		if (active) {
			Chain().slotScope = a_slot;
			Chain().slotDraws.clear();
		}
	}
	~ChainSlotGuard()
	{
		if (active)
			Chain().slotScope = -1;
	}
	bool active = false;
};

/** @brief Empty when CS does not own the pair or a_view is another target. Render owner. */
std::string_view AdaptedLuminanceIdOf(const ID3D11ShaderResourceView* a_view)
{
	const auto& chain = Chain();
	// The levels of the build the frame holds for the HDR part, as the draws count them.
	const auto held = chain.group.Held(1);
	const auto levels = held ? held->levels : 0u;
	if (!IS().on || !chain.group.Registered() || !chain.adapt[0] || !chain.adapt[1] || !a_view || levels == 0)
		return {};
	auto* view = Tripwire::Unwrap(const_cast<ID3D11ShaderResourceView*>(a_view));
	if (!view)
		view = const_cast<ID3D11ShaderResourceView*>(a_view);
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	for (std::uint32_t p = 0; p < 2; ++p) {
		const auto index = RE::RENDER_TARGET::kHDR_DOWNSAMPLE1 + static_cast<std::int32_t>(levels) - 1 + 2 * static_cast<std::int32_t>(p);
		if (index > RE::RENDER_TARGET::kHDR_DOWNSAMPLE13)
			return {};
		auto* engine = targets[index].SRV;
		auto* engineView = Tripwire::Unwrap(engine);
		if (!engineView)
			engineView = engine;
		if (engineView && engineView == view)
			return kAdaptedIds[p];
	}
	return {};
}
