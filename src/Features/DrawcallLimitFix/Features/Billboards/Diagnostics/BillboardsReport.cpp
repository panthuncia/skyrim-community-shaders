// Billboards' report lines under CS_DCLF_STATS.

#include "Features/DrawcallLimitFix/Features/Billboards/Billboards.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"

#include <string>

namespace DCLF::Features::Billboards
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			// Only the Effect family's keys carry billboard mode bits.
			std::uint32_t members = 0;
			{
				const auto& tables = Scene::FrameView::Get().GetTables();
				for (const auto& object : tables.objects) {
					if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember) || object.pipelineIndex >= tables.pipelines.size())
						continue;
					const std::uint32_t rasterFlags = tables.pipelines[object.pipelineIndex].rasterFlags;
					const auto& rules = Extensions::FamilyRules(rasterFlags);
					members += rules.turnedKeyBits && ((rasterFlags >> rules.spareShift) & kProgramFamilySpareBits) ? 1u : 0u;
				}
			}
			a_text += fmt::format("[DCLF] billboards: {} members bound, turned to each view's camera by the vertex stage\n", members);
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "billboards", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
	}
}
