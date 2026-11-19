#pragma once

// DCLF's profiling zones. Each is a Tracy zone, and while a telemetry phase records it is also a BasicTelemetry scope and a
// per-thread tally, so a capture holds DCLF's CPU time by scope and thread in any build.
// Outside a phase a zone costs one relaxed load on top of Tracy. The BasicTelemetry scope is not opened then, because its
// constructor reads the session through an atomic shared pointer, which takes a lock.
// Use DCLF_ZONE("CS.DCLF.Name") and DCLF_ZONE_NAMED(variable, "CS.DCLF.Name", true). The name must be a string literal,
// since the callsite keeps a view of it.

#include <BasicTelemetry/Telemetry.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::Zones
{
	/** @brief True while a telemetry phase records. */
	inline std::atomic<bool> recording{ false };

	// The BasicTelemetry scope is built in place only while a phase records. Its alignment pads the class, hence C4324.
#pragma warning(push)
#pragma warning(disable: 4324)
	/** @brief One zone's extent on one thread, recorded while a phase records. */
	class Scope
	{
	public:
		explicit Scope(const basic_telemetry::Callsite& a_site) noexcept
		{
			if (recording.load(std::memory_order_relaxed)) [[unlikely]]
				Begin(a_site);
		}
		~Scope()
		{
			if (site) [[unlikely]]
				End();
		}
		Scope(const Scope&) = delete;
		Scope& operator=(const Scope&) = delete;

	private:
		void Begin(const basic_telemetry::Callsite& a_site) noexcept;
		void End() noexcept;

		const basic_telemetry::Callsite* site = nullptr;
		std::int64_t start = 0;
		bool topLevel = false;
		alignas(basic_telemetry::Scope) std::byte storage[sizeof(basic_telemetry::Scope)];
	};
#pragma warning(pop)

	/** @brief One scope's time on one thread over a phase. */
	struct ThreadRow
	{
		std::uint32_t threadId = 0;
		std::string thread;  // the thread's description, when it has one
		std::string scope;
		std::uint64_t count = 0, totalNs = 0, maxNs = 0;
		std::uint64_t topLevelNs = 0;  // time with no other DCLF zone open on the thread, so these add up to its DCLF time
	};

	/** @brief Clears every thread's tallies at a phase's start. */
	void ResetThreadRows();

	/** @brief Every thread's tallies since ResetThreadRows, at a phase's end. */
	std::vector<ThreadRow> TakeThreadRows();
}

#define DCLF_ZONE_JOIN_INNER(a, b) a##b
#define DCLF_ZONE_JOIN(a, b) DCLF_ZONE_JOIN_INNER(a, b)

#define DCLF_ZONE(a_name)                                                                                    \
	static const ::basic_telemetry::Callsite DCLF_ZONE_JOIN(dclfZoneSite, __LINE__){ a_name };              \
	const ::DCLF::Zones::Scope DCLF_ZONE_JOIN(dclfZoneScope, __LINE__){ DCLF_ZONE_JOIN(dclfZoneSite, __LINE__) }; \
	ZoneScopedN(a_name)

#define DCLF_ZONE_NAMED(a_variable, a_name, a_active)                                                       \
	static const ::basic_telemetry::Callsite DCLF_ZONE_JOIN(dclfZoneSite, __LINE__){ a_name };              \
	const ::DCLF::Zones::Scope DCLF_ZONE_JOIN(dclfZoneScope, __LINE__){ DCLF_ZONE_JOIN(dclfZoneSite, __LINE__) }; \
	ZoneNamedN(a_variable, a_name, a_active)
