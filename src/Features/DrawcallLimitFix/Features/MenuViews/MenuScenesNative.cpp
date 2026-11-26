// The native menu scenes' draws (roadmap phase 5, docs/development/plans/snap5-menus.md): what a menu scene's Lighting draws bind,
// computed by CS from the menu's scene graph, its scene node's lights and camera, through DCLF's ports (EvaluateTechnique,
// MaterialPort, GeometryPort) and Light Limit Fix's light rows. No engine setup function runs.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"

#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "ShaderCache.h"
#include "Snapshot/CoreFrame.h"
#include "State.h"
#include "Utils/Logging.h"

#include <algorithm>

#include <cstring>

namespace DCLF::Features::MenuViews::Scenes::Native
{
	namespace
	{
		/** @brief Packs one constant group into rows through a shader's constant table, and marks the floats it wrote in a_written. */
		void Pack(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_group, std::uint32_t a_first,
			float (*a_rows)[4], float (*a_written)[4], std::uint32_t a_count)
		{
			std::array<std::byte, 1024> bytes{};
			PackConstantGroup(a_block, a_layout, a_table, a_group, a_first, bytes);
			std::memcpy(a_rows, bytes.data(), a_count * 16);
			// The same group of a block that holds 1 where a_block is written.
			ConstantBlock mask;
			mask.Reset();
			for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f)
				if (a_block.Written(f))
					mask.floats[f] = 1.0f;
			bytes.fill(std::byte{ 0 });
			PackConstantGroup(mask, a_layout, a_table, a_group, a_first, bytes);
			std::memcpy(a_written, bytes.data(), a_count * 16);
		}

		/** @brief SetupGeometry's World with Community Shaders' world-space patch: rotation times scale, the translation less a_eye. */
		void WorldRows(const RE::NiTransform& a_world, const RE::NiPoint3& a_eye, float* a_out)
		{
			const auto& r = a_world.rotate.entry;
			const float s = a_world.scale;
			const float t[3]{ a_world.translate.x - a_eye.x, a_world.translate.y - a_eye.y, a_world.translate.z - a_eye.z };
			for (int i = 0; i < 3; ++i) {
				a_out[i * 4 + 0] = r[i][0] * s;
				a_out[i * 4 + 1] = r[i][1] * s;
				a_out[i * 4 + 2] = r[i][2] * s;
				a_out[i * 4 + 3] = t[i];
			}
		}

		/** @brief The point light rows (c15, c22, c29.x) as the latch writes Light Limit Fix's strict lights for a pass outside the world. */
		void PointLightRows(const Lights& a_lights, const RE::NiPoint3& a_eye, LightingSceneConstants& a_out, LightingSceneConstants& a_written)
		{
			auto& llf = globals::features::lightLimitFix;
			if (!llf.loaded)
				return;
			constexpr std::uint32_t kPositions = 15, kColours = 22, kCount = 29, kMaxLights = 7;
			std::uint32_t count = 0;
			for (std::uint32_t i = 1; i < a_lights.count && count < kMaxLights; ++i) {
				auto* bsLight = a_lights.lights[i];
				auto* niLight = bsLight ? bsLight->light.get() : nullptr;
				if (!niLight)
					continue;
				auto light = llf.ProcessLight(bsLight, niLight);
				light.fade *= bsLight->lodDimmer;
				if (light.lightFlags.any(LightLimitFix::LightFlags::Disabled))
					continue;
				const auto position = niLight->world.translate - a_eye;
				const float row0[4]{ position.x, position.y, position.z, light.radius };
				const float row1[4]{ light.color.x * light.fade, light.color.y * light.fade, light.color.z * light.fade, 0.0f };
				std::memcpy(a_out.psGeometry[kPositions + count], row0, 16);
				std::memcpy(a_out.psGeometry[kColours + count], row1, 16);
				for (std::uint32_t c = 0; c < 4; ++c)
					a_written.psGeometry[kPositions + count][c] = a_written.psGeometry[kColours + count][c] = 1.0f;
				++count;
			}
			a_out.psGeometry[kCount][0] = static_cast<float>(count);
			a_written.psGeometry[kCount][0] = 1.0f;
		}
	}

	Inputs SampleInputs(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, const PreviousCamera& a_previous)
	{
		Inputs in;
		in.eye = a_camera.world.translate;
		in.sceneNode = a_manager.shadowSceneNode;
		in.technique = SampleTechniqueInputs();
		in.pipeline = GeometryPort::SamplePipelineFrame(in.sceneNode ? in.sceneNode->GetRuntimeData().sunLight : nullptr, true);
		// The material port reads the engine's Lighting shader object, which DCLF captures only in a world frame.
		if (!EngineShaders::Lighting()) {
			static const REL::Relocation<RE::BSShader**> lightingShader{ REL::Offset(Engine::Address::kLightingShader) };
			if (auto* lighting = *lightingShader.get(); lighting && lighting->shaderType.get() == RE::BSShader::Type::Lighting)
				EngineShaders::SetLighting(lighting);
		}
		in.material = MaterialPort::SampleFrame();
		Snapshot::FrameCamera::Capture(a_camera, in.camera);
		// The previous camera is the menu scene's last one; the first render has none and takes its own, as the engine's first does not.
		in.previous = a_previous.valid ? a_previous.camera : Snapshot::FrameCamera::Previous{ in.camera.viewProj, in.eye };
		in.havePrevious = a_previous.valid;
		in.previousEye = a_previous.valid ? a_previous.camera.eye : RE::NiPoint3{};
		return in;
	}

	void SelectLights(const Inputs& a_in, const RE::BSGeometry& a_geometry, Lights& a_out)
	{
		a_out = {};
		if (!a_in.sceneNode)
			return;
		const auto& data = a_in.sceneNode->GetRuntimeData();
		const auto push = [&](RE::BSLight* a_light) {
			if (a_out.count < a_out.lights.size())
				a_out.lights[a_out.count++] = a_light;
		};
		// A light reaches a geometry whose bound its radius touches.
		const auto lights = [&](const RE::BSLight& a_light) {
			const auto* niLight = a_light.light.get();
			const auto d = niLight->world.translate - a_geometry.worldBound.center;
			const float reach = niLight->GetLightRuntimeData().radius.x + a_geometry.worldBound.radius;
			return d.x * d.x + d.y * d.y + d.z * d.z <= reach * reach;
		};
		push(data.sunLight);
		for (const auto& light : data.activeShadowLights)
			if (light && light->light && lights(*light)) {
				push(light.get());
				++a_out.shadow;
			}
		for (const auto& light : data.activeLights)
			if (light && light->light && lights(*light))
				push(light.get());
	}

	void FrameRows(const Inputs& a_in, float (*a_out)[4])
	{
		std::memset(a_out, 0, kFrameBytes);
		Snapshot::FrameCamera::WriteCamera(a_in.camera.view, a_in.camera.proj, a_in.camera.viewProj, a_in.eye, { 0.0f, 0.0f }, &a_in.previous, &a_out[0][0]);
		Snapshot::FrameCamera::WriteRatios({}, Snapshot::Core().drClampOffset, &a_out[0][0]);
		const float params[4]{ Snapshot::Core().inverseGamma, 1.0f, 0.0f, 0.0f };
		std::memcpy(a_out[42], params, sizeof(params));
	}

	bool ComputeLighting(const Inputs& a_in, const RE::BSGeometry& a_geometry, LightingResult& a_out, std::string& a_why)
	{
		a_out = {};
		const auto* property = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		if (!property || !property->material) {
			a_why = "not a Lighting property with a material";
			return false;
		}

		// The pass descriptor: derived from the scene graph, with the shadow lights' bits GetRenderPasses adds from the pass's lights.
		SelectLights(a_in, a_geometry, a_out.lights);
		{
			SceneCapture::LiveLeaf leaf(a_geometry);
			LightingDescriptors derived{};
			const auto reason = leaf.view.property ? DeriveLightingDescriptors(*leaf.view.property, leaf.view, nullptr, derived) : Ineligible::NotLightingShader;
			if (derived.derivedPass == kNotDerived) {
				a_why = std::format("no pass descriptor ({})", kIneligibleNames[static_cast<std::size_t>(reason)]);
				return false;
			}
			a_out.pass = derived.derivedPass;
			a_out.rawPixel = derived.rawPixel;
			if (a_out.lights.shadow)
				a_out.pass |= (1u << 14) | ((a_out.lights.shadow & 7u) << 6);
		}
		LightingShaderDescriptors(a_out.pass, false, a_out.vertex, a_out.pixel);

		auto* shader = EngineShaders::Lighting();
		auto* vs = shader ? globals::shaderCache->GetVertexShader(*shader, a_out.vertex) : nullptr;
		auto* ps = shader ? globals::shaderCache->GetPixelShader(*shader, a_out.pixel) : nullptr;
		if (!vs || !ps) {
			a_why = std::format("no shader for vs {:#x} ps {:#x}", a_out.vertex, a_out.pixel);
			return false;
		}
		const std::span<const std::uint8_t> vsTable(reinterpret_cast<const std::uint8_t*>(vs->constantTable.data()), vs->constantTable.size());
		const std::span<const std::uint8_t> psTable(reinterpret_cast<const std::uint8_t*>(ps->constantTable.data()), ps->constantTable.size());
		auto& c = a_out.constants;
		auto& w = a_out.written;
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();

		// PerTechnique (SetupTechnique).
		TechniqueConstants technique{};
		EvaluateTechnique(a_out.pass, a_in.technique, technique);
		Pack(technique.vs, vsLayout, vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique], c.vsTechnique, w.vsTechnique, kLightingVSTechniqueRows);
		Pack(technique.ps, psLayout, psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique], c.psTechnique, w.psTechnique, kLightingPSTechniqueRows);

		// PerMaterial (SetupMaterial, with Community Shaders' hooks as the port's contributors, Linear Lighting's SpecularColor among them).
		MaterialPort::MaterialSnapshot snapshot;
		if (!MaterialPort::Capture(*property->material, snapshot) || !MaterialPort::Evaluate(snapshot, a_out.pass, a_in.material, a_out.record)) {
			a_why = "the material port does not cover its material";
			return false;
		}
		Pack(a_out.record.vs, vsLayout, vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial], c.vsMaterial, w.vsMaterial, kLightingVSMaterialRows);
		Pack(a_out.record.ps, psLayout, psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial], c.psMaterial, w.psMaterial, kLightingPSMaterialRows);

		// PerGeometry (SetupGeometry): the pipeline's frame values from the menu's scene node, then the object's.
		GeometryConstants geometry{};
		if (!GeometryPort::PipelineGeometryConstants(a_out.pass, GeometryPort::kMainPassRenderFlags, a_in.pipeline, geometry)) {
			a_why = "the menu's scene node has no sun light";
			return false;
		}
		WorldRows(a_geometry.world, a_in.eye, &geometry.vs.floats[vsLayout.offset[kVSWorld]]);
		WorldRows(a_geometry.previousWorld, a_in.havePrevious ? a_in.previousEye : a_in.eye, &geometry.vs.floats[vsLayout.offset[kVSPreviousWorld]]);
		BindlessShading shading{};
		SampleShading(*property, a_out.pass, false, shading);
		for (std::uint32_t i = 0; i < 4; ++i)
			if (shading.written & (1u << i))
				geometry.ps.floats[psLayout.offset[kPSMaterialData] + i] = shading.shading.materialData[i];
		for (std::uint32_t i = 0; i < 3; ++i)
			if (shading.written & (0x10u << i))
				geometry.ps.floats[psLayout.offset[kPSEmitColor] + i] = shading.shading.emitColor[i];
		if (shading.written & 0x80u)
			geometry.ps.floats[psLayout.offset[kPSSSRParams] + 3] = shading.shading.ssrSpecular;
		Pack(geometry.vs, vsLayout, vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry], c.vsGeometry, w.vsGeometry, kLightingVSGeometryRows);
		Pack(geometry.ps, psLayout, psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry], c.psGeometry, w.psGeometry, kLightingPSGeometryRows);
		// The point light rows replace what the pipeline block left there.
		for (std::uint32_t r = 15; r < 29; ++r)
			for (std::uint32_t i = 0; i < 4; ++i)
				c.psGeometry[r][i] = w.psGeometry[r][i] = 0.0f;
		PointLightRows(a_out.lights, a_in.eye, c, w);
		return true;
	}

	Permutation PermutationOf(const RE::BSGeometry& a_geometry, bool a_effect)
	{
		using Extra = State::ExtraShaderDescriptors;
		Permutation out;
		// The per-draw bits are this draw's; the rest is the frame's.
		out.extra = globals::state->permutationData.ExtraShaderDescriptor &
		            ~(static_cast<std::uint32_t>(Extra::InWorld) | static_cast<std::uint32_t>(Extra::SourceAlphaBlend) | static_cast<std::uint32_t>(Extra::ProjectedTextureIsSRGB));
		auto& linear = globals::features::linearLighting;
		if (!linear.loaded || !linear.IsLinearLightingActive())
			return out;
		// Linear Lighting's RenderMenuScene sets RenderToUI around UI3DSceneManager::Render, and its SetupGeometry hooks the rest.
		out.renderToUI = 1;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		if (a_effect) {
			const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
			using Function = RE::NiAlphaProperty::AlphaFunction;
			if (property && !property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kPremultAlpha) && alpha && alpha->GetAlphaBlending() &&
				alpha->GetSrcBlendMode() == Function::kSrcAlpha && (alpha->GetDestBlendMode() == Function::kInvSrcAlpha || alpha->GetDestBlendMode() == Function::kOne))
				out.extra |= static_cast<std::uint32_t>(Extra::SourceAlphaBlend);
		} else if (property && property->material) {
			// With RenderToUI set only a water reflection source is in working space.
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			out.baseTextureIsWorking = material->diffuseRenderTargetSourceIndex == RE::RENDER_TARGETS::kWATER_REFLECTIONS;
		}
		return out;
	}

	bool BoneRows(const RE::BSGeometry& a_geometry, std::span<std::byte, kBonesBytes> a_out)
	{
		auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
		if (!skin)
			return false;
		const std::uint32_t rows = skin->numMatrices * SkinPalettes::kRowsPerBone;
		const float *current = nullptr, *previous = nullptr;
		if (!rows || rows > kPaletteRows || !SkinPalettes::Sample(a_geometry, rows, current, previous))
			return false;
		std::fill(a_out.begin(), a_out.end(), std::byte{ 0 });
		std::memcpy(a_out.data(), previous, rows * 16);
		std::memcpy(a_out.data() + kBonesBytes / 2, current, rows * 16);
		return true;
	}

	bool ComputeEffect(const Inputs& a_in, const RE::BSGeometry& a_geometry, EffectResult& a_out, std::string& a_why)
	{
		namespace EF = Effects::EffectFlag;
		a_out = {};
		const auto* property = netimmerse_cast<const RE::BSEffectShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
		if (!property || !property->material) {
			a_why = "not an Effect property with a material";
			return false;
		}
		{
			SceneCapture::LiveLeaf leaf(a_geometry);
			LightingDescriptors derived{};
			const auto reason = leaf.view.property ? Effects::DeriveEffectDescriptors(*leaf.view.property, leaf.view, derived) : Ineligible::NotLightingShader;
			if (derived.derivedPass == kNotDerived || (reason != Ineligible::None && derived.pass == 0)) {
				a_why = std::format("no pass descriptor ({})", kIneligibleNames[static_cast<std::size_t>(reason)]);
				return false;
			}
			a_out.pass = derived.pass;
			a_out.vertex = derived.vertex;
			a_out.pixel = derived.pixel;
			a_out.rawPixel = derived.rawPixel;
		}
		constexpr std::uint32_t kUnported = EF::Particles | EF::StripParticles | EF::Membrane | EF::Lighting | EF::ProjectedUv | EF::Soft | EF::Blood | EF::Skinned |
		                                    EF::TexCoordIndex;
		if (a_out.pass & kUnported) {
			a_why = std::format("an Effect variant the native menu scene does not draw (pass {:#x})", a_out.pass);
			return false;
		}
		static const REL::Relocation<RE::BSShader**> effectShader{ REL::Offset(Effects::Address::kEffectShader) };
		auto* shader = *effectShader.get();
		auto* vs = shader ? globals::shaderCache->GetVertexShader(*shader, a_out.vertex) : nullptr;
		auto* ps = shader ? globals::shaderCache->GetPixelShader(*shader, a_out.pixel) : nullptr;
		if (!vs || !ps) {
			a_why = std::format("no shader for vs {:#x} ps {:#x}", a_out.vertex, a_out.pixel);
			return false;
		}
		const std::span<const std::uint8_t> vsTable(reinterpret_cast<const std::uint8_t*>(vs->constantTable.data()), vs->constantTable.size());
		const std::span<const std::uint8_t> psTable(reinterpret_cast<const std::uint8_t*>(ps->constantTable.data()), ps->constantTable.size());
		const auto& space = Extensions::MaterialSpaceOf(Effects::kMaterialKindEffect);
		const auto& vsLayout = space.vs.layout;
		const auto& psLayout = space.ps.layout;
		auto& c = a_out.constants;
		auto& w = a_out.written;
		const auto variable = [](ConstantBlock& a_block, const StageLayout& a_layout, std::uint32_t a_variable) { return &a_block.floats[a_layout.offset[a_variable]]; };

		// PerTechnique (SetupTechnique, AE 0x1414ea2c0): the scene graph's fog, as Lighting's, and FilteringParam.
		ConstantBlock vs0, ps0;
		vs0.Reset();
		ps0.Reset();
		if (a_in.technique.fog) {
			float* param = variable(vs0, vsLayout, Effects::kEffectVSFogParam);
			if (a_in.technique.fogFar != 0.0f || a_in.technique.fogNear != 0.0f) {
				const float inverseRange = 1.0f / (a_in.technique.fogFar - a_in.technique.fogNear);
				param[0] = inverseRange * a_in.technique.fogNear;
				param[1] = inverseRange;
				param[2] = a_in.technique.fogPower;
				param[3] = a_in.technique.fogClamp;
			} else {
				param[0] = 5.0e6f;
				param[1] = 0.1f;
				param[2] = 1.0f;
				param[3] = 0.0f;
			}
			float* nearColour = variable(vs0, vsLayout, Effects::kEffectVSFogNearColor);
			for (std::uint32_t i = 0; i < 3; ++i)
				nearColour[i] = a_in.technique.fogNearColor[i];
			nearColour[3] = a_in.technique.invFrameBufferRange;
			float* farColour = variable(vs0, vsLayout, Effects::kEffectVSFogFarColor);
			for (std::uint32_t i = 0; i < 3; ++i)
				farColour[i] = a_in.technique.fogFarColor[i];
		}
		{
			static const REL::Relocation<const float*> filterX{ RELOCATION_ID(513745, 391710) }, filterY{ RELOCATION_ID(513747, 391713) };
			float* filter = variable(ps0, psLayout, Effects::kEffectPSFilteringParam);
			filter[0] = *filterX.get();
			filter[1] = *filterY.get();
		}
		Pack(vs0, vsLayout, vsTable, Effects::kEffectVSGroups[0], Effects::kEffectVSFirstVariable[0], c.vsTechnique, w.vsTechnique, kEffectVSTechniqueRows);
		Pack(ps0, psLayout, psTable, Effects::kEffectPSGroups[0], Effects::kEffectPSFirstVariable[0], c.psTechnique, w.psTechnique, kEffectPSTechniqueRows);

		// PerMaterial (the Effect material port, which converts BaseColor into Linear Lighting's working space).
		MaterialPort::MaterialSnapshot snapshot;
		if (!MaterialPort::Capture(*property->material, snapshot) || snapshot.port != Effects::kMaterialKindEffect ||
			!MaterialPort::Evaluate(snapshot, a_out.pass, a_in.material, a_out.record)) {
			a_why = "the Effect material port does not cover its material";
			return false;
		}
		auto& linear = globals::features::linearLighting;
		const bool linearOn = linear.loaded && linear.IsLinearLightingActive();
		Pack(a_out.record.vs, vsLayout, vsTable, Effects::kEffectVSGroups[1], Effects::kEffectVSFirstVariable[1], c.vsMaterial, w.vsMaterial, kEffectVSMaterialRows);
		Pack(a_out.record.ps, psLayout, psTable, Effects::kEffectPSGroups[1], Effects::kEffectPSFirstVariable[1], c.psMaterial, w.psMaterial, kEffectPSMaterialRows);

		// PerGeometry (SetupGeometry, AE 0x1414eab10): World and PreviousWorld less posAdjust, and PropertyColor.
		ConstantBlock vs2, ps2;
		vs2.Reset();
		ps2.Reset();
		WorldRows(a_geometry.world, a_in.eye, variable(vs2, vsLayout, Effects::kEffectVSWorld));
		WorldRows(a_geometry.previousWorld, a_in.havePrevious ? a_in.previousEye : a_in.eye, variable(vs2, vsLayout, Effects::kEffectVSPreviousWorld));
		{
			float* colour = variable(ps2, psLayout, Effects::kEffectPSPropertyColor);
			const auto* emittance = property->emittanceColor;
			RE::NiColor rgb = emittance ? *emittance : RE::NiColor{ 1.0f, 1.0f, 1.0f };
			// Linear Lighting's EffectGeometryUpload converts it, unless the pass maps greyscale to colour.
			if (linearOn && !(a_out.rawPixel & EF::GrayscaleToColor))
				rgb = linear.SRGBToWorking(rgb);
			colour[0] = rgb.red;
			colour[1] = rgb.green;
			colour[2] = rgb.blue;
			colour[3] = property->alpha;
		}
		Pack(vs2, vsLayout, vsTable, Effects::kEffectVSGroups[2], Effects::kEffectVSFirstVariable[2], c.vsGeometry, w.vsGeometry, kEffectVSGeometryRows);
		Pack(ps2, psLayout, psTable, Effects::kEffectPSGroups[2], Effects::kEffectPSFirstVariable[2], c.psGeometry, w.psGeometry, kEffectPSGeometryRows);
		return true;
	}

	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		/** @brief The reason a draw is left out while its program builds on the program service (the next session prebuilds it). */
		constexpr const char* kBuilding = "its program is still building";

		/** @brief What a draw needs besides its constants: the states SetupGeometry and the alpha setup leave, and its targets. */
		struct DrawState
		{
			std::uint32_t blend = 0, write = 1, depth = 3, cull = 1;
			float alphaRef = 0.0f;
		};

		/**
		 * @brief The draw's states as the engine's batch leaves them: the alpha property's blend (FUN_14150bc80) and test, the depth flags,
		 * two-sided culling, and the write mode the menu render left (1 after a depth pass and for kMAIN, 12 into the back buffer).
		 */
		DrawState StateOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty& a_property, bool a_effect, std::int32_t a_target)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			DrawState state;
			const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
			if (alpha && alpha->GetAlphaBlending()) {
				const auto destination = alpha->GetDestBlendMode();
				state.blend = destination == RE::NiAlphaProperty::AlphaFunction::kOne ? 2u : 1u;
			}
			if (alpha && alpha->GetAlphaTesting())
				state.alphaRef = static_cast<float>(alpha->alphaThreshold) / 255.0f;
			const bool test = a_property.flags.any(Flag::kZBufferTest), write = a_property.flags.any(Flag::kZBufferWrite);
			state.depth = a_effect ? (test ? (write ? 3u : 1u) : 0u) : 3u;
			state.cull = a_property.flags.any(Flag::kTwoSided) ? 0u : 1u;
			state.write = a_effect ? 11u : (a_target == kFramebufferTarget ? 12u : 1u);
			return state;
		}

		/** @brief The colour targets of a menu scene's colour pass, as UI::Render and UI3DSceneManager::Render bind them. */
		std::array<std::int32_t, kMaxColours> ColourTargets(std::int32_t a_scheme, bool a_taa)
		{
			if (a_scheme == 4)
				return { kMainTarget, kMotionVectorTarget, kNormalTaaMaskSwapTarget };
			const bool motion = a_taa && (a_scheme == 1 || a_scheme == 2 || a_scheme == 7);
			return { kFramebufferTarget, motion ? kMotionVectorTarget : -1, -1 };
		}

		/** @brief Maps a_targets into the colour client's imports. Null when mapped. */
		const char* MapColours(const std::array<std::int32_t, kMaxColours>& a_targets, std::uint32_t a_outputs, Draw& a_draw)
		{
			const auto imports = TargetsOf(View::MenuColour);
			a_draw.colourCount = 0;
			for (std::uint32_t i = 0; i < (std::min)(a_outputs, kMaxColours); ++i) {
				if (a_targets[i] < 0)
					break;
				const auto found = std::ranges::find(imports, a_targets[i]);
				if (found == imports.end())
					return "a colour target its view's client does not import";
				const auto index = static_cast<std::uint32_t>(found - imports.begin());
				a_draw.colours[i] = static_cast<std::uint8_t>(index);
				a_draw.key.colourFormats[i] = SceneDraws::ColourFormat(View::MenuColour, index);
				if (!a_draw.key.colourFormats[i])
					return "a colour target with no format";
				++a_draw.colourCount;
			}
			a_draw.key.depthFormat = SceneDraws::DepthFormat(View::MenuColour);
			return a_draw.key.depthFormat ? nullptr : "no depth import";
		}

		/** @brief The draw's geometry by device address, and its index count. */
		const char* AddGeometry(const RE::BSGeometry& a_geometry, std::uint64_t a_vertexDesc, Run& a_run, Draw& a_draw, RenderGraph::BufferRef& a_vertexRef,
			RenderGraph::BufferRef& a_indexRef, std::uint32_t* a_layout, std::uint32_t& a_stride, std::uint32_t& a_firstIndex, std::string& a_detail)
		{
			if (a_geometry.GetType().get() != RE::BSGeometry::Type::kTriShape)
				return "not a BSTriShape";
			const auto* shape = reinterpret_cast<const Family::TriShape*>(a_geometry.GetGeometryRuntimeData().rendererData);
			const auto triangles = static_cast<const RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData().triangleCount;
			Family::Geometry geometry;
			const auto result = Family::LatchGeometry(shape, 0, triangles, a_vertexDesc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, geometry, a_run.owners, &a_detail);
			if (result != Family::GeometryResult::Ok)
				return Family::ReasonOf(result);
			a_vertexRef = geometry.vertexRef;
			a_indexRef = geometry.indexRef;
			a_layout[0] = static_cast<std::uint32_t>(geometry.layout);
			a_layout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
			a_stride = geometry.stride;
			a_firstIndex = geometry.firstIndex;
			a_draw.indexCount = geometry.indexCount;
			return nullptr;
		}

		/** @brief A texture slot of a material record and its sampler. A slot the record leaves unwritten reads the null view. */
		const char* AddTexture(ID3D11ShaderResourceView* a_view, std::uint32_t a_address, std::uint32_t a_filter, std::uint32_t a_slot, Run& a_run, std::uint32_t& a_index,
			std::uint32_t& a_sampler, std::string& a_detail)
		{
			auto* view = Unwrap(a_view);
			GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
			const auto slot = Family::SlotTexture(view, Kind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, a_slot), a_index, a_run.owners, &failure);
			if (slot != Family::SlotResult::Ok) {
				a_detail = GpuTextures::DescribeFailure(view, a_slot, failure);
				return "a texture it samples could not be imported";
			}
			a_sampler = Family::SlotSampler(a_address, a_filter, a_run.owners);
			return a_sampler == GpuTextures::kInvalid ? "its sampler modes have no graph sampler" : nullptr;
		}

		/** @brief The frame rows a draw reads (b12), appended once per run. */
		std::uint32_t FrameOffset(const Inputs& a_in, Run& a_run, std::uint32_t& a_cached)
		{
			if (a_cached != ~0u)
				return a_cached;
			float rows[kFrameRows][4]{};
			FrameRows(a_in, rows);
			a_cached = a_run.Append(std::as_bytes(std::span<const float[4]>(rows, kFrameRows)));
			return a_cached;
		}

		/** @brief A skinned draw's palettes, appended to the run's latch. */
		const char* AddBones(const RE::BSGeometry& a_geometry, Run& a_run, Draw& a_draw)
		{
			std::array<std::byte, kBonesBytes> bones;
			if (!BoneRows(a_geometry, bones))
				return "a skinned draw whose palettes could not be computed";
			a_draw.bonesOffset = a_run.Append(bones);
			return a_draw.bonesOffset == ~0u ? "more palettes than the batch's latch holds" : nullptr;
		}

		const char* DescribeStates(Draw& a_draw, const DrawState& a_state)
		{
			// The colour pass writes stencil bit 0 everywhere it draws (mode 2, ref 0xff) for the TAA mask fill that follows.
			constexpr std::uint32_t kStencilMode = 2, kStencilRef = 0xff;
			a_draw.stencilRef = kStencilRef;
			if (!Family::DescribeStates(a_draw.key, Pipeline::EngineBlendState(a_state.blend, 0, a_state.write, 0), Pipeline::EngineDepthStencilState(a_state.depth, kStencilMode),
					Pipeline::EngineRasterState(0, a_state.cull, 0, 0)))
				return "a depth-stencil, blend or rasterizer state with no description";
			return nullptr;
		}

		const char* AddLighting(const Inputs& a_in, const RE::BSGeometry& a_geometry, const std::array<std::int32_t, kMaxColours>& a_targets, Run& a_run,
			std::uint32_t& a_frame, std::string& a_detail)
		{
			LightingResult r;
			if (!ComputeLighting(a_in, a_geometry, r, a_detail))
				return "its Lighting constants could not be computed";
			Draw draw{};
			draw.shader = Shader::Lighting;
			constexpr auto kVertexInputs = static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC) | static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned);
			draw.key.descriptor = (r.pixel & ~kVertexInputs) | (r.vertex & kVertexInputs);
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (const char* why = MapColours(a_targets, kMaxColours, draw))
				return why;
			const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
			const auto state = StateOf(a_geometry, *property, false, a_targets[0]);
			if (const char* why = DescribeStates(draw, state))
				return why;
			auto* shader = EngineShaders::Lighting();
			auto* vs = globals::shaderCache->GetVertexShader(*shader, r.vertex);
			LightingSceneConstants c = r.constants;
			c.alphaTestRef = state.alphaRef;
			// CS's permutation block (Hooks.cpp's shader setup): the raw descriptors, the lookup's runtime bits off the pixel one.
			c.descriptors[0] = r.vertex;
			c.descriptors[1] = r.rawPixel & ~r.pixel;
			const auto permutation = PermutationOf(a_geometry, false);
			c.descriptors[2] = permutation.extra;
			c.descriptors[3] = globals::state->permutationData.ExtraFeatureDescriptor;
			c.effectRadius = a_geometry.worldBound.radius;
			c.baseTextureIsWorking = permutation.baseTextureIsWorking;
			c.renderToUI = permutation.renderToUI;
			if (const char* why = AddGeometry(a_geometry, vs->vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail))
				return why;
			auto null = GpuTextures::Get().NullOf(Kind::Texture2D);
			if (null.index == GpuTextures::kInvalid)
				return "no null view";
			c.nullTexture = null.index;
			if (null.owner)
				a_run.owners.push_back(std::move(null.owner));
			for (std::uint32_t t = 0; t < kLightingSlots; ++t) {
				const bool written = (r.record.textureWritten >> t) & 1;
				const std::uint32_t filter = r.record.filterModes[t] == kUnwrittenFilterMode ? 0u : r.record.filterModes[t];
				if (const char* why = AddTexture(written ? r.record.textures[t] : nullptr, r.record.addressModes[t], filter, t, a_run, c.textures[t], c.samplers[t], a_detail))
					return why;
			}
			bool building = false;
			draw.program = SceneDraws::LightingProgram(draw.key, *shader, &building);
			if (!draw.program)
				return building ? kBuilding : "its ORG program cannot be built";
			if (r.vertex & static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned))
				if (const char* why = AddBones(a_geometry, a_run, draw))
					return why;
			draw.frameOffset = FrameOffset(a_in, a_run, a_frame);
			draw.latchOffset = a_run.Append(std::as_bytes(std::span<const LightingSceneConstants, 1>(&c, 1)));
			if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = Family::FrameViewport(a_targets[0], static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), 0);
			draw.viewport.depthRange = { 0.0f, 1.0f };
			a_run.draws.push_back(std::move(draw));
			return nullptr;
		}

		const char* AddEffect(const Inputs& a_in, const RE::BSGeometry& a_geometry, const std::array<std::int32_t, kMaxColours>& a_targets, Run& a_run,
			std::uint32_t& a_frame, std::string& a_detail)
		{
			EffectResult r;
			if (!ComputeEffect(a_in, a_geometry, r, a_detail))
				return "its Effect constants could not be computed";
			Draw draw{};
			draw.shader = Shader::Effect;
			draw.key.descriptor = r.pixel | (r.vertex & Effects::EffectFlag::Skinned);
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (const char* why = MapColours(a_targets, kMaxColours, draw))
				return why;
			const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
			const auto state = StateOf(a_geometry, *property, true, a_targets[0]);
			if (const char* why = DescribeStates(draw, state))
				return why;
			static const REL::Relocation<RE::BSShader**> effectShader{ REL::Offset(Effects::Address::kEffectShader) };
			auto* shader = *effectShader.get();
			auto* vs = globals::shaderCache->GetVertexShader(*shader, r.vertex);
			EffectSceneConstants c = r.constants;
			c.alphaTestRef = state.alphaRef;
			c.descriptors[0] = r.vertex;
			c.descriptors[1] = r.rawPixel & ~r.pixel;
			const auto permutation = PermutationOf(a_geometry, true);
			c.descriptors[2] = permutation.extra;
			c.descriptors[3] = globals::state->permutationData.ExtraFeatureDescriptor;
			c.effectRadius = a_geometry.worldBound.radius;
			c.baseTextureIsWorking = permutation.baseTextureIsWorking;
			c.renderToUI = permutation.renderToUI;
			if (const char* why = AddGeometry(a_geometry, vs->vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail))
				return why;
			auto null = GpuTextures::Get().NullOf(Kind::Texture2D);
			if (null.index == GpuTextures::kInvalid)
				return "no null view";
			c.nullTexture = null.index;
			if (null.owner)
				a_run.owners.push_back(std::move(null.owner));
			for (std::uint32_t t = 0; t < kEffectSlots; ++t) {
				const bool written = t < kEffectSlotsRead && ((r.record.textureWritten >> t) & 1);
				const std::uint32_t filter = r.record.filterModes[t] == kUnwrittenFilterMode ? 0u : r.record.filterModes[t];
				if (const char* why = AddTexture(written ? r.record.textures[t] : nullptr, written ? r.record.addressModes[t] : 0u, written ? filter : 0u, t, a_run, c.textures[t],
						c.samplers[t], a_detail))
					return why;
			}
			bool building = false;
			draw.program = SceneDraws::EffectProgram(draw.key, *shader, &building);
			if (!draw.program)
				return building ? kBuilding : "its ORG program cannot be built";
			if (r.vertex & Effects::EffectFlag::Skinned)
				if (const char* why = AddBones(a_geometry, a_run, draw))
					return why;
			draw.frameOffset = FrameOffset(a_in, a_run, a_frame);
			draw.latchOffset = a_run.Append(std::as_bytes(std::span<const EffectSceneConstants, 1>(&c, 1)));
			if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = Family::FrameViewport(a_targets[0], static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), 0);
			draw.viewport.depthRange = { 0.0f, 1.0f };
			a_run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief Whether a bound is outside the camera's frustum: a sphere is outside a plane when n.c - d < -r (ShadowCameras::Planes). */
		bool Outside(const RE::NiBound& a_bound, const ShadowCameras::Planes& a_planes)
		{
			for (std::uint32_t i = 0; i < 6; ++i) {
				if (!(a_planes.mask & (1u << i)))
					continue;
				const auto& p = a_planes.plane[i];
				if (p[0] * a_bound.center.x + p[1] * a_bound.center.y + p[2] * a_bound.center.z - p[3] < -a_bound.radius)
					return true;
			}
			return false;
		}

		/** @brief A visible geometry and the ordered group it registers into (-1 for none). */
		struct Visible
		{
			RE::BSGeometry* geometry = nullptr;
			std::int32_t group = -1;
		};

		/**
		 * @brief Appends a_object's visible geometry, depth first, as the menu's cull keeps it: hidden subtrees and bounds outside the
		 * camera's frustum are skipped, a switch node gives its active child, and a billboard turns to the camera. A BSOrderedNode opens a group of its own
		 * (StartGroupingAlphas, AE 0x1414f50d0) in visit order; its geometry registers into the nearest one.
		 */
		void Walk(RE::NiAVObject* a_object, const RE::NiCamera& a_camera, const ShadowCameras::Planes& a_planes, std::int32_t a_group, std::vector<Visible>& a_out,
			std::vector<const RE::NiNode*>& a_groups)
		{
			if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden) || Outside(a_object->worldBound, a_planes))
				return;
			if (auto* geometry = a_object->AsGeometry()) {
				a_out.push_back({ geometry, a_group });
				return;
			}
			auto* node = a_object->AsNode();
			if (!node)
				return;
			if (auto* switchNode = netimmerse_cast<RE::NiSwitchNode*>(node)) {
				const auto index = switchNode->index;
				if (index >= 0 && static_cast<std::size_t>(index) < node->GetChildren().size())
					Walk(node->GetChildren()[static_cast<std::uint16_t>(index)].get(), a_camera, a_planes, a_group, a_out, a_groups);
				return;
			}
			static const REL::Relocation<const RE::NiRTTI*> orderedRtti{ RE::BSOrderedNode::Ni_RTTI };
			if (node->GetRTTI() == orderedRtti.get()) {
				a_group = static_cast<std::int32_t>(a_groups.size());
				a_groups.push_back(node);
			}
			// A billboard turns to the culling camera in its OnVisible (AE 71063): its rotation (SE 69681, AE 71061) turns the node and updates
			// its children's world transforms, a CPU side effect of the cull the draws need.
			if (netimmerse_cast<RE::NiBillboardNode*>(node)) {
				static const REL::Relocation<void (*)(RE::NiNode*, const RE::NiCamera*)> rotateToCamera{ RELOCATION_ID(69681, 71061) };
				rotateToCamera(node, &a_camera);
			}
			for (auto& child : node->GetChildren())
				Walk(child.get(), a_camera, a_planes, a_group, a_out, a_groups);
		}

		struct NativeState
		{
			PreviousCamera previous;
			std::uint64_t renders = 0;
			std::uint64_t building = 0;  // draws left out while their programs build
		};

		NativeState& GetNative()
		{
			static NativeState state;
			return state;
		}
	}

	/**
	 * @brief Whether a_geometry's pass has accumulation hint 1, the accumulator's blended list (FUN_1414f50b0, batch renderer +0xF0), as
	 * GetRenderPasses gives it. BSEffectShaderProperty (AE 0x1414bd890): not a decal or billboard, and the property's alpha below one,
	 * the alpha property blending, a particle system or falloff. BSLightingShaderProperty: the alpha property blends (a blended pass
	 * that also fades below alpha one gets hint 9 or 10, which Render refuses).
	 */
	bool BlendedList(const RE::BSGeometry& a_geometry)
	{
		using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
		const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
		const bool blending = alpha && alpha->GetAlphaBlending();
		if (netimmerse_cast<const RE::BSEffectShaderProperty*>(property)) {
			if (property->flags.any(Flag::kDecal, Flag::kDynamicDecal, Flag::kBillboard))
				return false;
			const bool particles = netimmerse_cast<const RE::NiParticleSystem*>(&a_geometry) != nullptr;
			return property->alpha < 1.0f || blending || particles || property->flags.any(Flag::kFalloff);
		}
		return blending && !(property && property->flags.any(Flag::kDecal, Flag::kDynamicDecal));
	}

	void SceneGeometry(RE::NiAVObject* a_root, const RE::NiCamera& a_camera, bool a_closestPoint, std::vector<RE::BSGeometry*>& a_out)
	{
		a_out.clear();
		ShadowCameras::Planes planes;
		ShadowCameras::FrustumPlanes(a_camera.GetRuntimeData2().viewFrustum, a_camera.world, planes);
		std::vector<Visible> visible;
		std::vector<const RE::NiNode*> groupNodes;
		Walk(a_root, a_camera, planes, -1, visible, groupNodes);

		// The batches first, in registration (scene) order.
		std::vector<RE::BSGeometry*> list;
		std::vector<std::vector<RE::BSGeometry*>> groups(groupNodes.size());
		for (const auto& v : visible) {
			if (!BlendedList(*v.geometry))
				a_out.push_back(v.geometry);
			else if (v.group >= 0)
				groups[static_cast<std::size_t>(v.group)].push_back(v.geometry);
			else
				list.push_back(v.geometry);
		}
		// The blended list, merge sorted far to near (a stable sort), drawn with the ordered groups (FUN_1414f5330): the last opened
		// group is the stack's top, drawn whole, in registration order, before the list's head when its bound lies farther.
		const auto key = [&](const RE::NiBound& a_bound) {
			const auto& r = a_camera.world.rotate.entry;
			const auto& c = a_bound.center;
			const float k = r[1][0] * c.y + r[0][0] * c.x + r[2][0] * c.z;
			return a_closestPoint ? k - a_bound.radius : k;
		};
		std::stable_sort(list.begin(), list.end(), [&](const RE::BSGeometry* a_a, const RE::BSGeometry* a_b) { return key(a_a->worldBound) > key(a_b->worldBound); });
		std::size_t head = 0;
		std::size_t top = groups.size();
		while (head < list.size() || top > 0) {
			if (top > 0 && (head == list.size() || key(groupNodes[top - 1]->worldBound) > key(list[head]->worldBound))) {
				--top;
				a_out.insert(a_out.end(), groups[top].begin(), groups[top].end());
			} else
				a_out.push_back(list[head++]);
		}
	}

	bool SortsByClosestPoint(const RE::UI3DSceneManager& a_manager)
	{
		// NiAlphaAccumulator::sortByClosestPoint (+0x51) of the colour pass's accumulator.
		const auto* accumulator = a_manager.unk10.get();
		return accumulator && reinterpret_cast<const std::uint8_t*>(accumulator)[0x51] != 0;
	}

	bool Owns(std::int32_t a_scheme)
	{
		// The schemes whose draws CS computes as the engine does (observe parity, draw order included): the stats menu's constellations.
		constexpr std::array<std::int32_t, 1> kNativeSchemes{ 6 };
		static const bool native = ::RenderGraph::Engine::Native::Active();
		return native && std::ranges::find(kNativeSchemes, a_scheme) != kNativeSchemes.end();
	}

	DWORD Render(RE::UI3DSceneManager* a_manager, std::int32_t a_scheme, RE::NiCamera* a_camera, bool a_keepDepth)
	{
		RE::BSSpinLockGuard locker(a_manager->lock);
		const auto scheme = static_cast<std::int32_t>(a_manager->currentlightScheme);
		static const REL::Relocation<bool (*)(void*)> uiShowing{ RELOCATION_ID(79939, 82076) };
		if ((scheme != a_scheme && a_scheme != 0) || !uiShowing(RE::UI::GetSingleton()))
			return 0;
		auto* root = a_manager->menuObjects[scheme].get();
		if (!root)
			return 0;

		// The update half (decision 16), as UI3DSceneManager::Render (AE 52727) runs it before it draws: the scheme's lights into the menu's
		// scene node, the root's update, the camera, and the scene node's light update.
		static const REL::Relocation<void (*)(RE::MenuLight*)> addLightChain{ RELOCATION_ID(51876, 52749) };
		static const REL::Relocation<void (*)(RE::ShadowSceneNode*, RE::BSLight*)> addLight{ RELOCATION_ID(99707, 106341) };
		static const REL::Relocation<void (*)(RE::ShadowSceneNode*, RE::BSCullingProcess*)> updateLights{ RELOCATION_ID(99701, 106335) };
		if (static_cast<std::uint32_t>(scheme) < a_manager->menuLights.size())
			if (auto* light = a_manager->menuLights[scheme]) {
				if (light->last)
					addLightChain(light->last);
				addLight(a_manager->shadowSceneNode, light->light.get());
			}
		RE::NiUpdateData update{};
		root->Update(update);
		auto* camera = a_camera ? a_camera : a_manager->camera.get();
		if (!camera)
			camera = RE::Main::WorldRootCamera();
		if (!camera)
			return 0;
		updateLights(RE::BSShaderManager::State::GetSingleton().shadowSceneNode[0], a_manager->cullingProcess);

		// The draw half: the colour pass from the scene graph into CS's frame.
		auto& native = GetNative();
		const auto in = SampleInputs(*a_manager, *camera, native.previous);
		std::vector<RE::BSGeometry*> geometries;
		SceneGeometry(root, *camera, SortsByClosestPoint(*a_manager), geometries);

		const bool taa = false;  // the stats menu draws without TAA (UI3DSceneManager::Render: schemes 3 and 6)
		const auto targets = ColourTargets(scheme, taa);
		// The frame's family resources start before the draws lease theirs, then kMAIN's depth and stencil clear, as at the render's start.
		Family::BeginBatch();
		if (!::RenderGraph::Engine::Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL))
			::RenderGraph::Fatal("menu scenes", "kMAIN's depth clear at a native menu scene render was not taken");
		auto run = std::make_shared<Run>();
		run->view = View::MenuColour;
		std::uint32_t frame = ~0u;
		for (auto* geometry : geometries) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			std::string detail;
			const char* why = nullptr;
			if (netimmerse_cast<const RE::BSLightingShaderProperty*>(property))
				why = BlendedList(*geometry) && property->alpha < 1.0f ? "a blended Lighting draw that fades (accumulation hint 9 or 10)" :
				                                                          AddLighting(in, *geometry, targets, *run, frame, detail);
			else if (netimmerse_cast<const RE::BSEffectShaderProperty*>(property))
				why = AddEffect(in, *geometry, targets, *run, frame, detail);
			else
				why = "a shader the native menu scene has no program for";
			// A draw whose program is still building is left out until it is built, as the engine draw families do (the manifest
			// prebuilds it next session).
			if (why == kBuilding) {
				if (native.building++ == 0)
					logger::info("[DCLF] menu scenes: scheme {} draw {} is left out while its program builds", scheme, geometry->name.c_str() ? geometry->name.c_str() : "?");
				continue;
			}
			// Decision 149: a scheme is owned only when CS computes every draw in it.
			if (why)
				::RenderGraph::Fatal("menu scenes", std::format("scheme {} draw {}: {} ({})", scheme, geometry->name.c_str() ? geometry->name.c_str() : "?", why, detail));
		}
		if (SceneDraws::Available() && SceneDraws::CanTake(View::MenuColour))
			SceneDraws::Submit(std::move(run));
		else
			SceneDraws::Keep(std::move(run->owners));
		native.previous = { true, { in.camera.viewProj, in.eye } };
		if (native.renders++ == 0)
			logger::info("[DCLF] menu scenes: the first native menu scene render (scheme {}, keep depth {}): {} geometries", scheme, a_keepDepth, geometries.size());
		return 0;
	}
}

namespace DCLF::Features::MenuViews
{
	bool NativeOwns(std::int32_t a_scheme)
	{
		return Scenes::Native::Owns(a_scheme);
	}
}
