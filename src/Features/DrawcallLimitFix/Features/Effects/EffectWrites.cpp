// Hooks for effect controller updates and effect material rewrites. Each write is reported to Core so it can recapture the material
// or resample the shading. Runs on whichever thread runs the engine's update.

#include "Features/DrawcallLimitFix/Api/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialWrites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "RenderGraph/Fatal.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <vector>

namespace DCLF::Features::Effects
{
	namespace
	{
		// The controller's type field at +0x50 says which member it writes.
		std::uint32_t ControllerType(const RE::NiTimeController* a_controller)
		{
			return *reinterpret_cast<const std::uint32_t*>(reinterpret_cast<const std::byte*>(a_controller) + 0x50);
		}

		// What an effect controller can write in BSEffectShaderMaterial. Texture transforms, falloff, and base colour.
		constexpr std::size_t kEffectWrittenFirst = 0x0c, kEffectWrittenEnd = 0x58;
		constexpr std::size_t kEffectBaseAlpha = 0x54;  // baseColor.alpha, read by the shading
		using EffectWritten = std::array<std::byte, kEffectWrittenEnd - kEffectWrittenFirst>;
		EffectWritten EffectWrittenOf(const RE::BSShaderMaterial* a_material)
		{
			EffectWritten out{};
			if (a_material)
				std::memcpy(out.data(), reinterpret_cast<const std::byte*>(a_material) + kEffectWrittenFirst, out.size());
			return out;
		}
		// Kept on a per-thread stack because updates can nest and the bytes outgrow the event's scratch.
		struct Update
		{
			RE::BSShaderProperty* property;
			RE::BSShaderMaterial* material;
			EffectWritten before;
			bool propertyColour;
			bool captured;  // the Effect class was on at PRE
		};
		thread_local std::vector<Update> updates;

		// A colour controller of any other type writes a property colour (table 0x1435ef2e8), so the shading is resampled.
		void EffectControllerUpdate(const SWF_EventHeader* a_event, bool a_propertyColour)
		{
			if (SceneWriter::IsPre(a_event)) {
				// No effect record exists while the toggle is off, so nothing is captured.
				auto* controller = SceneWriter::Arg<RE::NiTimeController*>(a_event, 0);
				auto* property = static_cast<RE::BSShaderProperty*>(controller->target);
				auto* material = property ? property->material : nullptr;
				const bool captured = Extensions::ObjectClassOn(Api::ObjectClass::Effect);
				updates.push_back({ property, material, captured ? EffectWrittenOf(material) : EffectWritten{}, a_propertyColour, captured });
				return;
			}
			const Update update = updates.back();
			updates.pop_back();
			auto* property = update.property;
			auto* material = update.material;
			if (!update.captured || !property)
				return;
			// Bytes a feature patches every frame belong to the frame, not the record, so a write confined to them captures nothing.
			// A swapped material is always captured.
			const auto& before = update.before;
			const auto after = material ? EffectWrittenOf(material) : EffectWritten{};
			const bool swapped = property->material != material;
			const bool moved = material && (Extensions::FrameVSVariablePatched() ?
												   Extensions::DifferOutsideFrameSpans(kMaterialKindEffect, before.data(), after.data(), kEffectWrittenFirst, kEffectWrittenEnd) :
												   std::memcmp(after.data(), before.data(), after.size()) != 0);
			if ((swapped || moved) && property->material)
				Api::NoteMaterialWritten(property->material);
			// The shading reads only the base colour's alpha from these bytes.
			const auto alpha = kEffectBaseAlpha - kEffectWrittenFirst;
			if (swapped || update.propertyColour || (material && std::memcmp(after.data() + alpha, before.data() + alpha, sizeof(float)) != 0))
				Api::NotePropertyShading(property);
		}

		void OnEffectFloatController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			EffectControllerUpdate(a_event, false);
		}

		void OnEffectColorController(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			EffectControllerUpdate(a_event, SceneWriter::IsPre(a_event) && ControllerType(SceneWriter::Arg<RE::NiTimeController*>(a_event, 0)) != 0);
		}

		// BSEffectShaderMaterial::CopyMembers (0x1414e9cf0) rewrites the material in place.
		void OnEffectCopyMembers(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Api::NoteMaterialWritten(SceneWriter::Arg<RE::BSShaderMaterial*>(a_event, 0));
		}
	}

	void InstallWrites()
	{
		// Installed whatever the toggle. SceneWriterFramework's effect calls were required at PostPostLoad.
		const bool subscribed = SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_FLOAT_CONTROLLER_UPDATE, &OnEffectFloatController) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_COLOR_CONTROLLER_UPDATE, &OnEffectColorController) &&
		                        SceneWriter::SubscribeCall(SWF_HOOK_EFFECT_MATERIAL_COPY_MEMBERS, &OnEffectCopyMembers, SWF_PHASE_POST);
		if (!subscribed)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused the effect write hooks after reporting them installed");
		logger::info("[DCLF] effect write hooks subscribed: the effect controllers and material");
	}
}
