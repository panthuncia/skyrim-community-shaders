// Dynamic Cubemaps: its reflections and environment cubemaps (t30, t31), frame textures. The render thread's callback reads the feature live.
// On the feature's render-graph path (CS_ORG_DYNAMIC_CUBEMAPS) both registers read its published BC6H cubemaps through the graph; the
// views with frame constants of their own (the reflection's faces) keep reading their D3D11 exports.

#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/DynamicCubemaps.h"
#include "RenderGraph/PublishedResources.h"

namespace DCLF::Integrations::DynamicCubemaps
{
	namespace
	{
		// The view PostDeferred binds at t31 every frame it runs (the only PS binding there): its BC6H environment cubemap, a persistent
		// texture made at setup, so the binding a colour commit captured, last frame's. Null without the feature: the captured view.
		void* EnvironmentView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded && dynamicCubemaps.envTextureBC6H ? dynamicCubemaps.envTextureBC6H->srv.get() : nullptr;
		}

		// The view PostDeferred last bound at t30 (the reflections cubemap while water reflections were active, else the environment one):
		// last frame's choice, the binding a colour commit captured. Null before its first PostDeferred or without the feature: the captured view.
		void* ReflectionsView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsView : nullptr;
		}

		// The ids ORGDynamicCubemaps publishes its BC6H cubemaps under (the D3D11 path's texture names).
		constexpr const char* kEnvironmentId = "DynamicCubemaps::EnvTextureBC6H";
		constexpr const char* kReflectionsId = "DynamicCubemaps::EnvReflectionsBC6H";

		// t31 on the graph path: the published environment cubemap. False on the D3D11 path: EnvironmentView.
		bool EnvironmentGraphIndex(std::uint32_t& a_index)
		{
			return globals::features::dynamicCubemaps.loaded && PublishedResources::ShaderResourceIndex(kEnvironmentId, a_index);
		}

		// t30 on the graph path: the published cubemap of the two that PostDeferred last bound (t31 declares the environment one, this
		// register the reflections one). False without one bound, or on the D3D11 path: ReflectionsView.
		bool ReflectionsGraphIndex(std::uint32_t& a_index)
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			auto* bound = dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsView : nullptr;
			if (!bound)
				return false;
			if (dynamicCubemaps.envReflectionsTextureBC6H && bound == dynamicCubemaps.envReflectionsTextureBC6H->srv.get())
				return PublishedResources::ShaderResourceIndex(kReflectionsId, a_index);
			if (dynamicCubemaps.envTextureBC6H && bound == dynamicCubemaps.envTextureBC6H->srv.get())
				return PublishedResources::ShaderResourceIndex(kEnvironmentId, a_index);
			return false;
		}

		void Install(Api::InstallContext& a_context)
		{
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps reflections", .textureRegister = kReflectionsRegister,
				.view = &ReflectionsView, .capturedWhenNull = true, .graphResource = kReflectionsId, .graph = &ReflectionsGraphIndex });
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps environment", .textureRegister = kEnvironmentRegister,
				.view = &EnvironmentView, .capturedWhenNull = true, .graphResource = kEnvironmentId, .graph = &EnvironmentGraphIndex });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "dynamic cubemaps", .install = &Install, .status = [] { return FeatureStatus(globals::features::dynamicCubemaps); } };
	}
}
