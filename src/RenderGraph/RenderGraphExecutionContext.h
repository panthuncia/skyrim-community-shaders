#pragma once

#include <CommunityShaders/RenderGraphAPI.h>

#include <cstdint>
#include <unordered_map>
#include <memory>

namespace org { class Resource; namespace runtime { class IUploadService; } }

struct RenderGraphExecutionContextHost
{
	static constexpr uint64_t kMagic = 0x43535247484f5354ull; // "CSRGHOST"
	uint64_t magic{ kMagic };
	std::unordered_map<CSRGBinding, std::shared_ptr<org::Resource>> graphResourcesByBinding;
	std::unordered_map<CSRGBinding, CSRGBindingInfo> bindingInfo;
	org::runtime::IUploadService* graphUploads{};
	void* uploadUser{};
	CSRGStatus (*queueBufferUpload)(void*, CSRGBinding, uint64_t, const void*, uint64_t){};
};
