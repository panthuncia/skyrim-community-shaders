#include "DX12GraphHost.h"

#include <Render/RenderGraph/RenderGraph.h>

class DX12GraphHost::Impl
{
public:
	explicit Impl(rhi::Device device) : graph(device) {}
	org::RenderGraph graph;
};

DX12GraphHost::DX12GraphHost(std::unique_ptr<Impl> implementation) : impl(std::move(implementation)) {}
DX12GraphHost::~DX12GraphHost() = default;

std::unique_ptr<DX12GraphHost> DX12GraphHost::Create(rhi::Device device)
{
	return std::unique_ptr<DX12GraphHost>(new DX12GraphHost(std::make_unique<Impl>(device)));
}
