// Advanced Skin's material textures: its SetupMaterial hook as an After material contributor, and SkinPerGeometry's per-draw buffer.
// The sample runs on the render thread at the frame's start. covers and evaluate are pure.

#include "Features/DrawcallLimitFix/Integrations/AdvancedSkin/AdvancedSkin.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialHookFrame.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <winrt/base.h>

#include "Features/Skin.h"
#include "Globals.h"

namespace DCLF::Integrations::AdvancedSkin
{
	namespace
	{
		static_assert(std::tuple_size_v<Skin::MaterialTextures> == kMaterialTextureRegisters.size());
		constexpr std::uint32_t kRfaos = 0;    // local index of t71
		constexpr std::uint32_t kWetness = 1;  // local index of t74

		constexpr auto kFaceGen = static_cast<std::uint32_t>(RE::BSShaderMaterial::Feature::kFaceGen);
		constexpr auto kFaceGenRGBTint = static_cast<std::uint32_t>(RE::BSShaderMaterial::Feature::kFaceGenRGBTint);
		constexpr auto kHashKey = static_cast<std::uint16_t>(offsetof(RE::BSShaderMaterial, hashKey));

		/**
		 * @brief Advanced Skin's t71 and t74 views for one material hash key, with references held.
		 * Skin drops its entries on an unload or a load while records made from this table still name the views.
		 */
		struct SkinTextures
		{
			std::uint32_t hashKey = 0;
			winrt::com_ptr<ID3D11ShaderResourceView> rfaos;    // t71
			winrt::com_ptr<ID3D11ShaderResourceView> wetness;  // t74
		};
		// The held table, sorted by hashKey and shared between frames while unchanged. A missing key is one Skin has not set up yet, so covers is false.
		using SkinTextureTable = std::vector<SkinTextures>;

		/** @brief Render thread. Skin::skinExtraTextures resolved as MaterialTexturesOf does. */
		std::shared_ptr<const SkinTextureTable> SampleSkinTextures(ID3D11ShaderResourceView* a_black)
		{
			static std::shared_ptr<const SkinTextureTable> held;
			static SkinTextureTable scratch;
			scratch.clear();
			for (const auto& [hashKey, entry] : globals::features::skin.skinExtraTextures) {
				SkinTextures textures{ hashKey };
				textures.rfaos.copy_from(a_black);
				textures.wetness.copy_from(a_black);
				if (entry.hasExtraTexture || entry.hasWetnessTexture) {
					textures.rfaos.copy_from(MaterialPort::ViewOfTexture(entry.rfaosTexture.get()));
					textures.wetness.copy_from(MaterialPort::ViewOfTexture(entry.wetnessTexture.get()));
				}
				scratch.push_back(std::move(textures));
			}
			std::sort(scratch.begin(), scratch.end(), [](const SkinTextures& a, const SkinTextures& b) { return a.hashKey < b.hashKey; });
			const auto same = [](const SkinTextures& a, const SkinTextures& b) {
				return a.hashKey == b.hashKey && a.rfaos.get() == b.rfaos.get() && a.wetness.get() == b.wetness.get();
			};
			if (!held || !std::equal(held->begin(), held->end(), scratch.begin(), scratch.end(), same))
				held = std::make_shared<const SkinTextureTable>(scratch);
			return held;
		}

		const SkinTextures* FindSkinTextures(const Api::MaterialContributorEvaluation& a_evaluation, std::uint32_t a_hashKey)
		{
			const auto* table = a_evaluation.Held<SkinTextureTable>();
			if (!table)
				return nullptr;
			const auto it = std::lower_bound(table->begin(), table->end(), a_hashKey, [](const SkinTextures& a, std::uint32_t k) { return a.hashKey < k; });
			return it != table->end() && it->hashKey == a_hashKey ? &*it : nullptr;
		}

		// Only called while active, meaning Skin's hook is installed and running.
		bool SkinApplies(const Api::MaterialContributorEvaluation& a_evaluation)
		{
			const auto feature = a_evaluation.Feature();
			return feature == kFaceGen || feature == kFaceGenRGBTint;
		}

		void Sample(Api::MaterialContributorFrame& a_frame)
		{
			const bool loaded = globals::features::skin.loaded;
			a_frame.SetActive(loaded);
			if (!loaded)
				return;
			auto table = SampleSkinTextures(a_frame.hook.defaultBlackView);
			// The table's identity is the witness, since it is shared between frames while unchanged. Unloaded, the words stay zero.
			const auto identity = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(table.get()));
			a_frame.Word(0, static_cast<std::uint32_t>(identity));
			a_frame.Word(1, static_cast<std::uint32_t>(identity >> 32));
			a_frame.Hold(std::move(table));
		}

		bool Covers(const Api::MaterialContributorEvaluation& a_evaluation)
		{
			if (!SkinApplies(a_evaluation))
				return true;
			const auto hashKey = a_evaluation.At<std::uint32_t>(kHashKey);
			return hashKey == 0 || FindSkinTextures(a_evaluation, hashKey) != nullptr;
		}

		Api::MaterialOutcome Evaluate(Api::MaterialContributorEvaluation& a_evaluation)
		{
			// FaceGen and FaceGenRGBTint materials only. No hash key or no extra texture binds black to both.
			// TerrainHelper's t92-t97 and ExtraFeatureDescriptor bits are not ported, because no shader samples t92-t97.
			const auto skin = [&](ID3D11ShaderResourceView* a_rfaos, ID3D11ShaderResourceView* a_wetness) {
				a_evaluation.FeatureTexture(kRfaos, a_rfaos);
				a_evaluation.FeatureTexture(kWetness, a_wetness);
			};
			skin(nullptr, nullptr);
			if (!SkinApplies(a_evaluation))
				return Api::MaterialOutcome::Made;
			const auto hashKey = a_evaluation.At<std::uint32_t>(kHashKey);
			if (hashKey == 0) {
				skin(a_evaluation.hook.defaultBlackView, a_evaluation.hook.defaultBlackView);
				return Api::MaterialOutcome::Made;
			}
			if (const auto* textures = FindSkinTextures(a_evaluation, hashKey))
				skin(textures->rfaos.get(), textures->wetness.get());
			// A key Skin has not set up yet stays null, so covers is false for it.
			return Api::MaterialOutcome::Made;
		}
	}

	void InstallMaterialTextures(Api::InstallContext& a_context)
	{
		// Outside vanilla and TruePBR. Its branches read the material's feature and hash key, not pass bits.
		a_context.RegisterMaterialContributor({ .name = "Advanced Skin",
			.stage = Api::MaterialStage::After,
			.order = 0,
			.textureRegisters = kMaterialTextureRegisters,
			.witnessWords = 2,
			.signatureBits = 0,
			.sample = &Sample,
			.covers = &Covers,
			.evaluate = &Evaluate });
		// SkinPerGeometry (b7), bound per draw. DCLF's draws take it from the shading row instead.
		a_context.DeclarePerDrawBuffer({ .name = "SkinPerGeometry", .stage = Api::ShaderStage::Pixel, .constantRegister = kSkinPerGeometryRegister });
	}
}
