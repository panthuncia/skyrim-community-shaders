// The native post chain's deferred families (docs/development/plans/native-post-chain.md, families 5 and 6): the reflections pass
// (SE 100428 / AE 107146), RenderSSAO's two calls (100520 / 107245) and the water blend (100550 / 107285). Included by ImageSpace.cpp
// after NativePost.inl, before the hooks that use it.
//
// Their containers are not called. CS reads the values the containers read, at the container's own hook, computes each step's rows
// from them and stages the steps of the world steps' build (WorldSteps.inl). Then it writes back the engine state the containers
// leave behind. A container stays the engine's only when another mod hooks it (decision 4) or the world steps have no build yet.
//
// The globals are SE/AE pairs, matched in Ghidra by the order of their references in each SE/AE function pair (perf2-native.md).
// SE is untested at runtime.

namespace NativePost
{
	/** @brief The engine globals the deferred families read and write, resolved once at Install. The comments give the AE ids. */
	struct WorldGlobals
	{
		RE::NiCamera** ssrCamera = nullptr;  // 415058: the reflections pass keeps the world camera here for its ray march
		const bool* ssrBlur = nullptr;       // 415059: the reflections are blurred (kSSR), else the composite reads kSSR_RAW
		const bool* ssrDownsample = nullptr;  // 391448: the ray march reads a downsampled normals mask
		const std::int32_t* ssrDivider = nullptr;  // 391287: the mask's divider; 4 downsamples with the compute shader
		std::array<const float*, 3> ssrRay{};  // 391433, 391436, 391442: the ray march's thickness, radius and alpha weight
		// RenderSSAO's settings: 415060 to 415066 (the raw AO's power, its z row, radius, two more rows, the contrast, camera Z's w).
		const float* saoPower = nullptr;
		const float* saoBias = nullptr;
		const float* saoRadius = nullptr;
		const float* saoRow3x = nullptr;
		const float* saoRow3z = nullptr;
		const float* saoContrast = nullptr;
		const float* saoCameraZw = nullptr;
		const bool* saoHalf = nullptr;          // 415067: the raw AO and its blur at half size
		const bool* saoContrastOn = nullptr;    // 415069
		const bool* saoFog = nullptr;           // 415070: ToggleFog; the composite fogs with the portal graph's fog
		const bool* saoReflections = nullptr;   // 415072: Main::RenderWorld copies the reflections' activity here before the composite
		const std::uint32_t* saoLevels = nullptr;  // 415050
		const bool* saoTemporal = nullptr;      // 391450
		const bool* improvedSnow = nullptr;     // 391105 bEnableImprovedSnow
		// The snow sparkles (391479 to 391524) and two reflection settings of the composite's last row (391424, 391445).
		const bool* sparkleToggle = nullptr;    // 391479
		const bool* sparkleOnly = nullptr;      // 391482
		std::array<const float*, 6> sparkle{};  // 391485, 391488, 391491, 391494, 391497, 391500
		const bool* sparkleNoAo = nullptr;      // 391503
		const float* sparkleMaxDistance = nullptr;  // 391506
		const bool* sparkleNoise = nullptr;     // 391509 bUsePrecomputedNoise
		const float* sparkleShadow = nullptr;   // 391512
		const std::int32_t* sparkleColourMode = nullptr;  // 391515
		std::array<const float*, 3> sparkleColour{};  // 391518, 391521, 391524
		const float* ssrCompositeA = nullptr;   // 391424
		const float* ssrCompositeB = nullptr;   // 391445
		const std::int32_t* portalIndex = nullptr;  // 390982
		std::uintptr_t (*portalFog)(std::int32_t) = nullptr;  // 105637: the portal graph's fog property, or null
		// BSGraphics::State's camera data for a camera and jitter flag (77507), and the State object.
		std::uintptr_t (*cameraData)(void*, const RE::NiCamera*, bool) = nullptr;
		// BSShaderManager::State (390951). Its first word is shadowSceneNode[0], whose sunShadowDirLight (+0x210) and sunLight (+0x200)
		// the rows read. CommonLib's State::GetSingleton() returns the same object.
		const std::uintptr_t* shaderState = nullptr;
	};

	WorldGlobals& WG()
	{
		static WorldGlobals globals;
		return globals;
	}

	void ResolveWorldGlobals()
	{
		auto& w = WG();
		w.ssrCamera = At<RE::NiCamera*>(528113, 415058);
		w.ssrBlur = At<bool>(528114, 415059);
		w.ssrDownsample = At<bool>(513568, 391448);
		w.ssrDivider = At<std::int32_t>(513454, 391287);
		w.ssrRay = { At<float>(513558, 391433), At<float>(513560, 391436), At<float>(513564, 391442) };
		w.saoPower = At<float>(528115, 415060);
		w.saoBias = At<float>(528116, 415061);
		w.saoRadius = At<float>(528117, 415062);
		w.saoRow3x = At<float>(528118, 415063);
		w.saoRow3z = At<float>(528119, 415064);
		w.saoContrast = At<float>(528120, 415065);
		w.saoCameraZw = At<float>(528121, 415066);
		w.saoHalf = At<bool>(528122, 415067);
		w.saoContrastOn = At<bool>(528124, 415069);
		w.saoFog = At<bool>(528125, 415070);
		w.saoReflections = At<bool>(528127, 415072);
		w.saoLevels = At<std::uint32_t>(528105, 415050);
		w.saoTemporal = At<bool>(513569, 391450);
		w.improvedSnow = At<bool>(513332, 391105);
		w.sparkleToggle = At<bool>(513589, 391479);
		w.sparkleOnly = At<bool>(513591, 391482);
		w.sparkle = { At<float>(513593, 391485), At<float>(513595, 391488), At<float>(513597, 391491), At<float>(513599, 391494), At<float>(513601, 391497),
			At<float>(513603, 391500) };
		w.sparkleNoAo = At<bool>(513605, 391503);
		w.sparkleMaxDistance = At<float>(513607, 391506);
		w.sparkleNoise = At<bool>(513609, 391509);
		w.sparkleShadow = At<float>(513611, 391512);
		w.sparkleColourMode = At<std::int32_t>(513613, 391515);
		w.sparkleColour = { At<float>(513615, 391518), At<float>(513617, 391521), At<float>(513619, 391524) };
		w.ssrCompositeA = At<float>(513552, 391424);
		w.ssrCompositeB = At<float>(513566, 391445);
		w.portalIndex = At<std::int32_t>(513241, 390982);
		w.portalFog = reinterpret_cast<decltype(w.portalFog)>(REL::RelocationID(98984, 105637).address());
		w.cameraData = reinterpret_cast<decltype(w.cameraData)>(REL::RelocationID(75698, 77507).address());
		w.shaderState = At<std::uintptr_t>(513211, 390951);
	}

	// Code constants of the containers (AE .rdata, read in Ghidra; SE's have the same values).
	constexpr std::array<float, 3> kSsrPoint{ 0.0f, 0.0f, 1.0f };  // 0x1417ebc60: the point the ray march's row 1 takes to view space
	// 0x141aba790, 770, 780, 760: the mask downsample's four taps, in the order the reflections pass copies them.
	constexpr std::array<std::array<float, 4>, 4> kSsrMaskTaps{ { { -1.0f, -1.0f, 0.25f, 0.0f }, { -1.0f, 1.0f, 0.25f, 0.0f }, { 1.0f, -1.0f, 0.25f, 0.0f },
		{ 1.0f, 1.0f, 0.25f, 0.0f } } };
	constexpr std::uint32_t kSsrMaskGrid = 1024;          // the compute mask downsample's (width * 4 + 0x3FF) >> 10
	constexpr float kSaoAngle = 7.3145906e-10f;           // 0x141abd0b0: pi / 2^32, a random 32-bit number to an angle
	constexpr float kSaoProjection = -2.0f;               // 0x1417e5604
	constexpr float kSaoRadiusScale = 100.0f;             // 0x141ad2954
	constexpr float kSaoPowerExponent = 6.0f;             // 0x141ad28bc
	constexpr float kSaoDepthScale = 0.01f;               // 0x141ad2818
	constexpr float kSaoMinifyHalf = 0.5f;                // 0x141ad285c
	constexpr float kSunLength = 1.0e-6f;                 // 0x141ad27f8
	constexpr std::array<float, 4> kNoFogRange{ 5000000.0f, 0.1f, 1.0f, 0.0f };  // 0x141abbf40: fog with equal near and far
	constexpr float kSparkleNoiseDensity = -0.5f;         // 0x141ad2acc
	constexpr float kSsrCompositeScale = 0.8f;            // 0x141ad2868
	constexpr float kWaterMoveScale = 0.025f;             // 0x14189eac8

	/** @brief A step of the RenderWorld scopes: explicit rows (the setters copy their struct as rows), zero. */
	NativeStep WorldStep(WorldScope a_scope, Effects::ImageSpaceEffectEnum a_effect, std::uint16_t a_variant, std::uint32_t a_rows)
	{
		NativeStep step{ .key = { a_scope, a_effect, a_variant }, .effect = EffectIndex(a_effect) };
		step.rows = ExplicitRows(a_rows);
		return step;
	}

	/** @brief Writes raw bits into row a_row, as the engine copies an int into its float rows. */
	void SetBits(NativeSteps::Rows& a_rows, std::uint32_t a_row, std::uint32_t a_column, std::uint32_t a_bits)
	{
		std::memcpy(a_rows.bytes.data() + (a_rows.offsets[a_row] + a_column) * 4u, &a_bits, 4);
	}

	std::pair<float, float> TargetSize(std::int32_t a_target)
	{
		const auto& data = G().targets->renderTargetData[a_target];
		return { static_cast<float>(data.width), static_cast<float>(data.height) };
	}

	/** @brief The dynamic-resolution ratios the engine's compute blur reads (1 when locked). */
	std::pair<float, float> BlurRatios()
	{
		const auto& data = globals::game::graphicsState->GetRuntimeData();
		const bool locked = data.dynamicResolutionLock != 0;
		return { locked ? 1.0f : data.dynamicResolutionWidthRatio, locked ? 1.0f : data.dynamicResolutionHeightRatio };
	}

	/**
	* @brief A compute shader's grid size, from its first macro as the engine's blur reads it (AE 0x1414d40b0: the shader's vfunc 0x68,
	* GRIDSIZE_X or GRIDSIZE_Y). Read once per shader.
	*/
	std::uint32_t GridSize(std::uintptr_t a_shader, std::string_view a_name)
	{
		static std::unordered_map<std::uintptr_t, std::uint32_t> s_sizes;
		if (const auto found = s_sizes.find(a_shader); found != s_sizes.end())
			return found->second;
		std::array<const char*, 8> macro{};
		using MacroFn = void (*)(std::uintptr_t, const char**);
		(*reinterpret_cast<MacroFn const* const*>(a_shader))[0x68 / 8](a_shader, macro.data());
		if (!macro[0] || !macro[1] || a_name != macro[0] || std::atoi(macro[1]) <= 0)
			RenderGraph::Fatal("ImageSpace", fmt::format("native post chain: a compute blur's first macro is not {}", a_name));
		return s_sizes[a_shader] = static_cast<std::uint32_t>(std::atoi(macro[1]));
	}

	/** @brief The reflections pass's inputs, read at its hook. */
	struct ReflectionInputs
	{
		std::uint16_t normals = 0;  // the normals parity (AE 0x1414d0490)
		std::int32_t divider = 0;   // 0 without the mask downsample
		bool blur = false;
		std::array<float, 4> ray{};
		std::array<float, 3> point{};  // kSsrPoint in the world camera's view space
		std::pair<float, float> main{}, mask{}, raw{}, blurred{};
		std::pair<float, float> ratios{ 1.0f, 1.0f };
		std::uint32_t gridX = 0, gridY = 0;
	};

	/** @brief Render owner, at the reflections pass's hook, after the camera data is set. */
	ReflectionInputs ReadReflections(RE::NiCamera* a_camera)
	{
		using T = RE::RENDER_TARGET;
		const auto& w = WG();
		ReflectionInputs in;
		in.normals = EngineNormalsParity();
		if (*w.ssrDownsample && *w.ssrDivider > 1)
			in.divider = *w.ssrDivider;
		in.blur = *w.ssrBlur;
		const float radius = *w.ssrRay[0];
		in.ray = { radius, *w.ssrRay[1], *w.ssrRay[2], 1.0f / radius };
		// The ray march's row 1: the camera data's view matrix (+0x30, row vectors) applied to the point (AE 0x1414d54b0).
		const auto data = w.cameraData(RE::BSGraphics::State::GetSingleton(), a_camera, false);
		if (data < 0x10000)
			RenderGraph::Fatal("ImageSpace", "native post chain: the State has no camera data for the reflections pass's camera");
		const auto m = [&](std::uintptr_t a_offset) { return Field<float>(data, a_offset); };
		for (std::uint32_t c = 0; c < 3; ++c)
			in.point[c] = kSsrPoint[1] * m(0x40 + c * 4) + kSsrPoint[0] * m(0x30 + c * 4) + kSsrPoint[2] * m(0x50 + c * 4) + m(0x60 + c * 4);
		in.main = TargetSize(T::kMAIN);
		in.mask = TargetSize(T::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED);
		in.raw = TargetSize(T::kSSR_RAW);
		in.blurred = TargetSize(T::kSSR_BLURRED0);
		if (in.blur) {
			in.ratios = BlurRatios();
			const auto blur = Field<std::uintptr_t>(ManagerBase(), 0x188);
			in.gridX = GridSize(Field<std::uintptr_t>(blur, 0x10), "GRIDSIZE_X");
			in.gridY = GridSize(Field<std::uintptr_t>(blur, 0x18), "GRIDSIZE_Y");
		}
		return in;
	}

	/**
	* @brief The reflections pass's steps (AE 107146, its ray march 0x1414d54b0 and blur 0x1414d3fe0): the mask downsample at divider 2
	* or 3 (raster) or 4 (compute), the ray march into kSSR_RAW, and the blur into kSSR_BLURRED0 and kSSR.
	*/
	std::vector<NativeStep> ReflectionSteps(const ReflectionInputs& a_in)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		const bool raster = a_in.divider > 1 && a_in.divider != 4;
		if (a_in.divider == 4) {
			auto step = WorldStep(WorldScope::Reflections, E::ISDownsampleIgnoreBrightestCS, a_in.normals, 19);
			step.compute = true;
			SetId(step.rows, 0, 1.0f / a_in.main.first, 1.0f / a_in.main.second);
			SetBits(step.rows, 0, 2, 4);
			for (std::uint32_t t = 0; t < 4; ++t)
				SetId(step.rows, 1 + t, kSsrMaskTaps[t][0], kSsrMaskTaps[t][1], kSsrMaskTaps[t][2], kSsrMaskTaps[t][3]);
			step.groups = { (static_cast<std::uint32_t>(a_in.mask.first) * 4 + kSsrMaskGrid - 1) / kSsrMaskGrid, static_cast<std::uint32_t>(a_in.mask.second), 1 };
			steps.push_back(std::move(step));
		} else if (raster) {
			auto step = WorldStep(WorldScope::Reflections, E::ISDownsampleIgnoreBrightest, a_in.normals, 17);
			SetId(step.rows, 0, 1.0f / a_in.main.first, 1.0f / a_in.main.second);
			SetBits(step.rows, 0, 2, 4);
			SetBits(step.rows, 0, 3, 1);
			for (std::uint32_t t = 0; t < 4; ++t)
				SetId(step.rows, 1 + t, kSsrMaskTaps[t][0], kSsrMaskTaps[t][1], kSsrMaskTaps[t][2], kSsrMaskTaps[t][3]);
			steps.push_back(std::move(step));
		}
		auto ray = WorldStep(WorldScope::Reflections, E::ISReflectionsRayTracing, raster ? std::uint16_t(2) : a_in.normals, 2);
		SetId(ray.rows, 0, a_in.ray[0], a_in.ray[1], a_in.ray[2], a_in.ray[3]);
		SetId(ray.rows, 1, a_in.point[0], a_in.point[1], a_in.point[2], 0.0f);
		steps.push_back(std::move(ray));
		if (a_in.blur) {
			auto h = WorldStep(WorldScope::Reflections, E::ISReflectionBlurHCS, 0, 2);
			SetBlurCs(h, a_in.raw, a_in.blurred, a_in.ratios.first, a_in.ratios.second, false, a_in.gridX);
			steps.push_back(std::move(h));
			auto v = WorldStep(WorldScope::Reflections, E::ISReflectionBlurVCS, 0, 2);
			SetBlurCs(v, a_in.blurred, TargetSize(RE::RENDER_TARGET::kSSR), a_in.ratios.first, a_in.ratios.second, true, a_in.gridY);
			steps.push_back(std::move(v));
		}
		return steps;
	}

	/** @brief RenderSSAO's inputs, read at its hook. The camera is the current one, as the engine reads it. */
	struct SsaoInputs
	{
		float nearZ = 0, farZ = 0, frustumX = 1, frustumY = 1;  // frustumX = 2 / (right - left), frustumY = 2 / (top - bottom)
		std::uint32_t width = 0, height = 0;  // kMAIN
		std::uint32_t levels = 0;
		bool contrastOn = false, half = false, temporal = false;
		float power = 0, bias = 0, radius = 0, row3x = 0, row3z = 0, contrast = 0, cameraZw = 0;
		float angle = 0;
		std::uint16_t normals = 0, history = 0;  // the normals parity; the history CS writes (its frame parity)
	};

	/** @brief The random angle RenderSSAO draws each first call. CS's own generator: only the distribution is the engine's. */
	float SsaoAngle()
	{
		static std::mt19937 s_random{ 5489u };
		return static_cast<float>(s_random()) * kSaoAngle;
	}

	SsaoInputs ReadSsao()
	{
		const auto& w = WG();
		const auto camera = reinterpret_cast<std::uintptr_t>(*G().currentCamera);
		SsaoInputs in;
		in.nearZ = Field<float>(camera, 0x160);
		in.farZ = Field<float>(camera, 0x164);
		in.frustumX = 1.0f / (Field<float>(camera, 0x154) - Field<float>(camera, 0x150));
		in.frustumX += in.frustumX;
		in.frustumY = 1.0f / (Field<float>(camera, 0x158) - Field<float>(camera, 0x15c));
		in.frustumY += in.frustumY;
		const auto& main = G().targets->renderTargetData[RE::RENDER_TARGET::kMAIN];
		in.width = main.width;
		in.height = main.height;
		in.levels = *w.saoLevels;
		in.contrastOn = *w.saoContrastOn;
		in.half = *w.saoHalf;
		in.temporal = *w.saoTemporal;
		in.power = *w.saoPower;
		in.bias = *w.saoBias;
		in.radius = *w.saoRadius;
		in.row3x = *w.saoRow3x;
		in.row3z = *w.saoRow3z;
		in.contrast = *w.saoContrast;
		in.cameraZw = *w.saoCameraZw;
		in.angle = SsaoAngle();
		in.normals = EngineNormalsParity();
		in.history = static_cast<std::uint16_t>(Frame::Driver::Current().parity);
		return in;
	}

	/**
	* @brief RenderSSAO's first call (AE 107245): camera Z, its minify levels (the first level at most 16 high in the contrast form),
	* the raw AO into CS's history of this frame's parity, and its blur into CS's kSAO.
	*/
	std::vector<NativeStep> SsaoSteps(const SsaoInputs& a_in)
	{
		using E = Effects;
		std::vector<NativeStep> steps;
		const float w = static_cast<float>(a_in.width), h = static_cast<float>(a_in.height);
		auto cameraZ = WorldStep(WorldScope::Ssao, E::ISSAOCameraZ, 0, 2);
		SetId(cameraZ.rows, 0, a_in.farZ * a_in.nearZ, a_in.nearZ - a_in.farZ, a_in.farZ, a_in.cameraZw);
		SetId(cameraZ.rows, 1, w, h);
		steps.push_back(std::move(cameraZ));
		bool contrast = a_in.contrastOn;
		for (std::uint32_t level = 1; level < a_in.levels; ++level) {
			const auto lw = static_cast<float>(a_in.width >> (level - 1));
			const auto lhBits = a_in.height >> (level - 1);
			const auto lh = static_cast<float>(lhBits);
			const bool form = contrast && lhBits <= 16;
			auto step = WorldStep(WorldScope::Ssao, form ? E::ISMinifyContrast : E::ISMinify, static_cast<std::uint16_t>(level), 3);
			SetId(step.rows, 0, lw, lh, level < 5 ? kSaoMinifyHalf / lw : 0.0f, level < 5 ? kSaoMinifyHalf / lh : 0.0f);
			if (form) {
				SetId(step.rows, 1, 1.0f / (a_in.contrast + kDofEpsilon));
				contrast = false;
			}
			SetBits(step.rows, 2, 0, level == 1 ? 1u : 0u);
			steps.push_back(std::move(step));
		}
		const auto width = a_in.half ? a_in.width >> 1 : a_in.width;
		const auto height = a_in.half ? a_in.height >> 1 : a_in.height;
		const float sw = static_cast<float>(width), sh = static_cast<float>(height);
		auto raw = WorldStep(WorldScope::Ssao, a_in.temporal ? E::ISSAORawAO : E::ISSAORawAONoTemporal, static_cast<std::uint16_t>(a_in.normals | (a_in.history << 1)), 4);
		SetId(raw.rows, 0, kSaoProjection / (sw * a_in.frustumX), kSaoProjection / (sh * a_in.frustumY), 1.0f / a_in.frustumX, 1.0f / a_in.frustumY);
		SetId(raw.rows, 1, a_in.angle, a_in.radius * kSaoRadiusScale, a_in.bias, a_in.power / std::pow(a_in.radius, kSaoPowerExponent));
		SetId(raw.rows, 2, sw, sh, 1.0f / sw, 1.0f / sh);
		SetId(raw.rows, 3, a_in.row3x, a_in.radius * a_in.radius, a_in.row3z,
			(a_in.farZ * a_in.nearZ) / ((a_in.nearZ - a_in.farZ) * kSaoDepthScale + a_in.farZ));
		steps.push_back(std::move(raw));
		auto blurH = WorldStep(WorldScope::Ssao, E::ISSAOBlurH, static_cast<std::uint16_t>(a_in.history << 1), 1);
		SetId(blurH.rows, 0, sw, sh, 1.0f / sw, 1.0f / sh);
		steps.push_back(std::move(blurH));
		auto blurV = WorldStep(WorldScope::Ssao, E::ISSAOBlurV, 0, 1);
		SetId(blurV.rows, 0, sw, sh, 1.0f / sw, 1.0f / sh);
		steps.push_back(std::move(blurV));
		return steps;
	}

	/** @brief RenderSSAO's second call's inputs, read at its hook. */
	struct CompositeInputs
	{
		std::uint8_t form = 3;  // 0 SAO, 1 SAO and fog, 2 fog, 3 a copy
		std::uint16_t normals = 0;
		bool blur = false;
		NativeSteps::Rows rows;  // the eleven rows every form but the copy shares
	};

	CompositeInputs ReadComposite(std::uintptr_t a_sao)
	{
		const auto& w = WG();
		CompositeInputs in;
		const bool sao = Field<bool>(a_sao, 0x50);
		const auto fog = w.portalFog(*w.portalIndex);
		const bool fogged = fog && *w.saoFog;
		in.form = static_cast<std::uint8_t>(sao ? (fogged ? 1 : 0) : (fogged ? 2 : 3));
		in.normals = EngineNormalsParity();
		in.blur = *w.ssrBlur;
		if (in.form == 3)
			return in;
		auto& rows = in.rows;
		rows = ExplicitRows(11);
		const auto camera = reinterpret_cast<std::uintptr_t>(*G().currentCamera);
		if (fogged) {
			const float nearFog = Field<float>(fog, 0x50), farFog = Field<float>(fog, 0x54);
			if (farFog == nearFog)
				SetId(rows, 0, kNoFogRange[0], kNoFogRange[1], kNoFogRange[2], kNoFogRange[3]);
			else {
				const float scale = 1.0f / (farFog - nearFog);
				SetId(rows, 0, scale * nearFog, scale, Field<float>(fog, 0x84), Field<float>(fog, 0x88));
			}
			const float range = *G().range;
			SetId(rows, 1, Field<float>(fog, 0x38), Field<float>(fog, 0x3c), Field<float>(fog, 0x40), range);
			SetId(rows, 2, Field<float>(fog, 0x44), Field<float>(fog, 0x48), Field<float>(fog, 0x4c), range);
		}
		SetId(rows, 3, Field<float>(camera, 0x160), Field<float>(camera, 0x164));
		// The sun's direction: its light's world rotation, first column, normalised (zero when shorter than kSunLength).
		const auto shadowSceneNode = *w.shaderState;  // State::shadowSceneNode[0]
		if (const auto sun = Field<std::uintptr_t>(shadowSceneNode, 0x210)) {
			const auto light = Field<std::uintptr_t>(sun, 0x48);
			const float x = Field<float>(light, 0x7c), y = Field<float>(light, 0x88), z = Field<float>(light, 0x94);
			const float length = std::sqrt(y * y + x * x + z * z);
			if (length > kSunLength) {
				const float inverse = 1.0f / length;
				SetId(rows, 4, inverse * x, inverse * y, inverse * z);
			}
		}
		SetId(rows, 5, Field<float>(camera, 0xa0), Field<float>(camera, 0xa4), Field<float>(camera, 0xa8), *w.improvedSnow ? 1.0f : 0.0f);
		const bool noise = *w.sparkleNoise;
		SetId(rows, 6, *w.sparkle[0], *w.sparkle[5], *w.sparkle[2], noise ? *w.sparkle[1] + kSparkleNoiseDensity : *w.sparkle[1]);
		SetId(rows, 7, noise ? *w.sparkle[3] + 1.0f : *w.sparkle[3], *w.sparkle[4], *w.sparkleToggle ? 1.0f : 0.0f, *w.sparkleOnly ? 1.0f : 0.0f);
		SetId(rows, 8, *w.sparkleNoAo ? 1.0f : 0.0f, *w.sparkleMaxDistance, noise ? 1.0f : 0.0f);
		std::array<float, 3> colour{};
		switch (*w.sparkleColourMode) {
		case 0:
			colour = { Field<float>(a_sao, 0x58), Field<float>(a_sao, 0x5c), Field<float>(a_sao, 0x60) };
			break;
		case 1:
			colour = { Field<float>(a_sao, 0x64), Field<float>(a_sao, 0x68), Field<float>(a_sao, 0x6c) };
			break;
		case 2:
			{
				const auto light = Field<std::uintptr_t>(Field<std::uintptr_t>(shadowSceneNode, 0x200), 0x48);
				colour = { Field<float>(light, 0x11c), Field<float>(light, 0x120), Field<float>(light, 0x124) };
				break;
			}
		case 3:
			colour = { *w.sparkleColour[0], *w.sparkleColour[1], *w.sparkleColour[2] };
			break;
		default:
			break;
		}
		SetId(rows, 9, *w.sparkleShadow, colour[0], colour[1], colour[2]);
		SetId(rows, 10, *w.ssrCompositeA * kSsrCompositeScale, *w.ssrCompositeB, *w.saoReflections ? 1.0f : 0.0f);
		return in;
	}

	/** @brief RenderSSAO's second call: the composite of its form into kMAIN, or the copy (RenderEffect with ISCopy). */
	NativeStep CompositeStep(const CompositeInputs& a_in)
	{
		using E = Effects;
		if (a_in.form == 3)
			return Step(WorldScope::SsaoComposite, E::ISCopy, static_cast<std::uint16_t>(3 << 8), 0);
		constexpr std::array<E::ImageSpaceEffectEnum, 3> kForms{ E::ISSAOCompositeSAO, E::ISSAOCompositeSAOFog, E::ISSAOCompositeFog };
		const auto variant = static_cast<std::uint16_t>((a_in.form << 8) | ((a_in.blur ? 1u : 0u) << 1) | a_in.normals);
		NativeStep step{ .key = { WorldScope::SsaoComposite, kForms[a_in.form], variant }, .effect = EffectIndex(kForms[a_in.form]) };
		step.rows = a_in.rows;
		return step;
	}

	/** @brief The water blend's row (AE 107285): the current camera's near and far, the menu flag, and how far the eye moved. */
	NativeStep WaterBlendStep()
	{
		const auto camera = reinterpret_cast<std::uintptr_t>(*G().currentCamera);
		auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		const auto now = shadow.posAdjust.getEye(), before = shadow.previousPosAdjust.getEye();
		const float dx = before.x - now.x, dy = before.y - now.y, dz = before.z - now.z;
		const float moved = (std::max)(1.0f - std::sqrt(dy * dy + dx * dx + dz * dz) * kWaterMoveScale, 0.0f);
		auto* ui = RE::UI::GetSingleton();
		auto step = WorldStep(WorldScope::WaterBlend, Effects::ISWaterBlend, static_cast<std::uint16_t>(EngineFrameCounter() & 1u), 1);
		SetId(step.rows, 0, Field<float>(camera, 0x160), Field<float>(camera, 0x164), ui && ui->numPausesGame != 0 ? 1.0f : 0.0f, moved);
		return step;
	}

	/** @brief Whether a deferred family's container is replaced now: CS draws the world steps and no other mod hooks it. Render owner. */
	bool WorldNative(std::uint8_t a_family)
	{
		return Usable() && World().group.Registered() && Native(a_family) && G().ready;
	}

	/** @brief Stages a_steps in one scope of a_kind. */
	void StageScope(ScopeKind a_kind, const std::vector<NativeStep>& a_steps)
	{
		EnterScope(a_kind);
		for (const auto& step : a_steps)
			Stage(step);
		LeaveScope();
	}

	/**
	* @brief The reflections pass without its container. Write-back: the State's camera data for the world camera (the pass sets it
	* for what follows in RenderPreResolve), the camera and normals target it keeps, its drawn flag, and the targets it leaves unbound.
	*/
	void RunReflections()
	{
		ZoneScopedN("CS.PostNative.Reflections");
		auto& r = R();
		const auto& g = G();
		const auto ssr = Field<std::uintptr_t>(ManagerBase(), 0x1c0);
		++r.native[4];
		if (!ssr || !Field<bool>(ssr, 0x10))
			return;
		auto* camera = reinterpret_cast<RE::NiCamera*>(Field<std::uintptr_t>(*g.worldSceneGraph, 0x128));
		RE::BSGraphics::State::GetSingleton()->SetCameraData(camera, 1);
		g.setDepth(g.targets, -1, 3, false);
		*WG().ssrCamera = camera;
		const auto in = ReadReflections(camera);
		// The normals target it keeps (+0x34): the downsampled mask after the raster downsample, else the frame's normals.
		Field<std::int32_t>(ssr, 0x34) = in.divider > 1 && in.divider != 4 ? static_cast<std::int32_t>(RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED) :
		                                                                     static_cast<std::int32_t>(RE::RENDER_TARGET::kNORMAL_TAAMASK_SSRMASK) + in.normals;
		StageScope(ScopeKind::Reflections, ReflectionSteps(in));
		Field<bool>(ssr, 0x11) = true;
		g.setTarget(g.targets, 0, -1, 3, true);
		g.setTarget(g.targets, 1, -1, 3, true);
	}

	/**
	* @brief RenderSSAO without its container. The first call bumps the SAO counter (+0x54) and leaves kSAO bound; the composite leaves
	* its destination bound with the main depth target. Returns a_destination, as the engine does.
	*/
	std::uint64_t RunSsao(void* a_helper, bool a_composite, std::uint64_t a_destination)
	{
		ZoneScopedN("CS.PostNative.Ssao");
		auto& r = R();
		const auto& g = G();
		const auto sao = reinterpret_cast<std::uintptr_t>(a_helper);
		++r.native[5];
		if (!a_composite) {
			auto& counter = Field<std::int32_t>(sao, 0x54);
			counter = counter + 1 > 1 ? 0 : counter + 1;
			const auto in = ReadSsao();
			StageScope(ScopeKind::SSAO, SsaoSteps(in));
			g.setDepth(g.targets, -1, 3, false);
			g.setTarget(g.targets, 0, static_cast<std::int32_t>(RE::RENDER_TARGET::kSAO) + (in.half ? 1 : 0), 3, true);
		} else {
			StageScope(ScopeKind::SSAO, { CompositeStep(ReadComposite(sao)) });
			g.setTarget(g.targets, 0, static_cast<std::int32_t>(a_destination), 3, true);
			g.setDepth(g.targets, 0, 3, false);
		}
		return a_destination & 0xffffffff;
	}

	/** @brief The water blend's clear request (AE 107285, bit 0 of +0x0C): kWATER_1, kWATER_2 and kRAW_WATER to zero. */
	void ClearWaterTargets()
	{
		constexpr FLOAT kZero[4]{};
		for (const auto target : { RE::RENDER_TARGET::kWATER_1, RE::RENDER_TARGET::kWATER_2, RE::RENDER_TARGET::kRAW_WATER }) {
			auto* view = globals::game::renderer->GetRuntimeData().renderTargets[target].RTV;
			if (view && !RenderGraph::Engine::Skeleton::Sites::TakeColourClear(globals::d3d::context, view, kZero))
				globals::d3d::context->ClearRenderTargetView(Tripwire::Unwrap(view), kZero);
		}
	}

	/**
	* @brief The water blend without its container: the clear request, then the blend into a_destination and the water history of the
	* frame's parity. Write-back: no targets bound, depth test off, blend mode 0 and write mode 1, as it leaves them. Returns the target
	* the frame continues in: a_source when the blend is off.
	*/
	std::int32_t RunWaterBlend(void* a_blend, std::int32_t a_source, std::int32_t a_destination)
	{
		ZoneScopedN("CS.PostNative.WaterBlend");
		const auto blend = reinterpret_cast<std::uintptr_t>(a_blend);
		++R().native[6];
		if (!Field<bool>(blend, 0x08))
			return a_source;
		if (auto& clear = Field<std::int32_t>(blend, 0x0c); clear != 0) {
			if (clear & 1)
				ClearWaterTargets();
			clear = 0;
		}
		StageScope(ScopeKind::Effect, { WaterBlendStep() });
		const auto& g = G();
		g.setTarget(g.targets, 1, -1, 3, true);
		g.setTarget(g.targets, 0, -1, 3, true);
		auto& shadow = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		if (shadow.alphaBlendWriteMode != 1) {
			shadow.alphaBlendWriteMode = 1;
			shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
		}
		if (shadow.alphaBlendMode != 0) {
			shadow.alphaBlendMode = 0;
			shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
		}
		// As the engine's depth mode setter: the flag follows whether the previous mode differs.
		if (shadow.depthStencilDepthMode != RE::BSGraphics::DepthStencilDepthMode::kDisabled) {
			shadow.depthStencilDepthMode = RE::BSGraphics::DepthStencilDepthMode::kDisabled;
			if (shadow.depthStencilDepthModePrevious == RE::BSGraphics::DepthStencilDepthMode::kDisabled)
				shadow.stateUpdateFlags.reset(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
			else
				shadow.stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
		}
		return a_destination;
	}
}
