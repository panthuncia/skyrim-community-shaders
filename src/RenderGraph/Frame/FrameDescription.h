#pragma once

#include "RenderGraph/Frame/SegmentId.h"

#include <array>
#include <cstdint>
#include <string>

/**
 * @brief What CS decides a frame contains, read at Renderer::Begin from state it reads itself.
 *
 * A plain value that can be hashed and diffed field by field, so a re-recorded segment can log which field changed.
 * See docs/development/org-native-frame-graph.md, "The declared frame". This header stays free of ORG includes.
 */
namespace RenderGraph::Frame
{
	/** @brief What kind of frame the engine renders, decided from the menu stack, the loading screen and Bink events. */
	enum class Kind : std::uint8_t
	{
		World,        // no menu freezes the background and the player has 3D
		FrozenWorld,  // a freeze-background menu is open, so the world is captured once and then composited
		MainMenu,     // the main menu, or any frame before the player has 3D
		Loading,      // runs on the loading loop's thread
		Bink,         // begun by BSMoviePlayer::Tick, such as the startup logo
		Count
	};

	const char* KindName(Kind a_kind);

	/** @brief The most clients a segment describes one by one. */
	inline constexpr std::uint32_t kSegmentClients = 32;

	/** @brief What a segment's recording depends on. Two frames with equal fields submit the same kept recording. */
	struct SegmentFields
	{
		bool declared = false;        // the frame contains the segment, so its end tap is expected
		std::uint64_t graph = 0;      // 0 when not built yet
		// Moves when a slot the graph binds publishes a new version, such as after a resize.
		// A recording binds the versions it was recorded with, so a new version needs a new recording.
		std::uint64_t versions = 0;
		std::uint32_t clients = 0;
		std::uint32_t clientsOn = 0;  // a bit per client in the graph's client order
		// Per client, how many instances of its passes run this frame. 0 for a client without instances.
		std::array<std::uint8_t, kSegmentClients> instances{};
		// A bit per step that holds another module's epoch, such as light culling.
		std::uint32_t foreignOn = 0;
		// A hash of the pass revisions. A pass whose recording depends on more than its bindings names that in its revision.
		std::uint64_t passes = 0;
		// A bit per declared row of the segment's clients. A pass whose row bit is clear records only its barriers.
		std::array<std::uint64_t, 4> rows{};

		friend bool operator==(const SegmentFields&, const SegmentFields&) = default;
	};

	/**
	 * @brief Which of the world post chain's effects are on this frame, read from the engine's own state.
	 * Per-frame variants such as ping-pong targets are not here, since every variant of an effect that is on is recorded.
	 */
	struct PostFields
	{
		bool read = false;          // the ImageSpace manager exists
		bool volumetric = false;    // VL enabled and its effect active
		bool sil = false;           // +0x1d8
		bool lensFlares = false;    // +0x180
		bool eof = false;           // gEnableEOFImageSpace
		bool ldr = false;           // usesLDR (+0x210), slots 3 to 7 run after the tonemap
		std::uint8_t slots = 0;     // a bit per active end-of-frame slot 0 to 7
		bool hdr = false;           // else a copy takes its place
		bool effects11 = false;     // Effects11 owns the tonemap, so the HDR effect's tonemap is not called
		std::uint8_t bloom = 0;     // bloom radius index, 0 to 6
		bool fade = false;          // the manager's fade is above 0
		bool adapt = false;         // last frame's adapted luminance exists
		std::uint8_t lumLevels = 0; // levels for kHDR_DOWNSAMPLE0's size
		bool taa = false;
		std::uint8_t taaMode = 0;   // 0 plain, 1 with the mask swap, 2 the UI form
		bool taaWater = false;
		bool dofCompute = false;
		bool screenshot = false;    // +0x211
		bool upscaler = false;      // DLSS, FSR or XeSS

		friend bool operator==(const PostFields&, const PostFields&) = default;
	};

	/**
	 * @brief The RenderWorld scopes' state, read from the engine at each scope's entry.
	 * Covers SAO (SE 100520 / AE 107245), snow (100424 / 107142), SSR (100428 / 107146) and the water blend.
	 */
	struct WorldFields
	{
		bool read = false;             // the ImageSpace manager exists
		std::uint8_t saoLevels = 0;    // the minify loop draws levels 1 to saoLevels - 1
		std::uint8_t saoContrast = 0;  // the first level at most 16 pixels high, drawn with ISMinifyContrast; 0 for none
		bool saoTemporal = false;
		bool saoHalf = false;
		std::uint8_t saoParity = 0;    // the helper's counter after RenderSSAO's increment
		std::uint8_t saoForm = 0;      // 0 SAO, 1 SAO and fog, 2 fog, 3 a copy
		bool ssr = false;
		std::uint8_t ssrDownsample = 0;  // 0 none, 1 raster, 2 compute
		bool ssrBlur = false;
		std::uint8_t normals = 4;      // GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP
		bool snow = false;             // else kMAIN is copied into its copy target
		std::uint8_t waterHistory = 0;

		friend bool operator==(const WorldFields&, const WorldFields&) = default;
	};

	/** @brief The post fields that differ, as "name old -> new"; empty when equal. */
	std::string DiffPost(const PostFields& a_before, const PostFields& a_after);

	struct Description
	{
		Kind kind = Kind::MainMenu;
		bool capture = false;      // FrozenWorld only, the frame that draws the world once
		// DCLF has started drawing the world, which never stops once started. Before that every clear keeps its own ticket.
		bool dclfDraws = false;
		std::uint64_t frame = 0;   // every Renderer::Begin, any thread
		std::uint32_t slot = 0;    // selects the published copy and latch regions
		std::uint32_t parity = 0;  // a slot always has the same parity
		std::array<SegmentFields, kSegments> segments{};
		PostFields post{};

		const SegmentFields& Segment(SegmentId a_segment) const { return segments[static_cast<std::size_t>(a_segment)]; }
		SegmentFields& Segment(SegmentId a_segment) { return segments[static_cast<std::size_t>(a_segment)]; }
	};

	/** @brief The revision of a segment's recording. Never 0. */
	std::uint64_t FieldsHash(const SegmentFields& a_fields);

	/** @brief The fields that differ, as "name old -> new"; empty when equal. */
	std::string DiffFields(const SegmentFields& a_before, const SegmentFields& a_after);

	/** @brief The frame-level fields that differ. The frame number, slot and parity are left out. */
	std::string DiffFrame(const Description& a_before, const Description& a_after);
}
