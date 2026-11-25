#include "Features/DrawcallLimitFix/Features/Portals/PortalBatch.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Extensions/SceneResources.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalPrograms.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalViews.h"
#include "Features/DrawcallLimitFix/Core/Gpu/SetupPrograms.h"

#include <array>
#include <cstddef>

namespace DCLF::Features::Portals
{
	void RegisterBatch(org::RenderGraph& a_graph, const Batch& a_batch)
	{
		const auto add = [&](const char* suffix, const Draws::Versioned& buffer) {
			Draws::Register(a_graph, org::ResourceIdentifier(a_batch.name + suffix), buffer);
		};
		add("-queries", a_batch.queries);
		add("-results", a_batch.results);
		add("-claims", a_batch.claims);
		add("-traces", a_batch.traces);
	}

	namespace
	{
		bool Traced() { return RegisteredSwitchValue("CS_DCLF_PORTAL_TRACE") == "1"; }

		ComputeProgram::Desc BatchDesc(bool a_contextAtlas)
		{
			ComputeProgram::Desc desc{ .source = "DrawcallLimitFix/PortalBatchCS.hlsl", .entry = L"Inline", .constantWords = 12 };
			if (a_contextAtlas)
				desc.defines.emplace_back(L"DCLF_PORTAL_CONTEXT_ATLAS", L"1");
			if (RegisteredSwitchValue("CS_DCLF_PORTAL_UNIFORM_SCAN") == "1")
				desc.defines.emplace_back(L"DCLF_PORTAL_UNIFORM_SCAN", L"1");
			if (Traced())
				desc.defines.emplace_back(L"DCLF_PORTAL_TRACE", L"1");
			return desc;
		}

		/** @brief The program with and without the context atlas, queued at CS's setup (SetupPrograms). */
		const std::array<SetupPrograms::Compute, 2> kBatchPrograms{ SetupPrograms::Compute("DCLF portal batch", [] { return BatchDesc(false); }),
			SetupPrograms::Compute("DCLF portal batch", [] { return BatchDesc(true); }) };
	}

	std::shared_ptr<Batch> CreateBatch(const Api::SceneResourcesSetup& a_setup, std::string a_name, bool a_contextAtlas)
	{
		auto batch = std::make_shared<Batch>();
		batch->name = std::move(a_name);
		batch->trace = Traced();
		batch->program = kBatchPrograms[a_contextAtlas ? 1 : 0].Get();
		const auto buffer = [&](std::uint32_t stride, const char* suffix, std::uint32_t count) {
			return Draws::MakeVersioned(a_setup.StructuredBuffer(count, stride, (batch->name + suffix).c_str(), true), { .extent = { 0, stride, stride } });
		};
		const auto roots = a_setup.smallStart ? 4u : 4096u;
		batch->queries = buffer(sizeof(DCLF::Portals::Query), "-queries", roots);
		batch->results = buffer(sizeof(DCLF::Portals::Result), "-results", roots);
		batch->claims = buffer(4, "-claims", roots);
		batch->traces = buffer(16, "-traces", roots);
		return batch;
	}
}
