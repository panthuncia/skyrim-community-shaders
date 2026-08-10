#pragma once

#include <CommunityShaders/RenderGraphAPI.h>

#include <cstdint>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

class RenderGraphRegistry
{
public:
	struct Resource
	{
		uint64_t handle{};
		uint64_t contributor{};
		std::string id;
		CSRGResourceDesc desc{};
	};
	struct Access
	{
		uint64_t resource{};
		std::string resourceId;
		CSRGBinding binding{};
		CSRGAccessKind kind{};
		CSRGSubresourceRange range{};
		CSRGViewKind viewKind{};
		CSRGViewDimension viewDimension{};
		CSRGFormat viewFormat{};
		uint32_t viewFlags{};
		uint64_t firstElement{};
		uint32_t elementCount{};
		uint32_t structureByteStride{};
		CSRGBinding counterBinding{};
	};
	struct Pass
	{
		uint64_t contributor{};
		std::string id;
		CSRGPassKind kind{};
		CSRGQueueAssignment queue{};
		uint32_t flags{};
		int32_t priority{};
		std::string technique;
		std::vector<std::string> featureDomains;
		std::vector<std::string> after;
		std::vector<std::string> before;
		std::vector<Access> accesses;
		CSRGPrepareCallback prepare{};
		CSRGUpdateCallback update{};
		CSRGExecuteCallback execute{};
		CSRGCleanupCallback cleanup{};
		void* userData{};
	};
	struct Candidate
	{
		uint64_t generation{};
		std::vector<Resource> resources;
		std::vector<Pass> passes;
		std::vector<uint64_t> contributors;
	};

	static RenderGraphRegistry& Get();
	CSRGStatus Register(const CSRGContributorDesc*, CSRGRegistrationHandle*) noexcept;
	CSRGStatus BeginUnregister(CSRGRegistrationHandle) noexcept;
	CSRGStatus GetRegistrationState(CSRGRegistrationHandle, CSRGRegistrationState*) const noexcept;
	CSRGStatus DeclareResource(CSRGBuildHandle, const CSRGResourceDesc*) noexcept;
	CSRGStatus DeclarePass(CSRGBuildHandle, const CSRGPassDesc*) noexcept;
	CSRGStatus RequestRebuild(CSRGRegistrationHandle) noexcept;
	CSRGStatus GetDiagnostic(CSRGRegistrationHandle, CSRGDiagnostic*) const noexcept;
	bool HasPendingRebuild() const noexcept;
	CSRGStatus Compile(uint64_t generation, uint32_t renderWidth, uint32_t renderHeight,
		uint32_t displayWidth, uint32_t displayHeight, Candidate&) noexcept;
	void Activate(const Candidate&) noexcept;
	void Retire(uint64_t generation) noexcept;
	void NotifyDeviceLost(uint32_t reason) noexcept;
	void Shutdown() noexcept;

private:
	struct Contributor;
	struct Build;
	RenderGraphRegistry() = default;
	void SetDiagnostic(CSRGRegistrationHandle, CSRGStatus, uint32_t phase, uint64_t generation, std::string) noexcept;
	static bool IsNamespaced(const char*) noexcept;
	mutable std::shared_mutex mutex_;
	mutable std::mutex buildMutex_;
	std::unordered_map<uint64_t, Contributor> contributors_;
	std::unordered_map<uint64_t, Contributor> unregistering_;
	std::unordered_map<uint64_t, CSRGDiagnostic> diagnostics_;
	std::unordered_map<uint64_t, uint32_t> generationReferences_;
	std::unordered_map<uint64_t, std::vector<uint64_t>> generationContributors_;
	Build* currentBuild_{};
	std::thread::id buildThread_{};
	std::atomic_uint64_t nextHandle_{ 1 };
	uint64_t diagnosticSequence_{};
	bool rebuildRequested_{ true };
	bool open_{ true };
};
