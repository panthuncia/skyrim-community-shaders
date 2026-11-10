#pragma once

#include <cstdint>
#include <memory>

/**
 * @brief Effects11's ORG FX runtime (CS_ORG_FX=1): the preset's .fx passes run as render-graph passes at the Tonemap anchor,
 * in place of FX11's EffectManager::ExecuteEffects. Design and limits: docs/development/org-fx-runtime.md, section 5.
 *
 * Each effect FX11 compiled is parsed from the same preprocessed text (OrgFx::Parse), lowered to bindless HLSL (OrgFx::LowerEffect)
 * and compiled per pass with DXC (RasterProgram from generated text) when a frame first needs that pass. The frame's chain is
 * planned on the render thread, before the anchor runs: colour correction, the 1024 downsample, enbbloom.fx, enblens.fx,
 * enbadaptation.fx (with the sun mask), enbeffect.fx, enbeffectpostpass.fx and the copy to the tonemap's output, with FX11's
 * ping-pong resolved into fixed graph textures. Each plan is one client registration; a change of shape (effects on or off, the
 * selected technique, UIBinding toggles, sizes, the output target) registers a new one.
 *
 * Values: FX11 stays the value store for now. The commit runs EffectManager::UpdateEffectValues for each effect, then copies the
 * variables' bytes (FX11 lays out $Globals as the parser does; checked per variable at build) into one latch region per pass, with
 * that pass's ScreenSize. Texture and sampler handles go in the push block.
 *
 * FX11 runs instead, with a log line naming why, whenever the runtime cannot run the frame: the render graph or anchor is missing, a
 * parse, lowering or compile failure, an unsupported construct, enbdepthoffield.fx running, a tonemap input other than kMAIN. A
 * failed graph returns Effects11 to FX11 for the session. Render thread throughout.
 *
 * Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGFxRuntime
{
public:
	/** @brief Whether CS_ORG_FX=1 is set. */
	static bool Requested();

	static ORGFxRuntime& Get();

	/** @brief Reserves the Tonemap anchor when requested. Before the render graph initializes (Effects11::PostPostLoad). */
	static void Reserve();

	/**
	 * @brief Drops every effect's ORG state and the registered passes. Called before FX11 reloads the effects (EffectManager::Apply),
	 * whose variables the commit reads; the next tonemap builds them again.
	 */
	void Invalidate();

	/**
	 * @brief At the tonemap hook, before FrameAnchors::Run(Tonemap): decides whether the graph runs this frame's chain and registers
	 * its passes when the frame's shape changed. a_input and a_output are the hook's RE::RENDER_TARGET values.
	 */
	void Prepare(std::uint32_t a_input, std::uint32_t a_output);

	/** @brief Whether this frame's chain ran on the graph and wrote the tonemap's output: Effects11 then skips FX11. */
	bool RanThisFrame() const;

	~ORGFxRuntime();

	struct Impl;

private:
	ORGFxRuntime() = default;

	std::unique_ptr<Impl> impl;
};
