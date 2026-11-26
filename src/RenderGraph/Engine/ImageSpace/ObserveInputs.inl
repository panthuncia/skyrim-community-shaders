// Temporary: the observe half of observe-then-switch for the snapshot's ImageSpace block (snap5-imagespace.md). In native mode it
// compares the block with the engine read at the old read points and logs the differences per field; the frame still uses the
// engine's values. Included by NativePost.inl. Deleted at the switch.

namespace Observe
{
	struct State
	{
		std::uint64_t framesA = 0, framesB = 0, cameras = 0;
		std::map<std::string, std::uint64_t> diffs;
		std::set<std::string> logged;
		std::uint64_t resetsLive = 0, resetsSnapshot = 0;
		std::uint32_t lastSerial = 0;
	};

	State& S()
	{
		static State state;
		return state;
	}

	template <class T>
	std::string Show(const T& a_value)
	{
		if constexpr (std::is_same_v<T, std::uint8_t>)
			return fmt::format("{:#x}", static_cast<std::uint32_t>(a_value));
		else if constexpr (requires { a_value.size(); a_value[0]; }) {
			std::string out;
			for (const auto& value : a_value)
				out += fmt::format("{}{}", out.empty() ? "" : " ", value);
			return "[" + out + "]";
		}
		else
			return fmt::format("{}", a_value);
	}

	template <class T>
	void Compare(std::string_view a_point, std::string_view a_name, const T& a_engine, const T& a_snapshot)
	{
		if (a_engine == a_snapshot)
			return;
		const auto key = fmt::format("{}.{}", a_point, a_name);
		++S().diffs[key];
		if (S().logged.insert(key).second)
			logger::info("[ORG] ImageSpace observe: first difference {} at frame {}: engine {} snapshot {}", key, Frame::Driver::Current().frame, Show(a_engine),
				Show(a_snapshot));
	}

	void CompareNear(std::string_view a_point, std::string_view a_name, float a_engine, float a_snapshot)
	{
		const float scale = (std::max)({ 1.0f, std::abs(a_engine), std::abs(a_snapshot) });
		if (std::abs(a_engine - a_snapshot) <= 1e-5f * scale)
			return;
		Compare(a_point, a_name, a_engine, a_snapshot);
	}

	void Log()
	{
		auto& s = S();
		std::string diffs;
		for (const auto& [key, count] : s.diffs)
			diffs += fmt::format("{}{}={}", diffs.empty() ? "" : ", ", key, count);
		logger::info("[ORG] ImageSpace observe: {} frames at A, {} at B, {} camera checks; adaptation resets engine {} snapshot {}; differences: {}", s.framesA,
			s.framesB, s.cameras, s.resetsLive, s.resetsSnapshot, diffs.empty() ? std::string("none") : diffs);
	}
}

#define CS_OBSERVE_A(X)                                                                                                                     \
	X(read) X(data) X(filmic) X(worldMap) X(adaptOff) X(stopAt2) X(menu) X(reinhardWhite) X(filmicWhite) X(saturationBoost) X(contrastBoost) \
		X(brightnessBoost) X(adaptSpeed) X(adaptStrength) X(bloomBoost) X(frameTime) X(menuTimer) X(range) X(blurRadius) X(blurBright) X(hdr) \
			X(eof) X(usesLdr) X(sil) X(iblf) X(taaK) X(taaThreshold) X(taaBlend)
#define CS_OBSERVE_B(X)                                                                                                                       \
	X(gameSlots) X(volumetric) X(screenshot) X(frameCounter) X(refractionTint) X(dofCompute) X(underwater) X(underwaterData) X(underwaterDof) \
		X(dofIni) X(dofCustom) X(radialEffect) X(dofDynamic) X(radialDynamic) X(slotBlurRadius) X(slotBlurDynamic) X(fullBlurDynamic)        \
			X(fullBlurLegacy) X(getHitDynamic) X(getHitScale) X(getHitAmount) X(timer)
#define CS_OBSERVE_W(X)                                                                                                                         \
	X(ssrBlur) X(ssrDownsample) X(ssrDivider) X(ssrRay) X(saoPower) X(saoBias) X(saoRadius) X(saoRow3x) X(saoRow3z) X(saoContrast) X(saoCameraZw) \
		X(saoHalf) X(saoContrastOn) X(saoFog) X(saoTemporal) X(improvedSnow) X(saoLevels) X(sparkleToggle) X(sparkleOnly) X(sparkleNoAo)        \
			X(sparkleNoise) X(sparkle) X(sparkleMaxDistance) X(sparkleShadow) X(sparkleColourMode) X(sparkleColour) X(ssrCompositeA) X(ssrCompositeB)

/** @brief At A in native mode: compares the block's A fields and settings with the engine's, then keeps the engine's for the frame. */
void ObserveA(ImageSpaceFrame& a_f, const Block& a_block)
{
	GameInputs engine;
	ReadGameA(engine);
	const auto& snap = a_block.inputs;
#define CS_COMPARE(name) Observe::Compare("A", #name, engine.name, snap.name);
	CS_OBSERVE_A(CS_COMPARE)
#undef CS_COMPARE
	const auto world = ReadWorldSettings();
#define CS_COMPARE(name) Observe::Compare("W", #name, world.name, a_block.world.name);
	CS_OBSERVE_W(CS_COMPARE)
#undef CS_COMPARE
	auto& s = Observe::S();
	s.resetsLive += *G().adaptReset ? 1 : 0;
	s.resetsSnapshot += a_block.adaptResets != s.lastSerial ? 1 : 0;
	s.lastSerial = a_block.adaptResets;
	++s.framesA;
	if (Frame::Driver::Current().frame % 300 == 0)
		Observe::Log();
	ReadGameA(a_f);
}

/** @brief A render-time camera value against the snapshot's main camera, for the fields that stay render reads. */
void ObserveCamera(std::string_view a_point, std::uintptr_t a_camera)
{
	const auto& snapshot = Snapshot::Current();
	if (!snapshot || !a_camera || !RenderGraph::Engine::Native::Requested())
		return;
	const auto& view = snapshot->cameras.Current(Snapshot::ViewId::Main);
	if (!view.valid)
		return;
	++Observe::S().cameras;
	Observe::CompareNear(a_point, "near", Field<float>(a_camera, 0x160), view.frustum.fNear);
	Observe::CompareNear(a_point, "far", Field<float>(a_camera, 0x164), view.frustum.fFar);
	Observe::CompareNear(a_point, "left", Field<float>(a_camera, 0x150), view.frustum.fLeft);
	Observe::CompareNear(a_point, "right", Field<float>(a_camera, 0x154), view.frustum.fRight);
	Observe::CompareNear(a_point, "top", Field<float>(a_camera, 0x158), view.frustum.fTop);
	Observe::CompareNear(a_point, "bottom", Field<float>(a_camera, 0x15c), view.frustum.fBottom);
	Observe::CompareNear(a_point, "x", Field<float>(a_camera, 0xa0), view.position.x);
	Observe::CompareNear(a_point, "y", Field<float>(a_camera, 0xa4), view.position.y);
	Observe::CompareNear(a_point, "z", Field<float>(a_camera, 0xa8), view.position.z);
}

/** @brief At B in native mode: compares the block's B fields with the engine's, then keeps the engine's for the frame. */
void ObserveB(ImageSpaceFrame& a_f)
{
	const auto* block = NativeBlock();
	GameInputs engine;
	engine.eof = a_f.eof;
	ReadGameB(engine);
	const auto& snap = block->inputs;
#define CS_COMPARE(name) Observe::Compare("B", #name, engine.name, snap.name);
	CS_OBSERVE_B(CS_COMPARE)
#undef CS_COMPARE
	const auto& g = G();
	if (*g.worldSceneGraph)
		ObserveCamera("B.world-camera", Field<std::uintptr_t>(*g.worldSceneGraph, 0x128));
	ObserveCamera("B.current-camera", reinterpret_cast<std::uintptr_t>(*g.currentCamera));
	if (*g.dofCamera)
		ObserveCamera("B.dof-camera", *g.dofCamera);
	++Observe::S().framesB;
	ReadGameB(a_f);
	a_f.slots = a_f.gameSlots | (a_f.slots & (1u << 1));
	R().lastSlots = a_f.slots;
}
