// SceneScheduler: DCLF's scopes and state graph on CS's shared executor. Created on first use, never destroyed.
#include "Features/DrawcallLimitFix/Core/Runtime/SceneScheduler.h"

namespace DCLF
{
	Runtime::SceneGraph& SceneScheduler::Graph()
	{
		// Never destroyed, like the executor it runs on. The graph holds it without owning it.
		static auto* graph = new Runtime::SceneGraph(std::shared_ptr<org::async::GraphScheduler>(&::RenderGraph::Frame::SharedExecutor(), [](org::async::GraphScheduler*) {}));
		return *graph;
	}

	const org::async::Scope& SceneScheduler::Scope()
	{
		static const org::async::Scope scope = ::RenderGraph::Frame::SharedExecutor().CreateScope("DCLF");
		return scope;
	}
}
