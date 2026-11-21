// The draw families a trees view draws per face, such as tree LOD. Resolves their pipelines, ownership and rows.
// Render thread, except LodPhaseReady, which runs in the scene lane's commit.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"

#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"

namespace DCLF
{
	bool IndirectDraws::ResolveViewFaces(Api::EngineView a_view, bool a_on, const PipelineCatalog* a_catalog)
	{
		if (!TraitsOf(a_view).trees)
			return false;
		auto& state = impl->View(a_view);
		bool waited = false;
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			auto& faces = state.faces[d];
			faces.pipeline = {};
			// A family without faces, such as grass, has no forward pipeline.
			if (a_on && Extensions::DrawFamilyHasFaces(family) && Extensions::DrawFamilyOn(family)) {
				auto& drawPipelines = DrawPipelines::Get();
				const ForwardPipelineKey key{ 0, 0, FamilySlotOf(family).forwardFlags };
				drawPipelines.RequestForward(key);
				faces.pipeline = ForwardPipelineOf(a_catalog, key, state.targets);
				const auto* entry = a_catalog ? a_catalog->FindForward(key) : nullptr;
				waited = (!faces.pipeline.valid() && impl->familyOwned[d] && !(entry && entry->failed)) || waited;
			}
			faces.ready = faces.pipeline.valid();
		}
		return waited;
	}

	void IndirectDraws::DecideViewFaces(Api::EngineView a_view)
	{
		if (!TraitsOf(a_view).trees)
			return;
		// DCLF owns a family's faces only while it owns the main view's family and the faces' row is ready to draw.
		auto& state = impl->View(a_view);
		const auto* scene = impl->scene.get();
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			auto& own = state.faces[d];
			const auto rowDraws = [&] {
				own.row.assign(faces.rowBytes, std::byte{});
				Api::DrawFamilyFaceRow query{ *scene, true, own.row, nullptr };
				return faces.row && faces.row(query);
			};
			state.facesOwned[d] = ViewDrawable(a_view) && impl->ViewCovered(a_view) && own.ready && impl->familyOwned[d] && scene && rowDraws();
			PassCapture::Get().SetViewFamilyOwned(a_view, family, state.facesOwned[d]);
		}
	}

	ViewFamilyRows IndirectDraws::Impl::ViewFaceRows(ViewKind a_kind, const ViewFrame& a_shape, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		// Each family's row is drawn only when the shape has its pipeline and DCLF owns the family. Zero without the scene.
		ViewFamilyRows rows{};
		auto& state = View(a_kind);
		for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d) {
			const auto family = static_cast<Api::DrawFamily>(d);
			if (!Extensions::DrawFamilyHasFaces(family))
				continue;
			const auto& faces = Extensions::DrawFamilyOf(family).contribution.faces;
			auto& own = state.faces[d];
			own.row.assign(faces.rowBytes, std::byte{});
			if (scene && faces.row) {
				Api::DrawFamilyFaceRow row{ *scene, a_shape.faces[d].pipeline.valid() && own.pipeline.valid() && state.facesOwned[d], own.row, &a_owners };
				faces.row(row);
			}
			rows[d] = own.row;
		}
		return rows;
	}

	bool IndirectDraws::Impl::LodPhaseReady(std::uint8_t a_phase, const SceneTables& a_tables, std::uint32_t a_slot) const
	{
		const auto& views = Extensions::Views();
		for (std::size_t v = 0; v < views.views.size(); ++v) {
			if (views.views[v].phase != a_phase)
				continue;
			const auto& slotPipelines = View(static_cast<ViewKind>(v)).slotPipelines;
			const std::uint32_t p = a_slot < a_tables.objects.size() ? a_tables.objects[a_slot].pipelineIndex : ~0u;
			return p < slotPipelines.size() && slotPipelines[p].valid();
		}
		return false;
	}
}
