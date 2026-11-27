// The native menu scenes' draws (roadmap phase 5, docs/development/plans/snap5-menus.md): what a menu scene's Lighting draws bind,
// computed by CS from the menu's scene graph, its scene node's lights and camera, through DCLF's ports (EvaluateTechnique,
// MaterialPort, GeometryPort) and Light Limit Fix's light rows. No engine setup function runs.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneCapture.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPalettes.h"
#include "Features/DrawcallLimitFix/Core/Engine/SkinPartitions.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Handoff/ShadowViews.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"
#include "Features/DrawcallLimitFix/Core/Shading/GeometryPort.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPort.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectDescriptors.h"
#include "Features/DrawcallLimitFix/Features/MenuViews/MenuScenes.h"
#include "Features/DrawcallLimitFix/Features/Particles/ParticleSnapshots.h"
#include "Features/DrawcallLimitFix/Features/ShadowMask/ShadowMask.h"

#include "EngineFixes/LandscapeSeamsFix.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "RenderGraph/Engine/ImageSpace/ImageSpace.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Skeleton/Skeleton.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "Snapshot/CoreFrame.h"
#include "State.h"
#include "TruePBR/BSLightingShaderMaterialPBRLandscape.h"
#include "Utils/Logging.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <vector>

namespace DCLF::Features::MenuViews::Scenes::Native
{
	namespace
	{
		/** @brief A draw's lights as GetRenderPasses gives a menu pass: the scene node's sun, its shadow lights, then its point lights. */
		struct Lights
		{
			std::array<RE::BSLight*, 16> lights{};
			std::uint32_t count = 0;
			std::uint32_t shadow = 0;
		};

		/** @brief A Lighting draw's constants, its descriptors and material record. */
		struct LightingResult
		{
			LightingSceneConstants constants{};
			std::uint32_t pass = 0, vertex = 0, pixel = 0, rawPixel = 0;
			MaterialRecord record{};
			Lights lights;
		};

		/** @brief An Effect draw's constants, descriptors and material record. */
		struct EffectResult
		{
			EffectSceneConstants constants{};
			std::uint32_t pass = 0, vertex = 0, pixel = 0, rawPixel = 0;
			MaterialRecord record{};
		};

		/** @brief CS's permutation block for a draw: the extra shader descriptor and Linear Lighting's two words, as CS's hooks set them. */
		struct Permutation
		{
			std::uint32_t extra = 0, baseTextureIsWorking = 0, renderToUI = 0;
		};

		/** @brief A Utility draw as BSUtilityShader's Setup functions would bind it: its rows, technique, states, t0. */
		struct UtilityResult
		{
			UtilitySceneConstants constants{};
			std::uint32_t technique = 0, vertex = 0;
			DrawState state;
			const RE::NiSourceTexture* texture = nullptr;  // t0, the diffuse an alpha-tested draw samples
			std::uint32_t address = 0, filter = 0;
		};

		/** @brief The engine's Lighting shader object. Null before the engine makes it. */
		RE::BSShader* LightingShader()
		{
			static const REL::Relocation<RE::BSShader**> shader{ REL::Offset(Engine::Address::kLightingShader) };
			return *shader.get();
		}

		/** @brief The engine's Effect shader object. Null before the engine makes it. */
		RE::BSShader* EffectShader()
		{
			static const REL::Relocation<RE::BSShader**> shader{ REL::Offset(Effects::Address::kEffectShader) };
			return *shader.get();
		}

		/** @brief The FrameBuffer rows after the camera's: the frame's ratios, and row 42's inverse gamma. */
		void FrameTailRows(float (*a_out)[4])
		{
			Snapshot::FrameCamera::WriteRatios({}, Snapshot::Core().drClampOffset, &a_out[0][0]);
			const float params[4]{ Snapshot::Core().inverseGamma, 1.0f, 0.0f, 0.0f };
			std::memcpy(a_out[42], params, sizeof(params));
		}

		/** @brief Packs one constant group into rows through a shader's constant table. */
		void Pack(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_group, std::uint32_t a_first,
			float (*a_rows)[4], std::uint32_t a_count)
		{
			std::array<std::byte, 1024> bytes{};
			PackConstantGroup(a_block, a_layout, a_table, a_group, a_first, bytes);
			std::memcpy(a_rows, bytes.data(), a_count * 16);
		}

		/** @brief Pack, and marks the floats it wrote in a_written: the same group of a block that holds 1 where a_block is written. */
		void PackWritten(const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::uint8_t> a_table, std::uint64_t a_group, std::uint32_t a_first,
			float (*a_rows)[4], float (*a_written)[4], std::uint32_t a_count)
		{
			Pack(a_block, a_layout, a_table, a_group, a_first, a_rows, a_count);
			ConstantBlock mask;
			mask.Reset();
			for (std::uint32_t f = 0; f < kConstantBlockFloats; ++f)
				if (a_block.Written(f))
					mask.floats[f] = 1.0f;
			Pack(mask, a_layout, a_table, a_group, a_first, a_written, a_count);
		}

		/**
		 * @brief SetupGeometry's World with Community Shaders' world-space patch: rotation times scale, the translation less a_eye. A
		 * particle system's model centre a_centre is folded into the translation, as ParticleSnapshots' placement does.
		 */
		void WorldRows(const RE::NiTransform& a_world, const RE::NiPoint3& a_eye, float* a_out, const RE::NiPoint3& a_centre = {})
		{
			const auto& r = a_world.rotate.entry;
			const float s = a_world.scale;
			const float c[3]{ a_centre.x, a_centre.y, a_centre.z };
			const float t[3]{ a_world.translate.x, a_world.translate.y, a_world.translate.z };
			const float e[3]{ a_eye.x, a_eye.y, a_eye.z };
			for (int i = 0; i < 3; ++i) {
				a_out[i * 4 + 0] = r[i][0] * s;
				a_out[i * 4 + 1] = r[i][1] * s;
				a_out[i * 4 + 2] = r[i][2] * s;
				const float folded = c[0] == 0.0f && c[1] == 0.0f && c[2] == 0.0f ? t[i] : (r[i][0] * c[0] + r[i][1] * c[1] + r[i][2] * c[2]) * s + t[i];
				a_out[i * 4 + 3] = folded - e[i];
			}
		}

		/** @brief The alpha test reference the batch renderer sets for an alpha property: its threshold times the exe's 1/255 (0x3B808081). */
		float AlphaRefOf(const RE::NiAlphaProperty& a_alpha)
		{
			return static_cast<float>(a_alpha.alphaThreshold) * std::bit_cast<float>(0x3B808081u);
		}

		/**
		 * @brief The alpha test reference with the exe's bias: the threshold times 1/255 plus a bias just under it (the constants at
		 * AE 0x141ad2810 and 0x141abfc44); a threshold of 4 adds 1/255 more.
		 */
		float BiasedAlphaRefOf(const RE::NiAlphaProperty& a_alpha)
		{
			constexpr float kStep = std::bit_cast<float>(0x3B808081u), kBias = std::bit_cast<float>(0x3B80802Cu);
			float ref = static_cast<float>(a_alpha.alphaThreshold) * kStep + kBias;
			if (a_alpha.alphaThreshold == 4)
				ref += kStep;
			return ref;
		}

		/** @brief Whether a_geometry is a particle system drawn as camera-facing quads (not strips). */
		bool IsParticles(const RE::BSGeometry& a_geometry, std::uint32_t a_pass)
		{
			namespace EF = Effects::EffectFlag;
			return (a_pass & EF::Particles) && !(a_pass & EF::StripParticles) && netimmerse_cast<const RE::NiParticleSystem*>(&a_geometry) != nullptr;
		}

		/** @brief A light's LOD dimmer: a shadow light's as MistMenu's shadow render sets it (Inputs::shadowDimmers), else the light's own. */
		float DimmerOf(const Inputs& a_in, const RE::BSLight& a_light)
		{
			for (const auto& [light, dimmer] : a_in.shadowDimmers)
				if (light == &a_light)
					return dimmer;
			return a_light.lodDimmer;
		}

		/** @brief The point light rows (c15, c22, c29.x) as the latch writes Light Limit Fix's strict lights for a pass outside the world. */
		void PointLightRows(const Inputs& a_in, const Lights& a_lights, const RE::NiPoint3& a_eye, LightingSceneConstants& a_out)
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
				light.fade *= DimmerOf(a_in, *bsLight);
				if (light.lightFlags.any(LightLimitFix::LightFlags::Disabled))
					continue;
				const auto position = niLight->world.translate - a_eye;
				const float row0[4]{ position.x, position.y, position.z, light.radius };
				const float row1[4]{ light.color.x * light.fade, light.color.y * light.fade, light.color.z * light.fade, 0.0f };
				std::memcpy(a_out.psGeometry[kPositions + count], row0, 16);
				std::memcpy(a_out.psGeometry[kColours + count], row1, 16);
				++count;
			}
			a_out.psGeometry[kCount][0] = static_cast<float>(count);
		}
	}

	/**
	 * @brief The scene node's shadow lights' dimmers as MistMenu's shadow render leaves them (scheme 4 with iLoadingMenuShadowLightFlags set,
	 * FUN_140973210): for each active shadow light in order, until four were visible, the camera update (vtable +0x80, AE 0x14151b620) sets
	 * it to 1 when the light's bound sphere is in the camera's frustum and, for a light that fades by distance, (d^2 - r^2) times the
	 * camera's LOD adjust is below the shadow fade distance (SE 528316, AE 415264); else to 0. CS draws no shadow map for an owned render;
	 * only the dimmer, which the light rows read, is the render's.
	 */
	std::vector<std::pair<const RE::BSLight*, float>> ShadowLightDimmers(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera)
	{
		std::vector<std::pair<const RE::BSLight*, float>> out;
		static const REL::Relocation<const std::int32_t*> shadowFlags{ RELOCATION_ID(510351, 383327) };
		static const REL::Relocation<const float*> fadeDistance{ RELOCATION_ID(528316, 415264) };
		if (a_manager.currentlightScheme != RE::INTERFACE_LIGHT_SCHEME::kLoading || *shadowFlags.get() == 0 || !a_manager.shadowSceneNode)
			return out;
		ShadowCameras::Planes planes;
		const auto& camera = a_camera.GetRuntimeData2();
		ShadowCameras::FrustumPlanes(camera.viewFrustum, a_camera.world, planes);
		std::uint32_t visible = 0;
		for (const auto& light : a_manager.shadowSceneNode->GetRuntimeData().activeShadowLights) {
			if (!light || !light->light || visible >= kMaxShadowSlices)
				continue;
			const auto* niLight = light->light.get();
			const float radius = niLight->GetLightRuntimeData().radius.x;
			bool inside = true;
			for (std::uint32_t i = 0; i < 6 && inside; ++i) {
				const auto& p = planes.plane[i];
				const auto& c = niLight->world.translate;
				inside = !(planes.mask & (1u << i)) || p[0] * c.x + p[1] * c.y + p[2] * c.z - p[3] >= -radius;
			}
			float dimmer = inside ? 1.0f : 0.0f;
			if (inside && light->lodFade) {
				const auto d = niLight->world.translate - a_camera.world.translate;
				if (*fadeDistance.get() <= ((d.x * d.x + d.y * d.y + d.z * d.z) - radius * radius) * camera.lodAdjust)
					dimmer = 0.0f;
			}
			visible += dimmer != 0.0f ? 1u : 0u;
			out.emplace_back(light.get(), dimmer);
		}
		return out;
	}

	Inputs SampleInputs(const RE::UI3DSceneManager& a_manager, const RE::NiCamera& a_camera, const PreviousCamera& a_previous, bool a_shadowMaps)
	{
		// DirLightColor scales by the ImageSpace data's HDR multiplier. MistMenu's scenes (main menu, loading model) have no world update
		// behind them, so the engine's draws read the override's (manager +0xB0), which its last render copied into the current data
		// (AE 0x141482ba0). Over a world, the game's blend replaces the current data each update.
		const auto* imageSpace = reinterpret_cast<const std::byte*>(RE::ImageSpaceManager::GetSingleton());
		const auto* override = imageSpace && a_manager.currentlightScheme == RE::INTERFACE_LIGHT_SCHEME::kLoading ?
		                           *reinterpret_cast<const float* const*>(imageSpace + 0xB0) :
		                           nullptr;
		auto in = SampleSceneInputs(a_manager.shadowSceneNode, a_camera, a_previous, override ? &override[6] : nullptr);
		in.shadowMaps = a_shadowMaps;
		in.shadowDimmers = ShadowLightDimmers(a_manager, a_camera);
		return in;
	}

	Inputs SampleSceneInputs(const RE::ShadowSceneNode* a_sceneNode, const RE::NiCamera& a_camera, const PreviousCamera& a_previous, const float* a_hdrOverride)
	{
		Inputs in;
		in.eye = a_camera.world.translate;
		in.sceneNode = a_sceneNode;
		in.technique = SampleTechniqueInputs();
		// The engine's render makes the menu's scene node the current scene graph for its draws; SetupTechnique's fog is that node's.
		SampleFog(in.sceneNode, in.technique);
		in.pipeline = GeometryPort::SamplePipelineFrame(in.sceneNode ? in.sceneNode->GetRuntimeData().sunLight : nullptr, true, a_hdrOverride);
		// The material port reads the engine's Lighting shader object, which DCLF captures only in a world frame.
		if (!EngineShaders::Lighting())
			if (auto* lighting = LightingShader(); lighting && lighting->shaderType.get() == RE::BSShader::Type::Lighting)
				EngineShaders::SetLighting(lighting);
		in.material = MaterialPort::SampleFrame();
		Snapshot::FrameCamera::Capture(a_camera, in.camera);
		// The previous camera is the menu scene's last one; the first render has none and takes its own, as the engine's first does not.
		in.previous = a_previous.valid ? a_previous.camera : Snapshot::FrameCamera::Previous{ in.camera.viewProj, in.eye };
		in.havePrevious = a_previous.valid;
		in.previousEye = a_previous.valid ? a_previous.camera.eye : RE::NiPoint3{};
		return in;
	}

	namespace
	{
		/** @brief A draw's lights as GetRenderPasses gives a menu pass: the scene node's sun, its shadow lights, then its point lights. */
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
			// The local map's land passes take no other light (their point light rows stay empty).
			if (a_in.localMap)
				return;
			for (const auto& light : data.activeShadowLights)
				if (light && light->light && lights(*light)) {
					push(light.get());
					++a_out.shadow;
				}
			for (const auto& light : data.activeLights)
				if (light && light->light && lights(*light))
					push(light.get());
		}
	}

	void FrameRows(const Inputs& a_in, float (*a_out)[4])
	{
		std::memset(a_out, 0, kFrameBytes);
		Snapshot::FrameCamera::WriteCamera(a_in.camera.view, a_in.camera.proj, a_in.camera.viewProj, a_in.eye, { 0.0f, 0.0f }, a_in.localMap ? nullptr : &a_in.previous, &a_out[0][0]);
		FrameTailRows(a_out);
	}

	namespace
	{
		/** @brief The Lighting draw of a_geometry as SetupTechnique, SetupMaterial and SetupGeometry would bind it. False, with why, when it cannot. */
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
				// Without shadow maps a shadow light is a plain light of the pass (GetRenderPasses reads the colour accumulator's +0x178). The raw
				// pixel descriptor takes the same bits: DefShadow reaches the shader through CS's permutation block.
				if (a_out.lights.shadow && a_in.shadowMaps) {
					const std::uint32_t bits = (1u << 14) | ((a_out.lights.shadow & 7u) << 6);
					a_out.pass |= bits;
					a_out.rawPixel |= bits;
				}
			}
			LightingShaderDescriptors(a_out.pass, false, a_out.vertex, a_out.pixel);
			// The local map's land passes as the engine draws them: True PBR's glint bit from its material (its GetRenderPasses hook,
			// TruePBR.cpp) and Landscape Seams' bit on a blended quad (LandscapeSeamsFix.cpp). The ORG Lighting build compiles both features
			// out, so the bits only name the program.
			if (a_in.localMap && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kMultiTextureLandscape)) {
				using LF = SIE::ShaderCache::LightingShaderFlags;
				const auto seams = static_cast<std::uint32_t>(LF::LandscapeSeams), glint = static_cast<std::uint32_t>(LF::AnisoLighting);
				const bool blended = LandscapeSeamsFix::GetInstance().IsBlended(const_cast<RE::BSGeometry*>(&a_geometry));
				const bool glints = (a_out.pass & static_cast<std::uint32_t>(LF::TruePbr)) && static_cast<const BSLightingShaderMaterialPBRLandscape*>(property->material)->HasGlint();
				for (auto* d : { &a_out.pass, &a_out.pixel, &a_out.rawPixel })
					*d = (*d & ~(seams | glint)) | (blended ? seams : 0u) | (glints ? glint : 0u);
			}

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
			const auto& vsLayout = LightingVSLayout();
			const auto& psLayout = LightingPSLayout();

			// PerTechnique (SetupTechnique).
			TechniqueConstants technique{};
			EvaluateTechnique(a_out.pass, a_in.technique, technique);
			Pack(technique.vs, vsLayout, vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique], c.vsTechnique, kLightingVSTechniqueRows);
			Pack(technique.ps, psLayout, psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique], c.psTechnique, kLightingPSTechniqueRows);

			// PerMaterial (SetupMaterial, with Community Shaders' hooks as the port's contributors, Linear Lighting's SpecularColor among them).
			MaterialPort::MaterialSnapshot snapshot;
			if (!MaterialPort::Capture(*property->material, snapshot) || !MaterialPort::Evaluate(snapshot, a_out.pass, a_in.material, a_out.record)) {
				a_why = "the material port does not cover its material";
				return false;
			}
			Pack(a_out.record.vs, vsLayout, vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial], c.vsMaterial, kLightingVSMaterialRows);
			Pack(a_out.record.ps, psLayout, psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial], c.psMaterial, kLightingPSMaterialRows);

			// PerGeometry (SetupGeometry): the pipeline's frame values from the menu's scene node, then the object's.
			GeometryConstants geometry{};
			if (!GeometryPort::PipelineGeometryConstants(a_out.pass, GeometryPort::kMainPassRenderFlags, a_in.pipeline, geometry)) {
				a_why = "the menu's scene node has no sun light";
				return false;
			}
			WorldRows(a_geometry.world, a_in.eye, &geometry.vs.floats[vsLayout.offset[kVSWorld]]);
			WorldRows(a_geometry.previousWorld, a_in.localMap ? RE::NiPoint3{} : a_in.havePrevious ? a_in.previousEye :
																									 a_in.eye,
				&geometry.vs.floats[vsLayout.offset[kVSPreviousWorld]]);
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
			Pack(geometry.vs, vsLayout, vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry], c.vsGeometry, kLightingVSGeometryRows);
			Pack(geometry.ps, psLayout, psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry], c.psGeometry, kLightingPSGeometryRows);
			// The point light rows replace what the pipeline block left there.
			for (std::uint32_t r = 15; r < 29; ++r)
				for (std::uint32_t i = 0; i < 4; ++i)
					c.psGeometry[r][i] = 0.0f;
			PointLightRows(a_in, a_out.lights, a_in.eye, c);
			if (a_in.shadowMaps && a_out.lights.shadow) {
				// The point light routine (AE 0x1414df650) zeroes ShadowLightMaskSelect, then gives each shadow light its mask channel.
				ConstantBlock select;
				select.Reset();
				float* channels = &select.floats[psLayout.offset[kPSShadowLightMaskSelect]];
				for (std::uint32_t i = 0; i < 4; ++i)
					channels[i] = 0.0f;
				for (std::uint32_t i = 0; i < a_out.lights.shadow && i < 4; ++i)
					channels[i] = static_cast<float>(static_cast<const RE::BSShadowLight*>(a_out.lights.lights[1 + i])->GetRuntimeData().maskIndex);
				float rows[kLightingPSGeometryRows][4]{}, written[kLightingPSGeometryRows][4]{};
				PackWritten(select, psLayout, psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry], rows, written, kLightingPSGeometryRows);
				for (std::uint32_t r = 0; r < kLightingPSGeometryRows; ++r)
					for (std::uint32_t i = 0; i < 4; ++i)
						if (written[r][i] != 0.0f)
							c.psGeometry[r][i] = rows[r][i];
			}
			// The local map's land draws find the graphics state's default height map in t6 and t14 (observed in every one; the ORG build reads
			// neither: no parallax layer, no shadows).
			if (a_in.localMap) {
				const auto* height = RE::BSGraphics::State::GetSingleton()->GetRuntimeData().defaultHeightMap.get();
				for (const std::uint32_t slot : { 6u, 14u })
					if (!((a_out.record.textureWritten >> slot) & 1)) {
						a_out.record.textures[slot] = MaterialPort::ViewOf(height);
						a_out.record.addressModes[slot] = 3;
						a_out.record.filterModes[slot] = 3;
						a_out.record.textureWritten |= 1u << slot;
					}
			}
			// A pass with shadow bits reads the shadow mask in t14 (SetupTechnique): clamped, with the technique's filter.
			if (technique.shadowMask && technique.shadowMaskTexture) {
				a_out.record.textures[kShadowMaskSlot] = technique.shadowMaskTexture;
				a_out.record.addressModes[kShadowMaskSlot] = 0;
				a_out.record.filterModes[kShadowMaskSlot] = technique.filterModes[kShadowMaskSlot];
				a_out.record.textureWritten |= 1u << kShadowMaskSlot;
			}
			return true;
		}

		/** @brief CS's permutation block for a_geometry's draw. */
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

		/**
		 * @brief A skinned geometry's palettes as VS b9 (previous) and b10 (current) hold them, after the engine's per-frame palette update.
		 * False without skin or with more bones than the palettes hold.
		 */
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

		/** @brief An Effect constant variable's floats in a block. */
		float* Variable(ConstantBlock& a_block, const StageLayout& a_layout, std::uint32_t a_variable)
		{
			return &a_block.floats[a_layout.offset[a_variable]];
		}

		/** @brief BSEffectShader::SetupTechnique's rows (AE 0x1414ea2c0): the scene graph's fog, as Lighting's, and FilteringParam. */
		void EffectTechnique(const Inputs& a_in, std::uint32_t a_pass, const StageLayout& a_vsLayout, const StageLayout& a_psLayout,
			std::span<const std::uint8_t> a_vsTable, std::span<const std::uint8_t> a_psTable, EffectSceneConstants& a_c)
		{
			ConstantBlock vs0, ps0;
			vs0.Reset();
			ps0.Reset();
			if (a_in.technique.fog) {
				float* param = Variable(vs0, a_vsLayout, Effects::kEffectVSFogParam);
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
				float* nearColour = Variable(vs0, a_vsLayout, Effects::kEffectVSFogNearColor);
				for (std::uint32_t i = 0; i < 3; ++i)
					nearColour[i] = a_in.technique.fogNearColor[i];
				nearColour[3] = a_in.technique.invFrameBufferRange;
				float* farColour = Variable(vs0, a_vsLayout, Effects::kEffectVSFogFarColor);
				for (std::uint32_t i = 0; i < 3; ++i)
					farColour[i] = a_in.technique.fogFarColor[i];
			}
			{
				static const REL::Relocation<const float*> filterX{ RELOCATION_ID(513745, 391710) }, filterY{ RELOCATION_ID(513747, 391713) };
				float* filter = Variable(ps0, a_psLayout, Effects::kEffectPSFilteringParam);
				filter[0] = *filterX.get();
				filter[1] = *filterY.get();
			}
			// A Soft pass: CameraDataEffect.yz, the current camera's near and far - near (the menu camera during the render), and VPOSOffset,
			// the inverse render size.
			if (a_pass & Effects::EffectFlag::Soft) {
				float* camera = Variable(ps0, a_psLayout, Effects::kEffectPSCameraData);
				camera[1] = a_in.camera.frustum.fNear;
				camera[2] = a_in.camera.frustum.fFar - a_in.camera.frustum.fNear;
				static const REL::Relocation<const std::uint32_t*> width{ RELOCATION_ID(524972, 411453) }, height{ RELOCATION_ID(524973, 411454) };
				float* offset = Variable(ps0, a_psLayout, Effects::kEffectPSVPOSOffset);
				offset[0] = 1.0f / static_cast<float>(*width.get());
				offset[1] = 1.0f / static_cast<float>(*height.get());
				offset[2] = 0.0f;
				offset[3] = 0.0f;
			}
			Pack(vs0, a_vsLayout, a_vsTable, Effects::kEffectVSGroups[0], Effects::kEffectVSFirstVariable[0], a_c.vsTechnique, kEffectVSTechniqueRows);
			Pack(ps0, a_psLayout, a_psTable, Effects::kEffectPSGroups[0], Effects::kEffectPSFirstVariable[0], a_c.psTechnique, kEffectPSTechniqueRows);
		}

		/** @brief The Effect shader's vertex and pixel shaders for a_out's descriptors, and their constant tables. False, with why, without them. */
		bool EffectShaders(const EffectResult& a_out, std::span<const std::uint8_t>& a_vsTable, std::span<const std::uint8_t>& a_psTable, std::string& a_why)
		{
			auto* shader = EffectShader();
			auto* vs = shader ? globals::shaderCache->GetVertexShader(*shader, a_out.vertex) : nullptr;
			auto* ps = shader ? globals::shaderCache->GetPixelShader(*shader, a_out.pixel) : nullptr;
			if (!vs || !ps) {
				a_why = std::format("no shader for vs {:#x} ps {:#x}", a_out.vertex, a_out.pixel);
				return false;
			}
			a_vsTable = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(vs->constantTable.data()), vs->constantTable.size());
			a_psTable = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(ps->constantTable.data()), ps->constantTable.size());
			return true;
		}

		/**
		 * @brief A lit Effect pass's light rows (SetupGeometry, AE 0x1414eab10): DLightColor from light 0 (the HDR multiplier times its fade
		 * times its colour at +0x14c), then up to four point lights' PLightPosition (in model space, or less the eye for a skinned pass),
		 * PLightingRadiusInverseSquared (of the radius over the world scale, unskinned) and PLightColor (LOD dimmer times fade times
		 * diffuse); the rest zero. Linear Lighting's EffectGeometryUpload converts the colours.
		 */
		void EffectLightRows(const Inputs& a_in, const RE::BSGeometry& a_geometry, std::uint32_t a_pass, bool a_linear, ConstantBlock& a_block, const StageLayout& a_layout)
		{
			Lights lights;
			SelectLights(a_in, a_geometry, lights);
			if (!lights.count || !lights.lights[0] || !lights.lights[0]->light)
				return;
			auto& linear = globals::features::linearLighting;
			{
				const auto* light = lights.lights[0]->light.get();
				const auto* bytes = reinterpret_cast<const std::byte*>(light);
				const auto read = [](const std::byte* a_from, std::size_t a_offset) {
					float value = 0.0f;
					std::memcpy(&value, a_from + a_offset, sizeof(value));
					return value;
				};
				float* colour = Variable(a_block, a_layout, Effects::kEffectPSDLightColor);
				const auto* directional = netimmerse_cast<const RE::NiDirectionalLight*>(light);
				if (directional && (a_linear || linear.IsSunlightOverridden(directional))) {
					const auto rgb = linear.LightColorToWorking(directional, true) * (directional->GetLightRuntimeData().fade * linear.GetSunlightScale(directional));
					colour[0] = rgb.red;
					colour[1] = rgb.green;
					colour[2] = rgb.blue;
				} else {
					const auto* imageSpace = reinterpret_cast<const std::byte*>(RE::ImageSpaceManager::GetSingleton());
					const float scale = (imageSpace ? read(imageSpace, 0xe0) : 1.0f) * read(bytes, 0x134);
					colour[0] = scale * read(bytes, 0x14c);
					colour[1] = scale * read(bytes, 0x150);
					colour[2] = scale * read(bytes, 0x154);
				}
			}
			// The inverse world (AE 0x140e19a20), then D3DXVec3TransformCoord with it.
			const auto& t = a_geometry.world;
			const auto& m = t.rotate.entry;
			const float inverse = 1.0f / t.scale;
			const float rows[3][3]{ { inverse * m[0][0], m[0][1] * inverse, inverse * m[0][2] }, { inverse * m[1][0], m[1][1] * inverse, inverse * m[1][2] },
				{ inverse * m[2][0], inverse * m[2][1], inverse * m[2][2] } };
			const float translation[3]{ -((t.translate.y * m[1][0] + m[0][0] * t.translate.x + t.translate.z * m[2][0]) * inverse),
				-((m[0][1] * t.translate.x + m[1][1] * t.translate.y + t.translate.z * m[2][1]) * inverse),
				-((m[0][2] * t.translate.x + m[1][2] * t.translate.y + t.translate.z * m[2][2]) * inverse) };
			const bool skinned = a_pass & Effects::EffectFlag::Skinned;
			const std::uint32_t count = (std::min)(lights.count, 5u);
			for (std::uint32_t i = 1; i < 5; ++i) {
				float position[3]{}, radius = 0.0f, rgb[3]{};
				const auto* bsLight = i < count ? lights.lights[i] : nullptr;
				const auto* light = bsLight ? bsLight->light.get() : nullptr;
				if (light) {
					const auto& data = light->GetLightRuntimeData();
					const float dimmer = DimmerOf(a_in, *bsLight);
					if (a_linear) {
						const auto colour = linear.LightColorToWorking(light) * (data.fade * dimmer);
						rgb[0] = colour.red;
						rgb[1] = colour.green;
						rgb[2] = colour.blue;
					} else {
						const float scale = dimmer * data.fade;
						rgb[0] = scale * data.diffuse.red;
						rgb[1] = scale * data.diffuse.green;
						rgb[2] = scale * data.diffuse.blue;
					}
					const auto& p = light->world.translate;
					if (!skinned) {
						for (int c = 0; c < 3; ++c)
							position[c] = p.x * rows[0][c] + p.y * rows[1][c] + p.z * rows[2][c] + translation[c];
						radius = data.radius.x / t.scale;
					} else {
						position[0] = p.x - a_in.eye.x;
						position[1] = p.y - a_in.eye.y;
						position[2] = p.z - a_in.eye.z;
						radius = data.radius.x;
					}
					radius = 1.0f / (radius * radius);
				}
				const float values[7]{ position[0], position[1], position[2], radius, rgb[0], rgb[1], rgb[2] };
				for (std::uint32_t v = 0; v < 7; ++v)
					Variable(a_block, a_layout, Effects::kEffectPSPLightPositionX + v)[i - 1] = values[v];
			}
		}

		/** @brief The Effect draw of a_geometry as BSEffectShader's Setup functions would bind it. The lit, membrane, soft and projected variants are refused. */
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
			// A particle system's quads are expanded by CS (ParticleSnapshots::ExpandNow); their subtexture rects are resolved there. A skinned
			// draw's bones are its palettes (BoneRows).
			const bool particles = IsParticles(a_geometry, a_out.pass);
			constexpr std::uint32_t kUnported = EF::Particles | EF::StripParticles | EF::Membrane | EF::ProjectedUv | EF::TexCoordIndex;
			if (a_out.pass & kUnported & ~(particles ? EF::Particles | EF::TexCoordIndex : 0u)) {
				a_why = std::format("an Effect variant the native menu scene does not draw (pass {:#x})", a_out.pass);
				return false;
			}
			std::span<const std::uint8_t> vsTable, psTable;
			if (!EffectShaders(a_out, vsTable, psTable, a_why))
				return false;
			const auto& space = Extensions::MaterialSpaceOf(Effects::kMaterialKindEffect);
			const auto& vsLayout = space.vs.layout;
			const auto& psLayout = space.ps.layout;
			auto& c = a_out.constants;
			EffectTechnique(a_in, a_out.pass, vsLayout, psLayout, vsTable, psTable, c);

			// PerMaterial (the Effect material port, which converts BaseColor into Linear Lighting's working space).
			MaterialPort::MaterialSnapshot snapshot;
			if (!MaterialPort::Capture(*property->material, snapshot) || snapshot.port != Effects::kMaterialKindEffect ||
				!MaterialPort::Evaluate(snapshot, a_out.pass, a_in.material, a_out.record)) {
				a_why = "the Effect material port does not cover its material";
				return false;
			}
			auto& linear = globals::features::linearLighting;
			const bool linearOn = linear.loaded && linear.IsLinearLightingActive();
			// A Soft pass's LightingInfluence.y is far * near / softFalloffDepth (SetupMaterial, with the camera SetupTechnique took); the port
			// keeps the depth itself for DCLF's shader, which divides.
			if (a_out.pass & EF::Soft) {
				const auto& camera = a_in.camera.frustum;
				const float depth = static_cast<const RE::BSEffectShaderMaterial*>(property->material)->softFalloffDepth;
				a_out.record.ps.floats[psLayout.offset[Effects::kEffectPSLightingInfluence] + 1] = camera.fFar * camera.fNear / depth;
			}
			Pack(a_out.record.vs, vsLayout, vsTable, Effects::kEffectVSGroups[1], Effects::kEffectVSFirstVariable[1], c.vsMaterial, kEffectVSMaterialRows);
			Pack(a_out.record.ps, psLayout, psTable, Effects::kEffectPSGroups[1], Effects::kEffectPSFirstVariable[1], c.psMaterial, kEffectPSMaterialRows);

			// PerGeometry (SetupGeometry, AE 0x1414eab10): World and PreviousWorld less posAdjust, and PropertyColor.
			ConstantBlock vs2, ps2;
			vs2.Reset();
			ps2.Reset();
			// A particle system's World folds its model bound's centre into the translation (SetupGeometry, AE 0x1414eab10). A skinned pass
			// writes no World: its palettes place it.
			if (!(a_out.pass & EF::Skinned)) {
				const auto centre = particles ? a_geometry.GetModelData().modelBound.center : RE::NiPoint3{};
				WorldRows(a_geometry.world, a_in.eye, Variable(vs2, vsLayout, Effects::kEffectVSWorld), centre);
				// A particle system's PreviousWorld is its World (SetupGeometry copies the rows).
				if (particles)
					WorldRows(a_geometry.world, a_in.eye, Variable(vs2, vsLayout, Effects::kEffectVSPreviousWorld), centre);
				else
					WorldRows(a_geometry.previousWorld, a_in.havePrevious ? a_in.previousEye : a_in.eye, Variable(vs2, vsLayout, Effects::kEffectVSPreviousWorld), centre);
			}
			{
				float* colour = Variable(ps2, psLayout, Effects::kEffectPSPropertyColor);
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
			if (a_out.pass & EF::Lighting)
				EffectLightRows(a_in, a_geometry, a_out.pass, linearOn, ps2, psLayout);
			// Weapon blood's AlphaTestRef (SetupGeometry's Blood branch): the alpha property's threshold times the exe's 1/255, then the two
			// globals BSTempEffectWeaponBlood writes. w is left as the last draw wrote it; the shader does not read it.
			if (a_out.pass & EF::Blood) {
				static const REL::Relocation<const float*> blood{ REL::Offset(Effects::Address::kEffectBlood) };
				const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
				float* ref = Variable(ps2, psLayout, Effects::kEffectPSAlphaTestRef);
				ref[0] = alpha ? AlphaRefOf(*alpha) : 0.0f;
				ref[1] = blood.get()[0];
				ref[2] = blood.get()[1];
			}
			Pack(vs2, vsLayout, vsTable, Effects::kEffectVSGroups[2], Effects::kEffectVSFirstVariable[2], c.vsGeometry, kEffectVSGeometryRows);
			Pack(ps2, psLayout, psTable, Effects::kEffectPSGroups[2], Effects::kEffectPSFirstVariable[2], c.psGeometry, kEffectPSGeometryRows);
			return true;
		}

		/**
		 * @brief The membrane pass descriptor BSLightingShaderProperty::GetRenderPasses (AE 0x1414adfb0) adds for a Lighting property with
		 * effect data (an enchantment glow), or 0 when it adds none.
		 */
		std::uint32_t MembranePassOf(const RE::BSGeometry& a_geometry)
		{
			namespace EF = Effects::EffectFlag;
			const auto* property = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
			// Property flag bit 17 (refraction) adds none.
			const std::uint64_t flags = property ? property->flags.underlying() : 0;
			if (!property || !property->effectData || (flags & (1ull << 17)))
				return 0;
			const auto& data = *property->effectData;
			std::uint32_t d = EF::Membrane | EF::Texture | EF::TexCoord;
			if (flags & (1ull << 37))  // vertex colours
				d |= EF::Vc;
			if (flags & (1ull << 1))  // skinned
				d |= EF::Skinned;
			if (!(flags & (1ull << 12)))  // not model-space normals
				d |= EF::Normals | EF::BinormalTangent;
			if (data.grayscaleToColor)
				d |= EF::GrayscaleToColor;
			if (data.grayscaleToAlpha)
				d |= EF::GrayscaleToAlpha;
			if (data.ignoreTextureAlpha)
				d |= EF::IgnoreTexAlpha;
			if (data.baseTextureProjectedUVs)
				d |= EF::ProjectedUv;
			const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
			if (!data.ignoreBaseGeomTexAlpha && alpha && alpha->GetAlphaTesting())
				d |= EF::AlphaTest;
			if (data.lighting)
				d |= EF::Lighting;
			if (data.destBlend == RE::NiAlphaProperty::AlphaFunction::kOne)
				d |= EF::AddBlend;
			return d;
		}

		/** @brief The membrane pass of a_geometry as BSEffectShader's Setup functions would bind it (the material is the Lighting property's). */
		bool ComputeMembrane(const Inputs& a_in, const RE::BSGeometry& a_geometry, EffectResult& a_out, std::string& a_why)
		{
			namespace EF = Effects::EffectFlag;
			a_out = {};
			const auto* property = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
			const std::uint32_t d = MembranePassOf(a_geometry);
			if (!d || !property->material || !property->fadeNode) {
				a_why = "not a membrane pass of a Lighting property with a material and a fade node";
				return false;
			}
			const auto& data = *property->effectData;
			// Lit and projected membranes read lights and a projection CS does not compute; a block-out texture adds a Utility pass.
			if ((d & (EF::Lighting | EF::ProjectedUv)) || data.blockOutTexture) {
				a_why = std::format("a membrane variant the native menu scene does not draw (pass {:#x}, block-out {})", d, static_cast<bool>(data.blockOutTexture));
				return false;
			}
			// The descriptors as SetupTechnique splits them (FUN_141521300, FUN_141521310) and State::ModifyShaderLookup clears them.
			constexpr std::uint32_t kPermutationBits = EF::GrayscaleToColor | EF::GrayscaleToAlpha | EF::IgnoreTexAlpha;
			a_out.pass = d;
			a_out.rawPixel = d & ~EF::SkyObject;
			a_out.vertex = (d & 0xffc7ffffu) & ~kPermutationBits;
			a_out.pixel = a_out.rawPixel & ~kPermutationBits;
			std::span<const std::uint8_t> vsTable, psTable;
			if (!EffectShaders(a_out, vsTable, psTable, a_why))
				return false;
			const auto& space = Extensions::MaterialSpaceOf(Effects::kMaterialKindEffect);
			const auto& vsLayout = space.vs.layout;
			const auto& psLayout = space.ps.layout;
			auto& c = a_out.constants;
			EffectTechnique(a_in, a_out.pass, vsLayout, psLayout, vsTable, psTable, c);

			// SetupMaterial (AE 0x1414ea6c0) writes no constants for a membrane; it binds the Lighting material's textures. SetupTechnique set
			// the base texture's filter.
			auto& record = a_out.record;
			record.kind = Effects::kMaterialKindEffect;
			record.vs.Reset();
			record.ps.Reset();
			record.filterModes.fill(kUnwrittenFilterMode);
			const auto bind = [&](std::uint32_t a_slot, const void* a_texture, std::uint32_t a_address, std::uint32_t a_filter) {
				record.textures[a_slot] = MaterialPort::ViewOf(a_texture);
				record.addressModes[a_slot] = a_address;
				record.filterModes[a_slot] = a_filter;
				record.textureWritten |= 1u << a_slot;
			};
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			bind(0, data.baseTexture.get(), 3, 3);
			if (!(d & EF::Normals))
				bind(1, material->normalTexture.get(), 3, 3);
			if (d & EF::AlphaTest)
				bind(2, material->diffuseTexture.get(), 3, 3);
			if (d & (EF::GrayscaleToColor | EF::GrayscaleToAlpha))
				bind(4, data.paletteTexture.get(), 0, 1);

			// SetupGeometry (AE 0x1414eab10) and its membrane setup (AE 0x1414ebbe0).
			ConstantBlock vs2, ps2;
			vs2.Reset();
			ps2.Reset();
			if (!(d & EF::Skinned)) {
				WorldRows(a_geometry.world, a_in.eye, Variable(vs2, vsLayout, Effects::kEffectVSWorld));
				WorldRows(a_geometry.previousWorld, a_in.havePrevious ? a_in.previousEye : a_in.eye, Variable(vs2, vsLayout, Effects::kEffectVSPreviousWorld));
			}
			{
				// EyePosition: the accumulator's eye in the geometry's model space (the inverse world).
				const auto& t = a_geometry.world;
				const auto& r = t.rotate.entry;
				const auto offset = a_in.eye - t.translate;
				const float inverseScale = 1.0f / t.scale;
				float* eye = Variable(vs2, vsLayout, Effects::kEffectVSEyePosition);
				for (int i = 0; i < 3; ++i)
					eye[i] = (r[0][i] * offset.x + r[1][i] * offset.y + r[2][i] * offset.z) * inverseScale;
				float* texcoord = Variable(vs2, vsLayout, Effects::kEffectVSTexcoordOffsetMembrane);
				texcoord[0] = data.uOffset;
				texcoord[1] = data.vOffset;
				texcoord[2] = data.uScale;
				texcoord[3] = data.vScale;
				// PosAdjust: the camera offset's z, then the fade node's bound bottom and height.
				const auto& bound = property->fadeNode->worldBound;
				float* posAdjust = Variable(vs2, vsLayout, Effects::kEffectVSPosAdjust);
				posAdjust[0] = bound.center.z - bound.radius;
				posAdjust[1] = bound.radius + bound.radius;
				posAdjust[2] = a_in.eye.z;
			}
			{
				auto& linear = globals::features::linearLighting;
				const bool linearOn = linear.loaded && linear.IsLinearLightingActive();
				float* colour = Variable(ps2, psLayout, Effects::kEffectPSPropertyColor);
				float* vars = Variable(ps2, psLayout, Effects::kEffectPSMembraneVars);
				vars[0] = data.edgeExponent;
				const auto& fill = data.fillColor;
				if (d & EF::GrayscaleToColor) {
					colour[0] = fill.red;
					colour[1] = fill.green;
					colour[2] = fill.blue;
					colour[3] = fill.alpha;
					vars[2] = data.baseFillScale;
				} else {
					// Linear Lighting's EffectGeometryUpload converts the fill colour.
					const auto rgb = linearOn ? linear.SRGBToWorking({ fill.red, fill.green, fill.blue }) * data.baseFillScale :
					                            RE::NiColor{ fill.red * data.baseFillScale, fill.green * data.baseFillScale, fill.blue * data.baseFillScale };
					colour[0] = rgb.red;
					colour[1] = rgb.green;
					colour[2] = rgb.blue;
					colour[3] = fill.alpha * data.baseFillScale;
				}
				float* rim = Variable(ps2, psLayout, Effects::kEffectPSMembraneRimColor);
				rim[0] = data.rimColor.red;
				rim[1] = data.rimColor.green;
				rim[2] = data.rimColor.blue;
				rim[3] = data.rimColor.alpha;
				if (d & EF::AlphaTest)
					Variable(ps2, psLayout, Effects::kEffectPSAlphaTestRef)[0] = BiasedAlphaRefOf(*a_geometry.GetGeometryRuntimeData().alphaProperty);
			}
			Pack(vs2, vsLayout, vsTable, Effects::kEffectVSGroups[2], Effects::kEffectVSFirstVariable[2], c.vsGeometry, kEffectVSGeometryRows);
			Pack(ps2, psLayout, psTable, Effects::kEffectPSGroups[2], Effects::kEffectPSFirstVariable[2], c.psGeometry, kEffectPSGeometryRows);
			return true;
		}

		/**
		 * @brief A membrane pass's states, drawn in the accumulator's membrane group (accumulation hint 3, FUN_1414b3d50): the effect data's
		 * blend, depth test without write, depth bias mode 10 (11 with a_closestPoint), back faces culled, write mode 11. False for a blend
		 * pair the membrane setup leaves as it finds it.
		 */
		bool MembraneStateOf(const RE::BSGeometry& a_geometry, bool a_closestPoint, DrawState& a_out)
		{
			using Function = RE::NiAlphaProperty::AlphaFunction;
			const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
			if (!property || !property->effectData)
				return false;
			// The membrane setup's blend from the effect data's pair; another pair keeps the previous draw's blend.
			const auto source = property->effectData->srcBlend, destination = property->effectData->destBlend;
			a_out = {};
			if (source == Function::kSrcAlpha && destination == Function::kInvSrcAlpha)
				a_out.blend = 1;
			else if (source == Function::kSrcAlpha && (destination == Function::kOne || destination == Function::kInvDestAlpha))
				a_out.blend = 2;
			else if (source == Function::kDestColor && destination == Function::kInvSrcAlpha)
				a_out.blend = 3;
			else if (source == Function::kZero && destination == Function::kSrcColor)
				a_out.blend = 4;
			else
				return false;
			// The membrane group (FUN_1414b3d50): depth test without write and bias mode 10, plus one with the accumulator's +0x51; the
			// membrane setup culls back faces and turns the alpha test off; SetupGeometry writes mode 11 for a hint 3 pass.
			a_out.depth = 1;
			a_out.depthBias = a_closestPoint ? 11u : 10u;
			a_out.cull = 1;
			a_out.write = 11;
			a_out.alphaRef = 0.0f;
			return true;
		}

		/**
		 * @brief The states a menu model's Utility draw is drawn with: depth test and write and no colour, the property's culling, and the
		 * alpha property's test.
		 */
		DrawState UtilityStateOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty& a_property)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			DrawState state;
			state.depth = 3;
			state.write = 0;
			state.cull = a_property.flags.any(Flag::kTwoSided) ? 0u : 1u;
			const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
			if (alpha && alpha->GetAlphaTesting())
				state.alphaRef = AlphaRefOf(*alpha);
			return state;
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
	}

	void UtilityFrameRows(const RE::NiCamera& a_camera, float (*a_out)[4])
	{
		std::memset(a_out, 0, kFrameBytes);
		ShadowCameras::Camera matrices;
		ShadowCameras::CameraMatrices(a_camera, matrices);
		const RE::NiPoint3 eye{ matrices.eye[0], matrices.eye[1], matrices.eye[2] };
		Snapshot::FrameCamera::WriteCamera(matrices.view, matrices.proj, matrices.viewProj, eye, { 0.0f, 0.0f }, nullptr, &a_out[0][0]);
		FrameTailRows(a_out);
	}

	namespace
	{
		/**
		 * @brief The Utility pass a_geometry registers into a_view's accumulator (GetRenderPasses in the depth or shadow map render mode),
		 * with its constants (BSUtilityShader::SetupTechnique AE 0x1414fa160, SetupMaterial 0x1414faa60, SetupGeometry 0x1414fae40). False
		 * with an empty a_why when the geometry has no pass there; false with a_why when CS cannot compute it.
		 */
		bool ComputeUtility(const UtilityView& a_view, const RE::BSGeometry& a_geometry, UtilityResult& a_out, std::string& a_why)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			a_out = {};
			a_why.clear();
			const auto* property = netimmerse_cast<const RE::BSLightingShaderProperty*>(a_geometry.GetGeometryRuntimeData().shaderProperty.get());
			if (!property || !property->material)
				return false;
			const std::uint64_t flags = property->flags.underlying();
			const auto* alpha = a_geometry.GetGeometryRuntimeData().alphaProperty.get();
			const bool testing = alpha && alpha->GetAlphaTesting();
			const bool blending = alpha && alpha->GetAlphaBlending();
			// The passes GetRenderPasses registers: the depth pass takes what writes depth and is not blended, a shadow map its casters.
			std::uint32_t technique = 0;
			if (a_view.view == View::MenuDepth) {
				if (!property->flags.any(Flag::kZBufferWrite) || BlendedList(a_geometry) || property->flags.any(Flag::kDecal, Flag::kDynamicDecal))
					return false;
				technique = ShadowUtilityTechnique(flags, testing) | ShadowModeBits(0xC);
			} else if (a_view.view == View::LocalMap) {
				// GetRenderPasses_LocalMap (vtable 0x2C, AE 0x1414afb00, SE 0x1412c7cd0) in render mode 0x12: none for a skinned or refracting
				// property, a blended one, or one whose fade times material alpha is below 1 (the constant at SE 0x1415232d8).
				if ((flags & 0x8002) || blending)
					return false;
				const float fade = property->fadeNode ? property->fadeNode->GetRuntimeData().currentFade : 1.0f;
				if (fade * static_cast<const RE::BSLightingShaderMaterialBase*>(property->material)->materialAlpha < 1.0f)
					return false;
				technique = UtilityShaderDecl(flags) | (testing ? 0x80u : 0u) | static_cast<std::uint32_t>(SIE::ShaderCache::UtilityShaderFlags::RenderBaseTexture);
			} else if (a_view.view == View::LocalMapFog) {
				// The same function in render mode 0x13 (the fog of war): every property, no alpha test bit.
				technique = UtilityShaderDecl(flags) | static_cast<std::uint32_t>(SIE::ShaderCache::UtilityShaderFlags::LocalMapFogOfWar);
			} else {
				const auto type = static_cast<std::uint8_t>(a_geometry.GetType().underlying());
				if (ShadowCasterReject(flags, type, blending, property->alpha, FrameGlobals::Current().shadowGlobal) != ShadowReject::None)
					return false;
				technique = ShadowUtilityTechnique(flags, testing) | ShadowModeBits(a_view.renderMode);
			}
			a_out.technique = technique;
			a_out.vertex = ShadowMask::Native::VertexDescriptorOf(technique);
			auto& c = a_out.constants;
			const std::uint32_t t = a_out.vertex;
			// SetupTechnique: a paraboloid's ParabolaParam (VS c1).
			if ((t & 0x20004000u) == 0x4000u && (t & 0x10000u)) {
				c.vsTechnique[1][0] = a_view.parabola[0];
				c.vsTechnique[1][1] = a_view.parabola[1];
			}
			// SetupMaterial: a textured technique's TexcoordOffset, and t0 the diffuse with the material's clamp mode.
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(property->material);
			if ((t & 0x2u) && (t & 0x2040280u)) {
				const std::uint32_t buffer = a_view.textureTransformBuffer & 1;
				c.vsMaterial[0][0] = material->texCoordOffset[buffer].x;
				c.vsMaterial[0][1] = material->texCoordOffset[buffer].y;
				c.vsMaterial[0][2] = material->texCoordScale[buffer].x;
				c.vsMaterial[0][3] = material->texCoordScale[buffer].y;
				if (t & 0x2040080u) {
					if (property->material->GetType() != RE::BSShaderMaterial::Type::kLighting) {
						a_why = "a textured Utility pass of a material that is not a Lighting material";
						return false;
					}
					a_out.texture = material->diffuseTexture.get();
					a_out.address = static_cast<std::uint32_t>(material->textureClampMode);
					a_out.filter = 3;
				}
			}
			// SetupGeometry: World (VS c1-c4) less the eye, unskinned only; a skinned draw's palettes place it.
			if (!(t & 0x4u)) {
				float rows[12];
				WorldRows(a_geometry.world, a_view.eye, rows);
				for (std::uint32_t r = 0; r < 3; ++r)
					for (std::uint32_t i = 0; i < 4; ++i)
						c.vsGeometry[1 + r][i] = rows[r * 4 + i];
				const float last[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
				for (std::uint32_t i = 0; i < 4; ++i)
					c.vsGeometry[4][i] = last[i];
			}
			if (t & 0x200u) {
				a_why = std::format("a Utility technique with an eye position row ({:#x})", t);
				return false;
			}
			// AlphaTestRef.x (PS c2), which only the depth pass's pixel shader declares: 0x3F7EFEFF while the alpha property blends, else the
			// threshold over 255 plus the exe's bias.
			// The fog of war's pixel shader has no PerGeometry: the engine writes no row there.
			if ((a_view.view == View::MenuDepth || a_view.view == View::LocalMap) && alpha && (blending || (t & 0x80u) || (t & 0x14000u) == 0x10000u))
				c.psGeometry[2][0] = blending ? std::bit_cast<float>(0x3F7EFEFFu) : BiasedAlphaRefOf(*alpha);
			a_out.state = UtilityStateOf(a_geometry, *property);
			// The local map's states (LocalMapRenderer::Render): the base textures write colour (mode 1); the fog of war writes alpha only
			// (mode 8) with no depth test and no culling.
			if (a_view.view == View::LocalMap || a_view.view == View::LocalMapFog)
				a_out.state.stencil = 0;
			if (a_view.view == View::LocalMap)
				a_out.state.write = 1;
			else if (a_view.view == View::LocalMapFog) {
				a_out.state.write = 8;
				a_out.state.depth = 0;
				a_out.state.cull = 0;
			}
			c.alphaTestRef = a_out.state.alphaRef;
			return true;
		}

		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		/** @brief The reason a draw is left out while its program builds on the program service (the next session prebuilds it). */
		constexpr const char* kBuilding = "its program is still building";
		/** @brief A geometry with no renderer buffers yet (a head part still loading): the engine draws nothing for it either. */
		constexpr const char* kNoBuffers = "no renderer buffers";
		/** @brief A texture still streaming in (pending, or its full window not loaded yet): a later render takes it. */
		constexpr const char* kStreaming = "a texture it samples is still streaming in";

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
				state.alphaRef = AlphaRefOf(*alpha);
			const bool test = a_property.flags.any(Flag::kZBufferTest), write = a_property.flags.any(Flag::kZBufferWrite);
			state.depth = a_effect ? (test ? (write ? 3u : 1u) : 0u) : 3u;
			state.cull = a_property.flags.any(Flag::kTwoSided) ? 0u : 1u;
			state.write = a_effect ? 11u : (a_target == kFramebufferTarget ? 12u : 1u);
			return state;
		}

		/**
		 * @brief The colour targets of a menu scene's colour pass, as UI::Render and UI3DSceneManager::Render bind them. No motion vectors
		 * outside scheme 4: schemes 3 and 6 never take TAA, and CS turns the engine's TAA off around UI::Render (Hooks.cpp), so no TAA mask
		 * fill follows.
		 */
		std::array<std::int32_t, kMaxColours> ColourTargets(std::int32_t a_scheme)
		{
			if (a_scheme == 4)
				return { kMainTarget, kMotionVectorTarget, kNormalTaaMaskSwapTarget };
			return { kFramebufferTarget, -1, -1 };
		}

		/** @brief Maps a_targets into a_view's client's imports. Null when mapped. */
		const char* MapColours(View a_view, const std::array<std::int32_t, kMaxColours>& a_targets, std::uint32_t a_outputs, Draw& a_draw)
		{
			const auto imports = TargetsOf(a_view);
			a_draw.colourCount = 0;
			for (std::uint32_t i = 0; i < (std::min)(a_outputs, kMaxColours); ++i) {
				if (a_targets[i] < 0)
					break;
				const auto found = std::ranges::find(imports, a_targets[i]);
				if (found == imports.end())
					return "a colour target its view's client does not import";
				const auto index = static_cast<std::uint32_t>(found - imports.begin());
				a_draw.colours[i] = static_cast<std::uint8_t>(index);
				a_draw.key.colourFormats[i] = SceneDraws::ColourFormat(a_view, index);
				if (!a_draw.key.colourFormats[i])
					return "a colour target with no format";
				++a_draw.colourCount;
			}
			a_draw.key.depthFormat = SceneDraws::DepthFormat(a_view);
			return a_draw.key.depthFormat ? nullptr : "no depth import";
		}

		/**
		 * @brief The skin partitions a skinned shape without renderer data of its own draws, a bit each (0 for a shape with its own). Such a
		 * shape, the book's pages for one, draws each partition's buffers as the engine's skinned draw does: the partitions row 3 draws
		 * (DCLF's world rule), every one with the whole skin's palette (the bone setter, AE 0x14150be30).
		 */
		std::uint32_t DrawnPartitions(const RE::BSGeometry& a_geometry)
		{
			if (a_geometry.GetGeometryRuntimeData().rendererData)
				return 0;
			const auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
			return skin && skin->skinPartition ? SkinPartitions::Mask(*skin, 3) : 0u;
		}

		/**
		 * @brief The draw's geometry by device address, and its index count. A skinned shape without renderer data draws partition
		 * a_partition (DrawnPartitions); ~0u takes its first drawn one. A skin whose palette update makes no matrices (a first-person hand
		 * on a loading model) is left out.
		 */
		const char* AddGeometry(const RE::BSGeometry& a_geometry, std::uint64_t a_vertexDesc, Run& a_run, Draw& a_draw, RenderGraph::BufferRef& a_vertexRef,
			RenderGraph::BufferRef& a_indexRef, std::uint32_t* a_layout, std::uint32_t& a_stride, std::uint32_t& a_firstIndex, std::string& a_detail,
			std::uint32_t a_partition = ~0u)
		{
			if (a_geometry.GetType().get() != RE::BSGeometry::Type::kTriShape)
				return "not a BSTriShape";
			const auto* shape = reinterpret_cast<const Family::TriShape*>(a_geometry.GetGeometryRuntimeData().rendererData);
			auto triangles = static_cast<std::uint32_t>(static_cast<const RE::BSTriShape&>(a_geometry).GetTrishapeRuntimeData().triangleCount);
			if (const auto mask = DrawnPartitions(a_geometry)) {
				auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
				SkinPalettes::Update(skin, a_geometry.world);
				if (!skin->numMatrices || !skin->boneMatrices)
					return kNoBuffers;
				const auto index = a_partition == ~0u ? static_cast<std::uint32_t>(std::countr_zero(mask)) : a_partition;
				const auto& partition = skin->skinPartition->partitions[index];
				shape = reinterpret_cast<const Family::TriShape*>(partition.buffData);
				triangles = partition.triangles;
			}
			Family::Geometry geometry;
			const auto result = Family::LatchGeometry(shape, 0, triangles, a_vertexDesc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, geometry, a_run.owners, &a_detail);
			if (result == Family::GeometryResult::Missing)
				return kNoBuffers;
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

		/**
		 * @brief Pushes a_draw, whose latch holds a_constants, then one draw for each further partition a skinned shape without renderer
		 * data draws (DrawnPartitions): a copy with that partition's buffers in its own constants, the same program, palettes and frame rows.
		 * Every partition's buffers are taken before any draw is pushed, so a shape is drawn whole or left out.
		 */
		template <class Constants>
		const char* PushDraw(const RE::BSGeometry& a_geometry, std::uint64_t a_vertexDesc, Run& a_run, Draw&& a_draw, const Constants& a_constants, std::string& a_detail)
		{
			auto further = DrawnPartitions(a_geometry);
			further &= further - 1;  // the first drawn partition is a_draw's
			std::vector<Draw> draws;
			while (further) {
				const auto index = static_cast<std::uint32_t>(std::countr_zero(further));
				further &= further - 1;
				Draw draw = a_draw;
				Constants c = a_constants;
				if (const char* why = AddGeometry(a_geometry, a_vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail, index))
					return why;
				draw.latchOffset = a_run.Append(std::as_bytes(std::span<const Constants, 1>(&c, 1)));
				if (draw.latchOffset == ~0u)
					return "more constants than the batch's latch holds";
				draws.push_back(std::move(draw));
			}
			a_run.draws.push_back(std::move(a_draw));
			a_run.draws.insert(a_run.draws.end(), std::make_move_iterator(draws.begin()), std::make_move_iterator(draws.end()));
			return nullptr;
		}

		/** @brief A texture slot of a material record and its sampler. A slot the record leaves unwritten reads the null view. */
		const char* AddTexture(ID3D11ShaderResourceView* a_view, std::uint32_t a_address, std::uint32_t a_filter, std::uint32_t a_slot, Run& a_run, std::uint32_t& a_index,
			std::uint32_t& a_sampler, std::string& a_detail, Kind a_kind = Kind::Texture2D)
		{
			auto* view = Unwrap(a_view);
			GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
			const auto slot = Family::SlotTexture(view, a_kind, GpuTextures::Tag(GpuTextures::Source::FrameSlot, a_slot), a_index, a_run.owners, &failure);
			if (slot == Family::SlotResult::Published) {
				// A target a graph pass writes, such as the shadow mask, is read by its published id, so the graph orders the write first.
				const auto id = PublishedResources::ExportedId(view);
				const auto result = SceneDraws::ReadPublished(a_run.view, id, a_index);
				if (result != Family::InputResult::Graph) {
					a_detail = std::format("t{} holds '{}': {}", a_slot, id, Family::ReasonOf(result));
					return "a slot holds a published texture its client does not read";
				}
			} else if (slot != Family::SlotResult::Ok) {
				a_detail = GpuTextures::DescribeFailure(view, a_slot, failure);
				const bool streaming = failure == GpuTextures::ResolveFailure::Pending || failure == GpuTextures::ResolveFailure::StreamedWindow;
				return streaming ? kStreaming : "a texture it samples could not be imported";
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
		const char* AddBones(const RE::BSGeometry& a_geometry, Run& a_run, Draw& a_draw, std::string& a_detail)
		{
			std::array<std::byte, kBonesBytes> bones;
			if (!BoneRows(a_geometry, bones)) {
				const auto* skin = a_geometry.GetGeometryRuntimeData().skinInstance.get();
				a_detail = skin ? std::format("{} matrices ({} rows fit), palettes {} and {}", skin->numMatrices, kPaletteRows, skin->boneMatrices != nullptr,
									  skin->prevBoneMatrices != nullptr) :
				                  std::string("no skin instance");
				return "a skinned draw whose palettes could not be computed";
			}
			a_draw.bonesOffset = a_run.Append(bones);
			return a_draw.bonesOffset == ~0u ? "more palettes than the batch's latch holds" : nullptr;
		}

		const char* DescribeStates(Draw& a_draw, const DrawState& a_state)
		{
			a_draw.stencilRef = a_state.stencilRef;
			if (!Family::DescribeStates(a_draw.key, Pipeline::EngineBlendState(a_state.blend, 0, a_state.write, 0), Pipeline::EngineDepthStencilState(a_state.depth, a_state.stencil),
					Pipeline::EngineRasterState(0, a_state.cull, a_state.depthBias, 0)))
				return "a depth-stencil, blend or rasterizer state with no description";
			return nullptr;
		}

		const char* AddLighting(const Inputs& a_in, const RE::BSGeometry& a_geometry, const std::array<std::int32_t, kMaxColours>& a_targets, Run& a_run,
			std::uint32_t& a_frame, std::string& a_detail)
		{
			LightingResult r;
			// A shader CS's cache has not compiled yet (the first frames of a cold cache) leaves the draw out like a building program.
			if (!ComputeLighting(a_in, a_geometry, r, a_detail))
				return a_detail.starts_with("no shader") ? kBuilding : "its Lighting constants could not be computed";
			Draw draw{};
			draw.shader = Shader::Lighting;
			constexpr auto kVertexInputs = static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::VC) | static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned);
			draw.key.descriptor = (r.pixel & ~kVertexInputs) | (r.vertex & kVertexInputs);
			draw.key.defineStage = Family::DefineStage::Pixel;
			// The local map's land keeps Linear Lighting's pipeline: kLOCAL_MAP_SWAP stores the shader's output as plain D3D11 has it.
			if (a_run.view == View::LocalMap)
				draw.key.variant = kLightingLinearVariant;
			if (const char* why = MapColours(a_run.view, a_targets, kMaxColours, draw))
				return why;
			const auto* property = a_geometry.GetGeometryRuntimeData().shaderProperty.get();
			auto state = StateOf(a_geometry, *property, false, a_targets[0]);
			// The local map's draws test no stencil (its render leaves stencil mode 0).
			if (a_run.view == View::LocalMap)
				state.stencil = 0;
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
			// Linear Lighting's RenderToUI is set around menu scene renders only, not the local map's.
			c.renderToUI = a_run.view == View::LocalMap ? 0u : permutation.renderToUI;
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
				if (const char* why = AddBones(a_geometry, a_run, draw, a_detail))
					return why;
			draw.frameOffset = FrameOffset(a_in, a_run, a_frame);
			draw.latchOffset = a_run.Append(std::as_bytes(std::span<const LightingSceneConstants, 1>(&c, 1)));
			if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = MenuViewport(a_targets[0], state);
			return PushDraw(a_geometry, vs->vertexDesc, a_run, std::move(draw), c, a_detail);
		}

		/**
		 * @brief A particle system's quads as the engine draws them (FUN_140e51320): CS expands the system's state, read right after the
		 * update half, faced to the menu camera, and the draw pulls them by the renderer's quad index list. a_block gets the latch region:
		 * room for the constants, the indexed texture coordinates (row 0 the identity rect, the rects being resolved per particle), then
		 * five rows a particle. No particles leaves the draw's index count 0.
		 */
		const char* AddParticles(const Inputs& a_in, const RE::NiParticleSystem& a_system, Run& a_run, Draw& a_draw, EffectSceneConstants& a_constants,
			std::vector<std::byte>& a_block)
		{
			using Snapshots = Particles::ParticleSnapshots;
			const auto* data = static_cast<const RE::NiParticles&>(a_system).GetParticlesRuntimeData().particleData.get();
			const std::uint32_t count = data ? (std::min)(static_cast<std::uint32_t>(data->GetParticlesRuntimeData().numVertices), Snapshots::kMaxQuads) : 0u;
			a_draw.indexCount = 0;
			if (!count)
				return nullptr;
			auto* indices = Snapshots::QuadIndexBuffer();
			auto leased = indices ? GpuResources::Frame().Acquire(indices) : std::nullopt;
			if (!leased || !leased->buffer.ref)
				return "the renderer's particle quad index list could not be leased";
			Snapshots::Camera camera;
			const auto& r = a_in.camera.rotation.entry;
			for (int i = 0; i < 3; ++i) {
				camera.forward[i] = r[i][0];
				camera.up[i] = r[i][1];
				camera.right[i] = r[i][2];
			}
			camera.eye[0] = a_in.eye.x;
			camera.eye[1] = a_in.eye.y;
			camera.eye[2] = a_in.eye.z;
			camera.valid = true;
			constexpr std::size_t kRowsAt = sizeof(EffectSceneConstants) + kEffectIndexedTexCoordBytes;
			a_block.assign(kRowsAt + std::size_t(count) * Snapshots::kRowsPerParticle * 16, std::byte{ 0 });
			constexpr float kIdentityRect[4]{ 0.0f, 1.0f, 0.0f, 1.0f };
			std::memcpy(a_block.data() + sizeof(EffectSceneConstants), kIdentityRect, sizeof(kIdentityRect));
			Snapshots::Expanded expanded;
			Snapshots::ExpandNow(a_system, camera, count, reinterpret_cast<float*>(a_block.data() + kRowsAt), expanded);
			if (!expanded.particles)
				return nullptr;
			a_constants.vertexRef = {};
			a_constants.vertexStride = 0;
			a_constants.vertexLayout[0] = a_constants.vertexLayout[1] = 0;
			a_constants.streamRef = {};
			a_constants.streamStride = 0;
			a_constants.indexRef = leased->buffer.ref;
			a_constants.firstIndex = 0;
			a_draw.indexCount = expanded.particles * 6;
			a_run.owners.push_back(std::move(leased->owner));
			return nullptr;
		}

		/** @brief An Effect draw (an Effect property's pass or a Lighting property's membrane) from its computed constants and states. */
		const char* AddEffect(const Inputs& a_in, const RE::BSGeometry& a_geometry, const EffectResult& a_result, const DrawState& a_state,
			const std::array<std::int32_t, kMaxColours>& a_targets, Run& a_run, std::uint32_t& a_frame, std::string& a_detail)
		{
			Draw draw{};
			draw.shader = Shader::Effect;
			draw.key.descriptor = a_result.pixel | (a_result.vertex & Effects::EffectFlag::Skinned);
			draw.key.defineStage = Family::DefineStage::Pixel;
			if (const char* why = MapColours(a_run.view, a_targets, kMaxColours, draw))
				return why;
			if (const char* why = DescribeStates(draw, a_state))
				return why;
			auto* shader = EffectShader();
			auto* vs = globals::shaderCache->GetVertexShader(*shader, a_result.vertex);
			EffectSceneConstants c = a_result.constants;
			c.alphaTestRef = a_state.alphaRef;
			c.descriptors[0] = a_result.vertex;
			c.descriptors[1] = a_result.rawPixel & ~a_result.pixel;
			const auto permutation = PermutationOf(a_geometry, true);
			c.descriptors[2] = permutation.extra;
			c.descriptors[3] = globals::state->permutationData.ExtraFeatureDescriptor;
			c.effectRadius = a_geometry.worldBound.radius;
			c.baseTextureIsWorking = permutation.baseTextureIsWorking;
			c.renderToUI = permutation.renderToUI;
			const bool particles = IsParticles(a_geometry, a_result.pass);
			std::vector<std::byte> quads;
			if (particles) {
				if (const char* why = AddParticles(a_in, static_cast<const RE::NiParticleSystem&>(a_geometry), a_run, draw, c, quads))
					return why;
				// No particles: the engine draws nothing.
				if (!draw.indexCount)
					return nullptr;
			} else if (const char* why = AddGeometry(a_geometry, vs->vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail))
				return why;
			auto null = GpuTextures::Get().NullOf(Kind::Texture2D);
			if (null.index == GpuTextures::kInvalid)
				return "no null view";
			c.nullTexture = null.index;
			if (null.owner)
				a_run.owners.push_back(std::move(null.owner));
			for (std::uint32_t t = 0; t < kEffectSlots; ++t) {
				const bool written = t < kEffectSlotsRead && ((a_result.record.textureWritten >> t) & 1);
				const std::uint32_t filter = a_result.record.filterModes[t] == kUnwrittenFilterMode ? 0u : a_result.record.filterModes[t];
				if (const char* why = AddTexture(written ? a_result.record.textures[t] : nullptr, written ? a_result.record.addressModes[t] : 0u, written ? filter : 0u, t, a_run, c.textures[t],
						c.samplers[t], a_detail))
					return why;
			}
			bool building = false;
			draw.program = SceneDraws::EffectProgram(draw.key, *shader, &building);
			if (!draw.program)
				return building ? kBuilding : "its ORG program cannot be built";
			if (a_result.vertex & Effects::EffectFlag::Skinned)
				if (const char* why = AddBones(a_geometry, a_run, draw, a_detail))
					return why;
			draw.frameOffset = FrameOffset(a_in, a_run, a_frame);
			if (particles) {
				// The constants, then the quads' block (AddParticles), one latch region.
				std::memcpy(quads.data(), &c, sizeof(c));
				draw.latchOffset = a_run.Append(quads);
			} else
				draw.latchOffset = a_run.Append(std::as_bytes(std::span<const EffectSceneConstants, 1>(&c, 1)));
			if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.viewport = MenuViewport(a_targets[0], a_state);
			return PushDraw(a_geometry, vs->vertexDesc, a_run, std::move(draw), c, a_detail);
		}

		/**
		 * @brief A Lighting property's membrane pass, in the membrane group (accumulation hint 3). The engine gave every observed menu
		 * membrane hint 3, its fade node's fade (0 for a menu model) notwithstanding.
		 */
		const char* AddMembrane(const Inputs& a_in, const RE::BSGeometry& a_geometry, bool a_closestPoint, const std::array<std::int32_t, kMaxColours>& a_targets,
			Run& a_run, std::uint32_t& a_frame, std::string& a_detail)
		{
			EffectResult result;
			if (!ComputeMembrane(a_in, a_geometry, result, a_detail))
				return "its membrane constants could not be computed";
			DrawState state;
			if (!MembraneStateOf(a_geometry, a_closestPoint, state))
				return "a membrane blend pair its setup leaves as it finds it";
			return AddEffect(a_in, a_geometry, result, state, a_targets, a_run, a_frame, a_detail);
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
			std::array<bool, 8> schemesLogged{};  // a scheme's first native render with passes is logged
			std::array<std::uint64_t, 8> schemeRenders{};
			std::uint64_t building = 0;   // draws left out while their programs build
			std::uint64_t streaming = 0;  // draws left out while a texture streams in
		};

		NativeState& GetNative()
		{
			static NativeState state;
			return state;
		}
	}

	const char* AddUtility(const UtilityView& a_view, const RE::BSGeometry& a_geometry, Run& a_run, std::string& a_detail)
	{
		UtilityResult r;
		if (!ComputeUtility(a_view, a_geometry, r, a_detail))
			return a_detail.empty() ? nullptr : "its Utility constants could not be computed";
		auto* shader = RE::BSUtilityShader::GetSingleton();
		auto* vs = shader ? globals::shaderCache->GetVertexShader(*shader, r.vertex) : nullptr;
		if (!vs) {
			a_detail = std::format("vs {:#x}", r.vertex);
			return "no Utility vertex shader for its technique";
		}
		Draw draw{};
		draw.shader = Shader::Utility;
		draw.key.descriptor = r.vertex;
		draw.key.defineStage = Family::DefineStage::Vertex;
		// The local map's Utility draws write kLOCAL_MAP_SWAP; the depth pass and the shadow maps write depth only.
		if (a_view.view == View::LocalMap || a_view.view == View::LocalMapFog) {
			if (const char* why = MapColours(a_view.view, { kLocalMapSwapTarget, -1, -1 }, 1, draw))
				return why;
		} else
			draw.key.depthFormat = SceneDraws::DepthFormat(a_view.view);
		if (!draw.key.depthFormat)
			return "no depth import";
		draw.depthSlice = a_view.slice;
		if (const char* why = DescribeStates(draw, r.state))
			return why;
		UtilitySceneConstants c = r.constants;
		if (const char* why = AddGeometry(a_geometry, vs->vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail))
			return why;
		// UTILITY_ORG_SCENE reads t0 and t7; every other slot, and t0 without a texture, gets the null view and the first sampler.
		for (std::uint32_t slot = 0; slot < kUtilitySlots; ++slot) {
			if (slot == 0 && r.texture) {
				if (const char* why = AddTexture(MaterialPort::ViewOf(r.texture), r.address, r.filter, slot, a_run, c.textures[slot], c.samplers[slot], a_detail))
					return why;
				continue;
			}
			if (Family::SlotTexture(nullptr, UtilitySlotKind(slot), GpuTextures::Tag(GpuTextures::Source::FrameSlot, slot), c.textures[slot], a_run.owners) !=
				Family::SlotResult::Ok)
				return "no null view";
			c.samplers[slot] = Family::SlotSampler(0, 0, a_run.owners);
			if (c.samplers[slot] == GpuTextures::kInvalid)
				return "its sampler modes have no graph sampler";
		}
		bool building = false;
		draw.program = SceneDraws::UtilityProgram(draw.key, *shader, &building);
		if (!draw.program)
			return building ? kBuilding : "its ORG program cannot be built";
		if (r.vertex & 0x4u)
			if (const char* why = AddBones(a_geometry, a_run, draw, a_detail))
				return why;
		// Draws of one camera share its rows: the previous draw's are taken when they are the same.
		const auto frame = std::as_bytes(std::span<const float[4]>(a_view.frame, kFrameRows));
		const auto* last = a_run.draws.empty() ? nullptr : &a_run.draws.back();
		const bool same = last && last->frameOffset + frame.size() <= a_run.latch.size() &&
		                  std::memcmp(a_run.latch.data() + last->frameOffset, frame.data(), frame.size()) == 0;
		draw.frameOffset = same ? last->frameOffset : a_run.Append(frame);
		draw.latchOffset = a_run.Append(std::as_bytes(std::span<const UtilitySceneConstants, 1>(&c, 1)));
		if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
			return "more constants than the batch's latch holds";
		draw.viewport = a_view.viewport;
		draw.viewport.depthRange[1] -= Family::DepthBiasViewportOffset(r.state.depthBias);
		return PushDraw(a_geometry, vs->vertexDesc, a_run, std::move(draw), c, a_detail);
	}

	Family::Viewport MenuViewport(std::int32_t a_target, const DrawState& a_state)
	{
		auto viewport = Family::FrameViewport(a_target, static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN), 0);
		viewport.depthRange = { 0.0f, 1.0f - Family::DepthBiasViewportOffset(a_state.depthBias) };
		return viewport;
	}

	bool LeftOut(const char* a_why)
	{
		return a_why == kBuilding || a_why == kNoBuffers || a_why == kStreaming;
	}

	void Refuse(const char* a_scene, std::string_view a_batch, const RE::BSGeometry& a_geometry, const char* a_why, const std::string& a_detail,
		std::uint64_t& a_leftOut)
	{
		const char* name = a_geometry.name.c_str() ? a_geometry.name.c_str() : "?";
		if (LeftOut(a_why)) {
			if (a_leftOut++ == 0)
				logger::info("[DCLF] {}: {} draw {} is left out of this render: {} ({})", a_scene, a_batch, name, a_why, a_detail);
			return;
		}
		::RenderGraph::Fatal(a_scene, std::format("{} draw {}: {} ({})", a_batch, name, a_why, a_detail));
	}

	void SceneGeometry(RE::NiAVObject* a_root, const RE::NiCamera& a_camera, bool a_closestPoint, std::vector<ScenePass>& a_out)
	{
		a_out.clear();
		ShadowCameras::Planes planes;
		ShadowCameras::FrustumPlanes(a_camera.GetRuntimeData2().viewFrustum, a_camera.world, planes);
		std::vector<Visible> visible;
		std::vector<const RE::NiNode*> groupNodes;
		Walk(a_root, a_camera, planes, -1, visible, groupNodes);

		// The batches first, in registration (scene) order, then the membrane group (FinishAccumulating's FUN_1414b3d50, before the
		// blended list).
		std::vector<RE::BSGeometry*> list;
		std::vector<std::vector<RE::BSGeometry*>> groups(groupNodes.size());
		for (const auto& v : visible) {
			if (!BlendedList(*v.geometry))
				a_out.push_back({ v.geometry, false });
			else if (v.group >= 0)
				groups[static_cast<std::size_t>(v.group)].push_back(v.geometry);
			else
				list.push_back(v.geometry);
		}
		for (const auto& v : visible)
			if (MembranePassOf(*v.geometry))
				a_out.push_back({ v.geometry, true });
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
				for (auto* geometry : groups[top])
					a_out.push_back({ geometry, false });
			} else
				a_out.push_back({ list[head++], false });
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
		static const bool native = ::RenderGraph::Engine::Native::Active();
		if (!native)
			return false;
		// The schemes whose draws CS computes as the engine does (draw order included): the inventory's item (1, 2) with its
		// enchantment membranes, the book (3, its pages sample the kBOOK_TEXT targets), MistMenu (4: the main menu, a loading screen, and a
		// loading model with its shadow maps, depth pass and masks), the stats menu's constellations and the lockpicking menu's lock (7).
		// The journal's scheme (0) has no 3D scene, and no menu pushes scheme 5 (the light scheme push, AE 0x1409738f0, has callers for
		// 0 to 4, 6 and 7 only).
		switch (a_scheme) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 6:
		case 7:
			return true;
		default:
			return false;
		}
	}

	namespace
	{
		/** @brief A local map water draw (ComputeWater) into kLOCAL_MAP_SWAP, with the local map camera's FrameBuffer rows. */
		const char* AddWater(const Water::Port::FrameInputs& a_frame, const RE::BSGeometry& a_geometry, const UtilityView& a_view, Run& a_run, std::string& a_detail)
		{
			WaterResult r;
			if (!ComputeWater(a_frame, a_geometry, r, a_detail))
				return "its water constants could not be computed";
			Draw draw{};
			draw.shader = Shader::Water;
			// The vertex stage reads Wading, which State strips from the pixel descriptor, so the key is the vertex descriptor (the water
			// family's rule).
			draw.key.descriptor = r.vertex;
			draw.key.defineStage = Family::DefineStage::Vertex;
			if (const char* why = MapColours(a_run.view, { kLocalMapSwapTarget, -1, -1 }, 1, draw))
				return why;
			if (const char* why = DescribeStates(draw, r.state))
				return why;
			auto& c = r.constants;
			if (const char* why = AddGeometry(a_geometry, r.vertexDesc, a_run, draw, c.vertexRef, c.indexRef, c.vertexLayout, c.vertexStride, c.firstIndex, a_detail))
				return why;
			// LOD's pixel stage reads t4 (and t3 with Reflections, which the local map's passes never carry); every other slot is null.
			for (std::uint32_t t = 0; t < Water::kMaterialSlots; ++t) {
				const auto slot = t == 4 ? r.normals : WaterResult::Slot{};
				if (const char* why = AddTexture(slot.view, slot.address, slot.filter, t, a_run, c.textures[t], c.samplers[t], a_detail, Water::MaterialKind(t)))
					return why;
			}
			bool building = false;
			draw.program = SceneDraws::WaterProgram(draw.key, *r.shader, &building);
			if (!draw.program)
				return building ? kBuilding : "its ORG program cannot be built";
			const auto frame = std::as_bytes(std::span<const float[4]>(a_view.frame, kFrameRows));
			draw.frameOffset = a_run.Append(frame);
			draw.latchOffset = a_run.Append(std::as_bytes(std::span<const Water::DrawConstants, 1>(&c, 1)));
			if (draw.latchOffset == ~0u || draw.frameOffset == ~0u)
				return "more constants than the batch's latch holds";
			draw.stencilRef = r.state.stencilRef;
			draw.viewport = MenuViewport(kLocalMapSwapTarget, r.state);
			a_run.draws.push_back(std::move(draw));
			return nullptr;
		}

		/** @brief The local map's Utility view: its render mode, the camera's eye and rows, kLOCAL_MAP_SWAP's viewport. */
		UtilityView LocalMapUtilityView(View a_view, const RE::NiCamera& a_camera, const Inputs& a_in)
		{
			UtilityView view;
			view.view = a_view;
			view.renderMode = a_view == View::LocalMapFog ? 0x13u : 0x12u;
			view.eye = a_camera.world.translate;
			view.textureTransformBuffer = a_in.material.vanilla.textureTransformBuffer;
			view.viewport = MenuViewport(kLocalMapSwapTarget, DrawState{});
			UtilityFrameRows(a_camera, view.frame);
			return view;
		}

		struct LocalMapState
		{
			std::uint64_t renders = 0, leftOut = 0;
		};

		LocalMapState& GetLocalMap()
		{
			static LocalMapState state;
			return state;
		}

		/** @brief A local map draw's refusal (Refuse). */
		void Refused(const char* a_batch, const RE::BSGeometry& a_geometry, const char* a_why, const std::string& a_detail)
		{
			Refuse("local map", a_batch, a_geometry, a_why, a_detail, GetLocalMap().leftOut);
		}
	}

	namespace
	{
		struct LocalMapQueue
		{
			bool done = false, pending = false;
		};
		LocalMapQueue& GetLocalMapQueue()
		{
			static LocalMapQueue state;
			return state;
		}
	}

	void QueueLocalMapPrograms()
	{
		// The engine's shader objects pass through EngineDrawPrograms::Get, which reads only the key. Before the engine has made them (CS's
		// setup can come first), the queue waits for the first native menu scene render, which comes before any local map.
		const RE::BSShader* utility = RE::BSUtilityShader::GetSingleton();
		const RE::BSShader* lighting = LightingShader();
		const RE::BSShader* waterShaderObject = WaterShader();
		auto& state = GetLocalMapQueue();
		if (state.done)
			return;
		if (!utility || !lighting || !waterShaderObject) {
			state.pending = true;
			return;
		}
		state.done = true;
		state.pending = false;
		std::uint32_t queued = 0;
		const auto queue = [&](View a_view, Shader a_shader, std::uint32_t a_descriptor, Family::DefineStage a_stage, std::uint32_t a_variant, const DrawState& a_state) {
			Draw draw{};
			draw.key.descriptor = a_descriptor;
			draw.key.defineStage = a_stage;
			draw.key.variant = a_variant;
			if (MapColours(a_view, { kLocalMapSwapTarget, -1, -1 }, 1, draw) || DescribeStates(draw, a_state))
				return;
			if (a_shader == Shader::Utility && utility)
				SceneDraws::UtilityProgram(draw.key, *utility);
			else if (a_shader == Shader::Lighting && lighting)
				SceneDraws::LightingProgram(draw.key, *lighting);
			else if (a_shader == Shader::Water && waterShaderObject)
				SceneDraws::WaterProgram(draw.key, *waterShaderObject);
			else
				return;
			++queued;
		};
		using UF = SIE::ShaderCache::UtilityShaderFlags;
		// The base texture passes: GetRenderPasses_LocalMap's technique for the property flags it reads (model space normals, vertex colours,
		// tree animation), with and without the alpha test, both culling modes.
		for (const std::uint64_t normals : { 0ull, 0x1000ull })
			for (const std::uint64_t colours : { 0ull, 0x20'0000'0000ull })
				for (const std::uint64_t tree : { 0ull, 0x2000'0000'0000'0000ull })
					for (const std::uint32_t test : { 0u, 0x80u })
						for (const std::uint32_t cull : { 0u, 1u }) {
							const auto technique = UtilityShaderDecl(normals | colours | tree) | test | static_cast<std::uint32_t>(UF::RenderBaseTexture);
							queue(View::LocalMap, Shader::Utility, ShadowMask::Native::VertexDescriptorOf(technique), Family::DefineStage::Vertex, 0,
								DrawState{ .write = 1, .depth = 3, .cull = cull, .stencil = 0 });
						}
		// The fog of war's overlay (model space normals and vertex colours).
		queue(View::LocalMapFog, Shader::Utility, ShadowMask::Native::VertexDescriptorOf(UtilityShaderDecl(0x1000ull | 0x20'0000'0000ull) | static_cast<std::uint32_t>(UF::LocalMapFogOfWar)),
			Family::DefineStage::Vertex, 0, DrawState{ .write = 8, .depth = 0, .cull = 0, .stencil = 0 });
		// The land: MTLand and MTLandLODBlend with vertex colours, True PBR, Landscape Seams and glint, in the Linear Lighting variant.
		using LF = SIE::ShaderCache::LightingShaderFlags;
		for (const std::uint32_t technique : { 8u, 19u })
			for (const std::uint32_t pbr : { 0u, static_cast<std::uint32_t>(LF::TruePbr) })
				for (const std::uint32_t seams : { 0u, static_cast<std::uint32_t>(LF::LandscapeSeams) })
					for (const std::uint32_t glint : { 0u, static_cast<std::uint32_t>(LF::AnisoLighting) })
						if (pbr || !glint)
							queue(View::LocalMap, Shader::Lighting, (technique << 24) | static_cast<std::uint32_t>(LF::VC) | pbr | seams | glint, Family::DefineStage::Pixel,
								kLightingLinearVariant, DrawState{ .write = 1, .depth = 3, .cull = 1, .stencil = 0 });
		// The water's LOD pass, with and without vertex colours.
		for (const std::uint32_t descriptor : { 0x4802u, 0x4803u })
			queue(View::LocalMap, Shader::Water, descriptor, Family::DefineStage::Vertex, 0, DrawState{ .write = 1, .depth = 1, .cull = 0, .stencil = 30, .stencilRef = 1 });
		logger::info("[DCLF] local map: {} programs queued at setup", queued);
	}

	void RenderLocalMap(const void* a_renderer)
	{
		auto& state = GetLocalMap();
		// Outside the UI stage that holds its clients (a frame that closes the map) nothing of the local map can be drawn.
		if (!SceneDraws::Available() || !SceneDraws::CanTake(View::LocalMap))
			return;
		const auto scene = CollectLocalMap(a_renderer);
		if (!scene.camera)
			return;
		const auto& camera = *scene.camera;
		SetLocalMapTreeDetail();
		const auto in = SampleLocalMapInputs(camera);
		const auto water = SampleLocalMapWater(in, scene.water);
		const auto mapView = LocalMapUtilityView(View::LocalMap, camera, in);

		// The frame's family resources start before the draws lease theirs. The render clears kMAIN's depth and stencil and kLOCAL_MAP_SWAP to
		// (0, 0, 0, 1): only the fog of war writes alpha.
		Family::BeginBatch();
		if (!::RenderGraph::Engine::Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL))
			::RenderGraph::Fatal("local map", "kMAIN's depth clear was not taken");
		constexpr float kClear[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
		if (!::RenderGraph::Engine::Skeleton::ClearColour(RE::RENDER_TARGET::kLOCAL_MAP_SWAP, kClear))
			::RenderGraph::Fatal("local map", "kLOCAL_MAP_SWAP's clear was not taken");

		// The land and objects batch, then its water (the post-resolve group after them, FinishAccumulatingPostResolveDepth).
		thread_local std::vector<RE::BSGeometry*> visible;
		LocalMapVisible(scene.roots, camera, visible);
		auto run = std::make_shared<Run>();
		run->view = View::LocalMap;
		std::uint32_t frame = ~0u;
		std::vector<const RE::BSGeometry*> waters;
		constexpr std::array<std::int32_t, kMaxColours> kTargets{ kLocalMapSwapTarget, -1, -1 };
		for (auto* geometry : visible) {
			std::string detail;
			const char* why = nullptr;
			switch (LocalMapPassOf(*geometry, View::LocalMap, scene.water)) {
			case LocalMapPass::Utility:
				why = AddUtility(mapView, *geometry, *run, detail);
				break;
			case LocalMapPass::Land:
				why = AddLighting(in, *geometry, kTargets, *run, frame, detail);
				break;
			case LocalMapPass::Water:
				waters.push_back(geometry);
				break;
			case LocalMapPass::Tree:
				why = "tree LOD (BSDistantTreeShader), which the local map's port does not draw";
				break;
			default:
				break;
			}
			if (why)
				Refused("land and objects", *geometry, why, detail);
		}
		for (const auto* geometry : waters) {
			std::string detail;
			if (const char* why = AddWater(water, *geometry, mapView, *run, detail))
				Refused("water", *geometry, why, detail);
		}
		const auto mapDraws = run->draws.size();
		SceneDraws::Hand(std::move(run));

		// The fog of war batch: its root's Utility passes in render mode 0x13, alpha only.
		std::size_t fogDraws = 0;
		if (scene.fogOfWar && scene.fogRoot) {
			const auto fogView = LocalMapUtilityView(View::LocalMapFog, camera, in);
			const LocalMapRoot fogRoot{ scene.fogRoot, false };
			LocalMapVisible(std::span(&fogRoot, 1), camera, visible);
			auto fog = std::make_shared<Run>();
			fog->view = View::LocalMapFog;
			for (auto* geometry : visible) {
				if (LocalMapPassOf(*geometry, View::LocalMapFog, scene.water) != LocalMapPass::Utility)
					continue;
				std::string detail;
				if (const char* why = AddUtility(fogView, *geometry, *fog, detail))
					Refused("fog of war", *geometry, why, detail);
			}
			fogDraws = fog->draws.size();
			SceneDraws::Hand(std::move(fog));
		}

		// ISLocalMap from kLOCAL_MAP_SWAP into kLOCAL_MAP, its row 1 / the local map's size.
		static const REL::Relocation<const std::uint32_t*> width{ RELOCATION_ID(524976, 411457) }, height{ RELOCATION_ID(524977, 411458) };
		if (!::RenderGraph::Engine::ImageSpace::LocalMapEffect(1.0f / static_cast<float>(*width.get()), 1.0f / static_cast<float>(*height.get())))
			::RenderGraph::Fatal("local map", "ISLocalMap could not be staged");
		if (state.renders++ == 0)
			logger::info("[DCLF] local map: the first native render: {} visible, {} land and object draws, {} fog of war draws", visible.size(), mapDraws, fogDraws);
	}

	DWORD Render(RE::UI3DSceneManager* a_manager, std::int32_t a_scheme, RE::NiCamera* a_camera, bool a_keepDepth)
	{
		if (GetLocalMapQueue().pending)
			QueueLocalMapPrograms();
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
		// The light update dims each light by the culling process's frustum and camera (AE 0x141508eb0), which the engine's cull of its
		// last render left on the menu's culling process. CS does not cull with it, so it sets them for the menu camera first.
		if (auto* culler = a_manager->cullingProcess) {
			culler->camera = camera;
			culler->SetFrustum(&camera->GetRuntimeData2().viewFrustum);
		}
		// The menu scenes' scene graph is the shader manager's second (State +0x8, AE 390952), whose queued lights become active here.
		updateLights(RE::BSShaderManager::State::GetSingleton().shadowSceneNode[1], a_manager->cullingProcess);

		// The draw half: the colour pass from the scene graph into CS's frame.
		auto& native = GetNative();
		// Scheme 4 with its depth pass renders the shadow lights' maps (RenderShadowsAndDepth); otherwise they are plain lights of their passes.
		const bool shadowMaps = scheme == 4 && a_keepDepth;
		const auto in = SampleInputs(*a_manager, *camera, native.previous, shadowMaps);
		const bool closestPoint = SortsByClosestPoint(*a_manager);
		std::vector<ScenePass> passes;
		SceneGeometry(root, *camera, closestPoint, passes);

		const auto targets = ColourTargets(scheme);
		// The frame's family resources start before the draws lease theirs, then kMAIN's depth and stencil clear, as at the render's start.
		Family::BeginBatch();
		if (!::RenderGraph::Engine::Skeleton::ClearDepth(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL))
			::RenderGraph::Fatal("menu scenes", "kMAIN's depth clear at a native menu scene render was not taken");
		// Scheme 4 draws into kMAIN, which the render binds with a clear to black and zero alpha first (SetRenderTarget(0, kMAIN, clear)
		// after the clear colour (0, 0, 0, 0)); kMAIN otherwise holds the last frame's image.
		constexpr float kBlack[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		if (scheme == 4 && !::RenderGraph::Engine::Skeleton::ClearColour(RE::RENDER_TARGET::kMAIN, kBlack))
			::RenderGraph::Fatal("menu scenes", "kMAIN's colour clear at a native menu scene render was not taken");
		// The shadow maps, the depth pass the colour pass keeps, and the masks its Lighting draws read.
		if (shadowMaps)
			RenderShadowsAndDepth(*a_manager, *camera, root, in, passes);
		auto run = std::make_shared<Run>();
		run->view = View::MenuColour;
		std::uint32_t frame = ~0u;
		std::string leftOut;  // the last draw left out of this render, for the log
		for (const auto& [geometry, membrane] : passes) {
			const auto* property = geometry->GetGeometryRuntimeData().shaderProperty.get();
			if (!property)
				continue;
			std::string detail;
			const char* why = nullptr;
			if (membrane)
				why = AddMembrane(in, *geometry, closestPoint, targets, *run, frame, detail);
			else if (netimmerse_cast<const RE::BSLightingShaderProperty*>(property))
				why = BlendedList(*geometry) && property->alpha < 1.0f ? "a blended Lighting draw that fades (accumulation hint 9 or 10)" :
				                                                         AddLighting(in, *geometry, targets, *run, frame, detail);
			else if (netimmerse_cast<const RE::BSEffectShaderProperty*>(property)) {
				EffectResult result;
				why = ComputeEffect(in, *geometry, result, detail) ?
				          AddEffect(in, *geometry, result, StateOf(*geometry, *property, true, targets[0]), targets, *run, frame, detail) :
				      detail.starts_with("no shader") ? kBuilding :
				                                        "its Effect constants could not be computed";
			} else
				why = "a shader the native menu scene has no program for";
			// A draw whose program is still building is left out until it is built, as the engine draw families do (the manifest
			// prebuilds it next session).
			if (why == kNoBuffers || why == kStreaming || why == kBuilding)
				leftOut = std::format("{}: {} ({})", geometry->name.c_str() ? geometry->name.c_str() : "?", why, detail);
			if (why == kNoBuffers)
				continue;
			// A texture still streaming in leaves its draw out of this render, as the engine draw families leave the batch for a later one.
			if (why == kStreaming) {
				if (native.streaming++ == 0)
					logger::info("[DCLF] menu scenes: scheme {} draw {} is left out while a texture streams in ({})", scheme, geometry->name.c_str() ? geometry->name.c_str() : "?", detail);
				continue;
			}
			if (why == kBuilding) {
				if (native.building++ == 0)
					logger::info("[DCLF] menu scenes: scheme {} draw {} is left out while its program builds", scheme, geometry->name.c_str() ? geometry->name.c_str() : "?");
				continue;
			}
			// Decision 149: a scheme is owned only when CS computes every draw in it.
			if (why)
				::RenderGraph::Fatal("menu scenes", std::format("scheme {} draw {}: {} ({})", scheme, geometry->name.c_str() ? geometry->name.c_str() : "?", why, detail));
		}
		const auto draws = run->draws.size();
		const bool taken = SceneDraws::Hand(std::move(run));
		// A light scheme with an ImageSpace override (manager +0xB0) runs the HDR effect from kMAIN into the back buffer, as the engine's
		// RenderEndOfFrameEffects(1, 0) here does. Without one, and with the engine's TAA off (decision 168), nothing follows.
		const auto* imageSpace = RE::ImageSpaceManager::GetSingleton();
		if (imageSpace && *reinterpret_cast<const std::uintptr_t*>(reinterpret_cast<const std::byte*>(imageSpace) + 0xB0) &&
			!::RenderGraph::Engine::ImageSpace::MenuSceneHdr(kMainTarget, kFramebufferTarget))
			::RenderGraph::Fatal("menu scenes", std::format("scheme {}: the menu scene's HDR effect could not be staged", scheme));
		native.previous = { true, { in.camera.viewProj, in.eye } };
		++native.renders;
		if (static_cast<std::uint32_t>(scheme) < native.schemesLogged.size()) {
			auto& count = native.schemeRenders[scheme];
			++count;
			if (draws && !std::exchange(native.schemesLogged[scheme], true))
				logger::info("[DCLF] menu scenes: the first native render of scheme {} with draws (keep depth {}, render {}): {} passes, {} colour draws{}", scheme, a_keepDepth,
					count, passes.size(), draws, taken ? "" : " (its client cannot take them; nothing drawn)");
			if (count == 300 && !native.schemesLogged[scheme])
				logger::warn("[DCLF] menu scenes: scheme {} drew nothing in 300 renders ({} passes; last left out: {})", scheme, passes.size(), leftOut.empty() ? "none" : leftOut);
		}
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
