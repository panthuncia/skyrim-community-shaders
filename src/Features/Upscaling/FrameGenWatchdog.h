#pragma once

#include <atomic>
#include <thread>

class FrameGenWatchdog
{
public:
	void Start(const std::atomic<bool>& a_enabled, const std::atomic<uint64_t>& a_renderHeartbeat,
		const std::atomic<uint64_t>& a_presentHeartbeat);

private:
	std::atomic<bool> triggered{ false };
	std::jthread worker;
};
