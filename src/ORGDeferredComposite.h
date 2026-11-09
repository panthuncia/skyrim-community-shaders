#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11ShaderResourceView;

/**
 * @brief The deferred composite (DeferredCompositeCS) as a render-graph compute pass, behind CS_ORG_COMPOSITE=1 (default 0: the
 * D3D11 dispatch in Deferred::DeferredPasses, unchanged).
 *
 * It runs on the DeferredComposite frame anchor, where the D3D11 composite runs: after the D3D11 SSGI, SSS and cubemap update. The
 * GBuffer, the main target, the normals, the motion vectors and the final depth copy are engine targets imported once
 * (FrameAnchors::EngineTarget / EngineDepth) and tracked by the graph. What D3D11 features produce (SSGI's outputs, the cubemaps, the
 * skylighting probes, the IBL SH) is read in place through descriptors over their DXVK images (DCLF::GpuTextures::ResolveBinding),
 * read only. The shared constant buffers come from SharedConstants. The shader is the same DeferredCompositeCS.hlsl, built by DXC
 * with the defines Util::CompileShader would give it.
 *
 * When the ORG path cannot run a frame (an input it cannot import, a failed graph), Deferred runs the D3D11 composite instead. A
 * failed graph turns the ORG path off for the session.
 *
 * Render thread. Header deliberately free of ORG/Vulkan includes (see RenderGraphRuntime.h).
 */
class ORGDeferredComposite
{
public:
	/** @brief What one frame's composite reads, as Deferred::DeferredPasses gathers it for the D3D11 dispatch. */
	struct Frame
	{
		bool interior = false;
		std::uint32_t main = 0;     // RE::RENDER_TARGET of the main target (forwardRenderTargets[0])
		std::uint32_t normals = 0;  // and of the normals (forwardRenderTargets[2])
		std::uint32_t groupsX = 0, groupsY = 0;
		// The D3D11 composite's t0-t15 (null where it binds nothing); t0-t5 and t9 are the engine targets above, imported.
		std::array<ID3D11ShaderResourceView*, 16> views{};
	};

	static ORGDeferredComposite& Get();

	/** @brief CS_ORG_COMPOSITE=1. */
	static bool Requested();

	/** @brief Before the render graph initializes: reserves the DeferredComposite anchor when requested. */
	static void Reserve();

	/** @brief After the render graph initializes (Deferred::SetupResources): the programs, the sampler and the anchor client. */
	void Setup();

	/** @brief Before the anchor runs: the frame the composite would read. Nothing when the ORG path is off. */
	void SetFrame(const Frame& a_frame);

	/** @brief After the anchor ran: whether the ORG composite wrote this frame (else Deferred runs the D3D11 one). */
	bool Composited() const;

	/** @brief The shader defines changed (Deferred::ClearShaderCache): the programs are built again at the next frame. */
	void ClearShaderCache();

	~ORGDeferredComposite();

private:
	ORGDeferredComposite() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
