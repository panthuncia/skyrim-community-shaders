#pragma once

#include <memory>
#include <rhi.h>

class DX12GraphHost
{
public:
	static std::unique_ptr<DX12GraphHost> Create(rhi::Device device);
	~DX12GraphHost();

	DX12GraphHost(const DX12GraphHost&) = delete;
	DX12GraphHost& operator=(const DX12GraphHost&) = delete;

private:
	class Impl;
	explicit DX12GraphHost(std::unique_ptr<Impl> impl);
	std::unique_ptr<Impl> impl;
};
