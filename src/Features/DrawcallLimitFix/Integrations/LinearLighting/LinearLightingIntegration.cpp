// Linear Lighting: its material contributor, which writes SpecularColor in the working space as LightingMaterialUpload does,
// and the working space the other ported upload hooks read.

#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include "Features/LinearLighting.h"
#include "Globals.h"
#include "ShaderCache.h"

namespace DCLF::Integrations::LinearLighting
{
	namespace
	{
		using LightingFlag = SIE::ShaderCache::LightingShaderFlags;

		constexpr std::string_view kName = "Linear Lighting";
		constexpr std::uint32_t Bit(LightingFlag a_flag) { return static_cast<std::uint32_t>(a_flag); }

		// Lighting's SpecularColor, the PS variable LightingMaterialUpload rewrites.
		constexpr std::uint32_t kPSSpecularColor = 25;
		constexpr std::uint16_t kSpecularColor = static_cast<std::uint16_t>(offsetof(RE::BSLightingShaderMaterialBase, specularColor));
		constexpr std::uint16_t kSpecularColorScale = static_cast<std::uint16_t>(offsetof(RE::BSLightingShaderMaterialBase, specularColorScale));

		// Shared between frames, so the frame's view hold stays the same while the space does.
		const std::shared_ptr<const WorkingSpace>& Space(bool a_acescg)
		{
			static const auto srgb = std::make_shared<const WorkingSpace>(WorkingSpace{ .acescg = false });
			static const auto acescg = std::make_shared<const WorkingSpace>(WorkingSpace{ .acescg = true });
			return a_acescg ? acescg : srgb;
		}

		// Render thread, at the frame's start. Active while Linear Lighting is on. The word is the space, so a change re-evaluates every record.
		void Sample(Api::MaterialContributorFrame& a_frame)
		{
			const auto* space = LiveWorkingSpace();
			a_frame.SetActive(space != nullptr);
			if (!space)
				return;
			a_frame.Hold(Space(space->acescg));
			a_frame.Word(0, space->acescg ? 1u : 0u);
		}

		bool Covers(const Api::MaterialContributorEvaluation&) { return true; }

		// LightingMaterialUpload: SpecularColor's rgb is the material's specular colour in the working space times its scale,
		// for Specular passes that are not True PBR. w stays SetupMaterial's.
		Api::MaterialOutcome Evaluate(Api::MaterialContributorEvaluation& a_evaluation)
		{
			const std::uint32_t pass = a_evaluation.Pass();
			const auto* space = a_evaluation.Held<WorkingSpace>();
			if (!space || !(pass & Bit(LightingFlag::Specular)) || (pass & Bit(LightingFlag::TruePbr)))
				return Api::MaterialOutcome::Made;
			float colour[3] = { a_evaluation.At<float>(kSpecularColor), a_evaluation.At<float>(kSpecularColor + 4), a_evaluation.At<float>(kSpecularColor + 8) };
			ToWorking(*space, colour);
			const float scale = a_evaluation.At<float>(kSpecularColorScale);
			float* out = a_evaluation.PSFloats() + LightingPSLayout().offset[kPSSpecularColor];
			for (std::uint32_t c = 0; c < 3; ++c)
				out[c] = colour[c] * scale;
			return Api::MaterialOutcome::Made;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterMaterialContributor({ .name = kName,
				.stage = Api::MaterialStage::After,
				.order = 0,
				.witnessWords = 1,
				.signatureBits = Bit(LightingFlag::Specular) | Bit(LightingFlag::TruePbr),
				.sample = &Sample,
				.covers = &Covers,
				.evaluate = &Evaluate });
		}
	}

	const WorkingSpace* LiveWorkingSpace()
	{
		const auto& linearLighting = globals::features::linearLighting;
		if (!linearLighting.loaded || !linearLighting.IsLinearLightingActive())
			return nullptr;
		return Space(linearLighting.IsACEScgActive()).get();
	}

	const WorkingSpace* FrameWorkingSpace(const Extensions::MaterialContributorSamples& a_samples)
	{
		// The contributors are resolved before any frame, and never change after.
		static const std::size_t index = [] {
			const auto list = Extensions::MaterialContributorList();
			for (std::size_t c = 0; c < list.size(); ++c)
				if (list[c].name == kName)
					return c;
			return list.size();
		}();
		if (index >= a_samples.contributors.size() || !a_samples.contributors[index].active)
			return nullptr;
		return static_cast<const WorkingSpace*>(a_samples.contributors[index].held.get());
	}

	void ToWorking(const WorkingSpace& a_space, float* a_rgb)
	{
		::LinearLighting::SRGBToWorking(a_rgb, a_space.acescg);
	}

	Api::Feature Integration()
	{
		return { .name = "linear lighting", .install = &Install, .status = [] { return FeatureStatus(globals::features::linearLighting); } };
	}
}
