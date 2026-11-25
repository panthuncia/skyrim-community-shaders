// The native post chain (docs/development/plans/native-post-chain.md), families 0 to 4. Included by ImageSpace.cpp after the step
// files and the scope helpers, before the hooks that use it.
//
// Family 0 is the snapshot (ImageSpaceFrame), read once at Renderer::Begin (read point A) and at RenderWorldPostProcess's entry
// (read point B), and CS's post state (the parities and the adapted luminance). Families 1 to 4 are the HDR effect, the end-of-frame
// effects, the pre-tonemap composites and RenderWorldPostProcess's body. Each has three modes (CS_ORG_POST_NATIVE): the engine's
// container runs; observe, where the container runs and CS predicts each step's key and rows and the state after it, and logs a
// difference once; and native, where the container is not called and CS stages the steps from the snapshot and writes back what
// engine code after the chain reads.
//
// The globals are SE/AE pairs. AE runs natively by default. SE's ids were matched statically (perf-post-slots.md) but never run, so
// SE runs every family on the engine unless CS_ORG_POST_NATIVE is set.

namespace NativePost
{
	enum class Mode : std::uint8_t
	{
		Engine,
		Observe,
		Native,
	};

	namespace Family
	{
		constexpr std::uint8_t kHdr = 1;
		constexpr std::uint8_t kEof = 2;
		constexpr std::uint8_t kComposites = 4;
		constexpr std::uint8_t kPost = 8;
		constexpr std::uint8_t kAll = kHdr | kEof | kComposites | kPost;
	}

	struct Config
	{
		Mode mode = Mode::Engine;
		std::uint8_t families = 0;
		// Families whose container or effect holds another mod's hook. They stay on the engine path (decision 4).
		std::uint8_t foreign = 0;
		// Slot effects whose Render or UpdateParams holds another mod's hook: the engine's RenderEffect draws them.
		std::uint8_t foreignSlots = 0;
	};

	Config& Settings()
	{
		static Config config;
		return config;
	}

	bool In(Mode a_mode, std::uint8_t a_family)
	{
		const auto& config = Settings();
		return config.mode == a_mode && (config.families & a_family) && !(config.foreign & a_family);
	}

	/** @brief The engine globals the families read and write back, resolved once at Install. The comments give the AE ids. */
	struct Globals
	{
		// HDR settings (:Display) and their world map forms.
		const bool* filmic = nullptr;              // 391339 bUseFilmicCurve
		const float* reinhardWhite = nullptr;      // 391342
		const float* filmicWhite = nullptr;        // 391345
		const float* saturationBoost = nullptr;    // 391312
		const float* contrastBoost = nullptr;      // 391305, map 391324
		const float* contrastBoostMap = nullptr;
		const float* brightnessBoost = nullptr;    // 391309, map 391321
		const float* brightnessBoostMap = nullptr;
		const float* adaptSpeed = nullptr;         // 391330 fGlobalEyeAdaptSpeedScale
		const float* adaptStrength = nullptr;      // 391333 fGlobalEyeAdaptStrengthScale
		const float* menuTimer = nullptr;          // 391327 fConstHDRAdaptTimerForMenu
		const float* bloomBoost = nullptr;         // 391315 fGlobalBloomThresholdBoost, map 391318
		const float* bloomBoostMap = nullptr;
		const bool* stopAt2 = nullptr;             // 391336: the luminance chain stops at 2x2
		const bool* worldMap = nullptr;            // 415052
		const bool* adaptOff = nullptr;            // 415051: no adaptation, and the chain goes to 1x1
		bool* adaptReset = nullptr;                // 415053: Sky::Update sets it, the HDR effect clears it
		const float* frameTime = nullptr;          // 390954
		const float* range = nullptr;              // 390973, the framebuffer range
		// TAA.
		bool* taaReset = nullptr;                  // 391591: ConfigureTAA and the loading menu set it
		const float* taaK = nullptr;               // 391592
		const float* taaBlend = nullptr;           // 391593, four floats
		const float* taaThreshold = nullptr;       // 391611 fTAAEffectThreshold
		// State the HDR effect keeps.
		std::int32_t* lumParity = nullptr;         // 415054
		RE::ImageSpaceTexture* adapted = nullptr;  // 391302, its target at +8 (391303)
		// The bloom blur's weights by radius (0x142033cf8: 0xF0 bytes per radius, a float4 per tap, the weight in x).
		const std::byte* blurWeights = nullptr;    // 391307 - 0xE8
		const bool* eof = nullptr;                 // 391055 gEnableEOFImageSpace
		const bool* hdr = nullptr;                 // 391301
		const bool* volumetric = nullptr;          // 414913
		RE::NiCamera** currentCamera = nullptr;    // 414580
		const std::uintptr_t* worldSceneGraph = nullptr;  // 415032, its camera at +0x128
		const std::uintptr_t* dofCamera = nullptr;  // 415007, the camera whose near and far the depth of field takes
		const std::uint32_t* frameCounter = nullptr;  // 411489
		const RE::BSGraphics::RenderTargetManager* targets = nullptr;  // 411451, the object itself
		// Engine functions: the VL settings and their colour, ApplyVL, the VL blur, the lens flare gather, the composites,
		// SetRenderTarget and SetCurrentDepthStencilTarget.
		std::uintptr_t (*vlSettings)() = nullptr;                                          // 107014
		void (*vlColour)(std::uintptr_t, float*, float*, float*) = nullptr;                // 107017
		std::uint32_t (*applyVl)(std::uintptr_t, std::uint32_t, std::uint32_t) = nullptr;  // 107193
		std::uint32_t (*blurVl)(std::uintptr_t, std::uint32_t, std::uint32_t, std::uint32_t) = nullptr;  // 107206
		void (*lensFlares)(RE::NiCamera*) = nullptr;                                       // 106988
		void (*composites)(void*, std::uintptr_t, std::uintptr_t) = nullptr;              // 105675
		void (*setTarget)(const void*, std::uint32_t, std::int32_t, std::uint32_t, bool) = nullptr;  // 77453
		void (*setDepth)(const void*, std::int32_t, std::uint32_t, bool) = nullptr;       // 77454
		std::uintptr_t eofCallSite = 0;            // 107148 + 0x1E7, a call to RenderEndOfFrameEffects that Upscaling and HDR hook
		// The slot effects.
		bool* refractionDrawn = nullptr;           // 415291 and 415292: refraction geometry was drawn; IsActive reads +1, Render clears both
		const std::int32_t* underwater = nullptr;  // 391008: 1 or 2 under water, which picks the depth of field's underwater forms
		std::array<const float*, 7> dofIni{};      // 415121 to 415127, the depth of field's INI settings
		std::array<const float*, 7> dofCustom{};   // 415136 to 415142, the depth of field's custom range (mode bit 0x80)
		const bool* dofCompute = nullptr;          // 391684: the depth of field blurs with its compute shaders
		bool* underwaterMaskWritten = nullptr;     // 391616: the underwater mask was drawn; the depth of field's Render clears it
		// The full-screen blur and get hit.
		const float* blurCamera = nullptr;         // 415441: the world camera's values Main::RenderWorld keeps (three floats), (1, 1, 1) when reset
		const bool* blurCameraMoved = nullptr;     // 415422: the camera moved this frame; the full-screen blur then blurs at radius 2 or more
		const bool* blurCameraGate = nullptr;      // 414598: Main::RenderWorld tracks the camera for the blur
		const float* getHitScale = nullptr;        // 415419
		const float* getHitAmount = nullptr;       // 415420
		const float* timer = nullptr;              // 390953: the timer get hit's double vision turns with
		bool ready = false;
	};

	Globals& G()
	{
		static Globals globals;
		return globals;
	}

	template <class T>
	T* At(std::uint64_t a_se, std::uint64_t a_ae, std::ptrdiff_t a_offset = 0)
	{
		return reinterpret_cast<T*>(REL::RelocationID(a_se, a_ae).address() + a_offset);
	}

	void ResolveGlobals()
	{
		auto& g = G();
		g.filmic = At<bool>(513490, 391339);
		g.reinhardWhite = At<float>(513492, 391342);
		g.filmicWhite = At<float>(513494, 391345);
		g.saturationBoost = At<float>(513472, 391312);
		g.contrastBoost = At<float>(513467, 391305);
		g.contrastBoostMap = At<float>(513480, 391324);
		g.brightnessBoost = At<float>(513470, 391309);
		g.brightnessBoostMap = At<float>(513478, 391321);
		g.adaptSpeed = At<float>(513484, 391330);
		g.adaptStrength = At<float>(513486, 391333);
		g.menuTimer = At<float>(513482, 391327);
		g.bloomBoost = At<float>(513474, 391315);
		g.bloomBoostMap = At<float>(513476, 391318);
		g.stopAt2 = At<bool>(513488, 391336);
		g.worldMap = At<bool>(528107, 415052);
		g.adaptOff = At<bool>(528106, 415051);
		g.adaptReset = At<bool>(528108, 415053);
		g.frameTime = At<float>(513214, 390954);
		g.range = At<float>(513233, 390973);
		g.taaReset = At<bool>(513663, 391591);
		g.taaK = At<float>(513664, 391592);
		g.taaBlend = At<float>(513665, 391593);
		g.taaThreshold = At<float>(513677, 391611);
		g.lumParity = At<std::int32_t>(528109, 415054);
		g.adapted = At<RE::ImageSpaceTexture>(513464, 391302);
		g.blurWeights = At<std::byte>(513468, 391307, -0xE8);
		g.eof = At<bool>(513299, 391055);
		g.hdr = At<bool>(513463, 391301);
		g.volumetric = At<bool>(527940, 414913);
		g.currentCamera = At<RE::NiCamera*>(527667, 414580);
		g.worldSceneGraph = At<std::uintptr_t>(528087, 415032);
		g.dofCamera = At<std::uintptr_t>(528062, 415007);
		g.frameCounter = At<std::uint32_t>(525008, 411489);
		g.targets = At<RE::BSGraphics::RenderTargetManager>(524970, 411451);
		g.vlSettings = reinterpret_cast<decltype(g.vlSettings)>(REL::RelocationID(100297, 107014).address());
		g.vlColour = reinterpret_cast<decltype(g.vlColour)>(REL::RelocationID(100300, 107017).address());
		g.applyVl = reinterpret_cast<decltype(g.applyVl)>(REL::RelocationID(100475, 107193).address());
		g.blurVl = reinterpret_cast<decltype(g.blurVl)>(REL::RelocationID(100486, 107206).address());
		g.lensFlares = reinterpret_cast<decltype(g.lensFlares)>(REL::RelocationID(100274, 106988).address());
		g.composites = reinterpret_cast<decltype(g.composites)>(REL::RelocationID(99024, 105675).address());
		g.setTarget = reinterpret_cast<decltype(g.setTarget)>(REL::RelocationID(75646, 77453).address());
		g.setDepth = reinterpret_cast<decltype(g.setDepth)>(REL::RelocationID(75647, 77454).address());
		g.eofCallSite = REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7);
		g.refractionDrawn = At<bool>(528345, 415291);
		g.underwater = At<std::int32_t>(513267, 391008);
		for (std::uint64_t i = 0; i < 7; ++i) {
			g.dofIni[i] = At<float>(528176 + i, 415121 + i);
			g.dofCustom[i] = At<float>(528191 + i, 415136 + i);
		}
		g.dofCompute = At<bool>(513727, 391684);
		g.underwaterMaskWritten = At<bool>(513680, 391616);
		g.blurCamera = At<float>(528495, 415441);
		g.blurCameraMoved = At<bool>(528476, 415422);
		g.blurCameraGate = At<bool>(527646, 414598);
		g.getHitScale = At<float>(528473, 415419);
		g.getHitAmount = At<float>(528474, 415420);
		g.timer = At<float>(513213, 390953);
		g.ready = true;
	}

	// Code constants of the engine's formulas (AE .rdata, read in Ghidra).
	constexpr float kFilmicBias = 0.004f;    // 0x141ad2814
	constexpr float kFilmicA = 6.2f;         // 0x141abb4c0
	constexpr float kFilmicB = 0.5f;         // 0x141ad285c
	constexpr float kFilmicC = 1.7f;         // 0x14179b0e4
	constexpr float kFilmicD = 0.06f;        // 0x141abb4b8
	constexpr float kFilmicPower = -2.2f;    // 0x141abb4c4
	constexpr float kAdaptRate = 0.01f;      // 0x141ad2818
	constexpr float kAdaptFrames = 30.0f;    // 0x141ad28e0
	constexpr float kBlurCentre = 7.0f;      // 0x1417a6970
	constexpr float kDofEpsilon = 1.1920929e-07f;  // 0x1417afdc0, FLT_EPSILON
	constexpr float kDofNoRange = -1.0e8f;   // 0x141abf108
	// The depth of field's compute blur shaders' thread groups (ISBlurCS.hlsl's GRIDSIZE_X and GRIDSIZE_Y for 112 and 113).
	constexpr std::uint32_t kDofGridX = 480, kDofGridY = 270;
	constexpr float kGetHitTurn = 0.0017453278f;  // 0x141ac58d0: the timer to the double vision's angle (SE: / 3600 * 2 pi)
	constexpr float kGetHitRadius = 2.0f;          // 0x141ad288c: get hit's blurs take radius 2's weights

	/** @brief ImageSpaceManager +0xC8 to +0x15C: the blended ImageSpace data (HDR, cinematic, tint, fade, blurs, DoF). */
	constexpr std::uintptr_t kDataFirst = 0xC8, kDataEnd = 0x15C;

	/** @brief The snapshot. Values only, no pointers. "A" fields are read at Renderer::Begin, "B" at the post chain's entry. */
	struct ImageSpaceFrame
	{
		bool read = false;  // A: the manager exists (AE)
		bool readB = false;
		std::array<float, (kDataEnd - kDataFirst) / 4> data{};  // A
		// A: settings.
		bool filmic = false, worldMap = false, adaptOff = false, stopAt2 = false, menu = false;
		float reinhardWhite = 0, filmicWhite = 0, saturationBoost = 0, contrastBoost = 0, brightnessBoost = 0, adaptSpeed = 0, adaptStrength = 0, bloomBoost = 0;
		float frameTime = 0, menuTimer = 0, range = 1;
		// A: the blur effects of the bloom (effects 0x17 + r): their radius (+0x90) and bright pass flag (+0x94).
		std::array<std::int32_t, 7> blurRadius{};
		std::array<bool, 7> blurBright{};
		// A: target sizes, from the render target manager.
		std::array<std::pair<std::uint32_t, std::uint32_t>, RE::RENDER_TARGET::kTOTAL> sizes{};
		float ratioX = 1, ratioY = 1;
		bool locked = false;
		bool hdr = false, eof = false, usesLdr = false, volumetric = false /* B */, sil = false, iblf = false, upsample = false;
		std::array<float, 3> vlColour{};  // B
		float taaK = 1, taaThreshold = 0;
		std::array<float, 4> taaBlend{};
		// B.
		std::uint8_t slots = 0;          // active end-of-frame slots: refraction turns on mid-frame
		std::uint32_t vlParity = 0;      // VL Render bumps the counter during the frame
		float cameraNear = 0, cameraFar = 0;  // the world camera, current at the end of frame
		std::array<float, 2> jitter{};   // CS's (PostChain::Jitter)
		bool screenshot = false;         // a save asks with a frame's notice
		std::uint32_t frameCounter = 0;
		// B: the slot effects' inputs.
		std::array<float, 4> refractionTint{};  // manager +0x98
		std::array<bool, 5> dofSteps{};         // the depth of field's step flags (its +0x98 array): downsample, blur, composites 2 to 4
		bool dofCompute = false;
		float dofNear = 0, dofFar = 0;          // the camera RenderWorldPostProcess copies into the depth of field (+0x14C, +0x150)
		std::int32_t underwater = 0;
		bool underwaterData = false;            // manager +0xB8, the underwater depth of field
		std::array<float, 4> underwaterDof{};   // its +0x40 to +0x4C
		std::array<float, 7> dofIni{}, dofCustom{};
		std::int32_t radialEffect = -1;         // the radial blur's shader (by quality), a kEffects index
		bool dofDynamic = false, radialDynamic = false;  // the depth of field's and radial blur's ImageSpaceEffect +0x88
		// B: the full-screen blur (slot 6) and get hit (slot 7). Their blurs are effects 9 to 15: radius (+0x90) and flag (+0x88).
		std::array<std::int32_t, 7> slotBlurRadius{};
		std::array<bool, 7> slotBlurDynamic{};
		bool fullBlurDynamic = false, fullBlurLegacy = false, fullBlurAtLeast2 = false;
		bool getHitDynamic = false;
		float getHitScale = 0, getHitAmount = 0, timer = 0;
		// The end-of-frame entry: Upscaling's call-site hook turns TAA on just before it, and sets the dynamic resolution.
		bool taa = false;
		std::int32_t taaMode = 0;
		bool taaWater = false;

		float D(std::uintptr_t a_offset) const { return data[(a_offset - kDataFirst) / 4]; }
		std::pair<float, float> Size(std::int32_t a_target) const
		{
			if (a_target < 0 || a_target >= static_cast<std::int32_t>(sizes.size()))
				return { 1.0f, 1.0f };
			return { static_cast<float>(sizes[a_target].first), static_cast<float>(sizes[a_target].second) };
		}
		bool DynamicResolution() const { return !locked && (ratioX != 1.0f || ratioY != 1.0f); }
	};

	/**
	* @brief CS's post state: what the containers kept between frames. It starts each frame from the engine's copy at A, which CS's
	* write-back keeps current, so a frame whose family runs on the engine continues it.
	*/
	struct PostState
	{
		std::int32_t lumParity = 0;      // flips on the luminance chain's last level
		std::int32_t adaptTarget = -1;   // the adapted luminance's engine target, -1 for none
		bool resetPending = false;       // the adaptation reset
		std::int32_t taaParity = 0;      // TAA object +0x34
		bool operator==(const PostState&) const = default;
	};

	/** @brief A step CS stages: its key, group and rows, and a compute step's thread groups. */
	struct NativeStep
	{
		StepKey key;
		std::uint8_t group = 0;
		std::size_t effect = 0;
		NativeSteps::Rows rows;
		bool compute = false;
		std::array<std::uint32_t, 3> groups{};
	};

	struct Runtime
	{
		ImageSpaceFrame frame;
		PostState state;
		std::uint64_t frameNumber = UINT64_MAX;
		// Observe mode: the steps CS predicts, in order, and the post state it predicts after them.
		std::deque<NativeStep> expected;
		std::optional<PostState> predicted;
		bool predicting = false;
		std::uint64_t compared = 0, differed = 0, staged = 0;
		// Per-family counts of frames run natively and on the engine path, for the log.
		std::array<std::uint64_t, 4> native{}, engine{};
		std::uint64_t slotNative = 0, slotEngine = 0;  // slot effects staged natively, and run through the engine's RenderEffect
		std::uint8_t lastSlots = 0;  // the last B read's, for the log
	};

	Runtime& R()
	{
		static Runtime runtime;
		return runtime;
	}

	Util::ReasonLog& Mismatches()
	{
		static Util::ReasonLog log("[ORG] native post chain: observe found a difference", spdlog::level::warn);
		return log;
	}

	std::uintptr_t ManagerBase()
	{
		return reinterpret_cast<std::uintptr_t>(RE::ImageSpaceManager::GetSingleton());
	}

	template <class T>
	T& Field(std::uintptr_t a_object, std::uintptr_t a_offset)
	{
		return *reinterpret_cast<T*>(a_object + a_offset);
	}

	/** @brief The engine's state copy, which CS writes back. */
	PostState EngineState()
	{
		PostState out;
		const auto& g = G();
		out.lumParity = *g.lumParity;
		out.adaptTarget = g.adapted->renderTarget;
		out.resetPending = *g.adaptReset;
		if (const auto base = ManagerBase())
			if (const auto taa = Field<std::uintptr_t>(base, 0x1f0))
				out.taaParity = Field<std::int32_t>(taa, 0x34);
		return out;
	}

	/** @brief Done once, at the first frame with the ImageSpace manager (ImageSpace.cpp, after the hooks it checks). */
	void CheckForeignHooksOnce();

	/** @brief Read point A, Renderer::Begin. Render owner. */
	void ReadA()
	{
		ZoneScopedN("CS.PostNative.ReadA");
		auto& r = R();
		auto& f = r.frame;
		f = {};
		const auto& g = G();
		const auto base = ManagerBase();
		if (!g.ready || !base || Settings().mode == Mode::Engine)
			return;
		CheckForeignHooksOnce();
		std::memcpy(f.data.data(), reinterpret_cast<const void*>(base + kDataFirst), sizeof(f.data));
		f.filmic = *g.filmic;
		f.worldMap = *g.worldMap;
		f.adaptOff = *g.adaptOff;
		f.stopAt2 = *g.stopAt2;
		auto* ui = RE::UI::GetSingleton();
		f.menu = ui && ui->numPausesGame != 0;
		f.reinhardWhite = *g.reinhardWhite;
		f.filmicWhite = *g.filmicWhite;
		f.saturationBoost = *g.saturationBoost;
		f.contrastBoost = f.worldMap ? *g.contrastBoostMap : *g.contrastBoost;
		f.brightnessBoost = f.worldMap ? *g.brightnessBoostMap : *g.brightnessBoost;
		f.adaptSpeed = *g.adaptSpeed;
		f.adaptStrength = *g.adaptStrength;
		f.bloomBoost = f.worldMap ? *g.bloomBoostMap : *g.bloomBoost;
		f.frameTime = *g.frameTime;
		f.menuTimer = *g.menuTimer;
		f.range = *g.range;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		for (std::uint32_t r7 = 0; r7 < 7; ++r7) {
			const auto index = 0x17u + r7;
			const auto* blur = index < manager->effects.free_idx() ? manager->effects[static_cast<std::uint16_t>(index)] : nullptr;
			f.blurRadius[r7] = blur ? Field<std::int32_t>(reinterpret_cast<std::uintptr_t>(blur), 0x90) : 0;
			f.blurBright[r7] = blur && Field<bool>(reinterpret_cast<std::uintptr_t>(blur), 0x94);
		}
		for (std::uint32_t t = 0; t < f.sizes.size(); ++t)
			f.sizes[t] = { g.targets->renderTargetData[t].width, g.targets->renderTargetData[t].height };
		f.hdr = *g.hdr;
		f.eof = *g.eof;
		f.usesLdr = Field<bool>(base, 0x210);
		if (const auto sil = Field<std::uintptr_t>(base, 0x1d8))
			f.sil = Field<bool>(sil, 0x10);
		if (const auto iblf = Field<std::uintptr_t>(base, 0x180))
			f.iblf = Field<bool>(iblf, 0x30);
		f.taaK = *g.taaK;
		std::memcpy(f.taaBlend.data(), g.taaBlend, sizeof(f.taaBlend));
		f.taaThreshold = *g.taaThreshold;
		f.read = true;
		r.state = EngineState();
		if (Frame::Driver::Current().frame % 300 == 0)
			logger::info("[ORG] native post chain: {} steps compared ({} differed), {} staged natively; native frames hdr {} eof {} composites {} post {}, "
						 "end-of-frame frames on the engine {} (last slots {:#x}); slot effects native {}, through RenderEffect {}",
				r.compared, r.differed, r.staged, r.native[0], r.native[1], r.native[2], r.native[3], r.engine[1], r.lastSlots, r.slotNative, r.slotEngine);
		r.frameNumber = Frame::Driver::Current().frame;
	}

	/** @brief The slot effects' inputs, read at B with the slot activity. */
	void ReadSlotInputs(ImageSpaceFrame& a_f, std::uintptr_t a_base)
	{
		const auto& g = G();
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		const auto effect = [&](std::uint32_t a_slot) -> std::uintptr_t {
			return a_slot < manager->effects.free_idx() ? reinterpret_cast<std::uintptr_t>(manager->effects[static_cast<std::uint16_t>(a_slot)]) : 0;
		};
		std::memcpy(a_f.refractionTint.data(), reinterpret_cast<const void*>(a_base + 0x98), sizeof(a_f.refractionTint));
		if (const auto dof = effect(3))
			if (const auto* steps = Field<const bool*>(dof, 0x98))
				std::memcpy(a_f.dofSteps.data(), steps, sizeof(a_f.dofSteps));
		a_f.dofCompute = *g.dofCompute;
		if (*g.dofCamera) {
			a_f.dofNear = Field<float>(*g.dofCamera, 0x160);
			a_f.dofFar = Field<float>(*g.dofCamera, 0x164);
		}
		a_f.underwater = *g.underwater;
		const auto underwater = Field<std::uintptr_t>(a_base, 0xb8);
		a_f.underwaterData = underwater != 0;
		if (underwater)
			std::memcpy(a_f.underwaterDof.data(), reinterpret_cast<const void*>(underwater + 0x40), sizeof(a_f.underwaterDof));
		for (std::size_t i = 0; i < 7; ++i) {
			a_f.dofIni[i] = *g.dofIni[i];
			a_f.dofCustom[i] = *g.dofCustom[i];
		}
		// The radial blur draws the one shader its Setup picked by quality (AE 107750): its list's first entry.
		a_f.dofDynamic = effect(3) && Field<bool>(effect(3), 0x88);
		a_f.radialDynamic = effect(5) && Field<bool>(effect(5), 0x88);
		a_f.radialEffect = -1;
		if (const auto radial = effect(5))
			if (const auto* list = Field<RE::ImageSpaceEffect* const*>(radial, 0x18); list && list[0])
				a_f.radialEffect = EffectOf(static_cast<const RE::BSImagespaceShader*>(list[0]));
		for (std::uint32_t r = 0; r < 7; ++r)
			if (const auto blur = effect(9 + r)) {
				a_f.slotBlurRadius[r] = Field<std::int32_t>(blur, 0x90);
				a_f.slotBlurDynamic[r] = Field<bool>(blur, 0x88);
			}
		if (const auto full = effect(6)) {
			a_f.fullBlurDynamic = Field<bool>(full, 0x88);
			// Legacy Graphics Compatibility's full-screen blur adapter adds an eleventh entry (its HasElevenSlots and the copy there).
			const auto& object = *reinterpret_cast<const RE::ImageSpaceEffect*>(full);
			a_f.fullBlurLegacy = object.effects.size() >= 11 && object.effectParams.size() >= 11 && object.effectTextures.size() >= 11 && object.effects[10];
		}
		// The full-screen blur's Render (AE 108563) blurs at radius 2 or more while Main::RenderWorld saw the camera move.
		const bool reset = g.blurCamera[0] == 1.0f && g.blurCamera[1] == 1.0f && g.blurCamera[2] == 1.0f;
		a_f.fullBlurAtLeast2 = !reset && *g.blurCameraMoved && *g.blurCameraGate;
		a_f.getHitDynamic = effect(7) && Field<bool>(effect(7), 0x88);
		a_f.getHitScale = *g.getHitScale;
		a_f.getHitAmount = *g.getHitAmount;
		a_f.timer = *g.timer;
	}

	/** @brief Read point B, RenderWorldPostProcess's entry. Render owner. */
	void ReadB()
	{
		ZoneScopedN("CS.PostNative.ReadB");
		auto& f = R().frame;
		const auto& g = G();
		const auto base = ManagerBase();
		f.readB = false;
		if (!f.read || !base)
			return;
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		f.slots = 0;
		if (f.eof)
			for (std::uint32_t slot = 0; slot < 8; ++slot) {
				auto* effect = slot < manager->effects.free_idx() ? manager->effects[static_cast<std::uint16_t>(slot)] : nullptr;
				if (effect && effect->IsActive())
					f.slots |= static_cast<std::uint8_t>(1u << slot);
			}
		// VL's activity changes with the cell (observe saw an interior frame turn it off after A).
		const auto vlEffect = Field<std::uintptr_t>(base, 0x170);
		f.volumetric = *g.volumetric && vlEffect && Field<bool>(vlEffect, 0x08);
		R().lastSlots = f.slots;
		f.vlParity = Frame::PostChain::VolumetricParity();
		// The VL composite's colour: the sun shadow light's diffuse blended with the VL colour (107017). VL Render (107023) fills the
		// settings block during the frame, so this is a B value (observe found the A value stale).
		if (const auto settings = g.vlSettings())
			g.vlColour(settings, &f.vlColour[0], &f.vlColour[1], &f.vlColour[2]);
		if (const auto camera = *g.worldSceneGraph ? Field<std::uintptr_t>(*g.worldSceneGraph, 0x128) : 0) {
			f.cameraNear = Field<float>(camera, 0x160);
			f.cameraFar = Field<float>(camera, 0x164);
		}
		f.jitter = Frame::PostChain::Jitter();
		f.screenshot = Field<bool>(base, 0x211);
		f.frameCounter = *g.frameCounter;
		ReadSlotInputs(f, base);
		f.readB = true;
	}

	/** @brief The end-of-frame entry: the TAA state Upscaling's call-site hook sets just before it. */
	void ReadEndOfFrame()
	{
		auto& f = R().frame;
		// The dynamic-resolution ratios, their lock and the upsample's activity change with Upscaling's call-site hook too (native
		// mode's upscalers render through the engine's dynamic resolution).
		const auto& data = globals::game::graphicsState->GetRuntimeData();
		f.ratioX = data.dynamicResolutionWidthRatio;
		f.ratioY = data.dynamicResolutionHeightRatio;
		f.locked = data.dynamicResolutionLock != 0;
		if (const auto base = ManagerBase())
			if (const auto upsample = Field<std::uintptr_t>(base, 0x1f8))
				f.upsample = Field<bool>(upsample, 0x08);
		if (const auto base = ManagerBase())
			if (const auto taa = Field<std::uintptr_t>(base, 0x1f0)) {
				f.taa = Field<bool>(taa, 0x18);
				f.taaMode = Field<std::int32_t>(taa, 0x2c);
				f.taaWater = Field<bool>(taa, 0x38);
			}
	}

	/** @brief Where a param effect's constant ids land in PS b2 (the shader's table), as NativeSteps::BuildRows places them. */
	struct RowTable
	{
		bool made = false;
		std::uint32_t count = 0;
		std::array<std::uint8_t, 64> offsets{};
	};

	/** @brief Taken from the effect's shader on first use. A shader with explicit rows (+0x1A0) has none. */
	const RowTable& TableOf(std::size_t a_effect)
	{
		static std::array<RowTable, kEffects.size()> tables{};
		auto& table = tables[a_effect];
		if (table.made)
			return table;
		const auto* shader = ShaderOf(a_effect);
		if (!shader)
			return table;
		const auto* base = reinterpret_cast<const std::byte*>(shader);
		table.made = true;
		if (*reinterpret_cast<const bool*>(base + 0x1A0))
			return table;
		const auto count = *reinterpret_cast<const std::uint32_t*>(base + 0x160);
		const auto* pixel = NativeSteps::PixelShaderOf(*shader);
		if (!pixel || count > table.offsets.size())
			RenderGraph::Fatal("ImageSpace", fmt::format("native post chain: {} has no constant table CS can read", kEffects[a_effect].effectName));
		table.count = count;
		for (std::uint32_t id = 0; id < count; ++id)
			table.offsets[id] = static_cast<std::uint8_t>(pixel->constantTable[id]);
		return table;
	}

	/** @brief Rows for a param effect, every id zero, laid out as BuildRows lays out the engine's. */
	NativeSteps::Rows RowsOf(std::size_t a_effect)
	{
		NativeSteps::Rows rows;
		const auto& table = TableOf(a_effect);
		rows.count = table.count;
		for (std::uint32_t id = 0; id < table.count; ++id) {
			rows.offsets[id] = table.offsets[id];
			rows.size = (std::max)(rows.size, ((table.offsets[id] + 4u) * 4u + 15u) & ~15u);
		}
		return rows;
	}

	/** @brief Rows an explicit setter copies as its struct: row r at register r. */
	NativeSteps::Rows ExplicitRows(std::uint32_t a_count)
	{
		NativeSteps::Rows rows;
		rows.count = a_count;
		for (std::uint32_t row = 0; row < a_count; ++row)
			rows.offsets[row] = static_cast<std::uint8_t>(row * 4);
		rows.size = a_count * 16;
		return rows;
	}

	void SetId(NativeSteps::Rows& a_rows, std::uint32_t a_id, float a_x, float a_y = 0.0f, float a_z = 0.0f, float a_w = 0.0f)
	{
		if (a_id >= a_rows.count)
			return;
		const float values[4]{ a_x, a_y, a_z, a_w };
		std::memcpy(a_rows.bytes.data() + a_rows.offsets[a_id] * 4u, values, 16);
	}

	std::size_t EffectIndex(Effects::ImageSpaceEffectEnum a_effect)
	{
		const auto index = EffectIndexOf(a_effect);
		if (index < 0)
			RenderGraph::Fatal("ImageSpace", "native post chain: an effect it stages is not covered");
		return static_cast<std::size_t>(index);
	}

	/**
	* @brief The sample taps the HDR effects' constructors write once and the shaders read: ISHDRDownSample4's four (ids a_first on,
	* +-1 texel, weight 1/4) and ISHDRDownSample16's sixteen (a 4x4 grid at +-0.5 and +-1.5 texels, weight 1/16).
	*/
	void SetTaps(NativeSteps::Rows& a_rows, std::uint32_t a_first, bool a_sixteen)
	{
		if (!a_sixteen) {
			constexpr std::array<std::array<float, 2>, 4> kTaps{ { { -1.0f, -1.0f }, { 1.0f, -1.0f }, { 1.0f, 1.0f }, { -1.0f, 1.0f } } };
			for (std::uint32_t t = 0; t < 4; ++t)
				SetId(a_rows, a_first + t, kTaps[t][0], kTaps[t][1], 0.25f);
			return;
		}
		for (std::uint32_t t = 0; t < 16; ++t)
			SetId(a_rows, a_first + t, static_cast<float>(t % 4) - 1.5f, static_cast<float>(t / 4) - 1.5f, 0.0625f);
	}

	NativeStep Step(WorldScope a_scope, Effects::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, std::uint8_t a_group)
	{
		NativeStep step{ .key = { a_scope, a_effect, a_variant }, .group = a_group, .effect = EffectIndex(a_effect) };
		step.rows = RowsOf(step.effect);
		return step;
	}

	/** @brief The bloom blur's tap table for radius a_radius (7 is the most): 15 float4 rows, x the offset in texels, z the weight. */
	std::array<std::array<float, 4>, 15> BlurTaps(std::int32_t a_radius, float a_fraction, bool a_vertical, float a_texel)
	{
		// ImageSpaceEffectBlur::UpdateParams (AE 108558): each tap's weight interpolates between the tables of radius - 1 and radius.
		const auto high = (std::min)(a_radius, 7);
		const auto low = (std::max)(a_radius - 1, 1);
		const auto weight = [&](std::int32_t a_r, std::uint32_t a_tap) {
			return *reinterpret_cast<const float*>(G().blurWeights + a_r * 0xF0 + a_tap * 16);
		};
		std::array<std::array<float, 4>, 15> taps{};
		for (std::uint32_t tap = 0; tap < 15; ++tap) {
			const float low_ = weight(low, tap);
			const float w = (weight(high, tap) - low_) * a_fraction + low_;
			const float offset = (static_cast<float>(tap) - kBlurCentre) * a_texel;
			taps[tap] = a_vertical ? std::array<float, 4>{ 0.0f, offset, w, 0.0f } : std::array<float, 4>{ offset, 0.0f, w, 0.0f };
		}
		return taps;
	}

	/** @brief The HDR effect's tonemap rows and the adaptation's rate row (ImageSpaceEffectHDR::UpdateParams, AE 107185). */
	void HdrParams(const ImageSpaceFrame& a_f, NativeSteps::Rows& a_tonemap, bool a_fade, NativeSteps::Rows* a_adapt)
	{
		if (a_fade)
			SetId(a_tonemap, 4, a_f.D(0x11c), a_f.D(0x120), a_f.D(0x124), a_f.D(0x118));
		float white = 0.0f;
		if (!a_f.filmic) {
			const float scale = a_f.reinhardWhite * a_f.D(0xdc);
			white = 1.0f / (scale * scale);
		} else {
			const float x = (std::max)(a_f.filmicWhite * a_f.D(0xdc) - kFilmicBias, 0.0f);
			white = std::pow(((x * kFilmicA + kFilmicB) * x) / ((x * kFilmicA + kFilmicC) * x + kFilmicD), kFilmicPower);
		}
		SetId(a_tonemap, 0, 0.0f);
		SetId(a_tonemap, 1, a_f.D(0xd8), white, a_f.filmic ? 1.0f : 0.0f, 0.0f);
		SetId(a_tonemap, 2, a_f.saturationBoost + a_f.D(0xec), 0.0f, a_f.D(0xf4) + a_f.brightnessBoost, a_f.D(0xf0) + a_f.contrastBoost);
		SetId(a_tonemap, 3, a_f.D(0xfc), a_f.D(0x100), a_f.D(0x104), a_f.D(0xf8));
		if (!a_adapt)
			return;
		const float dt = (a_f.menu ? a_f.menuTimer : a_f.frameTime) * kAdaptFrames;
		const float speed = a_f.adaptSpeed * a_f.D(0xc8);
		const float strong = std::pow((std::max)(1.0f - (kAdaptRate / (a_f.adaptStrength * a_f.D(0xe8))) * speed, 0.0f), dt);
		const float plain = std::pow((std::max)(1.0f - speed * kAdaptRate, 0.0f), dt);
		SetId(*a_adapt, 0, 0.0f);
		SetId(*a_adapt, 2, 0.0f, 0.0f, 1.0f - plain, 1.0f - strong);
	}

	/**
	* @brief The HDR effect's steps from a_source (ImageSpaceEffectHDR::Render, AE 107183): the adaptation reset, the downsample, the
	* luminance levels, the last level or its light adaptation, the bloom of the frame's radius and the tonemap. Advances a_state.
	*/
	std::vector<NativeStep> HdrSteps(const ImageSpaceFrame& a_f, PostState& a_state, std::int32_t a_source, std::uint32_t a_levels)
	{
		ZoneScopedN("CS.PostNative.HdrSteps");
		using E = Effects;
		using T = RE::RENDER_TARGET;
		std::vector<NativeStep> steps;
		const bool fade = a_f.D(0x118) > 0.0f;
		auto tonemap = Step(WorldScope::Tonemap, fade ? E::ISHDRTonemapBlendCinematicFade : E::ISHDRTonemapBlendCinematic, 0,
			fade ? ChainGroup::kTonemapFade : ChainGroup::kTonemap);
		auto adaptRows = RowsOf(EffectIndex(E::ISHDRDownSample16LightAdapt));
		HdrParams(a_f, tonemap.rows, fade, &adaptRows);
		// UpdateParams clears the adaptation while it is off, and Render on the reset edge.
		if (a_f.adaptOff) {
			a_state.adaptTarget = -1;
			a_state.resetPending = false;
		}
		if (a_state.resetPending) {
			a_state.adaptTarget = -1;
			a_state.resetPending = false;
		}
		{
			auto down = Step(WorldScope::Hdr, E::ISHDRDownSample4, 0, ChainGroup::kHdr);
			const auto [w, h] = a_f.Size(a_source);
			SetId(down.rows, 0, 1.0f);
			SetId(down.rows, 1, 1.0f / w, 1.0f / h);
			SetTaps(down.rows, 2, false);
			steps.push_back(std::move(down));
		}
		const std::int32_t least = a_f.stopAt2 && !a_f.adaptOff ? 2 : 1;
		std::int32_t width = static_cast<std::int32_t>(a_f.sizes[T::kHDR_DOWNSAMPLE0].first);
		std::int32_t height = static_cast<std::int32_t>(a_f.sizes[T::kHDR_DOWNSAMPLE0].second);
		std::int32_t target = T::kHDR_DOWNSAMPLE1, input = T::kHDR_DOWNSAMPLE0, last = -1;
		std::uint32_t level = 0;
		const auto quarter = [](std::int32_t a_size) { return (a_size + 3) / 4; };
		while (least < width || least < height) {
			width = (std::max)(quarter(width), least);
			const auto next = quarter(height);
			height = (std::max)(next, least);
			const bool isLast = next <= least && width == least;
			if (isLast) {
				a_state.lumParity = 1 - a_state.lumParity;
				target += a_state.lumParity * 2;
			}
			const auto output = target++;
			if (a_state.adaptTarget < 0 || !isLast) {
				const auto effect = level == 0 ? E::ISHDRDownSample4RGB2Lum : E::ISHDRDownSample16Lum;
				const auto parity = static_cast<std::uint32_t>(a_state.lumParity);
				auto lum = Step(WorldScope::Hdr, effect, LuminanceVariant(level, a_levels, parity), isLast ? ChainGroup::kHdrLum : ChainGroup::kHdr);
				const auto [w, h] = a_f.Size(input);
				SetId(lum.rows, 0, 0.0f);
				SetId(lum.rows, 1, 1.0f / w, 1.0f / h);
				SetTaps(lum.rows, 2, level != 0);
				steps.push_back(std::move(lum));
				if (!isLast)
					input = output;
			}
			last = output;
			++level;
		}
		if (a_state.adaptTarget >= 0 && level > 0) {
			auto adapt = Step(WorldScope::Hdr, E::ISHDRDownSample16LightAdapt, LuminanceVariant(level - 1, a_levels, static_cast<std::uint32_t>(a_state.lumParity)),
				ChainGroup::kHdrAdapt);
			adapt.rows = adaptRows;
			const auto [w, h] = a_f.Size(input);
			SetId(adapt.rows, 3, 1.0f / w, 1.0f / h);
			SetId(adapt.rows, 0, 0.0f);
			SetTaps(adapt.rows, 4, true);
			steps.push_back(std::move(adapt));
		}
		a_state.adaptTarget = last;
		// The bloom: the blur effect of the radius index, its bright pass then its blur (ImageSpaceEffectBlur::UpdateParams). Post
		// Processing's bloom replaces it (its hook on the blur effect's Render skips the draws).
		if (!(globals::features::postProcessing.loaded && PostProcessing::SkipsVanillaBloom())) {
			const auto r = static_cast<std::uint32_t>(std::clamp(static_cast<std::int32_t>(a_f.D(0xcc)) - 1, 0, 6));
			const float threshold = a_f.bloomBoost + a_f.D(0xd0);
			const auto radius = a_f.blurRadius[r];
			const float fraction = [&] {
				const float value = 1.0f - (static_cast<float>(radius) - static_cast<float>(r + 1));
				return value == 0.0f ? 1.0f : value;
			}();
			const auto [w, h] = a_f.Size(T::kHDR_BLURSWAP);
			const auto vertical = BlurTaps(radius, fraction, true, 1.0f / h);
			const auto horizontal = BlurTaps(radius, fraction, false, 1.0f / w);
			const auto n = static_cast<std::uint32_t>((std::min)(radius, 7));
			// The HDR effect clears the blur effect's dynamic-viewport flag (+0x88) for its draw, so x is 1.
			const auto fill = [&](NativeSteps::Rows& a_rows, std::uint32_t a_first, const std::array<std::array<float, 4>, 15>& a_taps) {
				SetId(a_rows, a_first - 1, 1.0f, 0.0f, a_f.range, 1.0f / a_f.range);
				for (std::uint32_t tap = 0; tap < 2 * n + 1; ++tap) {
					const auto& t = a_taps[7 - n + tap];
					SetId(a_rows, a_first + tap, t[0], t[1], t[2], t[3]);
				}
			};
			const auto group = static_cast<std::uint8_t>(ChainGroup::kBloom + r);
			auto bright = Step(WorldScope::Hdr, kBrightPasses[r], static_cast<std::uint16_t>(a_state.lumParity), group);
			if (a_f.blurBright[r]) {
				SetId(bright.rows, 0, threshold, a_f.D(0xd4));
				fill(bright.rows, 2, vertical);
			} else {
				fill(bright.rows, 1, vertical);
			}
			steps.push_back(std::move(bright));
			auto blur = Step(WorldScope::Hdr, kBloomBlurs[r], 0, group);
			fill(blur.rows, 1, horizontal);
			steps.push_back(std::move(blur));
		}
		tonemap.key.variant = static_cast<std::uint16_t>(a_state.lumParity << 15);
		steps.push_back(std::move(tonemap));
		return steps;
	}

	/** @brief ISTemporalAA's six rows (AE 107276) for the output target a_output. */
	NativeSteps::Rows TaaRows(const ImageSpaceFrame& a_f, std::int32_t a_output)
	{
		auto rows = ExplicitRows(6);
		const auto [w, h] = a_f.Size(a_output);
		const float jx = a_f.jitter[0], jy = a_f.jitter[1];
		const float sx = std::ceil(jx) <= 0.5f ? 1.0f : -1.0f;
		const float sy = std::ceil(jy) <= 0.5f ? 1.0f : -1.0f;
		const auto tent = [](float a_v) { return a_v < -0.5f || a_v > 0.5f ? 0.0f : 1.0f - (std::abs(a_v) + std::abs(a_v)); };
		const float x0 = jx * 0.5f * w, y0 = jy * 0.5f * h;
		const float k = a_f.taaK;
		const float ta = tent(x0 * k * 0.5f), tb = tent(k * y0 * 0.5f), tc = tent(k * (y0 + sy) * 0.5f), td = tent((x0 + sx) * k * 0.5f);
		const float sum = tb * ta + tc * ta + tb * td + tc * td;
		const float rx = a_f.locked ? 1.0f : a_f.ratioX, ry = a_f.locked ? 1.0f : a_f.ratioY;
		const auto trunc = [](float a_v) { return static_cast<float>(static_cast<std::uint32_t>(static_cast<std::int64_t>(a_v))); };
		SetId(rows, 0, 1.0f / w, 1.0f / h, trunc(w * rx) * (1.0f / w), trunc(h * ry) * (1.0f / h));
		SetId(rows, 1, jx * 0.5f, jy * 0.5f);
		SetId(rows, 2, tb * ta / sum, tc * ta / sum, tb * td / sum, tc * td / sum);
		SetId(rows, 3, sx * (1.0f / w), sy * (1.0f / h));
		SetId(rows, 4, a_f.taaBlend[0], a_f.taaBlend[1], a_f.taaBlend[2], a_f.taaBlend[3]);
		SetId(rows, 5, a_f.cameraNear, a_f.cameraFar, a_f.menu ? 1.0f : 0.0f, a_f.taaThreshold);
		return rows;
	}

	/** @brief One end-of-frame slot effect the engine's RenderEffect draws: the slot, its source and its destination. */
	struct SlotCall
	{
		std::uint32_t slot = 0;
		std::int32_t source = 0, destination = 0;
	};

	/**
	* @brief The end-of-frame routing, as RenderEndOfFrameEffects (AE 105674) picks its targets: the pre-tonemap slots, the HDR effect's
	* source and output, TAA's output, the post-tonemap (usesLDR) slots and the upsample's input.
	*/
	struct EofRoute
	{
		std::vector<SlotCall> pre, ldr;
		bool direct = false;
		std::int32_t hdrSource = 0, hdrOut = 0, taaOut = 0, image = 0;
		std::int32_t lastSlot = -1, slotCount = 0;
	};

	EofRoute RouteOf(const ImageSpaceFrame& a_f, std::int32_t a_source, std::int32_t a_final, std::int32_t a_spare)
	{
		EofRoute route;
		const auto active = [&](std::uint32_t a_slot) { return (a_f.slots >> a_slot & 1u) != 0; };
		for (std::uint32_t slot = 0; slot < 8; ++slot)
			if (active(slot)) {
				++route.slotCount;
				route.lastSlot = static_cast<std::int32_t>(slot);
			}
		std::int32_t source = a_source, spare = a_spare, lastLdr = -1;
		for (std::int32_t slot = 0; slot <= route.lastSlot; ++slot) {
			if (!active(static_cast<std::uint32_t>(slot)))
				continue;
			if (a_f.usesLdr && slot >= 3 && slot <= 7) {
				lastLdr = slot;
				continue;
			}
			route.pre.push_back({ static_cast<std::uint32_t>(slot), source, spare });
			std::swap(source, spare);
		}
		const bool dynamic = a_f.DynamicResolution();
		route.direct = !a_f.taa && lastLdr < 0 && !dynamic;
		route.hdrSource = source;
		route.hdrOut = route.direct ? a_final : RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY;
		if (route.direct)
			return route;
		std::int32_t image = route.hdrOut, other = RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2;
		if (a_f.taa) {
			const bool last = lastLdr < 0 && !dynamic;
			other = last ? a_final : RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2;
			route.taaOut = other;
			if (!last) {
				image = other;
				other = route.hdrOut;
			}
		}
		if (lastLdr >= 0)
			for (std::int32_t slot = 3; slot < 8; ++slot) {
				if (!active(static_cast<std::uint32_t>(slot)))
					continue;
				const bool last = slot == lastLdr && !dynamic;
				route.ldr.push_back({ static_cast<std::uint32_t>(slot), image, last ? a_final : other });
				if (!last)
					std::swap(image, other);
			}
		route.image = image;
		return route;
	}

	/** @brief The end-of-frame steps after the HDR effect (or its copy): TAA, the screenshot copy and the upsample. Advances a_state. */
	std::vector<NativeStep> EofTailSteps(const ImageSpaceFrame& a_f, PostState& a_state, const EofRoute& route)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		if (!route.direct && a_f.taa) {
			if (a_f.taaMode != 2)
				a_state.taaParity = 1 - a_state.taaParity;
			const auto history = static_cast<std::uint16_t>(Frame::Driver::Current().parity & 1u);
			const auto normals = static_cast<std::uint16_t>(globals::deferred && globals::deferred->forwardRenderTargets[2] == RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_SWAP);
			const auto effect = a_f.taaWater ? E::ISTemporalAA_Water : E::ISTemporalAA;
			NativeStep taa{ .key = { WorldScope::Taa, effect, static_cast<std::uint16_t>((history << 15) | (normals << 14)) }, .group = ChainGroup::kTaa,
				.effect = EffectIndex(effect) };
			taa.rows = TaaRows(a_f, route.taaOut);
			steps.push_back(std::move(taa));
		}
		if (!route.direct && a_f.screenshot)
			steps.push_back(Step(WorldScope::Screenshot, E::ISCopy, 0, ChainGroup::kScreenshot));
		if (!route.direct && a_f.upsample && a_f.DynamicResolution()) {
			auto up = Step(WorldScope::Upsample, E::ISUpsampleDynamicResolution, 0, ChainGroup::kUpsample);
			const auto [w, h] = a_f.Size(route.image);
			const auto scaled = [](float a_size, float a_ratio) { return static_cast<float>(static_cast<std::int32_t>(static_cast<std::int64_t>(a_size * a_ratio)) - 1); };
			SetId(up.rows, 0, a_f.ratioX, a_f.ratioY, scaled(w, a_f.ratioX) / w, scaled(h, a_f.ratioY) / h);
			steps.push_back(std::move(up));
		}
		return steps;
	}

	/**
	* @brief Before AE 1.7.99 the shaders' selector float is set at the draw by Legacy Graphics Compatibility's Render hooks, from the
	* parent effect's +0x88 (LegacyGraphicsCompatibilityShaders.cpp). The native steps set it in their rows the same way.
	*/
	void SetLegacySelector(NativeStep& a_step, bool a_dynamic)
	{
		if (!LegacyGraphicsCompatibility::IsLegacyVersion())
			return;
		const auto translation = LegacyGraphicsCompatibility::GetLegacyImageSpaceSelectorTranslation(a_step.key.effect);
		if (!translation)
			return;
		// As the hook: it compares the float index with the param's size in rows, so the radial blur's (index 7, two rows) is never set.
		if (translation->floatIndex >= a_step.rows.count)
			return;
		const auto id = static_cast<std::uint32_t>(translation->floatIndex / 4);
		const float value = LegacyGraphicsCompatibility::ResolveLegacyImageSpaceSelector(*translation, a_dynamic);
		std::memcpy(a_step.rows.bytes.data() + (a_step.rows.offsets[id] + translation->floatIndex % 4) * 4u, &value, 4);
	}

	/** @brief One slot step: the slot's group and scope, variant 0, the effect's param rows all zero. */
	NativeStep SlotStep(std::uint32_t a_slot, bool a_ldr, Effects::ImageSpaceEffectEnum a_effect)
	{
		return Step(a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, a_effect, 0, SlotGroup(a_slot, a_ldr));
	}

	/** @brief Refraction (ImageSpaceEffectRefraction::UpdateParams, AE 107745): one row, the manager's refraction tint. */
	std::vector<NativeStep> RefractionSteps(const ImageSpaceFrame& a_f)
	{
		std::vector<NativeStep> steps;
		auto step = SlotStep(1, false, Effects::ISRefraction);
		const auto& tint = a_f.refractionTint;
		SetId(step.rows, 0, tint[0], tint[1], tint[2], tint[3]);
		steps.push_back(std::move(step));
		return steps;
	}

	/** @brief The radial blur (ImageSpaceEffectRadialBlur::UpdateParams, AE 107752): two rows of the ImageSpace data, copied. */
	std::vector<NativeStep> RadialSteps(const ImageSpaceFrame& a_f, bool a_ldr, bool& a_supported)
	{
		std::vector<NativeStep> steps;
		if (a_f.radialEffect < 0) {
			a_supported = false;
			return steps;
		}
		auto step = SlotStep(5, a_ldr, kEffects[static_cast<std::size_t>(a_f.radialEffect)].id);
		SetId(step.rows, 0, a_f.D(0x130), a_f.D(0x134), a_f.D(0x138), a_f.D(0x13c));
		SetId(step.rows, 1, a_f.D(0x144), a_f.D(0x148), a_f.D(0x140), 0.0f);
		SetLegacySelector(step, a_f.radialDynamic);
		steps.push_back(std::move(step));
		return steps;
	}

	/** @brief A compute blur pass of the depth of field (AE 0x1414d40b0 and 0x1414d42d0, rows from 0x1414d44d0). */
	NativeStep DofBlurStep(const ImageSpaceFrame& a_f, bool a_ldr, bool a_vertical, std::int32_t a_input, std::int32_t a_output)
	{
		using E = Effects;
		const auto effect = a_vertical ? E::ISDepthOfFieldBlurVCS : E::ISDepthOfFieldBlurHCS;
		NativeStep step{ .key = { a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, effect, 0 }, .group = SlotGroup(3, a_ldr), .effect = EffectIndex(effect) };
		step.compute = true;
		step.rows = ExplicitRows(2);
		const auto [w, h] = a_f.Size(a_input);
		const auto [ow, oh] = a_f.Size(a_output);
		const float rx = a_f.locked ? 1.0f : a_f.ratioX, ry = a_f.locked ? 1.0f : a_f.ratioY;
		const auto trunc = [](float a_v) { return static_cast<float>(static_cast<std::int32_t>(static_cast<std::int64_t>(a_v))); };
		SetId(step.rows, 0, 1.0f / ow, 1.0f / oh);
		SetId(step.rows, 1, (trunc(w * rx) - 1.0f) / w, (trunc(h * ry) - 1.0f) / h);
		// The dispatch covers the input's render-size area: one group per line along the blur, GRIDSIZE texels per group across it.
		const auto up = [](float a_v) { return static_cast<std::uint32_t>(std::ceil(a_v)); };
		const auto x = up(w * rx), y = up(h * ry);
		step.groups = a_vertical ? std::array<std::uint32_t, 3>{ x, (y + kDofGridY - 1) / kDofGridY, 1 } : std::array<std::uint32_t, 3>{ (x + kDofGridX - 1) / kDofGridX, y, 1 };
		return step;
	}

	/**
	* @brief The depth of field (ImageSpaceEffectDepthOfField::Render and UpdateParams, AE 107385 and 107388; the underwater form
	* 0x1414e5610): the downsample, the compute blur and the composites its step flags turn on. a_supported is false when a step needs
	* what CS does not stage: the raster blur, or a composite whose param UpdateParams leaves as an earlier frame wrote it.
	*/
	std::vector<NativeStep> DofSteps(const ImageSpaceFrame& a_f, const SlotCall& a_call, bool a_ldr, bool& a_supported)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		const auto& flags = a_f.dofSteps;
		const auto bitsOf = [](float a_mode) { return static_cast<std::uint64_t>(static_cast<std::int64_t>(a_mode)); };
		// The ImageSpace data's DoF, or the underwater data's when the player is under water (mode 2).
		const bool fromUnderwater = a_f.underwater == 2 && a_f.underwaterData;
		float strength = fromUnderwater ? a_f.underwaterDof[0] : a_f.D(0x14c);
		float distance = fromUnderwater ? a_f.underwaterDof[1] : a_f.D(0x150);
		float range = fromUnderwater ? a_f.underwaterDof[2] : a_f.D(0x154);
		const auto bits = bitsOf(fromUnderwater ? a_f.underwaterDof[3] : a_f.D(0x158));
		const auto form = static_cast<std::uint32_t>(bits & 3u);
		const float notDistant = form > 1 ? 0.0f : 1.0f;
		const float noBlurFar = (bits & 1u) ? 0.0f : 1.0f;
		float sky = (bits & 4u) ? 1.0f : 0.0f;
		float scale = kDofNoRange, offset = kDofNoRange;
		if ((bits & 0x80u) && a_f.underwater != 1) {
			// The custom range from the INI (415136 to 415142).
			const auto& c = a_f.dofCustom;
			scale = 1.0f / ((c[1] - c[0]) + kDofEpsilon);
			offset = c[0] * scale;
			strength = c[6] * c[4];
			sky = c[5] * c[6];
			distance = c[3];
			range = c[2];
		}
		// UpdateParams writes one composite's param, picked by the flags, and points list entry 2 at the form of the mode.
		const std::uint32_t written = flags[3] ? 3u : flags[4] ? 4u : 2u;
		const bool override = a_f.underwater == 1 && a_f.underwaterData;
		E::ImageSpaceEffectEnum entry2 = flags[3] ? (form == 2 ? E::ISDistantBlurFogged : E::ISDepthOfFieldFogged) :
		                                 flags[4] ? (form == 2 ? E::ISDistantBlurMaskedFogged : E::ISDepthOfFieldMaskedFogged) :
		                                            (form == 2 ? E::ISDistantBlur : E::ISDepthOfField);
		const auto underwaterBits = bitsOf(a_f.underwaterDof[3]);
		if (override)
			entry2 = (underwaterBits & 3u) == 2 ? E::ISDistantBlurMaskedFogged : E::ISDepthOfFieldMaskedFogged;
		const std::array<E::ImageSpaceEffectEnum, 3> composites{ entry2, E::ISDepthOfFieldFogged, E::ISDepthOfFieldMaskedFogged };
		const auto& ini = a_f.dofIni;
		if (flags[0]) {
			// The downsample's rows (the end of UpdateParams, skipped in the underwater override): the source's texel size and the
			// four taps of its Setup (0x142033bb0).
			auto down = SlotStep(3, a_ldr, E::ISHDRDownSample4);
			const auto [w, h] = a_f.Size(a_call.source);
			SetId(down.rows, 0, 1.0f);
			SetId(down.rows, 1, 1.0f / w, 1.0f / h);
			SetTaps(down.rows, 2, false);
			steps.push_back(std::move(down));
		}
		if (flags[1]) {
			if (!a_f.dofCompute) {
				a_supported = false;
				return {};
			}
			// BorrowTextures: the blur pair is rt26 and rt22 (rt25 and rt23 with usesLDR).
			const std::int32_t blur = 0x1a - (a_ldr ? 1 : 0), swap = 0x16 + (a_ldr ? 1 : 0);
			steps.push_back(DofBlurStep(a_f, a_ldr, false, blur, swap));
			steps.push_back(DofBlurStep(a_f, a_ldr, true, swap, blur));
		}
		for (std::uint32_t k = 2; k < 5; ++k) {
			if (!flags[k])
				continue;
			if (k != written || (override && k != 4)) {
				a_supported = false;
				return {};
			}
			auto step = SlotStep(3, a_ldr, composites[k - 2]);
			const auto [w, h] = a_f.Size(a_call.destination);
			SetId(step.rows, 0, 1.0f / w, 1.0f / h);
			SetId(step.rows, 1, range, distance, a_f.cameraNear, a_f.cameraFar);
			SetId(step.rows, 2, strength, notDistant, noBlurFar, sky);
			SetId(step.rows, 3, a_f.dofNear, a_f.dofFar, scale, offset);
			if (override) {
				const auto& u = a_f.underwaterDof;
				SetId(step.rows, 4, ini[0], ini[1], ini[2], ini[3]);
				SetId(step.rows, 5, ini[4], ini[5], ini[6], 0.0f);
				SetId(step.rows, 6, u[2], u[1], a_f.cameraNear, a_f.cameraFar);
				SetId(step.rows, 7, u[0], (underwaterBits & 3u) > 1 ? 0.0f : 1.0f, (underwaterBits & 1u) ? 0.0f : 1.0f, (underwaterBits & 4u) ? 1.0f : 0.0f);
			} else if (flags[3]) {
				SetId(step.rows, 4, ini[0], ini[1], ini[2], ini[3]);
				SetId(step.rows, 5, ini[4], ini[5], ini[6], 0.0f);
			}
			SetLegacySelector(step, a_f.dofDynamic);
			steps.push_back(std::move(step));
		}
		return steps;
	}

	/**
	* @brief One draw of effects 9 to 15, the blurs the full-screen blur and get hit draw (ImageSpaceEffectBlur::UpdateParams, AE 108558):
	* radius index a_r, its weights interpolated by the radius its parent handed over (AE 415421), the texel size of the blur's swap
	* target a_swap. Its first draw takes the vertical taps, its second the horizontal ones. Row 0's x is 0 with the blur's flag (+0x88).
	*/
	NativeStep SlotBlurStep(const ImageSpaceFrame& a_f, std::uint32_t a_slot, bool a_ldr, std::uint32_t a_r, float a_handoff, std::int32_t a_swap, bool a_dynamic,
		bool a_vertical, std::uint32_t a_occurrence)
	{
		using E = Effects;
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kHdr{ E::ISBlur3, E::ISBlur5, E::ISBlur7, E::ISBlur9, E::ISBlur11, E::ISBlur13, E::ISBlur15 };
		constexpr std::array<E::ImageSpaceEffectEnum, 7> kLdr{ E::ISNonHDRBlur3, E::ISNonHDRBlur5, E::ISNonHDRBlur7, E::ISNonHDRBlur9, E::ISNonHDRBlur11,
			E::ISNonHDRBlur13, E::ISNonHDRBlur15 };
		const auto effect = Chain().blurHdr[a_r] ? kHdr[a_r] : kLdr[a_r];
		auto step = Step(a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot, effect, InnerVariant(static_cast<std::int32_t>(a_slot), effect, a_occurrence, false),
			SlotGroup(a_slot, a_ldr));
		const auto radius = a_f.slotBlurRadius[a_r];
		const float fraction = [&] {
			const float value = 1.0f - (static_cast<float>(radius) - a_handoff);
			return value == 0.0f ? 1.0f : value;
		}();
		const auto [w, h] = a_f.Size(a_swap);
		const auto taps = BlurTaps(radius, fraction, a_vertical, a_vertical ? 1.0f / h : 1.0f / w);
		const auto n = static_cast<std::uint32_t>(std::clamp(radius, 0, 7));
		SetId(step.rows, 0, a_dynamic ? 0.0f : 1.0f, 0.0f, a_f.range, 1.0f / a_f.range);
		for (std::uint32_t tap = 0; tap < 2 * n + 1; ++tap) {
			const auto& t = taps[7 - n + tap];
			SetId(step.rows, 1 + tap, t[0], t[1], t[2], t[3]);
		}
		return step;
	}

	/** @brief The full-screen blur's radius from manager +0x128: rounded up (down below zero), at least 2 while the camera moved. */
	std::int32_t FullBlurDrawnRadius(const ImageSpaceFrame& a_f)
	{
		const float amount = a_f.D(0x128);
		auto radius = static_cast<std::int32_t>(amount);
		if (radius != INT32_MIN && static_cast<float>(radius) != amount && !std::signbit(amount))
			++radius;
		if (a_f.fullBlurAtLeast2 && radius < 2)
			radius = 2;
		return radius;
	}

	/**
	* @brief The full-screen blur (ImageSpaceEffectFullScreenBlur::Render and UpdateParams, AE 108563 and 108564): the downsample into
	* rt22, the blur of its radius between rt22 and rt26, and the upsample back, which blends over the image below an amount of 1.
	* CS draws it in place, so the engine's copy of the input before the blend has no step. Legacy Graphics Compatibility's adapter gives
	* the blurs the effect's flag and sets the upsample's row 0 x to 1.
	*/
	std::vector<NativeStep> FullBlurSteps(const ImageSpaceFrame& a_f, const SlotCall& a_call, bool a_ldr)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		const auto scope = a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
		const auto group = SlotGroup(6, a_ldr);
		const float amount = a_f.D(0x128);
		{
			auto down = Step(scope, E::ISHDRDownSample4, InnerVariant(6, E::ISHDRDownSample4, 0, false), group);
			const auto [w, h] = a_f.Size(a_call.source);
			SetId(down.rows, 0, 0.0f);
			SetId(down.rows, 1, 1.0f / w, 1.0f / h);
			SetTaps(down.rows, 2, false);
			steps.push_back(std::move(down));
		}
		if (const auto radius = FullBlurDrawnRadius(a_f); radius >= 1 && radius <= 7) {
			const auto r = static_cast<std::uint32_t>(radius - 1);
			const bool dynamic = a_f.fullBlurLegacy ? a_f.fullBlurDynamic : a_f.slotBlurDynamic[r];
			steps.push_back(SlotBlurStep(a_f, 6, a_ldr, r, amount, RE::RENDER_TARGET::kHDR_DOWNSAMPLE0, dynamic, true, 0));
			steps.push_back(SlotBlurStep(a_f, 6, a_ldr, r, amount, RE::RENDER_TARGET::kHDR_DOWNSAMPLE0, dynamic, false, 1));
		}
		{
			const bool blended = amount < 1.0f;
			auto up = Step(scope, E::ISHDRDownSample4, InnerVariant(6, E::ISHDRDownSample4, 1, blended), group);
			const auto [w, h] = a_f.Size(a_call.destination);
			SetId(up.rows, 0, a_f.fullBlurLegacy ? 1.0f : 0.0f);
			SetId(up.rows, 1, 1.0f / w, 1.0f / h, (std::min)(amount, 1.0f), 0.0f);
			SetTaps(up.rows, 2, false);
			steps.push_back(std::move(up));
		}
		return steps;
	}

	/**
	* @brief Get hit (ImageSpaceEffectGetHit::UpdateParams, AE 108553, drawn by the base Render): the copy into the get-hit buffer, the
	* radius-index-1 blur twice (radius 2's weights), and the double vision over the image. Its rows turn with the timer.
	*/
	std::vector<NativeStep> GetHitSteps(const ImageSpaceFrame& a_f, bool a_ldr)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		const auto scope = a_ldr ? WorldScope::LdrSlot : WorldScope::PreSlot;
		const auto group = SlotGroup(7, a_ldr);
		steps.push_back(Step(scope, E::ISCopy, InnerVariant(7, E::ISCopy, 0, false), group));
		for (std::uint32_t k = 0; k < 4; ++k)
			steps.push_back(SlotBlurStep(a_f, 7, a_ldr, 1, kGetHitRadius, RE::RENDER_TARGET::kGETHIT_BUFFER, a_f.getHitDynamic, k % 2 == 0, k));
		auto vision = Step(scope, E::ISDoubleVision, InnerVariant(7, E::ISDoubleVision, 0, false), group);
		const float strength = a_f.D(0x12c);
		const float angle = a_f.timer * kGetHitTurn;
		SetId(vision.rows, 0, 0.0f, 0.0f, a_f.getHitScale * a_f.getHitAmount, a_f.getHitAmount);
		SetId(vision.rows, 1, std::sin(angle) * strength, std::cos(angle) * strength, 1.0f, 1.0f);
		SetLegacySelector(vision, a_f.getHitDynamic);
		steps.push_back(std::move(vision));
		return steps;
	}

	/** @brief The slots CS stages natively: refraction (1), the depth of field (3), the radial blur (5), the full-screen blur (6), get hit (7). */
	bool SlotIsNative(std::uint32_t a_slot)
	{
		return (a_slot == 1 || a_slot == 3 || a_slot == 5 || a_slot == 6 || a_slot == 7) && !(Settings().foreignSlots >> a_slot & 1u);
	}

	/** @brief A native slot's steps. a_supported is false when the frame's slot needs the engine's RenderEffect. */
	std::vector<NativeStep> SlotSteps(const ImageSpaceFrame& a_f, const SlotCall& a_call, bool a_ldr, bool& a_supported)
	{
		a_supported = SlotIsNative(a_call.slot);
		if (!a_supported)
			return {};
		switch (a_call.slot) {
		case 1:
			return RefractionSteps(a_f);
		case 3:
			return DofSteps(a_f, a_call, a_ldr, a_supported);
		case 6:
			return FullBlurSteps(a_f, a_call, a_ldr);
		case 7:
			return GetHitSteps(a_f, a_ldr);
		default:
			return RadialSteps(a_f, a_ldr, a_supported);
		}
	}

	/** @brief The VL composite (AE 107213) when VL is on and the image-based flares are off. */
	std::vector<NativeStep> CompositeSteps(const ImageSpaceFrame& a_f)
	{
		std::vector<NativeStep> steps;
		if (!a_f.volumetric)
			return steps;
		auto vl = Step(WorldScope::Composite, Effects::ISCompositeVolumetricLighting, static_cast<std::uint16_t>(a_f.vlParity), ChainGroup::kComposite);
		// An explicit setter (AE 108775 and its forms) copies one row.
		vl.rows = ExplicitRows(1);
		SetId(vl.rows, 0, a_f.vlColour[0], a_f.vlColour[1], a_f.vlColour[2], 1.0f);
		steps.push_back(std::move(vl));
		return steps;
	}

	// Observe mode.

	void Expect(std::vector<NativeStep> a_steps)
	{
		auto& r = R();
		for (auto& step : a_steps)
			r.expected.push_back(std::move(step));
	}

	std::string KeyName(const StepKey& a_key)
	{
		return fmt::format("{} {:#x}", kEffects[EffectIndex(a_key.effect)].effectName, a_key.variant);
	}

	/** @brief Each float within 1e-5 relative (1e-6 absolute near zero), as the plan's parity rule. */
	bool Near(float a_a, float a_b)
	{
		if (std::memcmp(&a_a, &a_b, 4) == 0)
			return true;
		const float scale = (std::max)(std::abs(a_a), std::abs(a_b));
		return std::abs(a_a - a_b) <= (std::max)(1e-5f * scale, 1e-6f);
	}

	/** @brief The effects whose ORG shader reads no PerGeometry rows: their engine rows are not compared. */
	bool ReadsNoRows(Effects::ImageSpaceEffectEnum a_effect)
	{
		using E = Effects;
		return a_effect == E::ISCopy || a_effect == E::ISCopyDynamicFetchDisabled;
	}

	void Compare(const StepKey& a_key, const NativeSteps::Rows& a_predicted, const NativeSteps::Rows& a_engine, bool a_compute = false)
	{
		auto& r = R();
		++r.compared;
		if (ReadsNoRows(a_key.effect))
			return;
		std::string differences;
		// An explicit step's engine rows are the whole pool buffer its setter bound. Only the setter's struct is the step's.
		const bool param = !a_compute && TableOf(EffectIndex(a_key.effect)).count > 0;
		const auto count = param ? (std::max)(a_predicted.count, a_engine.count) : a_predicted.count;
		for (std::uint32_t id = 0; id < count && differences.size() < 500; ++id) {
			const auto* source = id < a_engine.count ? &a_engine : &a_predicted;
			const auto at = source->offsets[id] * 4u;
			for (std::uint32_t c = 0; c < 4; ++c) {
				float predicted = 0.0f, engine = 0.0f;
				if (id < a_predicted.count)
					std::memcpy(&predicted, a_predicted.bytes.data() + at + c * 4, 4);
				if (id < a_engine.count)
					std::memcpy(&engine, a_engine.bytes.data() + at + c * 4, 4);
				if (!Near(predicted, engine))
					differences += fmt::format(" id{}.{} {} vs {}", id, "xyzw"[c], predicted, engine);
			}
		}
		if (differences.empty())
			return;
		++r.differed;
		Mismatches().Report(fmt::format("rows of {}", KeyName(a_key)), fmt::format("CS vs engine:{}", differences));
	}
}

namespace NativePost
{
	/** @brief The engine's CS b0 rows for the dispatch it makes now, captured as StageStep captures them. Observe mode only. */
	NativeSteps::Rows EngineComputeRows(std::uint32_t a_rows)
	{
		auto rows = ExplicitRows(a_rows);
		ID3D11Buffer* constants = Tripwire::Unwrap(Constants::EngineComputeBuffer());
		if (!constants)
			return rows;
		globals::d3d::context->CSSetConstantBuffers(0, 1, &constants);
		const auto use = Constants::Capture(Constants::StageBit(Constants::Stage::Compute));
		if (const auto* slot = use.Valid() ? Constants::Find(use, Constants::Stage::Compute, 0) : nullptr) {
			const auto bytes = Constants::Bytes(*slot);
			std::memcpy(rows.bytes.data(), bytes.data(), (std::min)(bytes.size(), static_cast<std::size_t>(rows.size)));
		}
		return rows;
	}

	/** @brief The slot a slot step belongs to: an inner draw of the full-screen blur (6) or get hit (7) by its variant, else by its effect. */
	std::int32_t SlotOfStep(const StepKey& a_key)
	{
		if (a_key.scope != WorldScope::PreSlot && a_key.scope != WorldScope::LdrSlot)
			return -1;
		if (a_key.variant & 0x400u)
			return (a_key.variant & 0x800u) ? 7 : 6;
		return SlotOfEffect(a_key.effect);
	}
}

void ObservePostStep(const StepKey& a_key, std::size_t a_effect, bool a_compute, std::array<std::uint32_t, 3> a_groups)
{
	using namespace NativePost;
	auto& r = R();
	if (!r.predicting)
		return;
	// The slots CS does not stage natively run through the engine's RenderEffect in every mode, so their steps are not predicted.
	const bool slotStep = a_key.scope == WorldScope::PreSlot || a_key.scope == WorldScope::LdrSlot;
	if (const auto slot = SlotOfStep(a_key); slotStep && (slot < 0 || !SlotIsNative(static_cast<std::uint32_t>(slot))))
		return;
	auto& native = NativeSteps::Get();
	if (r.expected.empty()) {
		++r.differed;
		Mismatches().Report("an engine step CS did not predict", KeyName(a_key));
		return;
	}
	auto step = std::move(r.expected.front());
	r.expected.pop_front();
	if (!(step.key == a_key)) {
		++r.differed;
		Mismatches().Report(fmt::format("the step CS predicted, {}", KeyName(step.key)), fmt::format("the engine drew {}", KeyName(a_key)));
		return;
	}
	if (a_compute) {
		if (step.groups != a_groups) {
			++r.differed;
			Mismatches().Report(fmt::format("thread groups of {}", KeyName(a_key)),
				fmt::format("CS {}x{}x{} vs engine {}x{}x{}", step.groups[0], step.groups[1], step.groups[2], a_groups[0], a_groups[1], a_groups[2]));
		}
		Compare(a_key, step.rows, EngineComputeRows(step.rows.count), true);
		return;
	}
	// The engine's rows are known when the step is native per effect (CS_ORG_IMAGESPACE_NATIVE=2): BuildRows or BoundRows filled them.
	if (native.armed && native.effect == a_effect)
		Compare(a_key, step.rows, native.rows);
}

namespace NativePost
{
	/** @brief Starts a prediction for a container run on the engine. a_state is advanced by the builders. */
	void BeginPredict()
	{
		auto& r = R();
		r.expected.clear();
		r.predicting = true;
	}

	/** @brief Ends a prediction: steps CS predicted that the engine did not draw, and the state after the container. */
	void EndPredict(std::string_view a_where, const std::optional<PostState>& a_predicted)
	{
		auto& r = R();
		r.predicting = false;
		for (const auto& step : r.expected) {
			++r.differed;
			Mismatches().Report(fmt::format("a step CS predicted in {} that the engine did not draw", a_where), KeyName(step.key));
		}
		r.expected.clear();
		if (!a_predicted)
			return;
		const auto engine = EngineState();
		if (engine.lumParity != a_predicted->lumParity || engine.adaptTarget != a_predicted->adaptTarget || engine.resetPending != a_predicted->resetPending ||
			engine.taaParity != a_predicted->taaParity) {
			++r.differed;
			Mismatches().Report(fmt::format("the post state after {}", a_where),
				fmt::format("CS: luminance parity {}, adapted rt{}, reset {}, TAA parity {}; engine: {}, rt{}, {}, {}", a_predicted->lumParity, a_predicted->adaptTarget,
					a_predicted->resetPending, a_predicted->taaParity, engine.lumParity, engine.adaptTarget, engine.resetPending, engine.taaParity));
		}
	}

	// Native mode.

	/** @brief Stages a predicted step as the frame's own, from its rows. False when the build the frame runs has no pass for it. */
	bool Stage(const NativeStep& a_step)
	{
		ZoneScopedN("CS.PostNative.Stage");
		auto& native = NativeSteps::Get();
		native.rows = a_step.rows;
		native.rowsAt = {};
		native.sprite = {};
		native.effect = a_step.effect;
		native.shader = ShaderOf(a_step.effect);
		native.armed = true;
		native.computeRows = a_step.compute;
		const bool taken = StageChainKey(a_step.key, a_step.group, a_step.effect, a_step.compute, a_step.groups);
		native.armed = false;
		native.computeRows = false;
		++R().staged;
		return taken;
	}

	/** @brief Writes the state back where engine code reads it: the luminance parity, the adapted luminance, the reset flag. */
	void WriteBackHdr(const PostState& a_state)
	{
		auto& g = G();
		*g.lumParity = a_state.lumParity;
		*g.adaptReset = false;
		auto& adapted = *g.adapted;
		// As the engine's ImageSpaceTexture assignment and reset (AE 0x1414e8d60, 0x1414e8b50): a target, no texture, no depth.
		if (adapted.texture)
			adapted.texture.reset();
		adapted.renderTarget = static_cast<RE::RENDER_TARGET>(a_state.adaptTarget);
		adapted.unk0C = static_cast<std::uint32_t>(-1);
	}

	/** @brief The shadow state's render targets as a container leaves them: a_target as RT0 (none for -1), nothing else bound. */
	void KeepTargets(std::int32_t a_target)
	{
		auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		bool changed = false;
		for (std::uint32_t i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			const auto want = i == 0 && a_target >= 0 ? static_cast<RE::RENDER_TARGET>(a_target) : RE::RENDER_TARGET::kNONE;
			changed |= shadow.renderTargets[i] != want || shadow.setRenderTargetMode[i] != RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
			shadow.renderTargets[i] = want;
			shadow.setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_NO_CLEAR;
		}
		if (shadow.depthStencil != static_cast<std::uint32_t>(-1)) {
			shadow.depthStencil = static_cast<std::uint32_t>(-1);
			changed = true;
		}
		if (changed) {
			shadow.cubeMapRenderTarget = static_cast<std::uint32_t>(-1);
			shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
	}

	/** @brief The levels of the build the frame holds for the HDR part, which the luminance keys count. 0 with none held. */
	std::uint32_t HeldLevels()
	{
		const auto held = Chain().group.Held(1);
		return held ? held->levels : 0u;
	}

	/** @brief The native HDR effect from a_source into a_output, in its own scope, as the HDR effect's hook runs it. */
	void RunHdr(std::int32_t a_source, std::int32_t a_output)
	{
		ZoneScopedN("CS.PostNative.Hdr");
		auto& r = R();
		if (Usable() && Frame::PostChain::LuminanceLevels() != RaisedShape().luminanceLevels)
			RaiseShape("ImageSpace: the HDR effect's luminance chain changed");
		const auto levels = HeldLevels();
		if (levels == 0) {
			Frame::Driver::NoteLaggedWork(Frame::SegmentId::Post);
			return;
		}
		EnterScope(ScopeKind::HDR);
		{
			const ChainHdrGuard chain;
			for (const auto& step : HdrSteps(r.frame, r.state, a_source, levels))
				Stage(step);
			LeaveScope();
		}
		WriteBackHdr(r.state);
		KeepTargets(a_output);
		++r.native[0];
	}
}

/** @brief The HDR effect's hook: native in the world chain when the family is, observed when it is observed, else the engine's. */
template <class Engine>
void HdrFamily(void* a_effect, Engine&& a_engine)
{
	using namespace NativePost;
	auto& chain = Chain();
	auto& r = R();
	const bool world = chain.inEndOfFrame && r.frame.read && r.frame.readB && Chain().group.Registered();
	if (world && In(Mode::Native, Family::kHdr)) {
		// The effect's texture list names the source and the output (RenderEffect set them): texture 1 and texture 0.
		std::int32_t input = -1, output = -1;
		NativeSteps::TextureTargets(*static_cast<const RE::BSImagespaceShader*>(static_cast<const RE::ImageSpaceEffect*>(a_effect)), input, output);
		RunHdr(input >= 0 ? input : RE::RENDER_TARGET::kMAIN, output);
		return;
	}
	const bool observe = world && In(Mode::Observe, Family::kHdr) && !r.predicting;
	if (observe) {
		std::int32_t input = -1, output = -1;
		NativeSteps::TextureTargets(*static_cast<const RE::BSImagespaceShader*>(static_cast<const RE::ImageSpaceEffect*>(a_effect)), input, output);
		auto predicted = r.state;
		BeginPredict();
		Expect(HdrSteps(r.frame, predicted, input >= 0 ? input : RE::RENDER_TARGET::kMAIN, HeldLevels()));
		a_engine();
		predicted.taaParity = EngineState().taaParity;
		EndPredict("the HDR effect", predicted);
		r.state = EngineState();
		return;
	}
	a_engine();
	if (world)
		r.state = EngineState();
}

namespace NativePost
{
	/**
	* @brief Whether a frame's end-of-frame effects can run natively. The world map (slot 0) and FXAA (slot 2) keep the frame on the
	* engine (decision 7).
	*/
	bool EofNativeFrame(const ImageSpaceFrame& a_f)
	{
		return a_f.read && a_f.readB && (a_f.slots & 0b101) == 0 && Chain().group.Registered();
	}

	/** @brief The manager's spare image-space target (+0x60), which the slots ping-pong with. */
	std::int32_t SpareTarget()
	{
		return Field<std::int32_t>(ManagerBase(), 0x60);
	}

	/**
	* @brief One end-of-frame slot: its steps staged natively, then what its Render writes that engine code reads (refraction's drawn
	* flags, the underwater mask's flag), or the engine's RenderEffect for a slot CS does not stage.
	*/
	void RunSlot(const SlotCall& a_call, bool a_ldr)
	{
		auto& r = R();
		const auto& g = G();
		bool supported = false;
		const auto steps = SlotSteps(r.frame, a_call, a_ldr, supported);
		if (!supported) {
			ZoneScopedN("CS.PostNative.SlotEffect (engine RenderEffect)");
			static REL::Relocation<void (*)(void*, std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t)> renderEffect{ REL::RelocationID(99025, 105676) };
			renderEffect(RE::ImageSpaceManager::GetSingleton(), a_call.slot, static_cast<std::uint64_t>(a_call.source), static_cast<std::uint64_t>(a_call.destination), 0);
			++r.slotEngine;
			return;
		}
		ZoneScopedN("CS.PostNative.SlotEffect");
		bool composite = false;
		for (const auto& step : steps) {
			Stage(step);
			composite |= step.key.effect != Effects::ISHDRDownSample4 && !step.compute;
		}
		// The full-screen blur sets alpha blend mode 1 around its blended upsample and leaves mode 0.
		if (a_call.slot == 6 && r.frame.D(0x128) < 1.0f) {
			auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			if (shadow.alphaBlendMode != 0) {
				shadow.alphaBlendMode = 0;
				shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
			}
		}
		// Get hit's base Render gives its blur (effect 10) its own flag, which stays for the full-screen blur's next draw of it.
		if (a_call.slot == 7)
			if (auto* manager = RE::ImageSpaceManager::GetSingleton(); manager && 10 < manager->effects.free_idx() && manager->effects[10])
				Field<bool>(reinterpret_cast<std::uintptr_t>(manager->effects[10]), 0x88) = r.frame.getHitDynamic;
		// Refraction's Render (AE 107744) clears the flags refraction geometry sets, which its IsActive reads.
		if (a_call.slot == 1) {
			g.refractionDrawn[0] = false;
			g.refractionDrawn[1] = false;
		}
		// The depth of field's Render (AE 107385) clears the underwater mask to zero after a composite once the mask was drawn, and
		// its flag. The skeleton's clear pass at the post chain's place takes the clear; without one it is the D3D11 clear.
		if (a_call.slot == 3 && composite && *g.underwaterMaskWritten) {
			constexpr FLOAT kZero[4]{};
			auto* view = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK].RTV;
			if (view && !RenderGraph::Engine::Skeleton::Sites::TakeColourClear(globals::d3d::context, view, kZero))
				globals::d3d::context->ClearRenderTargetView(view, kZero);
			*g.underwaterMaskWritten = false;
		}
		++r.slotNative;
	}

	/**
	* @brief The native end-of-frame effects (AE 105674) from a_source into a_final. CS routes the frame and stages the HDR effect, TAA,
	* the screenshot copy and the upsample from the snapshot. The slot effects (refraction, depth of field, the blurs) go through the
	* engine's RenderEffect, whose steps stage natively per effect (CS_ORG_IMAGESPACE_NATIVE=2).
	*/
	void RunEndOfFrame(std::int32_t a_source, std::int32_t a_final)
	{
		ZoneScopedN("CS.PostNative.EndOfFrame");
		auto& r = R();
		auto& f = r.frame;
		const auto& g = G();
		const auto base = ManagerBase();
		const auto route = RouteOf(f, a_source, a_final, SpareTarget());
		// The active slot count and the last slot, as the engine counts them.
		Field<std::int32_t>(base, 0x3c) = route.slotCount;
		Field<std::int32_t>(base, 0x40) = route.lastSlot;
		EnterScope(ScopeKind::Effect);
		for (const auto& call : route.pre)
			RunSlot(call, false);
		LeaveScope();
		if (f.hdr) {
			ZoneScopedN("CS.PostNative.TonemapCallSite");
			RenderGraph::Engine::TonemapCallSite(static_cast<std::uint32_t>(route.hdrSource), static_cast<std::uint32_t>(route.hdrOut),
				[&](std::uint32_t a_from) { RunHdr(static_cast<std::int32_t>(a_from), route.hdrOut); });
		} else {
			EnterScope(ScopeKind::Effect);
			Stage(Step(WorldScope::HdrCopy, Effects::ISCopy, 0, ChainGroup::kHdrCopy));
			LeaveScope();
		}
		const auto taa = Field<std::uintptr_t>(base, 0x1f0);
		EnterScope(ScopeKind::Effect);
		const auto tail = EofTailSteps(f, r.state, route);
		for (const auto& step : tail)
			if (step.key.scope != WorldScope::Upsample)
				Stage(step);
		for (const auto& call : route.ldr)
			RunSlot(call, true);
		for (const auto& step : tail)
			if (step.key.scope == WorldScope::Upsample)
				Stage(step);
		LeaveScope();
		// TAA's helper (AE 107270): the reset is consumed, the parity and row 0 kept on the object, the form back to 0.
		if (taa && !route.direct && f.taa) {
			*g.taaReset = false;
			Field<std::int32_t>(taa, 0x30) = 0;
			Field<std::int32_t>(taa, 0x34) = r.state.taaParity;
			const auto [w, h] = f.Size(route.taaOut);
			const float rx = f.locked ? 1.0f : f.ratioX, ry = f.locked ? 1.0f : f.ratioY;
			const auto trunc = [](float a_v) { return static_cast<float>(static_cast<std::uint32_t>(static_cast<std::int64_t>(a_v))); };
			Field<float>(taa, 0x1c) = 1.0f / w;
			Field<float>(taa, 0x20) = 1.0f / h;
			Field<float>(taa, 0x24) = trunc(w * rx) * (1.0f / w);
			Field<float>(taa, 0x28) = trunc(h * ry) * (1.0f / h);
			Field<std::int32_t>(taa, 0x2c) = 0;
		}
		if (!route.direct && f.screenshot) {
			Field<bool>(base, 0x211) = false;
			Field<std::uint64_t>(base, 0x218) = f.frameCounter;
			globals::game::renderer->UpdateViewPort(0, 0, false);
		}
		KeepTargets(a_final);
		if (!route.direct && f.upsample && f.DynamicResolution())
			globals::game::renderer->UpdateViewPort(0, 0, true);
		++r.native[1];
	}
}

namespace NativePost
{
	/**
	* @brief RenderWorldPostProcess's body (AE 107148), without the engine's container. Its side effects are written back: the current
	* camera, the depth of field's near and far, the unbound targets. VL goes to the apply and blur functions CS's ORG VL detours, the
	* lens flare gather stays the engine's, and the end-of-frame effects are called through their call site's current target, so
	* Upscaling's, HDR Display's and any other hook there run in their order. Render owner.
	*/
	void RunBody(std::uint32_t a_target)
	{
		ZoneScopedN("CS.PostNative.Body");
		auto& r = R();
		const auto& g = G();
		const auto base = ManagerBase();
		auto* tls = RE::GetStaticTLSData();
		const auto taskFlag = tls->taskFlag;
		tls->taskFlag = 0x21;
		auto* camera = reinterpret_cast<RE::NiCamera*>(Field<std::uintptr_t>(*g.worldSceneGraph, 0x128));
		*g.currentCamera = camera;
		std::array<std::byte, 16> port{};
		std::memcpy(port.data(), reinterpret_cast<const std::byte*>(camera) + 0x174, port.size());
		if (const auto dof = Field<std::uintptr_t>(Field<std::uintptr_t>(base, 0x28), 0x18); dof && *g.dofCamera) {
			Field<float>(dof, 0x150) = Field<float>(*g.dofCamera, 0x164);
			Field<float>(dof, 0x14c) = Field<float>(*g.dofCamera, 0x160);
		}
		for (std::uint32_t slot = 0; slot < 4; ++slot)
			g.setTarget(g.targets, slot, -1, 3, true);
		g.setDepth(g.targets, -1, 3, false);
		if (r.frame.volumetric) {
			const auto b = r.frame.vlParity;
			g.applyVl(Field<std::uintptr_t>(base, 0x170), b + 0x2b, (b ^ 1) + 0x2b);
			g.blurVl(Field<std::uintptr_t>(base, 0x188), (b ^ 1) + 0x2b, b + 0x2b, 0x2d);
		}
		{
			ZoneScopedN("CS.PostNative.LensFlareGather (engine)");
			g.lensFlares(camera);
		}
		g.composites(RE::ImageSpaceManager::GetSingleton(), 1, 1);
		using EndOfFrame = void (*)(void*, std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
		const auto site = g.eofCallSite;
		const auto endOfFrame = reinterpret_cast<EndOfFrame>(site + 5 + *reinterpret_cast<const std::int32_t*>(site + 1));
		endOfFrame(RE::ImageSpaceManager::GetSingleton(), 1, a_target, 1, 0);
		std::memcpy(reinterpret_cast<std::byte*>(Field<std::uintptr_t>(*g.worldSceneGraph, 0x128)) + 0x174, port.data(), port.size());
		tls->taskFlag = taskFlag;
		++r.native[3];
	}
}

/** @brief The world chain's end-of-frame effects: native, observed or the engine's. Between OnEndOfFrame's begin and end. */
template <class Engine>
void EofFamily(std::int32_t a_source, std::int32_t a_final, Engine&& a_engine)
{
	using namespace NativePost;
	auto& r = R();
	ReadEndOfFrame();
	const bool frame = EofNativeFrame(r.frame);
	if (frame && In(Mode::Native, Family::kEof)) {
		RunEndOfFrame(a_source, a_final);
		return;
	}
	if (frame && In(Mode::Observe, Family::kEof)) {
		auto predicted = r.state;
		BeginPredict();
		const auto route = RouteOf(r.frame, a_source, a_final, SpareTarget());
		const auto expectSlots = [&](const std::vector<SlotCall>& a_calls, bool a_ldr) {
			for (const auto& call : a_calls) {
				bool supported = false;
				auto steps = SlotSteps(r.frame, call, a_ldr, supported);
				if (supported)
					Expect(std::move(steps));
				else if (SlotIsNative(call.slot))
					Mismatches().Report("a slot CS stages through the engine's RenderEffect", fmt::format("slot {} (depth of field steps {}{}{}{}{}, compute {})", call.slot,
						r.frame.dofSteps[0], r.frame.dofSteps[1], r.frame.dofSteps[2], r.frame.dofSteps[3], r.frame.dofSteps[4], r.frame.dofCompute));
			}
		};
		expectSlots(route.pre, false);
		if (r.frame.hdr && !(globals::state && globals::state->PeekTonemapOwner() == ::State::TonemapOwner::kEffects11))
			Expect(HdrSteps(r.frame, predicted, route.hdrSource, HeldLevels()));
		else if (!r.frame.hdr)
			Expect({ Step(WorldScope::HdrCopy, Effects::ISCopy, 0, ChainGroup::kHdrCopy) });
		auto tail = EofTailSteps(r.frame, predicted, route);
		std::vector<NativeStep> upsample;
		for (auto it = tail.begin(); it != tail.end();)
			if (it->key.scope == WorldScope::Upsample) {
				upsample.push_back(std::move(*it));
				it = tail.erase(it);
			} else
				++it;
		Expect(std::move(tail));
		expectSlots(route.ldr, true);
		Expect(std::move(upsample));
		a_engine();
		EndPredict("the end-of-frame effects", predicted);
		r.state = EngineState();
		return;
	}
	if (r.frame.read)
		++r.engine[1];
	a_engine();
	if (r.frame.read)
		r.state = EngineState();
}

/** @brief The pre-tonemap composites: native when the frame has neither SIL nor the image-based flares, observed, or the engine's. */
template <class Engine>
void CompositesFamily(Engine&& a_engine)
{
	using namespace NativePost;
	auto& r = R();
	const bool frame = r.frame.read && r.frame.readB && !r.frame.sil && !r.frame.iblf && Frame::PostChain::InChain() && Chain().group.Registered();
	if (frame && In(Mode::Native, Family::kComposites)) {
		EnterScope(ScopeKind::Composites);
		{
			const ChainCompositesGuard chain;
			for (const auto& step : CompositeSteps(r.frame))
				Stage(step);
			LeaveScope();
		}
		// The composite (AE 107213) unbinds its target and resets the blend mode.
		KeepTargets(-1);
		auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		if (shadow.alphaBlendMode != 0) {
			shadow.alphaBlendMode = 0;
			shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
		}
		++r.native[2];
		return;
	}
	if (frame && In(Mode::Observe, Family::kComposites)) {
		BeginPredict();
		Expect(CompositeSteps(r.frame));
		a_engine();
		EndPredict("the pre-tonemap composites", std::nullopt);
		return;
	}
	a_engine();
}

namespace NativePost
{
	/** @brief Follows E9 rel32, EB rel8 and FF 25 jumps from a_address to the first instruction that is not one. */
	std::uintptr_t FollowJumps(std::uintptr_t a_address)
	{
		for (int hop = 0; hop < 8 && a_address; ++hop) {
			const auto* code = reinterpret_cast<const std::uint8_t*>(a_address);
			if (code[0] == 0xE9)
				a_address = a_address + 5 + *reinterpret_cast<const std::int32_t*>(code + 1);
			else if (code[0] == 0xEB)
				a_address = a_address + 2 + static_cast<std::int8_t>(code[1]);
			else if (code[0] == 0xFF && code[1] == 0x25)
				a_address = *reinterpret_cast<const std::uintptr_t*>(a_address + 6 + *reinterpret_cast<const std::int32_t*>(code + 2));
			else
				break;
		}
		return a_address;
	}

	bool InModule(std::uintptr_t a_address, HMODULE a_module)
	{
		HMODULE owner = nullptr;
		return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(a_address), &owner) &&
		       owner == a_module;
	}

	HMODULE OwnModule()
	{
		HMODULE own = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&OwnModule), &own);
		return own;
	}

	/**
	* @brief Why a CS-detoured container holds another mod's hook, or empty: its entry must lead to CS's module, and the original code
	* CS's detour calls (a_original) must not start with a jump out of the game.
	*/
	std::string ForeignAt(std::string_view a_name, std::uintptr_t a_entry, std::uintptr_t a_original)
	{
		const auto game = GetModuleHandleW(nullptr);
		const auto own = OwnModule();
		const auto entry = FollowJumps(a_entry);
		if (!InModule(entry, own))
			return fmt::format("{} (entry {:#x}) leads to {:#x}, outside Community Shaders", a_name, a_entry, entry);
		if (a_original) {
			const auto original = FollowJumps(a_original);
			const auto* code = reinterpret_cast<const std::uint8_t*>(a_original);
			const bool jumps = code[0] == 0xE9 || code[0] == 0xEB || (code[0] == 0xFF && code[1] == 0x25);
			if (jumps && !InModule(original, game) && !InModule(original, own))
				return fmt::format("{}'s original code jumps to {:#x}, another module", a_name, original);
		}
		return {};
	}

	/** @brief Why an effect's vtable slot is not the engine's function a_expected, or empty. */
	std::string ForeignSlot(std::string_view a_name, const void* a_object, std::uint32_t a_slot, std::uintptr_t a_expected)
	{
		if (!a_object)
			return {};
		const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_object);
		if (vtable[a_slot] == a_expected)
			return {};
		return fmt::format("{}'s vtable slot {} is {:#x}, not the engine's {:#x}", a_name, a_slot, vtable[a_slot], a_expected);
	}
}
