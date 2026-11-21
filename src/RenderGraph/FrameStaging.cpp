// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/FrameStaging.h"

#include "RenderGraph/Fatal.h"

#include <OpenRenderGraph/Runtime.h>

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <map>
#include <mutex>
#include <string>

namespace RenderGraph
{
	namespace
	{
		/** @brief Live staging regions of one area, to tell which client holds the arena's oversize chunks. */
		struct AreaCount
		{
			std::atomic<std::int64_t> regions{ 0 }, bytes{ 0 };
		};

		struct Areas
		{
			std::mutex mutex;
			std::map<std::string, AreaCount, std::less<>> byName;
			std::atomic<std::uint64_t> acquired{ 0 };

			AreaCount& Of(std::string_view a_area)
			{
				const std::scoped_lock lock(mutex);
				if (const auto found = byName.find(a_area); found != byName.end())
					return found->second;
				return byName.try_emplace(std::string(a_area)).first->second;
			}

			std::string Describe()
			{
				const std::scoped_lock lock(mutex);
				std::string text;
				for (const auto& [name, count] : byName)
					text += std::format("{}{} {} ({:.1f} MB)", text.empty() ? "" : ", ", name, count.regions.load(std::memory_order_relaxed),
						count.bytes.load(std::memory_order_relaxed) / (1024.0 * 1024.0));
				return text;
			}
		};

		Areas& AllAreas()
		{
			// Never destroyed: regions may be let go during shutdown.
			static Areas* areas = new Areas;
			return *areas;
		}

		/** @brief Holds a region and counts it against its area until the last holder lets it go. */
		struct CountedRegion
		{
			Staging region;
			AreaCount* area = nullptr;

			~CountedRegion()
			{
				if (!area || !region)
					return;
				area->regions.fetch_sub(1, std::memory_order_relaxed);
				area->bytes.fetch_sub(static_cast<std::int64_t>(region->size), std::memory_order_relaxed);
			}
		};
	}

	Staging AcquireStaging(std::string_view a_area, std::uint64_t a_bytes)
	{
		auto* runtime = org::Runtime::Current();
		if (!runtime || !runtime->Arena())
			Fatal(a_area, "a staging region was asked for with no render graph runtime");
		Staging region;
		try {
			region = runtime->Arena()->AcquireOversize((std::max)(a_bytes, std::uint64_t{ 4 }));
		} catch (const std::exception& e) {
			Fatal(a_area, std::format("a staging region of {} bytes could not be made: {}", a_bytes, e.what()));
		}
		auto& areas = AllAreas();
		auto& area = areas.Of(a_area);
		area.regions.fetch_add(1, std::memory_order_relaxed);
		area.bytes.fetch_add(static_cast<std::int64_t>(region->size), std::memory_order_relaxed);
		if ((areas.acquired.fetch_add(1, std::memory_order_relaxed) + 1) % 4096 == 0)
			logger::info("[ORG] staging regions live by area: {}", areas.Describe());
		auto counted = std::make_shared<CountedRegion>();
		counted->region = std::move(region);
		counted->area = &area;
		return Staging(counted, counted->region.get());
	}
}

