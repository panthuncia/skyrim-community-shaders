// Effects report lines for the Effect port's members, materials and pipelines, with the Lighting parity under CS_DCLF_SET_PARITY.

#include "Features/DrawcallLimitFix/Features/Effects/Effects.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ProgramFamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Features/Effects/EffectConstants.h"

#include <string>

namespace DCLF::Features::Effects
{
	namespace
	{
		void Lines(std::string& a_text)
		{
			std::string text;
			{
				const auto& tables = Scene::FrameView::Get().GetTables();
				std::uint32_t members = 0, forwardMembers = 0, materials = 0, lit = 0, decals = 0, skinned = 0, opaque = 0;
				std::string firstOpaque;
				for (std::size_t o = 0; o < tables.objects.size(); ++o) {
					const auto& object = tables.objects[o];
					if ((object.flags & (kObjectFree | kObjectNoBindings)) || !(object.flags & kObjectMember))
						continue;
					forwardMembers += (object.flags & kObjectForward) ? 1u : 0u;
					if (object.pipelineIndex < tables.pipelines.size() && KeyFamily(tables.pipelines[object.pipelineIndex].rasterFlags) == Api::ProgramFamily::Effect) {
						++members;
						lit += (tables.pipelines[object.pipelineIndex].passDescriptor & EffectFlag::Lighting) ? 1u : 0u;
						decals += ObjectDecalGroup(object.flags) ? 1u : 0u;
						// An opaque effect is in decal group 1 with write mode 1. Real decals use 10 or 11.
						const auto rasterFlags = tables.pipelines[object.pipelineIndex].rasterFlags;
						if (RasterDecalGroup(rasterFlags) == 1 && RasterWriteMode(rasterFlags) == 1 && opaque++ == 0 && o < tables.objectGeometry.size() && tables.objectGeometry[o]) {
							const auto* geometry = tables.objectGeometry[o];
							firstOpaque = fmt::format("; first opaque: '{}' at ({:.0f}, {:.0f}, {:.0f}), raster flags {:#x}, pass {:#x}", geometry->name.c_str() ? geometry->name.c_str() : "",
								geometry->world.translate.x, geometry->world.translate.y, geometry->world.translate.z, rasterFlags,
								tables.pipelines[object.pipelineIndex].passDescriptor);
						}
						// A particle member's block holds its quads, not a skin palette.
						skinned += (object.flags & kObjectPaletteBlock) && !(tables.pipelines[object.pipelineIndex].passDescriptor & EffectFlag::Particles) ? 1u : 0u;
					}
				}
				for (std::uint32_t m = 0; m < tables.materials.size(); ++m)
					materials += tables.MaterialUsed(m) && tables.materials[m].kind == kMaterialKindEffect ? 1u : 0u;
				const auto pipelineStats = DrawPipelines::Get().GetStats();
				const auto& pipelines = pipelineStats.families[static_cast<std::size_t>(Api::ProgramFamily::Effect)];
				const bool on = ActiveToggles().On(Toggle());
				if (members || materials || pipelines.requested || on)
					text += fmt::format("[DCLF] effects (T7d's Effect port, CS_DCLF_EFFECTS {}): {} members bound ({} the Lighting variant, {} decals, {} skinned; {} forward "
										"members in all; {} opaque, hint 0, in decal group 1), {} effect materials in use; pipelines {} requested, {} ready, {} failed{}{}\n",
						on ? "on" : "off", members, lit, decals, skinned, forwardMembers, opaque, materials, pipelines.requested, pipelines.ready, pipelines.failed,
						pipelines.failed ? " <- EFFECT PIPELINE FAILED" : "", firstOpaque);
			}
			if (const auto [checked, other, cloud] = TakeLight(); checked)
				text += fmt::format(
					"[DCLF] effects' Lighting variant (CS_DCLF_SET_PARITY): {} engine passes of members checked ({} kCloudLOD), {} with a light 0 "
					"other than the port's{}\n",
					checked, cloud, other, other ? " <- EFFECT LIGHT" : " <- OK");
			a_text += text;
		}
	}

	void RegisterReport(Api::InstallContext& a_context)
	{
		a_context.RegisterReportLines({ .name = "effects", .point = Api::ReportPoint::AsyncWorkForward, .text = &Lines });
	}
}
