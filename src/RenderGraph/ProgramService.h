#pragma once

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RasterProgram.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief The program service: every graph program is built here, several at once on CS's executor. The render thread never
 * builds a program and never waits for one in a frame.
 *
 * Two ways in:
 * - a program keyed on frame variables or observed state: Frame::DeclareProgram (Frame/Declare.h), or Frame::ProgramSet
 *   (Frame/ProgramSet.h) for a set of keys the frame meets. Its builds are gating slots of this queue, so a revision starts
 *   only once its programs are built.
 * - a fixed program: Queue at setup. CS's setup joins the batch once (JoinSetup). A later queue gates the next revision.
 * A failed build is fatal.
 */
namespace RenderGraph::Programs
{
	/** @brief Resolves the desc's common defines. Render thread, since CS's common defines read State. Never on the pool. */
	void ResolveDefines(RasterProgram::Desc& a_desc);
	void ResolveDefines(ComputeProgram::Desc& a_desc);

	/** @brief A desc's name for the logs. */
	std::string Label(const RasterProgram::Desc& a_desc);
	std::string Label(const ComputeProgram::Desc& a_desc);

	/** @brief Builds a_desc on the calling thread. For build functions that run on the programs pool. Null when it failed. */
	std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc);
	std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc);

	/** @brief Queues a_job on the programs pool, which runs several jobs at once in no set order. False after Stop. Any thread. */
	bool Post(std::function<void()> a_job);

	/** @brief Whether the render graph runs, with a device and a compiler. */
	bool Available();

	/** @brief Drops queued builds before the device goes. Teardown only. */
	void Stop();

	namespace Detail
	{
		/** @brief One build on the programs pool. The pool writes program or error, then done. */
		struct Slot
		{
			std::string label;
			bool gates = true;  // a revision starts only once it is built (ProgramsReady)
			std::function<std::shared_ptr<const void>()> build;  // runs on the pool
			std::shared_ptr<const void> program;
			std::string error;
			std::atomic<bool> done{ false };
			bool collected = false;  // render thread: a revision start or the setup join saw it built
		};

		/**
		 * @brief Render thread. Posts a_build as a slot the next Collect checks. Fatal when the pool refuses it. a_gates: no revision
		 * starts while it builds. JoinSetup waits for every slot either way.
		 */
		std::shared_ptr<Slot> Queue(std::string a_label, std::function<std::shared_ptr<const void>()> a_build, bool a_gates);

		/** @brief Fatal: a fixed program was read before it was built. */
		[[noreturn]] void NotBuilt(const Slot& a_slot);
	}

	/**
	 * @brief A fixed program queued at setup. Copy it freely. Get is fatal before the build is done. Read it in passes and in
	 * client make functions, which run after CS's setup joined the batch or after the revision start that collected it.
	 */
	template <class P>
	class Fixed
	{
	public:
		Fixed() = default;
		explicit Fixed(std::shared_ptr<Detail::Slot> a_slot) :
			slot(std::move(a_slot)) {}

		/** @brief The program. Null for an empty handle. Fatal when it is not built yet. Any thread. */
		std::shared_ptr<const P> Get() const
		{
			if (!slot)
				return nullptr;
			if (!slot->done.load(std::memory_order_acquire) || !slot->program)
				Detail::NotBuilt(*slot);
			return std::static_pointer_cast<const P>(slot->program);
		}

		/** @brief Whether the build is done. Any thread. */
		bool Ready() const { return slot && slot->done.load(std::memory_order_acquire) && slot->program; }

		const P* operator->() const { return Get().get(); }
		operator std::shared_ptr<const P>() const { return Get(); }
		/** @brief Whether a program was queued. */
		explicit operator bool() const { return slot != nullptr; }

	private:
		std::shared_ptr<Detail::Slot> slot;
	};

	/**
	 * @brief Render thread, at setup. Queues a_desc's build on the programs pool, and resolves its common defines here. a_owner
	 * names the feature in the logs. An empty handle without the graph.
	 */
	Fixed<RasterProgram> Queue(std::string_view a_owner, RasterProgram::Desc a_desc);
	Fixed<ComputeProgram> Queue(std::string_view a_owner, ComputeProgram::Desc a_desc);

	/**
	 * @brief Render thread. Collects the finished builds. Fatal when a build failed. True when no gating build is in flight, such
	 * as a fixed program queued after setup. A revision starts only then (Frame/DeclareLane.h, ProgramsReady). DCLF's engine
	 * draw programs gate nothing.
	 */
	bool Collect();

	/** @brief Render thread. Whether a build is in flight, gating or not. */
	bool Pending();

	/**
	 * @brief Render thread, once at the end of CS's setup, before its setup graphs are built. Waits for the setup batch and
	 * collects it. Fatal when a build failed. Setup only: no frame runs yet.
	 */
	void JoinSetup(std::string_view a_point);
}

