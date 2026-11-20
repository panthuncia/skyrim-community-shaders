#pragma once

#include "Features/PhysicalSky.h"

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <functional>
#include <memory>
#include <string_view>

class Texture2D;
class Texture3D;

namespace ORGCloudInputs
{
	struct Inputs;
}

/**
* @brief Physical Sky's volumetric clouds on the render graph (docs/development/plans/physical-sky-port.md, 6 and 6a).
*
* One client, cs.physical-sky.clouds, at the Prepass anchor before cs.physical-sky. It holds both of the D3D11 passes in
* their order: the shadow volume and its mean, then the main view and cubemap (ambient, bounds, the boundary raster, the
* trace, voxel and upper views, reprojection, filter, lightning and the cube). Every dispatch D3D11 may issue is a gated
* step; the commit opens the ones the frame takes. Histories are fixed targets that a copy at the end of the frame
* refreshes, so every reader reads one resource under one id. The lightning query reads its candidates back through a
* RunReadback. The cloud textures are graph twins exported to D3D11, so the feature's binds for engine draws name them.
* The client is declared on the sky's and the clouds' toggles and the display size (Frame/Declare.h); its key adds the inputs
* the last Stage found. The full-resolution targets are declared at setup on the same variables, one texel while the clouds
* are off, so their ids are published from the start (the existence rule).
* In the graph modes the D3D11 cloud dispatches never run. Render thread throughout.
*/
class ORGVolumetricClouds
{
public:
	/** @brief The published ids of the outputs other passes read. */
	static constexpr std::string_view kShadowVolumeId = "PhysicalSky::ShadowVolume";
	static constexpr std::string_view kCloudShadowMeanId = "PhysicalSky::CloudShadowMean";
	static constexpr std::string_view kVolTrId = "PhysicalSky::VolTr";
	static constexpr std::string_view kVolLumId = "PhysicalSky::VolLum";
	static constexpr std::string_view kVolAuxId = "PhysicalSky::VolAux";
	// The reprojected targets before the filter, which the deferred composite's aerial perspective reads (D3D11 t18, t19).
	static constexpr std::string_view kVolTrCurrentId = "PhysicalSky::CloudTr";
	static constexpr std::string_view kVolLumCurrentId = "PhysicalSky::CloudLum";
	static constexpr std::string_view kVolCubeTrId = "PhysicalSky::VolCubeTr";
	static constexpr std::string_view kVolCubeLumId = "PhysicalSky::VolCubeLum";
	static constexpr std::string_view kVolCubeDepthId = "PhysicalSky::VolCubeDepth";
	static constexpr std::string_view kLightningChannelsId = "PhysicalSky::LightningChannels";
	static constexpr std::string_view kLightningReflectionId = "PhysicalSky::LightningReflection";

	/** @brief Whether the clouds are the render graph's in this session: the graph modes, guest and native. */
	static bool Requested();

	static ORGVolumetricClouds& Get();

	/**
	* @brief Whether a run of the client wrote the bound versions of the outputs: a run clears new versions to neutral first, so
	* readers may read the published ids from then on. Before that nothing has written them, and a graph must not read them.
	* False again from the frame a new version is bound, and while the clouds are off. Render thread.
	*/
	static bool OutputsWritten();

	/**
	* @brief In place of a cloud texture's D3D11 creation (SetupVolumetricResources, EnsureVolumetricScreenResources).
	* A graph twin with exported SRV and UAV (and RTV when the description binds one), published under a_publishedId when one
	* is given. Its wrapper names the export, so the D3D11 code keeps its members. Null means make a D3D11 texture.
	*/
	static Texture2D* MakeTexture2D(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, std::string_view a_publishedId = {});
	static Texture3D* MakeTexture3D(const D3D11_TEXTURE3D_DESC& a_desc, const char* a_name, std::string_view a_publishedId = {});

	/**
	* @brief In place of a full-resolution target's creation (EnsureVolumetricScreenResources), at setup. A twin declared on the
	* display size and the sky's and the clouds' toggles: the display size while both are on, one texel otherwise. a_desc's size
	* stands in before the display size is known. Null means make a D3D11 texture.
	*/
	static Texture2D* MakeScreenTarget(const D3D11_TEXTURE2D_DESC& a_desc, const char* a_name, std::string_view a_publishedId = {});

	/**
	* @brief ORGPhysicalSky::BeginFrame's. Points the screen targets' D3D11 wrappers at their bound versions. A new version asks
	* for the neutral clears and drops the history, as new D3D11 targets do.
	*/
	void FollowVersions(PhysicalSky& a_feature);

	/** @brief The cloud inputs' source (ORGCloudInputs::Gather). Without one the clouds render nothing, logged once. */
	using InputSource = std::function<bool(const PhysicalSky&, ORGCloudInputs::Inputs&)>;
	void SetInputSource(InputSource a_source);

	/** @brief At SetupVolumetricResources' end, with the twins made. Loads the programs (blocking) and makes the gate and latch. */
	void Setup(const PhysicalSky& a_feature);

	/** @brief Everything RenderVolumetricClouds computed before its dispatches, for Stage. */
	struct Frame
	{
		const PhysicalSky::VolumetricCloudCB* cloud = nullptr;
		float2 textureDim, frameDim;
		std::uint32_t renderW = 0, renderH = 0, lowW = 0, lowH = 0;
		float3 camera;
		float2 boundaryOrigin, boundarySpacing, noiseWindOffset;
		float4 ambientPhase;  // the low cloud phase's AmbientPhase(msAlbedo)
		bool renderCirrus = false;
		bool voxelActive = false;
	};

	/**
	* @brief In place of both of RenderVolumetricClouds' D3D11 passes, at the shadow volume pass in Prepass.
	* Stages the frame's steps for the client's commit and updates the feature's cloud state as the D3D11 passes do.
	* Notes the inputs the client's key reads. Until the adopted revision holds a registration the frame renders nothing, and the
	* targets hold their neutral clears.
	*/
	void Stage(PhysicalSky& a_feature, const Frame& a_frame);

	/** @brief In place of ClearVolumetricToNeutral's D3D11 clears: the client's next run clears the targets first. */
	void RequestNeutral();

	/** @brief Inside Stage (SkyLightning::QuerySource): adds the lightning query to the frame. False when the frame has none. */
	bool QueueLightningQuery();

	/** @brief SkyLightning::ReadSource's graph path: the candidates of a query since the last one queued. False until they arrive. */
	bool TakeLightningCandidates(std::array<float4, SkyLightning::kCandidates>& a_out);

	~ORGVolumetricClouds();

	struct Impl;

private:
	ORGVolumetricClouds() = default;

	std::shared_ptr<Impl> impl;
	InputSource inputs;
};
