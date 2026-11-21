#pragma once

// BasicRHI view of DrawPipelines for the graph pass. Include after rhi_interop_vulkan.h.

#include <array>
#include <memory>
#include <vector>
#include <rhi.h>

#include "Features/DrawcallLimitFix/Api/DeferredRanges.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"

namespace DCLF
{
	/** @brief What an ExecuteIndirect of DCLF's draws binds. Invalid until the first pipeline is in the set. */
	struct IndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		/**
		 * @brief A state class's execution set and signature per variant. Each device-generated call draws one class's sub-range,
		 * naming pipelines by their index in that class.
		 */
		struct Class
		{
			std::array<rhi::IndirectPipelineSetHandle, kVariantCount> sets{};
			std::array<rhi::CommandSignatureHandle, kVariantCount> signatures{};
			std::uint32_t pipelines = 0;
		};
		std::vector<Class> classes;  // in creation order, which is the calls' order within a range
		// By set index: class << kSetClassShift | index in the class. BuildDraws routes colour draws by it.
		std::vector<std::uint32_t> classOf;
		// Z-prepass groups by set index, each group's pulled depth pipeline, and the plain draw layout and signature.
		std::vector<std::uint32_t> zGroups;
		std::vector<rhi::PipelineHandle> zPipelines;
		// By set index, a forward key's plain colour pipeline. Null when the pulled stages would not build.
		std::vector<rhi::PipelineHandle> plainColour;
		// By set index, the colour pipeline for the CPU pipeline's pushed colour draws.
		std::vector<rhi::PipelineHandle> colour;
		std::vector<rhi::PipelineHandle> depth;
		// By set index, the plain depth pipeline for the decal depth pass.
		std::vector<rhi::PipelineHandle> plainDepth;
		// Per deferred range and by set index, a deferred family key's plain pipeline there. Null for every other key.
		std::array<std::vector<rhi::PipelineHandle>, Api::kDeferredRanges> deferred;
		rhi::PipelineLayoutHandle zLayout{};
		rhi::CommandSignatureHandle zDrawSignature{};
		// Null while it waits or once it failed.
		rhi::PipelineHandle refraction{};
		DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
		rhi::PipelineLayoutHandle refractionLayout{};
		rhi::CommandSignatureHandle refractionDrawSignature{};
		// While any copy of this state lives, the version's sets are not written. Hold it for as long as the handles are recorded with.
		std::shared_ptr<const void> version;
		bool valid = false;
	};

	/**
	 * @brief Any thread. The main set version of a_catalog. The main targets' formats are fixed at setup, so a published set
	 * version is always for them.
	 */
	IndirectState GetIndirectState(const PipelineCatalog& a_catalog);
	/** @brief Whether GetIndirectState would be valid, without building it. */
	bool IndirectStateValid(const PipelineCatalog& a_catalog);
	/** @brief Whether a set index's Z-prepass group has a depth pipeline, without building the state. */
	bool ZGroupPipelineReady(const PipelineCatalog& a_catalog, std::uint32_t a_setIndex);

	/** @brief The shadow views' pipelines from a catalog's shadow set version. */
	struct ShadowIndirectState
	{
		rhi::PipelineLayoutHandle layout{};
		// By index. Discarding pipelines draw after the other class.
		std::vector<rhi::PipelineHandle> pipelines;
		std::vector<std::uint64_t> vertexLayouts;
		std::vector<std::uint8_t> discards;
		rhi::CommandSignatureHandle drawSignature{};
		std::shared_ptr<const void> version;
		bool valid = false;
	};

	/** @brief Any thread. The shadow set version of a_catalog. Invalid once the format differs from the set's. */
	ShadowIndirectState GetShadowIndirectState(const PipelineCatalog& a_catalog, DXGI_FORMAT a_shadowFormat);

	/** @brief Render thread. The frame's indirect state, the only one the frame's epochs use. */
	IndirectState FrameIndirectState();
	/** @brief Render thread. Whether FrameIndirectState would be valid, without building it. */
	bool FrameIndirectStateValid();
	ShadowIndirectState FrameShadowIndirectState();

	/**
	 * @brief A forward view's pipeline for the given targets, or null while it waits or after it failed.
	 * Every forward build is kept for the process, so a handle stays valid.
	 */
	rhi::PipelineHandle ForwardPipelineOf(const PipelineCatalog* a_catalog, const ForwardPipelineKey& a_key, const ForwardTargets& a_targets);

	/** @brief A draw family's depth and colour pipelines and its draws' signature. */
	struct FamilyPipelines
	{
		rhi::PipelineHandle depth{}, colour{}, colour2{};
		// 16 bytes, or 20 with a first instance word before them.
		rhi::CommandSignatureHandle drawSignature{};
		// 4 for InstanceWordDrawInstanced past the word (Vulkan, and D3D12 with Shader Model 6.8), else 0.
		std::uint64_t argumentsOffset = 0;
	};

	/** @brief a_family's pipelines in a_catalog. False while waiting or after failure. Any thread. */
	bool CatalogFamilyPipelines(const PipelineCatalog* a_catalog, Api::DrawFamily a_family, FamilyPipelines& a_out);
}
