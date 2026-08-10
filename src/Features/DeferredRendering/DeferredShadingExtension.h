#pragma once

#include <Render/RenderGraph/RenderGraph.h>
#include <Interfaces/IResourceProvider.h>

#include <memory>
#include <unordered_map>

class DX12DeferredShading;

class DeferredShadingExtension final : public org::RenderGraph::IRenderGraphExtension, public org::IResourceProvider
{
public:
	explicit DeferredShadingExtension(DX12DeferredShading& owner);
	void PrepareForBuild(org::RenderGraph& graph) override;
	void GatherStructuralPasses(org::RenderGraph& graph,
		std::vector<org::RenderGraph::ExternalPassDesc>& out) override;
	std::shared_ptr<org::Resource> Resource(const org::ResourceIdentifier& id) const;
	std::shared_ptr<org::Resource> ProvideResource(const org::ResourceIdentifier& id) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;
private:
	DX12DeferredShading& owner_;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>> resources_;
};
