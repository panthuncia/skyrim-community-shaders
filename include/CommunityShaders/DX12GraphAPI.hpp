#pragma once

#include "DX12GraphAPI.h"
#include <span>
#include <stdexcept>
#include <utility>

namespace cs::dx12 {
inline constexpr const char* gbufferReady = CS_DX12_ANCHOR_GBUFFER_READY;
inline constexpr const char* deferredLightingBegin = CS_DX12_ANCHOR_DEFERRED_LIGHTING_BEGIN;

inline const char* StatusString(const CSDX12GraphAPI& api, CSDX12Status status) noexcept {
	return api.StatusString ? api.StatusString(status) : "unknown";
}

class Registration {
public:
	Registration() = default;
	Registration(const CSDX12GraphAPI* api, CSDX12RegistrationHandle handle) : api_(api), handle_(handle) {}
	Registration(const Registration&) = delete;
	Registration& operator=(const Registration&) = delete;
	Registration(Registration&& other) noexcept : api_(std::exchange(other.api_, nullptr)), handle_(std::exchange(other.handle_, 0)) {}
	Registration& operator=(Registration&& other) noexcept {
		if (this != &other) { Reset(); api_ = std::exchange(other.api_, nullptr); handle_ = std::exchange(other.handle_, 0); }
		return *this;
	}
	~Registration() { Reset(); }
	void Reset() noexcept { if (api_ && handle_) api_->UnregisterContributor(handle_); api_ = nullptr; handle_ = 0; }
	CSDX12RegistrationHandle Get() const noexcept { return handle_; }
private:
	const CSDX12GraphAPI* api_{};
	CSDX12RegistrationHandle handle_{};
};
}
