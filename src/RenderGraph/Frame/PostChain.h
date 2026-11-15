#pragma once

#include "RenderGraph/Frame/FrameDescription.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * @brief The post chain's description: which ImageSpace effects the engine's world post chain runs this frame, read by CS from the
 * engine's own state (docs/development/org-native-frame-graph.md, "The declared frame, stage 4").
 *
 * The chain is ImageSpaceManager::RenderWorldPostProcess (SE 100430, AE 107148): the volumetric lighting, then
 * RenderPreTonemapComposites (99024 / 105675: SIL, the image-based lens flares, the VL and lens flare composite), then
 * RenderEndOfFrameEffects (99023 / 105674: the end-of-frame slots, HDR or a copy, TAA, the LDR slots, the dynamic-resolution upsample).
 * Its order is fixed in the engine's code. What varies is which effects run and which variant each takes, and every one of those
 * decisions reads state CS can read too (PostFields).
 *
 * - **Read.** At Renderer::Begin (OnBegin fills Description::post) and again at the chain's decision event: the entry of
 *   RenderWorldPostProcess, after the game's update and the menus have set the ImageSpace data for the frame. The second read is
 *   the one the chain is predicted from; a field that differs from Begin's is counted by name (the engine decided it after Begin).
 * - **Predict.** PredictEffects turns the fields into the sequence of ImageSpace shaders the chain draws or dispatches, by their index
 *   in ImageSpaceManager::effects, reading the composites' children where the engine renders a list (the HDR effect's children, the
 *   bloom blur's).
 * - **Check.** The ImageSpace hooks note every shader the chain draws (NoteShader). At the chain's end the sequence is compared with
 *   the prediction. A difference is a tap error ("post not declared"): the description missed something the engine did.
 *
 * - **Rows.** Each predicted draw also names its step and variant (PredictDraws, RowKey): the row of the pass-state table
 *   (RenderGraph/Engine/ImageSpace/PostTable.h) that says what the draw's pass is. The ImageSpace port checks each draw against its row
 *   ("post state mismatch") and, in native mode, draws the chain from the rows in the two post segments (FrameAnchors::SegmentsOn).
 *   Decision events after Begin: RenderWorldPostProcess' entry, RenderEndOfFrameEffects' entry (its targets are its arguments), and
 *   the depth of field's first draw (its Render picks its composites as it runs).
 *
 * The check needs the ImageSpace hooks (CS_ORG_IMAGESPACE, which native mode turns on). CS_ORG_SEGMENT_POST=0 turns it off.
 */
namespace RenderGraph::Frame::PostChain
{
	/** @brief Installs the decision event's detour (RenderWorldPostProcess). With the ImageSpace hooks, native mode. */
	void Install();

	/** @brief Whether the check runs (installed, CS_ORG_SEGMENT_POST not 0). Any thread. */
	bool On();

	/** @brief Reads the post fields from the engine now. Render owner. read is false when the ImageSpace manager does not exist yet. */
	PostFields Read();

	/** @brief The two parts of the world post chain, each predicted from the fields read at its own decision event. */
	enum class Part : std::uint8_t
	{
		Composites,  // RenderWorldPostProcess up to RenderEndOfFrameEffects: the volumetric lighting and the pre-tonemap composites
		EndOfFrame,  // RenderEndOfFrameEffects (99023 / 105674)
		All,
	};

	/**
	 * @brief The ImageSpace shaders the world post chain (or a_part of it) draws for a_fields, in order, by their index in
	 * ImageSpaceManager::effects. An entry at or above kGroupBase is a group (kGroupVolumetricLighting and the others below): draws of
	 * the group's shaders whose order and count the description does not settle yet (one or more; the volumetric lighting's zero or
	 * more, since Volumetric Lighting's port takes its draws before the hooks see them).
	 */
	std::vector<std::uint16_t> PredictEffects(const PostFields& a_fields, Part a_part = Part::All);

	/**
	 * @brief A step of the world post chain that the static pass-state table describes (RenderGraph/Engine/ImageSpace/PostTable.h).
	 * A step with its variant (RowKey) selects one table row: what that draw's pass is (its program, targets, samplers, blend and
	 * viewport). Groups (kGroupBase and up) have no step: their draws are not declared one by one yet.
	 */
	enum class Step : std::uint8_t
	{
		None,
		SilRaw,        // ISSILRawInd (parity: silParity)
		SilComposite,  // ISSILComposite (parity: silParity)
		Composite,     // the VL / lens flare composite (index: its form, 0 VL, 1 lens flare, 2 both, + its first sampler's address mode * 16; parity: vlParity with VL)
		Slot,          // an end-of-frame slot's effect drawn as one shader (index: the slot, + 0x80 after the tonemap; FXAA)
		Copy,          // the copy in the HDR effect's place when HDR is off
		Downsample,    // the HDR effect's scene downsample
		Luminance,     // a luminance level (index: the level; parity: lumParity on the last level, else 0)
		Adapt,         // the light adaptation into the last level (parity: that level's target)
		Bloom,         // a bloom composite's child (index: radius * 8 + child; parity: the luminance target the bright pass reads, 0 for the blur)
		Tonemap,       // the HDR tonemap (index: 1 for the fade form; parity: the luminance target it reads)
		Taa,           // the engine's TAA (index: its shader, 0 plain, 1 UI, 2 water, + 0x10 reading the normals' swap; parity: taaParity)
		Screenshot,    // the screenshot copy
		Upsample,      // the dynamic-resolution upsample
		Dof,           // a depth-of-field draw (index: its step * 8 + its draw in the step, + 0x80 after the tonemap)
		// The RenderWorld scopes (Scope, below).
		SaoCameraZ,     // ISSAOCameraZ (index: + 0x20 for the half-resolution set)
		SaoMinify,      // a camera-Z level (index: the level, + 0x40 for ISMinifyContrast)
		SaoRawAo,       // the raw AO (index: 0 temporal, 1 not, + 0x20 half; parity: its target, + 2 when the SSR mask target is the swap)
		SaoBlur,        // the AO blur (index: 0 horizontal, 1 vertical, + 0x20 half; parity: the raw target the horizontal reads)
		SaoComposite,   // the SAO composite into kMAIN (index: its form, + 0x10 reading the blurred reflections, + 0x20 half; parity: the SSR mask swap)
		SnowSss,        // the snow's subsurface scattering into kMAIN's copy
		SnowCopy,       // the copy of kMAIN into its copy when there is no snow
		SsrDownsample,  // the SSR mask's downsample (index: 1 raster, 2 compute; parity: the mask swap)
		SsrRayMarch,    // the reflections' ray march (index: 1 from the downsampled mask; parity: the mask swap when not)
		SsrBlur,        // the reflections' compute blur (index: 0 horizontal, 1 vertical)
		WaterBlend,     // the water's temporal blend (index: its third sampler's address * 16 + its fifth's; parity: its history, + 2 for the mask swap)
		Count
	};

	/** @brief "luminance", for logs and the table. */
	const char* StepName(Step a_step);

	/**
	 * @brief Which table row a step's draw takes: the step, its index and parity, and for the steps whose targets follow
	 * RenderEndOfFrameEffects' ping-pong, the target it reads and the one it writes (RE::RENDER_TARGET values, -1 for the others).
	 */
	struct RowKey
	{
		Step step = Step::None;
		std::uint8_t index = 0;
		std::uint8_t parity = 0;
		std::int8_t in = -1;
		std::int8_t out = -1;

		friend auto operator<=>(const RowKey&, const RowKey&) = default;
	};

	/** @brief "luminance 3 p1 27->28". */
	std::string RowName(const RowKey& a_key);

	/** @brief One draw the description predicts: its effect index (or a group), and the row it takes (Step::None for a group). */
	struct Draw
	{
		std::uint16_t effect = 0;
		RowKey row;
	};

	inline constexpr std::uint16_t kGroupBase = 0x8000;
	inline constexpr std::uint16_t kGroupVolumetricLighting = kGroupBase + 0;  // the engine's VL apply and blur (Volumetric Lighting's port)
	inline constexpr std::uint16_t kGroupLensFlares = kGroupBase + 1;          // the image-based lens flares (+0x180)
	inline constexpr std::uint16_t kGroupDepthOfField = kGroupBase + 2;        // slot 3
	inline constexpr std::uint16_t kGroupFullScreenBlur = kGroupBase + 3;      // slot 6
	inline constexpr std::uint16_t kGroupGetHit = kGroupBase + 4;              // slot 7
	inline constexpr std::uint16_t kGroupWorldMap = kGroupBase + 5;            // slot 0
	inline constexpr std::uint16_t kGroupRadialBlur = kGroupBase + 6;          // slot 5
	inline constexpr std::uint16_t kGroupRefraction = kGroupBase + 7;          // slot 1

	/**
	 * @brief PredictEffects with each draw's row: the 99023 / 105674 ping-pong between the source, the swap target and the destination is
	 * followed as the engine follows it (eofSource, eofSwap, eofDest), so each step knows the targets it reads and writes.
	 */
	std::vector<Draw> PredictDraws(const PostFields& a_fields, Part a_part = Part::All);

	/**
	 * @brief The RenderWorld scopes the table describes too (docs/development/org-native-frame-graph.md, "The declared frame, stage 5"):
	 * each is predicted from the world fields read at its entry, its own decision event, and its draws are checked against their rows as
	 * the chain's are. Main::RenderWorld (SE 100424, AE 107142) runs them in this order, the reflections inside FinishAccumulating before
	 * it returns to the SAO, and the water blend later, after the water's draws.
	 */
	enum class Scope : std::uint8_t
	{
		Reflections,    // the screen-space reflections (100428 / 107146)
		Ssao,           // RenderSSAO's first call: the camera depth, its levels, the raw AO and its blur
		Snow,           // the snow pass (RenderSnowSSS, 100537 / 107267) or the copy of kMAIN into its copy target
		SsaoComposite,  // RenderSSAO's second call: the composite into kMAIN
		WaterBlend,     // the water's temporal blend (ISWaterBlend)
		Count,
		None = Count
	};

	/** @brief "reflections". */
	const char* ScopeName(Scope a_scope);

	/** @brief Reads the world fields a_scope selects from the engine now (its entry). Render owner. */
	WorldFields ReadWorld(Scope a_scope, const WorldFields& a_base);

	/** @brief The draws a_scope makes for a_fields, each with its row. */
	std::vector<Draw> PredictScope(Scope a_scope, const WorldFields& a_fields);

	/**
	 * @brief The ImageSpace port at a scope's entry and exit: the scope's draws are predicted (CurrentRow then gives their rows) and,
	 * at its end, compared with what it drew (a "post not declared" tap error on a difference). Render owner.
	 */
	void ScopeStart(Scope a_scope);
	void ScopeEnd();

	/** @brief The scope running now, or Scope::None. Render owner. */
	Scope CurrentScope();

	/** @brief As SetPartHook, for each scope's decision event (its entry), with its predicted draws. Render owner. */
	void SetScopeHook(void (*a_hook)(Scope a_scope, const std::vector<Draw>& a_draws));

	/**
	 * @brief The world fields this frame's scopes are declared with: this frame's, as far as its scopes read them, completed by the
	 * slot's last frame's (the decisions repeat from frame to frame). What the world segment's rows are selected from. Render owner.
	 */
	WorldFields DeclaredWorld();

	/**
	 * @brief Inside the world post chain, after NoteShader for the draw or dispatch in progress: the row the description declared for
	 * it. Null outside the chain, for a group's draw, and once the chain left its prediction (a "post not declared" error at its end).
	 * Render owner.
	 */
	const RowKey* CurrentRow();

	/** @brief The table check of one declared step's draw (ImageSpace's CheckRow): a_matched when it matched its row. Render owner. */
	void NoteRowCheck(bool a_matched);

	/** @brief The part of the world chain running now: Composites, EndOfFrame, or All outside the chain. Render owner. */
	Part CurrentPart();

	/**
	 * @brief Called at each part's decision event (RenderWorldPostProcess' entry for the composites, RenderEndOfFrameEffects' entry for
	 * the rest) with the draws predicted for that part, before any of them: the post segments decide there whether the part draws from
	 * the pass-state table. One hook (ImageSpace's); installing another replaces it. Render owner.
	 */
	void SetPartHook(void (*a_hook)(Part a_part, const std::vector<Draw>& a_draws));

	/** @brief Whether the world post chain is running now (RenderWorldPostProcess's entry to its return). Render owner. */
	bool InChain();

	/**
	 * @brief The post fields the chain of this frame slot was last predicted from, completed at its decision events: the best
	 * description of the slot's next chain at Begin (the decisions made after Begin repeat from frame to frame). a_have false when
	 * the slot had no chain yet; then Begin's read. Render owner.
	 */
	PostFields SlotFields(std::uint32_t a_slot, bool* a_have = nullptr);

	/** @brief The fields the running chain was predicted from (both parts read by now), else the current frame's Begin read. Render owner. */
	PostFields ChainFields();

	/**
	 * @brief The fields this frame's chain is declared with: the running (or finished) chain's, read at its decision events, once it
	 * started this frame; before that, the slot's last chain's (SlotFields). What the post segments' rows are selected from, at Begin
	 * and again at their end taps. Render owner.
	 */
	PostFields DeclaredFields();

	/** @brief "ISHDRDownSample4" for an effect index, a group's name for a group. */
	std::string EffectName(std::uint16_t a_effect);

	/** @brief Renderer::Begin (FrameDriver::OnBegin): fills a_frame.post and counts how often the description changes. Render owner. */
	void OnBegin(Description& a_frame);

	/**
	 * @brief The ImageSpace hooks: the engine is about to draw or dispatch an ImageSpace shader. a_effect is the shader's ImageSpaceEffect
	 * (BSImagespaceShader::Render's this) or the BSImagespaceShader itself (the explicit Render and DispatchComputeShader). Counted only
	 * inside the world post chain. Render owner.
	 */
	void NoteShader(const void* a_effect);

	/** @brief The census line (CS_ORG_EPOCH_STATS's report): chains checked and matched, and the description's changes by field. */
	void LogStats();
}
