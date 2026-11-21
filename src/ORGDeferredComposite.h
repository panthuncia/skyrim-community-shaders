#pragma once

#include "RenderGraph/Port.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

struct ID3D11ShaderResourceView;

/**
* @brief The deferred composite as a compute pass at the DeferredComposite anchor, in graph mode.
*
* Engine targets are imported once. Other features' outputs are read by their published ids, so the read is ordered after
* the writes. A slot D3D11 binds nothing at, or whose id is not published, reads the null descriptor.
* Outside graph mode Deferred runs the D3D11 composite. In graph mode a frame the graph cannot composite is a tap error.
* Render thread. Free of ORG/Vulkan includes.
*/
class ORGDeferredComposite : public RenderGraph::Port<ORGDeferredComposite>
{
public:
	static constexpr std::string_view kName = "Deferred composite";

	/** @brief As Deferred::DeferredPasses gathers it for the D3D11 dispatch. */
	struct Frame
	{
		bool interior = false;
		std::uint32_t main = 0;     // forwardRenderTargets[0]
		std::uint32_t normals = 0;  // forwardRenderTargets[2]
		std::uint32_t groupsX = 0, groupsY = 0;
		// The D3D11 composite's t0-t15, null where it binds nothing. t0-t5 and t9 are the imported engine targets.
		std::array<ID3D11ShaderResourceView*, 16> views{};
		// Its t21 and t22, Skylighting's roof coverage and zenith capture.
		std::array<ID3D11ShaderResourceView*, 2> skylightingCoverage{};
	};

	/** @brief At Deferred::SetupResources. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup();

	/** @brief Before the anchor runs. Nothing when the ORG path is off. */
	void SetFrame(const Frame& a_frame);

	/** @brief After the anchor ran. Outside graph mode always false, and Deferred runs the D3D11 composite. */
	bool Composited() const;

	/**
	* @brief After the anchor ran. The client is not registered yet, or the schedule does not hold the composite yet. Nothing
	* composites this frame, and Deferred must not run D3D11's either.
	*/
	bool Lagged() const;

	/** @brief After the anchor ran. Why the graph could not composite this frame. Empty when it composited or lagged. In graph mode it is a tap error. */
	std::string_view Refusal() const;

	/** @brief The frame the graph composite takes, as SetFrame gave it. */
	struct GraphFrame
	{
		std::uint32_t main = 0;
		bool interior = false;
	};

	/**
	* @brief After SetFrame, before the anchor's commit. Set when the graph composites this frame, so a pass after it reads its output.
	* The D3D11 composite runs on the other frames. Render thread.
	*/
	std::optional<GraphFrame> GraphComposite() const;
	~ORGDeferredComposite();

private:
	friend class RenderGraph::Port<ORGDeferredComposite>;
	ORGDeferredComposite();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
