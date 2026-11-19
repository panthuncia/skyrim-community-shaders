// The Effect port's program family, built from Effect.hlsl.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"

namespace DCLF::Features::Effects
{
	namespace
	{
		/** @brief Null before the shader exists or when the global is not an Effect shader. Safe from any thread. */
		const void* EffectShader()
		{
			static const REL::Relocation<RE::BSShader**> instance{ REL::Offset(Address::kEffectShader) };
			auto* shader = *instance.get();
			return shader && shader->shaderType.get() == RE::BSShader::Type::Effect ? shader : nullptr;
		}
	}

	void RegisterPrograms(Api::InstallContext& a_context)
	{
		// A first-person view draws effects forward only. Must register after the port.
		a_context.RegisterProgramFamily({
			.family = Api::ProgramFamily::Effect,
			.shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Effect),
			.name = "Effect",
			.keyName = "effect",
			.sourcePath = "Data/Shaders/Effect.hlsl",
			.nativeNote = "effect meshes stay native",
			.stages = Api::ProgramStages::VertexPixel,
			.build = Api::ProgramBuild::ForwardOrDecal,
			.rowBlocks = false,
			.rootViewsForwardOnly = true,
			.turnedKeyBits = true,
			.records = { .suppressEmittance = kRecordEffectSuppressEmittance,
				.turned = kRecordEffectBillboard,
				.turnedModeShift = kRecordEffectBillboardModeShift,
				.alphaRefPassBits = EffectFlag::Blood },
			.programSource = &EffectShader,
		});
	}
}
