// Open and Close on the main thread, leases on any worker, and the report's counters.

#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/FrameAnchors.h"
#include "Snapshot/Publication.h"

#include <array>
#include <chrono>
#include <cstring>
#include <immintrin.h>
#include <thread>

namespace DCLF
{
	void EngineReadWindow::Open()
	{
		state.fetch_or(kOpen, std::memory_order_acq_rel);
	}

	void EngineReadWindow::NoteClose(std::uint32_t a_before)
	{
		closes.fetch_add(1, std::memory_order_relaxed);
		if (!(a_before & kLeases))
			return;
		closesWaited.fetch_add(1, std::memory_order_relaxed);
		const auto ns = static_cast<std::uint64_t>(DrainLeases() * 1000.0);
		waitedNs.fetch_add(ns, std::memory_order_relaxed);
		for (auto max = waitedMaxNs.load(std::memory_order_relaxed); ns > max && !waitedMaxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed);) {}
	}

	void EngineReadWindow::Close()
	{
		const std::uint32_t before = state.fetch_and(~kOpen, std::memory_order_acq_rel);
		if (!(before & kOpen))
			return;
		NoteClose(before);
	}

	void EngineReadWindow::CloseAtPresent()
	{
		if (Snapshot::InGameUpdate())
			Close();
	}

	void EngineReadWindow::Suspend()
	{
		const std::uint32_t before = state.fetch_add(kSuspendUnit, std::memory_order_acq_rel);
		suspends.fetch_add(1, std::memory_order_relaxed);
		if (!(before & kLeases))
			return;
		suspendsWaited.fetch_add(1, std::memory_order_relaxed);
		const auto us = static_cast<std::uint64_t>(DrainLeases());
		suspendWaitedUs.fetch_add(us, std::memory_order_relaxed);
		for (auto max = suspendWaitedMaxUs.load(std::memory_order_relaxed); us > max && !suspendWaitedMaxUs.compare_exchange_weak(max, us, std::memory_order_relaxed);) {}
	}

	void EngineReadWindow::Resume()
	{
		state.fetch_sub(kSuspendUnit, std::memory_order_release);
	}

	double EngineReadWindow::DrainLeases()
	{
		// Each item in flight finishes. No new lease can be taken now.
		const auto start = std::chrono::steady_clock::now();
		for (std::uint32_t spins = 0; state.load(std::memory_order_acquire) & kLeases; ++spins) {
			if (spins < 64)
				_mm_pause();
			else
				std::this_thread::yield();
		}
		return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
	}

	EngineReadWindow::Lease::Lease()
	{
		if (renderSideJob)
			renderSideJobLeases.fetch_add(1, std::memory_order_relaxed);
		// Count the lease and check open-and-not-suspended in one step, so Close and Suspend cannot miss it.
		if ((state.fetch_add(1, std::memory_order_acq_rel) & (kOpen | kSuspended)) == kOpen) {
			held = true;
			++heldHere;
			if (FrameAnchors::IsMainThread())
				mainThreadLeases.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		state.fetch_sub(1, std::memory_order_acq_rel);
		refused.fetch_add(1, std::memory_order_relaxed);
	}

	EngineReadWindow::Lease::~Lease()
	{
		if (held) {
			--heldHere;
			state.fetch_sub(1, std::memory_order_release);
		}
	}

	void EngineReadWindow::NoteUnleased(const char* a_site)
	{
		if (unleased.fetch_add(1, std::memory_order_relaxed) == 0)
			unleasedFirst.store(a_site, std::memory_order_relaxed);
	}

	EngineReadWindow::Counters EngineReadWindow::Take()
	{
		Counters c;
		c.unleased = unleased.exchange(0, std::memory_order_relaxed);
		c.unleasedFirst = unleasedFirst.exchange(nullptr, std::memory_order_relaxed);
		c.suspends = suspends.exchange(0, std::memory_order_relaxed);
		c.suspendsWaited = suspendsWaited.exchange(0, std::memory_order_relaxed);
		c.suspendWaitedUs = static_cast<double>(suspendWaitedUs.exchange(0, std::memory_order_relaxed));
		c.suspendWaitedMaxUs = static_cast<double>(suspendWaitedMaxUs.exchange(0, std::memory_order_relaxed));
		c.mainThreadLeases = mainThreadLeases.exchange(0, std::memory_order_relaxed);
		c.renderSideJobLeases = renderSideJobLeases.exchange(0, std::memory_order_relaxed);
		c.closes = closes.exchange(0, std::memory_order_relaxed);
		c.closesWaited = closesWaited.exchange(0, std::memory_order_relaxed);
		c.waitedUs = static_cast<double>(waitedNs.exchange(0, std::memory_order_relaxed)) / 1000.0;
		c.waitedMaxUs = static_cast<double>(waitedMaxNs.exchange(0, std::memory_order_relaxed)) / 1000.0;
		c.any = c.closes || c.unleased || c.suspends || c.mainThreadLeases || c.renderSideJobLeases;
		if (c.any)
			c.refused = refused.exchange(0, std::memory_order_relaxed);
		return c;
	}
}
