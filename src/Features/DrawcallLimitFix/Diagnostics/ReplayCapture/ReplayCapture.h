#pragma once

// The DCLF pass capture (CS_DCLF_CAPTURE=<dir>, D3D12 native only). With the switch set, every recording of the Z-prepass and main
// opaque passes copies what the pass reads per epoch (frame constants, sequences, counts, depth, colour targets) on its own command
// list, with a marker. The cs-test command dclf:capture arms the dump: at a frame's end the render thread waits for the GPU, takes the
// recordings whose markers say they ran, walks every descriptor slot they can reach and writes a dump that pascal-fetch-bench replays
// (docs/development/plans/dclf-replay.md).

#include <rhi.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DCLF::ReplayCapture
{
	/** @brief Whether CS_DCLF_CAPTURE is set on D3D12. Cheap after the first call. Any thread. */
	bool Enabled();

	/** @brief Arms a capture of the next recorded passes. Any thread. False with a_error set when the capture is off. */
	bool Arm(std::string& a_error);

	/** @brief One plain bucket draw of a captured pass, as RecordBucketDraw records it. */
	struct Bucket
	{
		std::uint32_t set = 0;           // the set index, ~0u for a Z-prepass call (its bucket number is in countWord)
		std::uint64_t first = 0;         // the first sequence
		std::uint32_t capacity = 0;
		std::uint32_t countWord = 0;     // the count's word in the count buffer
		rhi::PipelineHandle pipeline{};
		std::array<std::uint32_t, 24> drawWords{};
	};

	/** @brief What a captured pass draws with. The resources are those the pass resolved for this run. */
	struct Pass
	{
		const char* name = "";
		rhi::Device device;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		std::array<std::uint32_t, 5> frameWords{};  // kFramePushWords (DrawPipelines.h)
		std::uint64_t recordOffset = 0;         // the frame record's offset in the frame constants
		std::uint64_t frameConstantsBytes = 0;  // the frame constants' size
		rhi::Resource sequences, counts, indexPool, depth;
		std::array<rhi::Resource, 8> colours{};  // the main opaque pass's targets, copied at its start
		std::uint32_t colourCount = 0;
		std::uint32_t width = 0, height = 0;
		float minDepth = 0.0f, maxDepth = 1.0f;
		std::uint64_t argumentsOffset = 0;  // PlainDrawArgumentsOffset
		bool instanceWord = false;          // D3D12InstanceWord: the signature writes draw word 23
		std::vector<Bucket> buckets;
	};

	/** @brief Whether a pass records its copies: always with the capture on, since a kept recording replays them. Recording thread. */
	bool Wanted(const char* a_pass);

	/**
	 * @brief Records a pass's copies at its start, before its pass begins, and keeps its description for the frame's end.
	 * a_commands is the pass's list, in the pass's declared layouts (depth in DepthReadWrite). Recording thread.
	 */
	void RecordPass(rhi::CommandList& a_commands, Pass a_pass);

	/** @brief Writes the dump once the captured passes ran: waits for the GPU, reads back and writes. Render thread, after Present. */
	void OnFrameEnd();

	/** @brief Which pulled stages a pipeline was built from, for the replay's offline compile. Any thread. */
	struct PipelineNote
	{
		std::string key;  // "vs 0000000F ps 13000019 flags 00000000"
		std::string vertexStage, pixelStage;  // StageKey of each, empty without that stage
	};
	void NotePipeline(rhi::PipelineHandle a_pipeline, PipelineNote a_note);

	/**
	 * @brief The key of a stage request, as NoteStageLine and PipelineNote name it. a_literals (ShaderStageSpec::literals) names the
	 * runtime descriptors a pulled stage was compiled for, and a_depthTex (ShaderStageSpec::depthTex) its Z-prepass texture build.
	 */
	std::string StageKey(bool a_pixel, bool a_depth, bool a_pulled, bool a_family, std::uint32_t a_descriptor, std::uint8_t a_literals = 0,
		std::uint32_t a_runtimeVertex = 0, std::uint32_t a_runtimePixel = 0, bool a_depthTex = false);

	/** @brief Keeps a CS_DCLF_STAGE_DUMP stage line (or the options line, with an empty key). Any thread. */
	void NoteStageLine(const std::string& a_key, std::string a_line);
}
