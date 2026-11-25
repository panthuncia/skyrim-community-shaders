// The sky batch's draws computed from the engine's registered passes (docs/development/plans/dclf-native-sky.md). Render thread.
// The engine functions this follows (AE 1.6.1170): BSSkyShaderProperty::GetRenderPasses 0x1414e6fd0, RenderBatches 0x1414b4fe0 and
// its group step 0x1414f39c0, BSSkyShader::SetupTechnique 0x1414e5d00 and SetupGeometry 0x1414e6030, the transform 0x1414aaf10.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Features/Effects11.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace DCLF::Features::Sky::Native
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		Mode s_mode = Mode::Off;

		namespace Address
		{
			// The engine globals SetupGeometry and SetupTechnique read (AE, SE).
			inline constexpr Engine::Offset kCloudOffsets{ { 415144 }, { 528203 } };    // NiPoint2[]: each cloud layer's texture offset
			inline constexpr Engine::Offset kSkyColours{ { 415145 }, { 528199 } };      // NiColorA[3]: the atmosphere's BlendColor
			inline constexpr Engine::Offset kVParams{ { 390973 }, { 513233 } };         // float: VParams, and PParams.y's scale
			inline constexpr Engine::Offset kDefaultTexture{ { 411499 }, { 525017 } };  // NiSourceTexture*: t0 of SunOcclude and Stars
			inline constexpr Engine::Offset kWhiteTexture{ { 411503 }, { 525021 } };    // NiSourceTexture*: t0 of the others without a base
			inline constexpr Engine::Offset kNullTexture{ { 411502 }, { 525020 } };     // NiSourceTexture*: the null texture clouds compare with
			inline constexpr Engine::Offset kNoiseTexture{ { 411504 }, { 525022 } };    // NiSourceTexture*: t2, the noise gradient
		}

		template <class T>
		T Global(const Engine::Offset& a_offset)
		{
			return *reinterpret_cast<const T*>(REL::Offset(a_offset).address());
		}

		template <class T>
		T Field(const void* a_object, std::size_t a_offset)
		{
			T value;
			std::memcpy(&value, static_cast<const std::byte*>(a_object) + a_offset, sizeof(T));
			return value;
		}

		// BSSkyShaderProperty fields (SetupGeometry, GetRenderPasses).
		constexpr std::size_t kBlendColor = 0x88;      // NiColorA
		constexpr std::size_t kBaseTexture = 0x98;     // NiSourceTexture*
		constexpr std::size_t kBlendTexture = 0xA0;    // NiSourceTexture*
		constexpr std::size_t kBlendValue = 0xB8;      // float fBlendValue
		constexpr std::size_t kCloudLayer = 0xBC;      // std::uint16_t usCloudLayer
		constexpr std::size_t kFadeSecond = 0xBE;      // bool bFadeSecondTexture
		constexpr std::size_t kSkyObjectType = 0xC0;   // std::uint32_t
		// BSGeometry: the world and previous world transforms (NiTransform, 13 floats).
		constexpr std::size_t kWorld = 0x7C;
		constexpr std::size_t kPreviousWorld = 0xB0;

		constexpr std::uint32_t kFirstPass = 0x5C00005D;
		constexpr std::uint32_t kLastPass = 0x5C000064;

		/** @brief SetupTechnique's raw technique for a pass id (BSSkyShader::SetupTechnique's switch). */
		std::uint32_t TechniqueOf(std::uint32_t a_pass)
		{
			switch (a_pass) {
			case 0x5C00005E:
				return 8;
			case 0x5C006072:
				return 1;
			case 0x5C00005F:
				return 2;
			case 0x5C000060:
				return 3;
			case 0x5C000061:
				return 7;
			case 0x5C000062:
				return 4;
			case 0x5C000063:
				return 5;
			case 0x5C000064:
				return 6;
			default:
				return 0;
			}
		}

		/** @brief Which of t0-t2 a technique samples, as the capture binds them (SkyCapture.cpp's TexturesOf). */
		std::array<bool, 3> TexturesOf(std::uint32_t a_technique)
		{
			switch (a_technique) {
			case 1:
				return { true, false, true };
			case 2:
			case 3:
			case 4:
			case 6:
			case 7:
				return { true, false, false };
			case 5:
				return { true, true, false };
			case 8:
				return { false, false, true };
			default:
				return { false, false, false };
			}
		}

		/** @brief The shadow state's mode fields a sky draw depends on, as the engine sets them pass by pass. */
		struct Modes
		{
			std::uint32_t depth = 0, stencil = 0, fill = 0, cull = 0, depthBias = 0, scissor = 0;
			std::uint32_t blend = 0, alphaToCoverage = 0, write = 0, blendExtra = 0;
			bool alphaTest = false;

			static Modes Current()
			{
				auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				Modes m;
				m.depth = static_cast<std::uint32_t>(rss.depthStencilDepthMode);
				m.stencil = rss.depthStencilStencilMode;
				m.fill = rss.rasterStateFillMode;
				m.cull = rss.rasterStateCullMode;
				m.depthBias = rss.rasterStateDepthBiasMode;
				m.scissor = rss.rasterStateScissorMode;
				m.blend = rss.alphaBlendMode;
				m.alphaToCoverage = rss.alphaBlendAlphaToCoverage;
				m.write = rss.alphaBlendWriteMode;
				m.blendExtra = rss.alphaBlendModeExtra;
				m.alphaTest = rss.alphaTestEnabled;
				return m;
			}

			/** @brief BSSkyShader::SetupTechnique (p5/sky.md). */
			void Setup(std::uint32_t a_technique)
			{
				depth = 1;
				switch (a_technique) {
				case 0:
					write = 0;
					break;
				case 1:
					depth = 0;
					blend = 2;
					write = 11;
					break;
				case 2:
					alphaTest = true;
					write = 0;
					depth = 3;
					depthBias = 3;
					break;
				case 3:
					blend = 2;
					break;
				case 4:
				case 5:
				case 6:
				case 7:
					blend = 1;
					depthBias = 2;
					break;
				case 8:
					blend = 1;
					break;
				default:
					break;
				}
			}

			/** @brief BSSkyShader::RestoreTechnique (0x1412fa810). */
			void Restore()
			{
				blend = 0;
				write = 1;
				depth = 3;
				alphaTest = false;
				depthBias = 0;
			}
		};

		/** @brief The batch renderer's pass group for a technique (its hash map at +0x48, the groups at +8), or null. */
		std::byte* PassGroupOf(std::byte* a_batch, std::uint32_t a_technique)
		{
			auto* buckets = Field<std::byte*>(a_batch, 0x48);
			if (!buckets)
				return nullptr;
			const auto capacity = Field<std::uint32_t>(a_batch, 0x2C);
			auto* sentinel = Field<std::byte*>(a_batch, 0x38);
			auto* entry = buckets + std::size_t((capacity - 1u) & a_technique) * 0x10;
			if (!Field<std::byte*>(entry, 8))
				return nullptr;
			for (;;) {
				if (Field<std::uint32_t>(entry, 0) == a_technique) {
					auto* groups = Field<std::byte*>(a_batch, 8);
					return groups ? groups + std::size_t(Field<std::uint32_t>(entry, 4)) * 0x30 : nullptr;
				}
				entry = Field<std::byte*>(entry, 8);
				if (!entry || entry == sentinel)
					return nullptr;
			}
		}

		std::byte* BatchOf(RE::BSShaderAccumulator* a_accumulator)
		{
			return reinterpret_cast<std::byte*>(a_accumulator->GetRuntimeData()->batchRenderer);
		}

		/** @brief The engine's 4x4 from a transform, less a position adjustment (FUN_1414aaf10): rows are the rotation's columns. */
		std::array<float, 16> MatrixOf(const float* a_transform, const RE::NiPoint3& a_adjust)
		{
			const float* r = a_transform;  // NiMatrix3, row-major
			const float s = a_transform[12];
			return { s * r[0], s * r[3], s * r[6], 0.0f,
				s * r[1], s * r[4], s * r[7], 0.0f,
				s * r[2], s * r[5], s * r[8], 0.0f,
				a_transform[9] - a_adjust.x, a_transform[10] - a_adjust.y, a_transform[11] - a_adjust.z, 1.0f };
		}

		void Transpose(const std::array<float, 16>& a_in, float* a_out)
		{
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j)
					a_out[j * 4 + i] = a_in[i * 4 + j];
		}

		/** @brief D3DXMatrixMultiplyTranspose: transpose(a * b), b as the engine stores it. */
		void MultiplyTranspose(const std::array<float, 16>& a_a, const float* a_b, float* a_out)
		{
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j) {
					float sum = 0.0f;
					for (int k = 0; k < 4; ++k)
						sum += a_a[i * 4 + k] * a_b[k * 4 + j];
					a_out[j * 4 + i] = sum;
				}
		}

		/** @brief The D3D11 view of a NiSourceTexture's renderer texture (+0x48, then +0x10), or null. */
		ID3D11ShaderResourceView* ViewOf(const void* a_texture)
		{
			if (!a_texture)
				return nullptr;
			const auto* renderer = Field<const std::byte*>(a_texture, 0x48);
			return renderer ? Unwrap(Field<ID3D11ShaderResourceView*>(renderer, 0x10)) : nullptr;
		}

		/** @brief The vertex shader's VertexDesc for a raw technique: the engine keys its layout on the vanilla shader's. */
		std::uint64_t VertexDescOf(const RE::BSShader& a_shader, std::uint32_t a_technique)
		{
			for (const auto& entry : a_shader.vertexShaders)
				if (entry && entry->id == a_technique)
					return entry->vertexDesc;
			return 0;
		}

		struct Parity
		{
			std::uint64_t batches = 0, matched = 0, mismatched = 0, skipped = 0;
		};
		Parity s_parity;

		Family::RefusalLog& Differences()
		{
			static Family::RefusalLog log("[DCLF] sky native: differs from the capture");
			return log;
		}
	}

	void RegisterSwitch(Api::InstallContext& a_context)
	{
		const auto id = a_context.RegisterSwitch({ .name = "CS_DCLF_SKY_NATIVE", .kind = Api::SwitchKind::Feature, .reduced = Api::SwitchReduced::Never,
			.description = "0: the sky batch and cloud layers are drawn from their latched engine draws; 1: as 0, and their draws are also computed from the "
						   "engine's registered passes and compared with the latched ones (differences logged once each); 2 (default): they are drawn from the "
						   "computed draws, and the engine's calls draw nothing of them (dclf-native-sky.md)" });
		const auto& value = SwitchValue(id);
		s_mode = value == "0" ? Mode::Off : value == "1" ? Mode::Observe : Mode::Draw;
		if (s_mode != Mode::Off)
			logger::info("[DCLF] sky native: CS_DCLF_SKY_NATIVE={} ({})", value.empty() ? std::string_view("2, the default") : std::string_view(value),
				s_mode == Mode::Observe ? "computed and compared with the capture" : "drawn from the computed draws");
	}

	Mode GetMode() { return s_mode; }

	namespace
	{
		/** @brief What every pass of a batch or cloud group reads alike: the camera and the engine globals, at the call. */
		struct Frame
		{
			BatchInputs inputs;
			State* state = nullptr;
			RE::NiPoint3 posAdjust, previousPosAdjust, eye;
			std::array<float, 16> viewProj{};
			float vParams = 0.0f, skyScale = 0.0f, alphaRef = 0.0f;
			const float* cloudOffsets = nullptr;
			const float* skyColours = nullptr;
			Family::Viewport viewport;
			std::uint32_t reflection = 0;

			bool Read(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::string& a_why)
			{
				inputs = a_inputs;
				// The first-person accumulator adjusts the world by its camera (SetupGeometry's +0x128 branch). The sky is never drawn there.
				if (reinterpret_cast<const std::uint8_t*>(a_accumulator)[0x128]) {
					a_why = "a first-person accumulator";
					return false;
				}
				state = globals::state;
				if (!state) {
					a_why = "no CS state";
					return false;
				}
				auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				posAdjust = rss.posAdjust.getEye();
				previousPosAdjust = rss.previousPosAdjust.getEye();
				const auto camera = rss.cameraData.getEye();
				std::memcpy(viewProj.data(), &camera.viewProjMat, sizeof(viewProj));
				eye = a_accumulator->GetRuntimeData()->eyePosition;
				vParams = Global<float>(Address::kVParams);
				const auto* ism = RE::ImageSpaceManager::GetSingleton();
				skyScale = vParams * (ism ? Field<float>(ism, 0xE4) : 0.0f);
				cloudOffsets = reinterpret_cast<const float*>(REL::Offset(Address::kCloudOffsets).address());
				skyColours = reinterpret_cast<const float*>(REL::Offset(Address::kSkyColours).address());
				viewport = Family::LatchViewport(0);
				alphaRef = rss.alphaTestRef;
				reflection = a_inputs.view == View::Face ? static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsReflections) : 0u;
				return true;
			}
		};

		/**
		 * @brief A draw's program key and programs, by what makes them: the engine's state objects, the descriptor, the targets.
		 * Describing the states and finding the program take a map lookup each, and a frame's sky draws use a handful of keys. A
		 * program is cached once built, and the program cache never drops one, so an entry stays right.
		 */
		struct ProgramInputs
		{
			const void* blend = nullptr;
			const void* depthStencil = nullptr;
			const void* raster = nullptr;
			const RE::BSShader* shader = nullptr;
			std::uint32_t descriptor = 0;
			std::uint32_t variant = 0;
			std::array<std::uint32_t, 4> colourFormats{};
			std::uint32_t depthFormat = 0;
			std::uint32_t occlusion = 0;
			bool operator==(const ProgramInputs&) const = default;
		};

		struct CachedProgram
		{
			ProgramInputs inputs;
			Family::ProgramKey key;
			std::shared_ptr<const void> program, occlusionProgram;
		};

		std::vector<CachedProgram> s_programs;  // render thread

		/** @brief A cloud layer's Cloud Shadows part (TryLatchDraw's): its face occlusion draw, or its own layer's self shadow. */
		struct CloudPart
		{
			bool occlusion = false;
			std::int32_t layer = -1;
		};

		/**
		 * @brief One pass's draw, as SetupTechnique, SetupGeometry and the engine's state for it would make it. a_cloudLayers is the
		 * cloud group's last face layer when the pass is a cloud layer of the group, else null.
		 */
		bool BuildPass(const Frame& a_frame, RE::BSRenderPass* a_pass, std::uint32_t a_technique, const Modes& a_modes, std::int32_t* a_cloudLayers, Draw& a_draw,
			Family::Owners& a_owners, std::string& a_why)
		{
			auto* geometry = a_pass->geometry;
			auto* property = a_pass->shaderProperty;
			if (!geometry || !property || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
				a_why = "a pass that is not a BSSkyShader draw";
				return false;
			}
			if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
				a_why = "a sky pass that is not a TriShape";
				return false;
			}
			auto* state = a_frame.state;
			const auto& inputs = a_frame.inputs;
			const auto type = Field<std::uint32_t>(property, kSkyObjectType);
			// CS's per-pass sky bits (sun, moon, Secunda, cloud scattering), as its SetupGeometry hook sets them (Hooks.cpp), and
			// Cloud Shadows' layer for a cloud draw, as its SetupMaterial hook finds it (CloudShadows.cpp).
			state->UpdateSkyShaderPermutation(a_pass);
			if (globals::features::effects11.loaded)
				globals::features::effects11.ModifySky(a_pass);
			auto& cloudShadows = globals::features::cloudShadows;
			CloudPart cloud;
			if (a_cloudLayers && cloudShadows.loaded) {
				cloudShadows.ModifySky(a_pass);
				cloud.occlusion = inputs.view == View::Face && cloudShadows.overrideSky && cloudShadows.cloudShadowBlendState;
				cloud.layer = cloudShadows.currentLayerForDraw;
			}
			const bool selfShadow = a_cloudLayers && cloudShadows.loaded && inputs.view == View::Main && cloudShadows.bindLayerSelfShadow;
			// The flags are taken here, as a latched draw takes them: SkyShaderHacks never runs for this draw.
			cloudShadows.overrideSky = false;
			cloudShadows.bindLayerSelfShadow = false;

			std::uint32_t vertexDescriptor = a_technique;
			std::uint32_t pixelDescriptor = a_technique;
			state->ModifyShaderLookup(*a_pass->shader, vertexDescriptor, pixelDescriptor);
			a_draw.stencilRef = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData().stencilRef;
			auto* depthStencil = Pipeline::EngineDepthStencilState(a_modes.depth, a_modes.stencil);
			auto* blend = cloud.occlusion ? cloudShadows.cloudShadowBlendState :
			                                Pipeline::EngineBlendState(a_modes.blend, a_modes.alphaToCoverage, a_modes.write, a_modes.blendExtra);
			auto* raster = Pipeline::EngineRasterState(a_modes.fill, a_modes.cull, a_modes.depthBias, a_modes.scissor);
			if (!depthStencil) {
				a_why = "a state with no description";
				return false;
			}
			ProgramInputs programInputs{ .blend = blend, .depthStencil = depthStencil, .raster = raster, .shader = a_pass->shader, .descriptor = pixelDescriptor,
				.variant = inputs.view != View::Main ? kVariantFaces : 0u, .occlusion = cloud.occlusion ? 1u : 0u };
			if (inputs.view == View::Main) {
				std::copy(inputs.colourFormats.begin(), inputs.colourFormats.end(), programInputs.colourFormats.begin());
				programInputs.depthFormat = inputs.depthFormat;
			} else {
				programInputs.colourFormats = { inputs.colourFormats[0], cloud.occlusion ? static_cast<std::uint32_t>(rhi::helpers::ToRHI(DXGI_FORMAT_R8_UNORM)) : 0u, 0u,
					0u };
				programInputs.depthFormat = cloud.occlusion ? 0u : inputs.depthFormat;
			}
			const auto cached = std::ranges::find(s_programs, programInputs, &CachedProgram::inputs);
			if (cached != s_programs.end()) {
				a_draw.key = cached->key;
				a_draw.program = cached->program;
				a_draw.occlusionProgram = cached->occlusionProgram;
			} else {
				a_draw.key.descriptor = programInputs.descriptor;
				a_draw.key.defineStage = Family::DefineStage::Pixel;
				a_draw.key.variant = programInputs.variant;
				// Cloud Shadows binds no depth target for its occlusion draws (SkyShaderHacks).
				if (!Family::DescribeStates(a_draw.key, blend, cloud.occlusion ? nullptr : depthStencil, raster)) {
					a_why = "a state with no description";
					return false;
				}
				a_draw.key.colourFormats = programInputs.colourFormats;
				a_draw.key.depthFormat = programInputs.depthFormat;
				bool building = false;
				a_draw.program = SkyDraws::Program(a_draw.key, *a_pass->shader, &building);
				if (!a_draw.program) {
					a_why = building ? "a program still building" : "a program that cannot be built";
					return false;
				}
				if (cloud.occlusion) {
					auto occlusionKey = a_draw.key;
					occlusionKey.maskedTargets = 1;
					a_draw.occlusionProgram = SkyDraws::Program(occlusionKey, *a_pass->shader, &building);
					if (!a_draw.occlusionProgram) {
						a_why = building ? "an occlusion program still building" : "an occlusion program that cannot be built";
						return false;
					}
				}
				// Bounded: the states come from fixed engine tables, so only a changed target format makes new entries.
				if (s_programs.size() >= 256)
					s_programs.clear();
				s_programs.push_back({ programInputs, a_draw.key, a_draw.program, a_draw.occlusionProgram });
			}
			if (cloud.occlusion) {
				// Each face layer once, in engine order, so a later layer holds every earlier layer's occlusion.
				if (cloud.layer < 0 || cloud.layer >= static_cast<std::int32_t>(kCloudLayers) || cloud.layer <= *a_cloudLayers) {
					a_why = "a cloud layer out of order";
					return false;
				}
				a_draw.occlusion = true;
				a_draw.layer = cloud.layer;
				*a_cloudLayers = cloud.layer;
			}

			// Geometry: the TriShape's buffers and its whole index range, as the dispatcher draws it.
			const auto* shape = reinterpret_cast<const Family::TriShape*>(geometry->GetGeometryRuntimeData().rendererData);
			const auto triangles = static_cast<RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
			Family::Geometry latched;
			std::string detail;
			if (const auto result = Family::LatchGeometry(shape, 0, triangles, VertexDescOf(*a_pass->shader, a_technique), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
					latched, a_owners, &detail);
				result != Family::GeometryResult::Ok) {
				a_why = std::format("geometry: {} {}", Family::ReasonOf(result), detail);
				return false;
			}

			// SetupGeometry's constants.
			auto& c = a_draw.constants;
			const auto* world = reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(geometry) + kWorld);
			const auto* previous = reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(geometry) + kPreviousWorld);
			const auto m = MatrixOf(world, a_frame.posAdjust);
			MultiplyTranspose(m, a_frame.viewProj.data(), c.worldViewProj);
			Transpose(m, c.world);
			Transpose(MatrixOf(previous, a_frame.previousPosAdjust), c.previousWorld);
			c.eyePosition[0] = a_frame.eye.x - a_frame.posAdjust.x;
			c.eyePosition[1] = a_frame.eye.y - a_frame.posAdjust.y;
			c.eyePosition[2] = a_frame.eye.z - a_frame.posAdjust.z;
			c.vParams = a_frame.vParams;
			switch (type) {
			case 2:
				std::memcpy(c.blendColor, a_frame.skyColours, sizeof(c.blendColor));
				break;
			case 0:
			case 1:
			case 3:
			case 5:
			case 6:
			case 7:
				std::memcpy(c.blendColor, reinterpret_cast<const std::byte*>(property) + kBlendColor, 16);
				break;
			default:
				c.blendColor[3] = 1.0f;
				break;
			}
			if (type == 3) {
				const float blendValue = Field<float>(property, kBlendValue);
				c.pParams[0] = a_technique == 6 && !Field<bool>(property, kFadeSecond) ? 1.0f - blendValue : blendValue;
				std::memcpy(c.texCoordOff, a_frame.cloudOffsets + 2u * Field<std::uint16_t>(property, kCloudLayer), sizeof(c.texCoordOff));
			}
			// MoonAndStarsMask's pixel shader declares no PParams (the capture reads it as zero, and Sky.hlsl does not read it).
			c.pParams[1] = type == 6 || type == 1 || a_technique == 2 ? 0.0f : a_frame.skyScale;
			c.vertexRef = latched.vertexRef;
			c.indexRef = latched.indexRef;
			c.vertexLayout[0] = static_cast<std::uint32_t>(latched.layout);
			c.vertexLayout[1] = static_cast<std::uint32_t>(latched.layout >> 32);
			c.vertexStride = latched.stride;
			c.firstIndex = latched.firstIndex;
			a_draw.indexCount = latched.indexCount;

			// SetupGeometry's and SetupTechnique's textures.
			const void* base = Field<const void*>(property, kBaseTexture);
			const void* blendTexture = Field<const void*>(property, kBlendTexture);
			std::array<const void*, 3> textures{};
			switch (a_technique) {
			case 0:
			case 3:
				textures[0] = base ? base : Global<const void*>(Address::kDefaultTexture);
				break;
			case 6:
				textures[0] = Field<bool>(property, kFadeSecond) ? blendTexture : base;
				if (!textures[0])
					textures[0] = Global<const void*>(Address::kWhiteTexture);
				break;
			default:
				textures[0] = base ? base : Global<const void*>(Address::kWhiteTexture);
				if (a_technique == 5)
					textures[1] = blendTexture ? blendTexture : base;
				break;
			}
			textures[2] = Global<const void*>(Address::kNoiseTexture);
			const auto used = TexturesOf(a_technique);
			for (std::uint32_t t = 0; t < 3; ++t) {
				auto* view = used[t] ? ViewOf(textures[t]) : nullptr;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				if (Family::SlotTexture(view, GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), c.textures[t], a_owners, &failure) !=
					Family::SlotResult::Ok) {
					a_why = std::format("t{}: {}", t, GpuTextures::DescribeFailure(view, t, failure));
					return false;
				}
				// t0 and t1: WRAP, LINEAR (SetupGeometry); t2: WRAP, POINT (SetupTechnique).
				c.samplers[t] = !used[t] ? Family::SlotSampler(0, 0, a_owners) : Family::SlotSampler(3, t == 2 ? 0u : 1u, a_owners);
				if (c.samplers[t] == GpuTextures::kInvalid) {
					a_why = "a sampler with no graph sampler";
					return false;
				}
			}
			// Cloud self shadowing (t26): a main pass cloud layer reads its own layer's occlusion, anything else the occlusion cubemap.
			c.textures[3] = kSelfShadowFromOcclusion;
			if (selfShadow) {
				const auto layer = cloud.layer;
				auto* layerTexture = layer >= 0 && layer < CloudShadows::kMaxCloudLayers ? cloudShadows.texCloudShadowLayers[layer] : nullptr;
				if (!layerTexture || !layerTexture->srv) {
					a_why = "a cloud layer with no occlusion texture";
					return false;
				}
				if (const auto id = ORGCloudShadows::PublishedLayerId(layer); !id.empty()) {
					if (SkyDraws::ReadPublished(View::Main, id, c.textures[3]) != Family::InputResult::Graph) {
						a_why = std::format("a cloud layer whose occlusion '{}' its client cannot read", id);
						return false;
					}
				} else if (Family::SlotTexture(layerTexture->srv.get(), GpuTextures::ViewKind::Cube, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 26), c.textures[3],
							   a_owners) != Family::SlotResult::Ok) {
					a_why = "a cloud layer whose occlusion cannot be imported";
					return false;
				}
			}
			c.alphaTestRef = a_modes.alphaTest ? a_frame.alphaRef : 0.0f;
			c.extraShaderDescriptor = state->permutationData.ExtraShaderDescriptor | a_frame.reflection;
			c.physSky = inputs.physSky;
			a_draw.viewport = a_frame.viewport;
			return true;
		}
	}

	bool BuildBatch(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners,
		std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.Native.Build");
		a_draws.clear();
		// Each draw leases about eight owners (geometry, textures, samplers); a batch holds a dozen draws or so.
		a_draws.reserve(16);
		a_owners.reserve(a_owners.size() + 128);
		auto* batch = BatchOf(a_accumulator);
		if (!batch) {
			a_why = "the accumulator has no batch renderer";
			return false;
		}
		Frame frame;
		if (!frame.Read(a_accumulator, a_inputs, a_why))
			return false;
		Modes modes = Modes::Current();
		std::uint32_t current = UINT32_MAX;
		for (std::uint32_t passId = kFirstPass; passId <= kLastPass; ++passId) {
			auto* group = PassGroupOf(batch, passId);
			if (!group)
				continue;
			for (std::uint32_t sub = 0; sub < 5; ++sub) {
				auto* head = Field<RE::BSRenderPass*>(group, sub * 8);
				if (!head)
					continue;
				// BSBatchRenderer's group step: the cull, alpha test and alpha to coverage of the sub-group.
				if (!(a_inputs.renderFlags & 0x108))
					modes.cull = (sub == 2 || sub == 3) ? 0u : 1u;
				modes.alphaTest = sub == 1 || sub == 3 || sub == 4;
				modes.alphaToCoverage = 0;
				for (auto* pass = head; pass; pass = pass->passGroupNext) {
					const std::uint32_t technique = TechniqueOf(pass->passEnum);
					if (technique != current) {
						if (current != UINT32_MAX)
							modes.Restore();
						modes.Setup(technique);
						current = technique;
					}
					// SetupGeometry: the sun and the moons blend additively.
					if (pass->shaderProperty) {
						const auto type = Field<std::uint32_t>(pass->shaderProperty, kSkyObjectType);
						if (type == 0 || type == 6)
							modes.blend = 2;
					}
					if (!BuildPass(frame, pass, technique, modes, nullptr, a_draws.emplace_back(), a_owners, a_why))
						return false;
				}
			}
		}
		return true;
	}

	bool BuildClouds(RE::BSShaderAccumulator* a_accumulator, RE::BSBatchRenderer::GeometryGroup* a_group, const BatchInputs& a_inputs, std::int32_t& a_lastLayer,
		std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.Native.BuildClouds");
		a_draws.clear();
		a_draws.reserve(8);
		a_owners.reserve(a_owners.size() + 64);
		Frame frame;
		if (!frame.Read(a_accumulator, a_inputs, a_why))
			return false;
		// RenderPersistentPassList (AE 0x1414f19d0): each sky pass in list order, its cull from the property's two-sided flag and its
		// alpha test from the geometry's alpha property. The sky-object effects at the head stay the engine's (CloudGroupOf).
		Modes modes = Modes::Current();
		std::uint32_t current = UINT32_MAX;
		std::int32_t lastLayer = a_lastLayer;
		for (auto* pass = a_group->passList.head; pass; pass = pass->passGroupNext) {
			if (!pass->geometry || !pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky)
				continue;
			if (!(a_inputs.renderFlags & 0x108))
				modes.cull = pass->shaderProperty && (Field<std::uint64_t>(pass->shaderProperty, 0x38) & 0x1000000000ull) ? 0u : 1u;
			const auto* alpha = Field<const std::byte*>(pass->geometry, 0x120);
			modes.alphaTest = alpha && ((Field<std::uint16_t>(alpha, 0x30) >> 9) & 1u);
			const std::uint32_t technique = TechniqueOf(pass->passEnum);
			if (technique != current) {
				if (current != UINT32_MAX)
					modes.Restore();
				modes.Setup(technique);
				current = technique;
			}
			if (!BuildPass(frame, pass, technique, modes, &lastLayer, a_draws.emplace_back(), a_owners, a_why))
				return false;
		}
		a_lastLayer = lastLayer;
		return true;
	}

	void DetachClouds(RE::BSBatchRenderer::GeometryGroup* a_group)
	{
		// Only the sky-object effects stay linked, for the engine to draw; the engine's call then empties the list.
		RE::BSRenderPass* head = nullptr;
		RE::BSRenderPass* tail = nullptr;
		for (auto* pass = a_group->passList.head; pass;) {
			auto* next = pass->passGroupNext;
			if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
				pass->passGroupNext = nullptr;
				if (tail)
					tail->passGroupNext = pass;
				else
					head = pass;
				tail = pass;
			}
			pass = next;
		}
		a_group->passList.head = head;
		a_group->passList.tail = tail;
	}

	Detached DetachBatch(RE::BSShaderAccumulator* a_accumulator)
	{
		Detached detached;
		auto* batch = BatchOf(a_accumulator);
		if (!batch)
			return detached;
		detached.restore = !Field<bool>(batch, 0x6C);
		for (std::uint32_t passId = kFirstPass; passId <= kLastPass; ++passId) {
			auto* group = PassGroupOf(batch, passId);
			if (!group)
				continue;
			Detached::Entry entry;
			entry.group = group;
			auto* heads = reinterpret_cast<RE::BSRenderPass**>(group);
			for (std::uint32_t sub = 0; sub < 5; ++sub)
				entry.heads[sub] = std::exchange(heads[sub], nullptr);
			auto* bits = reinterpret_cast<std::uint32_t*>(group + 0x28);
			entry.bits = std::exchange(*bits, 0u);
			detached.entries.push_back(entry);
		}
		return detached;
	}

	void Reattach(Detached& a_detached)
	{
		if (!a_detached.restore)
			return;
		for (const auto& entry : a_detached.entries) {
			auto* group = static_cast<std::byte*>(entry.group);
			auto* heads = reinterpret_cast<RE::BSRenderPass**>(group);
			for (std::uint32_t sub = 0; sub < 5; ++sub)
				heads[sub] = entry.heads[sub];
			*reinterpret_cast<std::uint32_t*>(group + 0x28) = entry.bits;
		}
		a_detached.entries.clear();
	}

	void Compare(View a_view, std::span<const Draw> a_native, std::span<const Draw> a_captured)
	{
		++s_parity.batches;
		const char* view = a_view == View::Main ? "main" : "face";
		if (a_native.size() != a_captured.size()) {
			++s_parity.mismatched;
			Differences().Report(std::format("{} batch draw count", view), std::format("native {}, captured {}", a_native.size(), a_captured.size()));
			return;
		}
		for (std::size_t i = 0; i < a_native.size(); ++i) {
			const auto& n = a_native[i];
			const auto& c = a_captured[i];
			const std::uint32_t technique = c.key.descriptor & 0xFF;
			std::string what;
			const auto note = [&](std::string_view a_field, std::string a_detail) {
				if (what.empty())
					Differences().Report(std::format("{} technique {}: {}", view, technique, a_field), std::move(a_detail));
				what = a_field;
			};
			if (n.key.descriptor != c.key.descriptor)
				note("descriptor", std::format("native {:#x}, captured {:#x}", n.key.descriptor, c.key.descriptor));
			if (n.key.blend != c.key.blend)
				note("blend state", "");
			if (n.key.depthStencil != c.key.depthStencil || n.key.hasDepth != c.key.hasDepth)
				note("depth-stencil state", "");
			if (n.key.raster != c.key.raster)
				note("raster state", std::format("cull words native {} captured {}", n.key.raster[1], c.key.raster[1]));
			if (n.key.colourFormats != c.key.colourFormats || n.key.depthFormat != c.key.depthFormat || n.key.variant != c.key.variant)
				note("targets or variant", "");
			if (n.indexCount != c.indexCount || std::memcmp(&n.constants.vertexRef, &c.constants.vertexRef, 32) != 0)
				note("geometry", std::format("indices native {}, captured {}", n.indexCount, c.indexCount));
			if (n.stencilRef != c.stencilRef)
				note("stencil reference", "");
			if (n.viewport.rect != c.viewport.rect || n.viewport.depthRange != c.viewport.depthRange)
				note("viewport", "");
			// The floats: matrices, eye, VParams, BlendColor; TexCoordOff and PParams.x only for clouds (others leave them unwritten).
			const auto floats = [&](const float* a_n, const float* a_c, std::size_t a_count, std::string_view a_name) {
				for (std::size_t f = 0; f < a_count; ++f)
					if (std::abs(a_n[f] - a_c[f]) > 1e-4f * (std::max)(1.0f, std::abs(a_c[f]))) {
						note(a_name, std::format("[{}] native {}, captured {}", f, a_n[f], a_c[f]));
						return;
					}
			};
			floats(n.constants.worldViewProj, c.constants.worldViewProj, 16, "WorldViewProj");
			floats(n.constants.world, c.constants.world, 16, "World");
			floats(n.constants.previousWorld, c.constants.previousWorld, 16, "PreviousWorld");
			floats(n.constants.eyePosition, c.constants.eyePosition, 3, "EyePosition");
			floats(&n.constants.vParams, &c.constants.vParams, 1, "VParams");
			floats(n.constants.blendColor, c.constants.blendColor, 12, "BlendColor");
			const bool cloud = technique >= 4 && technique <= 6;
			if (cloud) {
				floats(n.constants.texCoordOff, c.constants.texCoordOff, 2, "TexCoordOff");
				floats(n.constants.pParams, c.constants.pParams, 1, "PParams.x");
			}
			floats(&n.constants.pParams[1], &c.constants.pParams[1], 1, "PParams.y");
			floats(&n.constants.alphaTestRef, &c.constants.alphaTestRef, 1, "alpha test reference");
			// IsAurora leaks into the capture's sky draws from a sky-object effect drawn before them (UpdateEffectShaderPermutation sets it,
			// UpdateSkyShaderPermutation never clears it); Sky.hlsl does not read it.
			constexpr auto aurora = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsAurora);
			if ((n.constants.extraShaderDescriptor & ~aurora) != (c.constants.extraShaderDescriptor & ~aurora))
				note("ExtraShaderDescriptor", std::format("native {:#x}, captured {:#x}", n.constants.extraShaderDescriptor, c.constants.extraShaderDescriptor));
			for (std::uint32_t t = 0; t < 4; ++t)
				if (n.constants.textures[t] != c.constants.textures[t])
					note(std::format("t{}", t == 3 ? 26u : t), std::format("native {}, captured {}", n.constants.textures[t], c.constants.textures[t]));
			for (std::uint32_t s = 0; s < 3; ++s)
				if (n.constants.samplers[s] != c.constants.samplers[s])
					note(std::format("s{}", s), std::format("native {}, captured {}", n.constants.samplers[s], c.constants.samplers[s]));
			if (what.empty())
				++s_parity.matched;
			else
				++s_parity.mismatched;
		}
	}

	std::string Status()
	{
		if (s_mode != Mode::Observe)
			return {};
		return std::format("native parity: {} batches, {} draws match, {} differ, {} not compared", s_parity.batches, s_parity.matched, s_parity.mismatched, s_parity.skipped);
	}
}
