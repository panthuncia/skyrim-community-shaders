#include "RenderGraph/DepthViewSource.h"
#include "Snapshot/EngineReads.h"

#include "Globals.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"

namespace RenderGraph
{
	DepthViewSource DepthViewSource::Of(ID3D11ShaderResourceView* a_view)
	{
		namespace Tripwire = RenderGraph::Engine::Tripwire;
		namespace Targets = RenderGraph::Engine::Targets;
		DepthViewSource source;
		if (!a_view || !CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer))
			return source;
		if (Tripwire::IsHandle(a_view)) {
			const auto target = Targets::Identify(static_cast<const ID3D11View*>(a_view));
			if (target && target->kind == Targets::Kind::Depth)
				source.engineDepth = target->index;
			else
				source.other.copy_from(a_view);
			return source;
		}
		winrt::com_ptr<ID3D11Resource> resource;
		a_view->GetResource(resource.put());
		if (!resource) {
			source.other.copy_from(a_view);
			return source;
		}
		const auto& depthStencils = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetDepthStencilData().depthStencils;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++i)
			if (depthStencils[i].texture && Tripwire::Unwrap(depthStencils[i].texture) == Tripwire::Unwrap(static_cast<ID3D11Texture2D*>(resource.get()))) {
				source.engineDepth = i;
				return source;
			}
		source.other.copy_from(a_view);
		return source;
	}

	FrameAnchors::Texture DepthViewSource::Resolve(const char* a_id) const
	{
		if (engineDepth != UINT32_MAX)
			return FrameAnchors::EngineTexture(FrameAnchors::Depth(engineDepth));
		if (!other)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		other->GetResource(resource.put());
		auto texture = resource.try_as<ID3D11Texture2D>();
		if (!texture)
			return nullptr;
		RE::BSGraphics::DepthStencilData target{};
		target.texture = texture.get();
		target.depthSRV = other.get();
		return FrameAnchors::ImportTexture(a_id, { .depth = &target });
	}
}
