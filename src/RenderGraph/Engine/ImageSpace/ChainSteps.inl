// The world post chain as CS passes, for native mode. Included by ImageSpace.cpp after WorldSteps.inl, whose step helpers it uses.
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
	constexpr std::uint8_t kCount = 31;
}

constexpr std::uint16_t kNoCopy = 0xffff;

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

struct ChainSteps
{
	bool on = false;          // native mode with the post stages
	bool registered = false;  // a registration was built
	std::optional<ChainKey> built;
	std::array<std::optional<FrameAnchors::ClientDesc>, 3> descs;
	std::array<Frame::DeclaredClient, 3> clients;
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
	std::int32_t final = RE::RENDER_TARGET::kFRAMEBUFFER;
	// The registration's HDR scene texture, null while HDR is off.
	ID3D11Texture2D* hdrTarget = nullptr;
	DXGI_FORMAT hdrFormat = DXGI_FORMAT_UNKNOWN;
	struct Entry
	{
		std::uint8_t part = 0;
		std::uint32_t entry = 0;
	};
	std::map<StepKey, Entry> entries;
	// The three parts share the post stage's rows, in order. made[] is set on the build thread.
	struct Part
	{
		std::array<StepKey, kGateEntries> keys{};
		std::array<std::uint8_t, kGateEntries> group{};
		std::array<bool, kGateEntries> made{};
		std::array<bool, kGateEntries> copy{};
		std::array<std::uint16_t, kGateEntries> copyBack{};
		std::array<std::uint8_t, kGateEntries> route{};
		std::uint32_t count = 0;
		std::uint32_t rowBase = 0;
	};
	std::array<Part, 3> parts;
	// Groups a hook found off, with the frame each was last asked for. A group stays on for kRequestFrames after a request. Render owner.
	std::array<std::uint64_t, ChainGroup::kCount> requested{};
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
	std::uint16_t captureEntry = kNoCopy;
	bool captureFrame = false;
	std::uint16_t throughEntry = kNoCopy;
	bool scratchWritten = false, taaRouted = false;
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

/** @brief Whether any of the group's rows are set in a stage whose graph holds its client. */
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
	default:
		return fmt::format("group {}", a_group);
	}
}

/**
* @brief Replays the engine's end-of-frame loop (SE 0x141297410) only to check it against CS's description.
* An active effect missing from the schedule is lagged work if the description or the current post fields have it, else a mismatch.
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
* @brief CS_ORG_ADAPT_READBACK=1 diagnostic. Copies the luminance chain's last level and both adapted targets into a readback ring.
* BeginFrame logs them five frames later, every 30th frame.
*/
struct AdaptReadback
{
	static constexpr std::uint32_t kSlots = 8;
	static constexpr std::uint32_t kEntries = 3;    // last input level, adapted parity 0, adapted parity 1
	static constexpr std::uint32_t kEntryBytes = 4096;
	std::shared_ptr<org::Buffer> buffer;
	std::array<std::uint64_t, kSlots> frames{};    // render owner
	RunSlots<std::uint64_t> runs;                   // stored by the final part's commit
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

/** @brief Recorded at execution, into the ring slot of its run's frame. */
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

float ReadTexel(const std::byte* a_texel, rhi::Format a_format)
{
	const auto half = [](std::uint16_t a_bits) { return DirectX::PackedVector::XMConvertHalfToFloat(a_bits); };
	switch (a_format) {
	case rhi::Format::R11G11B10_Float:
		{
			// R11G11B10 red channel: 6 mantissa bits, 5 exponent bits, no sign.
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

/** @brief 8 when BasicRHI does not know the format. */
std::uint32_t TexelBytes(rhi::Format a_format)
{
	const auto bytes = rhi::FormatByteSize(a_format);
	return bytes ? bytes : 8;
}

/** @brief Render owner. */
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
	if (!shape || !Chain().on)
		return std::nullopt;
	return ChainKey{ a_values.Get(Frame::Vars::DisplaySize()), a_values.Get(Frame::Vars::HdrOutputTarget()), shape->luminanceLevels, shape->effects,
		shape->states };
}

bool ChainLagging()
{
	return Lagging(Chain().built, [](const auto& a_values) { return ChainKeyOf(a_values); });
}

void BuildChainSteps(const ChainKey& a_key);

/** @brief The chain client a_part's registration for a_key. Builds the three clients' once per key. */
const std::optional<FrameAnchors::ClientDesc>* ChainDescFor(const ChainKey& a_key, std::size_t a_part)
{
	auto& chain = Chain();
	if (chain.built != a_key) {
		chain.built = a_key;
		BuildChainSteps(a_key);
	}
	return &chain.descs[a_part];
}

/** @brief Setup. The kept chain textures, the adapted pair published once, and the three clients declared on their variables. */
bool DeclareChainSteps()
{
	using T = RE::RENDER_TARGET;
	auto& chain = Chain();
	// Kept across frames so a frame whose bloom lags reads the last bloom, not an unwritten transient.
	if (!DeclareKept(chain.bloom, T::kHDR_BLOOM, "ImageSpace::Bloom") || !DeclareKept(chain.taaHistory[0], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory0") ||
		!DeclareKept(chain.taaHistory[1], T::kTEMPORAL_AA_ACCUMULATION_1, "ImageSpace::TAAHistory1"))
		return false;
	// The pair follows the luminance chain's last level. One texel like the first level while the levels have no pair.
	for (std::uint32_t parity = 0; parity < 2; ++parity)
		if (!DeclareKept(chain.adapt[parity], kAdaptedIds[parity], Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::EngineImageSpace()),
				[parity](const Frame::Inputs& a_in, D3D11_TEXTURE2D_DESC& a_desc) {
					const auto target = AdaptTargetOf(a_in.Get(Frame::Vars::EngineImageSpace()).luminanceLevels, parity);
					if (target >= 0)
						return OwnedDescLike(target, a_desc);
					if (!OwnedDescLike(T::kHDR_DOWNSAMPLE1, a_desc))
						return false;
					a_desc.Width = a_desc.Height = 1;
					return true;
				}))
			return false;
	// Published once, so DCLF's graphs read it by id. A new level count is a new version of each slot and publishes nothing.
	for (const auto& target : chain.adapt)
		if (!PublishedResources::Publish(*target.texture)) {
			logger::error("[ORG] ImageSpace: the adapted luminance pair could not be published");
			return false;
		}
	// The ratios are no dependency: the VL composite's render-size viewport is latched at each draw.
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::HdrOutputTarget(), Frame::Vars::EngineImageSpace());
	for (std::size_t part = 0; part < chain.clients.size(); ++part)
		chain.clients[part] = DeclareStepClient<ChainKey>(kChainParts[part], dependencies, [](const Frame::Inputs& a_in) { return ChainKeyOf(a_in); },
			[part](const ChainKey& a_key) { return ChainDescFor(a_key, part); });
	return std::ranges::all_of(chain.clients, [](const Frame::DeclaredClient& a_client) { return static_cast<bool>(a_client); });
}

/** @brief The chain groups a description has on, from its post fields plus every group a hook asked for. */
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

/** @brief Per-frame variants such as the parities are all on. TAA only gets the history its frame slot's parity selects. */
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
		stl::report_and_fail(fmt::format("Community Shaders: the ImageSpace texture {} is made like target {}, which has no import", a_name, index));
	D3D11_RENDER_TARGET_VIEW_DESC view{};
	if (index & kHdrSceneOutput)
		view.Format = Chain().hdrFormat;
	else if (auto* rtv = globals::game::renderer->GetRuntimeData().renderTargets[index].RTV)
		rtv->GetDesc(&view);
	else
		stl::report_and_fail(fmt::format("Community Shaders: the ImageSpace texture {} is made like target {}, which has no RTV", a_name, index));
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
		stl::report_and_fail(fmt::format("Community Shaders: ImageSpace's {}: target {}'s view format {} and its import's format {} differ in size; no copy between them",
			a_name, index, static_cast<std::uint32_t>(view.Format), static_cast<std::uint32_t>(import.Format())));
	MakeTransient(a_out, desc, a_name);
	a_out.format = RtvFormatOf(index, import);
}

/** @brief One texel load per pixel, built once per format. Render thread. */
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
* @brief The chain's three clients' registrations for a_key. The passes are a fixed list in the engine's order. Their declarations
* register them when the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildChainSteps(const ChainKey& a_key)
{
	auto& state = IS();
	auto& chain = Chain();
	chain.registered = false;
	chain.descs = {};
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

	// The VL composite at the render size. The lens flare forms read an output with no CS pass.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("composite.vl.p{}", p), .output = engine(T::kMAIN), .blend = PackBlend(5, 0, 2, 0), .area = StepArea::Render };
		d.inputs[0] = engine(T::kIMAGESPACE_VOLUMETRIC_LIGHTING + p);
		d.samplers[0] = kLinear;
		add(0, ChainGroup::kComposite, WorldScope::Composite, E::ISCompositeVolumetricLighting, p, std::move(d));
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
		auto& part = chain.parts[p];
		client.specs.clear();
		client.batch.clear();
		part.rowBase = rowBase;
		std::vector<FrameAnchors::PassDesc> passes;
		std::vector<KeptClear> clears;
		// Clear the kept textures' first versions before any pass reads them. Later versions are born cleared.
		// The pre-tonemap part writes bloom and the adapted pair before the tonemap reads them, so its commit opens their clears.
		if (p == 1) {
			ClearFirstVersion(chain.bloom, client.id, -5, "bloom.clear", passes, clears);
			for (std::uint32_t h = 0; h < 2; ++h)
				ClearFirstVersion(chain.adapt[h], client.id, -3 - static_cast<std::int32_t>(h), fmt::format("hdr.adapted{}.clear", h), passes, clears);
		}
		if (p == 2)
			for (std::uint32_t h = 0; h < 2; ++h)
				ClearFirstVersion(chain.taaHistory[h], client.id, -1 - static_cast<std::int32_t>(h), fmt::format("taa.history{}.clear", h), passes, clears);
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
				// A texel load per pixel, since the formats may differ. The session has no copy if the sizes differ.
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
				// The draw that writes the scratch opens this copy's gate entry.
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
			passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
				.anchor = Anchor::ImageSpace,
				.order = static_cast<std::int32_t>(entry),
				.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
				.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, entry, p] {
					auto pass = MakePass(spec, latch, gate, id);
					Chain().parts[p].made[entry] = pass != nullptr;
					return pass;
				},
				// Every step's hook has run by the commit, so a step the engine did not draw skips and folds its barriers.
				.atExecution = true,
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
		for (const auto& clear : clears)
			desc.gates.push_back(clear.gate);
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
			readback.sizes = { std::pair{ input.width, input.height }, std::pair{ chain.adapt[0].width, chain.adapt[0].height },
				std::pair{ chain.adapt[1].width, chain.adapt[1].height } };
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
			// Only the targets kept across frames are registered. A registered resource gets a backing in every graph that holds the client.
			desc.registerResources = [](org::RenderGraph& a_graph) {
				const auto& current = Chain();
				for (const auto* target : { &current.bloom, &current.taaHistory[0], &current.taaHistory[1], &current.adapt[0], &current.adapt[1] })
					if (*target)
						target->texture->Register(a_graph);
			};
		desc.active = [partIndex] { return !IS().post[partIndex].batch.empty(); };
		desc.commit = [partIndex, p, clears](const FrameAnchors::Point&) {
			auto& current = IS().post[partIndex];
			CommitBatch(current.batch, *current.latch, *current.gate);
			OpenKeptClears(clears);
			if (p == 2 && ReadbackOn() && Readback().buffer) {
				const auto frame = Frame::Driver::Current().frame;
				Readback().frames[frame % AdaptReadback::kSlots] = frame;
				Readback().runs.Store(RenderGraphRuntime::Get().Host()->CurrentRun(), std::make_shared<const std::uint64_t>(frame));
			}
		};
		desc.executed = [clears](const FrameAnchors::Point&) {
			IS().executed = true;
			EndKeptClears(clears);
		};
		desc.declared = [] { return Frame::PostChain::On(); };
		desc.rows = [p](std::array<std::uint64_t, 4>& a_rows) { ChainRows(p, a_rows); };
		desc.repeats = true;
		desc.segmentOnly = true;
		// An empty client is no client. Its declaration then registers none.
		if (!desc.passes.empty())
			chain.descs[p] = std::move(desc);
	}
	chain.registered = true;
	logger::info("[ORG] ImageSpace: the post chain is CS passes: {} composite, {} pre-tonemap and {} post-tonemap passes ({} luminance levels), for {}x{} "
				 "into {}x{} (render-size viewports latched per draw), registered for the revision starting now",
		chain.parts[0].count, chain.parts[1].count, chain.parts[2].count, chain.levels, chain.width, chain.height, chain.finalWidth, chain.finalHeight);
}

bool ChainStepNow()
{
	const auto& chain = Chain();
	return chain.on && chain.registered && (chain.inComposites || chain.inEndOfFrame);
}

enum class ChainDraw : std::uint8_t
{
	Unknown,
	Step,
	Nothing,
};

/** @brief A chain draw's key from the engine's parity globals and the frame state. No variant depends on a target. */
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
* @brief Captures the constants the engine bound and stages them for the step's pass, then withholds the engine's draw.
* A draw with no pass is a mismatch. A group that is off draws nothing and asks the next description for it.
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
	const auto heldOn = [&](const StepKey& a_key) {
		const auto at = chain.entries.find(a_key);
		if (at == chain.entries.end())
			return false;
		const auto row = chain.parts[at->second.part].rowBase + at->second.entry;
		const auto& held = Frame::Driver::Current().Segment(segment).rows;
		return row < 256 && (held[row / 64] >> (row % 64) & 1u) != 0;
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
	const auto found = chain.entries.find(key);
	if (found == chain.entries.end()) {
		// Until the revision with a new registration is adopted, the frame runs the old build while the engine draws for the new one.
		// That is lag, not a mismatch.
		if (ChainLagging()) {
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
		static std::array<std::uint64_t, ChainGroup::kCount> logged{};
		if (Frame::Driver::Current().frame - logged[group] >= 600 && (logged[group] = Frame::Driver::Current().frame) != 0)
			logger::info("[ORG] ImageSpace: post step {} (variant {:#x}, {}) lags at frame {}: made {}, row {} {}, client {}", kEffects[a_effect].effectName,
				key.variant, ChainGroupName(group), Frame::Driver::Current().frame, part.made[entry], row, row < 256 && (rows[row / 64] >> (row % 64) & 1u) ? "on" : "off",
				FrameAnchors::CanTap(client.id) ? "tappable" : "not tappable");
		chain.requested[group] = Frame::Driver::Current().frame;
		Frame::Driver::NoteStepLag(segment, client.id, fmt::format("post step {} (variant {:#x})", kEffects[a_effect].effectName, key.variant));
		return true;
	}
	// A requested group stays on while the engine draws it, since Begin's fields may miss it, as with the VL composite.
	if (RequestHolds(chain.requested[group]))
		chain.requested[group] = Frame::Driver::Current().frame;
	const auto staged = client.batch.size();
	const bool taken = StageStep(client, entry, a_effect, a_compute, a_groups);
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

/** @brief In a routed frame with no TAA, copies the scratch into the final target. Render owner. */
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
		// The routed tonemap ran, so its build has the copy. A schedule without it is a mismatch.
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
