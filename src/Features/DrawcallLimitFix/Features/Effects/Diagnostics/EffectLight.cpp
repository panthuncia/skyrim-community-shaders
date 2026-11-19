// CS_DCLF_SET_PARITY check of withheld Effect Lighting passes. Counted on the registering threads, taken on the render thread.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <atomic>

namespace DCLF::Features::Effects
{
	namespace
	{
		std::atomic<std::uint32_t> lightChecked{ 0 }, lightOther{ 0 }, lightCloud{ 0 };

		// Light 0 must be the light the port reads, the cloud light for kCloudLOD and otherwise the sun.
		void ListPass(const Diagnostics::MemberListPassWithheld& a_event)
		{
			const auto* pass = a_event.pass;
			if (!pass || pass->accumulationHint != 1)
				return;
			if (pass->shader && pass->shader->shaderType.get() == RE::BSShader::Type::Effect && ((pass->passEnum - kEffectPassEnumBase) & EffectFlag::Lighting)) {
				lightChecked.fetch_add(1, std::memory_order_relaxed);
				auto* sceneNode = globals::game::smState ? globals::game::smState->shadowSceneNode[0] : nullptr;
				const bool cloud = pass->shaderProperty && pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kCloudLOD);
				const RE::BSLight* expected = sceneNode ? (cloud ? sceneNode->GetRuntimeData().cloudLight : sceneNode->GetRuntimeData().sunLight) : nullptr;
				if (cloud)
					lightCloud.fetch_add(1, std::memory_order_relaxed);
				if (!pass->numLights || !pass->sceneLights || pass->sceneLights[0] != expected)
					lightOther.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}

	void SubscribeLight()
	{
		Diagnostics::Subscribe<Diagnostics::MemberListPassWithheld>(ListPass);
	}

	LightCounts TakeLight()
	{
		return { lightChecked.exchange(0, std::memory_order_relaxed), lightOther.exchange(0, std::memory_order_relaxed),
			lightCloud.exchange(0, std::memory_order_relaxed) };
	}
}
