#pragma once

#include <d3d11.h>

#include <cstdint>

namespace RE
{
	class BSRenderPass;
}

/**
 * @brief The engine's state flush and shader binds on the way to P5.13 (org-full-port.md; docs/development/p5/flush.md).
 *
 * The four engine functions (RE in flush.md, SE / AE):
 * - BSGraphics::SetDirtyStates (75580 / 77386): the one state flush. It applies the shadow state's dirty bits to D3D11.
 * - Renderer::SetShader (75550 / 77351): RSS.currentVertexShader, dirty bit 0x400, VSSetShader.
 * - Renderer::SetPixelShader (75555 / 77356): RSS.currentPixelShader, PSSetShader.
 * - Renderer::DispatchCSShader (75532 / 77329): SetDirtyStates(true), CSSetShader, CSSetConstantBuffers(0), Dispatch.
 *
 * Two diagnostics, both off by default, both through the guest context (CS_ORG_TRIPWIRE_GUEST=1), whose filter
 * (Tripwire::SetContextFilter) sees every engine call before it reaches D3D11:
 *
 * - CS_ORG_RESIDUE_CENSUS=1: counts every engine draw and dispatch that reaches D3D11, after DCLF and the families had their chance
 *   to withhold it. Each is classed by its draw function and its callers (Address Library IDs), the last BSShader technique (type,
 *   file or ImageSpace name, vertex and pixel descriptors) and the view (RSS render target 0, depth stencil, cube target). It also
 *   counts state flushes and shader binds that no draw follows. The report is logged every CS_ORG_RESIDUE_CENSUS_FRAMES main-thread
 *   frames (default 600), with the CS_ORG_RESIDUE_CENSUS_TOP biggest entries (default 25).
 *
 * - CS_ORG_ENGINE_FLUSH_LATCH=1: the four functions still run on the CPU (shadow state, dirty bits, deferred clears, input layouts),
 *   but the state they would bind goes nowhere: inside them the guest context drops every IASet, VSSet, PSSet, CSSet, OMSet and RSSet
 *   call and the dispatch. Map, Unmap (the alpha-test constant), clears and queries still go through. A draw from one of the engine's
 *   draw functions that then reaches D3D11 is skipped, because its state was never bound, and logged once per call site. The state a
 *   latched flush consumed is owed: a flush CS itself asks for (DCLF's ApplyPendingState, Grass Optimizations, ImageSpace) applies it
 *   first, with the latched shaders, so CS's D3D11 readers still see the engine's current state. CS code that asks D3D11 what the
 *   engine bound at any other point (PSGetShaderResources, OMGetRenderTargets, RSGetViewports) reads stale state: flush.md lists it.
 *   CS_ORG_ENGINE_FLUSH_LATCH_KEEP=<method>,... names binds that still reach D3D11 inside the four functions, to bisect those readers.
 *   CS_ORG_ENGINE_FLUSH_LATCH_UNBIND=1: a latched flush that consumed the target bit unbinds DXVK's targets, so DXVK holds no stale
 *   target whose views CS binds would then be dropped (InstallStateTrace). The next real flush binds the engine's targets as owed.
 *
 * Allowed engine draws: code that declines a draw on purpose (DCLF's objects not yet claimed, a port's inherent refusals) says so,
 * and the latch lets that draw through with the owed state applied, as for a flush CS asks for. Two forms:
 * - EngineDraws: a scope around a synchronous call of the engine's draw (a family handing a batch back, ImageSpace calling the
 *   engine's Render). Every flush and draw on this thread inside it is real.
 * - VouchPass: at registration, for a pass the engine's batch walk draws later (DCLF's PassCapture). The latch's
 *   BSBatchRenderer::RenderPassImmediately detour opens an EngineDraws scope for a vouched pass.
 * Each is counted by reason in the latch's periodic line. A draw nobody vouched for is still skipped and logged as an error.
 */
namespace RenderGraph::Engine::Flush
{
	/** @brief Whether CS_ORG_RESIDUE_CENSUS=1 asks for the residual engine-draw census. */
	bool CensusRequested();

	/** @brief Whether CS_ORG_ENGINE_FLUSH_LATCH=1 asks for the latch-only flush. */
	bool LatchRequested();

	/** @brief Whether the latch is in effect now: requested, installed, and the guest context in the engine's pointers. */
	bool Latched();

	/**
	 * @brief Main thread, Engine::InstallGameHooks, after Hooks.cpp's SetDirtyStates detour and before any other DispatchCSShader
	 * detour: the four detours, the guest context's filter and the main-thread report. Nothing when neither switch is on, or when
	 * CS_ORG_TRIPWIRE_GUEST is off (logged).
	 */
	void Install();

	/**
	 * @brief CS's BeginTechnique vertex-shader thunk (Hooks.cpp), which replaces the engine's SetShader call there: binds a_shader on
	 * D3D11 now, or owes it while Latched(). a_caller is the engine's call site, for the census.
	 */
	void BindVertexShader(ID3D11VertexShader* a_shader, void* a_caller);

	/** @brief The same for CS's BeginTechnique pixel-shader thunk (the engine's SetPixelShader call there). */
	void BindPixelShader(ID3D11PixelShader* a_shader, void* a_caller);

	/**
	 * @brief Why the engine draws something on purpose while the latch is on. Make one per reason, once (a static), and name the owner
	 * first: "dclf: object not yet claimed (main views)", "sky: cloud layer refused". At most 31 reasons; more share the last one.
	 */
	class Reason
	{
	public:
		explicit Reason(const char* a_name);
		std::uint8_t Id() const { return id; }

	private:
		std::uint8_t id;
	};

	/**
	 * @brief A scope in which the engine's draws on this thread are the engine's on purpose: with the latch on, its flushes apply the
	 * state latched flushes owe, its shader binds and dispatches reach D3D11, and its draws are let through and counted under a_reason.
	 * Open it in the code that declines a draw, just before it calls the engine's original draw, batch or flush, and keep it to that
	 * call. Nothing when the latch is off. Scopes nest; the innermost reason counts.
	 */
	class EngineDraws
	{
	public:
		explicit EngineDraws(const Reason& a_reason);
		~EngineDraws();
		EngineDraws(const EngineDraws&) = delete;
		EngineDraws& operator=(const EngineDraws&) = delete;

	private:
		int previous = -1;
		bool active = false;
	};

	/**
	 * @brief The rest of a batch the engine draws on purpose, from a point inside it: a draw family that refuses a draw partway
	 * (Features/DrawcallLimitFix/Core/Engine/Family) begins it at the refusal and ends it when the engine's batch call returns. Unlike an
	 * EngineDraws scope it does not nest, so the scopes a pass opens and closes inside the batch (RenderPassImmediately's for a vouched
	 * pass) do not end it; an open scope's reason counts first. Thread-local; nothing is checked with the latch off. Begin while one is
	 * open replaces its reason.
	 */
	void BeginEngineRest(const Reason& a_reason);
	void EndEngineRest();

	/**
	 * @brief globals::InstallD3DHooks, on DXVK's immediate context: the state readers' trace (StateTrace.cpp), with the census
	 * (CS_ORG_RESIDUE_CENSUS=1, CS_ORG_TRIPWIRE_GUEST=1) only. DXVK drops a shader resource bind whose resource is bound as an output,
	 * and a target bind unbinds the shader resources of its resource. With the latch on, DXVK keeps the targets the last real flush
	 * bound, so CS binds of views of those targets can be dropped. The trace logs each dropped bind and unbinding once per site, with
	 * what holds the resource and whether the engine's shadow state has moved off it.
	 */
	void InstallStateTrace(ID3D11DeviceContext* a_context);

	/**
	 * @brief A CS reader of DXVK's PS bindings (DCLF's captures, the families' headers, the frame buffers): a_view is what it read at
	 * a_register. A null read where the trace saw the last bind dropped or unbound is logged once per reader and register, with how
	 * it was lost. Nothing without the trace.
	 */
	void NoteRead(const char* a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view);

	/** @brief Whether VouchPass records anything (the latch is on). Test it before work that only feeds VouchPass. Any thread. */
	bool Vouching();

	/**
	 * @brief Any thread, where the engine registers a_pass with a batch renderer: the pass is the engine's to draw on purpose. When the
	 * batch walk draws it (BSBatchRenderer::RenderPassImmediately) in this frame or the next, its draws run in an EngineDraws scope
	 * of a_reason. The mark is kept per pass object and checked against the pass's geometry, so a reused pass is not vouched for.
	 */
	void VouchPass(const RE::BSRenderPass* a_pass, const Reason& a_reason);
}
