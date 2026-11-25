// The start stage's water effects as CS passes, in graph mode. Included by ImageSpace.cpp after WorldSteps.inl and ChainSteps.inl.
//
// Main::RenderWaterEffects runs the displacement simulation, the flow copies and the underwater mask inside the start stage's window.
// How many ripples, heightmap steps and flow copies a frame makes is the engine's late decision. So each is a set of
// instances, all on, and the k-th draw of its kind is instance k. Unused instances stay closed in the run gate.
// The heightmap steps' count is the engine's bound from its timer settings (HeightmapStepBound), part of the shape.

/** @brief Instances of each repeated water step a frame. More in one frame is a mismatch. */
constexpr std::uint16_t kRippleInstances = 8;  // per target of the pair
constexpr std::uint16_t kFlowInstances = 32;
constexpr std::uint16_t kUnderwaterMaskInstances = 16;  // the water shapes the mask draws in a frame

/** @brief The water update's heightmap step in seconds (0x3ccccccd, set by ImageSpaceEffectWaterDisplacement's Render). */
constexpr double kHeightmapStep = 0.025;

/**
* @brief The most heightmap steps one water update can run (ImageSpaceEffectWaterDisplacement's Render, SE 0x141303da0,
* AE 0x1414ef370). The update adds the frame's game delta to its clock, then steps while the clock is more than one step
* ahead of the steps. So it carries at most one step into a frame and runs at most ceil(delta / step) steps.
* The delta comes from BSTimer::Update (66987 / 68244), the same on SE and AE:
* - each sample is the fixed step in fixed mode (iFPSClamp), else the frame's time, set to 1/6 s past 166.7 ms;
* - with bCompensateUnstableFrameTime (on by default), the delta is the mean of the last samples without the smallest,
*   and without the largest clamped to 0.05 s. With all samples at the largest value s, that is ((n - 1) s - min(s, 0.05)) / (n - 2);
* - the global time multiplier then scales it.
* One step more covers the float rounding of the update's clock.
*/
std::uint32_t HeightmapStepBound()
{
	const auto* timer = RE::BSTimer::GetSingleton();
	const double sample = timer->clamp > 0.0f ? static_cast<double>(timer->clamp) * 0.001 : 1.0 / 6.0;
	double delta = sample;
	const std::uint32_t history = timer->smoothingSampleCount;
	if (history >= 3)
		delta = ((history - 1) * sample - std::min(sample, 0.05)) / (history - 2);
	// The game only slows time. A faster multiplier counts as its next whole multiple, so ramps do not change the bound.
	const float multiplier = std::max({ 1.0f, RE::BSTimer::QGlobalTimeMultiplier(), RE::BSTimer::QGlobalTimeMultiplierTarget() });
	delta *= std::ceil(static_cast<double>(multiplier));
	// Past the gate's entries the build reports it. The clamp only keeps the cast defined.
	const double steps = std::min(std::ceil(delta / kHeightmapStep) + 1.0, static_cast<double>(kGateEntries));
	return static_cast<std::uint32_t>(steps);
}

/** @brief What the start steps' registration is a function of. Its client is declared on these variables (Frame/Declare.h). */
struct StartKey
{
	std::uint32_t displacementSize = 0, flowCopySize = 0;
	std::uint64_t flowMap = 0;
	std::uint32_t effects = 0, states = 0;
	std::uint32_t heightmaps = 0;
	friend bool operator==(const StartKey&, const StartKey&) = default;
};

/** @brief The start steps' build for a key. */
struct StartBuild : StepBuild<StartKey, 1>
{
	std::uint32_t displacementSize = 0, flowSize = 0;
	std::uint16_t heightmapInstances = 0;  // per target, from the key
};

struct StartSteps
{
	StepGroup<StartBuild> group;
	// The target the next step writes, whether the pair swapped after the ripples, and each step's count so far.
	WorldScope scope = WorldScope::None;
	std::int32_t target = -1;
	bool swapped = false;
	std::uint16_t ripples = 0, heightmaps = 0, flows = 0, masks = 0;
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
	if (!shape || !IS().on || !shape->displacementSize)
		return std::nullopt;
	return StartKey{ shape->displacementSize, shape->flowCopySize, shape->flowMap, shape->effects, shape->states, shape->heightmapSteps };
}

void BuildStartSteps(StartBuild& a_build);

/** @brief Setup. The start client, declared on the engine's ImageSpace shape. */
void DeclareStartSteps()
{
	Start().group.Declare({ PostPart::Start }, Frame::DependsOn(Frame::Vars::EngineImageSpace()), [](const Frame::Inputs& a_in) { return StartKeyOf(a_in); },
		[](StartBuild& a_build) { BuildStartSteps(a_build); });
}

/** @brief All on. Each pass only works where the engine opens its gate entry. */
void StartRows(const StepPart& a_part, std::array<std::uint64_t, 4>& a_rows)
{
	for (std::uint32_t entry = 0; entry < a_part.count; ++entry)
		if (const auto row = a_part.RowOf(entry); row < 256)
			a_rows[row / 64] |= 1ull << (row % 64);
}

/**
* @brief The start client's registration for a_build's key. Its declaration registers it when the revision starts, and the
* revision switches its build in. Render thread.
*/
void BuildStartSteps(StartBuild& a_build)
{
	auto& state = IS();
	const auto& a_key = a_build.key;
	const auto& table = a_build.table;
	auto& part = table->parts[0];
	using E = Effects;
	using T = RE::RENDER_TARGET;
	a_build.displacementSize = a_key.displacementSize;
	a_build.flowSize = a_key.flowCopySize;
	a_build.heightmapInstances = static_cast<std::uint16_t>(a_key.heightmaps);
	// The first texture seen in the flow map's slot is published for the water draws: Unified Water's, or the engine's
	// when Setup did not make one. The id is published once, so a later texture in the slot is fatal (PublishedResources).
	if (a_key.flowMap && a_key.flowMap != reinterpret_cast<std::uint64_t>(state.publishedFlowMap)) {
		logger::info("[ORG] ImageSpace: {} the flow map's slot; it is published{}", state.publishedFlowMap ? "another texture took" : "a texture is in",
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
		for (std::uint16_t h = 0; h < a_build.heightmapInstances; ++h) {
			StepDesc d{ .name = fmt::format("water.heightmap.{}.{}", out, h), .output = engine(out) };
			d.inputs[0] = engine(in);
			d.samplers[0] = kLinear;
			add(WorldScope::WaterDisplacement, E::ISWaterWadingHeightmap, static_cast<std::uint16_t>(high | h), std::move(d));
		}
	}
	// Each reads its loaded texture by heap index, with no declared input.
	if (a_build.flowSize)
		for (std::uint16_t k = 0; k < kFlowInstances; ++k)
			add(WorldScope::FlowCopy, E::ISCopySubRegionCS, k, StepDesc{ .name = fmt::format("flow.copy.{}", k), .output = engine(kFlowMapOutput) });
	// The underwater mask: one draw per water shape, of the engine's geometry (StageGeometry), reading nothing.
	for (std::uint16_t k = 0; k < kUnderwaterMaskInstances; ++k)
		add(WorldScope::UnderwaterMask, E::ISUnderwaterMask, k,
			StepDesc{ .name = fmt::format("water.underwater-mask.{}", k), .output = engine(static_cast<std::int32_t>(T::kUNDERWATER_MASK)) });

	auto& client = state.post[static_cast<std::size_t>(PostPart::Start)];
	client.batch.clear();
	std::vector<FrameAnchors::PassDesc> passes;
	for (auto& step : steps) {
		if (part.count >= kGateEntries) {
			logger::error("[ORG] ImageSpace: the start steps do not fit the gate's {} entries ({} heightmap steps per target); {} and later have no pass",
				kGateEntries, a_build.heightmapInstances, step.desc.name);
			break;
		}
		std::string error;
		auto spec = MakeStepSpec(step.desc, part.count, error);
		if (!spec) {
			logger::info("[ORG] ImageSpace: start step {} has no pass ({})", step.desc.name, error);
			continue;
		}
		// Most instances are empty most frames, so they record at execution and unused ones skip and fold their barriers.
		const auto entry = AddStep(*table, 0, step.key, spec);
		passes.push_back(StepPassDesc(table, 0, entry, client, spec));
	}
	auto desc = PartClientDesc(static_cast<std::size_t>(PostPart::Start), std::move(passes));
	desc.rows = [rows = std::shared_ptr<const StepTable<1>>(table)](std::array<std::uint64_t, 4>& a_rows) { StartRows(rows->parts[0], a_rows); };
	// Each water update and flow copy taps its own instances, and the frame's taps add up.
	desc.repeats = true;
	desc.segmentOnly = true;
	// An empty client is no client. Its declaration then registers none.
	if (!desc.passes.empty())
		a_build.descs[0] = std::move(desc);
	logger::info("[ORG] ImageSpace: the water effects are start stage passes: {} passes ({} ripples and {} heightmap steps per target, {} flow copies, "
				 "{} underwater mask draws), displacement {}x{}, flow map {}x{}, built for the revision starting now",
		part.count, kRippleInstances, a_build.heightmapInstances, a_build.flowSize ? kFlowInstances : 0, kUnderwaterMaskInstances, a_build.displacementSize,
		a_build.displacementSize, a_build.flowSize, a_build.flowSize);
}

WorldScope StartStepScope()
{
	const auto& start = Start();
	if (!IS().on || !start.group.Registered())
		return WorldScope::None;
	// The underwater mask has no scope of its own: its effect is the step, wherever Main::RenderWaterEffects draws it.
	if (start.scope == WorldScope::None && IS().current && kEffects[IS().current->effect].id == Effects::ISUnderwaterMask)
		return WorldScope::UnderwaterMask;
	return start.scope;
}

/** @brief Writes a geometry step's buffer refs (a ripple's quad, a water shape the underwater mask draws) into the PerFrame words for its vertex stage. Returns why not, or empty. */
std::string StageGeometry(Pending& a_pending)
{
	auto& state = IS();
	auto& native = NativeSteps::Get();
	const auto* shape = static_cast<const DCLF::Family::TriShape*>(state.drawShape);
	std::uint32_t start = state.drawStart, triangles = state.drawTriangles;
	std::uint32_t topology = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
	if (native.armed) {
		// Native: the quad BuildShape read from the geometry the engine passed to Render.
		shape = static_cast<const DCLF::Family::TriShape*>(native.sprite.shape);
		start = 0;
		triangles = native.sprite.triangles;
	} else {
		topology = static_cast<std::uint32_t>(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().topology);
		if (native.sprite.shape && (native.sprite.shape != state.drawShape || state.drawStart != 0 || native.sprite.triangles != state.drawTriangles ||
										topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)) {
			++native.differed;
			NativeSteps::Differences().Report("ISWaterDisplacementWadingRipple quad", fmt::format("computed {} 0 {} vs captured {} {} {} topology {}", native.sprite.shape,
				native.sprite.triangles, state.drawShape, state.drawStart, state.drawTriangles, topology));
		}
	}
	DCLF::Family::Geometry geometry;
	auto owners = std::make_shared<DCLF::Family::Owners>();
	std::string detail;
	const auto result = DCLF::Family::LatchGeometry(shape, start, triangles, ~std::uint64_t(0), topology, geometry, *owners, &detail);
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
* @brief Stages the step's constants, matched by effect, the current target and the instance number, in the build the frame runs.
* A step past the build's instances, or with no pass, is a mismatch unless the engine draws for newer variables.
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
		start.masks = 0;
	}
	const auto scope = StartStepScope();
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
			variant = static_cast<std::uint16_t>((start.target << 8) | instance);
			start.target = other(start.target);
			break;
		default:
			break;
		}
	} else if (scope == WorldScope::FlowCopy) {
		instance = start.flows++;
		limit = kFlowInstances;
		variant = instance;
	} else if (scope == WorldScope::UnderwaterMask) {
		instance = start.masks++;
		limit = kUnderwaterMaskInstances;
		variant = instance;
	}
	// Checked once a session. A difference means our RE of the update is wrong.
	if (start.scope == WorldScope::WaterDisplacement && !NativeSteps::Get().armed) {
		const auto bound = static_cast<std::int32_t>(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().renderTargets[0]);
		const auto planned = static_cast<std::int32_t>(variant >> 8);
		static bool logged = false;
		if (bound != planned && !std::exchange(logged, true))
			logger::error("[ORG] ImageSpace: the water update's {} writes rt{}, and CS planned rt{}", kEffects[a_effect].effectName, bound, planned);
	}
	auto& client = state.post[static_cast<std::size_t>(PostPart::Start)];
	const auto what = [&] { return fmt::format("water step {} (variant {:#x})", kEffects[a_effect].effectName, variant); };
	// The heightmap steps' count is the held build's, from the engine's timer settings it was made for.
	const auto held = start.group.Held(0);
	if (start.scope == WorldScope::WaterDisplacement && id == E::ISWaterWadingHeightmap)
		limit = held ? held->heightmapInstances : std::uint16_t{ 0 };
	if (instance >= limit) {
		if (!held) {
			Frame::Driver::NoteStepLag(segment, client.id, what());
			return true;
		}
		// The engine may run more heightmap steps for new timer settings before the revision with them is adopted.
		if (id == E::ISWaterWadingHeightmap && start.group.Ahead(&held->key)) {
			Frame::Driver::NoteLaggedWork(segment);
			return true;
		}
		Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment,
			fmt::format("{} instance {} of the frame is past the start stage's {} instances; it draws nothing", kEffects[a_effect].effectName, instance, limit));
		return true;
	}
	const StepKey key{ scope, id, variant };
	const auto find = start.group.Find(key);
	if (!start.group.Draws(find, segment, what, "in the water effects"))
		return true;
	const auto entry = find.at.entry;
	const auto& part = find.Part();
	// Every row is on, so a row off is a build the key left it out of. A pass not made was left out by its graph build (MakePass).
	if (!RowOn(segment, part.RowOf(entry)) || !part.Made(entry)) {
		Frame::Driver::NoteStepLag(segment, client.id, what());
		return true;
	}
	const auto before = client.batch.size();
	StageStep(client, entry, part.SpecOf(entry), a_effect, a_compute, a_groups);
	if (client.batch.size() == before)
		return true;
	auto& pending = client.batch.back();
	if (id == E::ISWaterDisplacementWadingRipple || id == E::ISUnderwaterMask) {
		if (const auto why = StageGeometry(pending); !why.empty()) {
			client.batch.pop_back();
			Frame::Driver::NoteTapError(Frame::Driver::TapError::NoPass, segment, fmt::format("{}'s geometry could not be staged: {}", kEffects[a_effect].effectName, why));
		}
	} else if (id == E::ISCopySubRegionCS) {
		// BGSAutoWater loads these and never writes them, so the graph exempts them from declarations. Held until the run retires.
		auto* view = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().CSTexture[0];
		auto binding = view ? ::DCLF::GpuTextures::Get().ResolveBinding(view, kImageSpaceTag) : ::DCLF::GpuTextures::Binding{};
		if (!binding) {
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
	WaterUpdateGuard() : active(IS().on && Start().group.Registered() && FrameAnchors::IsRenderOwner())
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
	explicit FlowCopyGuard(bool a_flowCopy) : active(a_flowCopy && IS().on && Start().group.Registered() && FrameAnchors::IsRenderOwner())
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
