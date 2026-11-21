#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <array>
#include <cstdint>
#include <string_view>

struct PhysicalSky;

/**
* @brief The cloud renderer's inputs that Physical Sky's managers make: file textures and generated maps
* (docs/development/plans/physical-sky-port.md, 6a).
*
* The generators run in cs.physical-sky.cloud-maps at Prepass, ahead of cs.physical-sky.clouds, which reads these.
* Each field is the texture the D3D11 code binds at the named register this frame, as a graph handle: a twin of a generated
* map, or an immutable ImportTexture of a file texture. Null means D3D11 binds null there.
* A change of any field registers the cloud renderer again, so compare whole structs. Render thread.
*/
namespace ORGCloudInputs
{
	/** @brief The published id of the cloud maps client's generators, for ordering and logs. */
	inline constexpr std::string_view kClientId = "cs.physical-sky.cloud-maps";

	struct Inputs
	{
		FrameAnchors::Texture shapeNoise;     // t4 TexCloudShapeNoise, 3D: generated or the DDS
		FrameAnchors::Texture profileLut;     // t11 TexCloudProfileLUT, the DDS
		FrameAnchors::Texture adjustmentLut;  // t12 TexCloudAdjustmentLUT: generated or the DDS
		FrameAnchors::Texture ndfHeight;      // t5 TexCloudHeight (NdfManager::GetNdf), also the boundary raster's VS t0
		FrameAnchors::Texture ndfModeling;    // t6 TexCloudModeling, also the boundary raster's VS t1
		// t101..t103: NdfManager::GetNoiseInputs()[1..3], the middle layer's warp and cell noise.
		std::array<FrameAnchors::Texture, 3> middleNoise;
		FrameAnchors::Texture middleWeather;   // t99 TexMiddleWeather (MidCloudMapManager::Weather), null with the layer off
		FrameAnchors::Texture cirrusWeather;   // t9 TexCirrusWeather, null without cirrus
		FrameAnchors::Texture cirrusPatterns;  // t10 TexCirrusPatterns
		FrameAnchors::Texture localHeight;     // t32 TexLocalNdfHeight, Texture2DArray
		FrameAnchors::Texture localModeling;   // t33 TexLocalNdfModeling, Texture2DArray
		FrameAnchors::Texture localMaximum;    // t34 TexLocalNdfMaximum, Texture2DArray
		FrameAnchors::Texture stormHeight;     // t55 TexStormHeight, Texture2DArray
		FrameAnchors::Texture stormModeling;   // t56 TexStormModeling, Texture2DArray
		// TexVoxelModeling0..7 (t60..t67) and TexVoxelDistance0..7 (t68..t75), Texture2DArray: VoxelCloudManager's bindings.
		std::array<FrameAnchors::Texture, 8> voxelModeling;
		std::array<FrameAnchors::Texture, 8> voxelDistance;
		FrameAnchors::Texture voxelLighting;   // t85 TexVoxelLighting / u0 RWVoxelLighting, Texture2DArray, written by the clouds client
		FrameAnchors::Texture voxelNoise;      // t76 NoiseVoxel, 3D, with the voxel noise setting

		bool operator==(const Inputs&) const = default;
	};

	/**
	* @brief At Physical Sky's Prepass, after the managers updated. The handles of this frame's inputs.
	* @return False, logged once, when an input the frame needs is not on the graph: the clouds then render nothing this frame.
	*/
	bool Gather(const PhysicalSky& a_feature, Inputs& a_out);
}

