#if defined(CS_HAS_RENDER_GRAPH)
// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>
#endif

#include "Vram/OrgMemory.h"

#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH)
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/DeviceMemory.h>
#	include <OpenRenderGraph/GraphHost.h>
#endif

namespace Vram::Org
{
	bool Selected()
	{
		static const bool selected = Switches::EqualsNoCase("CS_ORG_VRAM_SOURCE", "org");
		return selected;
	}

	std::string OwnerPath(std::string_view a_label)
	{
		return a_label.empty() ? std::string("ORG/Allocator") : std::string(a_label);
	}

#if defined(CS_HAS_RENDER_GRAPH)
	namespace
	{
		Segment ReadSegment(rhi::Device a_device, rhi::MemorySegmentGroup a_group, const rhi::ma::Budget& a_allocator, bool a_hasAllocator)
		{
			Segment segment;
			rhi::VideoMemoryInfo info{};
			if (a_device.QueryVideoMemoryInfo(0, a_group, info) != rhi::Result::Ok)
				return segment;
			segment.valid = true;
			segment.capacity = org::memory::MemoryCapacity(a_group);
			segment.usage = info.currentUsageBytes;
			segment.budget = info.budgetBytes;
			segment.usageAtBudget = info.usageAtBudget;
			if (a_hasAllocator) {
				segment.allocatorBlocks = a_allocator.stats.blockBytes;
				segment.allocatorUsed = a_allocator.stats.allocationBytes;
			}
			return segment;
		}
	}

	Heaps ReadHeaps()
	{
		Heaps heaps;
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.IsActive() ? runtime.Host() : nullptr;
		if (!host)
			return heaps;
		rhi::ma::Budget local{}, system{};
		const bool hasAllocator = org::memory::AllocatorBudget(local, system);
		const auto device = host->Device();
		heaps.local = ReadSegment(device, rhi::MemorySegmentGroup::Local, local, hasAllocator);
		heaps.system = ReadSegment(device, rhi::MemorySegmentGroup::NonLocal, system, hasAllocator);
		return heaps;
	}

	std::vector<Owner> ReadOwners()
	{
		std::vector<Owner> owners;
		if (!RenderGraphRuntime::Get().IsActive())
			return owners;
		const auto rows = org::memory::OwnerReport();
		owners.reserve(rows.size());
		for (const auto& row : rows) {
			Owner owner;
			owner.label = row.owner ? row.owner : "";
			owner.count = row.allocationCount;
			owner.videoBytes = row.bytesBySegment[static_cast<std::size_t>(rhi::MemorySegmentGroup::Local)];
			owner.systemBytes = row.bytesBySegment[static_cast<std::size_t>(rhi::MemorySegmentGroup::NonLocal)];
			owner.spilledBytes = row.spilledBytes;
			owners.push_back(std::move(owner));
		}
		return owners;
	}

	std::uint64_t SpilledBytes()
	{
		std::uint64_t spilled = 0;
		for (const auto& owner : ReadOwners())
			spilled += owner.spilledBytes;
		return spilled;
	}
#else
	Heaps ReadHeaps()
	{
		return {};
	}

	std::vector<Owner> ReadOwners()
	{
		return {};
	}

	std::uint64_t SpilledBytes()
	{
		return 0;
	}
#endif
}
