#pragma once

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RasterProgram.h"

#include <ORGModuleServices/Programs/ProgramService.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

/**
 * @brief CS's program service: OMS's org::services::ProgramService on CS's executor. Every graph program is built here, several
 * at once. The render thread never builds a program and never waits for one in a frame.
 *
 * Two ways in:
 * - a program keyed on frame variables or observed state: Frame::DeclareProgram (Frame/Declare.h), or Frame::ProgramSet
 *   (Frame/ProgramSet.h) for a set of keys the frame meets. Its builds are gating slots of this queue, so a revision starts
 *   only once its programs are built.
 * - a fixed program: Queue at setup. CS's setup joins the batch once (JoinSetup). A later queue gates the next revision.
 * A failed build is fatal, and so is a fixed program read before it was built.
 */
namespace RenderGraph::Programs
{
	/**
	 * @brief Render owner, once, before any program loads. CS's environment for OMS's programs: Data/Shaders, the active
	 * backend, the runtime's compiler and pipeline registry, CS's common defines, and the pipeline pins (PipelineVersions.h).
	 */
	void InstallEnvironment();

	/** @brief Resolves the desc's common defines. Render thread, since CS's common defines read State. Never on the pool. */
	inline void ResolveDefines(RasterProgram::Desc& a_desc) { org::services::ResolveProgramDefines(a_desc); }
	inline void ResolveDefines(ComputeProgram::Desc& a_desc) { org::services::ResolveProgramDefines(a_desc); }

	/** @brief A desc's name for the logs. */
	inline std::string Label(const RasterProgram::Desc& a_desc) { return org::services::ProgramLabel(a_desc); }
	inline std::string Label(const ComputeProgram::Desc& a_desc) { return org::services::ProgramLabel(a_desc); }

	/** @brief CS's service. Made on first use and never destroyed; Stop stops it before the device goes. */
	org::services::ProgramService& Service();

	/** @brief Builds a_desc on the calling thread. For build functions that run on the programs pool. Null when it failed. */
	inline std::shared_ptr<const RasterProgram> Load(const RasterProgram::Desc& a_desc) { return Service().Load(a_desc); }
	inline std::shared_ptr<const ComputeProgram> Load(const ComputeProgram::Desc& a_desc) { return Service().Load(a_desc); }

	/** @brief Queues a_job on the programs pool, which runs several jobs at once in no set order. False after Stop. Any thread. */
	inline bool Post(std::function<void()> a_job) { return Service().Post(std::move(a_job)); }

	/** @brief Whether the render graph runs, with a device and a compiler. */
	bool Available();

	/** @brief Drops queued builds before the device goes. Teardown only. */
	void Stop();

	namespace Detail
	{
		/** @brief One build on the programs pool. The pool writes program or error, then done. */
		using Slot = org::services::ProgramSlot;

		/**
		 * @brief Render thread. Posts a_build as a slot the next Collect checks. Fatal when the pool refuses it. a_gates: no revision
		 * starts while it builds. JoinSetup waits for every slot either way.
		 */
		inline std::shared_ptr<Slot> Queue(std::string a_label, std::function<std::shared_ptr<const void>()> a_build, bool a_gates)
		{
			return Service().Queue(std::move(a_label), std::move(a_build), a_gates);
		}
	}

	/**
	 * @brief A fixed program queued at setup. Copy it freely. Get is fatal before the build is done. Read it in passes and in
	 * client make functions, which run after CS's setup joined the batch or after the revision start that collected it.
	 */
	template <class P>
	using Fixed = org::services::FixedProgram<P>;

	/**
	 * @brief Render thread, at setup. Queues a_desc's build on the programs pool, and resolves its common defines here. a_owner
	 * names the feature in the logs. An empty handle without the graph.
	 */
	inline Fixed<RasterProgram> Queue(std::string_view a_owner, RasterProgram::Desc a_desc) { return Service().Queue(a_owner, std::move(a_desc)); }
	inline Fixed<ComputeProgram> Queue(std::string_view a_owner, ComputeProgram::Desc a_desc) { return Service().Queue(a_owner, std::move(a_desc)); }

	/**
	 * @brief Render thread. Collects the finished builds. Fatal when a build failed. True when no gating build is in flight, such
	 * as a fixed program queued after setup. A revision starts only then (Frame/DeclareLane.h, ProgramsReady). DCLF's engine
	 * draw programs gate nothing.
	 */
	inline bool Collect() { return Service().Collect(); }

	/** @brief Render thread. Whether a build is in flight, gating or not. */
	inline bool Pending() { return Service().Pending(); }

	/**
	 * @brief Render thread, once at the end of CS's setup, before its setup graphs are built. Waits for the setup batch and
	 * collects it. Fatal when a build failed. Setup only: no frame runs yet.
	 */
	inline void JoinSetup(std::string_view a_point) { Service().JoinSetup(a_point); }
}
