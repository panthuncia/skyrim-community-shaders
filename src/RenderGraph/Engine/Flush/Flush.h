#pragma once

#include <d3d11.h>

/**
 * @brief The engine draw census, and the shader binds of CS's BeginTechnique thunk.
 *
 * In graph mode the engine's context (guest or native handle) keeps every bind and drops every draw and dispatch. The census
 * names each dropped engine draw once per caller chain; with CS_ORG_ENGINE_DRAW_TAP=1 it is an EngineDraw tap error. In
 * development builds a game draw outside the engine's draw functions is a tap error; CS_ORG_ENGINE_DRAW_TAP=0 turns that off.
 * CS code that reads what the engine bound asks the engine's context (Tripwire::EngineContext), not D3D11's.
 */
namespace RenderGraph::Engine::Flush
{
	/** @brief Whether the engine's draws are dropped now: graph mode, with the engine's context handle in place. */
	bool EngineDrawsDropped();

	/** @brief Installs the DispatchCSShader detour and the context observer. Must run before any other DispatchCSShader detour. */
	void Install();

	/**
	 * @brief Binds a vertex shader for CS's BeginTechnique thunk. In graph mode it goes to the engine's context handle, which keeps it
	 * and draws nothing; in plain D3D11 mode to the context. a_shader is the engine's object, a handle or not.
	 */
	void BindVertexShader(ID3D11VertexShader* a_shader);

	/** @brief BindVertexShader for the pixel shader. */
	void BindPixelShader(ID3D11PixelShader* a_shader);
}
