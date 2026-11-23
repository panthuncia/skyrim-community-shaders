// The render graph device's on-disk pipeline cache: its file and its saves.
#include "RenderGraph/PipelineCache.h"

#include "RenderGraph/Switches.h"
#include "Utils/SharedShaderStore.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace RenderGraph::PipelineCache
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		// Pipelines come in bursts after a load, so a save waits until a burst has gone quiet for this long.
		constexpr auto kQuiet = std::chrono::seconds(5);

		std::atomic<bool> saving{ false };
		std::atomic<bool> saveAsked{ false };  // RequestSave: the next tick saves without the quiet wait
		std::uint64_t savedMisses = 0;  // misses at the last save
		std::uint64_t lastMisses = 0;   // misses at the last tick
		Clock::time_point lastAdded{};
		bool reported = false;

		const char* LoadName(rhi::PipelineCacheLoadResult a_load)
		{
			switch (a_load) {
			case rhi::PipelineCacheLoadResult::Off: return "off";
			case rhi::PipelineCacheLoadResult::Empty: return "no file yet";
			case rhi::PipelineCacheLoadResult::Loaded: return "loaded";
			case rhi::PipelineCacheLoadResult::Stale: return "for another device or driver, replaced";
			case rhi::PipelineCacheLoadResult::Corrupt: return "damaged, replaced";
			case rhi::PipelineCacheLoadResult::Unsupported: return "not supported";
			}
			return "?";
		}
	}

	std::string PathFor(rhi::Backend a_backend)
	{
		if (Switches::Off("CS_ORG_PIPELINE_CACHE"))
			return {};
		const auto folder = std::filesystem::path(L"Data/ShaderCache") / Util::kShaderStoreDirName;
		std::error_code ec;
		std::filesystem::create_directories(folder, ec);
		return (folder / (a_backend == rhi::Backend::D3D12 ? "pipelines-d3d12.bin" : "pipelines-vulkan.bin")).string();
	}

	void Tick(const std::shared_ptr<rhi::DevicePtr>& a_device)
	{
		if (!a_device || !*a_device)
			return;
		rhi::PipelineCacheStats stats{};
		if ((*a_device)->GetPipelineCacheStats(stats) != rhi::Result::Ok || stats.load == rhi::PipelineCacheLoadResult::Off ||
			stats.load == rhi::PipelineCacheLoadResult::Unsupported)
			return;
		const auto now = Clock::now();
		if (!reported) {
			reported = true;
			logger::info("[ORG] pipeline cache: {} ({} KB read)", LoadName(stats.load), stats.loadedBytes >> 10);
		}
		const bool asked = saveAsked.load();
		if (stats.misses != lastMisses) {
			lastMisses = stats.misses;
			lastAdded = now;
			if (!asked)
				return;
		}
		if (stats.misses == savedMisses) {
			saveAsked.store(false);  // nothing new to save
			return;
		}
		if ((!asked && now - lastAdded < kQuiet) || saving.exchange(true))
			return;
		saveAsked.store(false);
		savedMisses = stats.misses;
		std::thread([device = a_device, hits = stats.hits, misses = stats.misses] {
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
			const auto result = (*device)->SavePipelineCache();
			rhi::PipelineCacheStats after{};
			(void)(*device)->GetPipelineCacheStats(after);
			logger::info("[ORG] pipeline cache saved ({}): {} KB, {} pipelines served from it and {} built so far", result == rhi::Result::Ok ? "ok" : "failed",
				after.savedBytes >> 10, hits, misses);
			saving.store(false);
			saving.notify_all();
		}).detach();
	}

	void RequestSave()
	{
		saveAsked.store(true);
	}

	void WaitForSave()
	{
		const auto until = Clock::now() + std::chrono::seconds(2);
		while (saving.load() && Clock::now() < until)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}
