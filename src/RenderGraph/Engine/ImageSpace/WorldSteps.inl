// The RenderWorld scopes as CS passes, wherever the deferred+water stage runs. Included by ImageSpace.cpp.
// ChainSteps.inl reuses its step helpers for the post chain.
//
// The reflections, SAO, snow pass and water blend are passes CS declares here from its ports of the effect shaders.
// The engine's draw is withheld. Its hook only stages the constants the engine computed (TakeWorldStep).

/** @brief Set by the scopes' hooks. */
enum class WorldScope : std::uint8_t
{
	None,
	Reflections,    // 100428 / 107146
	Ssao,           // RenderSSAO's first call (100520 / 107245)
	Snow,           // RenderSnowSSS (100537 / 107267), or the kMAIN copy into kSNOW_SWAP
	SsaoComposite,  // RenderSSAO's second call
	WaterBlend,
	// The world post chain's steps (ChainSteps.inl).
	Composite,   // 99024 / 105675
	PreSlot,     // 99023 / 105674
	HdrCopy,     // when HDR is off
	Hdr,         // downsample, luminance, adaptation, bright pass and blur
	Tonemap,
	Taa,
	Screenshot,
	LdrSlot,     // after the tonemap (usesLDR)
	Upsample,
	// The start stage's water effects (StartSteps.inl).
	WaterDisplacement,  // 31385 / 32176
	FlowCopy,           // 35561 / 36560
	// The menu and loading screen composites (MenuSteps.inl).
	MenuHdr,   // inside UI3DSceneManager::Render
	MenuCopy,  // ImageSpaceManager::RenderEffect with ISCopy
};

enum class StepArea : std::uint8_t
{
	Full,
	Render,  // the output scaled by the dynamic-resolution ratios at the build
};

/** @brief Engine mode numbers. Address 0 clamp, 3 wrap, 4 border, 5 mirror. Filter 0 point, 1 linear without mips, 2 trilinear. */
struct StepSampler
{
	std::uint8_t address = 0;
	std::uint8_t filter = 0;
};

/**
* @brief A CS texture the steps use instead of an engine target. A kept one is declared once (Frame/Declare.h), so a new size is
* a new version of its slot. A transient one is made for each registration.
*/
struct OwnedTarget
{
	GraphResources::Texture2D* texture = nullptr;  // the twin passes declare
	Frame::DeclaredTexture2D declared;              // a kept texture's
	std::shared_ptr<GraphResources::Texture2D> transient;
	rhi::Format format = rhi::Format::Unknown;
	// The extent of the registration being built, which the revision that switches the registration in binds.
	std::uint32_t width = 0, height = 0;
	// A kept texture's first version is not born cleared, so the first registration clears it once. Later versions are.
	bool cleared = false;
	explicit operator bool() const { return texture && *texture; }
};

struct StepTexture
{
	std::int32_t engine = -1;
	const OwnedTarget* owned = nullptr;
	std::uint32_t mip = AnchorPasses::kAllMips;

	static StepTexture Engine(std::int32_t a_name) { return { a_name, nullptr, MipOfName(a_name) }; }
	static StepTexture Owned(const OwnedTarget& a_target) { return { -1, &a_target, AnchorPasses::kAllMips }; }
	explicit operator bool() const { return engine >= 0 || owned; }
};

/** @brief One engine effect step as a CS pass. Its constants are the engine's blocks for this step, staged by the step's hook. */
struct StepDesc
{
	std::string name;
	Effects::ImageSpaceEffectEnum effect = Effects::ISCopy;
	StepTexture output;
	StepTexture output1;
	std::array<StepTexture, kMaxSlots> inputs{};
	std::array<StepSampler, kMaxSlots> samplers{};
	std::uint32_t blend = PackBlend(0, 0, 2, 0);  // no blending, all channels written
	StepArea area = StepArea::Full;
	std::uint32_t latched = 0;
	std::uint32_t permutation = 0;  // Spec::permutation
};

std::int32_t EffectIndexOf(Effects::ImageSpaceEffectEnum a_effect)
{
	for (std::size_t i = 0; i < kEffects.size(); ++i)
		if (kEffects[i].id == a_effect)
			return static_cast<std::int32_t>(i);
	return -1;
}

/** @brief Null with a_error when the effect is not covered or a texture is missing. */
std::shared_ptr<const Spec> MakeStepSpec(const StepDesc& a_desc, std::uint32_t a_entry, std::string& a_error)
{
	const auto effect = EffectIndexOf(a_desc.effect);
	if (effect < 0 || !IS().enabled[static_cast<std::size_t>(effect)]) {
		a_error = "its effect is not covered";
		return nullptr;
	}
	const auto& info = kEffects[static_cast<std::size_t>(effect)];
	const auto name = [](const StepTexture& a_texture) {
		if (a_texture.engine >= 0)
			return a_texture.engine;
		return a_texture.mip == AnchorPasses::kAllMips ? 0 : WithMip(0, a_texture.mip);
	};
	const auto texture = [](const StepTexture& a_texture) -> FrameAnchors::Texture {
		if (a_texture.owned)
			return AnchorPasses::TextureOf(*a_texture.owned->texture);
		return a_texture.engine >= 0 ? ImportOf(ImportOfName(a_texture.engine)) : FrameAnchors::Texture{};
	};
	const auto format = [&](const StepTexture& a_texture, const FrameAnchors::Texture& a_import) {
		return a_texture.owned ? a_texture.owned->format : RtvFormatOf(a_texture.engine, a_import);
	};
	auto spec = std::make_shared<Spec>();
	spec->entry = a_entry;
	spec->name = a_desc.name;
	spec->permutation = a_desc.permutation;
	auto& key = spec->key;
	key.effect = static_cast<std::uint32_t>(effect);
	key.compute = info.compute;
	key.output = name(a_desc.output);
	key.output1 = a_desc.output1 ? name(a_desc.output1) : -1;
	spec->output = texture(a_desc.output);
	if (!spec->output) {
		a_error = "its output could not be resolved";
		return nullptr;
	}
	// A kept output may still have its last version's extent. The registration is for the extent the revision adopts with it.
	spec->width = a_desc.output.owned ? a_desc.output.owned->width : spec->output.Width();
	spec->height = a_desc.output.owned ? a_desc.output.owned->height : spec->output.Height();
	if (a_desc.output1) {
		spec->output1 = texture(a_desc.output1);
		if (!spec->output1) {
			a_error = "its second output could not be resolved";
			return nullptr;
		}
	}
	for (std::uint32_t slot = 0; slot < kMaxSlots; ++slot) {
		if (a_desc.latched & (1u << slot)) {
			key.inputs[slot] = kLatchSourceInput;
			key.address[slot] = a_desc.samplers[slot].address;
			key.filter[slot] = a_desc.samplers[slot].filter;
			continue;
		}
		if (!a_desc.inputs[slot])
			continue;
		key.inputs[slot] = name(a_desc.inputs[slot]);
		key.address[slot] = a_desc.samplers[slot].address;
		key.filter[slot] = a_desc.samplers[slot].filter;
		spec->inputs[slot] = texture(a_desc.inputs[slot]);
		if (!spec->inputs[slot]) {
			a_error = fmt::format("its input t{} could not be resolved", slot);
			return nullptr;
		}
	}
	if (!info.compute) {
		key.blend = a_desc.blend;
		spec->format = format(a_desc.output, spec->output);
		if (spec->output1)
			spec->format1 = format(a_desc.output1, spec->output1);
		const auto shift = a_desc.output.mip == AnchorPasses::kAllMips ? 0u : a_desc.output.mip;
		float width = static_cast<float>((std::max)(spec->width >> shift, 1u));
		float height = static_cast<float>((std::max)(spec->height >> shift, 1u));
		if (a_desc.area == StepArea::Render) {
			const auto [x, y] = DynamicRatios();
			width *= x;
			height *= y;
		}
		key.viewport = OriginViewport(width, height);
	}
	return spec;
}

struct StepKey
{
	WorldScope scope = WorldScope::None;
	Effects::ImageSpaceEffectEnum effect = Effects::ISCopy;
	std::uint16_t variant = 0;

	friend auto operator<=>(const StepKey&, const StepKey&) = default;
};

/**
* @brief What the world steps' registration is a function of. Their clients are declared on these variables (Frame/Declare.h),
* so a change registers them again when a revision starts, and the revision switches the new builds in.
*/
struct WorldKey
{
	Frame::Vars::Extent2D display{};
	Frame::Vars::Ratios ratios{};  // the render-size viewports are baked at registration until step 8
	std::uint32_t saoLevels = 0, effects = 0, states = 0;
	std::uint32_t ssr = 0;  // Dynamic Cubemaps' EnabledSSR: the SSR raymarch's ENABLESSR permutation
	friend bool operator==(const WorldKey&, const WorldKey&) = default;
};

struct WorldSteps
{
	bool on = false;          // the deferred+water stage runs in this mode
	bool registered = false;  // a registration was built
	std::optional<WorldKey> built;
	std::array<std::optional<FrameAnchors::ClientDesc>, 2> descs;  // the scopes' and the water blend's
	std::array<Frame::DeclaredClient, 2> clients;
	std::uint32_t saoLevels = 0, contrastLevel = 0, width = 0, height = 0;
	std::pair<float, float> ratios{ 1.0f, 1.0f };
	std::map<StepKey, std::uint32_t> entries;
	std::array<bool, kGateEntries> scopesMade{}, waterMade{};  // set on the build thread
	// Entries a hook found off, with the frame each was last asked for. The stage key keeps them present while asked for.
	std::array<std::uint64_t, kGateEntries> requestedScopes{};
	// Each client of a stage has its own rows. The water client's follow the scopes'.
	std::uint32_t waterRowBase = 0;
	// Lets the description at Begin turn on what the engine state selects.
	std::array<StepKey, kGateEntries> scopesKey{}, waterKey{};
	std::uint32_t scopesCount = 0, waterCount = 0;
	OwnedTarget ssrMask, saoTemp, sao;
	std::array<OwnedTarget, 2> saoHistory;
	WorldScope scope = WorldScope::None;
	std::uint32_t minifyLevel = 0;
};

WorldSteps& World()
{
	static WorldSteps world;
	return world;
}

constexpr std::uint64_t kRequestFrames = 300;

bool RequestHolds(std::uint64_t a_at)
{
	return a_at != 0 && Frame::Driver::Current().frame - a_at < kRequestFrames;
}

/** @brief gFrameCounter (525008 / 411489). Its parity picks the frame's normals target and water history. */
std::uint32_t EngineFrameCounter()
{
	static REL::Relocation<const std::uint32_t*> counter{ REL::RelocationID(525008, 411489) };
	return *counter.get();
}

/** @brief The normals target as the engine picks it (100460). The swap on even frames with SSR on, else the main one. */
std::uint16_t EngineNormalsParity()
{
	auto* manager = RE::ImageSpaceManager::GetSingleton();
	const auto reflections = manager ? *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<std::uintptr_t>(manager) + 0x1c0) : 0;
	const bool ssr = reflections && *reinterpret_cast<const bool*>(reflections + 0x10);
	return ssr ? static_cast<std::uint16_t>(~EngineFrameCounter() & 1u) : 0;
}

/** @brief Like the engine target, with a UAV for a history's first clear. False while the engine has not made it. */
bool OwnedDescLike(std::int32_t a_like, D3D11_TEXTURE2D_DESC& a_desc)
{
	if (a_like < 0 || a_like >= static_cast<std::int32_t>(RE::RENDER_TARGET::kTOTAL))
		return false;
	const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[a_like];
	if (!target.texture)
		return false;
	a_desc = {};
	target.texture->GetDesc(&a_desc);
	// The engine's texture may be typeless, so use the RTV's format.
	if (target.RTV) {
		D3D11_RENDER_TARGET_VIEW_DESC view{};
		target.RTV->GetDesc(&view);
		a_desc.Format = view.Format;
	}
	a_desc.MipLevels = 1;
	a_desc.ArraySize = 1;
	a_desc.Usage = D3D11_USAGE_DEFAULT;
	a_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
	a_desc.CPUAccessFlags = 0;
	a_desc.MiscFlags = 0;
	return true;
}

/** @brief A registration's transient texture of a_desc. Fatal when it cannot be made. Render thread. */
void MakeTransient(OwnedTarget& a_out, const D3D11_TEXTURE2D_DESC& a_desc, std::string_view a_name)
{
	a_out.transient = Frame::Resources::Texture2D(a_name, a_desc);
	if (!a_out.transient)
		stl::report_and_fail(fmt::format("Community Shaders: the ImageSpace texture {} could not be made", a_name));
	a_out.texture = a_out.transient.get();
	// The engine target may have no memory in native mode, so the format comes from its description, not an import.
	a_out.format = rhi::helpers::ToRHI(a_desc.Format);
	a_out.width = a_desc.Width;
	a_out.height = a_desc.Height;
}

/** @brief A transient texture like the engine target. Fatal when the target or the texture is missing. */
void MakeTransient(OwnedTarget& a_out, RE::RENDER_TARGET a_like, std::string_view a_name)
{
	D3D11_TEXTURE2D_DESC desc{};
	if (!OwnedDescLike(static_cast<std::int32_t>(a_like), desc))
		stl::report_and_fail(fmt::format("Community Shaders: the ImageSpace texture {} is made like engine target {}, which the engine has not made", a_name,
			static_cast<std::int32_t>(a_like)));
	MakeTransient(a_out, desc, a_name);
}

/** @brief A kept texture's description at a revision's values. False when the engine has no target for them. */
using KeptDescFn = std::function<bool(const Frame::Inputs&, D3D11_TEXTURE2D_DESC&)>;

/**
* @brief Setup. A texture kept across frames, declared on a_dependencies with a_desc, so a new size is a new version of its slot,
* born cleared. Its format is the first description's. One texel while a_desc finds no target. False, logged, on failure.
*/
bool DeclareKept(OwnedTarget& a_out, std::string_view a_name, Frame::Dependencies a_dependencies, KeptDescFn a_desc)
{
	D3D11_TEXTURE2D_DESC first{};
	if (!a_desc(Frame::Inputs(nullptr), first)) {
		logger::error("[ORG] ImageSpace: {} has no description: the engine target it is made like is missing", a_name);
		return false;
	}
	a_out.declared = Frame::DeclareTexture2D(std::string(a_name), std::move(a_dependencies),
		[a_desc, first](const Frame::Inputs& a_in) {
			D3D11_TEXTURE2D_DESC desc = first;
			if (!a_desc(a_in, desc)) {
				desc = first;
				desc.Width = desc.Height = 1;
			}
			desc.Format = first.Format;
			return desc;
		},
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
	if (!a_out.declared)
		return false;
	a_out.texture = a_out.declared.Get();
	a_out.format = rhi::helpers::ToRHI(first.Format);
	a_out.width = first.Width;
	a_out.height = first.Height;
	return true;
}

/** @brief Setup. A kept texture like an engine target. The engine makes its targets once a session (Q4), with the display size. */
bool DeclareKept(OwnedTarget& a_out, RE::RENDER_TARGET a_like, std::string_view a_name)
{
	const auto like = static_cast<std::int32_t>(a_like);
	return DeclareKept(a_out, a_name, Frame::DependsOn(Frame::Vars::DisplaySize()),
		[like](const Frame::Inputs&, D3D11_TEXTURE2D_DESC& a_desc) { return OwnedDescLike(like, a_desc); });
}

/** @brief A kept texture like an engine target: its extent in the registration being built. */
void KeptExtentLike(OwnedTarget& a_target, std::int32_t a_like)
{
	D3D11_TEXTURE2D_DESC desc{};
	const bool found = OwnedDescLike(a_like, desc);
	a_target.width = found ? desc.Width : 1u;
	a_target.height = found ? desc.Height : 1u;
}

/**
* @brief A one-shot clear of a kept texture's first version, before any pass reads it. Later versions are born cleared.
* Nothing when an earlier registration cleared it.
*/
void ClearFirstVersion(OwnedTarget& a_target, const char* a_client, std::int32_t a_order, std::string a_name, std::vector<FrameAnchors::PassDesc>& a_passes)
{
	if (a_target.cleared)
		return;
	a_target.cleared = true;
	AnchorPasses::Builder builder(Anchor::ImageSpace, a_client, a_order);
	builder.Add(AnchorPasses::ClearOf(std::move(a_name), *a_target.texture), true);
	for (auto& pass : builder.Take())
		a_passes.push_back(std::move(pass));
}

/** @brief The covered effects and blend states the engine has made. Steps register once it has its ImageSpace manager. */
template <class Values>
std::optional<Frame::Vars::ImageSpaceShape> ReadyShape(const Values& a_values)
{
	const auto shape = a_values.Get(Frame::Vars::EngineImageSpace());
	if (!IS().on || shape.effects == 0)
		return std::nullopt;
	return shape;
}

/**
* @brief Whether the engine draws for another registration than the frame's: a variable the registration follows was written
* since it was built, or the revision with it is not adopted yet. A draw with no pass is lag then, not a mismatch.
*/
template <class Key, class KeyOf>
bool Lagging(const std::optional<Key>& a_built, KeyOf&& a_keyOf)
{
	if (!a_built)
		return true;
	return a_keyOf(Frame::Inputs(nullptr)) != a_built || a_keyOf(Frame::Revision::Adopted()) != a_built;
}

/**
* @brief Setup. Declares the step client a_part on a_dependencies. Every client of a step group has the group's key, so they
* register again together. a_desc gives the client's registration for a key, and builds the group's registrations once per key.
* Null or empty means no client.
*/
template <class Key>
Frame::DeclaredClient DeclareStepClient(PostPart a_part, Frame::Dependencies a_dependencies, std::function<std::optional<Key>(const Frame::Inputs&)> a_key,
	std::function<const std::optional<FrameAnchors::ClientDesc>*(const Key&)> a_desc)
{
	const std::string id = IS().post[static_cast<std::size_t>(a_part)].id;
	return Frame::DeclareClient<Key>(
		id, std::move(a_dependencies),
		[a_key, a_desc](const Frame::Inputs& a_in) -> std::optional<Key> {
			auto key = a_key(a_in);
			if (!key)
				return std::nullopt;
			const auto* desc = a_desc(*key);
			if (!desc || !*desc)
				return std::nullopt;
			return key;
		},
		[a_desc](const Key& a_value, FrameAnchors::ClientDesc& a_out) {
			const auto* desc = a_desc(a_value);
			if (!desc || !*desc)
				return false;
			a_out = **desc;
			return true;
		});
}

/** @brief The SAO level count (528105 / 415050). The minify loop draws levels 1 to count - 1. */
std::uint32_t EngineSaoLevels()
{
	static REL::Relocation<const std::uint32_t*> levels{ REL::RelocationID(528105, 415050) };
	return *levels.get();
}

/** @brief The first level whose source height is at most 16 (RenderSSAO, SE 0x1412ebd70). 0 for none. */
std::uint32_t ContrastLevel(std::uint32_t a_levels)
{
	static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> targets{ REL::RelocationID(524970, 411451) };
	const auto height = targets.get()->renderTargetData[RE::RENDER_TARGET::kMAIN].height;
	for (std::uint32_t level = 1; level < a_levels; ++level)
		if ((height >> (level - 1)) <= 16)
			return level;
	return 0;
}

/** @brief The world steps' key at a revision's values, or none while the engine is not ready for them. */
template <class Values>
std::optional<WorldKey> WorldKeyOf(const Values& a_values)
{
	const auto shape = ReadyShape(a_values);
	if (!shape || !World().on)
		return std::nullopt;
	// ENABLESSR is in the engine's SSR defines only while Dynamic Cubemaps is loaded.
	const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	const std::uint32_t ssr = dynamicCubemaps.loaded && a_values.Get(dynamicCubemaps.settings.EnabledSSR) ? 1u : 0u;
	return WorldKey{ a_values.Get(Frame::Vars::DisplaySize()), a_values.Get(Frame::Vars::DynamicRatios()), shape->saoLevels, shape->effects, shape->states, ssr };
}

bool WorldLagging()
{
	return Lagging(World().built, [](const auto& a_values) { return WorldKeyOf(a_values); });
}

void BuildWorldSteps(const WorldKey& a_key);

/** @brief The world client a_part's registration for a_key. Builds both clients' once per key. */
const std::optional<FrameAnchors::ClientDesc>* WorldDescFor(const WorldKey& a_key, std::size_t a_part)
{
	auto& world = World();
	if (world.built != a_key) {
		world.built = a_key;
		BuildWorldSteps(a_key);
	}
	return &world.descs[a_part];
}

/** @brief Setup. The kept SAO textures, and the scopes and water blend clients declared on their variables. False, logged, on failure. */
bool DeclareWorldSteps()
{
	auto& world = World();
	// The fog-only composite reads the blurred AO in frames where SAO drew nothing, so it is kept across frames.
	if (!DeclareKept(world.sao, RE::RENDER_TARGET::kSAO, "ImageSpace::SAO") || !DeclareKept(world.saoHistory[0], RE::RENDER_TARGET::kSAO_RAWAO, "ImageSpace::SAORawAO0") ||
		!DeclareKept(world.saoHistory[1], RE::RENDER_TARGET::kSAO_RAWAO, "ImageSpace::SAORawAO1"))
		return false;
	// EnabledSSR picks the SSR raymarch's program, so its change registers the clients again with a revision.
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::DynamicRatios(), Frame::Vars::EngineImageSpace(),
		globals::features::dynamicCubemaps.settings.EnabledSSR);
	for (std::size_t part = 0; part < world.clients.size(); ++part)
		world.clients[part] = DeclareStepClient<WorldKey>(part ? PostPart::WorldWater : PostPart::WorldScopes, dependencies,
			[](const Frame::Inputs& a_in) { return WorldKeyOf(a_in); }, [part](const WorldKey& a_key) { return WorldDescFor(a_key, part); });
	return std::ranges::all_of(world.clients, [](const Frame::DeclaredClient& a_client) { return static_cast<bool>(a_client); });
}

template <class On>
void SelectEntries(const std::array<StepKey, kGateEntries>& a_keys, std::uint32_t a_count, std::array<std::uint64_t, 4>& a_bits, On&& a_on)
{
	for (std::uint32_t entry = 0; entry < a_count && entry < 256; ++entry)
		if (a_on(a_keys[entry]))
			a_bits[entry / 64] |= 1ull << (entry % 64);
}

/**
* @brief The scope passes this frame has on, read at Begin from the engine state that selects them.
* Variants the engine picks late, like the normals parity, are both on and the hook opens the one drawn.
*/
void WorldScopeRows(std::array<std::uint64_t, 4>& a_rows)
{
	auto& world = World();
	Frame::WorldFields fields{};
	for (const auto scope : { Frame::PostChain::Scope::Reflections, Frame::PostChain::Scope::Ssao, Frame::PostChain::Scope::Snow, Frame::PostChain::Scope::SsaoComposite })
		fields = Frame::PostChain::ReadWorld(scope, fields);
	const auto parity = static_cast<std::uint16_t>(Frame::Driver::Current().parity);
	const bool key = FrameAnchors::DescribingKey();
	SelectEntries(world.scopesKey, world.scopesCount, a_rows, [&](const StepKey& a_key) {
		switch (a_key.scope) {
		case WorldScope::Reflections:
			if (!fields.ssr)
				return false;
			switch (a_key.effect) {
			case Effects::ISDownsampleIgnoreBrightest:
				return fields.ssrDownsample != 0;
			case Effects::ISReflectionsRayTracing:
				// Variant 2 reads the downsampled mask. 0 and 1 read the normals of that parity.
				return (a_key.variant == 2) == (fields.ssrDownsample != 0);
			case Effects::ISReflectionBlurHCS:
			case Effects::ISReflectionBlurVCS:
				return fields.ssrBlur;
			default:
				return true;
			}
		case WorldScope::Ssao:
			if ((a_key.effect == Effects::ISSAORawAO && !fields.saoTemporal) || (a_key.effect == Effects::ISSAORawAONoTemporal && fields.saoTemporal))
				return false;
			// Variant bit 1 is the history written.
			if (a_key.effect == Effects::ISSAORawAO || a_key.effect == Effects::ISSAORawAONoTemporal || a_key.effect == Effects::ISSAOBlurH)
				return key || ((a_key.variant >> 1) & 1u) == parity;
			return true;
		case WorldScope::Snow:
			return key || (a_key.effect == Effects::ISSnowSSS ? fields.snow : !fields.snow);
		case WorldScope::SsaoComposite:
			// High byte is the form: 0 SAO, 1 SAO and fog, 2 fog, 3 copy. Bit 1 reads the blurred reflections.
			if ((a_key.variant >> 8) != fields.saoForm)
				return false;
			return (a_key.variant >> 8) == 3 || (((a_key.variant >> 1) & 1u) != 0) == fields.ssrBlur;
		default:
			return true;
		}
	});
	for (std::uint32_t entry = 0; entry < world.scopesCount && entry < 256; ++entry)
		if (RequestHolds(world.requestedScopes[entry]))
			a_rows[entry / 64] |= 1ull << (entry % 64);
}

/** @brief Both history parities. The engine's frame counter picks one and the hook opens it. */
void WorldWaterRows(std::array<std::uint64_t, 4>& a_rows)
{
	const auto& world = World();
	for (std::uint32_t entry = 0; entry < world.waterCount; ++entry) {
		const auto row = world.waterRowBase + entry;
		if (row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
	}
}

/**
* @brief The scopes and water blend clients' registrations for a_key, in Main::RenderWorld's order. Their declarations register
* them when the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildWorldSteps(const WorldKey& a_key)
{
	auto& state = IS();
	auto& world = World();
	world.registered = false;
	world.descs = {};
	world.entries.clear();
	world.scopesMade.fill(false);
	world.waterMade.fill(false);
	world.requestedScopes = {};
	world.scopesCount = world.waterCount = 0;
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!main.texture)
		return;
	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	world.width = mainDesc.Width;
	world.height = mainDesc.Height;
	world.ratios = { a_key.ratios.x, a_key.ratios.y };
	world.saoLevels = a_key.saoLevels;
	world.contrastLevel = ContrastLevel(world.saoLevels);
	// MakeStepSpec bakes the render-size viewports with the key's ratios.
	const BuildRatiosScope ratioScope(world.ratios);
	MakeTransient(world.ssrMask, RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED, "ImageSpace::SSRMaskDownsampled");
	MakeTransient(world.saoTemp, RE::RENDER_TARGET::kSAO_TEMP_BLUR, "ImageSpace::SAOBlurTemp");
	KeptExtentLike(world.sao, RE::RENDER_TARGET::kSAO);
	for (auto& history : world.saoHistory)
		KeptExtentLike(history, RE::RENDER_TARGET::kSAO_RAWAO);

	using E = Effects;
	using T = RE::RENDER_TARGET;
	const auto engine = [](T a_target, std::uint32_t a_mip = AnchorPasses::kAllMips) {
		return StepTexture::Engine(a_mip == AnchorPasses::kAllMips ? static_cast<std::int32_t>(a_target) : WithMip(static_cast<std::int32_t>(a_target), a_mip));
	};
	const auto normals = [&](std::uint32_t a_parity) { return engine(a_parity ? T::kNORMAL_TAAMASK_SSRMASK_SWAP : T::kNORMAL_TAAMASK_SSRMASK); };
	const auto mainDepth = StepTexture::Engine(kDepthInput | 0);
	const auto depthCopy = StepTexture::Engine(kDepthInput | 7);          // RenderSSAO's t0 is depth target 7
	const auto reflections = StepTexture::Engine(kCubeInput | 0);         // CS binds it with its own sampler
	const auto owned = [](const OwnedTarget& a_target) { return StepTexture::Owned(a_target); };

	struct Pending
	{
		bool water = false;
		StepKey key;
		StepDesc desc;
	};
	std::vector<Pending> steps;
	const auto add = [&](WorldScope a_scope, E::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, StepDesc a_desc) {
		a_desc.effect = a_effect;
		steps.push_back({ a_scope == WorldScope::WaterBlend, { a_scope, a_effect, a_variant }, std::move(a_desc) });
	};

	// The compute form of the mask downsample has no CS pass.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("ssr.downsample.n{}", p), .output = owned(world.ssrMask), .area = StepArea::Render };
		d.inputs[0] = normals(p);
		d.samplers[0] = { 4, 1 };
		add(WorldScope::Reflections, E::ISDownsampleIgnoreBrightest, p, std::move(d));
	}
	for (std::uint16_t v = 0; v < 3; ++v) {
		StepDesc d{ .name = v == 2 ? std::string("ssr.raymarch.mask") : fmt::format("ssr.raymarch.n{}", v), .output = engine(T::kSSR_RAW), .area = StepArea::Render };
		d.inputs[0] = v == 2 ? owned(world.ssrMask) : normals(v);
		d.inputs[1] = engine(T::kMAIN);
		d.inputs[2] = mainDepth;
		d.inputs[3] = engine(T::kMAIN_ONLY_ALPHA);
		d.inputs[4] = reflections;
		d.samplers[1] = { 0, 1 };
		d.samplers[3] = { 0, 1 };
		d.permutation = a_key.ssr ? kSsrPermutation : 0u;
		add(WorldScope::Reflections, E::ISReflectionsRayTracing, v, std::move(d));
	}
	{
		StepDesc h{ .name = "ssr.blur.h", .output = engine(T::kSSR_BLURRED0) };
		h.inputs[0] = engine(T::kSSR_RAW);
		add(WorldScope::Reflections, E::ISReflectionBlurHCS, 0, std::move(h));
		StepDesc v{ .name = "ssr.blur.v", .output = engine(T::kSSR) };
		v.inputs[0] = engine(T::kSSR_BLURRED0);
		add(WorldScope::Reflections, E::ISReflectionBlurVCS, 0, std::move(v));
	}

	// RenderSSAO's first call. kSAO_CAMERAZ stays an engine target because Upscaling's depth upscale writes it too.
	// Levels are point filtered up to level 4 and linear after, as RenderSSAO sets.
	{
		StepDesc d{ .name = "sao.cameraz", .output = engine(T::kSAO_CAMERAZ, 0), .area = StepArea::Render };
		d.inputs[0] = depthCopy;
		add(WorldScope::Ssao, E::ISSAOCameraZ, 0, std::move(d));
	}
	for (std::uint32_t level = 1; level < world.saoLevels && level <= kMaxMip; ++level) {
		for (const bool contrast : { false, true }) {
			if (contrast && level != world.contrastLevel)
				continue;
			StepDesc d{ .name = fmt::format("sao.minify{}{}", contrast ? ".contrast" : "", level), .output = engine(T::kSAO_CAMERAZ, level) };
			d.inputs[0] = engine(T::kSAO_CAMERAZ, level - 1);
			d.samplers[0] = { 0, static_cast<std::uint8_t>(level > 4 ? 1 : 0) };
			add(WorldScope::Ssao, contrast ? E::ISMinifyContrast : E::ISMinify, static_cast<std::uint16_t>(level), std::move(d));
		}
	}
	// Variant bit 0 is the normals parity, bit 1 the history written. The temporal form reads the other history.
	for (const bool temporal : { true, false })
		for (std::uint16_t h = 0; h < 2; ++h)
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc d{ .name = fmt::format("sao.rawao{}.h{}.n{}", temporal ? "" : ".notemporal", h, p), .output = owned(world.saoHistory[h]), .area = StepArea::Render };
				d.inputs[0] = engine(T::kSAO_CAMERAZ);
				d.inputs[1] = normals(p);
				if (temporal)
					d.inputs[2] = owned(world.saoHistory[1 - h]);
				d.inputs[3] = engine(T::kMOTION_VECTOR);
				d.samplers[0] = { 0, 2 };
				d.samplers[1] = { 0, 1 };
				d.samplers[2] = { 0, 1 };
				d.samplers[3] = { 0, 1 };
				add(WorldScope::Ssao, temporal ? E::ISSAORawAO : E::ISSAORawAONoTemporal, static_cast<std::uint16_t>(p | (h << 1)), std::move(d));
			}
	for (std::uint16_t h = 0; h < 2; ++h) {
		StepDesc d{ .name = fmt::format("sao.blur.h{}", h), .output = owned(world.saoTemp), .area = StepArea::Render };
		d.inputs[0] = owned(world.saoHistory[h]);
		d.samplers[0] = { 0, 3 };
		add(WorldScope::Ssao, E::ISSAOBlurH, static_cast<std::uint16_t>(h << 1), std::move(d));
	}
	{
		StepDesc d{ .name = "sao.blur.v", .output = owned(world.sao), .area = StepArea::Render };
		d.inputs[0] = owned(world.saoTemp);
		d.samplers[0] = { 0, 3 };
		add(WorldScope::Ssao, E::ISSAOBlurV, 0, std::move(d));
	}

	// The snow pass (SE 0x1412ee8d0) or a copy of kMAIN into kSNOW_SWAP. No test scene reaches the snow pass.
	{
		StepDesc d{ .name = "snow.sss", .output = engine(T::kSNOW_SWAP) };
		d.inputs[0] = engine(T::kSAO_CAMERAZ, 0);
		d.inputs[1] = engine(T::kMAIN);
		d.inputs[2] = engine(T::kSNOW_SPECALPHA);
		d.samplers[0] = { 0, 1 };
		d.samplers[1] = { 0, 1 };
		add(WorldScope::Snow, E::ISSnowSSS, 0, std::move(d));
		StepDesc c{ .name = "snow.copy", .output = engine(T::kSNOW_SWAP) };
		c.inputs[0] = engine(T::kMAIN);
		add(WorldScope::Snow, E::ISCopySubRegionCS, 0, std::move(c));
	}

	// The composite into kMAIN. Form 3, with neither SAO nor fog, is a copy.
	// Variant: form in the high byte, blur in bit 1, normals parity in bit 0.
	constexpr std::array<E::ImageSpaceEffectEnum, 3> kComposite{ E::ISSAOCompositeSAO, E::ISSAOCompositeSAOFog, E::ISSAOCompositeFog };
	for (std::uint16_t form = 0; form < 3; ++form)
		for (std::uint16_t blur = 0; blur < 2; ++blur)
			for (std::uint16_t p = 0; p < 2; ++p) {
				StepDesc d{ .name = fmt::format("sao.composite{}{}.n{}", form, blur ? ".blurred" : "", p), .output = engine(T::kMAIN), .area = StepArea::Render };
				d.inputs[0] = engine(T::kSNOW_SWAP);
				d.inputs[1] = owned(world.sao);
				d.inputs[2] = depthCopy;
				d.inputs[3] = normals(p);
				d.inputs[4] = engine(T::kSNOW_SPECALPHA);
				d.inputs[5] = engine(T::kSHADOW_MASK);
				d.inputs[6] = engine(blur ? T::kSSR : T::kSSR_RAW);
				d.samplers[6] = { 0, 1 };
				add(WorldScope::SsaoComposite, kComposite[form], static_cast<std::uint16_t>((form << 8) | (blur << 1) | p), std::move(d));
			}
	{
		StepDesc d{ .name = "sao.composite.copy", .output = engine(T::kMAIN), .area = StepArea::Render };
		d.inputs[0] = engine(T::kSNOW_SWAP);
		add(WorldScope::SsaoComposite, E::ISCopy, static_cast<std::uint16_t>(3 << 8), std::move(d));
	}

	// The engine alternates kWATER_1 and kWATER_2 by frame counter. Odd parity writes kWATER_2 and reads kWATER_1.
	// These stay engine targets because the skeleton's clears and the deferred path name them.
	for (std::uint16_t h = 0; h < 2; ++h) {
		StepDesc d{ .name = fmt::format("water.blend.h{}", h), .output = engine(T::kMAIN), .output1 = engine(h ? T::kWATER_2 : T::kWATER_1), .area = StepArea::Render };
		d.inputs[0] = engine(T::kRAW_WATER);
		d.inputs[1] = engine(h ? T::kWATER_1 : T::kWATER_2);
		d.inputs[2] = engine(T::kMOTION_VECTOR);
		d.inputs[3] = mainDepth;
		d.inputs[4] = normals(h ? 0 : 1);
		d.samplers[1] = { 0, 1 };
		add(WorldScope::WaterBlend, E::ISWaterBlend, h, std::move(d));
	}

	for (const bool water : { false, true }) {
		auto& client = state.post[static_cast<std::size_t>(water ? PostPart::WorldWater : PostPart::WorldScopes)];
		client.specs.clear();
		client.batch.clear();
		auto& keys = water ? world.waterKey : world.scopesKey;
		auto& count = water ? world.waterCount : world.scopesCount;
		std::vector<FrameAnchors::PassDesc> passes;
		// Clear the history pair's first versions before any pass reads them.
		if (!water)
			for (std::uint32_t h = 0; h < 2; ++h)
				ClearFirstVersion(world.saoHistory[h], client.id, -1 - static_cast<std::int32_t>(h), fmt::format("sao.history{}.clear", h), passes);
		for (auto& step : steps) {
			if (step.water != water)
				continue;
			const auto entry = count;
			std::string error;
			auto spec = MakeStepSpec(step.desc, entry, error);
			if (!spec) {
				logger::info("[ORG] ImageSpace: world step {} has no pass ({})", step.desc.name, error);
				continue;
			}
			++count;
			keys[entry] = step.key;
			world.entries[step.key] = entry;
			passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
				.anchor = Anchor::ImageSpace,
				.order = static_cast<std::int32_t>(entry),
				.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
				.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, entry,
							water] {
					auto pass = MakePass(spec, latch, gate, id);
					auto& made = water ? World().waterMade : World().scopesMade;
					made[entry] = pass != nullptr;
					return pass;
				},
				// Every hook has run by the commit, so a step the engine did not draw skips and folds its barriers.
				.atExecution = true,
				.gpuGated = false,
				.row = static_cast<std::int16_t>((water ? world.waterRowBase : 0) + entry) });
			client.specs.push_back(std::move(spec));
		}
		// Registered first, so the water client's rows follow.
		if (!water)
			world.waterRowBase = count;
		FrameAnchors::ClientDesc desc{};
		desc.id = client.id;
		desc.passes = std::move(passes);
		desc.sharedConstants = true;
		desc.gates = { client.gate };
		desc.registerResources = [water](org::RenderGraph& a_graph) {
			if (water)
				return;
			const auto& current = World();
			// Only targets kept across frames.
			for (const auto* target : { &current.sao, &current.saoHistory[0], &current.saoHistory[1] })
				if (*target)
					target->texture->Register(a_graph);
		};
		const auto part = static_cast<std::size_t>(water ? PostPart::WorldWater : PostPart::WorldScopes);
		desc.active = [part] { return !IS().post[part].batch.empty(); };
		desc.commit = [part](const FrameAnchors::Point&) {
			auto& current = IS().post[part];
			CommitBatch(current.batch, *current.latch, *current.gate);
		};
		desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
		if (water)
			desc.rows = WorldWaterRows;
		else
			desc.rows = WorldScopeRows;
		desc.repeats = true;
		desc.segmentOnly = FrameAnchors::NamedByStage(client.id);
		// An empty client is no client. Its declaration then registers none.
		if (!desc.passes.empty())
			world.descs[water ? 1 : 0] = std::move(desc);
	}
	world.registered = true;
	logger::info("[ORG] ImageSpace: the RenderWorld scopes are CS passes: {} scope passes ({} SAO levels, contrast level {}), {} water blend passes, for {}x{} at "
				 "ratios {:.3f}x{:.3f}, registered for the revision starting now",
		world.scopesCount, world.saoLevels, world.contrastLevel, world.waterCount, world.width, world.height, world.ratios.first, world.ratios.second);
}

/** @brief None means the draw goes the main client's way. */
WorldScope WorldStepScope()
{
	const auto& world = World();
	if (!world.on || !world.registered)
		return WorldScope::None;
	return world.scope;
}

/** @brief Captures the constants the engine bound for the step and stages them for its pass. Withholds the engine's draw. */
bool StageStep(PostClient& a_client, std::uint32_t a_entry, std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
	Pending pending{ .entry = a_entry, .compute = a_compute, .groups = a_groups };
	std::vector<std::shared_ptr<const org::LatchBlock>> blocks;
	if (a_compute) {
		setDirtyStates(true);
		ID3D11Buffer* constants = Tripwire::Unwrap(Constants::EngineComputeBuffer());
		if (!constants) {
			Failure("no engine compute constant buffer", a_effect);
			return true;
		}
		globals::d3d::context->CSSetConstantBuffers(0, 1, &constants);
		const auto use = Constants::Capture(Constants::StageBit(Constants::Stage::Compute));
		const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Compute, 0) : nullptr;
		const auto at = slot ? Constants::Where(*slot) : Constants::Location{};
		if (!slot || !at.block) {
			Failure("the capture holds no compute constants for its dispatch", a_effect);
			return true;
		}
		pending.at = { .srv = at.srvIndex, .offset = at.offset, .size = at.size };
		blocks.push_back(at.block);
	} else {
		setDirtyStates(false);
		const auto use = Constants::Capture(Constants::kDrawStages);
		const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 2) : nullptr;
		const auto* frameSlot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 12) : nullptr;
		if (!frameSlot) {
			Failure("the capture holds no PerFrame constants for its draw", a_effect);
			return true;
		}
		const auto frameAt = Constants::Where(*frameSlot);
		const auto at = slot ? Constants::Where(*slot) : frameAt;
		if (!at.block || !frameAt.block) {
			Failure("its captured constants have no ring chunk", a_effect);
			return true;
		}
		pending.at = { .srv = at.srvIndex, .offset = at.offset, .frameSrv = frameAt.srvIndex, .frameOffset = frameAt.offset, .size = slot ? at.size : 0u,
			.frameSize = frameAt.size };
		blocks.push_back(at.block);
		blocks.push_back(frameAt.block);
	}
	a_client.batch.push_back(std::move(pending));
	for (auto& block : blocks)
		state.postKeep.push_back(std::move(block));
	++state.taken;
	return true;
}


/**
* @brief Matches the draw to its pass by scope, effect and the variant the engine picked, then stages its constants.
* A draw with no pass is a mismatch. A pass that is off draws nothing and asks the next description for it.
*/
bool TakeWorldStep(WorldScope a_scope, std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	auto& world = World();
	const auto id = kEffects[a_effect].id;
	using E = Effects;
	std::uint16_t variant = 0;
	const auto parity = static_cast<std::uint16_t>(Frame::Driver::Current().parity);
	switch (id) {
	case E::ISDownsampleIgnoreBrightest:
		variant = EngineNormalsParity();
		break;
	case E::ISReflectionsRayTracing:
		{
			static REL::Relocation<const bool*> downsample{ REL::RelocationID(513568, 391448) };
			static REL::Relocation<const std::int32_t*> mode{ REL::RelocationID(513454, 391287) };
			variant = (*downsample.get() && *mode.get() > 1) ? 2 : EngineNormalsParity();
			break;
		}
	case E::ISMinify:
	case E::ISMinifyContrast:
		// RenderSSAO draws levels 1 to count - 1 in order, once each.
		variant = static_cast<std::uint16_t>(++world.minifyLevel);
		break;
	case E::ISSAORawAO:
	case E::ISSAORawAONoTemporal:
		variant = static_cast<std::uint16_t>(EngineNormalsParity() | (parity << 1));
		break;
	case E::ISSAOBlurH:
		variant = static_cast<std::uint16_t>(parity << 1);
		break;
	case E::ISSAOCompositeSAO:
	case E::ISSAOCompositeSAOFog:
	case E::ISSAOCompositeFog:
		{
			static REL::Relocation<const bool*> blur{ REL::RelocationID(528114, 415059) };
			const std::uint16_t form = id == E::ISSAOCompositeSAO ? 0 : id == E::ISSAOCompositeSAOFog ? 1 : 2;
			variant = static_cast<std::uint16_t>((form << 8) | ((*blur.get() ? 1u : 0u) << 1) | EngineNormalsParity());
			break;
		}
	case E::ISCopy:
		variant = 3 << 8;
		break;
	case E::ISWaterBlend:
		variant = static_cast<std::uint16_t>(EngineFrameCounter() & 1u);
		break;
	default:
		break;
	}
	const StepKey key{ a_scope, id, variant };
	const auto found = world.entries.find(key);
	if (found == world.entries.end()) {
		// The engine may draw for new sizes before the revision with their registration is adopted.
		if (WorldLagging()) {
			Frame::Driver::NoteLaggedWork(Frame::SegmentId::Deferred);
			return true;
		}
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, Frame::SegmentId::Deferred,
			fmt::format("{} (variant {:#x}) in a RenderWorld scope has no CS pass; it draws nothing", kEffects[a_effect].effectName, variant));
		return true;
	}
	const auto entry = found->second;
	const bool water = a_scope == WorldScope::WaterBlend;
	auto& client = state.post[static_cast<std::size_t>(water ? PostPart::WorldWater : PostPart::WorldScopes)];
	const auto& made = water ? world.waterMade : world.scopesMade;
	const auto& rows = Frame::Driver::Current().Segment(Frame::SegmentId::Deferred).rows;
	const auto row = (water ? world.waterRowBase : 0) + entry;
	const bool scheduled = row < 256 && (rows[row / 64] >> (row % 64) & 1u) != 0 && FrameAnchors::CanTap(client.id);
	if (!made[entry] || !scheduled) {
		// Off this frame, so draw nothing and let the next description turn it on. The water blend's rows are always on.
		if (!water)
			world.requestedScopes[entry] = Frame::Driver::Current().frame;
		Frame::Driver::NoteStepLag(Frame::SegmentId::Deferred, client.id, fmt::format("world step {} (variant {:#x})", kEffects[a_effect].effectName, variant));
		return true;
	}
	// A requested step stays on while the engine draws it.
	if (!water && RequestHolds(world.requestedScopes[entry]))
		world.requestedScopes[entry] = Frame::Driver::Current().frame;
	return StageStep(client, entry, a_effect, a_compute, a_groups);
}

/** @brief Entering sets the scope. Leaving stages its draws. */
struct WorldScopeGuard
{
	explicit WorldScopeGuard(WorldScope a_scope)
	{
		auto& world = World();
		previous = world.scope;
		world.scope = a_scope;
		if (a_scope == WorldScope::Ssao)
			world.minifyLevel = 0;
	}
	~WorldScopeGuard() { World().scope = previous; }
	WorldScope previous = WorldScope::None;
};
