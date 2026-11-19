#pragma once

#include <d3d11.h>

#include <cstdint>

namespace RE
{
	class BSRenderPass;
}

/**
 * @brief Diagnostics around the engine's state flush (SetDirtyStates SE 75580 / AE 77386) and shader binds, through the guest
 * context's filter. Both are off by default.
 *
 * CS_ORG_RESIDUE_CENSUS=1 counts every engine draw and dispatch that still reaches D3D11, classed by caller, technique and target.
 * CS_ORG_ENGINE_FLUSH_LATCH=1 lets the engine's flush run on the CPU but drops the binds it makes. An engine draw that then reaches
 * D3D11 is skipped, unless code that declines a draw on purpose vouches for it with EngineDraws or VouchPass.
 * A latched flush owes its state, and a flush CS asks for applies it first. CS code that asks D3D11 what the engine bound at
 * any other time reads stale state.
 */
namespace RenderGraph::Engine::Flush
{
	bool CensusRequested();

	bool LatchRequested();

	/** @brief Whether the latch is in effect now, requested and installed with the guest context in place. */
	bool Latched();

	/**
	 * @brief Keeps CS's whole write of a dynamic constant buffer until a CS stage's window closes, since DXVK's stream takes no
	 * work inside a window. False means write it now.
	 */
	bool WriteBehind(ID3D11Buffer* a_buffer, const void* a_data, std::size_t a_size);

	/** @brief Installs the four detours and the filter. Must run after Hooks.cpp's SetDirtyStates detour and before any other DispatchCSShader detour. */
	void Install();

	/** @brief Binds a vertex shader for CS's BeginTechnique thunk now, or owes it while latched. */
	void BindVertexShader(ID3D11VertexShader* a_shader, void* a_caller);

	/** @brief Binds a pixel shader for CS's BeginTechnique thunk now, or owes it while latched. */
	void BindPixelShader(ID3D11PixelShader* a_shader, void* a_caller);

	/** @brief Why the engine draws on purpose while latched. Make one static per reason, owner first. At most 31, more share the last. */
	class Reason
	{
	public:
		explicit Reason(const char* a_name);
		std::uint8_t Id() const { return id; }

	private:
		std::uint8_t id;
	};

	/**
	 * @brief A scope in which this thread's engine draws are deliberate, so the latch applies owed state and lets them through.
	 * Keep it tight around the one call to the engine's original. Scopes nest and the innermost reason counts.
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
	 * @brief Traces CS binds DXVK drops. DXVK drops an SRV bind whose resource is bound as an output, and the latch leaves the last
	 * real flush's targets bound, so CS binds of those views can vanish. Census only.
	 */
	void InstallStateTrace(ID3D11DeviceContext* a_context);

	/** @brief Logs once per reader and register when a CS read of DXVK's PS bindings got null because the trace saw the bind lost. */
	void NoteRead(const char* a_reader, std::uint32_t a_register, ID3D11ShaderResourceView* a_view);

	/**
	 * @brief Vouches for a pass the batch walk will draw this frame or the next. The mark is checked against the pass's geometry,
	 * so a reused pass object is not vouched for. Any thread.
	 */
	void VouchPass(const RE::BSRenderPass* a_pass, const Reason& a_reason);
}
