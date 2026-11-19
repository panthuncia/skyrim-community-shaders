#include "Features/DrawcallLimitFix/Core/Render/Direct/SiteStats.h"

#include <rhi_helpers.h>

#include <array>
#include <cstring>
#include <mutex>
#include <vector>

namespace DCLF::Direct::SiteStats
{
	namespace
	{
		constexpr std::uint32_t kSlots = 4096;
		constexpr std::size_t kSites = 5;
		constexpr std::uint32_t kLogFrames = 300;
		constexpr std::uint32_t kGiveUpFrames = 32;
		constexpr std::array<const char*, kSites> kSiteNames{ "shadow", "depth", "colour", "forward", "first person" };
		// Input vertices, input primitives, vertex shader invocations, fragment shader invocations.
		constexpr std::size_t kValues = 4;
		constexpr std::array<rhi::PipelineStatTypes, kValues> kFields{ rhi::PipelineStatTypes::IAVertices, rhi::PipelineStatTypes::IAPrimitives,
			rhi::PipelineStatTypes::VSInvocations, rhi::PipelineStatTypes::PSInvocations };
		constexpr rhi::PipelineStatsMask kStatistics =
			rhi::PipelineStatBits::PS_IAVertices | rhi::PipelineStatBits::PS_IAPrimitives | rhi::PipelineStatBits::PS_VSInvocations | rhi::PipelineStatBits::PS_PSInvocations;

		struct Pending
		{
			std::uint32_t slot;
			std::size_t site;
			std::uint64_t frameValue;  // the CPU-frame timeline value that covers it
			std::uint64_t frame;  // Collect's count when it began
		};

		struct State
		{
			std::once_flag once;
			bool ready = false;
			rhi::QueryPoolPtr pool;
			rhi::ResourcePtr readback;
			std::uint32_t stride = 0;  // bytes per query in the readback buffer
			std::array<std::uint32_t, kValues> offsets{};  // ~0u when not supported
			std::mutex mutex;
			std::uint32_t next = 0;
			std::vector<Pending> pending;
			std::uint64_t frame = 0;
			std::uint32_t frames = 0;
			std::array<std::array<double, kValues>, kSites> sums{};
			std::array<std::uint32_t, kSites> queries{};
			std::uint64_t dropped = 0;
		};

		State& S()
		{
			static State state;
			return state;
		}

		void Setup(rhi::Device a_device)
		{
			auto& s = S();
			rhi::QueryPoolDesc desc{};
			desc.type = rhi::QueryType::PipelineStatistics;
			desc.count = kSlots;
			desc.statsMask = kStatistics;
			if (a_device.CreateQueryPool(desc, s.pool) != rhi::Result::Ok || !s.pool) {
				logger::warn("[DCLF] site stats: could not create the pipeline statistics query pool");
				return;
			}
			// Each counter's place in a query's result follows the backend's own layout.
			std::array<rhi::PipelineStatsFieldDesc, kValues> fields{};
			for (std::size_t v = 0; v < kValues; ++v)
				fields[v].field = kFields[v];
			const auto layout = s.pool->GetPipelineStatsLayout(fields.data(), static_cast<std::uint32_t>(fields.size()));
			s.offsets.fill(~0u);
			for (std::uint32_t i = 0; i < layout.fields.size; ++i)
				for (std::size_t v = 0; v < kValues; ++v)
					if (layout.fields.data[i].field == kFields[v] && layout.fields.data[i].supported)
						s.offsets[v] = layout.fields.data[i].byteOffset;
			s.stride = s.pool->GetQueryResultInfo().elementSize;
			const auto bufferDesc = rhi::helpers::ResourceDesc::Buffer(std::uint64_t(s.stride) * kSlots, rhi::HeapType::Readback);
			if (!s.stride || a_device.CreateCommittedResource(bufferDesc, s.readback) != rhi::Result::Ok || !s.readback) {
				logger::warn("[DCLF] site stats: could not create the pipeline statistics' readback buffer");
				s.pool = {};
				return;
			}
			s.readback->SetName("cs.dclf.cpu.site-stats");
			s.ready = true;
			logger::info("[DCLF] site stats on: pipeline statistics per CPU pipeline site, logged every {} frames", kLogFrames);
		}
	}

	bool Enabled()
	{
		static const bool enabled = [] {
			char value[8]{};
			return ::GetEnvironmentVariableA("CS_DCLF_SITE_STATS", value, sizeof(value)) && value[0] == '1';
		}();
		return enabled;
	}

	std::uint32_t Begin(rhi::Device a_device, rhi::CommandList& a_list, std::size_t a_site, std::uint64_t a_frameValue)
	{
		if (!Enabled() || a_site >= kSites)
			return ~0u;
		auto& s = S();
		std::call_once(s.once, [&] { Setup(a_device); });
		if (!s.ready)
			return ~0u;
		std::uint32_t slot = 0;
		{
			std::lock_guard lock(s.mutex);
			slot = s.next++ % kSlots;
			s.pending.push_back({ slot, a_site, a_frameValue, s.frame });
		}
		a_list.ResetQueries(s.pool->GetHandle(), slot, 1);
		a_list.BeginQuery(s.pool->GetHandle(), slot);
		return slot;
	}

	void End(rhi::CommandList& a_list, std::uint32_t a_slot)
	{
		if (a_slot == ~0u)
			return;
		auto& s = S();
		a_list.EndQuery(s.pool->GetHandle(), a_slot);
		a_list.ResolveQueryData(s.pool->GetHandle(), a_slot, 1, s.readback->GetHandle(), std::uint64_t(s.stride) * a_slot);
	}

	void Collect(std::uint64_t a_completed)
	{
		if (!Enabled())
			return;
		auto& s = S();
		if (!s.ready)
			return;
		std::lock_guard lock(s.mutex);
		++s.frame;
		std::vector<Pending> waiting;
		void* mapped = nullptr;
		bool mapFailed = false;
		for (const auto& entry : s.pending) {
			// Keep it until its frame is done, unless its signal was never queued.
			if (entry.frameValue > a_completed || mapFailed) {
				if (s.frame - entry.frame < kGiveUpFrames)
					waiting.push_back(entry);
				else
					++s.dropped;
				continue;
			}
			if (!mapped) {
				s.readback->Map(&mapped, 0, std::uint64_t(s.stride) * kSlots);
				if (!mapped) {
					mapFailed = true;
					waiting.push_back(entry);
					continue;
				}
			}
			const auto* result = static_cast<const std::byte*>(mapped) + std::uint64_t(s.stride) * entry.slot;
			for (std::size_t v = 0; v < kValues; ++v) {
				if (s.offsets[v] == ~0u)
					continue;
				std::uint64_t value = 0;
				std::memcpy(&value, result + s.offsets[v], sizeof(value));
				s.sums[entry.site][v] += static_cast<double>(value);
			}
			++s.queries[entry.site];
		}
		if (mapped)
			s.readback->Unmap(0, 0);
		s.pending.swap(waiting);
		if (++s.frames < kLogFrames)
			return;
		for (std::size_t site = 0; site < kSites; ++site) {
			if (!s.queries[site])
				continue;
			const auto& v = s.sums[site];
			const double f = s.frames;
			logger::info("[DCLF] site stats {}: {:.2f} queries, IA vertices {:.0f}, IA prims {:.0f}, VS {:.0f}, PS {:.0f} (per frame, {} frames, {} dropped)",
				kSiteNames[site], s.queries[site] / f, v[0] / f, v[1] / f, v[2] / f, v[3] / f, s.frames, s.dropped);
		}
		s.sums = {};
		s.queries = {};
		s.frames = 0;
		s.dropped = 0;
	}
}
