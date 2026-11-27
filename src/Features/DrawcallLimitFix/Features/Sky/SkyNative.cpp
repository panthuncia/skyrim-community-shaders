// The sky batch's draws: from the engine's registered passes, or, for the main view in native and for native faces, from the sky root.
// Render thread.
// The engine functions this follows (AE 1.6.1170): BSSkyShaderProperty::GetRenderPasses 0x1414e6fd0, RenderBatches 0x1414b4fe0 and
// its group step 0x1414f39c0, BSSkyShader::SetupTechnique 0x1414e5d00 and SetupGeometry 0x1414e6030, the transform 0x1414aaf10.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/ImageSpace/ImageSpaceBlock.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"

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
#include <cstring>
#include <format>

namespace DCLF::Features::Sky::Native
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

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
			inline constexpr Engine::Offset kSkyShader{ { 415143 }, { 528198 } };       // BSSkyShader*: the sun query's pass shader
		}

		// The pass id the sun's occlusion query draws with (FUN_1414b4c00's MakeRenderPass).
		constexpr std::uint32_t kSunOcclusionPass = 2;
		// FinishAccumulating's alpha reference for the sky batch (AE 0x1414b2d90: 0.5019608).
		constexpr float kFaceAlphaRef = 128.0f / 255.0f;
		// RenderBatches' flags at a face's sky batch: SubmitAccumulator's 8 (BSCubeMapCamera::RenderFaces, AE 0x1414ed920).
		constexpr std::uint32_t kFaceRenderFlags = 8;
		// The shadow state's stencil reference at a face's sky batch.
		constexpr std::uint32_t kFaceStencilRef = 255;

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

		/**
		 * @brief The ImageSpace data's sky brightness (ImageSpaceManager +0xE4), PParams.y's other factor. Native: the update's ImageSpace
		 * block; guest reads the manager.
		 */
		float SkyBrightness()
		{
			if (::RenderGraph::Engine::Native::Active())
				if (const auto& snapshot = Snapshot::Current())
					if (const auto* block = snapshot->Block<::RenderGraph::Engine::ImageSpace::Block>(); block && block->inputs.read)
						return block->inputs.At(0xE4);
			const auto* ism = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
			return ism ? Field<float>(ism, 0xE4) : 0.0f;
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
				Modes m;
				// Native's RenderWorld points draw with CS's frame's state.
				if (const auto* world = ::RenderGraph::Engine::Native::WorldPass::Current()) {
					m.depth = world->depthMode;
					m.stencil = world->stencil;
					m.fill = world->fill;
					m.cull = world->cull;
					m.depthBias = world->depthBias;
					m.scissor = world->scissor;
					m.blend = world->blend;
					m.alphaToCoverage = world->alphaToCoverage;
					m.write = world->write;
					m.blendExtra = world->blendExtra;
					m.alphaTest = world->alphaTest;
					return m;
				}
				auto& rss = CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
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

			/** @brief BSSkyShader::SetupTechnique (engine/sky.md). */
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

			/**
			 * @brief The shadow state at a face's sky batch, as the engine's face render leaves it in its steady state (perf-reflections.md):
			 * stencil mode 2 and cull mode 2 from the face render, the alpha test from FinishAccumulating. The face's render flags (8)
			 * keep the cull mode for every pass.
			 */
			static Modes FaceBatch()
			{
				Modes m;
				m.depth = 3;
				m.stencil = 2;
				m.cull = 2;
				m.write = 1;
				m.alphaTest = true;
				return m;
			}

			/** @brief The shadow state at a face's cloud group: the batch's last technique restored, then write mode 11. */
			static Modes FaceClouds()
			{
				Modes m = FaceBatch();
				m.Restore();
				m.write = 11;
				return m;
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

		/** @brief Draws left out while their program builds, each logged once by view and technique. */
		Family::RefusalLog& LeftOut()
		{
			static Family::RefusalLog log("[DCLF] sky: a draw is left out until its program is built");
			return log;
		}

		std::uint64_t s_builds = 0;  // render thread: one per batch, cloud group, sun or face build

		/** @brief What every pass of a batch or cloud group reads alike: the camera and the engine globals, at the call. */
		struct Frame
		{
			std::uint64_t build = ++s_builds;  // which build this is, so a slot entry's owners join its run once
			BatchInputs inputs;
			State* state = nullptr;
			RE::NiPoint3 posAdjust, previousPosAdjust, eye;
			std::array<float, 16> viewProj{};
			float vParams = 0.0f, skyScale = 0.0f, alphaRef = 0.0f;
			const float* cloudOffsets = nullptr;
			const float* skyColours = nullptr;
			Family::Viewport viewport;
			std::uint32_t reflection = 0;
			std::uint32_t stencilRef = 0;

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
				// Native's RenderWorld points read nothing of the shadow state.
				const auto* world = ::RenderGraph::Engine::Native::WorldPass::Current();
				const bool frameCamera = Snapshot::FrameCamera::Owned() && a_inputs.view != View::Face;
				auto* const rss = world && frameCamera ? nullptr : &CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
				if (frameCamera) {
					// Native: the frame camera, with the render side's last drawn camera as the previous one. The sun's query is unjittered.
					const auto& frame = (a_inputs.view == View::Sun ? Snapshot::FrameCamera::Post() : Snapshot::FrameCamera::Main()).data;
					posAdjust = { frame.CameraPosAdjust.x, frame.CameraPosAdjust.y, frame.CameraPosAdjust.z };
					previousPosAdjust = { frame.CameraPreviousPosAdjust.x, frame.CameraPreviousPosAdjust.y, frame.CameraPreviousPosAdjust.z };
					viewProj = Snapshot::FrameCamera::RowMajor(&frame.CameraViewProj);
				} else {
					posAdjust = rss->posAdjust.getEye();
					previousPosAdjust = rss->previousPosAdjust.getEye();
					const auto camera = rss->cameraData.getEye();
					std::memcpy(viewProj.data(), &camera.viewProjMat, sizeof(viewProj));
				}
				// StartAccumulating gives the accumulator the world camera's position, which is the frame camera's eye at native's points.
				eye = world && frameCamera ? posAdjust : a_accumulator->GetRuntimeData()->eyePosition;
				vParams = Global<float>(Address::kVParams);
				skyScale = vParams * SkyBrightness();
				cloudOffsets = reinterpret_cast<const float*>(REL::Offset(Address::kCloudOffsets).address());
				skyColours = reinterpret_cast<const float*>(REL::Offset(Address::kSkyColours).address());
				viewport = Family::LatchViewport(0);
				// Native's RenderWorld points take the references from CS's frame.
				if (!rss) {
					alphaRef = world->alphaRef;
					stencilRef = world->stencilRef;
				} else {
					alphaRef = rss->alphaTestRef;
					stencilRef = rss->stencilRef;
				}
				reflection = a_inputs.view == View::Face ? static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsReflections) : 0u;
				return true;
			}

			/**
			 * @brief A face drawn without the engine's face render: what the engine's face would hold at its sky batch. The camera data
			 * and posAdjust are the face camera's (SetCameraData), the eye is the accumulator's (the camera position), the alpha
			 * reference is FinishAccumulating's 128/255, and the viewport is the face's.
			 */
			bool ReadFace(const FaceView& a_view, const BatchInputs& a_inputs, std::string& a_why)
			{
				inputs = a_inputs;
				state = globals::state;
				if (!state) {
					a_why = "no CS state";
					return false;
				}
				// SetCameraData leaves a face's previous posAdjust zero.
				posAdjust = eye = a_view.eye;
				previousPosAdjust = {};
				viewProj = a_view.viewProj;
				vParams = Global<float>(Address::kVParams);
				skyScale = vParams * SkyBrightness();
				cloudOffsets = reinterpret_cast<const float*>(REL::Offset(Address::kCloudOffsets).address());
				skyColours = reinterpret_cast<const float*>(REL::Offset(Address::kSkyColours).address());
				viewport = Family::LatchViewport(0);
				viewport.rect = a_view.rect;
				alphaRef = kFaceAlphaRef;
				stencilRef = kFaceStencilRef;
				reflection = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsReflections);
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
		 * @brief A draw's t0-t2 and s0-s2, kept for the frame by the views it samples: the faces and the main batch draw the same sky
		 * objects, so most draws find theirs.
		 */
		struct Slots
		{
			std::array<ID3D11ShaderResourceView*, 3> views{};
			std::array<bool, 3> used{};
			std::array<std::uint32_t, 3> textures{}, samplers{};
			Family::Owners owners;
			std::uint64_t build = 0;  // the last build whose run holds the owners
		};

		struct SlotCache
		{
			std::uint32_t frame = UINT32_MAX;
			std::vector<Slots> entries;
		};
		SlotCache s_slots;  // render thread

		/** @brief The slots for a_views, resolved on the frame's first use. Null, with a_why, when one cannot be read. */
		Slots* SlotsOf(const std::array<ID3D11ShaderResourceView*, 3>& a_views, const std::array<bool, 3>& a_used, std::string& a_why)
		{
			if (const auto frame = Family::RenderFrame(); s_slots.frame != frame) {
				s_slots.frame = frame;
				s_slots.entries.clear();
			}
			for (auto& entry : s_slots.entries)
				if (entry.views == a_views && entry.used == a_used)
					return &entry;
			Slots slots{ .views = a_views, .used = a_used };
			for (std::uint32_t t = 0; t < 3; ++t) {
				auto* view = a_views[t];
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				if (Family::SlotTexture(view, GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), slots.textures[t], slots.owners, &failure) !=
					Family::SlotResult::Ok) {
					a_why = std::format("t{}: {}", t, GpuTextures::DescribeFailure(view, t, failure));
					return nullptr;
				}
				// t0 and t1: WRAP, LINEAR (SetupGeometry); t2: WRAP, POINT (SetupTechnique).
				slots.samplers[t] = !a_used[t] ? Family::SlotSampler(0, 0, slots.owners) : Family::SlotSampler(3, t == 2 ? 0u : 1u, slots.owners);
				if (slots.samplers[t] == GpuTextures::kInvalid) {
					a_why = "a sampler with no graph sampler";
					return nullptr;
				}
			}
			return &s_slots.entries.emplace_back(std::move(slots));
		}

		/**
		 * @brief One pass's draw, as SetupTechnique, SetupGeometry and the engine's state for it would make it. a_cloudLayers is the
		 * cloud group's last face layer when the pass is a cloud layer of the group, else null.
		 */
		PassResult BuildPass(const Frame& a_frame, RE::BSRenderPass* a_pass, std::uint32_t a_technique, const Modes& a_modes, std::int32_t* a_cloudLayers, Draw& a_draw,
			Family::Owners& a_owners, std::string& a_why)
		{
			CS_ENGINE_READ_MARK(Scene, RecordTime);
			auto* geometry = a_pass->geometry;
			auto* property = a_pass->shaderProperty;
			if (!geometry || !property || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
				a_why = "a pass that is not a BSSkyShader draw";
				return PassResult::Failed;
			}
			if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
				a_why = "a sky pass that is not a TriShape";
				return PassResult::Failed;
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
				cloudShadows.ModifySky(a_pass, inputs.view == View::Face);
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
			a_draw.stencilRef = a_frame.stencilRef;
			auto* depthStencil = Pipeline::EngineDepthStencilState(a_modes.depth, a_modes.stencil);
			auto* blend = cloud.occlusion ? cloudShadows.cloudShadowBlendState :
			                                Pipeline::EngineBlendState(a_modes.blend, a_modes.alphaToCoverage, a_modes.write, a_modes.blendExtra);
			auto* raster = Pipeline::EngineRasterState(a_modes.fill, a_modes.cull, a_modes.depthBias, a_modes.scissor);
			if (!depthStencil) {
				a_why = "a state with no description";
				return PassResult::Failed;
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
					return PassResult::Failed;
				}
				a_draw.key.colourFormats = programInputs.colourFormats;
				a_draw.key.depthFormat = programInputs.depthFormat;
				bool building = false;
				a_draw.program = SkyDraws::Program(a_draw.key, *a_pass->shader, &building);
				if (!a_draw.program) {
					a_why = "a program that cannot be built";
					return building ? PassResult::LeftOut : PassResult::Failed;
				}
				if (cloud.occlusion) {
					auto occlusionKey = a_draw.key;
					occlusionKey.maskedTargets = 1;
					a_draw.occlusionProgram = SkyDraws::Program(occlusionKey, *a_pass->shader, &building);
					if (!a_draw.occlusionProgram) {
						a_why = "an occlusion program that cannot be built";
						return building ? PassResult::LeftOut : PassResult::Failed;
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
					return PassResult::Failed;
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
				return PassResult::Failed;
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
			// The MoonAndStarsMask and SunOcclude pixel shaders declare no PParams (the capture reads it as zero, and Sky.hlsl does not read it).
			c.pParams[1] = type == 6 || type == 1 || a_technique == 2 || a_technique == 0 ? 0.0f : a_frame.skyScale;
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
			std::array<ID3D11ShaderResourceView*, 3> views{};
			for (std::uint32_t t = 0; t < 3; ++t)
				views[t] = used[t] ? ViewOf(textures[t]) : nullptr;
			auto* slots = SlotsOf(views, used, a_why);
			if (!slots)
				return PassResult::Failed;
			std::copy(slots->textures.begin(), slots->textures.end(), c.textures);
			std::copy(slots->samplers.begin(), slots->samplers.end(), c.samplers);
			if (std::exchange(slots->build, a_frame.build) != a_frame.build)
				a_owners.insert(a_owners.end(), slots->owners.begin(), slots->owners.end());
			// Cloud self shadowing (t26): a main pass cloud layer reads its own layer's occlusion, anything else the occlusion cubemap.
			c.textures[3] = kSelfShadowFromOcclusion;
			if (selfShadow) {
				const auto layer = cloud.layer;
				auto* layerTexture = layer >= 0 && layer < CloudShadows::kMaxCloudLayers ? cloudShadows.texCloudShadowLayers[layer] : nullptr;
				if (!layerTexture || !layerTexture->srv) {
					a_why = "a cloud layer with no occlusion texture";
					return PassResult::Failed;
				}
				if (const auto id = ORGCloudShadows::PublishedLayerId(layer); !id.empty()) {
					if (SkyDraws::ReadPublished(View::Main, id, c.textures[3]) != Family::InputResult::Graph) {
						a_why = std::format("a cloud layer whose occlusion '{}' its client cannot read", id);
						return PassResult::Failed;
					}
				} else if (Family::SlotTexture(layerTexture->srv.get(), GpuTextures::ViewKind::Cube, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 26), c.textures[3],
							   a_owners) != Family::SlotResult::Ok) {
					a_why = "a cloud layer whose occlusion cannot be imported";
					return PassResult::Failed;
				}
			}
			c.alphaTestRef = a_modes.alphaTest ? a_frame.alphaRef : 0.0f;
			c.extraShaderDescriptor = state->permutationData.ExtraShaderDescriptor | a_frame.reflection;
			c.physSky = inputs.physSky;
			a_draw.viewport = a_frame.viewport;
			return PassResult::Drawn;
		}

		/**
		 * @brief Takes a pass's result for the draw it built last: a draw whose program still builds is dropped, so the batch stays the
		 * graph's (handed to the engine, a face's cloud layer would reach Cloud Shadows' D3D11 path). False when it failed.
		 */
		bool Take(PassResult a_result, std::vector<Draw>& a_draws, View a_view, std::uint32_t a_technique)
		{
			if (a_result == PassResult::LeftOut) {
				a_draws.pop_back();
				const char* view = a_view == View::Main ? "main" : a_view == View::Face ? "face" :
				                                                                          "sun";
				LeftOut().Report(std::format("a {} draw of technique {}", view, a_technique));
			}
			return a_result != PassResult::Failed;
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
					if (!Take(BuildPass(frame, pass, technique, modes, nullptr, a_draws.emplace_back(), a_owners, a_why), a_draws, a_inputs.view, technique))
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
			if (!Take(BuildPass(frame, pass, technique, modes, &lastLayer, a_draws.emplace_back(), a_owners, a_why), a_draws, a_inputs.view, technique))
				return false;
		}
		a_lastLayer = lastLayer;
		return true;
	}

	PassResult BuildSun(RE::BSShaderAccumulator* a_accumulator, RE::BSGeometry* a_geometry, const BatchInputs& a_inputs, Draw& a_draw, Family::Owners& a_owners,
		std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.Native.BuildSun");
		Frame frame;
		if (!frame.Read(a_accumulator, a_inputs, a_why))
			return PassResult::Failed;
		// SetupGeometry reads the eye from the shader manager's current accumulator, which after the world's render is the last one
		// submitted (the screen splatter's), not the query's. Native's RenderWorld submits none: the main accumulator is current, and its
		// eye is the world camera's position, the frame camera's eye.
		if (::RenderGraph::Engine::Native::WorldPass::InRenderWorld()) {
			const auto& camera = Snapshot::FrameCamera::Main().data;
			frame.eye = { camera.CameraPosAdjust.x, camera.CameraPosAdjust.y, camera.CameraPosAdjust.z };
		} else {
			const auto* current = reinterpret_cast<RE::BSShaderAccumulator*>(*CS_ENGINE_READ(Accumulator, RecordTime, globals::game::currentAccumulator).get());
			frame.eye = current ? current->GetRuntimeData()->eyePosition : RE::NiPoint3{};
		}
		// The pass the engine's call makes (BSShader::MakeRenderPass with pass id 2), without its pool entry. The property is the
		// geometry's shader property (+0x128).
		RE::BSRenderPass pass{};
		pass.shader = Global<RE::BSShader*>(Address::kSkyShader);
		pass.shaderProperty = Field<RE::BSShaderProperty*>(a_geometry, 0x128);
		pass.geometry = a_geometry;
		pass.passEnum = kSunOcclusionPass;
		// RenderPassImmediately(pass, 2, no alpha test): SetupTechnique has no case for pass id 2, so it is raw technique 0 (SunOcclude).
		Modes modes = Modes::Current();
		modes.alphaTest = false;
		modes.Setup(0);
		// SetupGeometry: the sun blends additively.
		modes.blend = 2;
		return BuildPass(frame, &pass, 0, modes, nullptr, a_draw, a_owners, a_why);
	}

	namespace
	{
		/** @brief A face's sky pass: what the engine's face cull would register for a sky object. */
		struct FacePass
		{
			RE::BSGeometry* geometry = nullptr;
			RE::BSShaderProperty* property = nullptr;
			std::uint32_t passId = 0;
			std::uint32_t sub = 0;  // the batch sub-group (FindTechniqueGroup), or the cloud list
		};

		/** @brief GetRenderPasses' pass id for a sky object type (AE 0x1414e6fd0), or 0 for the sun glare, which no face draws. */
		std::uint32_t PassIdOf(const RE::BSShaderProperty* a_property, std::uint32_t a_type)
		{
			switch (a_type) {
			case 1:
				return 0;
			case 3:
				if (Field<float>(a_property, kBlendValue) <= 0.0f)
					return 0x5C000062;
				// CloudsFade when the base or the blend texture is the null texture, else CloudsLerp.
				return Field<const void*>(a_property, kBaseTexture) == Global<const void*>(Address::kNullTexture) ||
				               Field<const void*>(a_property, kBlendTexture) == Global<const void*>(Address::kNullTexture) ?
				           0x5C000064u :
				           0x5C000063u;
			case 5:
				return 0x5C000060;
			case 7:
				return 0x5C00005F;
			case 2:
			case 4:
				return 0x5C00005E;
			default:
				return 0x5C000061;
			}
		}

		bool IsCloudPass(std::uint32_t a_passId)
		{
			const auto technique = TechniqueOf(a_passId);
			return technique >= 4 && technique <= 6;
		}

		/** @brief FindTechniqueGroup's sub-group (SE 0x141308bb0): 4 for property flag bit 54, else two-sided (bit 36) and alpha test. */
		std::uint32_t SubGroupOf(const RE::BSGeometry* a_geometry, const RE::BSShaderProperty* a_property)
		{
			const auto flags = Field<std::uint64_t>(a_property, 0x38);
			if (flags & (1ull << 54))
				return 4;
			const auto* alpha = Field<const std::byte*>(a_geometry, 0x120);
			const std::uint32_t alphaTest = alpha ? (Field<std::uint16_t>(alpha, 0x30) >> 9) & 1u : 0u;
			return static_cast<std::uint32_t>((flags >> 35) & 2u) | alphaTest;
		}

		/** @brief Whether a bound lies wholly outside a plane of the set (NiCullingProcess's test: n.c - d < -r). */
		bool Outside(const RE::NiBound& a_bound, const ShadowCameras::Planes& a_planes)
		{
			for (std::uint32_t p = 0; p < 6; ++p) {
				if (!(a_planes.mask & (1u << p)))
					continue;
				const auto& plane = a_planes.plane[p];
				const float distance = plane[0] * a_bound.center.x + plane[1] * a_bound.center.y + plane[2] * a_bound.center.z - plane[3];
				if (distance < -a_bound.radius)
					return true;
			}
			return false;
		}

		/** @brief One object under the sky root as the game half took it, in the cull's pre-order. Hidden objects are left out with their subtrees. */
		struct RootObject
		{
			enum class Kind : std::uint8_t
			{
				Node,    // its children follow, up to end
				Pass,    // a sky object with a pass
				Effect,  // a geometry with another shader (a sky-object effect)
				None     // a geometry with no property or no pass
			};
			Kind kind = Kind::None;
			bool always = false;  // kAlwaysDraw: skips its own bound test
			std::uint32_t end = 0;  // the index past its subtree
			RE::NiBound bound;
			RE::BSGeometry* geometry = nullptr;  // Pass: the draw's geometry and property, read when its draw is built
			RE::BSShaderProperty* property = nullptr;
			std::uint32_t passId = 0;
			std::uint32_t sub = 0;
			std::string name;  // Effect: the geometry's name, for the refusal
		};

		/** @brief The sky root's objects, taken once per game update (TicketPart::SkyRoot). Empty without a sky root. */
		struct RootObjects
		{
			bool root = false;
			std::vector<RootObject> objects;
		};

		void Flatten(RE::NiAVObject* a_object, std::vector<RootObject>& a_out)
		{
			if (!a_object)
				return;
			const auto flags = a_object->GetFlags();
			if (flags.all(RE::NiAVObject::Flag::kHidden))
				return;
			const auto index = a_out.size();
			{
				auto& entry = a_out.emplace_back();
				entry.always = flags.all(RE::NiAVObject::Flag::kAlwaysDraw);
				entry.bound = a_object->worldBound;
			}
			if (auto* node = a_object->AsNode()) {
				a_out[index].kind = RootObject::Kind::Node;
				for (auto& child : node->GetChildren())
					Flatten(child.get(), a_out);
				a_out[index].end = static_cast<std::uint32_t>(a_out.size());
				return;
			}
			auto& entry = a_out[index];
			entry.end = static_cast<std::uint32_t>(index + 1);
			auto* geometry = a_object->AsGeometry();
			auto* property = geometry ? geometry->GetGeometryRuntimeData().shaderProperty.get() : nullptr;
			if (!property)
				return;
			if (!netimmerse_cast<RE::BSSkyShaderProperty*>(property)) {
				entry.kind = RootObject::Kind::Effect;
				entry.name = geometry->name.c_str();
				return;
			}
			const auto passId = PassIdOf(property, Field<std::uint32_t>(property, kSkyObjectType));
			if (!passId)
				return;
			entry.kind = RootObject::Kind::Pass;
			entry.geometry = geometry;
			entry.property = property;
			entry.passId = passId;
			entry.sub = IsCloudPass(passId) ? 0u : SubGroupOf(geometry, property);
		}

		/**
		 * @brief The game half's capture (TicketPart::SkyRoot): the sky root flattened, with each object's bound and pass. Both modes:
		 * the reflection faces cull it in guest too.
		 */
		std::shared_ptr<const void> CaptureRootObjects()
		{
			auto out = std::make_shared<RootObjects>();
			auto* sky = CS_ENGINE_READ(Sky, CapturePoint, globals::game::sky);
			if (!sky || !sky->root)
				return out;
			CS_ENGINE_READ_MARK(Scene, CapturePoint);
			out->root = true;
			Flatten(sky->root.get(), out->objects);
			return out;
		}

		/** @brief What the cull of the sky root registers for a face or the main camera, in registration order. */
		struct FaceCull
		{
			const ShadowCameras::Planes* planes = nullptr;
			const std::vector<RootObject>* objects = nullptr;
			std::vector<FacePass> passes;
			std::string why;
			// The main camera's cull leaves sky-object effects out (they are not the sky family's); a face fails on them.
			bool skipEffects = false;
			// The main camera's cull registers every sky object that is not hidden, whatever its bound (parity: the engine's main
			// accumulator held exactly these). A face tests each object's bound.
			bool noBounds = false;

			/** @brief An always-drawn object skips its own bound test; its children test theirs (as the engine's cull does). */
			void Visit(std::uint32_t a_index, bool a_always)
			{
				if (a_index >= objects->size() || !why.empty())
					return;
				const auto& object = (*objects)[a_index];
				const bool always = a_always || object.always;
				if (!always && !noBounds && Outside(object.bound, *planes))
					return;
				switch (object.kind) {
				case RootObject::Kind::Node:
					for (auto child = a_index + 1; child < object.end; child = (*objects)[child].end)
						Visit(child, false);
					break;
				case RootObject::Kind::Effect:
					if (!skipEffects)
						why = std::format("a sky-object effect is in view ({})", object.name);
					break;
				case RootObject::Kind::Pass:
					passes.push_back({ object.geometry, object.property, object.passId, object.sub });
					break;
				case RootObject::Kind::None:
					break;
				}
			}
		};

		/** @brief The face's draw order: the batch by technique, sub-group and newest first (push-front), then the clouds as registered. */
		void Order(const std::vector<FacePass>& a_registered, std::vector<FacePass>& a_batch, std::vector<FacePass>& a_clouds)
		{
			a_batch.clear();
			a_clouds.clear();
			for (const auto& pass : a_registered)
				if (IsCloudPass(pass.passId))
					a_clouds.push_back(pass);
			for (std::uint32_t passId = kFirstPass; passId <= kLastPass; ++passId) {
				if (IsCloudPass(passId))
					continue;
				for (std::uint32_t sub = 0; sub < 5; ++sub)
					for (auto it = a_registered.rbegin(); it != a_registered.rend(); ++it)
						if (it->passId == passId && it->sub == sub)
							a_batch.push_back(*it);
			}
		}

		/** @brief The pass the engine's GetRenderPasses holds for a sky object, without its pool entry. */
		RE::BSRenderPass PassOf(const FacePass& a_pass)
		{
			RE::BSRenderPass pass{};
			pass.shader = Global<RE::BSShader*>(Address::kSkyShader);
			pass.shaderProperty = a_pass.property;
			pass.geometry = a_pass.geometry;
			pass.passEnum = a_pass.passId;
			return pass;
		}

		/**
		 * @brief The passes the engine's face cull of the sky root would register, in the engine's draw order: the sky batch by
		 * technique, sub-group and newest first, then the cloud layers (geometry group 13) in registration order. False, with a_why,
		 * when the face shows something the native face cannot draw (a sky-object effect).
		 */
		bool RootPasses(const RE::NiFrustum& a_frustum, const RE::NiTransform& a_world, bool a_face, std::vector<FacePass>& a_batch,
			std::vector<FacePass>& a_clouds, std::string& a_why)
		{
			DCLF_ZONE("CS.DCLF.Sky.Native.RootPasses");
			// The game half took the sky root into the frame ticket.
			const auto* root = static_cast<const RootObjects*>(TicketPartOf(TicketPart::SkyRoot));
			if (!root || !root->root || root->objects.empty()) {
				a_why = root ? "no sky root" : "the frame's ticket holds no sky root";
				return false;
			}
			ShadowCameras::Planes planes;
			ShadowCameras::FrustumPlanes(a_frustum, a_world, planes);
			// Render thread: the registration list is kept between calls.
			static FaceCull cull;
			cull.planes = &planes;
			cull.objects = &root->objects;
			cull.passes.clear();
			cull.why.clear();
			cull.skipEffects = !a_face;
			cull.noBounds = !a_face;
			// TESWaterReflections::Update holds the sky root always drawn while it renders the faces (AE 0x140520570).
			cull.Visit(0, a_face);
			if (!cull.why.empty()) {
				a_why = cull.why;
				return false;
			}
			Order(cull.passes, a_batch, a_clouds);
			return true;
		}

		bool FacePasses(const FaceView& a_view, std::vector<FacePass>& a_batch, std::vector<FacePass>& a_clouds, std::string& a_why)
		{
			return RootPasses(a_view.frustum, a_view.world, true, a_batch, a_clouds, a_why);
		}
	}

	namespace
	{
		/**
		 * @brief The draws of the sky root's passes in draw order: the batch as BuildBatch walks the engine's registrations, then the
		 * cloud layers as BuildClouds walks geometry group 13, the clouds starting from a_cloudModes.
		 */
		bool DrawRootPasses(const Frame& a_frame, const std::vector<FacePass>& a_batch, const std::vector<FacePass>& a_clouds, View a_view,
			std::uint32_t a_renderFlags, Modes a_modes, Modes a_cloudModes, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why)
		{
			a_draws.reserve(a_batch.size() + a_clouds.size());
			a_owners.reserve(a_owners.size() + 8 * (a_batch.size() + a_clouds.size()));
			Modes modes = a_modes;
			std::uint32_t current = UINT32_MAX;
			for (const auto& facePass : a_batch) {
				if (!(a_renderFlags & 0x108))
					modes.cull = (facePass.sub == 2 || facePass.sub == 3) ? 0u : 1u;
				modes.alphaTest = facePass.sub == 1 || facePass.sub == 3 || facePass.sub == 4;
				modes.alphaToCoverage = 0;
				const std::uint32_t technique = TechniqueOf(facePass.passId);
				if (technique != current) {
					if (current != UINT32_MAX)
						modes.Restore();
					modes.Setup(technique);
					current = technique;
				}
				// SetupGeometry: the sun and the moons blend additively.
				const auto type = Field<std::uint32_t>(facePass.property, kSkyObjectType);
				if (type == 0 || type == 6)
					modes.blend = 2;
				auto pass = PassOf(facePass);
				if (!Take(BuildPass(a_frame, &pass, technique, modes, nullptr, a_draws.emplace_back(), a_owners, a_why), a_draws, a_view, technique))
					return false;
			}
			modes = a_cloudModes;
			current = UINT32_MAX;
			std::int32_t lastLayer = -1;
			for (const auto& facePass : a_clouds) {
				if (!(a_renderFlags & 0x108))
					modes.cull = (Field<std::uint64_t>(facePass.property, 0x38) & 0x1000000000ull) ? 0u : 1u;
				const auto* alpha = Field<const std::byte*>(facePass.geometry, 0x120);
				modes.alphaTest = alpha && ((Field<std::uint16_t>(alpha, 0x30) >> 9) & 1u);
				const std::uint32_t technique = TechniqueOf(facePass.passId);
				if (technique != current) {
					if (current != UINT32_MAX)
						modes.Restore();
					modes.Setup(technique);
					current = technique;
				}
				auto pass = PassOf(facePass);
				if (!Take(BuildPass(a_frame, &pass, technique, modes, &lastLayer, a_draws.emplace_back(), a_owners, a_why), a_draws, a_view, technique))
					return false;
			}
			return true;
		}

		/**
		 * @brief The shadow state at the main pass's cloud group, from the state at its sky batch: FinishAccumulating sets write mode 11
		 * between them, and a batch that drew anything ended with its last technique restored and alpha to coverage off.
		 */
		Modes MainCloudModes(Modes a_atBatch, bool a_batchDrew)
		{
			if (a_batchDrew) {
				a_atBatch.Restore();
				a_atBatch.alphaToCoverage = 0;
			}
			a_atBatch.write = 11;
			return a_atBatch;
		}

		/** @brief The adopted snapshot's main camera as a world transform and frustum. False when it has none. */
		bool MainCamera(RE::NiTransform& a_world, RE::NiFrustum& a_frustum, RE::NiPoint3& a_eye)
		{
			const auto& snapshot = Snapshot::Current();
			if (!snapshot)
				return false;
			const auto& view = snapshot->cameras.Current(Snapshot::ViewId::Main);
			if (!view.valid)
				return false;
			a_world.rotate = view.rotation;
			a_world.translate = view.position;
			a_world.scale = 1.0f;
			a_frustum = view.frustum;
			a_eye = view.position;
			return true;
		}
	}

	void RegisterRootCapture()
	{
		RegisterTicketPart(TicketPart::SkyRoot, &CaptureRootObjects);
	}

	bool BuildMain(RE::BSShaderAccumulator* a_accumulator, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.Native.BuildMain");
		a_draws.clear();
		RE::NiTransform world;
		RE::NiFrustum frustum;
		RE::NiPoint3 eye;
		if (!MainCamera(world, frustum, eye)) {
			a_why = "the adopted snapshot has no main camera";
			return false;
		}
		// Render thread: the lists are kept between frames.
		static std::vector<FacePass> batch, clouds;
		if (!RootPasses(frustum, world, false, batch, clouds, a_why))
			return false;
		Frame frame;
		if (!frame.Read(a_accumulator, a_inputs, a_why))
			return false;
		frame.eye = eye;
		const Modes atBatch = Modes::Current();
		return DrawRootPasses(frame, batch, clouds, View::Main, a_inputs.renderFlags, atBatch, MainCloudModes(atBatch, !batch.empty()), a_draws, a_owners, a_why);
	}

	bool BuildFace(const FaceView& a_view, const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.Native.BuildFace");
		a_draws.clear();
		// Render thread: the lists are kept between faces.
		static std::vector<FacePass> batch, clouds;
		if (!FacePasses(a_view, batch, clouds, a_why))
			return false;
		Frame frame;
		if (!frame.ReadFace(a_view, a_inputs, a_why))
			return false;
		return DrawRootPasses(frame, batch, clouds, View::Face, kFaceRenderFlags, Modes::FaceBatch(), Modes::FaceClouds(), a_draws, a_owners, a_why);
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
}
