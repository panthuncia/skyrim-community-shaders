#pragma once

#include "Utils/Setting.h"

#include <array>
#include <cstdint>
#include <d3d11.h>
#include <memory>
#include <string_view>

class Texture2D;
class Texture3D;
struct PhysicalSky;

/**
* @brief Physical Sky on the render graph (docs/development/plans/physical-sky-port.md).
*
* Three clients, declared at setup (Frame/Declare.h), so they switch in with frame revisions:
* - cs.physical-sky.luts at EarlyPrepass: the disk phase, transmittance, multiscatter, sky-view and aerial perspective LUTs.
*   It has no dependencies and is registered from the first revision on.
* - cs.physical-sky at Prepass: the aerial perspective shadow (ShadowAccum) and the sky tail (LutGen SKY_TAIL). Declared on
*   the feature's toggle; its key adds the inputs the last Prepass found.
* - cs.physical-sky.composite at DeferredComposite, after cs.deferred: the aerial perspective and cloud composite on kMAIN,
*   which the graph's deferred composite leaves out (ApplyAerialPerspectiveCS.hlsl). Declared on the toggle and the clouds.
* The LUTs (no dependencies) and the AP shadow (on the display size) are declared graph textures exported to D3D11, so the
* feature's D3D11 binds read the same memory, and published, so DCLF and other graph passes read them after the writes.
* Run gates hold the dispatches: a frame with the feature off dispatches nothing, and the disk phase LUT only on a change.
* Render thread throughout. Free of ORG/Vulkan includes.
*/
class ORGPhysicalSky
{
public:
	/** @brief The published ids. */
	static constexpr std::string_view kDiskPhaseLutId = "PhysicalSky::DiskPhaseLut";
	static constexpr std::string_view kTransmittanceLutId = "PhysicalSky::TransmittanceLut";
	static constexpr std::string_view kMultiscatterLutId = "PhysicalSky::MultiscatterLut";
	static constexpr std::string_view kSkyViewLutId = "PhysicalSky::SkyViewLut";
	static constexpr std::string_view kApLutId = "PhysicalSky::ApLut";
	static constexpr std::string_view kApShadowId = "PhysicalSky::ApShadow";
	static constexpr std::string_view kSkyTailId = "PhysicalSky::SkyTail";

	/** @brief The LUTs, in LutGen's LUTGEN order, then the disk phase LUT. */
	enum class Lut : std::uint32_t
	{
		Transmittance,
		Multiscatter,
		SkyView,
		AerialPerspective,
		DiskPhase,
		Count
	};

	static bool Requested();

	/**
	* @brief Render thread. A toggle's value for this frame: the adopted revision's on the render graph, so the frame and the
	* clients declared on it switch at one Begin. The setting's own value in plain D3D11 mode. PhysicalSky::FollowFrameToggles
	* keeps a copy for other threads.
	*/
	static bool FrameValue(const CS::Setting<bool>& a_setting);

	/** @brief Physical Sky's PostPostLoad, before the render graph initializes. */
	static void Reserve();

	static ORGPhysicalSky& Get();

	/**
	* @brief At SetupResources, before Setup. A persistent graph twin exported to D3D11 (SRV and UAV), published by its id.
	* It lives as long as the game. @return Null means make a D3D11 texture.
	*/
	static Texture2D* MakeLut2D(Lut a_lut, const D3D11_TEXTURE2D_DESC& a_desc);
	static Texture3D* MakeLut3D(Lut a_lut, const D3D11_TEXTURE3D_DESC& a_desc);

	/** @brief The AP shadow's twin, as MakeLut2D makes a LUT's. A screen-sized R8 texture. @return Null means make a D3D11 texture. */
	static Texture2D* MakeApShadow(const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief The sky tail's twin (LutGen SKY_TAIL), the sky-view LUT's shape. @return Null means make a D3D11 texture. */
	static Texture2D* MakeSkyTail(const D3D11_TEXTURE2D_DESC& a_desc);

	/** @brief At SetupResources, after the LUTs exist. @return False keeps the LUTs on D3D11. */
	bool Setup(const PhysicalSky& a_feature);

	bool IsActive() const;

	/**
	* @brief At the feature's first hook of each frame (ReflectionsPrepass, EarlyPrepass or Prepass), after the frame's Begin.
	* The D3D11 wrappers follow the bound versions of the declared textures, and an adoption that switched the sky or its clouds
	* off since Reset clears the flags the shaders read. Once a frame.
	*/
	void BeginFrame(PhysicalSky& a_feature);

	/** @brief The samplers the LUT passes made from the feature's, in this order. */
	enum class Sampler : std::uint32_t
	{
		Transmittance,  // sampTr
		SkyView,        // sampSv
		Noise,          // sampNoise
	};
	static constexpr std::uint32_t kNoSampler = ~0u;

	/**
	* @brief Render thread. A sampler's heap index for graph draws, or kNoSampler while inactive.
	* a_owner, when given, gets what keeps the descriptor alive, for a run that latches the index.
	*/
	std::uint32_t SamplerIndex(Sampler a_sampler, std::shared_ptr<const void>* a_owner = nullptr) const;

	/**
	* @brief At the feature's EarlyPrepass with the feature on. Queues the four LUTs, and the disk phase LUT when its constants changed.
	* A run of the client takes them. @return False without the render graph: the D3D11 dispatches run.
	*/
	bool GenerateLuts(PhysicalSky& a_feature);

	/**
	* @brief At the feature's Prepass in place of AccumShadow. Stages the AP shadow for the Prepass anchor, and notes what it
	* reads: a change registers the client again with the next revision. @return False without the render graph.
	*/
	bool AccumShadow(const PhysicalSky& a_feature);

	/**
	* @brief At the feature's Prepass with the feature on. Stages the AP composite, which runs right after the graph's deferred
	* composite on frames that one composites. The D3D11 composite applies it itself on the other frames. Notes what it reads,
	* as AccumShadow does.
	*/
	void StageComposite(const PhysicalSky& a_feature);

	/**
	* @brief At the feature's Prepass in place of GenerateSkyTail's dispatch, when ViewAtmosphere's composite asks for the tail.
	* @return False without the render graph: the D3D11 dispatch runs.
	*/
	bool GenerateSkyTail(const PhysicalSky& a_feature);

	~ORGPhysicalSky();

	struct Impl;

private:
	ORGPhysicalSky() = default;

	std::shared_ptr<Impl> impl;
};
