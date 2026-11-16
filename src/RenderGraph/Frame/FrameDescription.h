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
	 * @brief The declared frame's segments, in frame order: the start stage, the plan's S1 (pre-depth: the EarlyPrepass clients), S2 (pre-main: the
	 * depth copy at Main::RenderDepth's end, Terrain Blending, the shadow mask, light culling and the Prepass clients) and S3
	 * (deferred: the deferred passes and the composite). The others are added by later stages.
	 */
	enum class SegmentId : std::uint8_t
	{
		Start,           // the frame's start: the reflections prepass, the reflection faces' sky (the faces cleared by its load operations), to DCLF's shadow view
		PreDepth,
		PreMain,
		Deferred,        // deferred+water: the deferred passes and the composite, the water mask, the RenderWorld scopes, the kMAIN copy, the water and its blend
		Post,            // the post stage: the volumetric lighting, the pre-tonemap composites, the upscaler's evaluate and the depth upscale,
		                 // the end-of-frame effects with the tonemap, to RenderEndOfFrameEffects' return
		Ui,              // the UI stage: the displays, the movie's tiles and the overlay, to just before the present; world and Bink frames
		MenuUi,          // the menu UI stage: the displays before and after a menu's 3D scene, the scene, its composite, the overlay;
		                 // the loading screen, the main menu and a menu over a frozen world
		Count
	};
	inline constexpr std::uint32_t kSegments = static_cast<std::uint32_t>(SegmentId::Count);

	/** @brief The most clients a segment describes one by one (SegmentFields::clientsOn, SegmentFields::instances). */
	inline constexpr std::uint32_t kSegmentClients = 32;

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
		// Per client, in the same order: how many instances of its passes do work this frame, for a client whose hook repeats inside the
		// segment (FrameAnchors::ClientDesc::instances). 0 for a client without instances.
		std::array<std::uint8_t, kSegmentClients> instances{};
		// A bit per step of the segment that holds another module's epoch (light culling): its passes do work this frame.
		std::uint32_t foreignOn = 0;
		// A hash of the revisions of the segment's passes recorded ahead (RenderGraphPass::RevisionHash): a pass whose recording
		// depends on something beyond its bindings and host data names it in its revision, and a kept recording binds it.
		std::uint64_t passes = 0;
		// A bit per declared row of the segment's clients (FrameAnchors::ClientDesc::rows, PassDesc::row: the engine effect steps the
		// description has on). A pass whose row bit is clear records only its barriers.
		std::array<std::uint64_t, 4> rows{};

		friend bool operator==(const SegmentFields&, const SegmentFields&) = default;
	};

	/**
	 * @brief The world post chain's structure for the frame (RenderGraph/Frame/PostChain.h): which of the chain's effects the post
	 * stage has on, read at Renderer::Begin from the engine's own state (PostChain.cpp names each). Variants the engine picks per
	 * frame (the ping-pong targets, the parities) are not here: every variant of an effect that is on is recorded, and its hook opens
	 * the one drawn (ChainSteps.inl).
	 */
	struct PostFields
	{
		bool read = false;          // the ImageSpace manager exists
		bool volumetric = false;    // the engine's volumetric lighting runs (VL enabled and its effect active): its composite
		bool sil = false;           // the SIL composite (+0x1d8)
		bool lensFlares = false;    // the image-based lens flares (+0x180)
		bool eof = false;           // gEnableEOFImageSpace: the end-of-frame slots run at all
		bool ldr = false;           // usesLDR (+0x210): slots 3 to 7 run after the tonemap
		std::uint8_t slots = 0;     // a bit per end-of-frame slot 0 to 7 whose effect is active (its IsActive)
		bool hdr = false;           // the HDR effect runs (else a copy takes its place)
		bool effects11 = false;     // Effects11 owns the tonemap: the HDR effect's tonemap is not called
		std::uint8_t bloom = 0;     // the bloom radius index (0 to 6): which bright-pass blur and blur
		bool fade = false;          // the tonemap's fade form (the manager's fade above 0)
		bool adapt = false;         // last frame's adapted luminance exists: the light adaptation draws the last level
		std::uint8_t lumLevels = 0; // the luminance chain's levels for kHDR_DOWNSAMPLE0's size
		bool taa = false;           // the engine's TAA runs: CS's upscaler method is TAA (without Upscaling, the engine's flag)
		std::uint8_t taaMode = 0;   // its form: 0 the plain one, 1 with the mask swap, 2 the UI one
		bool taaWater = false;      // its water form
		bool dynres = false;        // dynamic resolution scales the frame: the upsample runs
		bool dofCompute = false;    // the depth of field's blur takes the compute form (the compute-blur option)
		bool screenshot = false;    // the manager's screenshot copy (+0x211) runs this frame
		bool upscaler = false;      // CS's upscaler evaluates this frame (DLSS, FSR or XeSS)

		friend bool operator==(const PostFields&, const PostFields&) = default;
	};

	/**
	 * @brief The RenderWorld scopes' state (RenderGraph/Frame/PostChain.h, Scope): the SAO generation and
	 * composite (RenderSSAO, SE 100520 / AE 107245), the snow pass or its copy (Main::RenderWorld, 100424 / 107142), the screen-space
	 * reflections (100428 / 107146) and the water blend. Every field is read from the engine's own state at the scope's entry, named
	 * where it is read (PostChain.cpp).
	 */
	struct WorldFields
	{
		bool read = false;             // the ImageSpace manager exists
		std::uint8_t saoLevels = 0;    // the camera-Z chain's levels: the minify loop draws levels 1 to saoLevels - 1
		std::uint8_t saoContrast = 0;  // the level the minify loop draws with ISMinifyContrast (the first at most 16 pixels high), 0 for none
		bool saoTemporal = false;      // the raw AO's temporal form
		bool saoHalf = false;          // the SAO targets' half-resolution set
		std::uint8_t saoParity = 0;    // the raw AO target this frame (the helper's counter after RenderSSAO's increment)
		std::uint8_t saoForm = 0;      // the composite: 0 SAO, 1 SAO and fog, 2 fog, 3 a copy
		bool ssr = false;              // the reflections helper is active
		std::uint8_t ssrDownsample = 0;  // the normal mask's downsample: 0 none, 1 raster, 2 compute
		bool ssrBlur = false;          // the reflections' compute blur runs (the composite then reads its output)
		std::uint8_t normals = 4;      // the SSR mask target the engine reads this frame (GetImageSpaceRenderTarget_TAAMASK_SSRMASK_SWAP)
		bool snow = false;             // the snow pass runs (else the copy of kMAIN into its copy target)
		std::uint8_t waterHistory = 0;  // the water blend's history pair this frame

		friend bool operator==(const WorldFields&, const WorldFields&) = default;
	};

	/** @brief The world fields that differ, as DiffPost gives them. a_parity: the parity fields too. */
	std::string DiffWorld(const WorldFields& a_before, const WorldFields& a_after, bool a_parity = true);

	/** @brief The post fields that differ, as "name old -> new" joined by ", "; empty when equal. */
	std::string DiffPost(const PostFields& a_before, const PostFields& a_after);

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
		PostFields post{};  // read at Begin: the post stage's structure

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
