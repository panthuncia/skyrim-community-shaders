// Dynamic Cubemaps: its reflections and environment cubemaps (t30, t31) as frame textures. The callback reads the feature live.
// On the feature's render-graph path the views are its BC6H cubemaps' exports, so commits read the published cubemaps.

#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"

#include "Features/DynamicCubemaps.h"
#include "Features/DynamicCubemaps/ORGDynamicCubemaps.h"

namespace DCLF::Integrations::DynamicCubemaps
{
	namespace
	{
		// The view PostDeferred binds at t31: a persistent BC6H cubemap made at setup, so last frame's binding. Null without the feature.
		void* EnvironmentView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded && dynamicCubemaps.envTextureBC6H ? dynamicCubemaps.envTextureBC6H->srv.get() : nullptr;
		}

		// The view PostDeferred last bound at t30, last frame's choice. Null before its first PostDeferred or without the feature.
		void* ReflectionsView()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsView : nullptr;
		}

		// The published id of that view: which of the two cubemaps PostDeferred picked. Empty when it bound none.
		std::string_view ReflectionsId()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsId : std::string_view{};
		}

		// The environment cubemap's id while PostDeferred binds it at t31.
		std::string_view EnvironmentId()
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			return dynamicCubemaps.loaded && dynamicCubemaps.cubemapValid[0] ? ORGDynamicCubemaps::kEnvBC6HId : std::string_view{};
		}

		void Install(Api::InstallContext& a_context)
		{
			// t30 holds the reflections cubemap while water reflections were active, else the environment one.
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps reflections", .textureRegister = kReflectionsRegister,
				.view = &ReflectionsView, .capturedWhenNull = true,
				.graphResources = { ORGDynamicCubemaps::kEnvReflectionsBC6HId, ORGDynamicCubemaps::kEnvBC6HId }, .graphId = &ReflectionsId });
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps environment", .textureRegister = kEnvironmentRegister,
				.view = &EnvironmentView, .capturedWhenNull = true, .graphResources = { ORGDynamicCubemaps::kEnvBC6HId }, .graphId = &EnvironmentId });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "dynamic cubemaps", .install = &Install, .status = [] { return FeatureStatus(globals::features::dynamicCubemaps); } };
	}
}
