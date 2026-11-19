#pragma once

#if defined(CS_HAS_RENDER_GRAPH)

#	include <cstddef>
#	include <cstdint>
#	include <functional>
#	include <memory>
#	include <string>
#	include <string_view>
#	include <utility>
#	include <vector>

namespace org
{
	class LatchBlock;
	class RenderPass;
	class Resource;
	struct RunRef;
}

namespace RenderGraph
{
	/**
	* @brief A small copy from the GPU to the CPU that a kept recording can replay.
	*
	* A stage recorded ahead submits its recording again and again (a revision-driven epoch), so its passes may not use the
	* readback service. Each run copies into its own region of a host buffer, picked by the run's lease index. A region is read
	* when the lease's next run is committed, since the lease's last run is done by then. Nothing waits.
	* Each copy also copies a stamp from the run's latch, so a run the client did not commit is never read as one it did.
	* Make one per reader and keep it across builds. Render thread, except the passes' recording.
	* Users: Post Processing's effects (ORGPostEffect.h) and Physical Sky's lightning query.
	*/
	class RunReadback
	{
	public:
		/** @brief The latch bytes a run's stamp takes. The client writes it where Bind said, with the value Commit returns. */
		struct Stamp
		{
			std::uint32_t value = 0;
			std::uint32_t pad[3]{};
		};

		/** @brief One part per copy, a_parts[i] bytes each. Null and logged when the buffer cannot be made. */
		static std::shared_ptr<RunReadback> Create(std::string_view a_name, std::vector<std::uint32_t> a_parts);

		/** @brief At every build, before the passes: the client's latch, and the offset of the Stamp the client writes there. */
		void Bind(std::shared_ptr<const org::LatchBlock> a_latch, std::uint32_t a_stampOffset);

		/** @brief A pass that copies the first bytes of buffer a_source into part a_part. Empty, with a_why, when it cannot be made. */
		std::function<std::shared_ptr<org::RenderPass>()> BufferPass(std::uint32_t a_part, std::shared_ptr<org::Resource> a_source, std::string& a_why) const;

		/** @brief A pass that copies mip 0 of a small 2D texture into part a_part. Rows are 256 bytes apart. Empty, with a_why, when it cannot. */
		std::function<std::shared_ptr<org::RenderPass>()> TexturePass(std::uint32_t a_part, std::shared_ptr<org::Resource> a_source, std::uint32_t a_width,
			std::uint32_t a_height, std::string& a_why) const;

		/**
		* @brief In the client's commit, before Take. Reads the lease's last run and stamps this run.
		* @return This run's stamp, which the client writes into a_run's latch as a Stamp. Stamps grow by one each commit.
		*/
		std::uint32_t Commit(const org::RunRef& a_run);

		/** @brief In the client's executed callback: the run last committed was submitted. */
		void Executed();

		/** @brief The newest copy of part a_part read since the last Take, and its run's stamp. Empty when none arrived. */
		std::vector<std::byte> Take(std::uint32_t a_part, std::uint32_t* a_stamp = nullptr);

		struct State;
		explicit RunReadback(std::shared_ptr<State> a_state) :
			state(std::move(a_state)) {}

	private:
		std::shared_ptr<State> state;
	};
}

#endif
