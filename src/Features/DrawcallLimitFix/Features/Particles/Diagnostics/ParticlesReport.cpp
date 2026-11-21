// Particles' report lines under CS_DCLF_STATS: the snapshots and the master particle systems.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Particles/MasterParticles.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "ShaderCache.h"

#include <string>

namespace DCLF::Features::Particles
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			std::uint32_t members = 0;
			{
				const auto& installedTables = Scene::FrameView::Get().GetTables();
				for (const auto& object : installedTables.objects) {
					if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember) || object.pipelineIndex >= installedTables.pipelines.size())
						continue;
					const auto& pipeline = installedTables.pipelines[object.pipelineIndex];
					const bool particlePass = (pipeline.passDescriptor & static_cast<std::uint32_t>(SIE::ShaderCache::EffectShaderFlags::Particles)) != 0;
					members += KeyFamily(pipeline.rasterFlags) == Api::ProgramFamily::Effect && particlePass ? 1u : 0u;
				}
			}
			const auto c = ParticleSnapshots::Get().Take();
			std::string text;
			text += fmt::format("[DCLF] particles (T7e): {} members bound, {} systems followed; {} snapshots taken at the engine's updates ({} "
									"update passes with nothing new skipped, {} while another update wrote the system); {} expansions of {} particles ({} without a "
									"snapshot yet, {} systems newly frozen: the same snapshot {} runs in a row, {} of them aligned quads); {} tables "
									"published, {} records pruned\n",
					members, c.followed, c.captures, c.unchanged, c.contended, c.expansions, c.expandedParticles, c.withoutSnapshot, c.frozen,
					ParticleSnapshots::kFrozenRuns, c.aligned, c.published, c.pruned);
			// The master particle systems' lifetime and references (CS_DCLF_MASTER_PARTICLES).
			MasterParticles::Lines(text);
			a_text += text;
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "particles", .point = Api::ReportPoint::AsyncWork, .text = &Lines });
	}
}
