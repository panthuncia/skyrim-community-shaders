#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DeclaredTargets.h"

#include "Deferred.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Globals.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Targets/EngineTargetSet.h"
#include "RenderGraph/Fatal.h"

#include <d3d11.h>

#include <array>
#include <cstdint>

namespace DCLF::DeclaredTargets
{
	namespace
	{
		namespace Targets = RenderGraph::Engine::Targets;

		// The order Deferred::StartDeferred binds them: the engine's own two, then the G-buffer.
		constexpr std::array<RE::RENDER_TARGET, 8> kMainPassTargets{ RE::RENDER_TARGET::kMAIN, RE::RENDER_TARGET::kMOTION_VECTOR, NORMALROUGHNESS, ALBEDO, SPECULAR,
			REFLECTANCE, MASKS, MASKS2 };

		/** @brief The target set answers in graph modes. D3D11 mode reads the engine's fields, which CS's setup has just made. */
		bool FromSet()
		{
			return RenderGraph::Engine::GraphMode() && Targets::SetDeclared();
		}

		DXGI_FORMAT FieldColourFormat(RE::RENDER_TARGET a_target)
		{
			auto* renderer = globals::game::renderer;
			auto* view = renderer ? renderer->GetRuntimeData().renderTargets[a_target].RTV : nullptr;
			if (!view)
				return DXGI_FORMAT_UNKNOWN;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			view->GetDesc(&desc);
			return desc.Format;
		}

		DXGI_FORMAT FieldDepthFormat(std::uint32_t a_target)
		{
			auto* renderer = globals::game::renderer;
			auto* view = renderer ? renderer->GetDepthStencilData().depthStencils[a_target].views[0] : nullptr;
			if (!view)
				return DXGI_FORMAT_UNKNOWN;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			view->GetDesc(&desc);
			return desc.Format;
		}

		DXGI_FORMAT DepthFormat(std::uint32_t a_target)
		{
			return FromSet() ? Targets::ViewFormat(Targets::Depth(a_target, Targets::View::Dsv)) : FieldDepthFormat(a_target);
		}
	}

	TargetFormats MainPass()
	{
		TargetFormats formats;
		const bool fromSet = FromSet();
		for (std::uint32_t i = 0; i < kMainPassTargets.size(); ++i) {
			const auto target = kMainPassTargets[i];
			formats.colors[i] = fromSet ? Targets::ViewFormat(Targets::Colour(target, Targets::View::Rtv)) : FieldColourFormat(target);
			if (formats.colors[i] != DXGI_FORMAT_UNKNOWN)
				formats.colorCount = i + 1;
		}
		formats.depth = DepthFormat(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
		return formats;
	}

	DXGI_FORMAT ShadowMap()
	{
		const auto format = DepthFormat(RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM);
		return format != DXGI_FORMAT_UNKNOWN ? format : DepthFormat(RE::RENDER_TARGETS_DEPTHSTENCIL::kPRECIPITATION_OCCLUSION_MAP);
	}

	void Init()
	{
		auto& pipelines = DrawPipelines::Get();
		const auto formats = MainPass();
		// Every one of the eight is bound, so a gap is a target the set does not know.
		for (std::uint32_t i = 0; i < kMainPassTargets.size(); ++i)
			if (formats.colors[i] == DXGI_FORMAT_UNKNOWN)
				::RenderGraph::Fatal("Drawcall Limit Fix", fmt::format("main pass target {} (render target {}) has no declared format", i,
					static_cast<std::uint32_t>(kMainPassTargets[i])));
		pipelines.InitTargetFormats(formats);
	}

	void InitShadows()
	{
		// The engine makes its Utility shader when the game's data loads, after CS's setup.
		auto* utility = globals::game::utilityShader;
		if (!utility)
			::RenderGraph::Fatal("Drawcall Limit Fix", "the Utility shader is not loaded when the game's data has loaded");
		DrawPipelines::Get().InitShadowInputs(ShadowMap(), *utility);
	}
}
