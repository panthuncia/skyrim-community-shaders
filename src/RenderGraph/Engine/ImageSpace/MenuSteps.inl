// The menu and loading screen composites as CS passes, for native mode. Included by ImageSpace.cpp after StartSteps.inl.
//
// Outside the world chain the engine draws a few ImageSpace effects of its own:
// - a menu's 3D scene runs the HDR effect and its tonemap into the framebuffer when its light scheme has an ImageSpace override,
// - otherwise, and after a menu drawn into kMAIN, kMAIN is copied into the framebuffer,
// - under a menu that freezes the world, the frozen frame (kMENUBG) is composited into the framebuffer.
// Each is a step of the UI stage in every frame kind. The player's face tint at a load is a step of the start stage in a
// world frame and of the UI stage in a loading screen. Hooks only stage the engine's constants, and the stage records at execution.

/** @brief What the composites' registration is a function of. Their clients are declared on these variables (Frame/Declare.h). */
struct MenuKey
{
	Frame::Vars::Extent2D display{};
	std::uint32_t levels = 0, effects = 0, states = 0;
	friend bool operator==(const MenuKey&, const MenuKey&) = default;
};

/** @brief The composites' clients, in their declarations' order. */
constexpr std::array<PostPart, 3> kMenuParts{ PostPart::Menu, PostPart::Frozen, PostPart::FaceTint };

struct MenuSteps
{
	bool on = false;          // native mode with CS's stages
	bool registered = false;  // a registration was built
	std::optional<MenuKey> built;
	std::array<std::optional<FrameAnchors::ClientDesc>, 3> descs;
	std::array<Frame::DeclaredClient, 3> clients;
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
	std::map<StepKey, std::uint32_t> entries;
	std::array<bool, kGateEntries> made{};
	std::uint32_t count = 0;
	bool frozenMade = false;
	bool tintMade = false;
	// Inside a menu scene's end-of-frame call, not the world chain's.
	bool inEndOfFrame = false, inHdr = false;
	std::int32_t source = -1, final = -1;
	std::uint32_t lumLevel = 0;
	// Innermost last.
	struct EffectCall
	{
		std::uint32_t effect = 0;
		std::int32_t source = -1, destination = -1;
	};
	std::vector<EffectCall> calls;
	std::uint64_t lagged = 0;  // draws withheld before the UI stage's first build held the client
};

MenuSteps& Menu()
{
	static MenuSteps menu;
	return menu;
}

/** @brief The composites' key at a revision's values, or none while the engine is not ready for them. */
template <class Values>
std::optional<MenuKey> MenuKeyOf(const Values& a_values)
{
	const auto shape = ReadyShape(a_values);
	if (!shape || !Menu().on)
		return std::nullopt;
	return MenuKey{ a_values.Get(Frame::Vars::DisplaySize()), shape->luminanceLevels, shape->effects, shape->states };
}

bool MenuLagging()
{
	return Lagging(Menu().built, [](const auto& a_values) { return MenuKeyOf(a_values); });
}

void BuildMenuSteps(const MenuKey& a_key);

/** @brief The composite client a_part's registration for a_key. Builds the three clients' once per key. */
const std::optional<FrameAnchors::ClientDesc>* MenuDescFor(const MenuKey& a_key, std::size_t a_part)
{
	auto& menu = Menu();
	if (menu.built != a_key) {
		menu.built = a_key;
		BuildMenuSteps(a_key);
	}
	return &menu.descs[a_part];
}

/**
* @brief Setup. The composites' clients, declared on their variables. The first revision registers them, so the build is
* ready for the main menu. False, logged, on failure.
*/
bool DeclareMenuSteps()
{
	auto& menu = Menu();
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::EngineImageSpace());
	for (std::size_t part = 0; part < menu.clients.size(); ++part)
		menu.clients[part] = DeclareStepClient<MenuKey>(kMenuParts[part], dependencies, [](const Frame::Inputs& a_in) { return MenuKeyOf(a_in); },
			[part](const MenuKey& a_key) { return MenuDescFor(a_key, part); });
	return std::ranges::all_of(menu.clients, [](const Frame::DeclaredClient& a_client) { return static_cast<bool>(a_client); });
}

/** @brief Its own client, first in the UI stage. */
constexpr StepKey FrozenKey()
{
	return { WorldScope::MenuCopy, Effects::ISCopy, InOut(RE::RENDER_TARGET::kMENUBG, RE::RENDER_TARGET::kFRAMEBUFFER) };
}

/** @brief Its own client, a step of both the start stage and the UI stage. */
constexpr StepKey TintKey()
{
	return { WorldScope::MenuCopy, Effects::ISAlphaBlend, 0 };
}

/**
* @brief The UI stage's composite clients' registrations for a_key, in the engine's order. Their declarations register them when
* the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildMenuSteps(const MenuKey& a_key)
{
	auto& state = IS();
	auto& menu = Menu();
	menu.registered = false;
	menu.descs = {};
	menu.entries.clear();
	menu.made.fill(false);
	menu.count = 0;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	menu.width = main.Width;
	menu.height = main.Height;
	std::tie(menu.finalWidth, menu.finalHeight) = FinalSize();
	menu.levels = a_key.levels;
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
	// A menu scene's HDR effect on the engine's own targets, ending with the tonemap into the final target.
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
	// kMAIN with every channel written, and kMENUBG with alpha write mode 1 (SE 0x1412e2550).
	{
		StepDesc d{ .name = "copy.main", .output = engine(kFinal) };
		d.inputs[0] = engine(T::kMAIN);
		add(WorldScope::MenuCopy, E::ISCopy, InOut(T::kMAIN, kFinal), std::move(d));
		StepDesc f{ .name = "copy.frozen", .output = engine(kFinal), .blend = PackBlend(0, 0, 1, 0) };
		f.inputs[0] = engine(T::kMENUBG);
		add(WorldScope::MenuCopy, E::ISCopy, InOut(T::kMENUBG, kFinal), std::move(f));
	}

	// Sixteen loaded tint masks blended over grey, each read by heap index.
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
		// The frozen composite and the face tint are entry 0 of their own clients. Other steps take the menu client's next entry.
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
					return pass;
				},
				.atExecution = true,
				// Every UI stage hook runs before its commit, so a step the engine did not draw skips and folds its barriers.
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
		// A frame's composites are several engine calls, each its own tap.
		desc.repeats = true;
		// A composite outside the stage windows is a mismatch.
		desc.segmentOnly = true;
		desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
		return desc;
	};
	// An empty client is no client. Its declaration then registers none.
	std::array<std::vector<FrameAnchors::PassDesc>*, 3> partPasses{ &passes, &frozenPasses, &tintPasses };
	for (std::size_t part = 0; part < kMenuParts.size(); ++part)
		if (!partPasses[part]->empty())
			menu.descs[part] = makeDesc(kMenuParts[part], std::move(*partPasses[part]));
	if (!menu.descs[1])
		logger::info("[ORG] ImageSpace: the frozen composite has no pass; a menu over a frozen world draws no frozen frame");
	menu.registered = true;
	logger::info("[ORG] ImageSpace: the menus' and the loading screen's composites are CS passes: {} passes ({} luminance levels), {}x{} into {}x{}, registered "
				 "for the revision starting now",
		menu.count, menu.levels, menu.width, menu.height, menu.finalWidth, menu.finalHeight);
}

bool MenuStepNow(std::size_t a_effect)
{
	const auto& menu = Menu();
	return menu.on && menu.registered && (menu.inHdr || !menu.calls.empty() || kEffects[a_effect].id == Effects::ISAlphaBlend);
}

/**
* @brief Stages the sixteen masks by heap index, held with the run. An unset layer reads the black default texture.
* A slot holding an engine render target reads the null texture, since the graph has no declared read of it. Logged once.
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
		// Default textures are loaded textures like the masks, so read them by heap index.
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
* @brief Stages the step's constants, matched by effect and by a variant from the call's arguments and the luminance parity.
* A draw with no pass, or with no open stage holding the client, is a mismatch. Before the stage's first build it is lag.
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
	// In a capture frame the post stage already copied the final image into kMENUBG and left it in the final target. Nothing to draw.
	if (known && key == FrozenKey() && Frame::Driver::Current().kind == Frame::Kind::FrozenWorld && Frame::Driver::Current().capture)
		return true;
	const bool isFrozen = known && key == FrozenKey() && !IS().post[static_cast<std::size_t>(PostPart::Frozen)].specs.empty();
	const bool isTint = known && key == TintKey() && !IS().post[static_cast<std::size_t>(PostPart::FaceTint)].specs.empty();
	const auto found = known && !isFrozen && !isTint ? menu.entries.find(key) : menu.entries.end();
	if (!isFrozen && !isTint && found == menu.entries.end()) {
		// The engine may draw for new sizes before the revision with their registration is adopted.
		if (MenuLagging()) {
			Frame::Driver::NoteLaggedWork(Frame::SegmentId::Ui);
			return true;
		}
		const auto& call = menu.calls.empty() ? MenuSteps::EffectCall{} : menu.calls.back();
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Count,
			fmt::format("{} (variant {:#x}){} outside the world chain has no CS pass; it draws nothing", kEffects[a_effect].effectName, key.variant,
				menu.inHdr ? " in a menu scene's HDR effect" : fmt::format(" (RenderEffect {} from rt{} into rt{})", call.effect, call.source, call.destination)));
		return true;
	}
	const auto entry = isFrozen || isTint ? 0u : found->second;
	auto& client = state.post[static_cast<std::size_t>(isFrozen ? PostPart::Frozen : isTint ? PostPart::FaceTint : PostPart::Menu)];
	const auto hold = FrameAnchors::HeldNow(client.id, Anchor::ImageSpace);
	if (!hold.held) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotDeclared, isTint ? Frame::SegmentId::Count : Frame::SegmentId::Ui,
			fmt::format("{} (variant {:#x}) of {} came while no open stage holds the client; it draws nothing", kEffects[a_effect].effectName, key.variant, client.id));
		return true;
	}
	if (!(isFrozen ? menu.frozenMade : isTint ? menu.tintMade : menu.made[entry]) || !hold.scheduled) {
		// The build runs off the render thread, so nothing is drawn until the schedule holds it.
		if (menu.lagged++ == 0)
			logger::info("[ORG] ImageSpace: {} waits for the build with the menu composites; it draws nothing until then (counted)", kEffects[a_effect].effectName);
		return true;
	}
	const auto before = client.batch.size();
	StageStep(client, entry, a_effect, a_compute, a_groups);
	if (isTint && client.batch.size() > before)
		StageTintMasks(client.batch.back());
	return true;
}

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

/** @brief RenderEffect's arguments, for the key of a copy outside the world chain. */
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
