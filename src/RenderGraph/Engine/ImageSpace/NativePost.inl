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
// Native mode is AE only: the globals below are AE ids, and SE has no pairs for them yet. SE runs every family on the engine.

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

	/** @brief The engine globals the families read and write back. AE ids (1.6.1170), resolved once at Install. */
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
		bool ready = false;
	};

	Globals& G()
	{
		static Globals globals;
		return globals;
	}

	template <class T>
	T* At(std::uint64_t a_id, std::ptrdiff_t a_offset = 0)
	{
		return reinterpret_cast<T*>(REL::ID(a_id).address() + a_offset);
	}

	void ResolveGlobals()
	{
		auto& g = G();
		if (!REL::Module::IsAE())
			return;
		g.filmic = At<bool>(391339);
		g.reinhardWhite = At<float>(391342);
		g.filmicWhite = At<float>(391345);
		g.saturationBoost = At<float>(391312);
		g.contrastBoost = At<float>(391305);
		g.contrastBoostMap = At<float>(391324);
		g.brightnessBoost = At<float>(391309);
		g.brightnessBoostMap = At<float>(391321);
		g.adaptSpeed = At<float>(391330);
		g.adaptStrength = At<float>(391333);
		g.menuTimer = At<float>(391327);
		g.bloomBoost = At<float>(391315);
		g.bloomBoostMap = At<float>(391318);
		g.stopAt2 = At<bool>(391336);
		g.worldMap = At<bool>(415052);
		g.adaptOff = At<bool>(415051);
		g.adaptReset = At<bool>(415053);
		g.frameTime = At<float>(390954);
		g.range = At<float>(390973);
		g.taaReset = At<bool>(391591);
		g.taaK = At<float>(391592);
		g.taaBlend = At<float>(391593);
		g.taaThreshold = At<float>(391611);
		g.lumParity = At<std::int32_t>(415054);
		g.adapted = At<RE::ImageSpaceTexture>(391302);
		g.blurWeights = At<std::byte>(391307, -0xE8);
		g.eof = At<bool>(391055);
		g.hdr = At<bool>(391301);
		g.volumetric = At<bool>(414913);
		g.currentCamera = At<RE::NiCamera*>(414580);
		g.worldSceneGraph = At<std::uintptr_t>(415032);
		g.dofCamera = At<std::uintptr_t>(415007);
		g.frameCounter = At<std::uint32_t>(411489);
		g.targets = At<RE::BSGraphics::RenderTargetManager>(411451);
		g.vlSettings = reinterpret_cast<decltype(g.vlSettings)>(REL::ID(107014).address());
		g.vlColour = reinterpret_cast<decltype(g.vlColour)>(REL::ID(107017).address());
		g.applyVl = reinterpret_cast<decltype(g.applyVl)>(REL::ID(107193).address());
		g.blurVl = reinterpret_cast<decltype(g.blurVl)>(REL::ID(107206).address());
		g.lensFlares = reinterpret_cast<decltype(g.lensFlares)>(REL::ID(106988).address());
		g.composites = reinterpret_cast<decltype(g.composites)>(REL::ID(105675).address());
		g.setTarget = reinterpret_cast<decltype(g.setTarget)>(REL::ID(77453).address());
		g.setDepth = reinterpret_cast<decltype(g.setDepth)>(REL::ID(77454).address());
		g.eofCallSite = REL::ID(107148).address() + 0x1E7;
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

	/** @brief ImageSpaceManager +0xC8 to +0x150: the blended ImageSpace data (HDR, cinematic, tint, fade, DoF, blurs). */
	constexpr std::uintptr_t kDataFirst = 0xC8, kDataEnd = 0x150;

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

	struct Runtime
	{
		ImageSpaceFrame frame;
		PostState state;
		std::uint64_t frameNumber = UINT64_MAX;
		// Observe mode: the steps CS predicts, in order, and the post state it predicts after them.
		std::deque<std::pair<StepKey, NativeSteps::Rows>> expected;
		std::optional<PostState> predicted;
		bool predicting = false;
		std::uint64_t compared = 0, differed = 0, staged = 0;
		// Per-family counts of frames run natively and on the engine path, for the log.
		std::array<std::uint64_t, 4> native{}, engine{};
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
						 "end-of-frame frames on the engine {} (last slots {:#x})",
				r.compared, r.differed, r.staged, r.native[0], r.native[1], r.native[2], r.native[3], r.engine[1], r.lastSlots);
		r.frameNumber = Frame::Driver::Current().frame;
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

	struct NativeStep
	{
		StepKey key;
		std::uint8_t group = 0;
		std::size_t effect = 0;
		NativeSteps::Rows rows;
	};

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
			r.expected.emplace_back(step.key, step.rows);
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

	void Compare(const StepKey& a_key, const NativeSteps::Rows& a_predicted, const NativeSteps::Rows& a_engine)
	{
		auto& r = R();
		++r.compared;
		if (ReadsNoRows(a_key.effect))
			return;
		std::string differences;
		// An explicit step's engine rows are the whole pool buffer its setter bound. Only the setter's struct is the step's.
		const bool param = TableOf(EffectIndex(a_key.effect)).count > 0;
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

void ObservePostStep(const StepKey& a_key, std::size_t a_effect)
{
	using namespace NativePost;
	auto& r = R();
	// The slot effects run through the engine's RenderEffect in every mode, so their steps are not predicted.
	if (!r.predicting || a_key.scope == WorldScope::PreSlot || a_key.scope == WorldScope::LdrSlot)
		return;
	auto& native = NativeSteps::Get();
	if (r.expected.empty()) {
		++r.differed;
		Mismatches().Report("an engine step CS did not predict", KeyName(a_key));
		return;
	}
	auto [key, rows] = std::move(r.expected.front());
	r.expected.pop_front();
	if (!(key == a_key)) {
		++r.differed;
		Mismatches().Report(fmt::format("the step CS predicted, {}", KeyName(key)), fmt::format("the engine drew {}", KeyName(a_key)));
		return;
	}
	// The engine's rows are known when the step is native per effect (CS_ORG_IMAGESPACE_NATIVE=2): BuildRows or BoundRows filled them.
	if (native.armed && native.effect == a_effect)
		Compare(a_key, rows, native.rows);
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
		for (const auto& [key, rows] : r.expected) {
			++r.differed;
			Mismatches().Report(fmt::format("a step CS predicted in {} that the engine did not draw", a_where), KeyName(key));
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
		const bool taken = StageChainKey(a_step.key, a_step.group, a_step.effect, false, {});
		native.armed = false;
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
		auto* manager = RE::ImageSpaceManager::GetSingleton();
		static REL::Relocation<void (*)(void*, std::uint32_t, std::uint64_t, std::uint64_t, std::uint64_t)> renderEffect{ REL::ID(105676) };
		const auto route = RouteOf(f, a_source, a_final, SpareTarget());
		// The active slot count and the last slot, as the engine counts them.
		Field<std::int32_t>(base, 0x3c) = route.slotCount;
		Field<std::int32_t>(base, 0x40) = route.lastSlot;
		EnterScope(ScopeKind::Effect);
		for (const auto& call : route.pre) {
			ZoneScopedN("CS.PostNative.SlotEffect (engine RenderEffect)");
			renderEffect(manager, call.slot, static_cast<std::uint64_t>(call.source), static_cast<std::uint64_t>(call.destination), 0);
		}
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
		for (const auto& call : route.ldr) {
			ZoneScopedN("CS.PostNative.SlotEffect (engine RenderEffect)");
			renderEffect(manager, call.slot, static_cast<std::uint64_t>(call.source), static_cast<std::uint64_t>(call.destination), 0);
		}
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
		if (r.frame.hdr && !(globals::state && globals::state->PeekTonemapOwner() == ::State::TonemapOwner::kEffects11))
			Expect(HdrSteps(r.frame, predicted, route.hdrSource, HeldLevels()));
		else if (!r.frame.hdr)
			Expect({ Step(WorldScope::HdrCopy, Effects::ISCopy, 0, ChainGroup::kHdrCopy) });
		Expect(EofTailSteps(r.frame, predicted, route));
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
