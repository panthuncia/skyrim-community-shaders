// Dynamic Cubemaps: its reflections and environment cubemaps (t30, t31), frame textures. The render thread's callback reads the feature live.
// With CS_DCLF_GRAPH_TEXTURE_TEST, t30 reads the graph texture test's cubemap instead (ReflectionsGraphTest.h).

#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/DynamicCubemaps.h"
#include "Features/DrawcallLimitFix/Core/Extensions/IntegrationStatus.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/ReflectionsGraphTest.h"

#include "Features/DynamicCubemaps.h"

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

		// The test's t30: its cubemap's D3D11 view once it exists (both modes; the faces always read it), else Dynamic Cubemaps' own.
		void* TestReflectionsView()
		{
			auto* view = ReflectionsGraphTest::View();
			return view ? view : ReflectionsView();
		}

		void Install(Api::InstallContext& a_context)
		{
			const auto testSwitch = a_context.RegisterSwitch({ .name = "CS_DCLF_GRAPH_TEXTURE_TEST", .kind = Api::SwitchKind::Test,
				.reduced = Api::SwitchReduced::Never,
				.description = "graph: t30 reads a copy of the reflections cubemap made at the Prepass anchor, through the render graph; d3d11: the "
							   "same copy through its D3D11 view (their screenshots must match)" });
			const auto& value = SwitchValue(testSwitch);
			const auto mode = value == "graph" ? ReflectionsGraphTest::Mode::Graph :
			                  value == "d3d11" ? ReflectionsGraphTest::Mode::D3D11 :
			                                     ReflectionsGraphTest::Mode::Off;
			ReflectionsGraphTest::Install(mode);
			if (mode == ReflectionsGraphTest::Mode::Off)
				a_context.RegisterFrameTexture({ .name = "dynamic cubemaps reflections", .textureRegister = kReflectionsRegister,
					.view = &ReflectionsView, .capturedWhenNull = true });
			else
				a_context.RegisterFrameTexture({ .name = "dynamic cubemaps reflections", .textureRegister = kReflectionsRegister,
					.view = &TestReflectionsView, .capturedWhenNull = true, .graphResource = ReflectionsGraphTest::kResourceId,
					.graph = &ReflectionsGraphTest::GraphIndex });
			a_context.RegisterFrameTexture({ .name = "dynamic cubemaps environment", .textureRegister = kEnvironmentRegister,
				.view = &EnvironmentView, .capturedWhenNull = true });
		}
	}

	Api::Feature Integration()
	{
		return { .name = "dynamic cubemaps", .install = &Install, .status = [] { return FeatureStatus(globals::features::dynamicCubemaps); } };
	}
}
