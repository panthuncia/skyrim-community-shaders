#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace org
{
	class GraphHost;
	class RecordingContext;
}

namespace org::services
{
	class LivePipelineSlot;
	struct PipelineVersion;
}

/**
* @brief Shader hot reload's pipeline versions (cleanup plan A4).
*
* Every graph build pins the pipeline registry's generation it is made at (org::services::PipelinePins, under its recording tag).
* Its recordings bind each live pipeline's version at that generation, and hold the pin, so a version lives while a graph or a kept recording binds it.
* A reload publishes new versions off the render thread. The graphs built after it bind them. Frames keep the old ones until the
* revision that carries the new ShaderGeneration is adopted with every graph rebuilt and each drawing fragment source's fragment
* recorded on its rebuilt graphs. Nothing waits for it on the render thread.
*/
namespace RenderGraph::PipelineVersions
{
	/**
	* @brief Record time, any thread. The version of a_slot that a_recording binds: the one at its graph's pin, or the newest for a
	* recording of a graph no host built. The pin keeps it alive for as long as the recording.
	*/
	std::shared_ptr<const org::services::PipelineVersion> VersionFor(const org::services::LivePipelineSlot& a_slot, const org::RecordingContext& a_recording);

	/** @brief Graph host setup, before any graph is built: every graph build pins the registry generation it is made at. */
	void Attach(org::GraphHost& a_host);

	/**
	* @brief Render owner, once at setup. The ShaderGeneration input producer: a revision that carries a new generation is adopted once
	* every epoch's graph, attached or staged, pins it.
	*/
	void Install();

	/** @brief Owner thread. The registry generation a_epoch's build a_build pins, attached or staged. 0 when it is neither. */
	std::uint64_t BuildPin(org::GraphHost& a_host, std::uint32_t a_epoch, std::uint64_t a_build);

	/**
	* @brief Owner thread. Whether every epoch with a graph runs, or has staged, a build that pins a_generation or newer. a_why names
	* one that does not.
	*/
	bool EveryEpochAt(org::GraphHost& a_host, std::uint64_t a_generation, std::string& a_why);

	/** @brief Any thread. The pipeline generation of the last adopted revision. 0 before the first reload's adoption. */
	std::uint64_t Adopted();
}
