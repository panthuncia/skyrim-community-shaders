#pragma once

// Core's side of node capture contributions: each class's entry, which the scene capture reads for every node it records,
// and the push a feature's hooks make when a contributed list changes.

#include "Features/DrawcallLimitFix/Api/EngineEvents.h"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace RE
{
	class NiAVObject;
}

namespace DCLF::Extensions
{
	/** @brief A registered node capture: its name, resolved RTTI and capture. */
	struct NodeCaptureEntry
	{
		std::string name;
		const void* rtti = nullptr;
		decltype(Api::NodeCaptureContribution::capture) capture = nullptr;
	};

	/** @brief Main thread, during InstallFeatures. Adds a_capture. Fails for a missing field or a class another contribution has. */
	void AddNodeCapture(std::string_view a_feature, const Api::NodeCaptureContribution& a_capture);
	/** @brief Any thread, after InstallFeatures. The contribution whose class has exactly a_rtti, or null. */
	const NodeCaptureEntry* NodeCaptureOf(const void* a_rtti);

	/**
	 * @brief Reports a feature's own store to a_node's a_fields. Captured on the storing thread after the store, in the engine-read window,
	 * in order with that thread's earlier pushes. Null is ignored.
	 */
	void PushNodeUpdate(const RE::NiAVObject* a_node, std::uint32_t a_fields);
}
