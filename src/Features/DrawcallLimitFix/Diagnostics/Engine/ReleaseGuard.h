#pragma once

#include <atomic>
#include <cstdint>
#include <string>

namespace DCLF
{
	/**
	 * @brief CS_DCLF_RELEASE_GUARD=1. Counts engine objects whose last reference is dropped on DCLF's own threads.
	 * Their destructors belong on engine threads, since a node's takes its collision object out of the Havok world.
	 * The switch replaces NiRefObject::DeleteThis (AE 0x140d27520) with a counting call. Counted only.
	 */
	class ReleaseGuard
	{
	public:
		/** @brief Patches DeleteThis under the switch, once, checking its bytes first. Render thread, at install. */
		static void Install();
		/** @brief Tags the calling executor thread. a_kind is 1 for the scene lane, 2 for the coordinator or a pool worker. */
		static void MarkThread(std::uint8_t a_kind) { dclfThread = a_kind; }
		/** @brief Release counts since the last Take, on the scene lane and other executor threads, with the first and last class. */
		struct Counters
		{
			std::uint64_t lane = 0, pool = 0, unnamed = 0;
			const char* first = nullptr;
			const char* last = nullptr;
		};
		static bool Installed() { return installed; }
		/** @brief The counters since the last Take, then resets them. */
		static Counters Take();

	private:
		static void DeleteThis(void* a_object);
		static void Note(void* a_object);

		static inline thread_local std::uint8_t dclfThread = 0;
		static inline bool installed = false;
		static inline std::atomic<std::uint64_t> laneReleases{ 0 }, poolReleases{ 0 }, unnamed{ 0 };
		static inline std::atomic<const char*> first{ nullptr }, last{ nullptr };
	};
}
