#include "ConstantEvaluator.h"

#include <cstring>

#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::size_t kSetupMaterialSlot = 4;  // BSShader vtable slots (engine notes)
		constexpr std::size_t kSetupGeometrySlot = 6;
		constexpr std::uint32_t kPerMaterial = 1;
		constexpr std::uint32_t kPerGeometry = 2;

		using ShadowStateData = std::remove_reference_t<decltype(RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData())>;

		StageLayout MakeLayout(std::uint32_t a_count, std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> a_sizes,
			std::initializer_list<std::uint32_t> a_scratch)
		{
			StageLayout layout{};
			layout.count = a_count;
			layout.size.fill(4);
			for (auto [variable, size] : a_sizes)
				layout.size[variable] = static_cast<std::uint8_t>(size);
			// Variables nothing reads (the per-geometry point lights under Light Limit Fix) share one region at the end.
			constexpr std::uint32_t kScratchRegion = 228;
			std::uint32_t next = 0;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				if (std::find(a_scratch.begin(), a_scratch.end(), i) != a_scratch.end()) {
					layout.offset[i] = static_cast<std::uint8_t>(kScratchRegion);
					continue;
				}
				layout.offset[i] = static_cast<std::uint8_t>(next);
				next += layout.size[i];
			}
			return layout;
		}

		template <class Shader>
		void PointAt(Shader* a_shader, const StageLayout& a_layout, ConstantBlock* a_blocks)
		{
			for (std::uint32_t level = 0; level < 3; ++level)
				a_shader->constantBuffers[level].data = a_blocks[level].floats.data();
			for (std::uint32_t i = 0; i < a_layout.count && i < a_shader->constantTable.size(); ++i)
				a_shader->constantTable[i] = static_cast<std::int8_t>(a_layout.offset[i]);
		}

		ID3D11ShaderResourceView* TextureSentinel(std::uint32_t a_slot)
		{
			return reinterpret_cast<ID3D11ShaderResourceView*>(static_cast<std::uintptr_t>(0xdc1f0000u + a_slot));
		}

		template <class Fn, class... Args>
		void CallVirtual(RE::BSShader* a_shader, std::size_t a_slot, Args... a_args)
		{
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_shader);
			reinterpret_cast<Fn>(vtable[a_slot])(a_shader, a_args...);
		}
	}

	const StageLayout& LightingVSLayout()
	{
		// World, PreviousWorld and TextureProj are float3x4; Bones (skinned only) never needs real storage.
		static const StageLayout layout = MakeLayout(kLightingVSVariables, { { 0, 12 }, { 1, 12 }, { 6, 12 } }, { 16 });
		return layout;
	}

	const StageLayout& LightingPSLayout()
	{
		// DirectionalAmbient is float3x4; PointLightPosition/Color[7] (1, 2) are unused with Light Limit Fix.
		static const StageLayout layout = MakeLayout(kLightingPSVariables, { { 5, 12 }, { 1, 28 }, { 2, 28 } }, { 1, 2 });
		return layout;
	}

	void ConstantBlock::Reset()
	{
		floats.fill(std::bit_cast<float>(kUnwrittenBits));
	}

	ConstantEvaluator& ConstantEvaluator::Get()
	{
		static ConstantEvaluator evaluator;
		return evaluator;
	}

	template <class Call>
	bool ConstantEvaluator::RunStandIn(std::uint32_t a_level, std::uint32_t a_passDescriptor, ConstantBlock& a_vs, ConstantBlock& a_ps, Call&& a_call)
	{
		if (!lightingShader)
			return false;

		auto& state = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
		auto* context = globals::d3d::context;
		auto* shader = static_cast<RE::BSLightingShader*>(lightingShader);

		// Stand-in shaders: constant tables point into ConstantBlocks, constant buffers are null, so the
		// engine writes straight into the blocks instead of mapping anything.
		alignas(16) static std::uint8_t vsStorage[sizeof(RE::BSGraphics::VertexShader)];
		alignas(16) static std::uint8_t psStorage[sizeof(RE::BSGraphics::PixelShader)];
		static ConstantBlock vsBlocks[3];
		static ConstantBlock psBlocks[3];
		std::memset(vsStorage, 0, sizeof(vsStorage));
		std::memset(psStorage, 0, sizeof(psStorage));
		auto* vs = reinterpret_cast<RE::BSGraphics::VertexShader*>(vsStorage);
		auto* ps = reinterpret_cast<RE::BSGraphics::PixelShader*>(psStorage);
		for (std::uint32_t level = 0; level < 3; ++level) {
			vsBlocks[level].Reset();
			psBlocks[level].Reset();
		}
		PointAt(vs, LightingVSLayout(), vsBlocks);
		PointAt(ps, LightingPSLayout(), psBlocks);

		// Everything the shader functions may touch, to restore afterwards.
		auto savedState = std::make_unique<std::uint8_t[]>(sizeof(state));
		std::memcpy(savedState.get(), &state, sizeof(state));
		const std::uint32_t savedTechnique = shader->currentRawTechnique;
		// Community Shaders' SetupGeometry hooks write its permutation data (external emittance, Extended
		// Translucency); it only reaches the GPU in State::Draw, so restoring it undoes the evaluation's
		// effect. (Light Limit Fix's hook uploads its StrictLightData and caches what it uploaded; that stays
		// consistent and is left alone.)
		const auto savedPermutation = globals::state->permutationData;
		ID3D11Buffer* savedVS = nullptr;
		ID3D11Buffer* savedPS = nullptr;
		context->VSGetConstantBuffers(a_level, 1, &savedVS);
		context->PSGetConstantBuffers(a_level, 1, &savedPS);

		state.currentVertexShader = vs;
		state.currentPixelShader = ps;
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			state.PSTexture[slot] = reinterpret_cast<std::remove_reference_t<decltype(state.PSTexture[slot])>>(TextureSentinel(slot));
			state.PSTextureFilterMode[slot] = static_cast<RE::BSGraphics::TextureFilterMode>(kUnwrittenFilterMode);
		}
		shader->currentRawTechnique = a_passDescriptor;

		evaluating = true;
		a_call(state);
		evaluating = false;

		a_vs = vsBlocks[a_level];
		a_ps = psBlocks[a_level];
		a_call.Collect(state);

		std::memcpy(&state, savedState.get(), sizeof(state));
		shader->currentRawTechnique = savedTechnique;
		globals::state->permutationData = savedPermutation;
		context->VSSetConstantBuffers(a_level, 1, &savedVS);
		context->PSSetConstantBuffers(a_level, 1, &savedPS);
		if (savedVS)
			savedVS->Release();
		if (savedPS)
			savedPS->Release();
		return true;
	}

	bool ConstantEvaluator::EvaluateMaterial(const RE::BSShaderMaterial* a_material, std::uint32_t a_passDescriptor, MaterialRecord& a_out)
	{
		if (!a_material)
			return false;

		struct Call
		{
			RE::BSShader* shader;
			const RE::BSShaderMaterial* material;
			MaterialRecord* out;

			void operator()(ShadowStateData&)
			{
				CallVirtual<void (*)(RE::BSShader*, const RE::BSShaderMaterial*)>(shader, kSetupMaterialSlot, material);
			}

			void Collect(ShadowStateData& a_state)
			{
				out->textureWritten = 0;
				for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
					auto* texture = reinterpret_cast<ID3D11ShaderResourceView*>(a_state.PSTexture[slot]);
					if (texture != TextureSentinel(slot)) {
						out->textures[slot] = texture;
						out->addressModes[slot] = static_cast<std::uint32_t>(a_state.PSTextureAddressMode[slot].underlying());
						out->textureWritten |= 1u << slot;
					} else {
						out->textures[slot] = nullptr;
						out->addressModes[slot] = 0;
					}
					out->filterModes[slot] = static_cast<std::uint32_t>(a_state.PSTextureFilterMode[slot].underlying());
				}
			}
		};
		return RunStandIn(kPerMaterial, a_passDescriptor, a_out.vs, a_out.ps, Call{ lightingShader, a_material, &a_out });
	}

	bool ConstantEvaluator::EvaluateGeometry(const RE::BSRenderPass& a_templatePass, std::uint32_t a_passDescriptor, std::uint32_t a_renderFlags, GeometryConstants& a_out)
	{
		struct Call
		{
			RE::BSShader* shader;
			RE::BSRenderPass pass;
			std::uint32_t renderFlags;

			void operator()(ShadowStateData&)
			{
				CallVirtual<void (*)(RE::BSShader*, RE::BSRenderPass*, std::uint32_t)>(shader, kSetupGeometrySlot, &pass, renderFlags);
			}

			void Collect(ShadowStateData&) {}
		};
		Call call{ lightingShader, a_templatePass, a_renderFlags };
		call.pass.next = nullptr;
		call.pass.passGroupNext = nullptr;
		call.pass.passEnum = a_passDescriptor + 0x4800002Du;
		return RunStandIn(kPerGeometry, a_passDescriptor, a_out.vs, a_out.ps, call);
	}

	void EvaluateTechnique(std::uint32_t a_passDescriptor, TechniqueConstants& a_out)
	{
		// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
		constexpr std::uint32_t kVSFogParam = 13;
		constexpr std::uint32_t kVSFogNearColor = 14;
		constexpr std::uint32_t kVSFogFarColor = 15;
		constexpr std::uint32_t kPSFogColor = 19;
		constexpr std::uint32_t kPSColourOutputClamp = 20;
		constexpr auto kAnisotropic = static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic);

		a_out.vs.Reset();
		a_out.ps.Reset();
		a_out.filterModes.fill(kUnwrittenFilterMode);

		// Sampler filter modes: diffuse and normal always, then per technique.
		const std::uint32_t technique = (a_passDescriptor >> 24) & 0x3f;
		a_out.filterModes[0] = a_out.filterModes[1] = kAnisotropic;
		switch (technique) {
		case 1:  // Envmap (and 0x10, not drawn): cube map and its mask
			a_out.filterModes[4] = a_out.filterModes[5] = kAnisotropic;
			break;
		case 2:  // Glowmap
			a_out.filterModes[6] = kAnisotropic;
			break;
		case 3:  // Parallax
			a_out.filterModes[3] = kAnisotropic;
			break;
		default:
			break;
		}

		// Shadowed directional or shadow-casting point lights with DefShadow: the shadow mask in t14
		// (clamped; point-filtered unless iShadowMaskQuarter is 4) and its inverse size in VPOSOffset.
		const bool shadowLights = (a_passDescriptor & (1u << 13)) || (a_passDescriptor & 0x1c0u);
		a_out.shadowMask = shadowLights && (a_passDescriptor & (1u << 14));
		a_out.shadowMaskTexture = nullptr;
		if (a_out.shadowMask) {
			constexpr std::uint32_t kPSVPOSOffset = 11;
			const auto& target = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kSHADOW_MASK];
			a_out.shadowMaskTexture = reinterpret_cast<ID3D11ShaderResourceView*>(target.SRV);
			static RE::Setting* quarter = RE::GetINISetting("iShadowMaskQuarter:Display");
			a_out.filterModes[kShadowMaskSlot] = (quarter && quarter->GetInteger() != 4) ? 1u : 0u;
			if (target.texture) {
				D3D11_TEXTURE2D_DESC desc{};
				reinterpret_cast<ID3D11Texture2D*>(target.texture)->GetDesc(&desc);
				float* offset = &a_out.ps.floats[LightingPSLayout().offset[kPSVPOSOffset]];
				offset[0] = 1.0f / static_cast<float>(desc.Width);
				offset[1] = 1.0f / static_cast<float>(desc.Height);
				offset[2] = 0.0f;
				offset[3] = 0.0f;
			}
		}

		// Fog from the current scene graph's fog property (FUN_1414dfad0 in AE).
		const auto& shaderState = RE::BSShaderManager::State::GetSingleton();
		auto* sceneNode = shaderState.shadowSceneNode[shaderState.sceneGraph];
		const auto* fog = sceneNode ? sceneNode->GetRuntimeData().fogProperty.get() : nullptr;
		if (fog) {
			const auto& vsLayout = LightingVSLayout();
			const auto& psLayout = LightingPSLayout();
			float* param = &a_out.vs.floats[vsLayout.offset[kVSFogParam]];
			if (fog->farDistance != 0.0f || fog->nearDistance != 0.0f) {
				const float inverseRange = 1.0f / (fog->farDistance - fog->nearDistance);
				param[0] = inverseRange * fog->nearDistance;
				param[1] = inverseRange;
				param[2] = fog->power;
				param[3] = fog->clamp;
			} else {
				param[0] = 5000000.0f;
				param[1] = 0.1f;
				param[2] = 1.0f;
				param[3] = 0.0f;
			}
			const float nearColor[4] = { fog->nearColor.red, fog->nearColor.green, fog->nearColor.blue, shaderState.invFrameBufferRange };
			std::memcpy(&a_out.vs.floats[vsLayout.offset[kVSFogNearColor]], nearColor, sizeof(nearColor));
			std::memcpy(&a_out.ps.floats[psLayout.offset[kPSFogColor]], nearColor, sizeof(nearColor));
			// The engine leaves w of FogFarColor as whatever was on its stack.
			float* farColor = &a_out.vs.floats[vsLayout.offset[kVSFogFarColor]];
			farColor[0] = fog->farColor.red;
			farColor[1] = fog->farColor.green;
			farColor[2] = fog->farColor.blue;
		}

		// ColourOutputClamp: the fLightingOutputColourClamp* settings, copied when the shader is created.
		static RE::Setting* clampSettings[3] = {
			RE::GetINISetting("fLightingOutputColourClampPostLit:General"),
			RE::GetINISetting("fLightingOutputColourClampPostEnv:General"),
			RE::GetINISetting("fLightingOutputColourClampPostSpec:General"),
		};
		float* clamp = &a_out.ps.floats[LightingPSLayout().offset[kPSColourOutputClamp]];
		for (std::uint32_t i = 0; i < 3; ++i)
			clamp[i] = clampSettings[i] ? clampSettings[i]->GetFloat() : 1.0f;
		clamp[3] = 0.0f;
	}
}
