#pragma once

#include <cstdint>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Runtime/KeptState.h"
#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"

namespace DCLF
{
	/**
	 * @brief The per-frame engine values the frame evaluates, kept out of the tables because the frame owns these and the coordinator
	 * owns the tables. Indexed by the tables' slots. Versions come from the frame's own counter so they never collide with the tables'.
	 * The render thread writes them, and the builds kicked after its writes read them.
	 */
	struct FrameTables
	{
		// Each pipeline's PerGeometry block, evaluated in full once, with its frame globals refreshed every frame.
		std::vector<GeometryConstants> geometryConstants;
		std::vector<std::uint8_t> geometryConstantsValid;
		std::vector<std::uint32_t> pipelineConstantsVersion;
		std::vector<PipelineKey> pipelineKeys;
		std::vector<std::uint32_t> pipelineBindings;  // the slot's SceneTables::pipelineBindingVersion its block is for
		// The technique rows and material records are the coordinator's, read from the installed publication.

		std::uint32_t tablesGeneration = ~0u;
		// An unmoved stamp in the same tables generation means the same family, so nothing needs comparing.
		std::uint64_t pipelinesStamp = ~0ull;
		std::uint32_t versionCounter = 0;

		std::uint32_t NextVersion() { return ++versionCounter; }

		/** @brief A slot whose key or binding moved holds no block. Does nothing unless the stamp or the generation moved. */
		void SyncPipelines(const std::vector<PipelineKey>& a_pipelines, const std::vector<std::uint32_t>& a_bindings, std::uint32_t a_tablesGeneration, std::uint64_t a_stamp)
		{
			if (a_tablesGeneration == tablesGeneration && a_stamp == pipelinesStamp)
				return;
			pipelinesStamp = a_stamp;
			if (a_tablesGeneration != tablesGeneration) {
				tablesGeneration = a_tablesGeneration;
				geometryConstants.clear();
				geometryConstantsValid.clear();
				pipelineConstantsVersion.clear();
				pipelineKeys.clear();
				pipelineBindings.clear();
			}
			const std::size_t count = a_pipelines.size();
			geometryConstants.resize(count);
			geometryConstantsValid.resize(count, 0);
			pipelineConstantsVersion.resize(count, 0);
			pipelineKeys.resize(count);
			pipelineBindings.resize(count, 0);
			for (std::size_t p = 0; p < count; ++p) {
				const std::uint32_t binding = p < a_bindings.size() ? a_bindings[p] : 0u;
				if (!(pipelineKeys[p] == a_pipelines[p]) || pipelineBindings[p] != binding) {
					pipelineKeys[p] = a_pipelines[p];
					pipelineBindings[p] = binding;
					geometryConstantsValid[p] = 0;
				}
			}
		}
	};
}
