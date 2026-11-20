// The start stage's water effects as CS passes, for native mode. Included by ImageSpace.cpp after WorldSteps.inl and ChainSteps.inl.
//
// Main::RenderWaterEffects runs the displacement simulation and the flow copies inside the start stage's window.
// How many ripples, heightmap steps and flow copies a frame makes is the engine's late decision. So each is a fixed set of
// instances, all on, and the k-th draw of its kind is instance k. Unused instances stay closed in the run gate.

/** @brief Instances of each repeated water step a frame. More in one frame is a mismatch. */
constexpr std::uint16_t kRippleInstances = 8;    // per target of the pair
constexpr std::uint16_t kHeightmapInstances = 4;  // 0.025 s each, so a 0.1 s frame makes four
constexpr std::uint16_t kFlowInstances = 32;

/** @brief What the start steps' registration is a function of. Its client is declared on these variables (Frame/Declare.h). */
struct StartKey
{
	std::uint32_t displacementSize = 0, flowCopySize = 0;
	std::uint64_t flowMap = 0;
	std::uint32_t effects = 0, states = 0;
	friend bool operator==(const StartKey&, const StartKey&) = default;
};

struct StartSteps
{
	bool on = false;          // native mode with the start stage
	bool registered = false;  // a registration was built
	std::optional<StartKey> built;
	std::array<std::optional<FrameAnchors::ClientDesc>, 1> descs;
	Frame::DeclaredClient client;
	std::uint32_t displacementSize = 0, flowSize = 0;
	std::map<StepKey, std::uint32_t> entries;
	std::array<bool, kGateEntries> made{};
	std::uint32_t count = 0;
	// The target the next step writes, whether the pair swapped after the ripples, and each step's count so far.
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

/** @brief SE 0x143283ae0, AE 0x1433dcf10. The update may reset it before rendering, so read it at the first draw. */
std::int32_t WaterDisplacementTarget()
{
	static REL::Relocation<const std::int32_t*> target{ REL::RelocationID(528286, 415236) };
	return *target.get();
}

/** @brief The start steps' key at a revision's values, or none while the engine has no displacement pair. */
template <class Values>
std::optional<StartKey> StartKeyOf(const Values& a_values)
{
	const auto shape = ReadyShape(a_values);
	if (!shape || !Start().on || !shape->displacementSize)
		return std::nullopt;
	return StartKey{ shape->displacementSize, shape->flowCopySize, shape->flowMap, shape->effects, shape->states };
}

void BuildStartSteps(const StartKey& a_key);

/** @brief The start client's registration for a_key. Built once per key. */
const std::optional<FrameAnchors::ClientDesc>* StartDescFor(const StartKey& a_key)
{
	auto& start = Start();
	if (start.built != a_key) {
		start.built = a_key;
		BuildStartSteps(a_key);
	}
	return &start.descs[0];
}

/** @brief Setup. The start client, declared on the engine's ImageSpace shape. False, logged, on failure. */
bool DeclareStartSteps()
{
	auto& start = Start();
	start.client = DeclareStepClient<StartKey>(PostPart::Start, Frame::DependsOn(Frame::Vars::EngineImageSpace()),
		[](const Frame::Inputs& a_in) { return StartKeyOf(a_in); }, [](const StartKey& a_key) { return StartDescFor(a_key); });
	return static_cast<bool>(start.client);
}

/** @brief All on. Each pass only works where the engine opens its gate entry. */
void StartRows(std::array<std::uint64_t, 4>& a_rows)
{
	const auto& start = Start();
	for (std::uint32_t entry = 0; entry < start.count && entry < 256; ++entry)
		a_rows[entry / 64] |= 1ull << (entry % 64);
}

/**
* @brief The start client's registration for a_key. Its declaration registers it when the revision starts, and the revision
* switches its build in. Render thread.
*/
void BuildStartSteps(const StartKey& a_key)
{
	auto& state = IS();
	auto& start = Start();
	start.registered = false;
	start.descs = {};
	start.entries.clear();
	start.made.fill(false);
	start.count = 0;
	using E = Effects;
	using T = RE::RENDER_TARGET;
	start.displacementSize = a_key.displacementSize;
	start.flowSize = a_key.flowCopySize;
	// Another texture in the flow map's slot (Unified Water's) is published in its place, for the water draws.
	// A new resource under the id rebuilds every graph once (step 9 makes it a declared import).
	if (state.publishedFlowMap && a_key.flowMap && a_key.flowMap != reinterpret_cast<std::uint64_t>(state.publishedFlowMap)) {
		logger::info("[ORG] ImageSpace: another texture took the flow map's slot; it is published in its place{}",
			a_key.flowCopySize ? "" : " (it has no UAV, so there are no flow copies)");
		PublishFlowMap();
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
	// The variant's high byte is the target each step writes. The other one is its input.
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
	// Each reads its loaded texture by heap index, with no declared input.
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
		passes.push_back({ .name = fmt::format("{}.{}", client.id, spec->name),
			.anchor = Anchor::ImageSpace,
			.order = static_cast<std::int32_t>(entry),
			.kind = spec->key.compute ? FrameAnchors::PassKind::Compute : FrameAnchors::PassKind::Render,
			.make = [spec, latch = std::shared_ptr<const org::LatchBlock>(client.latch), gate = std::shared_ptr<const RunGate>(client.gate), id = client.id, entry] {
				auto pass = MakePass(spec, latch, gate, id);
				Start().made[entry] = pass != nullptr;
				return pass;
			},
			// Most instances are empty most frames, so record at execution and let unused ones skip and fold their barriers.
			.atExecution = true,
			.gpuGated = false,
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
	// Each water update and flow copy taps its own instances, and the frame's taps add up.
	desc.repeats = true;
	desc.segmentOnly = true;
	// An empty client is no client. Its declaration then registers none.
	if (!desc.passes.empty())
		start.descs[0] = std::move(desc);
	start.registered = true;
	logger::info("[ORG] ImageSpace: the water effects are start stage passes: {} passes ({} ripples and {} heightmap steps per target, {} flow copies), "
				 "displacement {}x{}, flow map {}x{}, registered for the revision starting now",
		start.count, kRippleInstances, kHeightmapInstances, start.flowSize ? kFlowInstances : 0, start.displacementSize, start.displacementSize, start.flowSize,
		start.flowSize);
}

WorldScope StartStepScope()
{
	const auto& start = Start();
	if (!start.on || !start.registered)
		return WorldScope::None;
	return start.scope;
}

/** @brief Writes the ripple quad's buffer refs into the PerFrame words for its vertex stage. Returns why not, or empty. */
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
* @brief Stages the step's constants, matched by effect, the current target and the instance number.
* A step past the declared instances, or with no pass, is a mismatch.
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
			// The pair swaps after the ripples, then after each heightmap step.
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
	// Checked once a session. A difference means our RE of the update is wrong.
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
		// The schedule does not hold the client yet, so nothing is drawn this frame.
		Frame::Driver::NoteStepLag(segment, client.id, fmt::format("water step {} (variant {:#x})", kEffects[a_effect].effectName, variant));
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
		// BGSAutoWater loads these and never writes them, so the graph exempts them from declarations. Held until the run retires.
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

/** @brief The water update (31385 / 32176). */
struct WaterUpdateGuard
{
	WaterUpdateGuard() : active(Start().on && Start().registered && FrameAnchors::IsRenderOwner())
	{
		if (!active)
			return;
		auto& start = Start();
		start.scope = WorldScope::WaterDisplacement;
		start.target = -1;  // the update may reset the pair before it renders
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

/** @brief Each dispatch is the frame's next flow copy instance. */
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
