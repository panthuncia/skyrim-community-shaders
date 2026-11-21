#pragma once

// DCLF's profiling zones. Each is a Tracy zone, and while a telemetry phase records it is also a BasicTelemetry scope and a
// per-thread tally (BasicTelemetry's TallyScope), so a capture holds DCLF's CPU time by scope and thread in any build.
// Outside a phase a zone costs one relaxed load on top of Tracy. The BasicTelemetry scope is not opened then, because its
// constructor reads the session through an atomic shared pointer, which takes a lock.
// Use DCLF_ZONE("CS.DCLF.Name") and DCLF_ZONE_NAMED(variable, "CS.DCLF.Name", true). The name must be a string literal,
// since the callsite keeps a view of it.

#include <BasicTelemetry/Tallies.h>

namespace DCLF::Zones
{
	/** @brief One zone's extent on one thread, tallied while a phase records (basic_telemetry::talliesRecording). */
	using Scope = basic_telemetry::TallyScope;
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
