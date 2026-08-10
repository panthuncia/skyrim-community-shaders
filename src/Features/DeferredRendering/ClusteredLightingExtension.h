#pragma once

#include <Render/RenderGraph/RenderGraph.h>
#include <Interfaces/IResourceProvider.h>

#include <memory>
#include <unordered_map>

#include "Features/DeferredRendering.h"

namespace org { class Buffer; }
class DX12LightCulling;

class ClusteredLightingExtension final : public org::RenderGraph::IRenderGraphExtension, public org::IResourceProvider
{
public:
	explicit ClusteredLightingExtension(DX12LightCulling& owner);
	void PrepareForBuild(org::RenderGraph& graph) override;
	void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& out) override;
	std::shared_ptr<org::Resource> ProvideResource(const org::ResourceIdentifier& key) override;
	std::vector<org::ResourceIdentifier> GetSupportedKeys() override;

	DX12LightCulling& Owner() const noexcept { return owner_; }
	std::uint32_t PageCapacity() const noexcept { return pageCapacity_; }
	std::shared_ptr<const DeferredRendering::FrameSnapshot> Snapshot() const noexcept { return snapshot_; }
	void UpdateSnapshot();

private:
	DX12LightCulling& owner_;
	std::unordered_map<org::ResourceIdentifier, std::shared_ptr<org::Resource>, org::ResourceIdentifier::Hasher> resources_;
	std::shared_ptr<const DeferredRendering::FrameSnapshot> snapshot_;
	std::uint32_t pageCapacity_{};
};
