#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace FrameAnchors
{
	struct ClientDesc;
}

/**
* @brief TruePBR's glints noise on the render graph, in graph mode.
*
* The 128x128 noise never changes, so a gated pass at EarlyPrepass fills it once after Setup. That anchor runs before every reader.
* The texture is persistent, exported for the engine at t20 and published for DCLF. TruePBR binds nothing at t20 until the fill ran.
* In graph mode a setup failure is fatal. Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGGlintsNoise : public RenderGraph::Port<ORGGlintsNoise>
{
public:
	static constexpr std::string_view kName = "TruePBR glints";

	/** @brief The noise texture's published id. */
	static constexpr std::string_view kNoiseId = "TruePBR::GlintsNoise";

	/** @brief At TruePBR::PostSetupResources. Outside graph mode it does nothing; in graph mode a failure is fatal. */
	void Setup();

	/** @brief A run with the fill was submitted, so later work sees it filled. */
	bool Filled() const;

	/** @brief t20. Null when inactive or not filled yet. */
	ID3D11ShaderResourceView* NoiseSRV() const;

	/** @brief Rebuilds the fill program and fills again. A failed compile keeps the last fill. */
	void ClearShaderCache();
	~ORGGlintsNoise();

	struct Impl;

private:
	friend class RenderGraph::Port<ORGGlintsNoise>;
	ORGGlintsNoise();

	/** @brief The fill's registration for the current program, made when a revision starts. A new one fills again. Fatal on failure. */
	bool MakeClient(const std::shared_ptr<Impl>& a_impl, FrameAnchors::ClientDesc& a_client);

	/** @brief Declares the client once, on the program serial a shader reload raises. A later call evaluates it again. */
	void DeclareClient();

	std::shared_ptr<Impl> impl;
	std::uint64_t programSerial = 0;  // the client's key: raised when a reload replaces the program
};
