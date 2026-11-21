#pragma once

#include "RenderGraph/Port.h"

#include <d3d11.h>
#include <memory>
#include <string_view>

/**
* @brief Image Based Lighting's SH projection as compute passes at Prepass, in graph mode.
*
* Projects the environment cubemap and the engine's reflections cubemap into the same 3x1 SH textures the D3D11 path writes.
* The SH textures are History, since the reflection prepass reads last frame's. They are exported for D3D11 readers and published.
* A missing source clears its SH to zero, as a null view projects. The client is declared on the reflection cubemap and the DALC
* mode (Frame/Declare.h), so a change of either switches in with a frame revision. In graph mode any failure is fatal.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGIBL : public RenderGraph::Port<ORGIBL>
{
public:
	static constexpr std::string_view kName = "IBL";

	/** @brief The SH textures' published ids. */
	static constexpr std::string_view kEnvId = "IBL::EnvIBL";
	static constexpr std::string_view kSkyId = "IBL::SkyIBL";

	/** @brief At SetupResources. @return Whether the graph path runs: false outside graph mode. In graph mode a failure is fatal. */
	bool Setup();

	/** @brief Before the Prepass anchor. The adopted revision's client projects the SH. @return False outside graph mode, where the D3D11 dispatches run. */
	bool Prepass() const { return IsActive(); }

	/** @brief Null when inactive. */
	ID3D11ShaderResourceView* EnvSRV() const;
	ID3D11ShaderResourceView* SkySRV() const;
	~ORGIBL();

private:
	friend class RenderGraph::Port<ORGIBL>;
	ORGIBL();

	struct Impl;
	std::unique_ptr<Impl> impl;
};
