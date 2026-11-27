// The shadow masks' draws computed from the shadow lights' values (Shadows/Native/LightSet.h). Render thread.
// The engine functions this follows (AE 1.6.1170, SE 1.5.97): the world's masks 0x1414cd6a0 / 0x1412e3b80, a menu scene's masks
// 0x140972f70 / 0x1408d2d70, BSUtilityShader::SetupTechnique 0x1414fa160 / 0x14130df90 and SetupGeometry 0x1414fae40 / 0x14130ec70,
// the mask textures 0x1414fbab0 / 0x14130f960, the mask projections 0x1414fbd30 / 0x14130fbe0, the descriptors 0x1415221e0 and
// 0x141522260 / 0x141334900 and 0x141334970.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"
#include "Features/DrawcallLimitFix/Features/Shadows/Native/NativeCasters.h"
#include "Snapshot/EngineReads.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Variables.h"
#include "RenderGraph/PublishedResources.h"
#include "Snapshot/FrameCamera.h"
#include "Snapshot/Publication.h"
#include "ShaderCache.h"
#include "ShaderTools/LegacyGraphicsCompatibility.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <cstring>
#include <format>

namespace DCLF::Features::ShadowMask::Native
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;
		using Flags = SIE::ShaderCache::UtilityShaderFlags;

		namespace Address
		{
			// The engine globals the masks' setup reads (AE, SE).
			inline constexpr Engine::Offset kWorldNode{ { 415033 }, { 528088 } };        // ShadowSceneNode*: the world masks' caster array
			inline constexpr Engine::Offset kWorldBlend{ { 254901 }, { 304858 } };       // std::uint32_t[4]: the world's write mode by mask channel
			inline constexpr Engine::Offset kMenuBlend{ { 216241 }, { 270481 } };        // std::uint32_t[4]: a menu scene's
			inline constexpr Engine::Offset kCamera{ { 414580 }, { 527667 } };           // NiCamera*: RenderShadowMasks' camera
			inline constexpr Engine::Offset kSceneShadows{ { 390933 }, { 513204 } };     // bool: the scene's shadows are on
			inline constexpr Engine::Offset kRenderMode{ { 414601 }, { 527651 } };       // std::uint32_t: the shader manager's render mode
			inline constexpr Engine::Offset kRenderTargets{ { 411451 }, { 524970 } };    // BSGraphics::RenderTargetManager (the object)
			inline constexpr Engine::Offset kEndTechnique{ { 107653 }, { 100860 } };     // FUN_1414f4350: RestoreTechnique of the open technique
		}

		// Read-only constants of the setup (the same values on both runtimes).
		constexpr float kSplitCount = 4.0f;             // DAT_141ac07b0 / DAT_141867188, the int 4: EndSplitDistances.y, StartSplitDistances.w
		constexpr float kNearParam = 1.0e8f;            // DAT_1418af8b0 / DAT_141667cd0: ShadowLightParam.z for a light with +0x63 clear
		constexpr float kBiasFactor = 0.00025f;         // DAT_141ac1d48 / DAT_141868784
		constexpr float kSecondBiasAe = 8.333334e-05f;  // AE DAT_141ac1d44: SE multiplies kBiasFactor by kThird instead
		constexpr float kThird = 0.33333334f;           // SE DAT_1415d6764

		// The rows of PS b0 and PS b2 (Utility.hlsl's packoffsets).
		constexpr std::uint32_t kVPOSOffset = 0, kShadowSampleParam = 1, kEndSplits = 2, kStartSplits = 3;
		constexpr std::uint32_t kAlphaTestRef = 2, kShadowLightParam = 3, kShadowMapProj = 16;

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

		/** @brief The shadow state's mode fields a mask draw depends on, as the engine sets them light by light. */
		struct Modes
		{
			std::uint32_t fill = 0, cull = 0, depthBias = 0, scissor = 0;
			std::uint32_t blend = 0, alphaToCoverage = 0, write = 0, blendExtra = 0;  // write: the loop's channel mask by light

			static Modes Current()
			{
				const auto& rss = CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
				Modes m;
				m.fill = rss.rasterStateFillMode;
				m.cull = rss.rasterStateCullMode;
				m.depthBias = rss.rasterStateDepthBiasMode;
				m.scissor = rss.rasterStateScissorMode;
				m.blend = rss.alphaBlendMode;
				m.alphaToCoverage = rss.alphaBlendAlphaToCoverage;
				m.write = rss.alphaBlendWriteMode;
				m.blendExtra = rss.alphaBlendModeExtra;
				return m;
			}

			/**
			 * @brief Native: the modes the world masks' loop starts from, as RenderDepth leaves them (observed constant over every native
			 * world frame: back-face culling and the write mode 1, the rest 0).
			 */
			static Modes WorldStart()
			{
				Modes m;
				m.cull = 1;
				m.write = 1;
				return m;
			}
		};

		using Shadows::Native::LightKind;

		/** @brief The light kinds the masks' loop tells apart, by its technique bits. */
		std::uint32_t TechniqueOf(View a_view, const Light& a_light, bool a_focusTargets)
		{
			if (a_view == View::World && a_light.kind == LightKind::Directional)
				return ((a_focusTargets ? 0x2100u : 0x2000u) | 0x200002u) + 0x2B;
			std::uint32_t bits = 0;
			if (a_light.kind == LightKind::Frustum)
				bits = a_view == View::World && a_light.drawFocusShadows ? 0x400100u : 0x400000u;
			else
				bits = a_light.omni ? 0x1000000u : 0x800000u;
			return (bits | 0x2002u) + 0x2B;
		}

		/** @brief FUN_141522260: the pixel descriptor of a mask technique, with the filter's bits. */
		std::uint32_t PixelDescriptorOf(std::uint32_t a_raw, std::uint32_t a_filterBits)
		{
			const auto bits = a_filterBits;
			std::uint32_t d = (a_raw & 0x1E00100u) | ((bits & 1) ? 0x22002u : 0x2002u);
			if (bits & 2)
				d |= 0x40000u;
			if (bits & 4)
				d |= 0x80000u;
			return d;
		}

		/** @brief The vertex shader's VertexDesc for a vertex descriptor: the engine keys its layout on the vanilla shader's. */
		std::uint64_t VertexDescOf(const RE::BSShader& a_shader, std::uint32_t a_descriptor)
		{
			for (const auto& entry : a_shader.vertexShaders)
				if (entry && entry->id == a_descriptor)
					return entry->vertexDesc;
			return 0;
		}

		/** @brief Writes a_row's xyzw. */
		void Row(float (*a_rows)[4], std::uint32_t a_row, float a_x, float a_y, float a_z, float a_w)
		{
			a_rows[a_row][0] = a_x;
			a_rows[a_row][1] = a_y;
			a_rows[a_row][2] = a_z;
			a_rows[a_row][3] = a_w;
		}

		/**
		 * @brief A light map's projection less the position adjustment (FUN_1414fbd30): the matrix with its translation row moved by
		 * a_adjust, transposed. a_rows gets the transpose's first a_count rows. The sums keep the engine's order.
		 */
		void Projection(const float* a_m, const RE::NiPoint3& a_adjust, bool a_cascade, float (*a_rows)[4], std::uint32_t a_count)
		{
			float m[16];
			std::memcpy(m, a_m, sizeof(m));
			for (int j = 0; j < 4; ++j) {
				// The cascade loop adds the translation last; the single map's sums y, x, z (both as the engine compiles them).
				m[12 + j] = a_cascade ? m[12 + j] + ((a_adjust.x * m[j] + a_adjust.y * m[4 + j]) + a_adjust.z * m[8 + j]) :
				                        m[12 + j] + a_adjust.y * m[4 + j] + a_adjust.x * m[j] + a_adjust.z * m[8 + j];
			}
			for (std::uint32_t r = 0; r < a_count; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					a_rows[r][c] = m[c * 4 + r];
		}

		/** @brief Draws left out while their program builds, each logged once by descriptor. */
		Family::RefusalLog& LeftOut()
		{
			static Family::RefusalLog log("[DCLF] shadow mask: a draw is left out until its program is built");
			return log;
		}

		/** @brief What a draw's program key and program are made from. */
		struct ProgramInputs
		{
			ID3D11BlendState* blend = nullptr;
			ID3D11RasterizerState* raster = nullptr;
			std::uint32_t technique = 0;
			bool sceneShadows = false;
			std::uint32_t filterBits = 0;
			std::uint32_t colourFormat = 0;
			bool operator==(const ProgramInputs&) const = default;
		};

		struct CachedProgram
		{
			ProgramInputs inputs;
			Family::ProgramKey key;
			std::shared_ptr<const void> program;
			std::uint64_t vertexDesc = 0;  // the screen quad's vertex shader's
		};

		std::vector<CachedProgram> s_programs;  // render thread

		/** @brief kSHADOW_MASK's format by its view, read once a view. */
		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			static ID3D11RenderTargetView* view = nullptr;
			static std::uint32_t format = 0;
			if (a_rtv != view) {
				view = a_rtv;
				format = 0;
				if (a_rtv) {
					D3D11_RENDER_TARGET_VIEW_DESC desc{};
					a_rtv->GetDesc(&desc);
					format = static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
				}
			}
			return format;
		}

		/** @brief What every light of a batch reads alike, at the call. */
		struct Frame
		{
			View view = View::World;
			State* state = nullptr;
			const RE::BSShader* shader = nullptr;
			const Family::TriShape* quad = nullptr;
			std::uint32_t quadTriangles = 0;
			RE::NiPoint3 adjust;
			bool haveCamera = false;
			float nearPlane = 0.0f, farPlane = 0.0f;  // the world camera's frustum: RenderShadowMasks' camera
			const Shadows::Native::SunSplits* sun = nullptr;
			std::uint32_t filterMode = 0, filterBits = 0;
			bool sceneShadows = false;
			float poisson = 0.0f, resolution = 1.0f, farParam = 0.0f, biasScale = 0.0f, sunBiasScale = 0.0f, secondBiasScale = 0.0f;
			float vposX = 0.0f, vposY = 0.0f;
			std::uint32_t colourFormat = 0;
			const std::uint32_t* writeTable = nullptr;
			bool focusTargets = false;
			Family::Viewport viewport;

			bool Read(View a_view, const Shadows::Native::MaskSettings& a_settings, const Shadows::Native::SunSplits& a_sun, const RE::NiPoint3* a_menuEye,
				std::string& a_why)
			{
				view = a_view;
				state = globals::state;
				shader = CS_ENGINE_READ(Shaders, RecordTime, RE::BSUtilityShader::GetSingleton());
				auto* ism = CS_ENGINE_READ(ImageSpace, RecordTime, RE::ImageSpaceManager::GetSingleton());
				auto* quadGeometry = ism ? Field<RE::BSGeometry*>(ism, 0x48) : nullptr;
				if (!state || !shader || !quadGeometry || quadGeometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
					a_why = "no CS state, utility shader or screen quad";
					return false;
				}
				quad = reinterpret_cast<const Family::TriShape*>(quadGeometry->GetGeometryRuntimeData().rendererData);
				quadTriangles = static_cast<RE::BSTriShape*>(quadGeometry)->GetTrishapeRuntimeData().triangleCount;
				auto& rss = CS_ENGINE_READ(ShadowState, RecordTime, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
				// FUN_1414fbd30's adjustment: the posAdjust, less the lighting offset in two render modes when the scene's shadows are off.
				RE::NiPoint3 offset{};
				if (!a_settings.sceneShadows) {
					const auto mode = Global<std::uint32_t>(Address::kRenderMode);
					auto* node = CS_ENGINE_READ(ShaderManagerState, RecordTime, RE::BSShaderManager::State::GetSingleton()).shadowSceneNode[0];
					if ((mode == 0x16 || mode == 0x11) && node)
						offset = node->GetRuntimeData().lightingOffset;
					offset = { -offset.x, -offset.y, -offset.z };
				}
				// Native: the world view's camera is the snapshot's main camera, which RenderShadowMasks would set (SetCameraData, the
				// current camera). The scene's shadows are on whenever the world view's masks draw, so there is no lighting offset.
				const bool native = Snapshot::FrameCamera::Owned() && (a_view == View::World || a_menuEye);
				if (a_menuEye) {
					// A native menu scene: the menu camera's eye, which its SetCameraData would make the posAdjust. A menu has no cascades,
					// so the camera's planes are not read.
					adjust = *a_menuEye;
				} else if (native) {
					const auto& eye = Snapshot::FrameCamera::Main().data.CameraPosAdjust;
					adjust = { offset.x + eye.x, offset.y + eye.y, offset.z + eye.z };
					if (const auto& snapshot = Snapshot::Current()) {
						const auto& frustum = snapshot->cameras.Current(Snapshot::ViewId::Main).frustum;
						haveCamera = true;
						nearPlane = frustum.fNear;
						farPlane = frustum.fFar;
					}
				} else {
					const auto posAdjust = rss.posAdjust.getEye();
					adjust = { offset.x + posAdjust.x, offset.y + posAdjust.y, offset.z + posAdjust.z };
					if (const auto* camera = Global<const RE::NiCamera*>(Address::kCamera)) {
						haveCamera = true;
						nearPlane = Field<float>(camera, 0x160);
						farPlane = Field<float>(camera, 0x164);
					}
				}
				sun = a_sun.light ? &a_sun : nullptr;
				filterMode = a_settings.filterMode;
				filterBits = a_settings.filterBits;
				sceneShadows = a_settings.sceneShadows;
				poisson = a_settings.poisson;
				resolution = a_settings.resolution;
				farParam = a_settings.farParam;
				biasScale = a_settings.biasScale;
				sunBiasScale = a_settings.sunBiasScale;
				secondBiasScale = a_settings.secondBiasScale;
				// Native draws into the adopted revision's mask, whose size the engine's own target takes ahead of it at a resize.
				static REL::Relocation<const RE::BSGraphics::RenderTargetManager*> targets{ REL::Offset(Address::kRenderTargets) };
				const auto& live = targets->renderTargetData[kShadowMaskTarget];
				const auto adopted = ::RenderGraph::Frame::Vars::Adopted(::RenderGraph::Frame::Vars::EngineTargetShapes()).colours[kShadowMaskTarget];
				const struct
				{
					std::uint32_t width, height;
				} mask{ native ? adopted.width : live.width, native ? adopted.height : live.height };
				if (!mask.width || !mask.height) {
					a_why = "kSHADOW_MASK has no size";
					return false;
				}
				vposX = 1.0f / static_cast<float>(mask.width);
				vposY = 1.0f / static_cast<float>(mask.height);
				colourFormat = FormatOf(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[kShadowMaskTarget].RTV);
				if (!colourFormat) {
					a_why = "no shadow mask target";
					return false;
				}
				writeTable = reinterpret_cast<const std::uint32_t*>(REL::Offset(a_view == View::World ? Address::kWorldBlend : Address::kMenuBlend).address());
				// FUN_1414cec50: focus shadows when the scene's shadows are on and a focus target is listed.
				focusTargets = a_settings.focusTargets;
				viewport = native ? Family::FrameViewport(static_cast<std::int32_t>(kShadowMaskTarget), -1, 1) : Family::LatchViewport(1);
				return true;
			}
		};

		/** @brief SetupTechnique's PS b0 rows for a mask technique (bit 21: the cascaded light). */
		bool TechniqueRows(const Frame& a_frame, std::uint32_t a_technique, float a_slice, DrawConstants& a_c, std::string& a_why)
		{
			auto* rows = a_c.psTechnique;
			Row(rows, kVPOSOffset, a_frame.vposX, a_frame.vposY, 0.0f, 0.0f);
			const bool poisson = a_frame.filterMode - 2u < 2u;
			if (!(a_technique & 0x200000u)) {
				rows[kEndSplits][0] = a_slice;
				rows[kEndSplits][1] = kSplitCount;
				rows[kStartSplits][3] = kSplitCount;
				if (poisson) {
					const float scale = a_frame.poisson / a_frame.resolution;
					rows[kShadowSampleParam][2] = scale;
					rows[kShadowSampleParam][3] = a_frame.poisson * scale;
				}
				return true;
			}
			const auto* sun = a_frame.sun;
			if (!sun || !a_frame.haveCamera) {
				a_why = "a cascaded mask with no sun light or camera";
				return false;
			}
			const auto count = sun->maps;
			if (count == 0 || count > 3) {
				a_why = std::format("a sun light with {} cascades", count);
				return false;
			}
			const float nearPlane = a_frame.nearPlane;
			const float farPlane = a_frame.farPlane;
			const float a = farPlane * nearPlane;
			const float b = farPlane - nearPlane;
			for (std::uint32_t i = 0; i < count; ++i) {
				const float end = sun->end[i];
				rows[kEndSplits][i] = (end * farPlane - a) / (end * b);
				const float start = sun->start[i];
				rows[kStartSplits][i] = (start * farPlane - a) / (start * b);
			}
			rows[kEndSplits][2] = rows[kEndSplits][count - 1];
			rows[kEndSplits][3] = static_cast<float>(count);
			rows[kStartSplits][3] = kSplitCount;
			if (poisson) {
				rows[kShadowSampleParam][2] = a_frame.poisson / a_frame.resolution;
				rows[kShadowSampleParam][3] = a_frame.poisson / a_frame.resolution;
			}
			return true;
		}

		/** @brief SetupGeometry's PS b2 rows for a mask technique: the projections, the biases and the light's parameters. */
		void GeometryRows(const Frame& a_frame, const Light& a_light, std::uint32_t a_technique, DrawConstants& a_c)
		{
			auto* rows = a_c.psGeometry;
			const auto& descriptors = a_light.descriptors;
			// FUN_1414fbd30: a light that is neither a frustum nor a paraboloid light has a map per cascade, each a 4x3.
			const bool cascades = a_light.kind == LightKind::Directional;
			if (cascades) {
				const auto count = (std::min)(a_light.maps, 3u);
				for (std::uint32_t i = 0; i < count && i < descriptors.size(); ++i)
					Projection(descriptors[i].lightTransform.data(), a_frame.adjust, true, rows + kShadowMapProj + i * 3, 3);
			} else if (!descriptors.empty()) {
				Projection(descriptors[0].lightTransform.data(), a_frame.adjust, false, rows + kShadowMapProj, 4);
			}
			const float bias = a_light.biasScale;
			if (Engine::Running() == Engine::Runtime::Se1597) {
				const float first = bias * kBiasFactor * (cascades ? a_frame.sunBiasScale : 1.0f);
				const float second = bias * kBiasFactor * kThird * a_frame.secondBiasScale;
				rows[kAlphaTestRef][1] = a_frame.biasScale * first;
				rows[kAlphaTestRef][2] = a_frame.biasScale * second;
			} else {
				const float first = bias * kBiasFactor * (cascades ? a_frame.sunBiasScale : 1.0f);
				const float second = bias * kSecondBiasAe * a_frame.secondBiasScale;
				rows[kAlphaTestRef][1] = first * a_frame.biasScale;
				rows[kAlphaTestRef][2] = second * a_frame.biasScale;
			}
			rows[kShadowLightParam][2] = a_light.farFlag ? a_frame.farParam : kNearParam;
			if (a_technique & 0x1800000u)
				rows[kShadowLightParam][0] = a_light.hasNiLight ? a_light.radius : 0.0f;
			else if (a_technique & 0x400000u)
				rows[kShadowLightParam][0] = a_light.spotFalloff;
		}

		std::uint64_t s_builds = 0;  // render thread: one per batch

		/** @brief One light's draw. */
		PassResult BuildLight(const Frame& a_frame, const Light& a_light, Modes& a_modes, Run& a_run, Draw& a_draw, std::string& a_why)
		{
			const auto raw = TechniqueOf(a_frame.view, a_light, a_frame.focusTargets);
			const auto technique = raw - 0x2B;
			if (a_light.descriptors.empty()) {
				a_why = "a shadow light with no shadow map";
				return PassResult::Failed;
			}
			const auto mask = a_light.maskIndex;
			if (mask > 3) {
				a_why = std::format("a shadow light on mask channel {}", mask);
				return PassResult::Failed;
			}
			// The loop's write mode by channel, then SetupTechnique's depth bias and, for a local light, its scissor.
			a_modes.write = a_frame.writeTable[mask];
			a_modes.depthBias = 1;
			if (!(technique & 0x200000u))
				a_modes.scissor = 1;

			auto& c = a_draw.constants;
			c = {};
			const float slice = static_cast<float>(static_cast<std::int32_t>(a_light.descriptors[0].slice));
			if (!TechniqueRows(a_frame, technique, slice, c, a_why))
				return PassResult::Failed;
			GeometryRows(a_frame, a_light, technique, c);
			// SetupTechnique turns the alpha test off.
			c.alphaTestRef = 0.0f;

			// The program, cached by what makes it: the descriptors, the engine's state objects and the target format. A frame
			// uses a handful of keys, and the program cache never drops a built program.
			const ProgramInputs inputs{ .blend = Pipeline::EngineBlendState(a_modes.blend, a_modes.alphaToCoverage, a_modes.write, a_modes.blendExtra),
				.raster = Pipeline::EngineRasterState(a_modes.fill, a_modes.cull, a_modes.depthBias, a_modes.scissor),
				.technique = technique,
				.sceneShadows = a_frame.sceneShadows,
				.filterBits = a_frame.filterBits,
				.colourFormat = a_frame.colourFormat };
			auto cached = std::ranges::find(s_programs, inputs, &CachedProgram::inputs);
			if (cached == s_programs.end()) {
				CachedProgram entry{ .inputs = inputs };
				const std::uint32_t vertexDescriptor = VertexDescriptorOf(technique);
				std::uint32_t pixelDescriptor = PixelDescriptorOf(technique, a_frame.filterBits);
				if (LegacyGraphicsCompatibility::IsLegacyVersion())
					pixelDescriptor = LegacyGraphicsCompatibility::NormalizeLegacyUtilityDescriptor(pixelDescriptor);
				auto modifiedVertex = vertexDescriptor;
				a_frame.state->ModifyShaderLookup(*a_frame.shader, modifiedVertex, pixelDescriptor);
				entry.key.descriptor = pixelDescriptor;
				entry.key.defineStage = Family::DefineStage::Pixel;
				if (!Family::DescribeStates(entry.key, inputs.blend, nullptr, inputs.raster)) {
					a_why = "a blend or rasterizer state with no description";
					return PassResult::Failed;
				}
				entry.key.colourFormats[0] = a_frame.colourFormat;
				entry.vertexDesc = VertexDescOf(*a_frame.shader, vertexDescriptor);
				bool building = false;
				entry.program = ShadowMaskDraws::Program(entry.key, *a_frame.shader, &building);
				if (!entry.program) {
					a_why = std::format("descriptor {:#x}", pixelDescriptor);
					return building ? PassResult::LeftOut : PassResult::Failed;
				}
				// Bounded: the inputs come from fixed engine tables, so only a changed target format makes new entries.
				if (s_programs.size() >= 64)
					s_programs.clear();
				cached = s_programs.insert(s_programs.end(), std::move(entry));
			}
			a_draw.key = cached->key;
			a_draw.program = cached->program;

			// The screen quad, drawn whole.
			Family::Owners owners;
			Family::Geometry geometry;
			std::string detail;
			if (const auto result = Family::LatchGeometry(a_frame.quad, 0, a_frame.quadTriangles, cached->vertexDesc,
					D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, geometry, owners, &detail);
				result != Family::GeometryResult::Ok) {
				a_why = std::format("the screen quad: {} {}", Family::ReasonOf(result), detail);
				return PassResult::Failed;
			}
			c.vertexRef = geometry.vertexRef;
			c.indexRef = geometry.indexRef;
			c.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
			c.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			c.vertexStride = geometry.stride;
			c.firstIndex = geometry.firstIndex;
			a_draw.indexCount = geometry.indexCount;

			// t2 the main depth (WRAP, POINT), the light's map in t4 (t3 when the filter has no bits) and its sampler. The ORG build
			// reads the map through t4's index either way. Other slots get a null view and the first sampler.
			auto& depths = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils;
			const auto target = static_cast<std::uint32_t>(a_light.descriptors[0].target);
			if (target >= RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL) {
				a_why = std::format("a shadow map in depth target {}", target);
				return PassResult::Failed;
			}
			std::array<ID3D11ShaderResourceView*, kSlots> views{};
			views[2] = Unwrap(depths[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].depthSRV);
			views[4] = Unwrap(depths[target].depthSRV);
			for (std::uint32_t t = 0; t < kSlots; ++t) {
				auto* view = views[t];
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto slot = Family::SlotTexture(view, SlotKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), c.textures[t], owners, &failure);
				if (slot == Family::SlotResult::Published) {
					// A depth a graph pass writes is read by published id, so the graph orders the write before the masks.
					const auto id = PublishedResources::ExportedId(view);
					const auto result = ShadowMaskDraws::ReadPublished(a_frame.view, id, c.textures[t]);
					if (result != Family::InputResult::Graph) {
						a_why = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(result));
						return PassResult::Failed;
					}
				} else if (slot != Family::SlotResult::Ok) {
					a_why = std::format("a texture it samples could not be imported: {}", GpuTextures::DescribeFailure(view, t, failure));
					return PassResult::Failed;
				}
			}
			// s4 compares (filter 4) when the filter has bits; then SetupGeometry leaves s3 alone, and it holds the default sampler
			// (address 0, anisotropic) that the engine's draw and the ORG build's filter-0 path read. Without bits s3 is set to POINT.
			const bool comparison = a_frame.filterBits != 0;
			for (std::uint32_t s = 0; s < kSlots; ++s) {
				std::uint32_t filter = 0;
				if (s == 4 && comparison)
					filter = 4;
				else if (s == 3 && comparison)
					filter = 3;
				c.samplers[s] = Family::SlotSampler(0, filter, owners);
				if (c.samplers[s] == GpuTextures::kInvalid) {
					a_why = "its sampler modes have no graph sampler";
					return PassResult::Failed;
				}
			}
			a_draw.viewport = a_frame.viewport;
			for (auto& owner : owners)
				if (owner)
					a_run.owners.push_back(std::move(owner));

			// RestoreTechnique.
			a_modes.depthBias = 0;
			a_modes.scissor = 0;
			return PassResult::Drawn;
		}
	}

	std::uint32_t VertexDescriptorOf(std::uint32_t a_raw)
	{
		std::uint32_t d = a_raw & 0xF7E5FF9Fu;
		if (Global<bool>(Address::kSceneShadows) && (a_raw & 0x1E00000u) && (Engine::Running() == Engine::Runtime::Se1597 || (d & 0x2000u)))
			d = (a_raw & 0x1E00000u) | 0x2002u;
		if ((d & 0x14000u) != 0x14000u && ((d & 0x20004000u) == 0x4000u || (d & 0x1E02000u) == 0x2000u) && !(d & 0x80u) && (d & 0x14000u) != 0x10000u)
			d &= 0xDFFFE1E4u;
		return d;
	}

	bool Build(View a_view, Run& a_run, std::span<const Light> a_lights, const Shadows::Native::MaskSettings& a_settings, const Shadows::Native::SunSplits& a_sun,
		std::string& a_why, const RE::NiPoint3* a_menuEye)
	{
		DCLF_ZONE("CS.DCLF.ShadowMask.Build");
		++s_builds;
		Frame frame;
		if (!frame.Read(a_view, a_settings, a_sun, a_menuEye, a_why))
			return false;
		const bool native = Snapshot::FrameCamera::Owned() && a_view == View::World;
		// A native menu scene starts from the same modes: nothing engine-side ran before it.
		auto modes = native || a_menuEye ? Modes::WorldStart() : Modes::Current();
		for (const auto& light : a_lights) {
			if (!light.maps) {
				a_why = "a shadow light with no maps";
				return false;
			}
			// Native: a kept light whose maps RenderShadowMaps did not draw casts no shadow this frame; its channel stays lit.
			if (native && !Shadows::Native::DrewMaps(light.light))
				continue;
			if (a_run.draws.size() >= kMaxDraws) {
				a_why = std::format("more than {} shadow lights", kMaxDraws);
				return false;
			}
			Draw draw{};
			std::string why;
			switch (BuildLight(frame, light, modes, a_run, draw, why)) {
			case PassResult::Drawn:
				a_run.draws.push_back(std::move(draw));
				break;
			case PassResult::LeftOut:
				LeftOut().Report(why);
				break;
			case PassResult::Failed:
				a_why = std::move(why);
				return false;
			}
		}
		return true;
	}

	const RE::ShadowSceneNode* SceneNodeOf(View a_view)
	{
		return a_view == View::World ? Global<const RE::ShadowSceneNode*>(Address::kWorldNode) :
		                               CS_ENGINE_READ(ShaderManagerState, RecordTime, RE::BSShaderManager::State::GetSingleton()).shadowSceneNode[1];
	}

	Shadows::Native::SunSplits ReadSun()
	{
		auto* node0 = CS_ENGINE_READ(ShaderManagerState, RecordTime, RE::BSShaderManager::State::GetSingleton()).shadowSceneNode[0];
		return Shadows::Native::ReadSun(node0 ? reinterpret_cast<const RE::BSShadowLight*>(node0->GetRuntimeData().sunShadowDirLight) : nullptr);
	}

	ID3D11ShaderResourceView* SunMap(View a_view, std::span<const Light> a_lights)
	{
		if (a_view != View::World)
			return nullptr;
		for (const auto& light : a_lights) {
			if (light.kind != LightKind::Directional || light.descriptors.empty())
				continue;
			const auto target = static_cast<std::uint32_t>(light.descriptors[0].target);
			if (target < RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL)
				return Unwrap(CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils[target].depthSRV);
		}
		return nullptr;
	}

	void EndTechnique()
	{
		static REL::Relocation<void (*)(std::uint32_t)> end{ REL::Offset(Address::kEndTechnique) };
		end(0);
	}
}
