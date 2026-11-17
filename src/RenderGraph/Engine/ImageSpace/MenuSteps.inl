// The menus' and the loading screen's composites as CS passes (native mode). Included by ImageSpace.cpp inside its anonymous
// namespace, after StartSteps.inl; it uses the step helpers of WorldSteps.inl (StepDesc, MakeStepSpec, StepKey, StageStep).
//
// Outside the world chain the engine draws a few ImageSpace effects of its own (SE decompiles of 51855, 35560 and 50995):
// - UI3DSceneManager::Render (51855 / 52727), a menu's 3D scene (the loading screen's model, the main menu's backdrop, the inventory's
//   item), calls RenderEndOfFrameEffects with kMAIN as the source and kFRAMEBUFFER as the final target when its light scheme has an
//   ImageSpace override: the HDR effect, its tonemap into the framebuffer;
// - otherwise, and after a menu that drew into kMAIN (UI's render, SE 0x140ebf510), a copy of kMAIN into the framebuffer;
// - Main::Render (35560 / 36559) composites a frozen world's frame (kMENUBG) into the framebuffer under a menu that freezes the
//   world (SE 0x1412e2550, the alpha write mode 1).
// Each is a CS pass, a step of the UI stage in every frame kind (FrameAnchors.cpp, kDeclaredInfo): the frozen composite first (its own client,
// cs.engine.imagespace.frozen), the scene's composite after the scene's colour (cs.engine.imagespace.menu), a world frame's menu scene (the
// inventory's item) included. Both clients are segment-only: the ImageSpace anchor's own graph leaves them out. The player's face tint at a
// load (ISAlphaBlend into kPLAYER_FACEGEN_TINT) is its own client, cs.engine.imagespace.facetint: it comes in a world frame outside every
// stage's window (the session's first world frame), so it keeps the ImageSpace anchor's own epoch until a stage has a place for it. A tap at the end of each engine
// call stages its constants into the stage's frame inputs; the stage records the passes at execution with the CPU's gate answers. The hook
// stages the constants the engine computed (StageStep); the variant comes from the call's own arguments (RenderEffect's source and
// destination, RenderEndOfFrameEffects' source and final target) and the engine's luminance parity. Nothing is read from the engine's device
// state.

/** @brief The menu composites' registration and the engine calls running now. */
struct MenuSteps
{
	bool on = false;          // native mode with CS's stages: the composites are CS passes
	bool registered = false;  // registered for the sizes below
	bool retry = false;       // a pass was left out at a build (an effect or a blend state not made yet): register again later
	std::uint64_t registeredFrame = 0;
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
	std::map<StepKey, std::uint32_t> entries;
	std::array<bool, kGateEntries> made{};
	std::uint32_t count = 0;
	bool frozenMade = false;  // the frozen composite's pass (its own client, entry 0) was made at a build
	bool tintMade = false;    // the face tint's pass (its own client, entry 0) was made at a build
	// A menu scene's end-of-frame call (not the world chain's), its HDR effect, and the luminance level so far in it.
	bool inEndOfFrame = false, inHdr = false;
	std::int32_t source = -1, final = -1;
	std::uint32_t lumLevel = 0;
	// ImageSpaceManager::RenderEffect calls running now (innermost last): the effect, its source and its destination.
	struct EffectCall
	{
		std::uint32_t effect = 0;
		std::int32_t source = -1, destination = -1;
	};
	std::vector<EffectCall> calls;
	std::uint64_t lagged = 0;  // draws withheld while the UI stage's schedule did not hold the client yet (its first build)
};

MenuSteps& Menu()
{
	static MenuSteps menu;
	return menu;
}

void RegisterMenuSteps();

/**
 * @brief Begin of a frame: the menu composites registered again when kMAIN's or the final target's size or the luminance level count
 * changed, or a build left a pass out. The build runs off the render thread. Render owner.
 */
void CheckMenuSteps()
{
	auto& menu = Menu();
	if (!menu.on || !IS().on || !RE::ImageSpaceManager::GetSingleton())
		return;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	const auto final = FinalSize();
	constexpr std::uint64_t kRetryFrames = 120;
	const auto frame = Frame::Driver::Current().frame;
	if (menu.registered && menu.retry && frame - menu.registeredFrame < kRetryFrames)
		return;
	if (menu.registered && !menu.retry && menu.width == main.Width && menu.height == main.Height && menu.finalWidth == final.first &&
		menu.finalHeight == final.second && menu.levels == Frame::PostChain::LuminanceLevels())
		return;
	RegisterMenuSteps();
}

/** @brief The frozen world's composite: its own client, first in the UI stage. */
constexpr StepKey FrozenKey()
{
	return { WorldScope::MenuCopy, Effects::ISCopy, InOut(RE::RENDER_TARGET::kMENUBG, RE::RENDER_TARGET::kFRAMEBUFFER) };
}

/** @brief The player's face tint: its own client, on the ImageSpace anchor's own epoch. */
constexpr StepKey TintKey()
{
	return { WorldScope::MenuCopy, Effects::ISAlphaBlend, 0 };
}

/** @brief Registers the menu composites' clients (the UI stage's steps) for the sizes now, their passes in the engine's order. */
void RegisterMenuSteps()
{
	auto& state = IS();
	auto& menu = Menu();
	menu.registered = false;
	menu.retry = false;
	menu.registeredFrame = Frame::Driver::Current().frame;
	menu.entries.clear();
	menu.made.fill(false);
	menu.count = 0;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	menu.width = main.Width;
	menu.height = main.Height;
	std::tie(menu.finalWidth, menu.finalHeight) = FinalSize();
	menu.levels = Frame::PostChain::LuminanceLevels();
	using E = Effects;
	using T = RE::RENDER_TARGET;
	const auto engine = [](std::int32_t a_target) { return StepTexture::Engine(a_target); };
	constexpr StepSampler kPoint{ 0, 0 }, kLinear{ 0, 1 };
	constexpr std::int32_t kSource = T::kMAIN, kFinal = T::kFRAMEBUFFER;
	struct Pending
	{
		StepKey key;
		StepDesc desc;
	};
	std::vector<Pending> steps;
	const auto add = [&](WorldScope a_scope, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
		a_desc.effect = a_effect;
		steps.push_back({ { a_scope, a_effect, a_variant }, std::move(a_desc) });
	};
	// The HDR effect of a menu scene's end-of-frame call (ImageSpaceEffectHDR::Render, SE 0x1412e6fe0), on the engine's targets: the
	// source downsampled into kHDR_DOWNSAMPLE0, the luminance levels into kHDR_DOWNSAMPLE1 onwards, the last level into
	// 27 + levels - 1 + 2 * parity (the luminance shader, or the light adaptation from the other parity's target), the bloom of the
	// scene's radius (the bright pass into kHDR_BLURSWAP, its blur into kHDR_BLOOM), then the tonemap or its fade form into the final
	// target. The menus' manifest shapes of earlier sessions had exactly these targets and samplers.
	constexpr std::int32_t kDownsample = T::kHDR_DOWNSAMPLE0, kFirstLevel = T::kHDR_DOWNSAMPLE1;
	const auto levels = menu.levels;
	const auto levelEffect = [](std::uint32_t a_level) { return a_level == 0 ? E::ISHDRDownSample4RGB2Lum : E::ISHDRDownSample16Lum; };
	const auto levelInput = [&](std::uint32_t a_level) { return a_level == 0 ? kDownsample : kFirstLevel + static_cast<std::int32_t>(a_level) - 1; };
	const auto lastTarget = [&](std::uint32_t a_parity) { return kFirstLevel + static_cast<std::int32_t>(levels) - 1 + 2 * static_cast<std::int32_t>(a_parity); };
	if (levels > 0 && lastTarget(1) <= T::kHDR_DOWNSAMPLE13) {
		{
			StepDesc d{ .name = "hdr.downsample", .output = engine(kDownsample) };
			d.inputs[0] = engine(kSource);
			d.samplers[0] = kLinear;
			add(WorldScope::MenuHdr, E::ISHDRDownSample4, InOut(kSource, 0), std::move(d));
		}
		for (std::uint32_t level = 0; level + 1 < levels; ++level) {
			StepDesc d{ .name = fmt::format("hdr.luminance{}", level), .output = engine(kFirstLevel + static_cast<std::int32_t>(level)) };
			d.inputs[0] = engine(levelInput(level));
			d.samplers[0] = level == 0 ? kLinear : kPoint;
			add(WorldScope::MenuHdr, levelEffect(level), static_cast<std::uint16_t>(level), std::move(d));
		}
		const auto lastLevel = levels - 1;
		for (std::uint16_t p = 0; p < 2; ++p) {
			const auto variant = static_cast<std::uint16_t>(lastLevel | (1u << 8) | (static_cast<std::uint32_t>(p) << 9));
			StepDesc l{ .name = fmt::format("hdr.luminance{}.p{}", lastLevel, p), .output = engine(lastTarget(p)) };
			l.inputs[0] = engine(levelInput(lastLevel));
			l.samplers[0] = lastLevel == 0 ? kLinear : kPoint;
			add(WorldScope::MenuHdr, levelEffect(lastLevel), variant, std::move(l));
			StepDesc a{ .name = fmt::format("hdr.adapt.p{}", p), .output = engine(lastTarget(p)) };
			a.inputs[0] = engine(levelInput(lastLevel));
			a.inputs[1] = engine(lastTarget(1 - p));
			a.samplers[0] = kLinear;
			add(WorldScope::MenuHdr, E::ISHDRDownSample16LightAdapt, variant, std::move(a));
		}
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kBright{ E::ISBrightPassBlur3, E::ISBrightPassBlur5, E::ISBrightPassBlur7, E::ISBrightPassBlur9,
			E::ISBrightPassBlur11, E::ISBrightPassBlur13, E::ISBrightPassBlur15 };
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlur{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
		for (std::uint8_t r = 0; r < 7; ++r) {
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc b{ .name = fmt::format("bloom{}.bright.p{}", r, p), .output = engine(T::kHDR_BLURSWAP) };
				b.inputs[0] = engine(kDownsample);
				b.inputs[1] = engine(lastTarget(p));
				add(WorldScope::MenuHdr, kBright[r], p, std::move(b));
			}
			StepDesc d{ .name = fmt::format("bloom{}.blur", r), .output = engine(T::kHDR_BLOOM) };
			d.inputs[0] = engine(T::kHDR_BLURSWAP);
			add(WorldScope::MenuHdr, kBlur[r], 0, std::move(d));
		}
		for (const bool fade : { false, true })
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc d{ .name = fmt::format("tonemap{}.p{}", fade ? ".fade" : "", p), .output = engine(kFinal) };
				d.inputs[0] = engine(T::kHDR_BLOOM);
				d.inputs[1] = engine(kSource);
				d.inputs[2] = engine(lastTarget(p));
				d.samplers[0] = kLinear;
				d.samplers[2] = kLinear;
				add(WorldScope::MenuHdr, fade ? E::ISHDRTonemapBlendCinematicFade : E::ISHDRTonemapBlendCinematic, static_cast<std::uint16_t>((p << 15) | InOut(kSource, kFinal)),
					std::move(d));
			}
	}
	// The copies into the framebuffer: kMAIN (a menu scene without an ImageSpace override, or a menu drawn into kMAIN) with every
	// channel written, and the frozen world's frame (kMENUBG) with the alpha write mode 1 (SE 0x1412e2550).
	{
		StepDesc d{ .name = "copy.main", .output = engine(kFinal) };
		d.inputs[0] = engine(T::kMAIN);
		add(WorldScope::MenuCopy, E::ISCopy, InOut(T::kMAIN, kFinal), std::move(d));
		StepDesc f{ .name = "copy.frozen", .output = engine(kFinal), .blend = PackBlend(0, 0, 1, 0) };
		f.inputs[0] = engine(T::kMENUBG);
		add(WorldScope::MenuCopy, E::ISCopy, InOut(T::kMENUBG, kFinal), std::move(f));
	}

	// The player's face tint (ISAlphaBlend into kPLAYER_FACEGEN_TINT, at a load): sixteen tint masks blended over grey, each a loaded
	// texture read by its heap index (a frame input).
	if (targets[T::kPLAYER_FACEGEN_TINT].texture) {
		StepDesc d{ .name = "facetint", .output = engine(T::kPLAYER_FACEGEN_TINT), .latched = 0xFFFF };
		add(WorldScope::MenuCopy, E::ISAlphaBlend, 0, std::move(d));
	}

	for (const auto part : { PostPart::Menu, PostPart::Frozen, PostPart::FaceTint }) {
		state.post[static_cast<std::size_t>(part)].specs.clear();
		state.post[static_cast<std::size_t>(part)].batch.clear();
	}
	menu.frozenMade = false;
	menu.tintMade = false;
	std::vector<FrameAnchors::PassDesc> passes, frozenPasses, tintPasses;
	for (auto& step : steps) {
		// The frozen composite and the face tint are their own clients' entry 0; every other step is the menu client's next entry.
		const bool isFrozen = step.key == FrozenKey();
		const bool isTint = step.key == TintKey();
		auto& owner = state.post[static_cast<std::size_t>(isFrozen ? PostPart::Frozen : isTint ? PostPart::FaceTint : PostPart::Menu)];
		const auto entry = isFrozen || isTint ? 0u : menu.count;
		if (entry >= kGateEntries)
			break;
		std::string error;
		auto spec = MakeStepSpec(step.desc, entry, error);
		if (!spec) {
			logger::info("[ORG] ImageSpace: menu step {} has no pass ({})", step.desc.name, error);
			continue;
		}
		if (!isFrozen && !isTint) {
			++menu.count;
			menu.entries[step.key] = entry;
		}
		NoteImports(*spec);
		(isFrozen ? frozenPasses : isTint ? tintPasses : passes)
			.push_back({ .name = fmt::format("{}.{}", owner.id, spec->name),
				.anchor = Anchor::ImageSpace,
				.order = static_cast<std::int32_t>(entry),
				.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
				.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(owner.latch), gate = std::shared_ptr<const RunGate>(owner.gate), id = owner.id, entry,
							isFrozen, isTint] {
					auto pass = MakePass(spec, latch, gate, id);
					if (isFrozen)
						Menu().frozenMade = pass != nullptr;
					else if (isTint)
						Menu().tintMade = pass != nullptr;
					else
						Menu().made[entry] = pass != nullptr;
					if (!pass)
						Menu().retry = true;
					return pass;
				},
				.atExecution = true,
				// Every hook of the UI stage runs before its commit (the present): recorded at execution with the CPU's gate answer, and a
				// step the engine did not draw skips and folds its barriers.
				.gpuGated = false });
		owner.specs.push_back(std::move(spec));
	}
	const auto makeDesc = [](PostPart a_part, std::vector<FrameAnchors::PassDesc> a_passes) {
		const auto part = static_cast<std::size_t>(a_part);
		auto& owner = IS().post[part];
		FrameAnchors::ClientDesc desc{};
		desc.id = owner.id;
		desc.passes = std::move(a_passes);
		desc.sharedConstants = true;
		desc.gates = { owner.gate };
		desc.active = [part] { return !IS().post[part].batch.empty(); };
		desc.commit = [part](const FrameAnchors::Point&) {
			auto& current = IS().post[part];
			CommitBatch(current.batch, *current.latch, *current.gate);
		};
		// A frame's composites are several engine calls (the HDR effect, a copy), a tap each, each opening its own entries.
		desc.repeats = true;
		// Passes of the UI stage only: a composite outside its window is a mismatch (TakeMenuStep). The face tint is the anchor's own.
		desc.segmentOnly = a_part != PostPart::FaceTint;
		desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
		desc.onFailed = [] {
			logger::error("[ORG] ImageSpace: the menu composites' passes failed; they draw nothing for the session");
			Menu().on = false;
		};
		return desc;
	};
	if (!FrameAnchors::Register(makeDesc(PostPart::Menu, std::move(passes)))) {
		logger::error("[ORG] ImageSpace: the menu composites' client was refused");
		menu.on = false;
		return;
	}
	if (!frozenPasses.empty() && !FrameAnchors::Register(makeDesc(PostPart::Frozen, std::move(frozenPasses))))
		logger::error("[ORG] ImageSpace: the frozen composite's client was refused; a menu over a frozen world draws no frozen frame");
	if (!tintPasses.empty() && !FrameAnchors::Register(makeDesc(PostPart::FaceTint, std::move(tintPasses))))
		logger::error("[ORG] ImageSpace: the face tint's client was refused; the player's face tint is not drawn");
	menu.registered = true;
	logger::info("[ORG] ImageSpace: the menus' and the loading screen's composites are CS passes: {} passes ({} luminance levels), {}x{} into {}x{}", menu.count,
		menu.levels, menu.width, menu.height, menu.finalWidth, menu.finalHeight);
}

/**
 * @brief Whether a covered effect's draw is a menu composite step now: inside a menu scene's HDR effect, a RenderEffect call, or the
 * face tint.
 */
bool MenuStepNow(std::size_t a_effect)
{
	const auto& menu = Menu();
	return menu.on && menu.registered && (menu.inHdr || !menu.calls.empty() || kEffects[a_effect].id == Effects::ISAlphaBlend);
}

/**
 * @brief The face tint's sixteen masks as frame inputs: each slot's loaded texture by its heap index, held with the run (a layer the engine
 * leaves unset reads its black default texture, a loaded texture too). A slot that holds an engine render target reads the null texture:
 * the graph has no declared read of it there. Counted and logged once.
 */
void StageTintMasks(Pending& a_pending)
{
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	auto owners = std::make_shared<std::vector<std::shared_ptr<const void>>>();
	SourcesAt sources{};
	constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::FrameSlot, 0);
	std::string targets;
	for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
		auto* view = shadow.PSTexture[slot];
		// The engine's default textures (an unset layer reads the black one) are loaded textures like the masks: read by heap index.
		const auto input = view ? InputOfView(view) : -1;
		const auto target = input >= 0 && !(input & kDefaultTextureInput) ? input : -1;
		if (target >= 0)
			targets += fmt::format("{}t{} {}", targets.empty() ? "" : ", ", slot, NameForLog(target));
		auto binding = view && target < 0 ? DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag) : DCLF::GpuTextures::Get().NullOf(DCLF::GpuTextures::ViewKind::Texture2D);
		if (binding.index == DCLF::GpuTextures::kInvalid)
			binding = DCLF::GpuTextures::Get().NullOf(DCLF::GpuTextures::ViewKind::Texture2D);
		sources[slot] = binding.index;
		if (binding.owner)
			owners->push_back(std::move(binding.owner));
	}
	if (!targets.empty()) {
		static std::uint64_t count = 0;
		if (count++ == 0)
			logger::info("[ORG] ImageSpace: the face tint's slots {} hold engine targets; they read the null texture (counted, logged once)", targets);
	}
	a_pending.sources = sources;
	a_pending.source = std::move(owners);
}

/**
 * @brief A menu composite step's data-only hook: the constants the engine computed are staged for the step's pass, matched by the
 * effect and the variant from the engine call's arguments and the luminance parity. The engine's draw is withheld (true). A draw CS has
 * no pass for is a mismatch (NoPass), and so is one while no open stage holds the client (NotDeclared): the UI stage holds the composites
 * in every frame kind. Before the stage's schedule holds the client (its first build), the draw is lag: nothing is drawn.
 */
bool TakeMenuStep(std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	using E = Effects;
	auto& state = IS();
	auto& menu = Menu();
	const auto id = kEffects[a_effect].id;
	StepKey key{};
	bool known = false;
	if (menu.inHdr) {
		const auto parity = static_cast<std::uint16_t>(Frame::PostChain::LuminanceParity());
		switch (id) {
		case E::ISHDRDownSample4:
			key = { WorldScope::MenuHdr, id, InOut(menu.source, 0) };
			known = true;
			break;
		case E::ISHDRDownSample4RGB2Lum:
		case E::ISHDRDownSample16Lum:
		case E::ISHDRDownSample16LightAdapt:
			{
				const auto level = menu.lumLevel++;
				const bool isLast = level + 1 == menu.levels;
				key = { WorldScope::MenuHdr, id, static_cast<std::uint16_t>(isLast ? (level | (1u << 8) | (static_cast<std::uint32_t>(parity) << 9)) : level) };
				known = true;
				break;
			}
		case E::ISHDRTonemapBlendCinematic:
		case E::ISHDRTonemapBlendCinematicFade:
			key = { WorldScope::MenuHdr, id, static_cast<std::uint16_t>((parity << 15) | InOut(menu.source, menu.final)) };
			known = true;
			break;
		default:
			{
				constexpr std::array<E::ImageSpaceEffectEnum, 7> kBright{ E::ISBrightPassBlur3, E::ISBrightPassBlur5, E::ISBrightPassBlur7, E::ISBrightPassBlur9,
					E::ISBrightPassBlur11, E::ISBrightPassBlur13, E::ISBrightPassBlur15 };
				for (const auto bright : kBright)
					if (id == bright) {
						key = { WorldScope::MenuHdr, id, parity };
						known = true;
					}
				constexpr std::array<E::ImageSpaceEffectEnum, 7> kBlur{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
				for (const auto blur : kBlur)
					if (id == blur) {
						key = { WorldScope::MenuHdr, id, 0 };
						known = true;
					}
				break;
			}
		}
	} else if (id == E::ISAlphaBlend) {
		key = { WorldScope::MenuCopy, id, 0 };
		known = true;
	} else if (!menu.calls.empty() && menu.calls.back().effect == static_cast<std::uint32_t>(RE::ImageSpaceManager::GetCurrentIndex(id))) {
		const auto& call = menu.calls.back();
		key = { WorldScope::MenuCopy, id, InOut(call.source, call.destination) };
		known = call.source >= 0 && call.destination >= 0;
	}
	// A frozen world's capture frame composites the frame it just drew (kMENUBG) into the final target. CS's post stage drew it into the
	// final target and copied that into kMENUBG (ChainSteps.inl, OpenCapture): the final target holds the image already. Nothing to draw.
	if (known && key == FrozenKey() && Frame::Driver::Current().kind == Frame::Kind::FrozenWorld && Frame::Driver::Current().capture)
		return true;
	const bool isFrozen = known && key == FrozenKey() && !IS().post[static_cast<std::size_t>(PostPart::Frozen)].specs.empty();
	const bool isTint = known && key == TintKey() && !IS().post[static_cast<std::size_t>(PostPart::FaceTint)].specs.empty();
	const auto found = known && !isFrozen && !isTint ? menu.entries.find(key) : menu.entries.end();
	if (!isFrozen && !isTint && found == menu.entries.end()) {
		const auto& call = menu.calls.empty() ? MenuSteps::EffectCall{} : menu.calls.back();
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Count,
			fmt::format("{} (variant {:#x}){} outside the world chain has no CS pass; it draws nothing", kEffects[a_effect].effectName, key.variant,
				menu.inHdr ? " in a menu scene's HDR effect" : fmt::format(" (RenderEffect {} from rt{} into rt{})", call.effect, call.source, call.destination)));
		return true;
	}
	const auto entry = isFrozen || isTint ? 0u : found->second;
	auto& client = state.post[static_cast<std::size_t>(isFrozen ? PostPart::Frozen : isTint ? PostPart::FaceTint : PostPart::Menu)];
	// The face tint: the ImageSpace anchor's own epoch draws it once its built graph holds the client.
	if (isTint) {
		if (!menu.tintMade || !FrameAnchors::CanRun(client.id, Anchor::ImageSpace)) {
			if (menu.lagged++ == 0)
				logger::info("[ORG] ImageSpace: {} waits for the build with the face tint; it draws nothing until then (counted)", kEffects[a_effect].effectName);
			return true;
		}
		const auto before = client.batch.size();
		StageStep(client, entry, a_effect, a_compute, a_groups);
		if (client.batch.size() > before)
			StageTintMasks(client.batch.back());
		return true;
	}
	const auto hold = FrameAnchors::HeldNow(client.id, Anchor::ImageSpace);
	if (!hold.held) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotDeclared, Frame::SegmentId::Ui,
			fmt::format("{} (variant {:#x}) of {} came while no open stage holds the client; it draws nothing", kEffects[a_effect].effectName, key.variant, client.id));
		return true;
	}
	if (!(isFrozen ? menu.frozenMade : menu.made[entry]) || !hold.scheduled) {
		// Before the stage's schedule holds the registration (the build runs off the render thread): nothing is drawn this frame.
		if (menu.lagged++ == 0)
			logger::info("[ORG] ImageSpace: {} waits for the build with the menu composites; it draws nothing until then (counted)", kEffects[a_effect].effectName);
		return true;
	}
	StageStep(client, entry, a_effect, a_compute, a_groups);
	return true;
}

/** @brief A menu scene's HDR effect: its draws are the menu composite's HDR steps. */
struct MenuHdrGuard
{
	MenuHdrGuard() : active(Menu().inEndOfFrame)
	{
		if (active) {
			Menu().inHdr = true;
			Menu().lumLevel = 0;
		}
	}
	~MenuHdrGuard()
	{
		if (active)
			Menu().inHdr = false;
	}
	bool active = false;
};

/** @brief ImageSpaceManager::RenderEffect's arguments, for the step key of the draw inside it (a copy outside the world chain). */
struct MenuEffectCall
{
	MenuEffectCall(std::uint32_t a_effect, std::int32_t a_source, std::int32_t a_destination) : active(Menu().on && FrameAnchors::IsRenderOwner())
	{
		if (active)
			Menu().calls.push_back({ a_effect, a_source, a_destination });
	}
	~MenuEffectCall()
	{
		if (active && !Menu().calls.empty())
			Menu().calls.pop_back();
	}
	bool active = false;
};
