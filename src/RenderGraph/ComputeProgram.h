#pragma once

#include <ORGModuleServices/Passes/ComputeKernel.h>
#include <rhi.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace org
{
	class LatchBlock;
	class RecordingContext;
}

namespace org::services
{
	class LivePipelineSlot;
}

/**
 * @brief A render-graph compute shader, its pipeline and its push-constant layout.
 *
 * Compiled from HLSL to SPIR-V at runtime and saved in the shared shader store. Shaders fetch everything else by heap index.
 * For hot reload the pipeline lives in a registry slot. A recompiled pipeline is a new version, which graphs built after it bind.
 * A program that fails to recompile keeps its pipeline. See docs/development/dclf-hot-reload.md.
 * It is OMS's ComputeKernel, so OMS's passes record through it.
 */
struct ComputeProgram final : org::services::ComputeKernel
{
	rhi::PipelineLayoutPtr layout;
	// Bind Pipeline(recording) at record time, never a handle kept across recordings.
	std::shared_ptr<org::services::LivePipelineSlot> slot;
	std::uint32_t constantWords = 0;
	// Three uints of group counts. The one owner for indirect dispatches.
	rhi::CommandSignaturePtr dispatchSignature;

	/**
	* @brief The pipeline a_recording binds: the slot's version at its graph's pipeline pin (RenderGraph/PipelineVersions.h). A shader
	* reload never changes it; the pin keeps it alive while the recording lives.
	*/
	rhi::PipelineHandle Pipeline(const org::RecordingContext& a_recording) const;
	/**
	* @brief The slot's newest pipeline, for a list recorded outside a graph and submitted at once. The registry keeps a version it
	* drops until every submission made before has completed.
	*/
	rhi::PipelineHandle CurrentPipeline() const;

	struct Desc
	{
		std::string source;  // relative to Data/Shaders, e.g. "DrawcallLimitFix/BuildDrawsCS.hlsl"
		std::wstring entry = L"main";
		std::uint32_t constantWords = 0;  // in 32-bit words
		std::vector<std::pair<std::wstring, std::wstring>> defines;
		// The defines the D3D11 build gets, so a dual-target shader compiles the same permutation on both paths.
		// Off for a shader with no D3D11 build that must not see CS's defines.
		bool commonDefines = true;
	};

	/** @brief Null and logged on failure. Blocks on a compile the cache does not have, so call it at setup, not per frame. */
	static std::shared_ptr<const ComputeProgram> Load(rhi::Device a_device, const Desc& a_desc);

	/** @brief Records nothing when the constants are longer than constantWords or a group count is zero. */
	void Dispatch(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, std::uint32_t a_x, std::uint32_t a_y, std::uint32_t a_z) const override;

	/**
	 * @brief Dispatches the group counts at a_offset in a_arguments, three 4-byte aligned uints.
	 * A zero count dispatches nothing, so a kept recording can skip work by its data alone.
	 */
	void DispatchIndirect(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, rhi::ResourceHandle a_arguments,
		std::uint64_t a_offset) const;

	/** @brief DispatchIndirect with the group counts in the latch region of the recording's run, which the commit writes each run. */
	void DispatchIndirect(org::RecordingContext& a_recording, std::span<const std::uint32_t> a_constants, const org::LatchBlock& a_latch,
		std::uint32_t a_offset) const override;

	/** @brief OMS's GenerateMips shader (org::services::kGenerateMipsShader), for 2D textures or for arrays and cubes. */
	static Desc GenerateMips(bool a_array);
};

