// The first-person view's per-frame setup and its view callbacks. Render thread.
//
// The skeleton's members draw with the camera the engine sets up for first person (FUN_1414cb020), in world space.
// The engine moves the root to the origin around its own draws (FUN_1414ce9d0). DCLF never reads the skeleton then,
// because FrameValues takes its rows and palettes at the frame's start.

#include "Features/DrawcallLimitFix/Features/FirstPerson/FirstPersonView.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/FrameDriver.h"
#include "Features/DrawcallLimitFix/Core/Render/IndirectDraws.h"
#include "Features/DrawcallLimitFix/Core/Scene/FrameView/FrameView.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/SceneSet.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Tables.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneTracker.h"

#include <memory>
#include <utility>
#include <vector>

namespace DCLF::Features::FirstPerson
{
	namespace
	{
		// Recomputes Main::Draw's first-person flag (cVar23, AE 0x1406444b0). The root's bit is the update's snapshot's, taken before
		// the water update hides the root (native runs that update in the capture). The camera check reads the current state at
		// PlayerCamera +0x28, not the free camera at +0xD0. Game half.
		bool FirstPersonLatch()
		{
			bool latch = false;
			const auto& snapshot = Snapshot::Building();
			if (auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton()); player && snapshot && snapshot->firstPersonShown)
				if (const auto* root = player->Get3D(true); root && root != player->Get3D(false)) {
					auto* ui = CS_ENGINE_READ(UI, CapturePoint, RE::UI::GetSingleton());
					const auto* camera = CS_ENGINE_READ(Camera, CapturePoint, RE::PlayerCamera::GetSingleton());
					const bool mainMenu = ui && ui->IsMenuOpen(RE::MainMenu::MENU_NAME);
					const bool freeCamera = camera && camera->IsCurrentState(RE::CameraState::kFree, RE::CameraState::kFree);
					latch = !mainMenu && camera && !freeCamera;
				}
			return latch;
		}

		// The colour targets are kMAIN and the motion vectors, as FUN_1414cb6d0 binds them. Returns true when they changed.
		bool CaptureFirstPersonTargets(const TargetFormats& a_formats)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
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

		/** @brief The game half's walk of the skeleton, kept between updates (game thread only). */
		struct SkeletonWalk
		{
			std::vector<std::uint8_t> slotsWanted;
			std::uint32_t slotsFrame = 0;
			std::uint32_t forwardMembersNow = 0;
		};

		SkeletonWalk& Walk()
		{
			static SkeletonWalk walk;
			return walk;
		}

		/** @brief What the game half's frame start took, for the render half's (a ticket part). */
		struct GamePart
		{
			bool latch = false;
			std::vector<std::uint8_t> slotsWanted;
			std::uint32_t forwardMembersNow = 0;
		};

		// Game thread: BeginGameFrame's part until the ticket takes it.
		std::shared_ptr<const GamePart> pendingPart;

		void WalkFirstPersonSkeleton(SkeletonWalk& a_walk, const SceneTables& a_tables, std::uint32_t a_frame)
		{
			auto& f = a_walk;
			const auto& tables = a_tables;
			f.slotsFrame = a_frame;
			f.slotsWanted.assign(tables.pipelines.size(), 0);
			std::vector<RE::BSGeometry*> geometry;
			if (auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton()))
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
		}
	}

	void BeginGameFrame(const Api::ViewPointContext& a_context)
	{
		DCLF_ZONE("CS.DCLF.BeginFirstPersonGameFrame");
		pendingPart = nullptr;
		if (!Extensions::ViewRegistered(kView))
			return;
		auto part = std::make_shared<GamePart>();
		part->latch = FirstPersonLatch();
		// Walk the skeleton again every 16 frames or when the slot table grows. This runs before Main::Draw touches the root.
		auto& walk = Walk();
		const auto& tables = *a_context.tables;
		if (a_context.frame - walk.slotsFrame >= 16 || walk.slotsWanted.size() != tables.pipelines.size())
			WalkFirstPersonSkeleton(walk, tables, a_context.frame);
		part->slotsWanted = walk.slotsWanted;
		part->forwardMembersNow = walk.forwardMembersNow;
		pendingPart = std::move(part);
	}

	std::shared_ptr<const void> TakeTicketPart()
	{
		return std::exchange(pendingPart, nullptr);
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
		const bool on = Extensions::ViewRegistered(kView);
		f.latch = false;
		f.latchFrame = frame;
		// The ticket's game part: every render of the ticket draws from it.
		const auto* part = static_cast<const GamePart*>(TicketPartOf(TicketPart::FirstPerson));
		if (!on || !part)
			return;
		++f.frames;
		f.latch = part->latch;
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
		// The main set's forward pipelines can draw here only when this pass's depth format matches the main pass's.
		const bool forwardCompatible = formats->depth != DXGI_FORMAT_UNKNOWN && formats->depth == state.targets.depth;
		if (draws.ExchangeViewForwardCompatible(kView, forwardCompatible) != forwardCompatible)
			logger::info("[DCLF] first-person forward part: the main depth format {} and kPOST_ZPREPASS_COPY's {}: {}", static_cast<int>(formats->depth),
				static_cast<int>(state.targets.depth), forwardCompatible ? "drawn by DCLF" : "the engine's");
		f.forwardMembers += part->forwardMembersNow;
		// A slot table that grew since the game half's walk wants none of its new slots yet.
		std::vector<std::uint8_t> wanted = part->slotsWanted;
		wanted.resize(tables.pipelines.size(), 0);
		const auto pipelines = draws.ResolveViewPipelines(kView, tables, catalog, wanted);
		f.slots = pipelines.slots;
		f.pipelinesReady = pipelines.ready;
		if (draws.SetUpView(kView))
			draws.ImportViewTarget(kView);
	}

	bool Covered(const Api::ViewCoverage& a_coverage)
	{
		const auto& f = FrameState();
		const auto& c = a_coverage;
		return c.main && c.on && f.latch && f.latchFrame == c.frame && c.epochsCovered && c.targetImported && !c.rebuildPending && c.mainResourcesCurrent &&
		       !c.rebuildRequested;
	}

	void CaptureRoot(Api::ViewRootCapture& a_capture)
	{
		// The first-person skeleton (PlayerCharacter +0x8F8) has no parent. Both roots are inputs so the category signature notices a swap.
		if (auto* player = CS_ENGINE_READ(Player, CapturePoint, RE::PlayerCharacter::GetSingleton())) {
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
		if (auto* player = CS_ENGINE_READ(Player, RecordTime, RE::PlayerCharacter::GetSingleton())) {
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
