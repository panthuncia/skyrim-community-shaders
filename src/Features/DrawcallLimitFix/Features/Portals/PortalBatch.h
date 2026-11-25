#pragma once

#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <memory>
#include <string>

namespace DCLF::Api
{
	class SceneResourcesSetup;
}
namespace DCLF::Draws
{
	struct Resources;
	struct SceneBuffers;
}
namespace org
{
	class RenderGraph;
	class RenderPass;
}
namespace DCLF::Features::Portals
{
	/** @brief Scene-owned scratch. All query-indexed buffers grow as one unit group with their producer's rows. */
	struct Batch
	{
		std::string name;
		// claims: the member queries' per-object claims, which BuildDrawsCS takes while it makes them (MemberClaims).
		// traces: CS_DCLF_PORTAL_TRACE, each query's decision trace (PortalPrograms.hlsli).
		Draws::Versioned queries, results, claims, traces;
		std::shared_ptr<const ComputeProgram> program;
		bool trace = false;
	};
	/** @brief Creates the batch's buffers and its evaluation program. */
	std::shared_ptr<Batch> CreateBatch(const Api::SceneResourcesSetup& a_setup, std::string a_name = "cs.dclf.portal", bool a_contextAtlas = false);
	/** @brief Register a batch under the same unique prefix used for its resource names. */
	void RegisterBatch(org::RenderGraph& a_graph, const Batch& a_batch);
	/** @brief The pass that evaluates its owner's queries, one thread per query against its own program. */
	std::shared_ptr<org::RenderPass> MakeBatchPass(std::shared_ptr<Draws::Resources> a_resources, std::shared_ptr<const Batch> a_batch,
		RenderGraphRuntime::Segment a_segment);
}
