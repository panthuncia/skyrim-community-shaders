#pragma once

#include <array>
#include <cstdint>
#include <string>

/**
 * @brief The declared frame's description: what CS decides a frame contains, at the frame's start (Renderer::Begin), from state it
 * reads itself. docs/development/org-native-frame-graph.md, "The declared frame".
 *
 * A plain value. It can be hashed and compared, and two descriptions can be diffed field by field, so a segment whose recording is
 * made again can log which field changed. Later stages add fields to SegmentFields or a segment's own struct here. The frame's own
 * fields (kind, slot, parity) stay as they are.
 *
 * Header deliberately free of ORG includes.
 */
namespace RenderGraph::Frame
{
	/** @brief What kind of frame the engine renders. Decided at Renderer::Begin, from the menu stack, the loading screen and Bink events. */
	enum class Kind : std::uint8_t
	{
		World,        // the main frame draws the world (no menu freezes the background, the player has 3D)
		FrozenWorld,  // a menu with the freeze-background flag is open: the world is drawn once (capture) and then composited
		MainMenu,     // the main frame without the world: the main menu, or any frame before the player has 3D
		Loading,      // a loading screen's frame, on the loading loop's thread
		Bink,         // a Bink movie's own frame (BSMoviePlayer::Tick began it: the startup logo, a blocking movie)
		Count
	};

	/** @brief "world", "frozen world", "main menu", "loading screen" or "Bink movie". */
	const char* KindName(Kind a_kind);

	/**
	 * @brief The declared frame's segments, in frame order: the plan's S1 (pre-depth: the EarlyPrepass clients), S2 (pre-main: the
	 * depth copy at Main::RenderDepth's end, Terrain Blending, the shadow mask, light culling and the Prepass clients) and S3
	 * (deferred: the deferred passes and the composite). The others are added by later stages.
	 */
	enum class SegmentId : std::uint8_t
	{
		PreDepth,
		PreMain,
		Deferred,
		Count
	};
	inline constexpr std::uint32_t kSegments = static_cast<std::uint32_t>(SegmentId::Count);

	/** @brief "pre-depth". */
	const char* SegmentName(SegmentId a_segment);

	/**
	 * @brief What a segment's recording depends on. Its hash is the revision the segment's recording is made for (FieldsHash), so
	 * two frames with equal fields submit the same kept recording.
	 */
	struct SegmentFields
	{
		bool declared = false;        // the frame contains the segment: its end tap is expected, and its recording is submitted there
		std::uint64_t graph = 0;      // the build of the segment's graph the fields were read against (0: not built yet)
		// The segment's version generation: it moves when a slot its graph binds published a new version (a resize, a recreated
		// engine target), found by checking the kept recording (GraphHost::EpochRecordingVersionsCurrent). A recording binds the
		// versions it was recorded with, so a new version is a new description.
		std::uint64_t versions = 0;
		std::uint32_t clients = 0;    // the segment's clients in that graph
		std::uint32_t clientsOn = 0;  // a bit per client, in the graph's client order: its passes do work this frame
		// A bit per step of the segment that holds another module's epoch (light culling): its passes do work this frame.
		std::uint32_t foreignOn = 0;
		// A hash of the revisions of the segment's passes recorded ahead (RenderGraphPass::RevisionHash): a pass whose recording
		// depends on something beyond its bindings and host data names it in its revision, and a kept recording binds it.
		std::uint64_t passes = 0;

		friend bool operator==(const SegmentFields&, const SegmentFields&) = default;
	};

	/** @brief One frame's description. */
	struct Description
	{
		Kind kind = Kind::MainMenu;
		bool capture = false;      // FrozenWorld: this frame draws the world once, for the frozen background
		// Drawcall Limit Fix draws the world (its first claims are installed; it never stops once started). Its epochs then run in a
		// world frame, so the clears they host run there. Before that the engine draws the world itself, and every clear keeps its
		// own ticket at the engine's call.
		bool dclfDraws = false;
		std::uint64_t frame = 0;   // frames begun so far (every Renderer::Begin, any thread)
		std::uint32_t slot = 0;    // frame modulo the frames in flight: the published copy and latch regions this frame uses
		std::uint32_t parity = 0;  // frame modulo 2: a slot always has the same parity
		std::array<SegmentFields, kSegments> segments{};

		const SegmentFields& Segment(SegmentId a_segment) const { return segments[static_cast<std::size_t>(a_segment)]; }
		SegmentFields& Segment(SegmentId a_segment) { return segments[static_cast<std::size_t>(a_segment)]; }
	};

	/** @brief The revision of a segment's recording: a hash of its fields. Never 0. */
	std::uint64_t FieldsHash(const SegmentFields& a_fields);

	/** @brief The fields that differ, as "name old -> new" joined by ", "; empty when equal. */
	std::string DiffFields(const SegmentFields& a_before, const SegmentFields& a_after);

	/** @brief The frame-level fields that differ (kind, capture, DCLF draws), the same way. The frame number, slot and parity are left out. */
	std::string DiffFrame(const Description& a_before, const Description& a_after);
}
