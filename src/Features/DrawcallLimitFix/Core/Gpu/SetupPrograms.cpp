#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"

#include "RenderGraph/Fatal.h"
#include "RenderGraph/ProgramService.h"

#include <string>
#include <utility>
#include <vector>

namespace DCLF::SetupPrograms
{
	struct Detail::Entry
	{
		std::string owner;
		std::function<ComputeProgram::Desc()> desc;
		::RenderGraph::Programs::Fixed<ComputeProgram> fixed;  // written once at Queue, before any frame
	};

	namespace
	{
		/** @brief Every registered program, in static initialisation order. Never destroyed: programs live at namespace scope. */
		std::vector<std::shared_ptr<Detail::Entry>>& Registry()
		{
			static auto* s_registry = new std::vector<std::shared_ptr<Detail::Entry>>();
			return *s_registry;
		}

		bool s_queued = false;  // render thread
	}

	Compute::Compute(std::string_view a_owner, std::function<ComputeProgram::Desc()> a_desc) :
		entry(std::make_shared<Detail::Entry>())
	{
		entry->owner = a_owner;
		entry->desc = std::move(a_desc);
		Registry().push_back(entry);
	}

	std::shared_ptr<const ComputeProgram> Compute::Get() const
	{
		if (!entry || !entry->fixed)
			::RenderGraph::Fatal("DCLF", fmt::format("{}'s program was read before DCLF's setup queued it", entry ? entry->owner : std::string("an empty")));
		return entry->fixed.Get();
	}

	void Queue()
	{
		if (std::exchange(s_queued, true))
			return;
		for (const auto& entry : Registry())
			entry->fixed = ::RenderGraph::Programs::Queue(entry->owner, entry->desc());
		logger::info("[DCLF] {} fixed compute programs queued with CS's setup batch", Registry().size());
	}
}
