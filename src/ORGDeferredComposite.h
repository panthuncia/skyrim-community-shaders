#pragma once

#include <array>
#include <cstdint>
#include <memory>

struct ID3D11ShaderResourceView;

/**
* @brief The deferred composite as a compute pass at the DeferredComposite anchor, with CS_ORG_COMPOSITE=1.
*
* Engine targets are imported once. Other features' outputs are read by published id when their producer runs on the graph,
* so the read is ordered after the writes. A producer on D3D11 is imported in guest mode and bound null in native mode.
* When a frame cannot run on the graph, Deferred runs the D3D11 composite. Render thread. Free of ORG/Vulkan includes.
*/
class ORGDeferredComposite
{
public:
	/** @brief As Deferred::DeferredPasses gathers it for the D3D11 dispatch. */
	struct Frame
	{
		bool interior = false;
		std::uint32_t main = 0;     // forwardRenderTargets[0]
		std::uint32_t normals = 0;  // forwardRenderTargets[2]
		std::uint32_t groupsX = 0, groupsY = 0;
		// The D3D11 composite's t0-t15, null where it binds nothing. t0-t5 and t9 are the imported engine targets.
		std::array<ID3D11ShaderResourceView*, 16> views{};
	};

	static ORGDeferredComposite& Get();

	static bool Requested();

	/** @brief Call before the render graph initializes. */
	static void Reserve();

	/** @brief At Deferred::SetupResources. */
	void Setup();

	/** @brief Before the anchor runs. Nothing when the ORG path is off. */
	void SetFrame(const Frame& a_frame);

	/** @brief After the anchor ran. False means Deferred runs the D3D11 composite. */
	bool Composited() const;

	/** @brief After the anchor ran. The schedule does not hold the composite yet, so nothing composites and Deferred must not run D3D11's either. */
	bool Lagged() const;

	/** @brief Programs rebuild at the next frame. */
	void ClearShaderCache();

	~ORGDeferredComposite();

private:
	ORGDeferredComposite() = default;

	struct Impl;
	std::unique_ptr<Impl> impl;
};
