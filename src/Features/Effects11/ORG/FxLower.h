#pragma once

// The ORG FX runtime's lowering: turns a parsed .fx effect into HLSL that DXC compiles for BasicRHI's bindless ABI, one source
// per pass. std only. Design: docs/development/org-fx-runtime.md, section 4.
//
// What a lowered pass reads, which the runtime provides:
// - The push block (b0), LoweredEffect::PushWords() words: the heap index of a raw buffer (the values block), the byte offset
//   of this pass's region in it, then one word per LoweredEffect::resources entry: a texture's index in the resource heap, or a
//   sampler's in the sampler heap. The handles are fixed for a recording; the values change every frame, so they stay in the
//   region (a latch).
// - The region: the effect's constant buffers ($Globals, then each cbuffer, at LoweredEffect::regions), valuesSize bytes.
// - The draw: kQuadVertexCount vertices, no vertex buffer. The vertex wrapper makes Effects11's quad (two triangles with the
//   corners and UVs of its 4-vertex strip) from SV_VertexID and calls the effect's vertex entry.

#include "FxModel.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace OrgFx
{
	inline constexpr const char* kVertexEntry = "OrgFxVS";
	inline constexpr const char* kPixelEntry = "OrgFxPS";
	inline constexpr std::uint32_t kQuadVertexCount = 6;
	inline constexpr std::uint32_t kPushHeaderWords = 2;  // the values block's handle and the region's offset
	inline constexpr std::uint32_t kMaxPushWords = 32;    // Vulkan's guaranteed push constant size
	inline constexpr std::uint32_t kMaxResources = kMaxPushWords - kPushHeaderWords;

	/** @brief One entry of the handle table, in table order. */
	struct ResourceSlot
	{
		enum class Kind : std::uint8_t
		{
			Texture,
			Sampler,
		};
		Kind kind = Kind::Texture;
		std::string name;
		std::int32_t variable = -1;  // EffectDesc::variables, for a texture or a sampler declared without a state block
		std::int32_t state = -1;     // EffectDesc::states, for a sampler state block
	};

	/** @brief Where a constant buffer's bytes sit in a pass's region. */
	struct ValueRegion
	{
		std::string cbuffer;  // empty for $Globals
		std::uint32_t offset = 0;
		std::uint32_t size = 0;
	};

	/** @brief The effect's text with its declarations replaced, shared by every pass. */
	struct LoweredEffect
	{
		std::string body;
		std::vector<ResourceSlot> resources;
		std::vector<ValueRegion> regions;
		std::uint32_t valuesSize = 0;  // the regions' bytes: one pass's region
		std::vector<Diagnostic> diagnostics;
		bool ok = false;

		/** @brief Bytes of one pass's region. */
		std::uint32_t RegionSize() const { return valuesSize; }

		/** @brief Words of the push block: the header, then the handles, rounded up to whole 16-byte rows. */
		std::uint32_t PushWords() const { return (kPushHeaderWords + static_cast<std::uint32_t>(resources.size()) + 3u) / 4u * 4u; }
	};

	struct LoweredPass
	{
		std::string source;  // compile with entries kVertexEntry and kPixelEntry
		std::vector<Diagnostic> diagnostics;
		bool ok = false;
	};

	/**
	 * @brief Replaces the effect's uniform, resource and sampler declarations in place with loads from the values block and the
	 * descriptor heaps, and blanks techniques, groups, state objects, strings and annotations. Lines are kept, and a #line
	 * directive maps DXC's messages back to the parsed text. Fails when the effect has more than kMaxResources textures and
	 * samplers, which the push block cannot hold.
	 * @param a_text The text a_fx was parsed from.
	 */
	LoweredEffect LowerEffect(std::string_view a_text, const EffectDesc& a_fx);

	/** @brief The effect's body plus the two entry wrappers for one pass. Fails for anything but a vertex and a pixel shader. */
	LoweredPass LowerPass(const LoweredEffect& a_effect, const EffectDesc& a_fx, std::uint32_t a_technique, std::uint32_t a_pass);
}
