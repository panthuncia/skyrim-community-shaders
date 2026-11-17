// The world post chain as CS passes (native mode). Included by ImageSpace.cpp inside its anonymous namespace, after WorldSteps.inl,
// whose step helpers it uses (StepDesc, MakeStepSpec, StepKey, MakeOwned, SelectEntries, StageStep).
//
// ImageSpaceManager::RenderWorldPostProcess (SE 100430 / AE 107148) is drawn by passes CS declares here, in the engine's order: the
// pre-tonemap composite, then RenderEndOfFrameEffects (99023 / 105674): the slots before the tonemap, the HDR effect (100465 /
// 107183) or the copy in its place, its tonemap, TAA, the screenshot copy, the slots after the tonemap, the upsample. Each step is
// CS's port of the effect's shader with its inputs, outputs, samplers, blend and viewport written below, sized at the build. The
// engine's draw is withheld; its hook only stages the constants the engine computed for the step (TakeChainStep).
//
// Which effects a frame's chain holds is the description's (ChainRows at Begin). The targets are CS's and fixed: a step reads and
// writes the same targets whichever other steps are present or run. The chain works in place on two images: kMAIN before the
// tonemap (the scene colour the Upscale anchor writes) and the final target after it (the image the UI draws over). An effect that
// cannot write the image it reads draws into its side's scratch target, and a copy, opened with the effect's last draw, puts the
// result back. So an effect that is absent from the stage's build, or that the engine did not draw this frame, leaves the image as the
// step before it left it: the frame is whole, only without that effect. The engine's own ping-pong targets (kMAIN_COPY and the two
// temporary copies) are not used. At RenderEndOfFrameEffects' entry the engine's loop is replayed only to check it against the
// description (CheckEndOfFrame).

/** @brief The groups a frame's description turns on or off as one (ChainRows), and that a hook asks for when its pass is off. */
namespace ChainGroup
{
	constexpr std::uint8_t kComposite = 0;
	constexpr std::uint8_t kPreSlot = 1;   // + the slot, 0 to 7
	constexpr std::uint8_t kLdrSlot = 9;   // + the slot - 3, slots 3 to 7
	constexpr std::uint8_t kHdrCopy = 14;  // the copy in the HDR effect's place
	constexpr std::uint8_t kHdr = 15;      // the downsample and the luminance levels before the last
	constexpr std::uint8_t kHdrLum = 16;   // the last level drawn by the luminance shader (no adapted luminance yet)
	constexpr std::uint8_t kHdrAdapt = 17; // the last level drawn by the light adaptation
	constexpr std::uint8_t kBloom = 18;    // + the radius index, 0 to 6
	constexpr std::uint8_t kTonemap = 25;
	constexpr std::uint8_t kTonemapFade = 26;
	constexpr std::uint8_t kTaa = 27;
	constexpr std::uint8_t kScreenshot = 28;
	constexpr std::uint8_t kUpsample = 29;
	constexpr std::uint8_t kCapture = 30;  // a frozen world's capture frame: the final target copied into kMENUBG
	constexpr std::uint8_t kCount = 31;
}

/** @brief An entry with no copy back after it (ChainSteps::Part::copyBack). */
constexpr std::uint16_t kNoCopy = 0xffff;

/** @brief The three post clients' parts the chain's passes live in, as PostPart values. */
constexpr std::array<PostPart, 3> kChainParts{ PostPart::Composites, PostPart::Hdr, PostPart::Final };

/** @brief The chain's registration: its passes by key, the CS-owned targets, what the build was made for, and the running chain. */
struct ChainSteps
{
	bool on = false;          // native mode with the post stages: the chain is CS passes
	bool registered = false;  // registered for the sizes below
	bool retry = false;       // a pass was left out at a build: register again at a later frame
	std::uint64_t registeredFrame = 0;
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
	std::pair<float, float> ratios{ 1.0f, 1.0f };
	struct Entry
	{
		std::uint8_t part = 0;  // index into kChainParts
		std::uint32_t entry = 0;
	};
	std::map<StepKey, Entry> entries;
	// Per part: each entry's key and group, whether the last build made its pass (set on the build thread), whether it is a copy back,
	// the copy back its draw opens (kNoCopy: none), the entry count, and the part's first row in the post stage's rows (the three parts
	// share them, in order).
	struct Part
	{
		std::array<StepKey, kGateEntries> keys{};
		std::array<std::uint8_t, kGateEntries> group{};
		std::array<bool, kGateEntries> made{};
		std::array<bool, kGateEntries> copy{};
		std::array<std::uint16_t, kGateEntries> copyBack{};
		// The TAA routing an entry belongs to (ChainRoute): any, direct or routed.
		std::array<std::uint8_t, kGateEntries> route{};
		std::uint32_t count = 0;
		std::uint32_t rowBase = 0;
	};
	std::array<Part, 3> parts;
	// Groups a hook found off in the frame's schedule (off, or absent from the stage's build): the frame each was last asked for at (0:
	// never). The descriptions have a group on for kRequestFrames after its request. Render owner.
	std::array<std::uint64_t, ChainGroup::kCount> requested{};
	// The CS-owned targets: the bloom's blur and its scratch, the depth of field's downsample and blur scratch, TAA's history pair, the
	// get-hit effect's two blur buffers and its blur swap, and the full-screen blur's downsample and its blur scratch.
	OwnedTarget bloom, bloomSwap, dofDown, dofSwap;
	std::array<OwnedTarget, 2> taaHistory;
	OwnedTarget getHitA, getHitB, getHitSwap, fullBlurDown, fullBlurSwap;
	// The scratch targets of the two images: an effect draws there, and its copy back puts the result into kMAIN (hdrScratch) or the
	// final target (ldrScratch). Each is made with its image's size and format, so the copy is a plain texel copy.
	OwnedTarget hdrScratch, ldrScratch;
	// The adapted luminance pair (the last luminance level of each parity, in place of the engine's 27 + levels - 1 + 2 * parity): a
	// history, published (kAdaptedIds) for Drawcall Limit Fix's character light, which reads it in place of the engine's target.
	std::array<OwnedTarget, 2> adapt;
	// The blur shaders the engine's blur effects draw (ImageSpaceEffectBlur's HDR flag, +0x95, of effects[9 + radius index]), read at the
	// registration: true for ISBlurN, false for ISNonHDRBlurN.
	std::array<bool, 7> blurHdr{};
	// The running chain (the hooks): inside the composites, inside RenderEndOfFrameEffects, inside its HDR effect, the luminance
	// level so far there, whether the copy takes the HDR effect's place this frame and whether it drew.
	bool inComposites = false, inEndOfFrame = false, inHdr = false, hdrCopy = false, copyDrawn = false;
	std::uint32_t lumLevel = 0;
	// Inside the full-screen blur (slot 6) or the get-hit effect (slot 7) of RenderEndOfFrameEffects, -1 elsewhere; each effect's draws
	// so far there.
	std::int32_t slotScope = -1;
	std::map<Effects::ImageSpaceEffectEnum, std::uint8_t> slotDraws;
	// The frozen background's copy (the final target into kMENUBG): its entry in the final part (kNoCopy: not made), and whether this
	// frame's end-of-frame effects run into kMENUBG (a frozen world's capture frame, checked at their entry).
	std::uint16_t captureEntry = kNoCopy;
	bool captureFrame = false;
	// The TAA routing (ChainRoute): the pass-through copy's entry in the final part (kNoCopy: not made), and this frame's end-of-frame
	// effects so far: whether a routed tonemap wrote the LDR scratch, and whether routed TAA read it.
	std::uint16_t throughEntry = kNoCopy;
	bool scratchWritten = false, taaRouted = false;
};

/**
 * @brief The TAA routing of a step. TAA cannot write the image it reads. Direct, it draws the final target into the LDR scratch and a
 * copy back puts the result into the final target, every frame it runs. Routed, the tonemap draws into the LDR scratch and TAA draws from
 * there into the final target, with no copy. The stage's build key picks the routing (TAA, the HDR effect and CS's tonemap: routed), so a
 * build holds one routing, and a frame is described with the routing its build holds (FrameAnchors::DescribedBuildHasRow). In a routed
 * build's frame without TAA (its build pending, or TAA off since), a pass-through copy moves the scratch into the final target instead.
 * A frame whose tonemap did not write the scratch (the HDR effect off, Effects11's tonemap) draws no TAA.
 */
namespace ChainRoute
{
	constexpr std::uint8_t kAny = 0;
	constexpr std::uint8_t kDirect = 1;
	constexpr std::uint8_t kRouted = 2;
	constexpr std::uint16_t kTonemapBit = 1u << 14;  // in the tonemap's variant (bit 15 is the parity)
	constexpr std::uint16_t kTaaBit = 1u << 13;      // in TAA's variant (bits 15 and 14 are the history and the normals target)
}

ChainSteps& Chain()
{
	static ChainSteps chain;
	return chain;
}

/** @brief The stage a chain part's passes are in: the post stage holds all three. */
Frame::SegmentId SegmentOfPart(std::size_t)
{
	return Frame::SegmentId::Post;
}

/** @brief Whether this frame's schedule has a chain group on: any of its passes' rows set, in a stage whose graph holds its client. */
bool GroupScheduled(std::uint8_t a_group)
{
	const auto& chain = Chain();
	const auto& state = IS();
	for (std::size_t p = 0; p < chain.parts.size(); ++p) {
		const auto& part = chain.parts[p];
		const auto& rows = Frame::Driver::Current().Segment(SegmentOfPart(p)).rows;
		for (std::uint32_t entry = 0; entry < part.count; ++entry) {
			if (part.group[entry] != a_group)
				continue;
			const auto row = part.rowBase + entry;
			if (row < 256 && (rows[row / 64] >> (row % 64) & 1u) && FrameAnchors::CanTap(state.post[static_cast<std::size_t>(kChainParts[p])].id))
				return true;
		}
	}
	return false;
}

/** @brief The end-of-frame slot (0 to 7) whose effect draws a_effect, or -1. ISHDRDownSample4 outside the HDR effect is the depth of field's. */
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

/** @brief The group of slot a_slot in its place (before or after the tonemap). */
std::uint8_t SlotGroup(std::uint32_t a_slot, bool a_ldr)
{
	return static_cast<std::uint8_t>(a_ldr ? ChainGroup::kLdrSlot + (a_slot - 3) : ChainGroup::kPreSlot + a_slot);
}

/** @brief Whether slot a_slot draws after the tonemap: usesLDR (manager +0x210) and a slot from 3 to 7 (RenderEndOfFrameEffects). */
bool SlotIsLdr(std::uint32_t a_slot)
{
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	const bool ldr = manager && *reinterpret_cast<const bool*>(reinterpret_cast<std::uintptr_t>(manager) + 0x210);
	return ldr && a_slot >= 3 && a_slot <= 7;
}

/** @brief A step variant's two targets: the one it reads and the one it writes (RE::RENDER_TARGET values below 256). The menu composites
 * key their copies by it (MenuSteps.inl); the world chain's targets are fixed. */
constexpr std::uint16_t InOut(std::int32_t a_in, std::int32_t a_out)
{
	return static_cast<std::uint16_t>(((a_in & 0x7f) << 8) | (a_out & 0xff));
}

std::array<bool, ChainGroup::kCount> ChainGroupsOn(const Frame::PostFields& a_post);

/** @brief A chain group's name for the logs. */
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
	default:
		return fmt::format("group {}", a_group);
	}
}

/**
 * @brief RenderEndOfFrameEffects' entry: the engine's loop (SE 0x141297410) replayed against CS's description. CS's targets do not come
 * from it: every step reads and writes fixed targets. The loop runs on a_source (kMAIN) and ends in a_final (the final target), the
 * chain's two images; another target is a mismatch. An effect the engine has active that the frame's schedule does not hold is lagged
 * work while the frame's own description has it (its build is pending; the step's hook counts it), or while the post fields read now
 * have it (the engine or CS changed it after Begin: a setting, a method switch; the next description has it). Otherwise it is a
 * mismatch: the description cannot see what the engine runs. Also notes whether the copy takes the HDR effect's place. Render owner.
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
	// A frozen world's capture frame (decided at Begin: a menu that freezes the background opened, Driver::DecideKind) runs the chain into
	// kMENUBG. CS's chain still ends in the final target, and its last pass copies that into kMENUBG (the frozen background's copy).
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
	const auto now = ChainGroupsOn(Frame::PostChain::Read());
	const auto check = [&](std::uint8_t a_group) {
		if (GroupScheduled(a_group) || wanted[a_group] || now[a_group])
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

/** @brief The adapted luminance pair's published ids (ChainSteps::adapt). */
constexpr std::array<std::string_view, 2> kAdaptedIds{ "ImageSpace::AdaptedLuminance0", "ImageSpace::AdaptedLuminance1" };

/** @brief The full-screen blur's radius (manager +0x128): the effect's Render picks its blur step from it, and blends below 1. */
float FullBlurRadius()
{
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	return manager ? *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(manager) + 0x128) : 0.0f;
}

/**
 * @brief The variant of a draw inside the full-screen blur (slot 6) or the get-hit effect (slot 7): which of the effect's draws it is (the
 * occurrence of its shader in the slot's scope), and whether the full-screen blur's last draw blends.
 *
 * ImageSpaceEffectGetHit (Setup SE 0x141328b80): the input copied into a buffer (ISCopy), blurred twice by effects[10] (an
 * ImageSpaceEffectBlur of radius 5: a horizontal then a vertical draw each, through a second buffer and the blur swap), then the input
 * and the blurred buffer drawn into the output (ISDoubleVision). ImageSpaceEffectFullScreenBlur (Setup 0x141329df0, Render 0x14132a370):
 * the input downsampled (ISHDRDownSample4), blurred by the blur effect of the radius step (effects[9 + step]: horizontal, vertical),
 * then, below radius 1, the input copied into the output (ISCopy, or ISCopyDynamicFetchDisabled with Legacy Graphics Compatibility's
 * eleventh step); last the blurred image drawn into the output (ISHDRDownSample4 with the blur's parameters), blended over that copy
 * below radius 1. CS draws the full-screen blur in place, so its copy has nothing to do.
 */
std::uint16_t InnerVariant(std::int32_t a_slot, Effects::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, bool a_blended)
{
	const bool blends = a_slot == 6 && a_blended && a_effect == Effects::ISHDRDownSample4 && a_occurrence == 1;
	// Bit 10 marks an inner draw, so none shares a key with a slot's own draws (the depth of field's downsample is variant 0).
	return static_cast<std::uint16_t>((blends ? 0x8000u : 0u) | (a_occurrence & 7u) << 12 | (a_slot == 7 ? 0x800u : 0u) | 0x400u);
}


/**
 * @brief CS_ORG_ADAPT_READBACK=1 (a diagnostic, native mode): each world frame the post stage copies the luminance chain's last input
 * level and both adapted luminance targets into a readback ring, and CheckChainSteps logs their values five frames later (every 30th
 * frame), to show the adaptation converging. The ring is outside the graph: written by a copy at the end of the post stage, read on the
 * CPU once the frame's work has surely completed.
 */
struct AdaptReadback
{
	static constexpr std::uint32_t kSlots = 8;
	static constexpr std::uint32_t kEntries = 3;    // the last input level, adapted parity 0, adapted parity 1
	static constexpr std::uint32_t kEntryBytes = 4096;
	std::shared_ptr<org::Buffer> buffer;
	std::array<std::uint64_t, kSlots> frames{};    // the frame each slot holds (render owner)
	RunSlots<std::uint64_t> runs;                   // the frame a run copies (the final part's commit stores it)
	std::array<rhi::Format, kEntries> formats{};
	std::array<std::pair<std::uint32_t, std::uint32_t>, kEntries> sizes{};
	std::uint64_t logged = 0;
};

AdaptReadback& Readback()
{
	static AdaptReadback readback;
	return readback;
}

bool ReadbackOn()
{
	static const bool on = Switches::On("CS_ORG_ADAPT_READBACK");
	return on;
}

struct ReadbackBindings
{
	std::array<org::ResourceBindingToken, AdaptReadback::kEntries> sources{};
};

struct ReadbackFrame
{
	std::shared_ptr<const std::uint64_t> frame;
};

/** @brief The readback's copy: recorded at execution, into the ring slot of the frame its run belongs to. */
class ReadbackPass final : public org::TypedRenderGraphPass<ReadbackPass, ReadbackFrame, ReadbackBindings>
{
public:
	explicit ReadbackPass(std::array<FrameAnchors::Texture, AdaptReadback::kEntries> a_sources) : sources(std::move(a_sources)) {}

	ReadbackBindings Declare(org::PassBuilder& a_builder)
	{
		a_builder.PreferQueue(org::QueueKind::Graphics);
		ReadbackBindings bindings{};
		for (std::size_t i = 0; i < sources.size(); ++i)
			bindings.sources[i] = a_builder.CopySource(sources[i]);
		return bindings;
	}

	void Revision(std::vector<std::uint64_t>&) const {}

	ReadbackFrame Prepare(const ReadbackBindings&, const org::PassPrepareContext&) const { return {}; }

	static bool SkipsAtExecution(const ReadbackBindings&, const ReadbackFrame&, const org::PassRecordContext& a_recording)
	{
		return !Readback().runs.Find(a_recording.Run()) || !Readback().buffer;
	}

	static void Record(const ReadbackBindings& a_bindings, const ReadbackFrame&, org::PassRecordContext& a_recording)
	{
		auto& readback = Readback();
		const auto frame = readback.runs.Take(a_recording.Run());
		if (!frame || !readback.buffer)
			return;
		const auto slot = static_cast<std::uint32_t>(*frame % AdaptReadback::kSlots);
		for (std::uint32_t i = 0; i < AdaptReadback::kEntries; ++i) {
			const auto [width, height] = readback.sizes[i];
			rhi::BufferTextureCopyFootprint copy{};
			copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
			copy.buffer = readback.buffer->GetAPIResource().GetHandle();
			copy.mip = 0;
			copy.arraySlice = 0;
			copy.footprint = { (static_cast<std::uint64_t>(slot) * AdaptReadback::kEntries + i) * AdaptReadback::kEntryBytes, 256, width, height, 1 };
			a_recording.Commands().CopyTextureToBuffer(copy);
		}
	}

private:
	std::array<FrameAnchors::Texture, AdaptReadback::kEntries> sources;
};

/** @brief One texel's first channel as a float, for the formats the luminance chain uses. */
float ReadTexel(const std::byte* a_texel, rhi::Format a_format)
{
	const auto half = [](std::uint16_t a_bits) { return DirectX::PackedVector::XMConvertHalfToFloat(a_bits); };
	switch (a_format) {
	case rhi::Format::R11G11B10_Float:
		{
			// The red channel: 6 mantissa bits, 5 exponent bits, no sign.
			const auto bits = *reinterpret_cast<const std::uint32_t*>(a_texel) & 0x7ffu;
			const auto exponent = static_cast<int>(bits >> 6);
			const auto mantissa = static_cast<float>(bits & 0x3fu) / 64.0f;
			if (exponent == 0)
				return std::ldexp(mantissa, -14);
			if (exponent == 31)
				return std::numeric_limits<float>::infinity();
			return std::ldexp(1.0f + mantissa, exponent - 15);
		}
	case rhi::Format::R32_Float:
	case rhi::Format::R32G32_Float:
	case rhi::Format::R32G32B32A32_Float:
		return *reinterpret_cast<const float*>(a_texel);
	default:
		return half(*reinterpret_cast<const std::uint16_t*>(a_texel));
	}
}

/** @brief The bytes of one texel of a_format (BasicRHI's size; 8 when it does not know the format). */
std::uint32_t TexelBytes(rhi::Format a_format)
{
	const auto bytes = rhi::FormatByteSize(a_format);
	return bytes ? bytes : 8;
}

/** @brief At a world frame's Begin: logs the readback of five frames ago, every 30th frame. Render owner. */
void LogReadback(std::uint64_t a_frame)
{
	auto& readback = Readback();
	if (!ReadbackOn() || !readback.buffer || a_frame < 5 || a_frame - readback.logged < 30)
		return;
	const auto frame = a_frame - 5;
	const auto slot = static_cast<std::uint32_t>(frame % AdaptReadback::kSlots);
	if (readback.frames[slot] != frame)
		return;
	void* mapped = nullptr;
	readback.buffer->GetAPIResource().Map(&mapped);
	if (!mapped)
		return;
	readback.logged = a_frame;
	std::array<std::string, AdaptReadback::kEntries> text;
	for (std::uint32_t i = 0; i < AdaptReadback::kEntries; ++i) {
		const auto* base = static_cast<const std::byte*>(mapped) + (static_cast<std::uint64_t>(slot) * AdaptReadback::kEntries + i) * AdaptReadback::kEntryBytes;
		const auto [width, height] = readback.sizes[i];
		const auto texel = TexelBytes(readback.formats[i]);
		double sum = 0.0;
		float least = 1e30f, most = -1e30f;
		for (std::uint32_t y = 0; y < height; ++y)
			for (std::uint32_t x = 0; x < width; ++x) {
				const float value = ReadTexel(base + y * 256 + x * texel, readback.formats[i]);
				sum += value;
				least = (std::min)(least, value);
				most = (std::max)(most, value);
			}
		text[i] = fmt::format("mean {:.5g} (min {:.5g}, max {:.5g}, {}x{})", sum / (std::max)(1u, width * height), least, most, width, height);
	}
	readback.buffer->GetAPIResource().Unmap(0, 0);
	logger::info("[ORG] adaptation readback, frame {} (luminance parity {}; formats {} {} {}): last input level {}; adapted 0 {}; adapted 1 {}", frame,
		Frame::PostChain::LuminanceParity(), static_cast<int>(readback.formats[0]), static_cast<int>(readback.formats[1]), static_cast<int>(readback.formats[2]),
		text[0], text[1], text[2]);
}

void RegisterChainSteps();

/**
 * @brief The final target's size (kFRAMEBUFFER, from the render target manager: a static object at 524970 / 411451, not the pointer
 * CommonLib's GetSingleton reads). The renderer's own entry for it holds no texture in native mode.
 */
std::pair<std::uint32_t, std::uint32_t> FinalSize()
{
	static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> manager{ REL::RelocationID(524970, 411451) };
	const auto& data = manager.get()->renderTargetData[RE::RENDER_TARGET::kFRAMEBUFFER];
	return { data.width, data.height };
}

/**
 * @brief Begin of a world frame: the chain registered again when what its passes were sized for changed (kMAIN's or the final
 * target's size, the luminance level count, the dynamic-resolution ratios) or a build left a pass out. The build runs off the render
 * thread; frames keep the old schedule until it switches in. Render owner.
 */
void CheckChainSteps()
{
	auto& chain = Chain();
	if (!chain.on || !IS().on || !RE::ImageSpaceManager::GetSingleton())
		return;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	const auto final = FinalSize();
	constexpr std::uint64_t kRetryFrames = 120;
	const auto frame = Frame::Driver::Current().frame;
	if (chain.registered)
		LogReadback(frame);
	if (chain.registered && chain.retry && frame - chain.registeredFrame < kRetryFrames)
		return;
	if (chain.registered && !chain.retry && chain.width == main.Width && chain.height == main.Height && chain.finalWidth == final.first &&
		chain.finalHeight == final.second && chain.levels == Frame::PostChain::LuminanceLevels() && chain.ratios == DynamicRatios())
		return;
	RegisterChainSteps();
}

/**
 * @brief The chain's groups a description has on, from its post fields (Description::post, read at Begin): the composite
 * with the volumetric lighting, each slot in its place by its activity and usesLDR, the HDR effect or its copy, the last luminance
 * level's form by the adapted luminance, the bloom of the frame's radius, the tonemap or its fade form unless Effects11 owns it, TAA
 * by CS's upscaler method, the upsample by dynamic resolution, and the screenshot copy, refraction and both forms of the tonemap and of
 * the last luminance level always. Plus every group a hook asked for. The key (FrameAnchors::DescribingKey) is the same.
 */
std::array<bool, ChainGroup::kCount> ChainGroupsOn(const Frame::PostFields& a_post)
{
	const auto& chain = Chain();
	const auto& post = a_post;
	std::array<bool, ChainGroup::kCount> on{};
	for (std::size_t group = 0; group < on.size(); ++group)
		on[group] = RequestHolds(chain.requested[group]);
	on[ChainGroup::kComposite] |= post.volumetric;
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
	on[ChainGroup::kUpsample] |= post.dynres;
	// The screenshot copy is always on: a save asks for it with at most a frame of notice, and the flag may be set after Begin read it.
	// A late copy leaves a stale thumbnail. Its pass is gated: a frame that does not copy skips it at execution, its barriers folded.
	on[ChainGroup::kScreenshot] = true;
	// The frozen background's copy is always on: the capture frame is known only at its own Begin (a menu that freezes the background
	// opened), too late for a build or for a recording of a new description. Its pass is gated: every other frame skips it at execution,
	// its barriers folded.
	on[ChainGroup::kCapture] = true;
	// The radial blur after the tonemap (slot 5) is always on too: a menu that freezes the background starts its blur (the menu's
	// ImageSpace modifier) in the capture frame itself, and the frozen background keeps that one frame for as long as the menu is open.
	// A frame without it skips its pass and copy back at execution, their barriers folded.
	on[SlotGroup(5, post.ldr)] = true;
	// Refraction (slot 1) is always on: the engine turns its effect on during the frame, when it draws a refracting object, so Begin
	// cannot know. Its pass and copy back are gated: a frame without refraction skips both at execution, their barriers folded.
	on[SlotGroup(1, false)] = true;
	// Both tonemap forms (the fade starts and ends on a frame) and both forms of the last luminance level (the adaptation's reset lasts
	// one frame) are on, in the key and in every frame: the engine picks one late, and the other skips at execution, its barriers folded.
	// TAA's history pair is chosen per frame (ChainRows).
	on[ChainGroup::kHdrLum] |= post.hdr;
	on[ChainGroup::kHdrAdapt] |= post.hdr;
	on[ChainGroup::kTonemap] |= post.hdr && !post.effects11;
	on[ChainGroup::kTonemapFade] |= post.hdr && !post.effects11;
	return on;
}

/**
 * @brief Whether part a_part's description takes the routed forms (ChainRoute). A key's: when the frame has TAA, the HDR effect and CS's
 * tonemap. A frame's: the routing its build holds; while a build holds both (the key kept the other for its use window), the frame's own.
 */
bool ChainRouted(std::size_t a_part, const Frame::PostFields& a_post, bool a_key)
{
	const bool wanted = a_post.taa && a_post.hdr && !a_post.effects11;
	if (a_key)
		return wanted;
	const auto& part = Chain().parts[a_part];
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

/**
 * @brief The chain's passes this frame's description has on (ChainGroupsOn), as the rows of part a_part. Variants the engine picks per
 * frame (the parities) are all on; TAA's history is CS's pair by the frame slot's parity, so only that history's passes.
 */
void ChainRows(std::size_t a_part, std::array<std::uint64_t, 4>& a_rows)
{
	const auto& chain = Chain();
	const auto& frame = Frame::Driver::Current();
	const bool key = FrameAnchors::DescribingKey();
	const auto on = ChainGroupsOn(frame.post);
	const auto& part = chain.parts[a_part];
	const bool routed = ChainRouted(a_part, frame.post, key);
	for (std::uint32_t entry = 0; entry < part.count; ++entry) {
		const auto group = part.group[entry];
		if (!on[group])
			continue;
		if (part.route[entry] != ChainRoute::kAny && (part.route[entry] == ChainRoute::kRouted) != routed)
			continue;
		// TAA's history pair is CS's, by the frame slot's parity: the slot's frames write one history and read the other. Its copy back
		// serves both.
		if (group == ChainGroup::kTaa && !key && !part.copy[entry] && ((part.keys[entry].variant >> 15) & 1u) != (frame.parity & 1u))
			continue;
		const auto row = part.rowBase + entry;
		if (row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
	}
}

/**
 * @brief A CS target made with an engine target's size (its import's) and its render target view's format, for a plain texel copy
 * between the two. The final target has no texture in the renderer's table in native mode, only its view: this works for it too.
 */
bool MakeOwnedLike(OwnedTarget& a_out, RE::RENDER_TARGET a_like, std::string_view a_name, GraphResources::Lifetime a_lifetime)
{
	const auto index = static_cast<std::int32_t>(a_like);
	const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[index];
	const auto import = ImportOf(ImportOfName(index));
	if (!target.RTV || !import || !import.Width() || !import.Height())
		return false;
	D3D11_RENDER_TARGET_VIEW_DESC view{};
	target.RTV->GetDesc(&view);
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
	if (!bytes || bytes != rhi::FormatByteSize(import.Format())) {
		logger::warn("[ORG] ImageSpace: {}: target {}'s view format {} and its import's format {} differ in size; no copy between them", a_name, index,
			static_cast<std::uint32_t>(view.Format), static_cast<std::uint32_t>(import.Format()));
		return false;
	}
	auto made = Frame::Resources::Texture2D(a_name, desc, a_lifetime);
	a_out.texture = std::move(made.texture);
	a_out.fresh = made.fresh;
	a_out.format = RtvFormatOf(index, import);
	return static_cast<bool>(a_out);
}

/**
 * @brief The frozen background's copy program for kMENUBG's format (Upscaling's CopyHudlessPS: one texel loaded per pixel), built once per
 * format and kept. Render thread, at the chain's registration.
 */
std::shared_ptr<const RasterProgram> CaptureProgram(rhi::Format a_format)
{
	struct CopyBlock
	{
		ShaderBindings::Handle colorInput;
	};
	static std::map<rhi::Format, std::shared_ptr<const RasterProgram>> programs;
	if (const auto found = programs.find(a_format); found != programs.end())
		return found->second;
	auto* host = RenderGraphRuntime::Get().Host();
	if (!host)
		return nullptr;
	auto program = RasterProgram::Load(host->Device(),
		{ .pixelSource = "Upscaling/CopyHudlessPS.hlsl", .constantWords = ShaderBindings::Words<CopyBlock>(), .targets = { { .format = a_format } } });
	if (program)
		programs.emplace(a_format, program);
	return program;
}

/**
 * @brief Registers the chain's three clients (the composites at the post stage's first ImageSpace step; the slots before the
 * tonemap and the HDR effect up to its bloom at its second ImageSpace step, after the Upscale anchor; the tonemap and after at its
 * second) for the sizes now. Their passes are a fixed list in the engine's order, with the luminance level count fixed for this size.
 */
void RegisterChainSteps()
{
	auto& state = IS();
	auto& chain = Chain();
	chain.registered = false;
	chain.retry = false;
	chain.registeredFrame = Frame::Driver::Current().frame;
	chain.entries.clear();
	chain.requested = {};
	chain.captureEntry = kNoCopy;
	for (auto& part : chain.parts) {
		part.made.fill(false);
		part.copy.fill(false);
		part.copyBack.fill(kNoCopy);
		part.route.fill(ChainRoute::kAny);
		part.count = 0;
	}
	chain.throughEntry = kNoCopy;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	chain.width = main.Width;
	chain.height = main.Height;
	std::tie(chain.finalWidth, chain.finalHeight) = FinalSize();
	chain.ratios = DynamicRatios();
	chain.levels = Frame::PostChain::LuminanceLevels();
	using Lifetime = GraphResources::Lifetime;
	using T = RE::RENDER_TARGET;
	// The adapted luminance pair, made like the engine's last level of each parity.
	bool adapted = chain.levels > 0 && T::kHDR_DOWNSAMPLE1 + static_cast<std::int32_t>(chain.levels) + 1 <= T::kHDR_DOWNSAMPLE13;
	for (std::uint32_t p = 0; adapted && p < 2; ++p)
		adapted = MakeOwned(chain.adapt[p], static_cast<T>(T::kHDR_DOWNSAMPLE1 + static_cast<std::int32_t>(chain.levels) - 1 + 2 * static_cast<std::int32_t>(p)),
			kAdaptedIds[p], Lifetime::History);
	if (!adapted) {
		logger::warn("[ORG] ImageSpace: the adapted luminance pair could not be made for {} levels; registered again at a later frame", chain.levels);
		chain.retry = true;
		return;
	}
	// Published, so Drawcall Limit Fix's graphs can declare a read of it by id (the character light). A new pair rebuilds every graph once.
	for (const auto& target : chain.adapt)
		(void)PublishedResources::Publish(*target.texture);
	// The bloom is kept across frames: a frame whose bloom lags (its radius absent from the stage's build until the build with it switches
	// in) leaves it unwritten, and the tonemap then reads the last bloom drawn, not an unwritten transient.
	if (!MakeOwned(chain.bloom, T::kHDR_BLOOM, "ImageSpace::Bloom", Lifetime::History) ||
		!MakeOwned(chain.bloomSwap, T::kHDR_BLURSWAP, "ImageSpace::BloomSwap", Lifetime::Transient) ||
		!MakeOwned(chain.dofDown, T::kLDR_DOWNSAMPLE0, "ImageSpace::DofDownsample", Lifetime::Transient) ||
		!MakeOwned(chain.dofSwap, T::kLDR_BLURSWAP, "ImageSpace::DofBlurSwap", Lifetime::Transient) ||
		!MakeOwned(chain.taaHistory[0], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory0", Lifetime::History) ||
		!MakeOwned(chain.taaHistory[1], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory1", Lifetime::History) ||
		!MakeOwned(chain.getHitA, T::kGETHIT_BUFFER, "ImageSpace::GetHitA", Lifetime::Transient) ||
		!MakeOwned(chain.getHitB, T::kGETHIT_BUFFER, "ImageSpace::GetHitB", Lifetime::Transient) ||
		!MakeOwned(chain.getHitSwap, T::kGETHIT_BLURSWAP, "ImageSpace::GetHitSwap", Lifetime::Transient) ||
		!MakeOwned(chain.fullBlurDown, T::kHDR_BLURSWAP, "ImageSpace::FullBlurDown", Lifetime::Transient) ||
		!MakeOwned(chain.fullBlurSwap, T::kHDR_DOWNSAMPLE0, "ImageSpace::FullBlurSwap", Lifetime::Transient) ||
		!MakeOwnedLike(chain.hdrScratch, T::kMAIN, "ImageSpace::HdrScratch", Lifetime::Transient) ||
		!MakeOwnedLike(chain.ldrScratch, T::kFRAMEBUFFER, "ImageSpace::LdrScratch", Lifetime::Transient)) {
		logger::warn("[ORG] ImageSpace: the post chain's targets could not be made; registered again at a later frame");
		chain.retry = true;
		return;
	}

	using E = Effects;
	// The blur effects (effects[9] to [15], radius 3 to 15) draw ISBlurN with their HDR flag (+0x95), else ISNonHDRBlurN; none of them is
	// a bright pass (+0x94).
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
	constexpr std::int32_t kFinal = T::kFRAMEBUFFER;
	// The chain's two images, before the tonemap and after it, and the scratch an effect draws into before its copy back.
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
	// The copy back of a group's scratch into its image, after the group's draws that write the scratch.
	const auto copyBack = [&](std::uint8_t a_part, std::uint8_t a_group, bool a_ldr, std::string a_name) {
		Pending pending{ .part = a_part, .group = a_group, .key = { WorldScope::None, E::ISCopy, 0 }, .copy = true, .ldr = a_ldr };
		pending.desc.name = std::move(a_name);
		steps.push_back(std::move(pending));
	};

	// The pre-tonemap composite (99024 / 105675; the VL form, 100493): the volumetric lighting's target of this frame's parity blended
	// over kMAIN at the render size. The lens flare forms read the image-based lens flares' output, which has no CS pass.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("composite.vl.p{}", p), .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[0] = engine(T::kIMAGESPACE_VOLUMETRIC_LIGHTING + p);
		d.samplers[0] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeVolumetricLighting, p, std::move(d));
	}

	// A slot's single-draw effect: the image into the scratch target.
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
	// The depth of field with its compute blur (ImageSpaceEffectDepthOfField::Render, SE 0x1412f90d0; ISBlurCS's depth-of-field build,
	// 107734 <- 107209 <- 107208): the image downsampled into CS's target, blurred horizontally then vertically against the depth, and
	// one composite of the image with it (the depth of field or the distant blur, fogged or not) by the effect's step flags. The masked
	// composites read a mask CS does not have; the raster blur (the compute-blur option off) has no CS pass.
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
			d.inputs[3] = engine(T::kSAO_CAMERAZ, 10);  // the average depth: the camera depth's mip 10, as the engine binds it
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
	// One draw of a slot effect's inner chain (InnerVariant), its viewport its output's size.
	const auto inner = [&](std::uint8_t a_part, std::int32_t a_slot, bool a_ldr, E::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, bool a_blended,
						   StepDesc a_desc, bool a_final = false) {
		const auto variant = InnerVariant(a_slot, a_effect, a_occurrence, a_blended);
		add(a_part, SlotGroup(static_cast<std::uint32_t>(a_slot), a_ldr), a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, variant, std::move(a_desc),
			a_final);
	};
	// The get-hit effect (slot 7, InnerVariant): copy, two blurs through CS's buffers, then the double vision of the image and the blur
	// into the scratch target, and its copy back.
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
	// The full-screen blur (slot 6, InnerVariant), in place: downsample, the radius step's two blurs, and the upsample into the image,
	// blended below radius 1. The engine's copy of the input into the output before that blend is the image itself here: no pass.
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
			// Below radius 1 the engine blends with blend mode 1 (RendererShadowState's alpha blend) over the copy.
			StepDesc d{ .name = fmt::format("fullblur.up{}", blended ? ".blend" : ""), .output = image(a_ldr), .blend = PackBlend(blended ? 1 : 0, 0, 2, 0) };
			d.inputs[0] = owned(chain.fullBlurDown);
			d.samplers[0] = kLinear;
			inner(a_part, 6, a_ldr, E::ISHDRDownSample4, 1, blended, std::move(d));
		}
	};

	// The slots before the tonemap (part 1): the world map (both forms; t1 the main depth), refraction (t1 the refraction normals),
	// FXAA, the depth of field, the radial blur, the full-screen blur and the get-hit effect when usesLDR is off. Slot 4 has no effect.
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

	// The copy in the HDR effect's place (effects[34], when the HDR switch 513463 / 391301 is off): kMAIN into the final target.
	{
		StepDesc d{ .name = "hdr.copy", .output = engine(kFinal) };
		d.inputs[0] = engine(T::kMAIN);
		add(1, ChainGroup::kHdrCopy, WorldScope::HdrCopy, E::ISCopy, 0, std::move(d));
	}

	// The HDR effect (ImageSpaceEffectHDR::Render, SE 0x1412e6fe0): the scene downsampled into kHDR_DOWNSAMPLE0, then the luminance
	// levels, a quarter each, into kHDR_DOWNSAMPLE1 onwards; the last level into 27 + levels - 1 + 2 * parity, by the luminance shader
	// or, with last frame's adapted luminance (the other parity's target), by the light adaptation. The luminance targets stay the
	// engine's: Drawcall Limit Fix's character light reads them. Then the bloom of the frame's radius (effects[23 + radius]): the bright
	// pass from the downsample and the adapted luminance into CS's scratch, its blur into CS's bloom.
	constexpr std::int32_t kDownsample = T::kHDR_DOWNSAMPLE0, kFirstLevel = T::kHDR_DOWNSAMPLE1;
	{
		StepDesc d{ .name = "hdr.downsample", .output = engine(kDownsample) };
		d.inputs[0] = engine(T::kMAIN);
		d.samplers[0] = kLinear;
		add(1, ChainGroup::kHdr, WorldScope::Hdr, E::ISHDRDownSample4, 0, std::move(d));
	}
	const auto levels = chain.levels;
	const auto levelEffect = [](std::uint32_t a_level) { return a_level == 0 ? E::ISHDRDownSample4RGB2Lum : E::ISHDRDownSample16Lum; };
	const auto levelInput = [&](std::uint32_t a_level) { return a_level == 0 ? kDownsample : kFirstLevel + static_cast<std::int32_t>(a_level) - 1; };
	const auto lastTarget = [&](std::uint32_t a_parity) { return kFirstLevel + static_cast<std::int32_t>(levels) - 1 + 2 * static_cast<std::int32_t>(a_parity); };
	for (std::uint32_t level = 0; level + 1 < levels; ++level) {
		StepDesc d{ .name = fmt::format("hdr.luminance{}", level), .output = engine(kFirstLevel + static_cast<std::int32_t>(level)) };
		d.inputs[0] = engine(levelInput(level));
		d.samplers[0] = level == 0 ? kLinear : kPoint;
		add(1, ChainGroup::kHdr, WorldScope::Hdr, levelEffect(level), static_cast<std::uint16_t>(level), std::move(d));
	}
	if (levels > 0 && lastTarget(1) <= T::kHDR_DOWNSAMPLE13) {
		const auto lastLevel = levels - 1;
		for (std::uint16_t p = 0; p < 2; ++p) {
			const auto variant = static_cast<std::uint16_t>(lastLevel | (1u << 8) | (static_cast<std::uint32_t>(p) << 9));
			StepDesc l{ .name = fmt::format("hdr.luminance{}.p{}", lastLevel, p), .output = owned(chain.adapt[p]) };
			l.inputs[0] = engine(levelInput(lastLevel));
			l.samplers[0] = lastLevel == 0 ? kLinear : kPoint;
			add(1, ChainGroup::kHdrLum, WorldScope::Hdr, levelEffect(lastLevel), variant, std::move(l));
			StepDesc a{ .name = fmt::format("hdr.adapt.p{}", p), .output = owned(chain.adapt[p]) };
			a.inputs[0] = engine(levelInput(lastLevel));
			a.inputs[1] = owned(chain.adapt[1 - p]);
			a.samplers[0] = kLinear;
			add(1, ChainGroup::kHdrAdapt, WorldScope::Hdr, E::ISHDRDownSample16LightAdapt, variant, std::move(a));
		}
	}
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBright{ E::ISBrightPassBlur3, E::ISBrightPassBlur5, E::ISBrightPassBlur7, E::ISBrightPassBlur9,
		E::ISBrightPassBlur11, E::ISBrightPassBlur13, E::ISBrightPassBlur15 };
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlur{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
	for (std::uint8_t r = 0; r < 7; ++r) {
		const auto group = static_cast<std::uint8_t>(ChainGroup::kBloom + r);
		for (std::uint16_t p = 0; p < 2; ++p) {
			StepDesc b{ .name = fmt::format("bloom{}.bright.p{}", r, p), .output = owned(chain.bloomSwap) };
			b.inputs[0] = engine(kDownsample);
			b.inputs[1] = owned(chain.adapt[p]);
			add(1, group, WorldScope::Hdr, kBright[r], p, std::move(b));
		}
		StepDesc d{ .name = fmt::format("bloom{}.blur", r), .output = owned(chain.bloom) };
		d.inputs[0] = owned(chain.bloomSwap);
		add(1, group, WorldScope::Hdr, kBlur[r], 0, std::move(d));
	}

	// After the Tonemap anchor's step (part 2). The tonemap or its fade form: t0 the bloom, t1 kMAIN, t2 the adapted luminance of the
	// frame's parity; into the final target.
	// Routed (ChainRoute): into the LDR scratch, which TAA reads.
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
	// TAA (the helper at manager +0x1f0, 100495): the final target into the LDR scratch, with CS's history pair by the frame slot's parity
	// in place of kTEMPORAL_AA_ACCUMULATION_1/2, then the copy back. t2 the motion vectors, t3 the main depth, t4 the TAA mask, t5 the
	// normals target of the frame (kNORMAL_TAAMASK_SSRMASK or its swap, as Deferred bound it). The UI and water forms have no CS source.
	// Routed (ChainRoute): from the LDR scratch the routed tonemap wrote, into the final target, with no copy back.
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
		// The pass-through copy of a routed build: the LDR scratch into the final target, in a frame whose tonemap wrote the scratch and
		// that drew no TAA. On with the tonemap.
		Pending pending{ .part = 2, .group = ChainGroup::kTonemap, .key = { WorldScope::None, E::ISCopy, 2 }, .copy = true, .ldr = true, .through = true,
			.route = ChainRoute::kRouted };
		pending.desc.name = "taa.through";
		steps.push_back(std::move(pending));
	}
	// The save-game screenshot's copy (manager +0x211; effects[34] into kSAVE_GAME_SCREENSHOT at its own size): the final target as TAA
	// left it, before the slots after the tonemap, as the engine copies it.
	{
		StepDesc d{ .name = "screenshot", .output = engine(T::kSAVE_GAME_SCREENSHOT) };
		d.inputs[0] = engine(kFinal);
		add(2, ChainGroup::kScreenshot, WorldScope::Screenshot, E::ISCopy, 0, std::move(d));
	}
	// The slots after the tonemap (usesLDR): the depth of field, the radial blur, the full-screen blur and the get-hit effect.
	dof(2, true);
	radial(2, true);
	fullBlur(2, true);
	getHit(2, true);
	// The dynamic-resolution upsample (the helper at manager +0x1f8, 100548): the final target's render area into the LDR scratch at the
	// final size, then the copy back.
	{
		StepDesc d{ .name = "upsample", .output = scratch(true) };
		d.inputs[0] = engine(kFinal);
		d.samplers[0] = kLinear;
		add(2, ChainGroup::kUpsample, WorldScope::Upsample, E::ISUpsampleDynamicResolution, 0, std::move(d), true);
		copyBack(2, ChainGroup::kUpsample, true, "upsample.copyback");
	}
	// A frozen world's capture frame: the engine runs its end-of-frame effects into kMENUBG, which the menus composite over later frames
	// (MenuSteps.inl, the frozen composite). CS's chain ends in the final target; this copy puts the finished image into kMENUBG. Last,
	// as the image is final here. Opened at RenderEndOfFrameEffects' return in a capture frame (OnEndOfFrame).
	{
		Pending pending{ .part = 2, .group = ChainGroup::kCapture, .key = { WorldScope::None, E::ISCopy, 1 }, .capture = true };
		pending.desc.name = "capture.menubg";
		steps.push_back(std::move(pending));
	}

	// The three clients.
	std::uint32_t rowBase = 0;
	for (std::uint8_t p = 0; p < 3; ++p) {
		const auto partIndex = static_cast<std::size_t>(kChainParts[p]);
		auto& client = state.post[partIndex];
		auto& part = chain.parts[p];
		client.specs.clear();
		client.batch.clear();
		// The three clients share the post stage's rows, in order.
		part.rowBase = rowBase;
		std::vector<FrameAnchors::PassDesc> passes;
		// The bloom's first clear, before any pass reads it: only when the registration made it new (the same for the pairs below).
		if (p == 1 && chain.bloom.fresh) {
			AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -5);
			builder.Add(AnchorPasses::ClearOf("bloom.clear", *chain.bloom.texture), true);
			for (auto& pass : builder.Take())
				passes.push_back(std::move(pass));
		}
		// The adapted luminance pair's first clear, before any pass reads it.
		if (p == 1)
			for (std::uint32_t h = 0; h < 2; ++h) {
				if (!chain.adapt[h].fresh)
					continue;
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -3 - static_cast<std::int32_t>(h));
				builder.Add(AnchorPasses::ClearOf(fmt::format("hdr.adapted{}.clear", h), *chain.adapt[h].texture), true);
				for (auto& pass : builder.Take())
					passes.push_back(std::move(pass));
			}
		// TAA's history pair's first clear, before any pass reads it.
		if (p == 2)
			for (std::uint32_t h = 0; h < 2; ++h) {
				if (!chain.taaHistory[h].fresh)
					continue;
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -1 - static_cast<std::int32_t>(h));
				builder.Add(AnchorPasses::ClearOf(fmt::format("taa.history{}.clear", h), *chain.taaHistory[h].texture), true);
				for (auto& pass : builder.Take())
					passes.push_back(std::move(pass));
			}
		// The entries whose draws write the scratch target, until the copy back of their group gets its entry.
		std::vector<std::uint32_t> finals;
		for (auto& step : steps) {
			if (step.part != p)
				continue;
			const auto entry = part.count;
			if (entry >= kGateEntries || part.rowBase + entry >= 256) {
				logger::error("[ORG] ImageSpace: the post chain has more steps than a stage's rows hold; {} has no pass", step.desc.name);
				continue;
			}
			if (step.capture) {
				// A draw of the final target into kMENUBG at its own gate entry: a texel load per pixel, written in kMENUBG's format (the two
				// are the same size, their formats may differ). A session where the sizes differ or the program cannot be built has no copy
				// (logged).
				const auto source = ImportOf(ImportOfName(kFinal));
				const auto target = ImportOf(ImportOfName(T::kMENUBG));
				const auto program = source && target ? CaptureProgram(RtvFormatOf(T::kMENUBG, target)) : nullptr;
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
					.viewport = { .width = target.Width(), .height = target.Height() },
					.gate = entry });
				auto made = builder.Take();
				if (builder.Failed() || made.size() != 1) {
					logger::error("[ORG] ImageSpace: post step {} has no pass ({}); a frozen world's capture frame has no frozen background", step.desc.name,
						builder.Failure());
					continue;
				}
				state.importedObjects[T::kMENUBG] = EngineObjectOf(T::kMENUBG);
				++part.count;
				part.keys[entry] = step.key;
				part.group[entry] = step.group;
				part.copy[entry] = true;
				part.made[entry] = true;
				chain.captureEntry = static_cast<std::uint16_t>(entry);
				auto pass = std::move(made.front());
				pass.order = static_cast<std::int32_t>(entry);
				pass.row = static_cast<std::int16_t>(part.rowBase + entry);
				passes.push_back(std::move(pass));
				continue;
			}
			if (step.copy) {
				// A plain copy at its own gate entry: the draw that writes the scratch opens it with its own (TakeChainStep).
				const auto& from = step.ldr ? chain.ldrScratch : chain.hdrScratch;
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, static_cast<std::int32_t>(entry), client.latch, client.gate);
				builder.Add(AnchorPasses::Copy{ .name = step.desc.name,
					.source = AnchorPasses::TextureOf(*from.texture),
					.target = ImportOf(ImportOfName(step.ldr ? kFinal : T::kMAIN)),
					.width = from.texture->Desc().Width,
					.height = from.texture->Desc().Height,
					.gate = entry });
				auto made = builder.Take();
				if (builder.Failed() || made.size() != 1) {
					logger::error("[ORG] ImageSpace: post step {} has no pass ({}); the effects before it draw nothing", step.desc.name, builder.Failure());
					finals.clear();
					continue;
				}
				++part.count;
				part.keys[entry] = step.key;
				part.group[entry] = step.group;
				part.copy[entry] = true;
				part.made[entry] = true;
				part.route[entry] = step.route;
				if (step.through)
					chain.throughEntry = static_cast<std::uint16_t>(entry);
				else
					for (const auto writer : finals)
						if (part.group[writer] == step.group)
							part.copyBack[writer] = static_cast<std::uint16_t>(entry);
				finals.clear();
				auto pass = std::move(made.front());
				pass.order = static_cast<std::int32_t>(entry);
				pass.row = static_cast<std::int16_t>(part.rowBase + entry);
				passes.push_back(std::move(pass));
				continue;
			}
			std::string error;
			auto spec = MakeStepSpec(step.desc, entry, error);
			if (!spec) {
				logger::info("[ORG] ImageSpace: post step {} has no pass ({})", step.desc.name, error);
				continue;
			}
			if (chain.entries.contains(step.key))
				logger::error("[ORG] ImageSpace: post step {} has the key of an earlier step (variant {:#x}); the hook finds only the earlier one", step.desc.name,
					step.key.variant);
			++part.count;
			part.keys[entry] = step.key;
			part.group[entry] = step.group;
			part.route[entry] = step.route;
			chain.entries.try_emplace(step.key, ChainSteps::Entry{ p, entry });
			if (step.final)
				finals.push_back(entry);
			NoteImports(*spec);
			passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
				.anchor = Anchor::ImageSpace,
				.order = static_cast<std::int32_t>(entry),
				.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
				.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, entry, p] {
					auto pass = MakePass(spec, latch, gate, id);
					Chain().parts[p].made[entry] = pass != nullptr;
					if (!pass)
						Chain().retry = true;
					return pass;
				},
				// Recorded at execution with the CPU's gate answer: every step's hook has run by the stage's commit (RenderEndOfFrameEffects'
				// return), so a step the engine did not draw skips the run, and its barriers fold into the next pass that runs.
				.atExecution = true,
				.gated = true,
				.gpuGated = false,
				.row = static_cast<std::int16_t>(part.rowBase + entry) });
			client.specs.push_back(std::move(spec));
		}
		rowBase += part.count;
		FrameAnchors::ClientDesc desc{};
		desc.id = client.id;
		desc.passes = std::move(passes);
		desc.sharedConstants = true;
		desc.gates = { client.gate };
		// CS_ORG_ADAPT_READBACK: the readback's copy, last in the stage.
		if (p == 2 && ReadbackOn() && levels > 1) {
			auto& readback = Readback();
			if (!readback.buffer) {
				readback.buffer = org::Buffer::CreateShared(rhi::HeapType::Readback, AdaptReadback::kSlots * AdaptReadback::kEntries * AdaptReadback::kEntryBytes);
				if (readback.buffer)
					readback.buffer->SetName("ImageSpace::AdaptReadback");
			}
			const auto inputLevel = levelInput(levels - 1);
			static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> manager{ REL::RelocationID(524970, 411451) };
			const auto& input = manager.get()->renderTargetData[inputLevel];
			readback.sizes = { std::pair{ input.width, input.height }, std::pair{ chain.adapt[0].texture->Desc().Width, chain.adapt[0].texture->Desc().Height },
				std::pair{ chain.adapt[1].texture->Desc().Width, chain.adapt[1].texture->Desc().Height } };
			readback.formats = { RtvFormatOf(inputLevel, ImportOf(inputLevel)), chain.adapt[0].format, chain.adapt[1].format };
			std::array<FrameAnchors::Texture, AdaptReadback::kEntries> sources{ FrameAnchors::EngineTarget(static_cast<std::uint32_t>(inputLevel)), AnchorPasses::TextureOf(*chain.adapt[0].texture), AnchorPasses::TextureOf(*chain.adapt[1].texture) };
			bool fits = readback.buffer != nullptr;
			for (const auto& [width, height] : readback.sizes)
				fits = fits && width * 16 <= 256 && height * 256 <= AdaptReadback::kEntryBytes;
			if (fits) {
				for (auto& source : sources)
					if (!source)
						fits = false;
			}
			if (fits)
				desc.passes.push_back({ .name = fmt::format("{}.adapt-readback", client.id),
					.anchor = Anchor::ImageSpace,
					.order = 1 << 20,
					.kind = FrameAnchors::PassKind::Compute,
					.make = [sources] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ReadbackPass>(sources)); },
					.atExecution = true });
			else
				logger::warn("[ORG] ImageSpace: the adaptation readback does not fit its ring; it is off");
		}
		if (p == 1)
			// The targets kept across frames, by name. The transients are not: a registered resource gets a backing of its own in every
			// graph that holds the client, used or not, and their passes declare them directly.
			desc.registerResources = [](org::RenderGraph& a_graph) {
				const auto& current = Chain();
				for (const auto* target : { &current.bloom, &current.taaHistory[0], &current.taaHistory[1], &current.adapt[0], &current.adapt[1] })
					if (*target)
						target->texture->Register(a_graph);
			};
		desc.active = [partIndex] { return !IS().post[partIndex].batch.empty(); };
		desc.commit = [partIndex, p](const FrameAnchors::Point&) {
			auto& current = IS().post[partIndex];
			CommitBatch(current.batch, *current.latch, *current.gate);
			// CS_ORG_ADAPT_READBACK: the frame this run's copy belongs to.
			if (p == 2 && ReadbackOn() && Readback().buffer) {
				const auto frame = Frame::Driver::Current().frame;
				Readback().frames[frame % AdaptReadback::kSlots] = frame;
				Readback().runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), std::make_shared<const std::uint64_t>(frame));
			}
		};
		desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
		desc.declared = [] { return Frame::PostChain::On(); };
		desc.rows = [p](std::array<std::uint64_t, 4>& a_rows) { ChainRows(p, a_rows); };
		desc.repeats = true;
		desc.segmentOnly = true;
		desc.onFailed = [] {
			logger::error("[ORG] ImageSpace: the post chain's passes failed; the post chain draws nothing for the session");
			Chain().on = false;
		};
		if (!FrameAnchors::Register(std::move(desc))) {
			logger::error("[ORG] ImageSpace: the post chain client {} was refused", client.id);
			chain.on = false;
			return;
		}
	}
	chain.registered = true;
	logger::info("[ORG] ImageSpace: the post chain is CS passes: {} composite, {} pre-tonemap and {} post-tonemap passes ({} luminance levels), for {}x{} "
				 "into {}x{} at ratios {:.3f}x{:.3f}",
		chain.parts[0].count, chain.parts[1].count, chain.parts[2].count, chain.levels, chain.width, chain.height, chain.finalWidth, chain.finalHeight,
		chain.ratios.first, chain.ratios.second);
}

/** @brief Whether a covered effect's draw is a chain step now: inside the world chain's composites or end-of-frame effects. */
bool ChainStepNow()
{
	const auto& chain = Chain();
	return chain.on && chain.registered && (chain.inComposites || chain.inEndOfFrame);
}

/** @brief What a draw inside the world chain is to CS: a step with a pass, a draw CS's chain has nothing to do for, or unknown. */
enum class ChainDraw : std::uint8_t
{
	Unknown,
	Step,
	Nothing,
};

/**
 * @brief The key of a chain step's draw: its scope, effect and the variant drawn, from the engine's luminance parity global (the
 * luminance, adaptation, bright pass and tonemap), the luminance level so far, the volumetric lighting's parity, CS's TAA history pair
 * (the frame slot's parity) and the frame's normals target. No variant depends on a target: the chain's targets are fixed. a_group gets
 * the step's group (the hook asks for it when its pass is off).
 */
ChainDraw ChainKeyOf(Effects::ImageSpaceEffectEnum a_effect, StepKey& a_key, std::uint8_t& a_group)
{
	using E = Effects;
	auto& chain = Chain();
	if (chain.inComposites && !chain.inEndOfFrame) {
		if (a_effect != E::ISCompositeVolumetricLighting)
			return ChainDraw::Unknown;
		a_group = ChainGroup::kComposite;
		a_key = { WorldScope::Composite, a_effect, static_cast<std::uint16_t>(Frame::PostChain::VolumetricParity()) };
		return ChainDraw::Step;
	}
	if (!chain.inEndOfFrame)
		return ChainDraw::Unknown;
	if (chain.slotScope >= 0) {
		const auto slot = chain.slotScope;
		const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
		a_group = SlotGroup(static_cast<std::uint32_t>(slot), ldr);
		const auto occurrence = chain.slotDraws[a_effect]++;
		// In place, the full-screen blur's copy of its input into its output (below radius 1) is the image itself.
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
				const auto level = chain.lumLevel++;
				const bool isLast = level + 1 == chain.levels;
				a_group = isLast ? (a_effect == E::ISHDRDownSample16LightAdapt ? ChainGroup::kHdrAdapt : ChainGroup::kHdrLum) : ChainGroup::kHdr;
				a_key = { WorldScope::Hdr, a_effect, static_cast<std::uint16_t>(isLast ? (level | (1u << 8) | (static_cast<std::uint32_t>(parity) << 9)) : level) };
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
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kBright{ E::ISBrightPassBlur3, E::ISBrightPassBlur5, E::ISBrightPassBlur7, E::ISBrightPassBlur9,
			E::ISBrightPassBlur11, E::ISBrightPassBlur13, E::ISBrightPassBlur15 };
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlur{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
		for (std::uint8_t r = 0; r < 7; ++r) {
			if (a_effect == kBright[r] || a_effect == kBlur[r]) {
				a_group = static_cast<std::uint8_t>(ChainGroup::kBloom + r);
				a_key = { WorldScope::Hdr, a_effect, static_cast<std::uint16_t>(a_effect == kBright[r] ? parity : 0) };
				return ChainDraw::Step;
			}
		}
		return ChainDraw::Unknown;
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
 * @brief A chain step's data-only hook: the engine computed this step's constants and bound them; they are captured and staged for
 * the step's pass, matched by its scope, effect and variant (ChainKeyOf), and the step's copy back is opened with it. The engine's draw
 * is withheld (true). A draw CS has no pass for is a mismatch (NoPass). One whose group the frame's schedule has off draws nothing this
 * frame (the image stays as the step before it left it) and asks the next description for its group.
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
	// The TAA routing the frame's schedule holds (ChainRoute): the routed tonemap writes the LDR scratch, and routed TAA reads it.
	const auto heldOn = [&](const StepKey& a_key) {
		const auto at = chain.entries.find(a_key);
		if (at == chain.entries.end())
			return false;
		const auto row = chain.parts[at->second.part].rowBase + at->second.entry;
		const auto& held = Frame::Driver::Current().Segment(segment).rows;
		return row < 256 && (held[row / 64] >> (row % 64) & 1u) != 0;
	};
	if (key.scope == WorldScope::Tonemap) {
		auto routedKey = key;
		routedKey.variant |= ChainRoute::kTonemapBit;
		if (heldOn(routedKey)) {
			key = routedKey;
			chain.scratchWritten = true;
		}
	} else if (key.scope == WorldScope::Taa) {
		auto routedKey = key;
		routedKey.variant |= ChainRoute::kTaaBit;
		// Routed TAA reads the scratch the routed tonemap wrote this frame; without it the frame draws no TAA (lagged work).
		if (heldOn(routedKey)) {
			if (!chain.scratchWritten) {
				Frame::Driver::NoteLaggedWork(segment);
				return true;
			}
			key = routedKey;
			chain.taaRouted = true;
		}
	}
	const auto found = chain.entries.find(key);
	if (found == chain.entries.end()) {
		// Shortly after a registration (a new size or level count), the frame may still run on the old build while the engine draws for
		// the new sizes: lagged work, not a mismatch.
		constexpr std::uint64_t kRegistrationFrames = 300;
		if (Frame::Driver::Current().frame - chain.registeredFrame < kRegistrationFrames) {
			chain.requested[group] = Frame::Driver::Current().frame;
			Frame::Driver::NoteLaggedWork(segment);
			return true;
		}
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} (variant {:#x}) in the world post chain has no CS pass; it draws nothing", kEffects[a_effect].effectName, key.variant));
		return true;
	}
	const auto [p, entry] = found->second;
	auto& part = chain.parts[p];
	auto& client = state.post[static_cast<std::size_t>(kChainParts[p])];
	const auto& rows = Frame::Driver::Current().Segment(SegmentOfPart(p)).rows;
	const auto row = part.rowBase + entry;
	const bool scheduled = row < 256 && (rows[row / 64] >> (row % 64) & 1u) != 0 && FrameAnchors::CanTap(client.id);
	if (!part.made[entry] || !scheduled) {
		// The frame's schedule does not have it: nothing is drawn this frame, and the next description turns its group on.
		static std::array<std::uint64_t, ChainGroup::kCount> logged{};
		if (Frame::Driver::Current().frame - logged[group] >= 600 && (logged[group] = Frame::Driver::Current().frame) != 0)
			logger::info("[ORG] ImageSpace: post step {} (variant {:#x}, {}) lags at frame {}: made {}, row {} {}, client {}", kEffects[a_effect].effectName,
				key.variant, ChainGroupName(group), Frame::Driver::Current().frame, part.made[entry], row, row < 256 && (rows[row / 64] >> (row % 64) & 1u) ? "on" : "off",
				FrameAnchors::CanTap(client.id) ? "tappable" : "not tappable");
		chain.requested[group] = Frame::Driver::Current().frame;
		Frame::Driver::NoteStepLag(segment, client.id, fmt::format("post step {} (variant {:#x})", kEffects[a_effect].effectName, key.variant));
		return true;
	}
	// A group on by a hook's request stays on while the engine draws it (a field at Begin may not see it: the VL composite).
	if (RequestHolds(chain.requested[group]))
		chain.requested[group] = Frame::Driver::Current().frame;
	const auto staged = client.batch.size();
	const bool taken = StageStep(client, entry, a_effect, a_compute, a_groups);
	// The step wrote the scratch target: its copy back runs with it. A step whose constants could not be staged opens nothing.
	if (client.batch.size() > staged && part.copyBack[entry] != kNoCopy)
		client.batch.push_back({ .entry = part.copyBack[entry] });
	return taken;
}

/**
 * @brief At RenderEndOfFrameEffects' return: in a routed build's frame whose tonemap wrote the LDR scratch and that drew no TAA, the
 * pass-through copy moves the scratch into the final target (ChainRoute). Render owner.
 */
void OpenThrough()
{
	auto& chain = Chain();
	const bool written = std::exchange(chain.scratchWritten, false);
	const bool taa = std::exchange(chain.taaRouted, false);
	if (!written || taa || chain.throughEntry == kNoCopy)
		return;
	constexpr std::size_t p = 2;
	auto& part = chain.parts[p];
	auto& client = IS().post[static_cast<std::size_t>(kChainParts[p])];
	const auto entry = chain.throughEntry;
	const auto row = part.rowBase + entry;
	const auto& rows = Frame::Driver::Current().Segment(Frame::SegmentId::Post).rows;
	if (row >= 256 || !(rows[row / 64] >> (row % 64) & 1u) || !part.made[entry] || !FrameAnchors::CanTap(client.id)) {
		// The routed tonemap ran, so its build has the copy: a schedule without it is a mismatch.
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, Frame::SegmentId::Post,
			"the routed tonemap wrote the LDR scratch, and the pass-through copy is not in the frame's schedule");
		return;
	}
	client.batch.push_back({ .entry = entry });
	if (Usable())
		Flush();
}

/**
 * @brief A frozen world's capture frame, at RenderEndOfFrameEffects' return: the frozen background's copy (the final target into kMENUBG)
 * runs last in the post stage. Without the copy in the frame's schedule (a registration whose build has not switched in yet) the frozen
 * background is not written this time: lagged work, logged. Render owner.
 */
void OpenCapture()
{
	auto& chain = Chain();
	auto& state = IS();
	constexpr auto segment = Frame::SegmentId::Post;
	constexpr std::size_t p = 2;
	auto& part = chain.parts[p];
	auto& client = state.post[static_cast<std::size_t>(kChainParts[p])];
	const auto entry = chain.captureEntry;
	const auto& rows = Frame::Driver::Current().Segment(segment).rows;
	const auto row = entry == kNoCopy ? 256u : part.rowBase + entry;
	const bool scheduled = row < 256 && (rows[row / 64] >> (row % 64) & 1u) != 0 && part.made[entry] && FrameAnchors::CanTap(client.id);
	if (!scheduled) {
		logger::warn("[ORG] ImageSpace: the frozen background's copy is not in frame {}'s schedule (made {}, row {}); kMENUBG keeps its last image",
			Frame::Driver::Current().frame, entry != kNoCopy && part.made[entry], row);
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

/** @brief The pre-tonemap composites' hook: inside the world chain, their draws are chain steps. */
struct ChainCompositesGuard
{
	ChainCompositesGuard() : active(Chain().on && Frame::PostChain::InChain() && FrameAnchors::IsRenderOwner())
	{
		if (active)
			Chain().inComposites = true;
	}
	~ChainCompositesGuard()
	{
		if (!active)
			return;
		Chain().inComposites = false;
		// The composites' step is tapped here, before the PostProcessing and Upscale anchors' steps that follow it in the post stage. A
		// scope still open around the composites (so LeaveScope did not flush) would otherwise tap it after them, past its step.
		if (!IS().post[static_cast<std::size_t>(PostPart::Composites)].batch.empty())
			Flush();
	}
	bool active = false;
};

/** @brief The HDR effect's hook: inside the world chain's end-of-frame effects, its draws are the HDR steps. */
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

/**
 * @brief The full-screen blur's and the get-hit effect's Render hooks (their vtables' Render, ImageSpaceEffect slot 1): inside the world
 * chain's end-of-frame effects, their draws are the slot's inner steps (InnerVariant), counted per shader.
 */
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

/**
 * @brief The CS target that stands for the engine's adapted luminance target a_view is a view of (the last luminance level of either
 * parity), or empty when CS does not own the pair or a_view is another target. Render owner.
 */
std::string_view AdaptedLuminanceIdOf(const ID3D11ShaderResourceView* a_view)
{
	const auto& chain = Chain();
	if (!chain.on || !chain.registered || !chain.adapt[0] || !chain.adapt[1] || !a_view || chain.levels == 0)
		return {};
	auto* view = Tripwire::Unwrap(const_cast<ID3D11ShaderResourceView*>(a_view));
	if (!view)
		view = const_cast<ID3D11ShaderResourceView*>(a_view);
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	for (std::uint32_t p = 0; p < 2; ++p) {
		const auto index = RE::RENDER_TARGET::kHDR_DOWNSAMPLE1 + static_cast<std::int32_t>(chain.levels) - 1 + 2 * static_cast<std::int32_t>(p);
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
