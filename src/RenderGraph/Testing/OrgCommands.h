#pragma once

/**
* @brief The render graph's test commands, owner "org" in the test runner's feature command registry. A step runs one as
* do = "org:<name>" or do = "call" with arg "org.<name>[=<arg>]". Also the "reload" and "nvperf" waits and the graph frame
* report at the test exit (GraphFrames.h). Compiled out of Release builds (CS_RELEASE_PROFILE).
*
* Commands, each taking effect at the next adoption point or from the next build, never inside a frame:
* - rebuild: every graph is built again;
* - refresh[=<graph id>]: the graphs that looked that id up are built again, every graph without one;
* - reload-shaders[=all|<file>]: a shader hot reload (the "reload" wait waits for the revision that adopts its versions);
* - gpu-window=<label>, ts-dump=<epochs>, nvperf=<range>|<range>...[#<samples>][@<metrics>] (the "nvperf" wait);
* - async-compute=0|1|default, pass-queries, heap-dedupe, list-heaps, list-barriers, rhi-globals, queue-boundary: what the next
*   builds record.
*/
namespace RenderGraph::TestSupport
{
#if defined(CS_RELEASE_PROFILE)
	inline void Install() {}
#else
	/** @brief Main thread, kPostPostLoad, before the test runner's own kPostPostLoad: registers the commands and waits. */
	void Install();
#endif
}
