#pragma once

// Scene diagnostics state kept between hook points and reports. Each struct names its owner thread.
// Anything crossing between the coordinator and the render thread goes under a lock or by exchange.

#include "SceneView.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace DCLF::Diagnostics
{
	/** @brief A mirror probe slice, sent to the render thread. The render thread drops the held geometries. */
	struct MirrorProbeRequest
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
	};

	/** @brief A slice captured live, with each record's owning reference form ID, 0 above any reference's root. */
	struct MirrorProbe
	{
		SceneCapture::Records records;
		ankerl::unordered_dense::map<const void*, std::uint32_t> owners;
		// The engine hides and shows cull-hidden nodes around views within a frame, so a slice can catch one in between.
		// Their hidden bit is not compared. Sorted.
		std::vector<const void*> frameLocalHidden;
	};

	/** @brief Mirror record parity, also used by the compatibility monitor. */
	struct MirrorParityState
	{
		// Coordinator thread. The next slice's position, the frame it was asked in, and what the applied batch's events named.
		std::size_t probeCursor = 0;
		std::uint32_t probeFrame = ~0u;
		std::unique_ptr<MirrorProbe> batchProbe, probe;
		SceneMirror::KeySet eventKeys;
		SceneMirror::FieldMap eventFields;
		// Coordinator to render thread, one slice at a time, and the records back.
		RenderGraph::Frame::EventQueue<std::shared_ptr<MirrorProbeRequest>, 16> probeRequests;
		::RenderGraph::Frame::LatestSlot<MirrorProbe> probePosted;
		std::atomic<bool> probeAsked{ false };
		// The check by record type and field.
		struct Pending
		{
			std::uint8_t type = 0;
			std::uint32_t fields = 0;
			std::string what;
			std::uint32_t flagBits = 0;  // for a node, the flag bits that differ
			std::uint32_t owner = 0;     // MirrorProbe::owners
		};
		// A geometry's node and geometry records share its key.
		ankerl::unordered_dense::map<std::uintptr_t, Pending> pending;
		// Objects an event named since their last capture. A field differing on one of them has a hook.
		SceneMirror::KeySet named;
		struct Tally
		{
			std::uint64_t probes = 0, checked = 0, absent = 0;
			std::array<std::array<std::uint64_t, SceneMirror::kMaxFields>, SceneMirror::kTypes> evented{}, late{}, missed{};
			std::string firstAbsent, firstMissed, firstEvented;
			// Per type and field, the first missed object. Per node flag bit, how often it differed.
			std::array<std::array<std::string, SceneMirror::kMaxFields>, SceneMirror::kTypes> firstMissedBy;
			std::array<std::uint64_t, 32> flagBitsMissed{}, flagBitsEvented{};
		} tally;
	};

	/** @brief Scene diagnostics state. */
	struct SceneState
	{
		MirrorParityState mirror;
	};

	SceneState& SceneDiagnostics();
}
