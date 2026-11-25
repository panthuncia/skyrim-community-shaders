#pragma once

#include "RenderGraph/BufferRef.h"

// The index pool holds every geometry slot's indices in one buffer, which every plain indexed draw binds.

// volk must come before every Vulkan header in this translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"

#include "RenderGraph/ComputeProgram.h"
#include <ORGModuleServices/GrowableBufferTypes.h>
#include <rhi.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	/** @brief Every geometry slot's indices in one buffer, so plain indexed draws bind one index buffer and keep vertex reuse.
	 * Ranges are per slot and never shared, since freed buffer addresses are reused at once. Render thread. */
	struct IndexPool
	{
		static constexpr std::uint32_t kNoRange = ~0u;
		// A slot's range. The count is even to keep 4-byte alignment.
		struct Range
		{
			std::uint32_t first = kNoRange, count = 0;
			std::uint64_t address = 0, bytes = 0;
			RenderGraph::BufferRef ref;
		};
		Versioned indices;  // 16-bit
		Versioned firsts;   // per geometry slot, kNoRange without one
		Versioned copies;   // this commit's copies
		// The versions the last update filled. A new indices version is laid out again and every range recopied.
		org::services::FilledVersion indicesFilled, firstsFilled;
		// Slots waiting for growth, retried by every update.
		std::vector<std::uint32_t> waiting;
		// The serial of an indices version found fragmented despite twice the room. The producer then requires a newer version.
		// A serial alone, so relaxed order is enough.
		std::atomic<std::uint64_t> fragmentedAt{ 0 };
		// The revision producer's bound, every slot's index count kept from the log.
		struct Bound
		{
			LogCursor cursor;
			std::vector<std::uint32_t> indices;
			std::uint64_t total = 0;
		} bound;
		std::shared_ptr<const ComputeProgram> program;
		rhi::CommandSignaturePtr dispatchSignature;
		LogCursor cursor;
		std::vector<Range> slots;
		std::vector<std::uint32_t> slotFirst;                       // what firsts holds
		std::uint64_t firstsSerial = 0;                             // a new value whenever slotFirst or a range's address may have changed
		std::map<std::uint32_t, std::uint32_t> free;                // first -> count
		std::uint32_t end = 0;                                      // past the last range ever given out
		std::uint64_t indicesHeld = 0;                              // live indices, for the report
	};
}
