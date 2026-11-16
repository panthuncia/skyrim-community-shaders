// The world post chain as CS passes (native mode). Included by ImageSpace.cpp inside its anonymous namespace, after WorldSteps.inl,
// whose step helpers it uses (StepDesc, MakeStepSpec, StepKey, MakeOwned, SelectEntries, StageStep).
//
// ImageSpaceManager::RenderWorldPostProcess (SE 100430 / AE 107148) is drawn by passes CS declares here, in the engine's order: the
// pre-tonemap composite, then RenderEndOfFrameEffects (99023 / 105674): the slots before the tonemap, the HDR effect (100465 /
// 107183) or the copy in its place, its tonemap, TAA, the screenshot copy, the slots after the tonemap, the upsample. Each step is
// CS's port of the effect's shader with its inputs, outputs, samplers, blend and viewport written below, sized at the build. The
// engine's draw is withheld; its hook only stages the constants the engine computed for the step (TakeChainStep).
//
// Which effects a frame's chain holds is the description's (ChainRows at Begin). Which targets each step reads and writes is CS's:
// at RenderEndOfFrameEffects' entry CS plans the chain (PlanEndOfFrame) the way the engine's loop does, over the effects that are
// both active in the engine and on in the frame's schedule. An effect the engine draws while the schedule has it off is left out of
// the plan, so the steps around it still read what the step before them wrote; it shows a frame or two late.

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
	constexpr std::uint8_t kCount = 30;
}

/** @brief One step of RenderEndOfFrameEffects in this frame's plan: on, and the targets it reads and writes. */
struct PlannedStep
{
	bool on = false;
	bool ldr = false;  // a slot drawn after the tonemap
	std::int32_t in = -1;
	std::int32_t out = -1;
};

/**
 * @brief RenderEndOfFrameEffects' targets for this frame, as CS planned them at its entry (PlanEndOfFrame): the slots, the HDR effect
 * or its copy, TAA, the screenshot copy and the upsample.
 */
struct ChainPlan
{
	bool valid = false;
	std::int32_t source = -1, final = -1;
	std::array<PlannedStep, 8> slots{};
	PlannedStep hdr;
	bool hdrCopy = false;  // the copy draws in the HDR effect's place (the HDR switch is off)
	PlannedStep taa;
	PlannedStep screenshot;
	PlannedStep upsample;
};

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
	// Per part: each entry's key and group, whether the last build made its pass (set on the build thread), the entry count, and the
	// part's first row in the post stage's rows (the three parts share them, in order).
	struct Part
	{
		std::array<StepKey, kGateEntries> keys{};
		std::array<std::uint8_t, kGateEntries> group{};
		std::array<bool, kGateEntries> made{};
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
	// The adapted luminance pair (the last luminance level of each parity, in place of the engine's 27 + levels - 1 + 2 * parity): a
	// history, published (kAdaptedIds) for Drawcall Limit Fix's character light, which reads it in place of the engine's target.
	std::array<OwnedTarget, 2> adapt;
	// The blur shaders the engine's blur effects draw (ImageSpaceEffectBlur's HDR flag, +0x95, of effects[9 + radius index]), read at the
	// registration: true for ISBlurN, false for ISNonHDRBlurN.
	std::array<bool, 7> blurHdr{};
	// The running chain (the hooks): inside the composites, inside RenderEndOfFrameEffects, inside its HDR effect, the luminance
	// level so far there, whether the HDR copy drew, and the frame's plan.
	bool inComposites = false, inEndOfFrame = false, inHdr = false, copyDrawn = false;
	std::uint32_t lumLevel = 0;
	ChainPlan plan;
	// Inside the full-screen blur (slot 6) or the get-hit effect (slot 7) of RenderEndOfFrameEffects, -1 elsewhere; each effect's draws
	// so far there.
	std::int32_t slotScope = -1;
	std::map<Effects::ImageSpaceEffectEnum, std::uint8_t> slotDraws;
};

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

/** @brief A step variant's two targets: the one it reads and the one it writes (RE::RENDER_TARGET values below 256). */
constexpr std::uint16_t InOut(std::int32_t a_in, std::int32_t a_out)
{
	return static_cast<std::uint16_t>(((a_in & 0x7f) << 8) | (a_out & 0xff));
}

/**
 * @brief Plans RenderEndOfFrameEffects for this frame at its entry, as its loop does (SE 0x141297410): the slots before the tonemap
 * alternate between a_source and the manager's swap target (+0x60); the HDR effect writes a_final when nothing follows it, else
 * kIMAGESPACE_TEMP_COPY; TAA writes a_final or kIMAGESPACE_TEMP_COPY2; the slots after the tonemap alternate between those two and
 * the last writes a_final unless the upsample follows. "Nothing follows" counts TAA, the slots after the tonemap and dynamic
 * resolution (its lock 525027 / 411509 off and a scale, 525023 / 525024, not 1). An effect is in the plan when the engine has it
 * active and the frame's schedule has it on.
 */
void PlanEndOfFrame(std::int32_t a_source, std::int32_t a_final)
{
	auto& chain = Chain();
	auto& plan = chain.plan;
	plan = {};
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	if (!manager)
		return;
	const auto base = reinterpret_cast<std::uintptr_t>(manager);
	const auto member = [&](std::uintptr_t a_offset) { return reinterpret_cast<const std::byte*>(base + a_offset); };
	plan.valid = true;
	plan.source = a_source;
	plan.final = a_final;
	std::int32_t source = a_source;
	std::int32_t swap = *reinterpret_cast<const std::int32_t*>(member(0x60));
	const auto count = (std::min)(*reinterpret_cast<const std::int32_t*>(member(0x40)), 7);
	std::int32_t lastLdr = -1;
	for (std::int32_t slot = 0; slot <= count; ++slot) {
		auto* effect = slot < static_cast<std::int32_t>(manager->effects.free_idx()) ? manager->effects[static_cast<std::uint16_t>(slot)] : nullptr;
		if (!effect || !effect->IsActive())
			continue;
		const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
		if (!GroupScheduled(SlotGroup(static_cast<std::uint32_t>(slot), ldr)))
			continue;
		if (ldr) {
			lastLdr = slot;
			continue;
		}
		plan.slots[static_cast<std::size_t>(slot)] = { .on = true, .in = source, .out = swap };
		std::swap(source, swap);
	}
	static REL::Relocation<const std::int32_t*> dynamicLock{ REL::RelocationID(525027, 411509) };
	static REL::Relocation<const float*> scaleX{ REL::RelocationID(525023, 411505) };
	static REL::Relocation<const float*> scaleY{ REL::RelocationID(525024, 411506) };
	const bool dynres = *dynamicLock.get() == 0 && (*scaleX.get() != 1.0f || *scaleY.get() != 1.0f);
	const auto taaHelper = *reinterpret_cast<const std::uintptr_t*>(member(0x1f0));
	const bool taa = taaHelper && *reinterpret_cast<const bool*>(taaHelper + 0x18) && GroupScheduled(ChainGroup::kTaa);
	const bool anyLdr = lastLdr >= 0;
	const bool hdrFinal = !taa && !anyLdr && !dynres;
	constexpr std::int32_t kTemp = RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY;
	constexpr std::int32_t kTemp2 = RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2;
	static REL::Relocation<const bool*> hdrSwitch{ REL::RelocationID(513463, 391301) };
	plan.hdrCopy = !*hdrSwitch.get();
	plan.hdr = { .on = true, .in = source, .out = hdrFinal ? a_final : kTemp };
	if (hdrFinal)
		return;
	std::int32_t current = kTemp, next = kTemp2;
	if (taa) {
		const bool taaFinal = !anyLdr && !dynres;
		plan.taa = { .on = true, .in = kTemp, .out = taaFinal ? a_final : kTemp2 };
		if (taaFinal)
			next = a_final;
		else {
			next = kTemp;
			current = kTemp2;
		}
	}
	if (*reinterpret_cast<const bool*>(member(0x211)) && GroupScheduled(ChainGroup::kScreenshot))
		plan.screenshot = { .on = true, .in = next, .out = RE::RENDER_TARGET::kSAVE_GAME_SCREENSHOT };
	for (std::int32_t slot = 3; slot <= 7 && slot <= count; ++slot) {
		auto* effect = slot < static_cast<std::int32_t>(manager->effects.free_idx()) ? manager->effects[static_cast<std::uint16_t>(slot)] : nullptr;
		if (!effect || !effect->IsActive() || !SlotIsLdr(static_cast<std::uint32_t>(slot)) || !GroupScheduled(SlotGroup(static_cast<std::uint32_t>(slot), true)))
			continue;
		const bool isLast = slot == lastLdr && !dynres;
		const auto out = isLast ? a_final : next;
		plan.slots[static_cast<std::size_t>(slot)] = { .on = true, .ldr = true, .in = current, .out = out };
		if (!isLast) {
			next = current;
			current = out;
		}
	}
	const auto upsample = *reinterpret_cast<const std::uintptr_t*>(member(0x1f8));
	if (upsample && *reinterpret_cast<const bool*>(upsample + 0x08) && dynres && GroupScheduled(ChainGroup::kUpsample))
		plan.upsample = { .on = true, .in = current, .out = a_final };
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
 * occurrence of its shader in the slot's scope), the slot's input where that draw reads it, its output where it writes it, and whether
 * the full-screen blur's last draw blends.
 *
 * ImageSpaceEffectGetHit (Setup SE 0x141328b80): the input copied into a buffer (ISCopy), blurred twice by effects[10] (an
 * ImageSpaceEffectBlur of radius 5: a horizontal then a vertical draw each, through a second buffer and the blur swap), then the input
 * and the blurred buffer drawn into the output (ISDoubleVision). ImageSpaceEffectFullScreenBlur (Setup 0x141329df0, Render 0x14132a370):
 * the input downsampled (ISHDRDownSample4), blurred by the blur effect of the radius step (effects[9 + step]: horizontal, vertical),
 * then, below radius 1, the input copied into the output (ISCopy, or ISCopyDynamicFetchDisabled with Legacy Graphics Compatibility's
 * eleventh step); last the blurred image drawn into the output (ISHDRDownSample4 with the blur's parameters), blended over that copy
 * below radius 1.
 */
std::uint16_t InnerVariant(std::int32_t a_slot, Effects::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, std::int32_t a_in, std::int32_t a_out, bool a_blended)
{
	using E = Effects;
	bool readsIn = false, writesOut = false, blends = false;
	if (a_slot == 7) {
		readsIn = a_effect == E::ISCopy || a_effect == E::ISDoubleVision;
		writesOut = a_effect == E::ISDoubleVision;
	} else {
		const bool copy = a_effect == E::ISCopy || a_effect == E::ISCopyDynamicFetchDisabled;
		readsIn = (a_effect == E::ISHDRDownSample4 && a_occurrence == 0) || copy;
		writesOut = (a_effect == E::ISHDRDownSample4 && a_occurrence == 1) || copy;
		blends = a_blended && a_effect == E::ISHDRDownSample4 && a_occurrence == 1;
	}
	return static_cast<std::uint16_t>((blends ? 0x8000u : 0u) | (a_occurrence & 7u) << 12 | (readsIn ? static_cast<std::uint32_t>(a_in) & 0x3fu : 0u) << 6 |
									  (writesOut ? static_cast<std::uint32_t>(a_out) & 0x3fu : 0u));
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
 * @brief The chain's passes this frame's description has on, read at Begin from the post fields (Description::post): the composite
 * with the volumetric lighting, each slot in its place by its activity and usesLDR, the HDR effect or its copy, the last luminance
 * level's form by the adapted luminance, the bloom of the frame's radius, the tonemap or its fade form unless Effects11 owns it, TAA
 * by CS's upscaler method, the upsample by dynamic resolution. Variants the engine or CS's plan picks per frame (targets, parities)
 * are all on; TAA's history is CS's pair by the frame slot's parity, so only that history's passes. Plus every group a hook asked for.
 */
void ChainRows(std::size_t a_part, std::array<std::uint64_t, 4>& a_rows)
{
	const auto& chain = Chain();
	const auto& frame = Frame::Driver::Current();
	const auto& post = frame.post;
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
	on[ChainGroup::kScreenshot] |= post.screenshot;
	on[ChainGroup::kUpsample] |= post.dynres;
	// As a stage's key (FrameAnchors::DescribingKey), the passes that must answer on demand stay present: the screenshot copy (a save asks
	// for it with at most a frame of notice, and a late copy leaves a stale thumbnail), both tonemap forms (the fade starts and ends on a
	// frame) and both forms of the last luminance level (the adaptation's reset lasts one frame). TAA's history pair is chosen per frame.
	const bool key = FrameAnchors::DescribingKey();
	if (key) {
		on[ChainGroup::kScreenshot] = true;
		on[ChainGroup::kHdrLum] |= post.hdr;
		on[ChainGroup::kHdrAdapt] |= post.hdr;
		on[ChainGroup::kTonemap] |= post.hdr && !post.effects11;
		on[ChainGroup::kTonemapFade] |= post.hdr && !post.effects11;
	}
	const auto& part = chain.parts[a_part];
	for (std::uint32_t entry = 0; entry < part.count; ++entry) {
		const auto group = part.group[entry];
		if (!on[group])
			continue;
		// TAA's history pair is CS's, by the frame slot's parity: the slot's frames write one history and read the other.
		if (group == ChainGroup::kTaa && !key && ((part.keys[entry].variant >> 15) & 1u) != (frame.parity & 1u))
			continue;
		const auto row = part.rowBase + entry;
		if (row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
	}
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
	for (auto& part : chain.parts) {
		part.made.fill(false);
		part.count = 0;
	}
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
		!MakeOwned(chain.fullBlurSwap, T::kHDR_DOWNSAMPLE0, "ImageSpace::FullBlurSwap", Lifetime::Transient)) {
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
	constexpr std::int32_t kFinal = T::kFRAMEBUFFER, kTemp = T::kIMAGESPACE_TEMP_COPY, kTemp2 = T::kIMAGESPACE_TEMP_COPY2;
	// The targets the ping-pong can give a slot before the tonemap (kMAIN and kMAIN_COPY, either way round) and after it (the two
	// temporary copies, and the final target for the last).
	constexpr std::array<std::pair<std::int32_t, std::int32_t>, 2> kPre{ { { T::kMAIN, T::kMAIN_COPY }, { T::kMAIN_COPY, T::kMAIN } } };
	constexpr std::array<std::pair<std::int32_t, std::int32_t>, 4> kLdr{ { { kTemp, kTemp2 }, { kTemp2, kTemp }, { kTemp, kFinal }, { kTemp2, kFinal } } };

	struct Pending
	{
		std::uint8_t part = 0;
		std::uint8_t group = 0;
		StepKey key;
		StepDesc desc;
	};
	std::vector<Pending> steps;
	const auto add = [&](std::uint8_t a_part, std::uint8_t a_group, WorldScope a_scope, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
		a_desc.effect = a_effect;
		steps.push_back({ a_part, a_group, { a_scope, a_effect, a_variant }, std::move(a_desc) });
	};

	// The pre-tonemap composite (99024 / 105675; the VL form, 100493): the volumetric lighting's target of this frame's parity blended
	// over kMAIN at the render size. The lens flare forms read the image-based lens flares' output, which has no CS pass.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("composite.vl.p{}", p), .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[0] = engine(T::kIMAGESPACE_VOLUMETRIC_LIGHTING + p);
		d.samplers[0] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeVolumetricLighting, p, std::move(d));
	}

	// A slot's single-draw effect for each pair of targets.
	const auto single = [&](std::uint8_t a_part, std::uint32_t a_slot, bool a_ldr, E::ImageSpaceEffectEnum a_effect, const char* a_name, StepSampler a_sampler,
							StepTexture a_second = {}, StepSampler a_secondSampler = {}) {
		const auto scope = a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
		const auto add1 = [&](std::int32_t a_in, std::int32_t a_out) {
			StepDesc d{ .name = fmt::format("{}.{}.{}", a_name, a_in, a_out), .output = engine(a_out) };
			d.inputs[0] = engine(a_in);
			d.samplers[0] = a_sampler;
			if (a_second) {
				d.inputs[1] = a_second;
				d.samplers[1] = a_secondSampler;
			}
			add(a_part, SlotGroup(a_slot, a_ldr), scope, a_effect, InOut(a_in, a_out), std::move(d));
		};
		if (a_ldr)
			for (const auto& [in, out] : kLdr)
				add1(in, out);
		else
			for (const auto& [in, out] : kPre)
				add1(in, out);
	};
	// The depth of field with its compute blur (ImageSpaceEffectDepthOfField::Render, SE 0x1412f90d0; ISBlurCS's depth-of-field build,
	// 107734 <- 107209 <- 107208): the image downsampled into CS's target, blurred horizontally then vertically against the depth, and
	// one composite of the image with it (the depth of field or the distant blur, fogged or not) by the effect's step flags. The masked
	// composites read a mask CS does not have; the raster blur (the compute-blur option off) has no CS pass.
	const auto dof = [&](std::uint8_t a_part, bool a_ldr) {
		const auto scope = a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
		const auto group = SlotGroup(3, a_ldr);
		std::vector<std::int32_t> ins;
		if (a_ldr)
			ins = { kTemp, kTemp2 };
		else
			ins = { T::kMAIN, T::kMAIN_COPY };
		for (const auto in : ins) {
			StepDesc d{ .name = fmt::format("dof.downsample.{}", in), .output = owned(chain.dofDown) };
			d.inputs[0] = engine(in);
			d.samplers[0] = kLinear;
			add(a_part, group, scope, E::ISHDRDownSample4, InOut(in, 0), std::move(d));
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
		const auto composite = [&](std::int32_t a_in, std::int32_t a_out) {
			for (const auto effect : kComposites) {
				StepDesc d{ .name = fmt::format("dof.composite{}.{}.{}", static_cast<std::uint32_t>(effect), a_in, a_out), .output = engine(a_out) };
				d.inputs[0] = engine(a_in);
				d.inputs[1] = owned(chain.dofDown);
				d.inputs[2] = mainDepth;
				d.inputs[3] = engine(T::kSAO_CAMERAZ, 10);  // the average depth: the camera depth's mip 10, as the engine binds it
				d.samplers[1] = kLinear;
				add(a_part, group, scope, effect, InOut(a_in, a_out), std::move(d));
			}
		};
		if (a_ldr)
			for (const auto& [in, out] : kLdr)
				composite(in, out);
		else
			for (const auto& [in, out] : kPre)
				composite(in, out);
	};
	const auto radial = [&](std::uint8_t a_part, bool a_ldr) {
		single(a_part, 5, a_ldr, E::ISRadialBlur, "radialblur", kPoint);
		single(a_part, 5, a_ldr, E::ISRadialBlurMedium, "radialblur.medium", kPoint);
		single(a_part, 5, a_ldr, E::ISRadialBlurHigh, "radialblur.high", kPoint);
	};
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlurHdr{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
	constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlurLdr{ E::ISNonHDRBlur3, E::ISNonHDRBlur5, E::ISNonHDRBlur7, E::ISNonHDRBlur9, E::ISNonHDRBlur11,
		E::ISNonHDRBlur13, E::ISNonHDRBlur15 };
	const auto blurOf = [&](std::uint32_t a_radiusIndex) { return chain.blurHdr[a_radiusIndex] ? kBlurHdr[a_radiusIndex] : kBlurLdr[a_radiusIndex]; };
	// The slot's inputs, and its outputs, in its place.
	const auto slotIns = [&](bool a_ldr) { return a_ldr ? std::vector<std::int32_t>{ kTemp, kTemp2 } : std::vector<std::int32_t>{ T::kMAIN, T::kMAIN_COPY }; };
	const auto slotOuts = [&](bool a_ldr) {
		return a_ldr ? std::vector<std::int32_t>{ kTemp, kTemp2, kFinal } : std::vector<std::int32_t>{ T::kMAIN, T::kMAIN_COPY };
	};
	// One draw of a slot effect's inner chain (InnerVariant), its viewport its output's size.
	const auto inner = [&](std::uint8_t a_part, std::int32_t a_slot, bool a_ldr, E::ImageSpaceEffectEnum a_effect, std::uint32_t a_occurrence, std::int32_t a_in,
						   std::int32_t a_out, bool a_blended, StepDesc a_desc) {
		const auto variant = InnerVariant(a_slot, a_effect, a_occurrence, a_in, a_out, a_blended);
		a_desc.name = fmt::format("{}.{:x}", a_desc.name, variant);
		add(a_part, SlotGroup(static_cast<std::uint32_t>(a_slot), a_ldr), a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, variant, std::move(a_desc));
	};
	// The get-hit effect (slot 7, InnerVariant): copy, two blurs through CS's buffers, then the double vision of the input and the blur.
	const auto getHit = [&](std::uint8_t a_part, bool a_ldr) {
		for (const auto in : slotIns(a_ldr)) {
			StepDesc d{ .name = fmt::format("gethit.copy.{}", in), .output = owned(chain.getHitA) };
			d.inputs[0] = engine(in);
			inner(a_part, 7, a_ldr, E::ISCopy, 0, in, 0, false, std::move(d));
		}
		const auto blur = blurOf(1);
		const std::array<std::pair<const OwnedTarget*, const OwnedTarget*>, 4> kBlurs{ { { &chain.getHitA, &chain.getHitB }, { &chain.getHitB, &chain.getHitSwap },
			{ &chain.getHitSwap, &chain.getHitB }, { &chain.getHitB, &chain.getHitA } } };
		for (std::uint32_t k = 0; k < kBlurs.size(); ++k) {
			StepDesc d{ .name = fmt::format("gethit.blur{}", k), .output = owned(*kBlurs[k].second) };
			d.inputs[0] = owned(*kBlurs[k].first);
			inner(a_part, 7, a_ldr, blur, k, 0, 0, false, std::move(d));
		}
		for (const auto in : slotIns(a_ldr))
			for (const auto out : slotOuts(a_ldr)) {
				if (in == out)
					continue;
				StepDesc d{ .name = fmt::format("gethit.doublevision.{}.{}", in, out), .output = engine(out) };
				d.inputs[0] = engine(in);
				d.inputs[1] = owned(chain.getHitA);
				inner(a_part, 7, a_ldr, E::ISDoubleVision, 0, in, out, false, std::move(d));
			}
	};
	// The full-screen blur (slot 6, InnerVariant): downsample, the radius step's two blurs, the copy below radius 1, and the upsample.
	const auto fullBlur = [&](std::uint8_t a_part, bool a_ldr) {
		for (const auto in : slotIns(a_ldr)) {
			StepDesc d{ .name = fmt::format("fullblur.down.{}", in), .output = owned(chain.fullBlurDown) };
			d.inputs[0] = engine(in);
			d.samplers[0] = kLinear;
			inner(a_part, 6, a_ldr, E::ISHDRDownSample4, 0, in, 0, false, std::move(d));
		}
		for (std::uint32_t r = 0; r < 7; ++r) {
			StepDesc h{ .name = fmt::format("fullblur.blur{}.h", r), .output = owned(chain.fullBlurSwap) };
			h.inputs[0] = owned(chain.fullBlurDown);
			inner(a_part, 6, a_ldr, blurOf(r), 0, 0, 0, false, std::move(h));
			StepDesc v{ .name = fmt::format("fullblur.blur{}.v", r), .output = owned(chain.fullBlurDown) };
			v.inputs[0] = owned(chain.fullBlurSwap);
			inner(a_part, 6, a_ldr, blurOf(r), 1, 0, 0, false, std::move(v));
		}
		for (const auto in : slotIns(a_ldr))
			for (const auto out : slotOuts(a_ldr)) {
				if (in == out)
					continue;
				// Legacy Graphics Compatibility's 1.7 stage draws this copy with ISCopyDynamicFetchDisabled.
				for (const auto copy : { E::ISCopy, E::ISCopyDynamicFetchDisabled }) {
					StepDesc d{ .name = fmt::format("fullblur.copy{}.{}.{}", static_cast<std::uint32_t>(copy), in, out), .output = engine(out) };
					d.inputs[0] = engine(in);
					inner(a_part, 6, a_ldr, copy, 0, in, out, false, std::move(d));
				}
			}
		for (const auto out : slotOuts(a_ldr))
			for (const bool blended : { false, true }) {
				// Below radius 1 the engine blends with blend mode 1 (RendererShadowState's alpha blend) over the copy.
				StepDesc d{ .name = fmt::format("fullblur.up.{}{}", out, blended ? ".blend" : ""), .output = engine(out), .blend = PackBlend(blended ? 1 : 0, 0, 2, 0) };
				d.inputs[0] = owned(chain.fullBlurDown);
				d.samplers[0] = kLinear;
				inner(a_part, 6, a_ldr, E::ISHDRDownSample4, 1, 0, out, blended, std::move(d));
			}
	};

	// The slots before the tonemap (part 1): the world map (both forms; t1 the main depth), refraction (t1 the refraction normals),
	// FXAA, the depth of field and the radial blur when usesLDR is off. The full-screen blur, the get-hit effect and slot 4 have no CS
	// pass.
	single(1, 0, false, E::ISWorldMap, "worldmap", kLinear, mainDepth, kPoint);
	single(1, 0, false, E::ISWorldMapNoSkyBlur, "worldmap.nosky", kLinear, mainDepth, kPoint);
	single(1, 1, false, E::ISRefraction, "refraction", kLinear, engine(T::kREFRACTION_NORMALS), kPoint);
	single(1, 2, false, E::ISFXAA, "fxaa", kLinear);
	dof(1, false);
	radial(1, false);
	fullBlur(1, false);
	getHit(1, false);

	// The copy in the HDR effect's place (effects[34], when the HDR switch 513463 / 391301 is off).
	for (const auto in : { T::kMAIN, T::kMAIN_COPY })
		for (const auto out : { kFinal, kTemp }) {
			StepDesc d{ .name = fmt::format("hdr.copy.{}.{}", static_cast<std::int32_t>(in), out), .output = engine(out) };
			d.inputs[0] = engine(in);
			add(1, ChainGroup::kHdrCopy, WorldScope::HdrCopy, E::ISCopy, InOut(in, out), std::move(d));
		}

	// The HDR effect (ImageSpaceEffectHDR::Render, SE 0x1412e6fe0): the scene downsampled into kHDR_DOWNSAMPLE0, then the luminance
	// levels, a quarter each, into kHDR_DOWNSAMPLE1 onwards; the last level into 27 + levels - 1 + 2 * parity, by the luminance shader
	// or, with last frame's adapted luminance (the other parity's target), by the light adaptation. The luminance targets stay the
	// engine's: Drawcall Limit Fix's character light reads them. Then the bloom of the frame's radius (effects[23 + radius]): the bright
	// pass from the downsample and the adapted luminance into CS's scratch, its blur into CS's bloom.
	constexpr std::int32_t kDownsample = T::kHDR_DOWNSAMPLE0, kFirstLevel = T::kHDR_DOWNSAMPLE1;
	for (const auto in : { T::kMAIN, T::kMAIN_COPY }) {
		StepDesc d{ .name = fmt::format("hdr.downsample.{}", static_cast<std::int32_t>(in)), .output = engine(kDownsample) };
		d.inputs[0] = engine(in);
		d.samplers[0] = kLinear;
		add(1, ChainGroup::kHdr, WorldScope::Hdr, E::ISHDRDownSample4, InOut(in, 0), std::move(d));
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

	// After the Tonemap anchor's step (part 2). The tonemap or its fade form: t0 the bloom, t1 the scene, t2 the adapted luminance of
	// the frame's parity; into the final target or kIMAGESPACE_TEMP_COPY.
	for (const bool fade : { false, true })
		for (const auto in : { T::kMAIN, T::kMAIN_COPY })
			for (const auto out : { kFinal, kTemp })
				for (std::uint16_t p = 0; p < 2; ++p) {
					StepDesc d{ .name = fmt::format("tonemap{}.{}.{}.p{}", fade ? ".fade" : "", static_cast<std::int32_t>(in), out, p), .output = engine(out) };
					d.inputs[0] = owned(chain.bloom);
					d.inputs[1] = engine(in);
					d.inputs[2] = owned(chain.adapt[p]);
					d.samplers[0] = kLinear;
					d.samplers[2] = kLinear;
					add(2, fade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap, WorldScope::Tonemap, fade ? E::ISHDRTonemapBlendCinematicFade : E::ISHDRTonemapBlendCinematic,
						static_cast<std::uint16_t>((p << 15) | InOut(in, out)), std::move(d));
				}
	// TAA (the helper at manager +0x1f0, 100495): kIMAGESPACE_TEMP_COPY into the final target or kIMAGESPACE_TEMP_COPY2, with CS's
	// history pair by the frame slot's parity in place of kTEMPORAL_AA_ACCUMULATION_1/2. t2 the motion vectors, t3 the main depth, t4
	// the TAA mask, t5 the normals target of the frame (kNORMAL_TAAMASK_SSRMASK or its swap, as Deferred bound it). The UI and water
	// forms have no CS source.
	for (const auto out : { kFinal, kTemp2 })
		for (std::uint16_t h = 0; h < 2; ++h)
			for (std::uint16_t n = 0; n < 2; ++n) {
				StepDesc d{ .name = fmt::format("taa.{}.h{}.n{}", out, h, n), .output = engine(out), .output1 = owned(chain.taaHistory[h]) };
				d.inputs[0] = engine(kTemp);
				d.inputs[1] = owned(chain.taaHistory[1 - h]);
				d.inputs[2] = engine(T::kMOTION_VECTOR);
				d.inputs[3] = mainDepth;
				d.inputs[4] = engine(T::kTEMPORAL_AA_MASK);
				d.inputs[5] = engine(n ? T::kNORMAL_TAAMASK_SSRMASK_SWAP : T::kNORMAL_TAAMASK_SSRMASK);
				d.samplers[1] = kLinear;
				d.samplers[4] = kWrapPoint;
				d.samplers[5] = kWrapPoint;
				add(2, ChainGroup::kTaa, WorldScope::Taa, E::ISTemporalAA, static_cast<std::uint16_t>((h << 15) | (n << 14) | InOut(kTemp, out)), std::move(d));
			}
	// The save-game screenshot's copy (manager +0x211; effects[34] into kSAVE_GAME_SCREENSHOT at its own size).
	for (const auto in : { kFinal, kTemp, kTemp2 }) {
		StepDesc d{ .name = fmt::format("screenshot.{}", in), .output = engine(T::kSAVE_GAME_SCREENSHOT) };
		d.inputs[0] = engine(in);
		add(2, ChainGroup::kScreenshot, WorldScope::Screenshot, E::ISCopy, InOut(in, T::kSAVE_GAME_SCREENSHOT), std::move(d));
	}
	// The slots after the tonemap (usesLDR): the depth of field and the radial blur.
	dof(2, true);
	radial(2, true);
	fullBlur(2, true);
	getHit(2, true);
	// The dynamic-resolution upsample (the helper at manager +0x1f8, 100548) into the final target.
	for (const auto in : { kTemp, kTemp2 }) {
		StepDesc d{ .name = fmt::format("upsample.{}", in), .output = engine(kFinal) };
		d.inputs[0] = engine(in);
		d.samplers[0] = kLinear;
		add(2, ChainGroup::kUpsample, WorldScope::Upsample, E::ISUpsampleDynamicResolution, InOut(in, kFinal), std::move(d));
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
		// The bloom's first clear, before any pass reads it.
		if (p == 1) {
			AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -5);
			builder.Add(AnchorPasses::ClearOf("bloom.clear", *chain.bloom.texture), true);
			for (auto& pass : builder.Take())
				passes.push_back(std::move(pass));
		}
		// The adapted luminance pair's first clear, before any pass reads it.
		if (p == 1)
			for (std::uint32_t h = 0; h < 2; ++h) {
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -3 - static_cast<std::int32_t>(h));
				builder.Add(AnchorPasses::ClearOf(fmt::format("hdr.adapted{}.clear", h), *chain.adapt[h].texture), true);
				for (auto& pass : builder.Take())
					passes.push_back(std::move(pass));
			}
		// TAA's history pair's first clear, before any pass reads it.
		if (p == 2)
			for (std::uint32_t h = 0; h < 2; ++h) {
				AnchorPasses::Builder builder(Anchor::ImageSpace, client.id, -1 - static_cast<std::int32_t>(h));
				builder.Add(AnchorPasses::ClearOf(fmt::format("taa.history{}.clear", h), *chain.taaHistory[h].texture), true);
				for (auto& pass : builder.Take())
					passes.push_back(std::move(pass));
			}
		for (auto& step : steps) {
			if (step.part != p)
				continue;
			const auto entry = part.count;
			if (entry >= kGateEntries || part.rowBase + entry >= 256) {
				logger::error("[ORG] ImageSpace: the post chain has more steps than a stage's rows hold; {} has no pass", step.desc.name);
				continue;
			}
			std::string error;
			auto spec = MakeStepSpec(step.desc, entry, error);
			if (!spec) {
				logger::info("[ORG] ImageSpace: post step {} has no pass ({})", step.desc.name, error);
				continue;
			}
			++part.count;
			part.keys[entry] = step.key;
			part.group[entry] = step.group;
			chain.entries[step.key] = { p, entry };
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
				.atExecution = true,
				.gated = true,
				.gpuGated = true,
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
			desc.registerResources = [](org::RenderGraph& a_graph) {
				const auto& current = Chain();
				for (const auto* target : { &current.bloom, &current.bloomSwap, &current.dofDown, &current.dofSwap, &current.taaHistory[0], &current.taaHistory[1],
					 &current.getHitA, &current.getHitB, &current.getHitSwap, &current.fullBlurDown, &current.fullBlurSwap, &current.adapt[0], &current.adapt[1] })
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

/**
 * @brief The key of a chain step's draw: its scope, effect and the variant drawn, from CS's plan (the targets), the engine's
 * luminance parity global (the luminance, adaptation, bright pass and tonemap), the luminance level so far, the volumetric lighting's
 * parity, CS's TAA history pair (the frame slot's parity) and the frame's normals target. a_group gets the step's group even when
 * the plan has it off (the hook then asks for it). False when the draw is no step CS knows.
 */
bool ChainKeyOf(Effects::ImageSpaceEffectEnum a_effect, StepKey& a_key, std::uint8_t& a_group, bool& a_planned)
{
	using E = Effects;
	auto& chain = Chain();
	const auto& plan = chain.plan;
	a_planned = true;
	if (chain.inComposites && !chain.inEndOfFrame) {
		if (a_effect != E::ISCompositeVolumetricLighting)
			return false;
		a_group = ChainGroup::kComposite;
		a_key = { WorldScope::Composite, a_effect, static_cast<std::uint16_t>(Frame::PostChain::VolumetricParity()) };
		return true;
	}
	if (!chain.inEndOfFrame || !plan.valid)
		return false;
	if (chain.slotScope >= 0) {
		const auto slot = chain.slotScope;
		const auto& step = plan.slots[static_cast<std::size_t>(slot)];
		const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
		a_group = SlotGroup(static_cast<std::uint32_t>(slot), ldr);
		a_planned = step.on;
		const auto occurrence = chain.slotDraws[a_effect]++;
		a_key = { ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, InnerVariant(slot, a_effect, occurrence, step.in, step.out, slot == 6 && FullBlurRadius() < 1.0f) };
		return true;
	}
	const auto parity = static_cast<std::uint16_t>(Frame::PostChain::LuminanceParity());
	if (chain.inHdr) {
		switch (a_effect) {
		case E::ISHDRDownSample4:
			a_group = ChainGroup::kHdr;
			a_key = { WorldScope::Hdr, a_effect, InOut(plan.hdr.in, 0) };
			return true;
		case E::ISHDRDownSample4RGB2Lum:
		case E::ISHDRDownSample16Lum:
		case E::ISHDRDownSample16LightAdapt:
			{
				const auto level = chain.lumLevel++;
				const bool isLast = level + 1 == chain.levels;
				a_group = isLast ? (a_effect == E::ISHDRDownSample16LightAdapt ? ChainGroup::kHdrAdapt : ChainGroup::kHdrLum) : ChainGroup::kHdr;
				a_key = { WorldScope::Hdr, a_effect, static_cast<std::uint16_t>(isLast ? (level | (1u << 8) | (static_cast<std::uint32_t>(parity) << 9)) : level) };
				return true;
			}
		case E::ISHDRTonemapBlendCinematic:
		case E::ISHDRTonemapBlendCinematicFade:
			a_group = a_effect == E::ISHDRTonemapBlendCinematicFade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap;
			a_key = { WorldScope::Tonemap, a_effect, static_cast<std::uint16_t>((parity << 15) | InOut(plan.hdr.in, plan.hdr.out)) };
			return true;
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
				return true;
			}
		}
		return false;
	}
	switch (a_effect) {
	case E::ISTemporalAA:
		{
			a_group = ChainGroup::kTaa;
			a_planned = plan.taa.on;
			const auto history = static_cast<std::uint16_t>(Frame::Driver::Current().parity & 1u);
			const auto normals = static_cast<std::uint16_t>(globals::deferred && globals::deferred->forwardRenderTargets[2] == RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_SWAP);
			a_key = { WorldScope::Taa, a_effect, static_cast<std::uint16_t>((history << 15) | (normals << 14) | InOut(plan.taa.in, plan.taa.out)) };
			return true;
		}
	case E::ISUpsampleDynamicResolution:
		a_group = ChainGroup::kUpsample;
		a_planned = plan.upsample.on;
		a_key = { WorldScope::Upsample, a_effect, InOut(plan.upsample.in, plan.upsample.out) };
		return true;
	case E::ISCopy:
		if (plan.hdrCopy && !chain.copyDrawn) {
			chain.copyDrawn = true;
			a_group = ChainGroup::kHdrCopy;
			a_key = { WorldScope::HdrCopy, a_effect, InOut(plan.hdr.in, plan.hdr.out) };
			return true;
		}
		a_group = ChainGroup::kScreenshot;
		a_planned = plan.screenshot.on;
		a_key = { WorldScope::Screenshot, a_effect, InOut(plan.screenshot.in, plan.screenshot.out) };
		return true;
	default:
		break;
	}
	const auto slot = SlotOfEffect(a_effect);
	if (slot < 0)
		return false;
	const auto& step = plan.slots[static_cast<std::size_t>(slot)];
	const bool ldr = SlotIsLdr(static_cast<std::uint32_t>(slot));
	a_group = SlotGroup(static_cast<std::uint32_t>(slot), ldr);
	a_planned = step.on;
	const auto scope = ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
	std::uint16_t variant = InOut(step.in, step.out);
	if (a_effect == E::ISHDRDownSample4)
		variant = InOut(step.in, 0);
	else if (a_effect == E::ISDepthOfFieldBlurHCS || a_effect == E::ISDepthOfFieldBlurVCS)
		variant = 0;
	a_key = { scope, a_effect, variant };
	return true;
}

/**
 * @brief A chain step's data-only hook: the engine computed this step's constants and bound them; they are captured and staged for
 * the step's pass, matched by its scope, effect and variant (ChainKeyOf). The engine's draw is withheld (true). A draw CS has no pass
 * for is a mismatch (NoPass). One whose group CS's plan or the frame's schedule has off draws nothing this frame and asks the next
 * description for its group.
 */
bool TakeChainStep(std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	auto& chain = Chain();
	const auto id = kEffects[a_effect].id;
	const auto segment = Frame::SegmentId::Post;
	StepKey key{};
	std::uint8_t group = 0;
	bool planned = true;
	if (!ChainKeyOf(id, key, group, planned)) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} in the world post chain{} has no CS pass; it draws nothing", kEffects[a_effect].effectName, chain.inHdr ? " (the HDR effect)" : ""));
		return true;
	}
	if (!planned) {
		// CS's plan left the step out: its group is off in the frame's schedule. Nothing is drawn; the next description has it on.
		chain.requested[group] = Frame::Driver::Current().frame;
		Frame::Driver::NoteLaggedWork(segment);
		return true;
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
		// The frame's schedule (or CS's plan, which follows it) does not have it: nothing is drawn this frame, and the next
		// description turns its group on.
		chain.requested[group] = Frame::Driver::Current().frame;
		Frame::Driver::NoteLaggedWork(segment);
		return true;
	}
	// A group on by a hook's request stays on while the engine draws it (a field at Begin may not see it: the VL composite).
	if (RequestHolds(chain.requested[group]))
		chain.requested[group] = Frame::Driver::Current().frame;
	return StageStep(client, entry, a_effect, a_compute, a_groups);
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
