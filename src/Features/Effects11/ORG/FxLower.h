#pragma once

// The ORG FX runtime's lowering: turns a parsed .fx effect into HLSL that DXC compiles for BasicRHI's bindless ABI, one source
// per pass. std only. Design: docs/development/org-fx-runtime.md, section 4.
//
// What a lowered pass reads, which the runtime provides:
// - The push block (b0), PassLayout::pushWords words: the heap index of a raw buffer (the values block), the byte offset of
//   this pass's region in it, then the handles: a texture's index in the resource heap, or a sampler's in the sampler heap, at
//   the word PassLayout::words names for each LoweredEffect::resources entry. An effect with at most kMaxResources textures and
//   samplers has one word per entry, in order. A larger one gives each pass only the entries its shaders can reach, and two
//   words for the rest: kUnusedTextureWord (a black texture) and kUnusedSamplerWord (any sampler). The handles are fixed for a
//   recording; the values change every frame, so they stay in the region (a latch).
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
	// A pass layout for an effect with more than kMaxResources entries: the words its unreached entries read.
	inline constexpr std::uint32_t kUnusedTextureWord = kPushHeaderWords;
	inline constexpr std::uint32_t kUnusedSamplerWord = kPushHeaderWords + 1;
	inline constexpr std::uint32_t kMaxReachedResources = kMaxPushWords - kPushHeaderWords - 2;

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

	/** @brief The identifiers a declaration's text names, for the reach of a pass's shaders. */
	struct References
	{
		std::string name;
		std::vector<std::string> identifiers;
	};

	/** @brief The effect's text with its declarations replaced, shared by every pass. */
	struct LoweredEffect
	{
		std::string body;  // after the push block's prologue, which each pass makes (LowerPass)
		std::vector<ResourceSlot> resources;
		std::vector<ValueRegion> regions;
		std::uint32_t valuesSize = 0;  // the regions' bytes: one pass's region
		std::vector<Diagnostic> diagnostics;
		bool ok = false;
		// What functions, structs and classes (their member functions) and static globals name: a pass reaches a resource when
		// its entries name it, directly or through these.
		std::vector<References> references;

		/** @brief Bytes of one pass's region. */
		std::uint32_t RegionSize() const { return valuesSize; }

		/** @brief Words of a push block with one handle per entry: the header, then the handles, rounded up to whole 16-byte rows. */
		std::uint32_t PushWords() const { return (kPushHeaderWords + static_cast<std::uint32_t>(resources.size()) + 3u) / 4u * 4u; }
	};

	/** @brief Where one pass's push block holds each handle. */
	struct PassLayout
	{
		std::vector<std::uint32_t> words;  // per LoweredEffect::resources entry: its push word
		std::uint32_t pushWords = 0;       // whole 16-byte rows
		bool shared = false;               // unreached entries share kUnusedTextureWord and kUnusedSamplerWord
		std::uint32_t reached = 0;         // entries with a word of their own
		bool ok = false;
		std::string error;
	};

	struct LoweredPass
	{
		std::string source;  // compile with entries kVertexEntry and kPixelEntry
		PassLayout layout;
		std::vector<Diagnostic> diagnostics;
		bool ok = false;
	};

	/**
	 * @brief Replaces the effect's uniform, resource and sampler declarations in place with loads from the values block and the
	 * descriptor heaps, and blanks techniques, groups, state objects, strings and annotations. Lines are kept, and a #line
	 * directive maps DXC's messages back to the parsed text.
	 * @param a_text The text a_fx was parsed from.
	 */
	LoweredEffect LowerEffect(std::string_view a_text, const EffectDesc& a_fx);

	/**
	 * @brief Where a pass's push block holds each handle. One word per entry, in order, when the effect has at most kMaxResources.
	 * Otherwise only the entries the pass's entries reach by name (through the functions, classes and static globals they name)
	 * get words of their own, and the pass fails when they are more than kMaxReachedResources.
	 */
	PassLayout LayoutPass(const LoweredEffect& a_effect, const EffectDesc& a_fx, std::uint32_t a_technique, std::uint32_t a_pass);

	/**
	 * @brief The push block's prologue, the effect's body and the two entry wrappers for one pass. A compute shader on the pass is
	 * left out (its draw never runs it). Fails for a geometry, hull or domain shader.
	 */
	LoweredPass LowerPass(const LoweredEffect& a_effect, const EffectDesc& a_fx, std::uint32_t a_technique, std::uint32_t a_pass);
}
