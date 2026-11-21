#pragma once

// Types for the frame schedule in Schedule/Frame.cpp: the engine points epochs are submitted at, each DCLF epoch and its pass keys,
// the epochs outside DCLF, and the frame and async orders.

#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace DCLF::Api
{
	/** @brief Which extension's resources an epoch's passes are made from. */
	enum class EpochShape : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};

	/** @brief The main view's shapes: the depth segment and the colour segment. */
	enum class MainShape : std::uint32_t
	{
		Depth,
		Colour,
	};

	/**
	 * @brief An engine point an epoch is submitted at. Its owner's hook marks it active around its DCLF work.
	 * Submitting an epoch while a different trigger is active is counted and logged once.
	 */
	struct TriggerDef
	{
		std::string_view id;     // "shadow-maps.after", "depth.committed", ...
		std::string_view owner;  // whose hook fires it, such as "Core" or "Features/Forward"
		std::string_view where;  // the engine point and the hook
	};

	/** @brief A schedule trigger's row in FrameSchedule::triggers. */
	struct TriggerId
	{
		std::uint32_t index = ~0u;
		explicit operator bool() const { return index != ~0u; }
		bool operator==(const TriggerId&) const = default;
	};

	/** @brief An epoch and its pass keys in order. Its index in FrameSchedule::epochs is its index everywhere in the scene revisions. */
	struct EpochDef
	{
		std::string_view id;                       // "main.depth", "main.colour", "shadow", "reflection", "forward", "fp.depth", "fp.colour"
		std::string_view label;                    // name in reports
		std::string_view trigger;                  // TriggerDef id
		std::uint32_t segment = 0;                 // RenderGraphRuntime::Segment value
		EpochShape shape = EpochShape::Main;
		// Main uses MainShape, Shadow uses 0, View uses the Api::EngineView.
		std::uint32_t shapeIndex = 0;
		// The epoch whose shape this one records against. Empty for its own shape.
		std::string_view sharesWith;
		bool claims = false;    // the frame's claims need this epoch's recordings
		bool revision = true;   // each submission carries the scene revision's recording
		// An earlier Main epoch that runs this epoch's passes, such as the reflection's faces inside the depth epoch.
		// The host lists them as "<id>/<key>" and commits them. A hosted epoch has no segment, recording or submission of its own.
		std::string_view hostedBy;
		std::span<const std::string_view> passes;
	};
	/** @brief Separates a hosted epoch's id from its pass key in the host's list. */
	inline constexpr char kHostedKey = '/';

	/**
	 * @brief The frame's triggers and epochs, plus the frame order by id. The host's async ticket loop runs the same order.
	 * Every epoch appears once in the order, except hosted epochs, which run as their host.
	 */
	struct FrameSchedule
	{
		std::span<const TriggerDef> triggers;
		std::span<const EpochDef> epochs;
		std::span<const std::string_view> frameOrder;
	};
}
