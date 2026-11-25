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
	LensFlare,   // BSLensFlare's frame function (SE 100274), before the composites
	Iblf,        // the image-based lens flares (AE 0x1414d21b0), inside the composites
	SaoCs,       // the compute SAO (100526 / 107252), from Main::Draw before the post chain
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
	Render,  // the output scaled by the engine's dynamic-resolution ratios at each draw, through the latch
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
	StepTexture output2;  // a dispatch's third UAV
	std::array<StepTexture, kMaxSlots> inputs{};
	std::array<StepSampler, kMaxSlots> samplers{};
	std::uint32_t blend = PackBlend(0, 0, 2, 0);  // no blending, all channels written
	StepArea area = StepArea::Full;
	std::uint32_t latched = 0;
	std::uint32_t permutation = 0;  // Spec::permutation
	// An engine depth-stencil target (kDepthInput | index) the draw tests against, with the engine's depth and stencil modes.
	StepTexture depth;
	std::uint32_t depthMode = 0, stencilMode = 0, stencilRef = 0;
};

/** @brief The format of an engine depth-stencil target's view, or Unknown before the engine made it. */
rhi::Format DepthViewFormatOf(std::uint32_t a_index)
{
	auto* view = globals::game::renderer->GetDepthStencilData().depthStencils[a_index].views[0];
	if (!view)
		return rhi::Format::Unknown;
	D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
	view->GetDesc(&desc);
	return rhi::helpers::ToRHI(desc.Format);
}

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
	if (effect < 0) {
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
	key.output2 = a_desc.output2 ? name(a_desc.output2) : -1;
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
	if (a_desc.output2) {
		spec->output2 = texture(a_desc.output2);
		if (!spec->output2) {
			a_error = "its third output could not be resolved";
			return nullptr;
		}
	}
	if (a_desc.depth) {
		key.depth = a_desc.depth.engine;
		key.depthMode = a_desc.depthMode;
		key.stencilMode = a_desc.stencilMode;
		key.stencilRef = a_desc.stencilRef;
		spec->depth = texture(a_desc.depth);
		spec->depthFormat = DepthViewFormatOf(IndexOfName(a_desc.depth.engine));
		if (!spec->depth || spec->depthFormat == rhi::Format::Unknown) {
			a_error = "its depth-stencil target could not be resolved";
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
		if (spec->output2)
			spec->format2 = format(a_desc.output2, spec->output2);
		const auto shift = a_desc.output.mip == AnchorPasses::kAllMips ? 0u : a_desc.output.mip;
		const float width = static_cast<float>((std::max)(spec->width >> shift, 1u));
		const float height = static_cast<float>((std::max)(spec->height >> shift, 1u));
		// A render-size step records the whole output. Its quad is drawn in the viewport the hook latches at each draw, so a new
		// ratio needs no registration (decision 6a).
		spec->latchViewport = a_desc.area == StepArea::Render && !info.geometry && !info.vertexStage;
		key.viewport = OriginViewport(width, height);
	}
	WantSpecProgram(*spec);
	return spec;
}

struct StepKey
{
	WorldScope scope = WorldScope::None;
	Effects::ImageSpaceEffectEnum effect = Effects::ISCopy;
	std::uint16_t variant = 0;

	friend auto operator<=>(const StepKey&, const StepKey&) = default;
};

/** @brief No copy entry. */
constexpr std::uint16_t kNoCopy = 0xffff;

/** @brief Where a step's pass is in a group build: its client (the part) and its gate entry there. */
struct StepEntry
{
	std::uint8_t part = 0;
	std::uint32_t entry = 0;
};

/** @brief One client's passes in a group build, by gate entry. */
struct StepPart
{
	std::uint32_t count = 0;
	std::uint32_t rowBase = 0;  // the client's first row in its stage
	std::array<StepKey, kGateEntries> keys{};
	std::array<std::shared_ptr<const Spec>, kGateEntries> specs{};
	// The post chain's (ChainSteps.inl). Zero elsewhere.
	std::array<std::uint8_t, kGateEntries> group{};
	std::array<std::uint8_t, kGateEntries> route{};
	std::array<bool, kGateEntries> copy{};
	std::array<std::uint16_t, kGateEntries> copyBack{};
	// Set on the build thread when a graph build makes the pass. A pass MakePass left out stays false.
	std::array<std::atomic<bool>, kGateEntries> made{};

	StepPart() { copyBack.fill(kNoCopy); }

	bool Made(std::uint32_t a_entry) const { return a_entry < count && made[a_entry].load(std::memory_order_acquire); }
	const Spec* SpecOf(std::uint32_t a_entry) const { return a_entry < kGateEntries ? specs[a_entry].get() : nullptr; }
	std::uint32_t RowOf(std::uint32_t a_entry) const { return rowBase + a_entry; }
};

/**
* @brief A group build's step table: what its rows, its passes' make functions and the hooks read. It holds nothing of its
* registrations, so those may capture it.
*/
template <std::size_t Parts>
struct StepTable
{
	std::array<StepPart, Parts> parts;
	// Drawn steps only. Copies are reached through the steps that write their source.
	std::map<StepKey, StepEntry> entries;
};

/** @brief One build of a step group for a key: its clients' registrations and its step table. The clients' state (Frame::Held). */
template <class Key, std::size_t Parts>
struct StepBuild
{
	static constexpr std::size_t kParts = Parts;
	Key key{};
	std::array<std::optional<FrameAnchors::ClientDesc>, Parts> descs;
	std::shared_ptr<StepTable<Parts>> table = std::make_shared<StepTable<Parts>>();
};

/** @brief Whether the frame's schedule of a_segment has a_row on. Render owner. */
bool RowOn(Frame::SegmentId a_segment, std::uint32_t a_row)
{
	if (a_segment >= Frame::SegmentId::Count || a_row >= 256)
		return false;
	const auto& rows = Frame::Driver::Current().Segment(a_segment).rows;
	return (rows[a_row / 64] >> (a_row % 64) & 1u) != 0;
}

/** @brief The open stage that holds client a_id now, or a_fallback when none does. Render owner. */
Frame::SegmentId StageOf(std::string_view a_id, Frame::SegmentId a_fallback)
{
	const auto place = FrameAnchors::Place(a_id);
	return place.stage < static_cast<std::uint32_t>(Frame::SegmentId::Count) ? static_cast<Frame::SegmentId>(place.stage) : a_fallback;
}

/** @brief How a hook's draw stands against the build the frame runs (StepGroup::Find). */
enum class StepFound : std::uint8_t
{
	Draw,     // the held build has its pass
	NotHeld,  // the frame holds no build of its client on now: the placement says why
	Behind,   // the engine draws for newer variables than the held build's: lagged work
	NoPass,   // the held build is for the newest variables and has no pass for it: a mismatch
};

template <class Build>
struct StepFind
{
	StepFound found = StepFound::NoPass;
	std::shared_ptr<Build> build;  // the held build, when found
	StepEntry at;
	FrameAnchors::Placement place;  // where the held build was looked for

	const StepPart& Part() const { return build->table->parts[at.part]; }
};

/**
* @brief A step group (WorldSteps, ChainSteps, StartSteps, MenuSteps): one CS client per part, all declared on one key and
* built together for it. Each registration's state is its build (Frame::DeclareClient<Key, State>), so a hook matches the
* engine's draw against the build the frame runs (Frame::Held), never the newest.
*/
template <class Build>
class StepGroup
{
public:
	using Key = decltype(Build::key);
	static constexpr std::size_t kParts = Build::kParts;
	using KeyFn = std::function<std::optional<Key>(const Frame::Inputs&)>;
	using BuildFn = std::function<void(Build&)>;

	/**
	* @brief Setup. Declares a client per part on a_dependencies and the programs. A part's key is the group's while its build has
	* passes for it, else it has no client.
	*/
	void Declare(const std::array<PostPart, kParts>& a_parts, const Frame::Dependencies& a_dependencies, KeyFn a_key, BuildFn a_build)
	{
		parts = a_parts;
		keyOf = std::move(a_key);
		build = std::move(a_build);
		// A build that missed a program is made again by the revision that has it.
		auto dependencies = a_dependencies;
		for (const auto& dependency : ProgramDependencies())
			dependencies.push_back(dependency);
		for (std::size_t part = 0; part < kParts; ++part) {
			clients[part] = Frame::DeclareClient<Key, Build>(
				std::string(Id(part)), dependencies,
				[this, part](const Frame::Inputs& a_in) -> std::optional<Key> {
					auto key = keyOf(a_in);
					if (!key)
						return std::nullopt;
					const auto made = For(*key, a_in);
					// A program the revision has not built: the registration stays as it is until the next revision.
					if (!made)
						return clients[part].Registered() ? registered[part] : std::nullopt;
					if (!made->descs[part])
						return std::nullopt;
					return key;
				},
				[this, part](const Key& a_key, FrameAnchors::ClientDesc& a_out) -> std::shared_ptr<Build> {
					// The key function just made or found it.
					const auto made = Kept(a_key);
					if (!made || !made->descs[part])
						return nullptr;
					a_out = *made->descs[part];
					registered[part] = a_key;
					return made;
				});
		}
	}

	/** @brief The part's client id. */
	const char* Id(std::size_t a_part) const { return IS().post[static_cast<std::size_t>(parts[a_part])].id; }

	PostPart PartOf(std::size_t a_part) const { return parts[a_part]; }

	/** @brief Whether a client of the group is registered. */
	bool Registered() const
	{
		return std::ranges::any_of(clients, [](const Frame::DeclaredClientOf<Build>& a_client) { return a_client.Registered(); });
	}

	/**
	* @brief The build a run of a_anchor uses now for the part's client. Render owner. With Anchor::Count, the build of any open stage
	* that holds the client: a hook's draws run at the next tap of whichever stage that is.
	*/
	Frame::Held<Build> Held(std::size_t a_part, Anchor a_anchor = Anchor::Count) const { return clients[a_part].Held(a_anchor); }

	/** @brief The newest build, or null. For what is not drawn by a held build, such as the hooks' scopes. Render thread. */
	std::shared_ptr<const Build> Newest() const { return builds.empty() ? nullptr : builds.back(); }

	/**
	* @brief Whether the newest written variables give another key than a_key (null: any key). The engine then draws for values
	* the build the frame runs was not made for, and a draw it has no pass for is lag.
	*/
	bool Ahead(const Key* a_key) const
	{
		const auto key = keyOf(Frame::Inputs(nullptr));
		return key && (!a_key || !(*key == *a_key));
	}

	/** @brief Ahead of the newest build: a draw no build has a pass for is lag while the newest variables give a newer key. */
	bool AheadOfNewest() const { return Ahead(builds.empty() ? nullptr : &builds.back()->key); }

	/**
	* @brief Render owner. The pass for a_key in the build the frame runs for its client. The part comes from the builds that have
	* the key. A held build without the key is lag while the newest variables give a newer key, else a mismatch.
	*/
	StepFind<Build> Find(const StepKey& a_key, Anchor a_anchor = Anchor::Count) const
	{
		StepFind<Build> out;
		std::optional<std::uint8_t> part;
		for (auto it = builds.rbegin(); it != builds.rend() && !part; ++it)
			if (const auto found = (*it)->table->entries.find(a_key); found != (*it)->table->entries.end())
				part = found->second.part;
		if (!part) {
			out.found = AheadOfNewest() ? StepFound::Behind : StepFound::NoPass;
			return out;
		}
		out.at.part = *part;
		const auto held = Held(*part, a_anchor);
		out.place = held.Placement();
		if (!held) {
			out.found = StepFound::NotHeld;
			return out;
		}
		out.build = held.Shared();
		const auto& entries = out.build->table->entries;
		const auto found = entries.find(a_key);
		if (found == entries.end() || found->second.part != *part) {
			out.found = Ahead(&out.build->key) ? StepFound::Behind : StepFound::NoPass;
			return out;
		}
		out.at = found->second;
		out.found = StepFound::Draw;
		return out;
	}

	/**
	* @brief Whether a_find has a pass to draw. Otherwise notes why not in a_segment: lag while the engine draws for newer variables
	* or the frame holds no build of the client, a tap error when the held build has no pass. a_what names the draw, a_where its place.
	*/
	template <class What>
	bool Draws(const StepFind<Build>& a_find, Frame::SegmentId a_segment, const What& a_what, std::string_view a_where) const
	{
		switch (a_find.found) {
		case StepFound::Behind:
			Frame::Driver::NoteLaggedWork(a_segment);
			return false;
		case StepFound::NoPass:
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, a_segment, fmt::format("{} {} has no CS pass; it draws nothing", a_what(), a_where));
			return false;
		case StepFound::NotHeld:
			Frame::Driver::NoteStepLag(a_segment, Id(a_find.at.part), a_what());
			return false;
		case StepFound::Draw:
			break;
		}
		return true;
	}

private:
	/** @brief Builds kept for their keys. A revision's adoption asks a client's key again, which then finds its build here. */
	static constexpr std::size_t kKeptBuilds = 4;

	/** @brief Render thread. The kept build for a_key, or null. */
	std::shared_ptr<Build> Kept(const Key& a_key) const
	{
		for (const auto& kept : builds)
			if (kept->key == a_key)
				return kept;
		return nullptr;
	}

	/**
	* @brief Render thread. The build for a_key: a kept one, else one made now with a_in's programs. Null when a_in has no build of
	* one of them: it is wanted (SpecProgram), so the next revision has it and makes the build again.
	*/
	std::shared_ptr<Build> For(const Key& a_key, const Frame::Inputs& a_in)
	{
		if (auto kept = Kept(a_key))
			return kept;
		auto made = std::make_shared<Build>();
		made->key = a_key;
		SpecProgramsMissing() = false;
		ProgramInputs() = &a_in;
		build(*made);
		ProgramInputs() = nullptr;
		if (std::exchange(SpecProgramsMissing(), false))
			return nullptr;
		builds.push_back(made);
		if (builds.size() > kKeptBuilds)
			builds.erase(builds.begin());
		return made;
	}

	std::array<PostPart, kParts> parts{};
	std::array<Frame::DeclaredClientOf<Build>, kParts> clients;
	std::array<std::optional<Key>, kParts> registered;  // each part's registered key, set by its make
	KeyFn keyOf;
	BuildFn build;
	std::vector<std::shared_ptr<Build>> builds;  // oldest first
};

/** @brief Adds a step's pass to a_table's part at its next entry. The entry, or kGateEntries when the part is full. */
template <std::size_t Parts>
std::uint32_t AddStep(StepTable<Parts>& a_table, std::uint8_t a_part, const StepKey& a_key, std::shared_ptr<const Spec> a_spec, bool a_drawn = true)
{
	auto& part = a_table.parts[a_part];
	const auto entry = part.count;
	if (entry >= kGateEntries || part.rowBase + entry >= 256)
		return kGateEntries;
	++part.count;
	part.keys[entry] = a_key;
	part.specs[entry] = std::move(a_spec);
	if (a_drawn)
		a_table.entries.try_emplace(a_key, StepEntry{ a_part, entry });
	return entry;
}

/**
* @brief A step's pass made at a graph build on the render thread: MakePass, and its made flag in the table. Recorded at
* execution, so a step the engine did not draw skips and folds its barriers.
*/
template <std::size_t Parts>
FrameAnchors::PassDesc StepPassDesc(const std::shared_ptr<StepTable<Parts>>& a_table, std::uint8_t a_part, std::uint32_t a_entry, const PostClient& a_client,
	const std::shared_ptr<const Spec>& a_spec)
{
	return { .name = fmt::format("{}.{}", a_client.id, a_spec->name),
		.anchor = Anchor::ImageSpace,
		.order = static_cast<std::int32_t>(a_entry),
		.kind = a_spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
		.make =
			[table = a_table, a_part, a_entry, spec = a_spec, latch = std::shared_ptr<const org::LatchBlock>(a_client.latch),
				gate = std::shared_ptr<const RunGate>(a_client.gate), id = a_client.id] {
				auto pass = MakePass(spec, latch, gate, id);
				table->parts[a_part].made[a_entry].store(pass != nullptr, std::memory_order_release);
				return pass;
			},
		.atExecution = true,
		.gpuGated = false,
		.row = static_cast<std::int16_t>(a_table->parts[a_part].RowOf(a_entry)) };
}

/**
* @brief Post part a_part's client with a_passes: its gate, the shared constants, and a commit that opens what its batch staged.
* The caller adds its rows, resources, repeats and stage flags.
*/
FrameAnchors::ClientDesc PartClientDesc(std::size_t a_part, std::vector<FrameAnchors::PassDesc> a_passes)
{
	const auto& client = IS().post[a_part];
	FrameAnchors::ClientDesc desc{};
	desc.id = client.id;
	desc.passes = std::move(a_passes);
	desc.sharedConstants = true;
	desc.gates = { client.gate };
	desc.active = [a_part] { return !IS().post[a_part].batch.empty(); };
	desc.commit = [a_part](const FrameAnchors::Point&) {
		auto& current = IS().post[a_part];
		CommitBatch(current.batch, *current.latch, *current.gate);
	};
	desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
	return desc;
}

/**
* @brief What the world steps' registration is a function of. Their clients are declared on these variables (Frame/Declare.h),
* so a change registers them again when a revision starts, and the revision switches the new builds in.
*/
struct WorldKey
{
	Frame::Vars::Extent2D display{};
	std::uint32_t saoLevels = 0, effects = 0, states = 0;
	std::uint32_t ssr = 0;  // Dynamic Cubemaps' EnabledSSR: the SSR raymarch's ENABLESSR permutation
	friend bool operator==(const WorldKey&, const WorldKey&) = default;
};

/** @brief The world steps' build for a key. Part 0 is the scopes' client, part 1 the water blend's, whose rows follow the scopes'. */
struct WorldBuild : StepBuild<WorldKey, 2>
{
	std::uint32_t saoLevels = 0, contrastLevel = 0, width = 0, height = 0;
};

struct WorldSteps
{
	StepGroup<WorldBuild> group;
	OwnedTarget ssrMask, saoTemp, sao;
	std::array<OwnedTarget, 2> saoHistory;
	WorldScope scope = WorldScope::None;
	std::uint32_t minifyLevel = 0;
	// The scopes' state at this frame's Begin, which its description read (WorldScopeRows). Render owner.
	Frame::WorldFields described{};
};

WorldSteps& World()
{
	static WorldSteps world;
	return world;
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
		RenderGraph::Fatal("ImageSpace", fmt::format("the texture {} could not be made", a_name));
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
		RenderGraph::Fatal("ImageSpace", fmt::format("the texture {} is made like engine target {}, which the engine has not made", a_name,
			static_cast<std::int32_t>(a_like)));
	MakeTransient(a_out, desc, a_name);
}

/** @brief A kept texture's description at a revision's values. False when the engine has no target for them. */
using KeptDescFn = std::function<bool(const Frame::Inputs&, D3D11_TEXTURE2D_DESC&)>;

/**
* @brief Setup. A texture kept across frames, declared on a_dependencies with a_desc, so a new size is a new version of its slot.
* Every version, the first too, is born zero on the revision lane. Its format is the first description's. One texel while a_desc finds no target.
* Fatal when the engine target it is made like is missing.
*/
void DeclareKept(OwnedTarget& a_out, std::string_view a_name, Frame::Dependencies a_dependencies, KeptDescFn a_desc)
{
	D3D11_TEXTURE2D_DESC first{};
	if (!a_desc(Frame::Inputs(nullptr), first))
		RenderGraph::Fatal("ImageSpace", fmt::format("{} has no description: the engine target it is made like is missing", a_name));
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
		GraphResources::Options{ .lifetime = GraphResources::Lifetime::History, .birth = Frame::Birth::Zero() });
	a_out.texture = a_out.declared.Get();
	a_out.format = rhi::helpers::ToRHI(first.Format);
	a_out.width = first.Width;
	a_out.height = first.Height;
}

/** @brief Setup. A kept texture like an engine target. The engine makes its targets once a session (Q4), with the display size. */
void DeclareKept(OwnedTarget& a_out, RE::RENDER_TARGET a_like, std::string_view a_name)
{
	const auto like = static_cast<std::int32_t>(a_like);
	DeclareKept(a_out, a_name, Frame::DependsOn(Frame::Vars::DisplaySize()),
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

/** @brief The covered effects and blend states the engine has made. Steps register once it has its ImageSpace manager. */
template <class Values>
std::optional<Frame::Vars::ImageSpaceShape> ReadyShape(const Values& a_values)
{
	const auto shape = a_values.Get(Frame::Vars::EngineImageSpace());
	if (!IS().on || shape.effects == 0)
		return std::nullopt;
	return shape;
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
	if (!shape || !IS().on)
		return std::nullopt;
	// ENABLESSR is in the engine's SSR defines only while Dynamic Cubemaps is loaded.
	const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	const std::uint32_t ssr = dynamicCubemaps.loaded && a_values.Get(dynamicCubemaps.settings.EnabledSSR) ? 1u : 0u;
	return WorldKey{ a_values.Get(Frame::Vars::DisplaySize()), shape->saoLevels, shape->effects, shape->states, ssr };
}

void BuildWorldSteps(WorldBuild& a_build);

/** @brief Setup. The kept SAO textures, and the scopes and water blend clients declared on their variables. */
void DeclareWorldSteps()
{
	auto& world = World();
	// The fog-only composite reads the blurred AO in frames where SAO drew nothing, so it is kept across frames.
	DeclareKept(world.sao, RE::RENDER_TARGET::kSAO, "ImageSpace::SAO");
	DeclareKept(world.saoHistory[0], RE::RENDER_TARGET::kSAO_RAWAO, "ImageSpace::SAORawAO0");
	DeclareKept(world.saoHistory[1], RE::RENDER_TARGET::kSAO_RAWAO, "ImageSpace::SAORawAO1");
	// EnabledSSR picks the SSR raymarch's program, so its change registers the clients again with a revision. The ratios are no
	// dependency: the render-size viewports are latched at each draw.
	const auto dependencies = Frame::DependsOn(Frame::Vars::DisplaySize(), Frame::Vars::EngineImageSpace(), globals::features::dynamicCubemaps.settings.EnabledSSR);
	world.group.Declare({ PostPart::WorldScopes, PostPart::WorldWater }, dependencies, [](const Frame::Inputs& a_in) { return WorldKeyOf(a_in); },
		[](WorldBuild& a_build) { BuildWorldSteps(a_build); });
}

/** @brief The scopes' state now, as a frame's description reads it at Begin. Render owner. */
Frame::WorldFields WorldFieldsNow()
{
	Frame::WorldFields fields{};
	for (const auto scope : { Frame::PostChain::Scope::Reflections, Frame::PostChain::Scope::Ssao, Frame::PostChain::Scope::Snow, Frame::PostChain::Scope::SsaoComposite })
		fields = Frame::PostChain::ReadWorld(scope, fields);
	return fields;
}

/** @brief Begin, after the frame's description: keeps the scopes' state it read, which the hooks check their draws against. Render owner. */
void NoteWorldBegin()
{
	World().described = WorldFieldsNow();
}

/**
* @brief Whether a scope step is on in a frame whose description read a_fields at Begin. For a build key (a_buildKey), every
* variant the engine picks late is on, as are the history parities. The water blend is always on.
*/
bool WorldStepOn(const StepKey& a_key, const Frame::WorldFields& a_fields, std::uint16_t a_parity, bool a_buildKey)
{
	switch (a_key.scope) {
	case WorldScope::Reflections:
		if (!a_fields.ssr)
			return false;
		switch (a_key.effect) {
		case Effects::ISDownsampleIgnoreBrightest:
			return a_fields.ssrDownsample == 1;
		case Effects::ISDownsampleIgnoreBrightestCS:
			return a_fields.ssrDownsample == 2;
		case Effects::ISReflectionsRayTracing:
			// Variant 2 reads the downsampled mask. 0 and 1 read the normals of that parity. After the compute downsample (divider 4)
			// the engine leaves the ray march's input at the normals (AE 107146), so it reads them too.
			return (a_key.variant == 2) == (a_fields.ssrDownsample == 1);
		case Effects::ISReflectionBlurHCS:
		case Effects::ISReflectionBlurVCS:
			return a_fields.ssrBlur;
		default:
			return true;
		}
	case WorldScope::Ssao:
		if ((a_key.effect == Effects::ISSAORawAO && !a_fields.saoTemporal) || (a_key.effect == Effects::ISSAORawAONoTemporal && a_fields.saoTemporal))
			return false;
		// Variant bit 1 is the history written.
		if (a_key.effect == Effects::ISSAORawAO || a_key.effect == Effects::ISSAORawAONoTemporal || a_key.effect == Effects::ISSAOBlurH)
			return a_buildKey || ((a_key.variant >> 1) & 1u) == a_parity;
		return true;
	case WorldScope::Snow:
		return a_buildKey || (a_key.effect == Effects::ISSnowSSS ? a_fields.snow : !a_fields.snow);
	case WorldScope::SsaoComposite:
		// High byte is the form: 0 SAO, 1 SAO and fog, 2 fog, 3 copy. Bit 1 reads the blurred reflections.
		if ((a_key.variant >> 8) != a_fields.saoForm)
			return false;
		return (a_key.variant >> 8) == 3 || (((a_key.variant >> 1) & 1u) != 0) == a_fields.ssrBlur;
	default:
		return true;
	}
}

/**
* @brief The scope passes this frame has on, read at Begin from the engine state that selects them (WorldStepOn).
* Variants the engine picks late, like the normals parity, are both on and the hook opens the one drawn.
*/
void WorldScopeRows(const StepPart& a_part, std::array<std::uint64_t, 4>& a_rows)
{
	const auto fields = WorldFieldsNow();
	const auto parity = static_cast<std::uint16_t>(Frame::Driver::Current().parity);
	const bool key = FrameAnchors::DescribingKey();
	for (std::uint32_t entry = 0; entry < a_part.count; ++entry)
		if (const auto row = a_part.RowOf(entry); row < 256 && WorldStepOn(a_part.keys[entry], fields, parity, key))
			a_rows[row / 64] |= 1ull << (row % 64);
}

/** @brief Both history parities. The engine's frame counter picks one and the hook opens it. */
void WorldWaterRows(const StepPart& a_part, std::array<std::uint64_t, 4>& a_rows)
{
	for (std::uint32_t entry = 0; entry < a_part.count; ++entry)
		if (const auto row = a_part.RowOf(entry); row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
}

/**
* @brief The scopes and water blend clients' registrations for a_build's key, in Main::RenderWorld's order. Their declarations
* register them when the revision starts, and the revision switches their builds in. Render thread.
*/
void BuildWorldSteps(WorldBuild& a_build)
{
	auto& state = IS();
	auto& world = World();
	const auto& a_key = a_build.key;
	const auto& main = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMAIN];
	if (!main.texture)
		return;
	D3D11_TEXTURE2D_DESC mainDesc{};
	main.texture->GetDesc(&mainDesc);
	a_build.width = mainDesc.Width;
	a_build.height = mainDesc.Height;
	a_build.saoLevels = a_key.saoLevels;
	a_build.contrastLevel = ContrastLevel(a_build.saoLevels);
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

	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("ssr.downsample.n{}", p), .output = owned(world.ssrMask), .area = StepArea::Render };
		d.inputs[0] = normals(p);
		d.samplers[0] = { 4, 1 };
		add(WorldScope::Reflections, E::ISDownsampleIgnoreBrightest, p, std::move(d));
	}
	// At divider 4 the compute form replaces it (AE 107146): u0 the mask target, t0 the normals, 4 samples, no jitter compensation.
	// The engine's ray march then still reads the normals, so nothing reads what it writes.
	for (std::uint16_t p = 0; p < 2; ++p) {
		StepDesc d{ .name = fmt::format("ssr.downsample.cs.n{}", p), .output = owned(world.ssrMask) };
		d.inputs[0] = normals(p);
		d.samplers[0] = { 4, 1 };
		add(WorldScope::Reflections, E::ISDownsampleIgnoreBrightestCS, p, std::move(d));
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
	for (std::uint32_t level = 1; level < a_build.saoLevels && level <= kMaxMip; ++level) {
		for (const bool contrast : { false, true }) {
			if (contrast && level != a_build.contrastLevel)
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

	const auto& table = a_build.table;
	for (const bool water : { false, true }) {
		const auto p = static_cast<std::uint8_t>(water ? 1 : 0);
		const auto partIndex = static_cast<std::size_t>(water ? PostPart::WorldWater : PostPart::WorldScopes);
		auto& client = state.post[partIndex];
		client.batch.clear();
		auto& part = table->parts[p];
		// Each client of a stage has its own rows. The water client's follow the scopes'.
		if (water)
			part.rowBase = table->parts[0].count;
		std::vector<FrameAnchors::PassDesc> passes;
		for (auto& step : steps) {
			if (step.water != water)
				continue;
			std::string error;
			auto spec = MakeStepSpec(step.desc, part.count, error);
			if (!spec) {
				logger::info("[ORG] ImageSpace: world step {} has no pass ({})", step.desc.name, error);
				continue;
			}
			const auto entry = AddStep(*table, p, step.key, spec);
			if (entry == kGateEntries) {
				logger::error("[ORG] ImageSpace: the world steps do not fit a stage's rows; {} has no pass", step.desc.name);
				continue;
			}
			passes.push_back(StepPassDesc(table, p, entry, client, spec));
		}
		auto desc = PartClientDesc(partIndex, std::move(passes));
		desc.registerResources = [water](org::RenderGraph& a_graph) {
			if (water)
				return;
			const auto& current = World();
			// Only targets kept across frames.
			for (const auto* target : { &current.sao, &current.saoHistory[0], &current.saoHistory[1] })
				if (*target)
					target->texture->Register(a_graph);
		};
		// Rows read the registration's own table, never a newer one's.
		const std::shared_ptr<const StepTable<2>> rows = table;
		if (water)
			desc.rows = [rows](std::array<std::uint64_t, 4>& a_rows) { WorldWaterRows(rows->parts[1], a_rows); };
		else
			desc.rows = [rows](std::array<std::uint64_t, 4>& a_rows) { WorldScopeRows(rows->parts[0], a_rows); };
		desc.repeats = true;
		desc.segmentOnly = FrameAnchors::NamedByStage(client.id);
		// An empty client is no client. Its declaration then registers none.
		if (!desc.passes.empty())
			a_build.descs[p] = std::move(desc);
	}
	logger::info("[ORG] ImageSpace: the RenderWorld scopes are CS passes: {} scope passes ({} SAO levels, contrast level {}), {} water blend passes, for {}x{} "
				 "(render-size viewports latched per draw), built for the revision starting now",
		table->parts[0].count, a_build.saoLevels, a_build.contrastLevel, table->parts[1].count, a_build.width, a_build.height);
}

/** @brief None means the draw goes the main client's way. */
WorldScope WorldStepScope()
{
	const auto& world = World();
	if (!IS().on || !world.group.Registered())
		return WorldScope::None;
	return world.scope;
}

/**
* @brief Captures the constants the engine bound for the step and stages them for its pass. Withholds the engine's draw.
* a_spec is the entry's spec in the build the frame runs, or null for a copy.
*/
bool StageStep(PostClient& a_client, std::uint32_t a_entry, const Spec* a_spec, std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
	Pending pending{ .entry = a_entry, .compute = a_compute, .groups = a_groups };
	std::vector<std::shared_ptr<const void>> blocks;
	if (a_compute) {
		// A native dispatch applies no engine state: only the CS b0 buffer is bound for the capture.
		if (!NativeSteps::Get().armed)
			setDirtyStates(true);
		else
			++NativeSteps::Get().taken;
		ID3D11Buffer* constants = Tripwire::Unwrap(Constants::EngineComputeBuffer());
		if (!constants) {
			RenderGraph::Fatal(kEffects[a_effect].effectName, "no engine compute constant buffer");
		}
		globals::d3d::context->CSSetConstantBuffers(0, 1, &constants);
		const auto use = Constants::Capture(Constants::StageBit(Constants::Stage::Compute));
		const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Compute, 0) : nullptr;
		const auto at = slot ? Constants::Where(*slot) : Constants::Location{};
		if (!slot || !at.owner) {
			RenderGraph::Fatal(kEffects[a_effect].effectName, "the capture holds no compute constants for its dispatch");
		}
		pending.at = { .srv = at.srvIndex, .offset = at.offset, .size = at.size };
		blocks.push_back(at.owner);
	} else if (auto& native = NativeSteps::Get(); native.armed) {
		// Native: the rows the engine's SetupGeometry would write and CS's FrameBuffer, with no engine state applied.
		const auto frameAt = NativeSteps::FrameLocation();
		const auto rowsAt = native.rowsAt.owner ? native.rowsAt : native.rows.size ? Constants::Upload({ native.rows.bytes.data(), native.rows.size }) : frameAt;
		if (!frameAt.owner || !rowsAt.owner) {
			RenderGraph::Fatal(kEffects[a_effect].effectName, "its computed constants have no ring chunk");
		}
		pending.at = { .srv = rowsAt.srvIndex, .offset = rowsAt.offset, .frameSrv = frameAt.srvIndex, .frameOffset = frameAt.offset, .size = native.rows.size,
			.frameSize = frameAt.size };
		blocks.push_back(rowsAt.owner);
		blocks.push_back(frameAt.owner);
		++native.taken;
		if (a_spec && a_spec->latchViewport) {
			pending.at.viewport = RenderViewportOf(*a_spec);
			pending.vertices = 6;
		}
	} else {
		setDirtyStates(false);
		const auto use = Constants::Capture(Constants::kDrawStages);
		const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 2) : nullptr;
		const auto* frameSlot = use.Valid() ? Constants::Find(use, Constants::Stage::Pixel, 12) : nullptr;
		if (!frameSlot) {
			RenderGraph::Fatal(kEffects[a_effect].effectName, "the capture holds no PerFrame constants for its draw");
		}
		const auto frameAt = Constants::Where(*frameSlot);
		const auto at = slot ? Constants::Where(*slot) : frameAt;
		if (!at.owner || !frameAt.owner) {
			RenderGraph::Fatal(kEffects[a_effect].effectName, "its captured constants have no ring chunk");
		}
		if (native.observing && native.effect == a_effect) {
			NativeSteps::FrameLocation();
			NativeSteps::Compare(a_effect, slot ? Constants::Bytes(*slot) : std::span<const std::byte>{}, Constants::Bytes(*frameSlot));
			native.observing = false;
		}
		pending.at = { .srv = at.srvIndex, .offset = at.offset, .frameSrv = frameAt.srvIndex, .frameOffset = frameAt.offset, .size = slot ? at.size : 0u,
			.frameSize = frameAt.size };
		blocks.push_back(at.owner);
		blocks.push_back(frameAt.owner);
		// A render-size step draws its quad, two triangles, in the viewport of this draw's ratios.
		if (a_spec && a_spec->latchViewport) {
			pending.at.viewport = RenderViewportOf(*a_spec);
			pending.vertices = 6;
		}
	}
	a_client.batch.push_back(std::move(pending));
	for (auto& block : blocks)
		state.postKeep.push_back(std::move(block));
	++state.taken;
	// Says once per effect that a step took it, so a test can tell a path ran. Render thread only.
	static std::array<bool, kEffects.size()> firstTaken{};
	if (!firstTaken[a_effect]) {
		firstTaken[a_effect] = true;
		logger::info("[ORG] ImageSpace: first {} taken by its step", kEffects[a_effect].effectName);
	}
	return true;
}


/**
* @brief Logs once a session per effect where the engine's state at a draw differs from its step's: the engine targets the step
* writes (a_targets, -1 to skip one), the blend, the depth-stencil state and the samplers of the slots it reads. The steps' states
* are read from the engine's code, so a difference means that reading is wrong. Render owner.
*/
void CheckStepState(std::size_t a_effect, const Spec& a_spec, const std::array<std::int32_t, kMaxTargets>& a_targets)
{
	// A native step applies no engine state, so there is nothing bound to compare.
	if (NativeSteps::Get().armed)
		return;
	static std::set<std::size_t> checked;
	if (!checked.insert(a_effect).second)
		return;
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	std::string differences;
	const auto note = [&](std::string a_what) {
		if (!differences.empty())
			differences += "; ";
		differences += a_what;
	};
	for (std::uint32_t target = 0; target < kMaxTargets; ++target) {
		const auto bound = static_cast<std::int32_t>(shadow.renderTargets[target]);
		if (a_targets[target] >= 0 && bound != a_targets[target])
			note(fmt::format("target {} is rt{}, the step's rt{}", target, bound, a_targets[target]));
	}
	const auto blend = a_spec.key.blend;
	if (shadow.alphaBlendMode != (blend & 0xFF) || shadow.alphaBlendAlphaToCoverage != ((blend >> 8) & 0xFF) || shadow.alphaBlendWriteMode != ((blend >> 16) & 0xFF))
		note(fmt::format("blend mode {}, coverage {} and write mode {}, the step's {}, {} and {}", shadow.alphaBlendMode, shadow.alphaBlendAlphaToCoverage,
			shadow.alphaBlendWriteMode, blend & 0xFF, (blend >> 8) & 0xFF, (blend >> 16) & 0xFF));
	if (a_spec.depth) {
		const auto depthMode = static_cast<std::uint32_t>(shadow.depthStencilDepthMode);
		if (shadow.depthStencil != IndexOfName(a_spec.key.depth) || depthMode != a_spec.key.depthMode || shadow.depthStencilStencilMode != a_spec.key.stencilMode ||
			shadow.stencilRef != a_spec.key.stencilRef)
			note(fmt::format("depth target {} with depth mode {}, stencil mode {} and reference {}, the step's {}, {}, {} and {}", shadow.depthStencil, depthMode,
				shadow.depthStencilStencilMode, shadow.stencilRef, IndexOfName(a_spec.key.depth), a_spec.key.depthMode, a_spec.key.stencilMode, a_spec.key.stencilRef));
	}
	for (std::uint32_t slot = 0; slot < kEffects[a_effect].samplers; ++slot) {
		if (!SlotRead(a_spec, slot))
			continue;
		const auto address = static_cast<std::uint32_t>(shadow.PSTextureAddressMode[slot].underlying());
		const auto filter = static_cast<std::uint32_t>(shadow.PSTextureFilterMode[slot].underlying());
		if (address != a_spec.key.address[slot] || filter != a_spec.key.filter[slot])
			note(fmt::format("s{} has address {} and filter {}, the step's {} and {}", slot, address, filter, a_spec.key.address[slot], a_spec.key.filter[slot]));
	}
	if (differences.empty())
		logger::info("[ORG] ImageSpace: {}'s step ({}) matches the engine's state at its first draw", kEffects[a_effect].effectName, a_spec.name);
	else
		logger::error("[ORG] ImageSpace: {}'s step ({}) differs from the engine's state at its first draw: {}", kEffects[a_effect].effectName, a_spec.name, differences);
}

/**
* @brief Matches the draw to its pass by scope, effect and the variant the engine picked, in the build the frame runs, then stages
* its constants. A draw the frame's description at Begin has off, or that has no pass, is a mismatch.
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
	case E::ISDownsampleIgnoreBrightestCS:
		variant = EngineNormalsParity();
		break;
	case E::ISReflectionsRayTracing:
		{
			static REL::Relocation<const bool*> downsample{ REL::RelocationID(513568, 391448) };
			static REL::Relocation<const std::int32_t*> mode{ REL::RelocationID(513454, 391287) };
			// Only the raster downsample (divider 2 or 3) moves the ray march's input to the mask.
			variant = (*downsample.get() && *mode.get() > 1 && *mode.get() != 4) ? 2 : EngineNormalsParity();
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
	const auto what = [&] { return fmt::format("world step {} (variant {:#x})", kEffects[a_effect].effectName, variant); };
	const auto find = world.group.Find(key);
	// Behind: the engine draws for variables the build the frame runs was not made for, such as a new size.
	if (!world.group.Draws(find, Frame::SegmentId::Deferred, what, "in a RenderWorld scope"))
		return true;
	const auto entry = find.at.entry;
	const auto& part = find.Part();
	const bool water = a_scope == WorldScope::WaterBlend;
	auto& client = state.post[static_cast<std::size_t>(world.group.PartOf(find.at.part))];
	// Under water the water blend is a step of the sun stage.
	const auto segment = StageOf(client.id, Frame::SegmentId::Deferred);
	if (!RowOn(segment, part.RowOf(entry))) {
		// The description at Begin has it on, so the build the frame runs was made for a key without it: lag. So is a step the
		// engine turned on after Begin (the first world frame sets the composite's state late); the next Begin reads it. Else a mismatch.
		if (water || WorldStepOn(key, world.described, parity, false) || WorldStepOn(key, WorldFieldsNow(), parity, false))
			Frame::Driver::NoteStepLag(segment, client.id, what());
		else
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NotInDescription, segment,
				fmt::format("{} is drawn, and the scopes' state the frame's description read at Begin has it off", what()));
		return true;
	}
	if (!part.Made(entry)) {
		// Its graph build left the pass out (MakePass says why).
		Frame::Driver::NoteStepLag(segment, client.id, what());
		return true;
	}
	return StageStep(client, entry, part.SpecOf(entry), a_effect, a_compute, a_groups);
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
