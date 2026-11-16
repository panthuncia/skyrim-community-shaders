// The start stage's water effects as CS passes (native mode). Included by ImageSpace.cpp inside its anonymous namespace, after
// WorldSteps.inl and ChainSteps.inl, whose step helpers it uses (StepDesc, MakeStepSpec, StepKey, StageStep).
//
// Main::RenderWaterEffects (35561 / 36560) runs after the reflection faces, inside the start stage's window. Its GPU work is drawn by
// passes CS declares here, as steps of the start stage (one client at its ImageSpace step):
// - the displacement simulation (ImageSpaceEffectWaterDisplacement's Render, SE 0x141303da0, AE 0x1414ef370): the texture offset into
//   one target of the pair kWATER_DISPLACEMENT / kWATER_DISPLACEMENT_SWAP (each child effect writes its texture 0 and reads its texture
//   1) from the other, one wading ripple a ripple into the same target, then the pair swaps, then the heightmap step zero or more times
//   (a fixed time step, 0.025 s), each from one target into the other with a swap after it;
// - the flow copies: each loaded flow texture copied into a rectangle of the flow map.
// How many ripples, heightmap steps and flow copies a frame makes is the engine's late decision: each is a fixed set of instances,
// all on in the description, and the k-th draw of its kind in the frame is instance k. An instance the engine does not use stays
// closed in the run gate and does nothing on the GPU. The ripple's quad and the flow copy's source texture are frame inputs (the
// geometry's buffers and the texture by heap index, in the latch).

/** @brief Instances of each repeated water step a frame. More in one frame is a mismatch (NoPass). */
constexpr std::uint16_t kRippleInstances = 8;    // wading ripples into each target of the pair
constexpr std::uint16_t kHeightmapInstances = 4;  // heightmap steps (0.025 s each; a frame of 0.1 s makes four)
constexpr std::uint16_t kFlowInstances = 32;      // flow copies

/** @brief The start stage's water steps: their passes by key, and the running water update. */
struct StartSteps
{
	bool on = false;          // native mode with the start stage: the water effects are CS passes
	bool registered = false;  // registered for the targets below
	bool retry = false;       // a pass was left out at a build (an effect not made yet): register again at a later frame
	std::uint64_t registeredFrame = 0;
	std::uint32_t displacementSize = 0, flowSize = 0;
	std::map<StepKey, std::uint32_t> entries;
	std::array<bool, kGateEntries> made{};
	std::uint32_t count = 0;
	// The running water update (the hooks): the scope, the target the next step writes (A), whether the pair swapped after the
	// ripples, and each repeated step's draws so far this frame.
	WorldScope scope = WorldScope::None;
	std::int32_t target = -1;
	bool swapped = false;
	std::uint16_t ripples = 0, heightmaps = 0, flows = 0;
	std::uint64_t frame = UINT64_MAX;
};

StartSteps& Start()
{
	static StartSteps start;
	return start;
}

/**
 * @brief The target the water displacement update writes first (SE 0x143283ae0, AE 0x1433dcf10): kWATER_DISPLACEMENT or its swap. The
 * update sets it on a reset before it renders, and its Render writes it back at the end; read at the update's first draw.
 */
std::int32_t WaterDisplacementTarget()
{
	static REL::Relocation<const std::int32_t*> target{ REL::RelocationID(528286, 415236) };
	return *target.get();
}

void RegisterStartSteps();

/**
 * @brief Begin of a frame: the start steps registered again when the displacement pair or the flow map was made again, or a build left
 * a pass out. The build runs off the render thread. Render owner.
 */
void CheckStartSteps()
{
	auto& start = Start();
	if (!start.on || !IS().on || !RE::ImageSpaceManager::GetSingleton())
		return;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	auto* displacement = targets[RE::RENDER_TARGET::kWATER_DISPLACEMENT].texture;
	if (!displacement)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	displacement->GetDesc(&desc);
	auto* flow = FlowMapTexture();
	D3D11_TEXTURE2D_DESC flowDesc{};
	if (flow)
		flow->GetDesc(&flowDesc);
	constexpr std::uint64_t kRetryFrames = 120;
	const auto frame = Frame::Driver::Current().frame;
	if (start.registered && start.retry && frame - start.registeredFrame < kRetryFrames)
		return;
	if (start.registered && !start.retry && start.displacementSize == desc.Width && start.flowSize == flowDesc.Width)
		return;
	RegisterStartSteps();
}

/** @brief Every pass of the start client is on: each does work only where its gate entry opens (the engine's late decisions). */
void StartRows(std::array<std::uint64_t, 4>& a_rows)
{
	const auto& start = Start();
	for (std::uint32_t entry = 0; entry < start.count && entry < 256; ++entry)
		a_rows[entry / 64] |= 1ull << (entry % 64);
}

/** @brief Registers the start client (the start stage's ImageSpace step) for the targets now. */
void RegisterStartSteps()
{
	auto& state = IS();
	auto& start = Start();
	start.registered = false;
	start.retry = false;
	start.registeredFrame = Frame::Driver::Current().frame;
	start.entries.clear();
	start.made.fill(false);
	start.count = 0;
	using E = Effects;
	using T = RE::RENDER_TARGET;
	const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
	if (auto* displacement = targets[T::kWATER_DISPLACEMENT].texture) {
		D3D11_TEXTURE2D_DESC desc{};
		displacement->GetDesc(&desc);
		start.displacementSize = desc.Width;
	}
	start.flowSize = 0;
	if (auto* flow = FlowMapTexture()) {
		D3D11_TEXTURE2D_DESC desc{};
		flow->GetDesc(&desc);
		start.flowSize = desc.Width;
	}
	const auto engine = [](std::int32_t a_target) { return StepTexture::Engine(a_target); };
	constexpr StepSampler kPoint{ 0, 0 }, kLinear{ 0, 1 };
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
	// The displacement pair: each step by the target it writes (the variant's high byte; the other one is its input).
	for (const std::int32_t out : { static_cast<std::int32_t>(T::kWATER_DISPLACEMENT), static_cast<std::int32_t>(T::kWATER_DISPLACEMENT_SWAP) }) {
		const std::int32_t in = out == T::kWATER_DISPLACEMENT ? T::kWATER_DISPLACEMENT_SWAP : T::kWATER_DISPLACEMENT;
		const auto high = static_cast<std::uint16_t>(out << 8);
		{
			StepDesc d{ .name = fmt::format("water.texoffset.{}", out), .output = engine(out) };
			d.inputs[0] = engine(in);
			d.samplers[0] = kPoint;
			add(WorldScope::WaterDisplacement, E::ISWaterDisplacementTexOffset, high, std::move(d));
		}
		for (std::uint16_t r = 0; r < kRippleInstances; ++r)
			add(WorldScope::WaterDisplacement, E::ISWaterDisplacementWadingRipple, static_cast<std::uint16_t>(high | r),
				StepDesc{ .name = fmt::format("water.ripple.{}.{}", out, r), .output = engine(out) });
		for (std::uint16_t h = 0; h < kHeightmapInstances; ++h) {
			StepDesc d{ .name = fmt::format("water.heightmap.{}.{}", out, h), .output = engine(out) };
			d.inputs[0] = engine(in);
			d.samplers[0] = kLinear;
			add(WorldScope::WaterDisplacement, E::ISWaterWadingHeightmap, static_cast<std::uint16_t>(high | h), std::move(d));
		}
	}
	// The flow copies into the flow map: each reads its loaded texture by the heap index in its latch (no declared input).
	if (start.flowSize)
		for (std::uint16_t k = 0; k < kFlowInstances; ++k)
			add(WorldScope::FlowCopy, E::ISCopySubRegionCS, k, StepDesc{ .name = fmt::format("flow.copy.{}", k), .output = engine(kFlowMapOutput) });

	auto& client = state.post[static_cast<std::size_t>(PostPart::Start)];
	client.specs.clear();
	client.batch.clear();
	std::vector<FrameAnchors::PassDesc> passes;
	for (auto& step : steps) {
		const auto entry = start.count;
		if (entry >= kGateEntries)
			break;
		std::string error;
		auto spec = MakeStepSpec(step.desc, entry, error);
		if (!spec) {
			logger::info("[ORG] ImageSpace: start step {} has no pass ({})", step.desc.name, error);
			continue;
		}
		++start.count;
		start.entries[step.key] = entry;
		NoteImports(*spec);
		passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
			.anchor = Anchor::ImageSpace,
			.order = static_cast<std::int32_t>(entry),
			.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
			.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, entry] {
				auto pass = MakePass(spec, latch, gate, id);
				Start().made[entry] = pass != nullptr;
				if (!pass)
					Start().retry = true;
				return pass;
			},
			.atExecution = true,
			.gated = true,
			.gpuGated = true,
			.row = static_cast<std::int16_t>(entry) });
		client.specs.push_back(std::move(spec));
	}
	FrameAnchors::ClientDesc desc{};
	desc.id = client.id;
	desc.passes = std::move(passes);
	desc.sharedConstants = true;
	desc.gates = { client.gate };
	desc.active = [] { return !IS().post[static_cast<std::size_t>(PostPart::Start)].batch.empty(); };
	desc.commit = [](const FrameAnchors::Point&) {
		auto& current = IS().post[static_cast<std::size_t>(PostPart::Start)];
		CommitBatch(current.batch, *current.latch, *current.gate);
	};
	desc.executed = [](const FrameAnchors::Point&) { IS().executed = true; };
	desc.rows = StartRows;
	// A tap per water update and per flow copy: each commits its own instances' entries, so the frame's taps add up.
	desc.repeats = true;
	desc.segmentOnly = true;
	desc.onFailed = [] {
		logger::error("[ORG] ImageSpace: the start steps' passes failed; the water effects draw nothing for the session");
		Start().on = false;
	};
	if (!FrameAnchors::Register(std::move(desc))) {
		logger::error("[ORG] ImageSpace: the start step client was refused");
		start.on = false;
		return;
	}
	start.registered = true;
	logger::info("[ORG] ImageSpace: the water effects are start stage passes: {} passes ({} ripples and {} heightmap steps per target, {} flow copies), "
				 "displacement {}x{}, flow map {}x{}",
		start.count, kRippleInstances, kHeightmapInstances, start.flowSize ? kFlowInstances : 0, start.displacementSize, start.displacementSize, start.flowSize,
		start.flowSize);
}

/** @brief The start scope a covered effect's draw is a step of now, or None. */
WorldScope StartStepScope()
{
	const auto& start = Start();
	if (!start.on || !start.registered)
		return WorldScope::None;
	return start.scope;
}

/**
 * @brief A ripple's quad as a frame input: the shape DrawTriShape gets, its buffers' view refs written into a_pending's PerFrame words
 * (the ripple's vertex stage pulls the geometry; it reads no PerFrame), and its index count as the draw's count. Empty, or why not.
 */
std::string StageGeometry(Pending& a_pending)
{
	auto& state = IS();
	const auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
	DCLF::Family::Geometry geometry;
	auto owners = std::make_shared<DCLF::Family::Owners>();
	std::string detail;
	const auto result = DCLF::Family::LatchGeometry(static_cast<const DCLF::Family::TriShape*>(state.drawShape), state.drawStart, state.drawTriangles, ~std::uint64_t(0),
		static_cast<std::uint32_t>(shadow.topology), geometry, *owners, &detail);
	if (result != DCLF::Family::GeometryResult::Ok)
		return fmt::format("{} ({})", DCLF::Family::ReasonOf(result), detail);
	if (geometry.firstIndex > 0xFFFF || geometry.stride > 0xFFFF)
		return "its first index or stride does not fit the latch's 16-bit fields";
	a_pending.at.frameSrv = geometry.vertexRef.viewSlot;
	a_pending.at.frameOffset = geometry.vertexRef.byteOffset;
	a_pending.at.frameSize = geometry.indexRef.viewSlot;
	a_pending.at.source = geometry.indexRef.byteOffset;
	a_pending.at.padding = geometry.stride | (geometry.firstIndex << 16);
	a_pending.vertices = geometry.indexCount;
	a_pending.source = std::move(owners);
	return {};
}

/**
 * @brief A start step's data-only hook: the engine computed this step's constants and bound them; they are staged for the step's pass,
 * matched by the effect, the target the water update writes now (it alternates) and the instance (the draw's count of its kind this
 * frame). The ripple's quad and a flow copy's source texture are staged with them. The engine's draw is withheld (true). A step past
 * the declared instances, or one CS has no pass for, is a mismatch.
 */
bool TakeStartStep(std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	auto& state = IS();
	auto& start = Start();
	const auto id = kEffects[a_effect].id;
	using E = Effects;
	const auto segment = Frame::SegmentId::Start;
	const auto frame = Frame::Driver::Current().frame;
	if (start.frame != frame) {
		start.frame = frame;
		start.flows = 0;
	}
	std::uint16_t variant = 0;
	std::uint16_t limit = 1;
	std::uint16_t instance = 0;
	const auto other = [](std::int32_t a_target) {
		return a_target == RE::RENDER_TARGET::kWATER_DISPLACEMENT ? static_cast<std::int32_t>(RE::RENDER_TARGET::kWATER_DISPLACEMENT_SWAP) :
		                                                           static_cast<std::int32_t>(RE::RENDER_TARGET::kWATER_DISPLACEMENT);
	};
	if (start.scope == WorldScope::WaterDisplacement) {
		if (start.target < 0)
			start.target = WaterDisplacementTarget();
		switch (id) {
		case E::ISWaterDisplacementTexOffset:
			variant = static_cast<std::uint16_t>(start.target << 8);
			break;
		case E::ISWaterDisplacementWadingRipple:
			instance = start.ripples++;
			limit = kRippleInstances;
			variant = static_cast<std::uint16_t>((start.target << 8) | instance);
			break;
		case E::ISWaterWadingHeightmap:
			// The pair swaps after the ripples, then after each heightmap step (ImageSpaceEffectWaterDisplacement's Render).
			if (!start.swapped) {
				start.swapped = true;
				start.target = other(start.target);
			}
			instance = start.heightmaps++;
			limit = kHeightmapInstances;
			variant = static_cast<std::uint16_t>((start.target << 8) | instance);
			start.target = other(start.target);
			break;
		default:
			break;
		}
	} else if (start.scope == WorldScope::FlowCopy) {
		instance = start.flows++;
		limit = kFlowInstances;
		variant = instance;
	}
	// The plan against the target the engine binds, checked once a session: a difference means the RE of the update is wrong.
	if (start.scope == WorldScope::WaterDisplacement) {
		const auto bound = static_cast<std::int32_t>(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().renderTargets[0]);
		const auto planned = static_cast<std::int32_t>(variant >> 8);
		static bool logged = false;
		if (bound != planned && !std::exchange(logged, true))
			logger::error("[ORG] ImageSpace: the water update's {} writes rt{}, and CS planned rt{}", kEffects[a_effect].effectName, bound, planned);
	}
	if (instance >= limit) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} instance {} of the frame is past the start stage's {} instances; it draws nothing", kEffects[a_effect].effectName, instance, limit));
		return true;
	}
	const StepKey key{ start.scope, id, variant };
	const auto found = start.entries.find(key);
	if (found == start.entries.end()) {
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} (variant {:#x}) in the water effects has no CS pass; it draws nothing", kEffects[a_effect].effectName, variant));
		return true;
	}
	const auto entry = found->second;
	auto& client = state.post[static_cast<std::size_t>(PostPart::Start)];
	const auto& rows = Frame::Driver::Current().Segment(segment).rows;
	const bool scheduled = entry < 256 && (rows[entry / 64] >> (entry % 64) & 1u) != 0 && FrameAnchors::CanTap(client.id);
	if (!start.made[entry] || !scheduled) {
		// The frame's schedule does not hold the client yet (its first build): nothing is drawn this frame.
		Frame::Driver::NoteLaggedWork(segment);
		return true;
	}
	const auto before = client.batch.size();
	StageStep(client, entry, a_effect, a_compute, a_groups);
	if (client.batch.size() == before)
		return true;
	auto& pending = client.batch.back();
	if (id == E::ISWaterDisplacementWadingRipple) {
		if (const auto why = StageGeometry(pending); !why.empty()) {
			client.batch.pop_back();
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment, fmt::format("a wading ripple's quad could not be staged: {}", why));
		}
	} else if (id == E::ISCopySubRegionCS) {
		// The loaded flow texture, by its heap index: a texture BGSAutoWater loaded from a file and never writes, the immutable class the
		// graph exempts from declarations. Held until the run's GPU work retires.
		auto* view = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().CSTexture[0];
		constexpr std::uint32_t kSourceTag = DCLF::GpuTextures::Tag(DCLF::GpuTextures::Source::FrameSlot, 0);
		auto binding = view ? DCLF::GpuTextures::Get().ResolveBinding(view, kSourceTag) : DCLF::GpuTextures::Binding{};
		if (!view || binding.index == DCLF::GpuTextures::kInvalid) {
			client.batch.pop_back();
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment, "a flow copy's source texture has no heap index");
		} else {
			pending.at.source = binding.index;
			pending.source = std::move(binding.owner);
		}
	}
	return true;
}

/** @brief The water update's hook (31385 / 32176): its draws are start steps, from the target the water system holds at its entry. */
struct WaterUpdateGuard
{
	WaterUpdateGuard() : active(Start().on && Start().registered && FrameAnchors::IsRenderOwner())
	{
		if (!active)
			return;
		auto& start = Start();
		start.scope = WorldScope::WaterDisplacement;
		start.target = -1;  // read at the first draw: the update may reset the pair before it renders
		start.swapped = false;
		start.ripples = 0;
		start.heightmaps = 0;
	}
	~WaterUpdateGuard()
	{
		if (active)
			Start().scope = WorldScope::None;
	}
	bool active = false;
};

/** @brief A flow copy's call: its dispatch is the frame's next flow copy instance. */
struct FlowCopyGuard
{
	explicit FlowCopyGuard(bool a_flowCopy) : active(a_flowCopy && Start().on && Start().registered && FrameAnchors::IsRenderOwner())
	{
		if (active) {
			previous = Start().scope;
			Start().scope = WorldScope::FlowCopy;
		}
	}
	~FlowCopyGuard()
	{
		if (active)
			Start().scope = previous;
	}
	bool active = false;
	WorldScope previous = WorldScope::None;
};
