// The menu and loading screen composites as CS passes, in graph mode. Included by ImageSpace.cpp after StartSteps.inl.
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

/**
* @brief The composites' build for a key. Part 0 is the menu client's steps. The frozen composite and the face tint are entry 0 of
* parts 1 and 2, their own clients.
*/
struct MenuBuild : StepBuild<MenuKey, 3>
{
	std::uint32_t width = 0, height = 0, finalWidth = 0, finalHeight = 0, levels = 0;
};

struct MenuSteps
{
	StepGroup<MenuBuild> group;
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
	if (!shape || !IS().on)
		return std::nullopt;
	return MenuKey{ a_values.Get(Frame::Vars::DisplaySize()), shape->luminanceLevels, shape->effects, shape->states };
}

void BuildMenuSteps(MenuBuild& a_build);

/**
* @brief Setup. The composites' clients, declared on their variables. The first revision registers them, so the build is
* ready for the main menu.
*/
void DeclareMenuSteps()
{
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::EngineImageSpace());
	Menu().group.Declare(kMenuParts, dependencies, [](const Frame::Inputs& a_in) { return MenuKeyOf(a_in); },
		[](MenuBuild& a_build) { BuildMenuSteps(a_build); });
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

/** @brief One composite step, before its spec is made. */
struct MenuStep
{
	StepKey key;
	StepDesc desc;
};

/** @brief The composites' steps for a_levels luminance levels, in the engine's order. Empty before the engine made kMAIN. */
std::vector<MenuStep> MenuStepList(std::uint32_t a_levels)
{
	std::vector<MenuStep> steps;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return steps;
	using E = Effects;
	using T = RE::RENDER_TARGET;
	const auto engine = [](std::int32_t a_target) { return StepTexture::Engine(a_target); };
	constexpr StepSampler kLinear{ 0, 1 };
	constexpr std::int32_t kSource = T::kMAIN, kFinal = T::kFRAMEBUFFER;
	const auto add = [&](WorldScope a_scope, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
		a_desc.effect = a_effect;
		steps.push_back({ { a_scope, a_effect, a_variant }, std::move(a_desc) });
	};
	// A menu scene's HDR effect on the engine's own targets, ending with the tonemap into the final target.
	const auto levels = a_levels;
	if (HdrLevelsFit(levels)) {
		{
			StepDesc d{ .name = "hdr.downsample", .output = engine(T::kHDR_DOWNSAMPLE0) };
			d.inputs[0] = engine(kSource);
			d.samplers[0] = kLinear;
			add(WorldScope::MenuHdr, E::ISHDRDownSample4, InOut(kSource, 0), std::move(d));
		}
		AddHdrSteps(levels,
			{ .adapted = { engine(HdrLastTarget(levels, 0)), engine(HdrLastTarget(levels, 1)) }, .bloomSwap = engine(T::kHDR_BLURSWAP), .bloom = engine(T::kHDR_BLOOM) },
			[&](std::uint8_t, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
				add(WorldScope::MenuHdr, a_effect, a_variant, std::move(a_desc));
			});
		for (const bool fade : { false, true })
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc d{ .name = fmt::format("tonemap{}.p{}", fade ? ".fade" : "", p), .output = engine(kFinal) };
				d.inputs[0] = engine(T::kHDR_BLOOM);
				d.inputs[1] = engine(kSource);
				d.inputs[2] = engine(HdrLastTarget(levels, p));
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
	return steps;
}

/**
* @brief Render thread, at each Begin. Wants the composites' programs once the engine has made their effects and blend states,
* which it does after CS's setup. So they build while the game starts, and the first revision with the composites finds them
* built. Once per new shape. Makes the specs only for their programs and keeps nothing.
*/
void WantMenuPrograms(const Frame::Vars::ImageSpaceShape& a_shape)
{
	if (!IS().on || a_shape.effects == 0)
		return;
	std::size_t made = 0;
	for (const auto& step : MenuStepList(a_shape.luminanceLevels)) {
		std::string error;
		made += MakeStepSpec(step.desc, 0, error) ? 1 : 0;
	}
	logger::info("[ORG] ImageSpace: the composites' programs are wanted for {} steps ({} luminance levels), before their first revision", made,
		a_shape.luminanceLevels);
}

/**
* @brief The UI stage's composite clients' registrations for a_build's key, in the engine's order. Their declarations register them
* when the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildMenuSteps(MenuBuild& a_build)
{
	auto& state = IS();
	const auto& table = a_build.table;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (!targets[RE::RENDER_TARGET::kMAIN].texture)
		return;
	D3D11_TEXTURE2D_DESC main{};
	targets[RE::RENDER_TARGET::kMAIN].texture->GetDesc(&main);
	a_build.width = main.Width;
	a_build.height = main.Height;
	std::tie(a_build.finalWidth, a_build.finalHeight) = FinalSize();
	a_build.levels = a_build.key.levels;
	auto steps = MenuStepList(a_build.levels);

	for (const auto part : kMenuParts)
		state.post[static_cast<std::size_t>(part)].batch.clear();
	std::array<std::vector<FrameAnchors::PassDesc>, 3> partPasses;
	for (auto& step : steps) {
		// The frozen composite and the face tint are entry 0 of their own clients. Other steps take the menu client's next entry.
		const auto p = static_cast<std::uint8_t>(step.key == FrozenKey() ? 1 : step.key == TintKey() ? 2 : 0);
		auto& owner = state.post[static_cast<std::size_t>(kMenuParts[p])];
		const auto entry = table->parts[p].count;
		if (entry >= kGateEntries || (p != 0 && entry != 0))
			continue;
		std::string error;
		auto spec = MakeStepSpec(step.desc, entry, error);
		if (!spec) {
			logger::info("[ORG] ImageSpace: menu step {} has no pass ({})", step.desc.name, error);
			continue;
		}
		AddStep(*table, p, step.key, spec);
		// Every UI stage hook runs before its commit, so a step the engine did not draw skips and folds its barriers.
		auto pass = StepPassDesc(table, p, entry, owner, spec);
		// The composites have no rows: every pass is on, and the engine opens the ones it draws.
		pass.row = -1;
		partPasses[p].push_back(std::move(pass));
	}
	const auto makeDesc = [](PostPart a_part, std::vector<FrameAnchors::PassDesc> a_passes) {
		auto desc = PartClientDesc(static_cast<std::size_t>(a_part), std::move(a_passes));
		// A frame's composites are several engine calls, each its own tap.
		desc.repeats = true;
		// A composite outside the stage windows is a mismatch.
		desc.segmentOnly = true;
		return desc;
	};
	// An empty client is no client. Its declaration then registers none.
	for (std::size_t part = 0; part < kMenuParts.size(); ++part)
		if (!partPasses[part].empty())
			a_build.descs[part] = makeDesc(kMenuParts[part], std::move(partPasses[part]));
	if (!a_build.descs[1])
		logger::info("[ORG] ImageSpace: the frozen composite has no pass; a menu over a frozen world draws no frozen frame");
	logger::info("[ORG] ImageSpace: the menus' and the loading screen's composites are CS passes: {} passes ({} luminance levels), {}x{} into {}x{}, built "
				 "for the revision starting now",
		table->parts[0].count, a_build.levels, a_build.width, a_build.height, a_build.finalWidth, a_build.finalHeight);
}

bool MenuStepNow(std::size_t a_effect)
{
	const auto& menu = Menu();
	// Before the group registers, a menu scene's draw is still a menu step: TakeMenuStep counts it as lag.
	return IS().on && (menu.inHdr || !menu.calls.empty() || kEffects[a_effect].id == Effects::ISAlphaBlend);
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
	std::string targets;
	for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
		auto* view = shadow.PSTexture[slot];
		// Default textures are loaded textures like the masks, so read them by heap index.
		const auto input = view ? InputOfView(view) : -1;
		const auto target = input >= 0 && !(input & kDefaultTextureInput) ? input : -1;
		if (target >= 0)
			targets += fmt::format("{}t{} {}", targets.empty() ? "" : ", ", slot, NameForLog(target));
		auto& textures = ::DCLF::GpuTextures::Get();
		auto binding = view && target < 0 ? textures.ResolveBinding(view, kImageSpaceTag) : ::DCLF::GpuTextures::Binding{};
		if (!binding)
			binding = textures.NullOf(::DCLF::GpuTextures::ViewKind::Texture2D);
		if (!binding)
			RenderGraph::Fatal("ImageSpace", "the zero Texture2D view could not be made");
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
* @brief Stages the step's constants, matched by effect and by a variant from the call's arguments and the luminance parity, in the
* build the frame runs. A draw with no pass, or with no open stage holding the client, is a mismatch. Before the stage's first
* build it is lag.
*/
bool TakeMenuStep(std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	using E = Effects;
	auto& state = IS();
	auto& menu = Menu();
	// The first revision registers the group; a menu scene can draw before it, such as the first loading screen's.
	if (!menu.group.Registered()) {
		Frame::Driver::NoteLaggedWork(Frame::SegmentId::Ui);
		return true;
	}
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
				// Count against the build the frame holds for the menu part, not the newest. With none held the draw is not drawn.
				const auto held = menu.group.Held(0);
				const auto level = menu.lumLevel++;
				if (!held) {
					Frame::Driver::NoteLaggedWork(Frame::SegmentId::Ui);
					return true;
				}
				const auto levels = held->levels;
				key = { WorldScope::MenuHdr, id, LuminanceVariant(level, levels, parity) };
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
				bool bright = false;
				if (BloomRadiusOf(id, bright) >= 0) {
					key = { WorldScope::MenuHdr, id, static_cast<std::uint16_t>(bright ? parity : 0) };
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
	const auto find = known ? menu.group.Find(key, Anchor::ImageSpace) : StepFind<MenuBuild>{ .found = menu.group.AheadOfNewest() ? StepFound::Behind : StepFound::NoPass };
	if (find.found == StepFound::Behind) {
		// The engine may draw for new sizes before the revision with their registration is adopted.
		Frame::Driver::NoteLaggedWork(Frame::SegmentId::Ui);
		return true;
	}
	if (find.found == StepFound::NoPass) {
		const auto& call = menu.calls.empty() ? MenuSteps::EffectCall{} : menu.calls.back();
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Count,
			fmt::format("{} (variant {:#x}){} outside the world chain has no CS pass; it draws nothing", kEffects[a_effect].effectName, key.variant,
				menu.inHdr ? " in a menu scene's HDR effect" : fmt::format(" (RenderEffect {} from rt{} into rt{})", call.effect, call.source, call.destination)));
		return true;
	}
	const bool isTint = find.at.part == 2;
	auto& client = state.post[static_cast<std::size_t>(menu.group.PartOf(find.at.part))];
	if (!find.place.InStage()) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NotDeclared, isTint ? Frame::SegmentId::Count : Frame::SegmentId::Ui,
			fmt::format("{} (variant {:#x}) of {} came while no open stage holds the client; it draws nothing", kEffects[a_effect].effectName, key.variant, client.id));
		return true;
	}
	const auto entry = find.at.entry;
	if (find.found == StepFound::NotHeld || !find.Part().Made(entry)) {
		// The build runs off the render thread, so nothing is drawn until the schedule holds it.
		if (menu.lagged++ == 0)
			logger::info("[ORG] ImageSpace: {} waits for the build with the menu composites; it draws nothing until then (counted)", kEffects[a_effect].effectName);
		return true;
	}
	const auto before = client.batch.size();
	StageStep(client, entry, find.Part().SpecOf(entry), a_effect, a_compute, a_groups);
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
	MenuEffectCall(std::uint32_t a_effect, std::int32_t a_source, std::int32_t a_destination) : active(IS().on && FrameAnchors::IsRenderOwner())
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
