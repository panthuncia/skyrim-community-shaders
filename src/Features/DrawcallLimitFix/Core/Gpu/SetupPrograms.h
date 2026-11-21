#pragma once

#include "RenderGraph/ComputeProgram.h"

#include <functional>
#include <memory>
#include <string_view>

/**
* @brief DCLF's fixed compute programs, queued at CS's setup so no frame-time setup compiles or waits.
*
* Declare one at namespace scope (static initialisation), with a function that gives its description. DrawcallLimitFix::
* SetupResources queues every one on the programs pool (Programs::Queue), and CS's setup join builds them before the first
* frame. A setup that runs later, in a frame (the scene tables, the main resources, a pass's state), reads it with Get(), which
* never builds. A failed build is fatal at the join.
*/
namespace DCLF::SetupPrograms
{
	namespace Detail
	{
		struct Entry;
	}

	class Compute
	{
	public:
		/** @brief Empty: Get is fatal. */
		Compute() = default;

		/** @brief Registers the program. a_desc runs at Queue, on the render thread, so it may read switches. Before Queue only. */
		Compute(std::string_view a_owner, std::function<ComputeProgram::Desc()> a_desc);

		/** @brief The program. Fatal when Queue did not queue it or its build is not done. Any thread. */
		std::shared_ptr<const ComputeProgram> Get() const;

	private:
		std::shared_ptr<Detail::Entry> entry;
	};

	/** @brief Render thread, once, in DrawcallLimitFix::SetupResources, before CS's setup join. Queues every Compute. */
	void Queue();
}
