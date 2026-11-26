#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace RE
{
	class NiRefObject;
}

/**
* @brief Snapshot lifetimes (render-thread roadmap, phase 6). Notes: docs/development/plans/snap6-life.md.
*
* A snapshot, and anything the render side keeps for it, may outlive the game frame that made it: rendered later, rendered again,
* or replaced unrendered. What it references stays alive until the render side is done with it, and is then let go through the
* retire queue, never by the render side itself:
* - The engine lane holds what must be let go on the game's main thread: snapshots (their blocks may hold engine references) and
*   engine refcounted objects. Dropping the last reference to an engine object runs the engine's destructors (NiRefObject counts
*   atomically, but DeleteThis runs on the thread that drops it, and the destructors touch scene graph, renderer and texture
*   database state with no lock). The main thread drains it once a game update's frame jobs are joined (the frame-job wait right
*   after Main::DrawFrame), where no engine job runs beside it and DCLF's engine-read window is closed.
* - The render lane holds CS objects and D3D11 handles (UI streams, upload textures). Their release is safe on any thread (a
*   tripwire handle's resource goes through ServiceRetired), so the render side drains it at its frame start. This keeps a
*   loading screen's streams, rendered on the serving thread while the main thread loads, from piling up until the load ends.
*
* An entry waits for the GPU to finish the render frame it names (RenderGraph::FrameClock::GpuCompleted, in graph mode), so
* references the GPU work may still read stay valid. Frame 0 means nothing rendered it: it retires at the next drain.
*
* The queues are never cleaned up at the process's end: nothing may release engine objects while the process is torn down.
*/
namespace Snapshot
{
	struct FrameSnapshot;

	namespace Retire
	{
		enum class Lane : std::uint8_t
		{
			Engine,  // let go on the main thread, after the update's frame jobs
			Render,  // let go on the render side, at its frame start
		};

		/** @brief Any thread. Keeps a_object until the GPU finished render frame a_afterFrame (0: none), then lets it go on a_lane's drain. */
		void Push(Lane a_lane, std::shared_ptr<const void> a_object, std::uint64_t a_afterFrame);

		/** @brief Any thread. Keeps a_object until the GPU finished the newest render frame begun (on the render side, the one running now). */
		void AfterThisFrame(Lane a_lane, std::shared_ptr<const void> a_object);

		/**
		* @brief The render side retires the snapshot it rendered last in a_lastFrame: its UI stream goes to the render lane, the rest
		* to the engine lane. Render side, at adoption.
		*/
		void RenderedSnapshot(std::shared_ptr<const FrameSnapshot>&& a_snapshot, std::uint64_t a_lastFrame);

		/** @brief A producer lets go of a snapshot reference through the engine lane, at its next drain. Any thread. */
		void LetGo(std::shared_ptr<const FrameSnapshot>&& a_snapshot);

		/** @brief Main thread, after the update's frame jobs. Lets go of the engine lane's entries whose GPU work is done. */
		void DrainEngine();

		/** @brief Render side, at its frame start (adoption). Lets go of the render lane's entries whose GPU work is done. */
		void DrainRender();

		/**
		* @brief One line for the snapshot log: snapshots alive, engine references held, and each lane's retired and waiting counts
		* since the last line (the leak check across loads).
		*/
		std::string TakeStatsLine();

		/** @brief Counts FrameSnapshot objects alive. A member of FrameSnapshot. */
		struct LiveCount
		{
			LiveCount();
			LiveCount(const LiveCount&);
			LiveCount& operator=(const LiveCount&) { return *this; }
			~LiveCount();
		};
	}

	/**
	* @brief A reference a snapshot block holds to an engine object it names, so the object's address stays its own while any
	* snapshot names it. A key: the render side compares it with what it draws and never dereferences it. Made, copied and let go on
	* the main thread only, which the retire queue guarantees for snapshots; anywhere else is fatal.
	*/
	class EngineRef
	{
	public:
		EngineRef() = default;
		/** @brief Main thread (the capture). Takes a reference to a_object; null makes an empty one. */
		explicit EngineRef(RE::NiRefObject* a_object);
		EngineRef(const EngineRef& a_other);
		EngineRef(EngineRef&& a_other) noexcept :
			object(a_other.object) { a_other.object = nullptr; }
		EngineRef& operator=(const EngineRef& a_other);
		EngineRef& operator=(EngineRef&& a_other) noexcept;
		~EngineRef();

		/** @brief The object's address as a key, 0 when empty. */
		std::uintptr_t Key() const { return reinterpret_cast<std::uintptr_t>(object); }
		/** @brief Whether this names a_object (never true for null). */
		bool Names(const void* a_object) const { return object && a_object == object; }
		explicit operator bool() const { return object != nullptr; }
		bool operator==(const EngineRef& a_other) const { return object == a_other.object; }

	private:
		void Let();
		RE::NiRefObject* object = nullptr;
	};
}
