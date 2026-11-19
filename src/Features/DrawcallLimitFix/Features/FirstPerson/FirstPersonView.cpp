// The first-person view's per-frame setup and its view callbacks. Render thread.
//
// The skeleton's members draw with the camera the engine sets up for first person (FUN_1414cb020), in world space.
// The engine moves the root to the origin around its own draws (FUN_1414ce9d0). DCLF never reads the skeleton then,
// because FrameValues takes its rows and palettes at the frame's start.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Diagnostics/Draws/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// Recomputes Main::Draw's first-person flag (cVar23, AE 0x1406444b0). This runs before 0x1406445a1 hides the root.
		// The camera check reads the current state at PlayerCamera +0x28, not the free camera at +0xD0.
		bool FirstPersonLatch()
		{
			bool latch = false;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (const auto* root = player->Get3D(true); root && root != player->Get3D(false) && !root->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
					auto* ui = RE::UI::GetSingleton();
					const auto* camera = RE::PlayerCamera::GetSingleton();
					const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
					const bool freeCamera = camera && camera->IsCurrentState(RE::CameraState::kFree, RE::CameraState::kFree);
					latch = !mainMenu && camera && !freeCamera;
				}
			return latch;
		}

		// The colour targets are kMAIN and the motion vectors, as FUN_1414cb6d0 binds them. Returns true when they changed.
		bool CaptureFirstPersonTargets(const TargetFormats& a_formats)
		{
			auto* renderer = globals::game::renderer;
			if (renderer && a_formats.colorCount >= 2) {
				const auto& copy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
				if (copy.views[0]) {
					D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
					copy.views[0]->GetDesc(&dsvDesc);
					ForwardTargets targets;
					targets.colour = a_formats.colors[0];
					targets.colour2 = a_formats.colors[1];
					targets.depth = dsvDesc.Format;
					return IndirectDraws::Get().SetViewTargets(kView, targets);
				}
			}
			return false;
		}

		void WalkFirstPersonSkeleton(FirstPersonFrame& a_f, const SceneTables& a_tables, std::uint32_t a_frame)
		{
			auto& f = a_f;
			const auto& tables = a_tables;
			f.slotsFrame = a_frame;
			f.slotsWanted.assign(tables.pipelines.size(), 0);
			std::vector<RE::BSGeometry*> geometry;
			if (auto* player = RE::PlayerCharacter::GetSingleton())
				if (auto* root = player->Get3D(true); root && root != player->Get3D(false))
					SceneTracker::CollectGeometry(root, geometry);
			const ankerl::unordered_dense::set<const RE::BSGeometry*> skeleton(geometry.begin(), geometry.end());
			const auto claims = Scene::FrameView::Get().FrameClaims();
			f.forwardMembersNow = 0;
			for (std::size_t o = 0; o < tables.objects.size() && !skeleton.empty(); ++o)
				if (o < tables.objectGeometry.size() && tables.objectGeometry[o] && skeleton.contains(tables.objectGeometry[o]) &&
					!(tables.objects[o].flags & kObjectFree) && tables.objects[o].pipelineIndex < f.slotsWanted.size()) {
					f.slotsWanted[tables.objects[o].pipelineIndex] = 1;
					if ((tables.objects[o].flags & kObjectForward) && claims && (claims->PhasesOf(tables.objectGeometry[o]) & kSetFirstPerson))
						++f.forwardMembersNow;
				}
			if (claims)
				Diagnostics::Notify(Diagnostics::FirstPersonSkeletonWalked{ tables, skeleton, geometry.size(), *claims });
		}
	}

	void BeginFrame(const Api::ViewPointContext& a_context)
	{
		DCLF_ZONE("CS.DCLF.BeginFirstPersonFrame");
		auto& draws = IndirectDraws::Get();
		const auto* formats = draws.MainTargetFormats();
		if (!formats)
			return;
		auto& f = FrameState();
		const std::uint32_t frame = a_context.frame;
		const bool on = Extensions::ViewOn(ActiveToggles(), kView) && !a_context.failed && Draws::CurrentGraphConditions().firstPerson;
		f.latch = false;
		f.latchFrame = frame;
		if (!on)
			return;
		++f.frames;
		f.latch = FirstPersonLatch();
		f.latched += f.latch ? 1 : 0;
		const auto& state = *draws.ViewState(kView);
		if (CaptureFirstPersonTargets(*formats)) {
			draws.PostViewInputs();
			logger::info("[DCLF] first-person targets: colour {}, motion vectors {}, depth {}", static_cast<int>(state.targets.colour), static_cast<int>(state.targets.colour2),
				static_cast<int>(state.targets.depth));
		}
		if (state.targets.colour == DXGI_FORMAT_UNKNOWN)
			return;
		DrawPipelines::Get().SetViewForwardTargets(Api::EngineView::FirstPerson, state.targets);
		const auto* catalog = a_context.catalog;
		const auto& tables = *a_context.tables;
		// Walk the skeleton again every 16 frames or when the slot table grows. This runs before Main::Draw touches the root.
		if (frame - f.slotsFrame >= 16 || f.slotsWanted.size() != tables.pipelines.size())
			WalkFirstPersonSkeleton(f, tables, frame);
		// The main set's forward pipelines can draw here only when this pass's depth format matches the main pass's.
		const bool forwardCompatible = formats->depth != DXGI_FORMAT_UNKNOWN && formats->depth == state.targets.depth;
		if (draws.ExchangeViewForwardCompatible(kView, forwardCompatible) != forwardCompatible)
			logger::info("[DCLF] first-person forward part: the main depth format {} and kPOST_ZPREPASS_COPY's {}: {}", static_cast<int>(formats->depth),
				static_cast<int>(state.targets.depth), forwardCompatible ? "drawn by DCLF" : "the engine's");
		f.forwardMembers += f.forwardMembersNow;
		const auto pipelines = draws.ResolveViewPipelines(kView, tables, catalog, f.slotsWanted);
		f.slots = pipelines.slots;
		f.pipelinesReady = pipelines.ready;
		if (draws.SetUpView(kView))
			draws.ImportViewTarget(kView);
	}

	bool Covered(const Api::ViewCoverage& a_coverage)
	{
		const auto& f = FrameState();
		const auto& c = a_coverage;
		// The forward part has no plain buckets. A GPU-driven graph without device-generated commands can only draw it through the
		// direct forward family, so otherwise the engine keeps the whole view.
		return c.main && c.on && f.latch && f.latchFrame == c.frame && c.epochsCovered && c.targetImported && !c.rebuildPending && c.mainResourcesCurrent &&
		       !c.rebuildRequested && Draws::CurrentGraphConditions().firstPerson;
	}

	void CaptureRoot(Api::ViewRootCapture& a_capture)
	{
		// The first-person skeleton (PlayerCharacter +0x8F8) has no parent. Both roots are inputs so the category signature notices a swap.
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			auto* firstPerson = player->Get3D(true);
			const auto* thirdPerson = player->Get3D(false);
			a_capture.inputs = { firstPerson, thirdPerson };
			a_capture.inputCount = 2;
			if (firstPerson && firstPerson != thirdPerson)
				a_capture.root = firstPerson->AsNode();
		}
	}

	void CullHiddenSkeleton(Api::CullHiddenNodes& a_nodes)
	{
		// Main::Draw (AE 0x1406444b0) keeps the skeleton hidden for every view except its own first-person draw.
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const RE::NiAVObject* thirdPerson = player->Get3D(false);
			const RE::NiAVObject* firstPerson = player->Get3D(true);
			if (firstPerson && firstPerson != thirdPerson)
				a_nodes.Add(firstPerson);
		}
	}

	void Accumulators(Api::ViewAccumulators& a_accumulators)
	{
		// Main::unk_FPCulling culls only the first-person root into this accumulator, and FUN_1414cb6d0 renders it.
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> firstPersonAccumulator{ REL::Offset(Engine::Address::kFirstPersonAccumulator) };
		a_accumulators.accumulators[0] = *firstPersonAccumulator;
		a_accumulators.count = 1;
	}
}
