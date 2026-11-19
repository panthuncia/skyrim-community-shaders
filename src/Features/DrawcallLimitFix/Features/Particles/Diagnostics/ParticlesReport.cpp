// Particles' report lines under CS_DCLF_STATS: the snapshots, and with CS_DCLF_SET_PARITY the engine's passes against them.

#include "Features/DrawcallLimitFix/Features/Particles/Particles.h"
#include "Features/DrawcallLimitFix/Features/Particles/MasterParticles.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
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
			const bool on = ActiveToggles().On(Toggle());
			std::string text;
			if (on || members || c.captures || c.expansions)
				text += fmt::format("[DCLF] particles (T7e, CS_DCLF_PARTICLES {}): {} members bound, {} systems followed; {} snapshots taken at the engine's updates ({} "
									"update passes with nothing new skipped, {} while another update wrote the system); {} expansions of {} particles ({} without a "
									"snapshot yet, {} systems newly frozen: the same snapshot {} runs in a row, {} of them aligned quads); {} tables "
									"published, {} records pruned\n",
					on ? "on" : "off", members, c.followed, c.captures, c.unchanged, c.contended, c.expansions, c.expandedParticles, c.withoutSnapshot, c.frozen,
					ParticleSnapshots::kFrozenRuns, c.aligned, c.published, c.pruned);
			// A member under a stood-in root is not culled by the engine at all, so 0 registrations is expected there. A non-zero uncovered count is the defect.
			if (const auto p = TakePasses(); p.passes || (Diagnostics::Observe().claims && on)) {
				const std::string first = p.first.empty() ? std::string() : fmt::format("; first {}", p.first);
				text += fmt::format("[DCLF] particles' engine passes (CS_DCLF_SET_PARITY): {} passes of particle members the engine registered (withheld), {} of "
									"them without a snapshot DCLF expanded in its last {} runs{}{}\n",
					p.passes, p.uncovered, ParticleSnapshots::kCoveredRuns, p.uncovered ? " <- PARTICLES" : " <- OK", first);
			}
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
