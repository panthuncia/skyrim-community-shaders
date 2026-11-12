// PassCapture: the registration hooks (RegisterPass and the direct group/list insertions; any registering thread), the withholding
// tests by view, the per-pass record the render thread drains, and the renderer sets refreshed on the render thread.
#include "Features/DrawcallLimitFix/Core/Handoff/PassCapture.h"

#include "Features/DrawcallLimitFix/Diagnostics/Feature/OpenDefectProbes.h"
#include "Features/DrawcallLimitFix/Diagnostics/Engine/RegistrationCensus.h"

#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "Features/DrawcallLimitFix/Core/Settings/Toggles.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"

#include <algorithm>
#include <span>
#include "Features/DrawcallLimitFix/Diagnostics/Engine/Events.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "TextureStreaming/Streamer.h"

namespace DCLF
{
	namespace
	{
		// BSShaderProperty::EShaderPropertyFlag bits the engine's batch-group classifier reads.
		constexpr std::uint64_t kFlagList4 = 1ull << 54;  // -> list 4 on its own
		constexpr std::uint64_t kFlagList2 = 1ull << 36;  // -> adds 2
		constexpr std::uint16_t kAlphaTestingFlag = 1u << 9;

		// A draw family's engine shapes are known by their shader property's vtable (DrawFamilyEntry::engineShapeVtable, resolved at
		// install; an untaken family's is 0, which no property's is).
		const void* ShaderPropertyOf(const RE::BSGeometry* a_geometry) { return a_geometry->GetGeometryRuntimeData().shaderProperty.get(); }

		/** @brief The draw family whose engine shape a_geometry is (kDrawFamilies: none). */
		std::size_t EngineShapeFamily(const RE::BSGeometry* a_geometry)
		{
			const void* property = ShaderPropertyOf(a_geometry);
			const auto& families = Extensions::DrawFamilies();
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				if (Engine::VtableIs(property, families[f].engineShapeVtable))
					return f;
			return Extensions::kDrawFamilies;
		}

		// A draw family's faces' root in a view's roots (Api::ViewRootBit; 0: none).
		std::uint32_t FacesRootBit(std::size_t a_family)
		{
			const auto root = Extensions::DrawFamilies()[a_family].contribution.faces.root;
			return root < Api::ViewRoot::Count ? Api::ViewRootBit(root) : 0u;
		}

		// The shadow-map registration this thread runs now (ShadowRegistration): its accumulator's render mode is a shadow view's.
		thread_local bool t_shadowRegistration = false;
		// The accumulator the shadow modes' registration on this thread registers into, and its render mode (~0u: none).
		thread_local const void* t_registrationAccumulator = nullptr;
		thread_local std::uint32_t t_registrationMode = ~0u;
		// Inside the engine's focus accumulation of a host whose focus views DCLF covers (SetFocusAccumulation).
		thread_local bool t_focusAccumulationCovered = false;
		// The menu scenes' renders this thread is inside (MenuSceneScope).
		thread_local std::uint32_t t_menuScenes = 0;

		/**
		 * @brief Whether a_pass is of DCLF's classes: a Lighting object's (grass included; its depth and shadow passes are Utility on the
		 * same property), an effect's or tree LOD's.
		 */
		bool DclfClass(const RE::BSRenderPass* a_pass)
		{
			const auto* property = a_pass->shaderProperty;
			return property && (netimmerse_cast<const RE::BSLightingShaderProperty*>(property) || netimmerse_cast<const RE::BSEffectShaderProperty*>(property) ||
								   netimmerse_cast<const RE::BSDistantTreeShaderProperty*>(property));
		}
	}

	PassCapture& PassCapture::Get()
	{
		static PassCapture capture;
		return capture;
	}

	PassCapture::PassCapture() = default;

	std::uint32_t PassCapture::SubPassOf(bool a_alphaTesting, std::uint64_t a_propertyFlags)
	{
		if (a_propertyFlags & kFlagList4)
			return 4;
		return ((a_propertyFlags & kFlagList2) ? 2u : 0u) | (a_alphaTesting ? 1u : 0u);
	}

	std::uint32_t PassCapture::SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags)
	{
		if (a_propertyFlags & kFlagList4)
			return 4;
		std::uint32_t subPass = (a_propertyFlags & kFlagList2) ? 2u : 0u;
		if (a_geometry) {
			const auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
			if (alpha && (alpha->alphaFlags & kAlphaTestingFlag))
				subPass |= 1u;
		}
		return subPass;
	}

	void PassCapture::Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique, bool a_withheld)
	{
		if (!a_pass || !a_pass->geometry || !a_pass->shader)
			return;
		if (a_pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
			return;

		// How many threads register, for the record: the engine classifier takes a mutex and the scene
		// lists are built on a job list, so more than one is expected.
		const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
		if (lastThread.exchange(thread, std::memory_order_relaxed) != thread)
			threadCount.fetch_add(1, std::memory_order_relaxed);

		const auto slot = cursor.fetch_add(1, std::memory_order_relaxed);
		if (slot >= kCapacity) {
			overflow.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto* property = a_pass->geometry->GetGeometryRuntimeData().shaderProperty.get();
		entries[slot] = Entry{ a_pass->geometry, a_pass, a_batch, a_technique,
			SubPassOf(a_pass->geometry, property ? property->flags.underlying() : 0ull), a_pass->passEnum, a_withheld,
			property && property->fadeNode ? property->fadeNode->GetRuntimeData().unk153 : std::uint8_t{ 0xFF }, static_cast<std::uint8_t>(a_pass->accumulationHint) };
	}

	std::span<const PassCapture::Entry> PassCapture::Drain()
	{
		const auto count = std::min(cursor.exchange(0, std::memory_order_relaxed), kCapacity);
		stats.captured = static_cast<std::uint32_t>(count);
		stats.overflowed = overflow.exchange(0, std::memory_order_relaxed);
		stats.threads = threadCount.exchange(0, std::memory_order_relaxed);
		for (std::uint32_t m = 0; m < kShadowModes; ++m)
			stats.shadowWithheld[m] = shadowWithheld[m].exchange(0, std::memory_order_relaxed);
		stats.volumetricWithheld = volumetricWithheld.exchange(0, std::memory_order_relaxed);
		stats.directWithheld = directWithheld.exchange(0, std::memory_order_relaxed);
		stats.mainWithheld = mainWithheld.exchange(0, std::memory_order_relaxed);
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
			stats.familyWithheld[f] = familyWithheld[f].exchange(0, std::memory_order_relaxed);
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.ownRendererViews[i]);
			stats.viewWithheld[v] = viewRenderers[v].ownWithheld.exchange(0, std::memory_order_relaxed);
			for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
				stats.viewFamilyWithheld[v][f] = viewRenderers[v].ownFamilyWithheld[f].exchange(0, std::memory_order_relaxed);
		}
		stats.occlusionWithheld = occlusionWithheld.exchange(0, std::memory_order_relaxed);
		stats.holdWithheld = holdWithheld.exchange(0, std::memory_order_relaxed);
		stats.pendingWithheld = pendingWithheld.exchange(0, std::memory_order_relaxed);
		stats.baseMipsWithheld = baseMipsWithheld.exchange(0, std::memory_order_relaxed);
		stats.dclfKept = dclfKept.exchange(0, std::memory_order_relaxed);
		lastDrain = { entries.data(), count };
		return lastDrain;
	}

	namespace
	{
		// An accumulator's batch renderers: its own, its geometry groups' and its alpha group's, in that order. The engine makes groups as
		// geometry first uses them, so a set made from these is made again whenever the list changes, not only when the accumulators do.
		void CollectRenderers(const void* a_accumulator, std::vector<const RE::BSBatchRenderer*>& a_out)
		{
			const auto* accumulator = static_cast<const RE::BSGraphics::BSShaderAccumulator*>(a_accumulator);
			const auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				return;
			a_out.push_back(batch);
			for (const auto* group : batch->geometryGroups)
				if (group && group->batchRenderer)
					a_out.push_back(group->batchRenderer);
			if (batch->alphaGroup && batch->alphaGroup->batchRenderer)
				a_out.push_back(batch->alphaGroup->batchRenderer);
		}

		/** @brief Whether a_batch is one of a_accumulator's batch renderers now (CollectRenderers' list, read live). */
		bool HasRenderer(const void* a_accumulator, const RE::BSBatchRenderer* a_batch)
		{
			const auto* accumulator = static_cast<const RE::BSGraphics::BSShaderAccumulator*>(a_accumulator);
			const auto* batch = accumulator ? accumulator->GetRuntimeData().batchRenderer : nullptr;
			if (!batch)
				return false;
			if (batch == a_batch)
				return true;
			for (const auto* group : batch->geometryGroups)
				if (group && group->batchRenderer == a_batch)
					return true;
			return batch->alphaGroup && batch->alphaGroup->batchRenderer == a_batch;
		}
	}

	void PassCapture::RefreshMainRenderers()
	{
		// The Z-prepass's accumulator (render mode 0xC) and the main camera's (render mode 0), as the registration jobs
		// (FUN_1414cbff0) take them (skyrim-engine-notes.md, "The primary's cull: the scene lists").
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> depthAccumulator{ REL::Offset(Engine::Address::kDepthAccumulator) };
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		const std::array<const void*, 2> accumulators{ *depthAccumulator, *mainAccumulator };
		// The InMainRenderers views' accumulators (their callbacks: the first person's, Main::unk_FPCulling culls the first-person root
		// alone into it and FUN_1414cb6d0 renders it). Their registrations are their members' alone, so a member's there is the view's
		// member's (WithholdMain).
		const auto& views = Extensions::Views();
		std::array<Api::ViewAccumulators, Extensions::kEngineViews> viewAccumulators{};
		std::vector<const RE::BSBatchRenderer*> sources;
		for (const void* accumulator : accumulators)
			CollectRenderers(accumulator, sources);
		std::array<std::size_t, Extensions::kEngineViews + 1> viewSourcesAt{};
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			views.views[v].accumulators(viewAccumulators[v]);
			viewSourcesAt[i] = sources.size();
			for (std::uint32_t a = 0; a < viewAccumulators[v].count; ++a)
				CollectRenderers(viewAccumulators[v].accumulators[a], sources);
		}
		viewSourcesAt[views.mainRendererCount] = sources.size();
		if (sources == mainSources && std::atomic_load(&mainRenderers))
			return;
		mainAccumulators = accumulators;
		mainSources = sources;
		auto renderers = std::make_shared<RendererSet>(sources.begin(), sources.end());
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			viewRenderers[v].accumulators = viewAccumulators[v];
			auto viewSet = std::make_shared<RendererSet>(sources.begin() + viewSourcesAt[i], sources.begin() + viewSourcesAt[i + 1]);
			std::atomic_store(&viewRenderers[v].renderers, std::shared_ptr<const RendererSet>(std::move(viewSet)));
		}
		logger::info("[DCLF] the main camera's views: {} batch renderers (Z-prepass accumulator {}, main {})", renderers->size(), accumulators[0], accumulators[1]);
		std::atomic_store(&mainRenderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	PassCapture::MenuSceneScope::MenuSceneScope() { ++t_menuScenes; }
	PassCapture::MenuSceneScope::~MenuSceneScope() { --t_menuScenes; }

	bool PassCapture::HeldByLoad(const RE::BSRenderPass* a_pass)
	{
		// A load's hold (Core/Render/LoadHold.h): the world's frames run off the screen, and every pass of DCLF's classes into a view DCLF
		// draws is withheld, member or not, so the engine draws none of the load's objects while their claims settle. A menu scene's are
		// not the world's (the loading menu's model and its shadow casters draw).
		if (!LoadHold::Active() || t_menuScenes || !Policy::WithholdFromEngine() || !DclfClass(a_pass))
			return false;
		holdWithheld.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	void PassCapture::NotePending(const SetSnapshot& a_set, const RE::BSGeometry* a_geometry, std::uint8_t a_phases)
	{
		if (!a_set.pending.empty() && !(a_set.PhasesOf(a_geometry) & a_phases))
			pendingWithheld.fetch_add(1, std::memory_order_relaxed);
	}

	bool PassCapture::HeldPhase(const SetSnapshot& a_set, const RE::BSGeometry* a_geometry, std::uint8_t a_phase)
	{
		if (!(a_set.WithheldPhasesOf(a_geometry) & a_phase))
			return false;
		NotePending(a_set, a_geometry, a_phase);
		return true;
	}

	bool PassCapture::WithholdMain(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || !Policy::WithholdFromEngine())
			return false;
		// The main views' renderers, and a geometry group's renderer the engine made during this frame's registrations (in no set until the
		// next frame's RefreshMainRenderers: MainRendererNow). A shadow-map registration is never the main views'.
		if (!IsMainRenderer(a_batch) && (t_shadowRegistration || !MainRendererNow(a_batch)))
			return false;
		if (HeldByLoad(a_pass))
			return true;
		// Tree LOD (dclf-architecture.md, "Tree LOD: the draws"): DCLF draws every instance the mirror holds this frame, so the engine's
		// passes of every tree LOD shape (its Z-prepass's, hint 7 into group 1 through the shadow modes' registration; its colour
		// pass's, hint 7 into group 1) are withheld. So for every draw family DCLF draws this frame, or waits to draw (its pipelines not
		// ready yet): its engine shapes' passes.
		for (std::size_t f = 0; f < Extensions::kDrawFamilies; ++f)
			if (familyHeld[f].load(std::memory_order_acquire) && Engine::VtableIs(ShaderPropertyOf(a_pass->geometry), Extensions::DrawFamilies()[f].engineShapeVtable)) {
				familyWithheld[f].fetch_add(1, std::memory_order_relaxed);
				return true;
			}
		// A member's phases, or a waiting object's (SetSnapshot::pending): the engine draws neither.
		const auto set = std::atomic_load(&frameSet);
		const std::uint8_t phases = set ? set->WithheldPhasesOf(a_pass->geometry) : std::uint8_t{ 0 };
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i)
			if (IsViewRenderer(views.mainRendererViews[i], a_batch))
				viewRenderers[static_cast<std::size_t>(views.mainRendererViews[i])].registered.fetch_add(1, std::memory_order_relaxed);
		// An InMainRenderers view's member (the first person's: kSetFirstPerson alone, no world view registers it, its root hidden there):
		// its passes into the view's accumulators and into the main views' (the first person's RenderDepth block culls into the
		// Z-prepass's) are the view's epochs' while they draw this frame (SetViewCovered); every fade of the skeleton is the engine's
		// root's (alpha 1).
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.mainRendererViews[i]);
			const std::uint8_t phase = views.views[v].phase;
			if (!(phases & phase))
				continue;
			auto& view = viewRenderers[v];
			if (ViewCovered(views.mainRendererViews[i]) && (set->drawn & phase)) {
				view.withheld.fetch_add(1, std::memory_order_relaxed);
				NotePending(*set, a_pass->geometry, phase);
				return true;
			}
			view.memberKept.fetch_add(1, std::memory_order_relaxed);
			return false;
		}
		if (!(phases & kSetMain))
			return false;
		// Every pass of a member, a fade's and a LOD cross-fade's hint-10 copy of the old level included: DCLF draws them (the
		// draw's alpha takes its root's FadeStateCS fade, and BuildDraws adds the copy partitions while the root crosses).
		mainWithheld.fetch_add(1, std::memory_order_relaxed);
		NotePending(*set, a_pass->geometry, kSetMain);
		return true;
	}

	void PassCapture::SetViewRenderers(Api::EngineView a_view, const Api::ViewAccumulators& a_accumulators)
	{
		// The view's accumulators (the reflection's: BSCubeMapCamera's two, the faces cull into the first): their batch renderers, their
		// geometry groups' and their alpha groups', made again whenever any of them changes.
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		std::vector<const RE::BSBatchRenderer*> sources;
		for (std::uint32_t a = 0; a < a_accumulators.count; ++a)
			CollectRenderers(a_accumulators.accumulators[a], sources);
		if (a_accumulators == view.accumulators && sources == view.sources && std::atomic_load(&view.renderers))
			return;
		view.accumulators = a_accumulators;
		view.sources = sources;
		auto renderers = std::make_shared<RendererSet>(sources.begin(), sources.end());
		std::string accumulators;
		for (std::uint32_t a = 0; a < a_accumulators.count; ++a)
			accumulators += fmt::format("{}{}", a ? ", " : "", a_accumulators.accumulators[a]);
		std::string batches;
		for (const auto* batch : sources)
			batches += fmt::format("{}{}", batches.empty() ? "" : ", ", static_cast<const void*>(batch));
		logger::info("[DCLF] the {} faces: {} batch renderers (accumulators {}; renderers {})", Extensions::ViewEntryOf(a_view).name, renderers->size(), accumulators, batches);
		std::atomic_store(&view.renderers, std::shared_ptr<const RendererSet>(std::move(renderers)));
	}

	bool PassCapture::OwnViewRendering() const
	{
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i)
			if (viewRenderers[static_cast<std::size_t>(views.ownRendererViews[i])].rendering.load(std::memory_order_acquire))
				return true;
		return false;
	}

	bool PassCapture::OwnViewWithholding() const
	{
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i)
			if (viewRenderers[static_cast<std::size_t>(views.ownRendererViews[i])].withholdActive.load(std::memory_order_acquire) && ViewCovered(views.ownRendererViews[i]))
				return true;
		return false;
	}

	bool PassCapture::WithholdOwn(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry || !Policy::WithholdFromEngine())
			return false;
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.ownRendererCount; ++i) {
			const auto v = static_cast<std::size_t>(views.ownRendererViews[i]);
			auto& view = viewRenderers[v];
			// The view's own gates, or a load's hold (every view DCLF draws), before its renderers are read.
			const bool withholding = view.withholdActive.load(std::memory_order_acquire) && ViewCovered(views.ownRendererViews[i]);
			if (!withholding && !LoadHold::Active())
				continue;
			const auto renderers = std::atomic_load(&view.renderers);
			if (!renderers || !renderers->contains(a_batch))
				continue;
			if (HeldByLoad(a_pass))
				return true;
			if (!withholding)
				continue;
			// A draw family's engine shape (tree LOD's), withheld while the view's faces draw the family. One of a family without faces
			// (grass) is never a view's: the view's own renderers draw it, and it is no residue of the view's roots.
			const std::size_t family = EngineShapeFamily(a_pass->geometry);
			if (family < Extensions::kDrawFamilies && !Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(family)))
				return false;
			const bool treeLod = family < Extensions::kDrawFamilies;
			if (treeLod && (view.familyOwned.load(std::memory_order_acquire) & (1u << family))) {
				view.ownFamilyWithheld[family].fetch_add(1, std::memory_order_relaxed);
				return true;
			}
			const auto set = std::atomic_load(&frameSet);
			const std::uint8_t phase = views.views[v].phase;
			if (!set || !(set->drawn & phase) || !HeldPhase(*set, a_pass->geometry, phase)) {
				// The residue watch: a draw family's engine shape under its faces' root, anything else but the sky under the view root
				// contributions' roots (the LOD land and objects').
				if (const auto watch = view.residueWatch.load(std::memory_order_acquire); watch & (treeLod ? FacesRootBit(family) : views.rootBits))
					NoteViewResidue(view, a_pass, treeLod);
				return false;
			}
			view.ownWithheld.fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	void PassCapture::NoteViewResidue(ViewRenderers& a_view, const RE::BSRenderPass* a_pass, bool a_treeLod)
	{
		const auto* sky = RE::Sky::GetSingleton();
		const RE::NiNode* skyRoot = sky ? sky->root.get() : nullptr;
		for (const RE::NiAVObject* node = a_pass->geometry; node; node = node->parent)
			if (node == skyRoot)
				return;
		if (!a_treeLod) {
			std::scoped_lock lock(a_view.residueLock);
			if (a_view.residueGeometries.size() < 4096)
				a_view.residueGeometries.push_back(a_pass->geometry);
		}
		if ((a_treeLod ? a_view.residueTree : a_view.residueLod).fetch_add(1, std::memory_order_relaxed) == 0) {
			std::scoped_lock lock(a_view.residueLock);
			if (a_view.residueFirst.empty()) {
				const auto* name = a_pass->geometry->name.c_str();
				a_view.residueFirst = fmt::format("'{}' (technique {:#x}, hint {})", name ? name : "?", a_pass->passEnum, static_cast<std::uint32_t>(a_pass->accumulationHint));
			}
		}
	}

	std::vector<const RE::BSGeometry*> PassCapture::TakeViewResidueGeometries(Api::EngineView a_view)
	{
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		std::scoped_lock lock(view.residueLock);
		return std::exchange(view.residueGeometries, {});
	}

	PassCapture::ViewResidue PassCapture::TakeViewResidue(Api::EngineView a_view)
	{
		auto& view = viewRenderers[static_cast<std::size_t>(a_view)];
		ViewResidue out;
		out.lodPasses = view.residueLod.exchange(0, std::memory_order_relaxed);
		out.treePasses = view.residueTree.exchange(0, std::memory_order_relaxed);
		std::scoped_lock lock(view.residueLock);
		out.first = std::move(view.residueFirst);
		view.residueFirst.clear();
		return out;
	}

	bool PassCapture::ShadowWithholdingEnabled()
	{
		return ActiveToggles().shadows && !!Policy::WithholdFromEngine();
	}

	void PassCapture::SetShadowBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&shadowRenderers, std::move(a_renderers));
	}

	void PassCapture::SetFocusBatchRenderers(std::shared_ptr<const ShadowRendererMap> a_renderers)
	{
		std::atomic_store(&focusRenderers, std::move(a_renderers));
	}

	void PassCapture::SetShadowAccumulators(std::shared_ptr<const ShadowAccumulatorMap> a_accumulators)
	{
		std::atomic_store(&shadowAccumulators, std::move(a_accumulators));
	}

	void PassCapture::SetFocusAccumulation(bool a_covered) { t_focusAccumulationCovered = a_covered; }

	bool PassCapture::ShadowModeOfBatch(const RE::BSBatchRenderer* a_batch, std::uint32_t& a_mode) const
	{
		const auto renderers = std::atomic_load(&shadowRenderers);
		if (!renderers)
			return false;
		if (const auto it = renderers->find(a_batch); it != renderers->end()) {
			if (it->second >= kShadowModes)
				return false;
			a_mode = it->second;
			return true;
		}
		// A renderer of no published set during the shadow modes' registration: a geometry group's the engine made for this pass. It is
		// the registering accumulator's view's (covered views only: ShadowViews::PublishRenderers).
		if (!t_shadowRegistration || !t_registrationAccumulator)
			return false;
		const auto accumulators = std::atomic_load(&shadowAccumulators);
		const auto it = accumulators ? accumulators->find(t_registrationAccumulator) : ShadowAccumulatorMap::const_iterator{};
		if (accumulators && it != accumulators->end() && it->second < kShadowModes) {
			a_mode = it->second;
			return true;
		}
		// Inside a covered host's focus accumulation: its focus view's, even through an accumulator made inside it.
		if (t_focusAccumulationCovered && t_registrationMode >= kFirstShadowMode && t_registrationMode < kFirstShadowMode + kShadowModes) {
			a_mode = t_registrationMode - kFirstShadowMode;
			return true;
		}
		return false;
	}

	bool PassCapture::CastersWithheld(std::uint8_t a_phase) const
	{
		const auto set = std::atomic_load(&frameSet);
		return set && (set->drawn & a_phase) && ShadowWithholdingEnabled();
	}

	std::shared_ptr<const SetSnapshot> PassCapture::CastersForBatch(const RE::BSBatchRenderer* a_batch, std::uint8_t* a_phase) const
	{
		std::uint32_t mode = 0;
		if (!ShadowModeOfBatch(a_batch, mode) || !ShadowWithholdingEnabled())
			return nullptr;
		const std::uint8_t phase = SetPhaseOfMode(mode);
		if (a_phase)
			*a_phase = phase;
		auto set = std::atomic_load(&frameSet);
		return set && (set->drawn & phase) ? set : nullptr;
	}

	bool PassCapture::Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!a_pass || !a_pass->geometry)
			return false;
		// A shadow view's renderers (the focus views' among them): the set's casters. A pass into any other renderer
		// (reflections, cubemaps) is never withheld here; the main camera's are WithholdMain's, an OwnRenderers view's WithholdOwn's.
		// What the engine registers into a covered focus view, for its observer (Features/Shadows/Local's focus parity: against its target's fade root).
		if (Diagnostics::Armed<Diagnostics::FocusRegistration>())
			if (const auto focus = std::atomic_load(&focusRenderers); focus && !focus->empty())
				if (const auto it = focus->find(a_batch); it != focus->end())
					Diagnostics::Notify(Diagnostics::FocusRegistration{ it->second, a_pass->geometry });
		std::uint32_t mode = 0;
		// Any shadow-map caster pass during a load's hold, whatever renderer it goes to.
		if ((t_shadowRegistration || ShadowModeOfBatch(a_batch, mode)) && HeldByLoad(a_pass))
			return true;
		std::uint8_t phase = 0;
		if (const auto set = CastersForBatch(a_batch, &phase); set && ShadowModeOfBatch(a_batch, mode) && HeldPhase(*set, a_pass->geometry, phase)) {
			shadowWithheld[mode].fetch_add(1, std::memory_order_relaxed);
			return true;
		}
		return false;
	}

	void PassCapture::NoteKept(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint8_t a_site, std::uint8_t a_occlusionPhase) const
	{
		// The census's per-pass values, read here as the pass registers (the frame's end has neither the pass, a pooled object, nor
		// these states): whether the reflection faces withhold now (WithholdOwn's own gates).
		const bool face = OwnViewWithholding();
		Diagnostics::Notify(Diagnostics::PassKept{ a_batch, a_pass, static_cast<RegistrationCensus::Site>(a_site), face, a_occlusionPhase });
	}

	namespace
	{
		/**
		 * @brief The shadow modes' registration (FUN_1414b2a60, SE 99932 / AE 106576; the Z-prepass's 0xC too): the one function that builds
		 * a caster's shadow-map passes (the property's GetRenderPasses_ShadowMapOrMask, vtable +0x158) and hands them to the accumulator's
		 * renderers. Every hook it reaches (RegisterPass, its direct group insertions) runs inside it on this thread, so the accumulator's
		 * render mode says the pass is a shadow view's, whatever renderer sets DCLF published (a load's hold withholds by it).
		 */
		struct ShadowRegistration
		{
			static std::uint64_t thunk(RE::BSShaderAccumulator* a_accumulator, RE::BSGeometry* a_geometry, RE::BSShaderProperty* a_property)
			{
				const auto* data = a_accumulator ? a_accumulator->GetRuntimeData() : nullptr;
				const auto mode = data ? static_cast<std::uint32_t>(data->renderMode) : 0u;
				const bool previous = std::exchange(t_shadowRegistration,
					mode >= PassCapture::kFirstShadowMode && mode < PassCapture::kFirstShadowMode + PassCapture::kShadowModes);
				const auto previousAccumulator = std::exchange(t_registrationAccumulator, static_cast<const void*>(a_accumulator));
				const auto previousMode = std::exchange(t_registrationMode, data ? mode : ~0u);
				const auto result = func(a_accumulator, a_geometry, a_property);
				t_shadowRegistration = previous;
				t_registrationAccumulator = previousAccumulator;
				t_registrationMode = previousMode;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	bool PassCapture::MainRendererNow(const RE::BSBatchRenderer* a_batch) const
	{
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> depthAccumulator{ REL::Offset(Engine::Address::kDepthAccumulator) };
		static REL::Relocation<RE::BSGraphics::BSShaderAccumulator**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
		if (HasRenderer(*depthAccumulator, a_batch) || HasRenderer(*mainAccumulator, a_batch))
			return true;
		// The InMainRenderers views' accumulators as RefreshMainRenderers last stored them (the first person's: an engine global, kept for
		// the session). An OwnRenderers view's (a cube camera's) may not outlive its set, so it is not read here.
		const auto& views = Extensions::Views();
		for (std::uint32_t i = 0; i < views.mainRendererCount; ++i) {
			const auto& accumulators = viewRenderers[static_cast<std::size_t>(views.mainRendererViews[i])].accumulators;
			for (std::uint32_t a = 0; a < accumulators.count; ++a)
				if (HasRenderer(accumulators.accumulators[a], a_batch))
					return true;
		}
		return false;
	}

	namespace
	{
		/** @brief The nearest ancestor of a_object with a reference: its form ID and base object's name, or empty. */
		std::string OwnerOf(const RE::NiAVObject* a_object)
		{
			std::uint32_t depth = 0;
			for (const auto* node = a_object; node && depth < 64; node = node->parent, ++depth)
				if (const auto* ref = node->GetUserData()) {
					const auto* base = ref->GetBaseObject();
					const char* name = base ? base->GetName() : nullptr;
					return fmt::format("{:08X} '{}'", ref->GetFormID(), name ? name : "");
				}
			return {};
		}

		/** @brief Whether a_object or an ancestor is app-culled (kHidden). */
		bool HiddenChain(const RE::NiAVObject* a_object)
		{
			std::uint32_t depth = 0;
			for (const auto* node = a_object; node && depth < 64; node = node->parent, ++depth)
				if (node->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					return true;
			return false;
		}
	}

	std::vector<PassCapture::KeptSample> PassCapture::TakeKeptSamples(std::size_t a_max)
	{
		std::vector<KeptSample> samples;
		{
			std::scoped_lock lock(keptLock);
			samples.reserve(keptSamples.size());
			for (auto& [geometry, sample] : keptSamples)
				samples.push_back(std::move(sample));
			keptSamples.clear();
		}
		std::ranges::sort(samples, [](const KeptSample& a_left, const KeptSample& a_right) { return a_left.passes > a_right.passes; });
		if (samples.size() > a_max)
			samples.resize(a_max);
		return samples;
	}

	void PassCapture::NoteMapPass(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, bool a_withheld) const
	{
		if (!mapCensus.load(std::memory_order_acquire) || !a_pass || !a_pass->geometry || t_menuScenes)
			return;
		const auto* shader = a_pass->shader;
		const std::uint64_t type = shader ? static_cast<std::uint64_t>(shader->shaderType.get()) : 99u;
		const std::uint64_t key = (type << 48) | (static_cast<std::uint64_t>(a_pass->accumulationHint) << 32) | a_pass->passEnum;
		const bool main = !t_shadowRegistration && (IsMainRenderer(a_batch) || MainRendererNow(a_batch));
		std::scoped_lock lock(mapLock);
		auto [it, added] = mapClasses.try_emplace(key);
		auto& entry = it->second;
		if (added) {
			entry.shader = fmt::format("shader {} technique {:08X} hint {}", type, a_pass->passEnum, static_cast<std::uint32_t>(a_pass->accumulationHint));
			entry.sample = a_pass->geometry->name.c_str() ? a_pass->geometry->name.c_str() : "";
			entry.dclf = DclfClass(a_pass);
		}
		++(main ? (a_withheld ? entry.withheld : entry.kept) : (a_withheld ? entry.otherWithheld : entry.otherKept));
		if (a_withheld || !entry.dclf)
			return;
		const auto set = std::atomic_load(&frameSet);
		const std::uint8_t phases = set ? set->PhasesOf(a_pass->geometry) : std::uint8_t{ 0 };
		++(!set ? entry.noSet : !phases ? entry.notMember : !(phases & kSetMain) ? entry.notMain : entry.mainNotDrawn);
		const auto* tes = RE::TES::GetSingleton();
		const RE::NiAVObject* root = tes ? tes->lodLandRoot : nullptr;
		std::uint32_t depth = 0;
		for (const RE::NiAVObject* node = a_pass->geometry; root && node && depth < 64; node = node->parent, ++depth)
			if (node == root) {
				++entry.underLodRoot;
				break;
			}
	}

	std::vector<PassCapture::MapClass> PassCapture::TakeMapCensus()
	{
		std::vector<MapClass> classes;
		{
			std::scoped_lock lock(mapLock);
			classes.reserve(mapClasses.size());
			for (auto& [key, entry] : mapClasses)
				classes.push_back(std::move(entry));
			mapClasses.clear();
		}
		const auto total = [](const MapClass& a_class) { return a_class.withheld + a_class.kept + a_class.otherWithheld + a_class.otherKept; };
		std::ranges::sort(classes, [&](const MapClass& a_left, const MapClass& a_right) { return total(a_left) > total(a_right); });
		return classes;
	}

	std::vector<const void*> PassCapture::TakeUncoveredAccumulators()
	{
		std::scoped_lock lock(keptLock);
		std::vector<const void*> out(uncoveredAccumulators.begin(), uncoveredAccumulators.end());
		uncoveredAccumulators.clear();
		return out;
	}

	void PassCapture::KeepForEngine(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass) const
	{
		// Counted for the log outside loads (LoadHold.h): what DCLF still hands the engine of its classes, with a sample by geometry: its
		// view, its owner and whether it is hidden or skinned, read while the engine registers it.
		if (a_pass && a_pass->geometry && !t_menuScenes && !bypassed.load(std::memory_order_acquire) && DclfClass(a_pass)) {
			dclfKept.fetch_add(1, std::memory_order_relaxed);
			std::scoped_lock lock(keptLock);
			auto it = keptSamples.find(a_pass->geometry);
			if (it == keptSamples.end() && keptSamples.size() < kKeptSamples) {
				KeptSample sample;
				const auto* geometry = a_pass->geometry;
				sample.name = geometry->name.c_str() ? geometry->name.c_str() : "";
				sample.owner = OwnerOf(geometry);
				std::uint32_t mode = 0;
				sample.renderMode = t_registrationMode;
				sample.where = ShadowModeOfBatch(a_batch, mode)                         ? fmt::format("a covered shadow view (mode {})", mode) :
				               t_shadowRegistration                                       ? std::string("a shadow view DCLF does not cover") :
				               IsMainRenderer(a_batch) || MainRendererNow(a_batch)        ? std::string("the main views") :
				                                                                            std::string("another view");
				if (t_shadowRegistration && t_registrationAccumulator && !ShadowModeOfBatch(a_batch, mode) && uncoveredAccumulators.size() < kKeptSamples)
					uncoveredAccumulators.insert(t_registrationAccumulator);
				// Another view: an OwnRenderers view's renderer (its name, and whether it withholds now), else one DCLF knows nothing of.
				if (sample.where == "another view") {
					const auto& views = Extensions::Views();
					for (std::uint32_t i = 0; i < views.ownRendererCount; ++i) {
						const auto v = static_cast<std::size_t>(views.ownRendererViews[i]);
						const auto renderers = std::atomic_load(&viewRenderers[v].renderers);
						if (renderers && renderers->contains(a_batch)) {
							sample.where = fmt::format("the {} view's renderers ({}withholding, {}covered)", views.views[v].name,
								viewRenderers[v].withholdActive.load(std::memory_order_acquire) ? "" : "not ",
								ViewCovered(views.ownRendererViews[i]) ? "" : "not ");
							break;
						}
					}
					if (sample.where == "another view")
						sample.where = "a renderer of no view DCLF knows (not the main, shadow or own-renderer views)";
				}
				const auto* shader = a_pass->shader;
				sample.shader = fmt::format("{} technique {:08X} hint {}", shader ? static_cast<std::uint32_t>(shader->shaderType.get()) : 99u, a_pass->passEnum,
					static_cast<std::uint32_t>(a_pass->accumulationHint));
				if (const auto set = std::atomic_load(&frameSet)) {
					sample.phases = set->PhasesOf(geometry);
					const auto p = set->pending.find(geometry);
					sample.pending = p != set->pending.end() ? p->second : std::uint8_t{ 0 };
				}
				sample.hidden = HiddenChain(geometry);
				sample.skinned = geometry->GetGeometryRuntimeData().skinInstance.get() != nullptr;
				it = keptSamples.emplace(geometry, std::move(sample)).first;
			}
			if (it != keptSamples.end())
				++it->second.passes;
		}
		// Nothing is vouched for here: members and waiting objects are withheld (WithholdMain, WithholdOwn, Withhold, the occlusion maps'),
		// and so is everything of them during a load's hold. The sky's precipitation is Features/Precipitation's (CS_ORG_PRECIPITATION).
	}

	namespace
	{
		thread_local std::uint32_t passesOnThisThread = 0;
		constexpr auto Census(RegistrationCensus::Site a_site) { return static_cast<std::uint8_t>(a_site); }
	}

	std::uint32_t PassCapture::PassesOnThisThread()
	{
		return passesOnThisThread;
	}

	namespace
	{
		bool ViewWaits(const RE::NiPointer<RE::NiSourceTexture>& a_texture)
		{
			const auto* renderer = a_texture ? a_texture->rendererTexture : nullptr;
			const auto* view = renderer ? renderer->resourceView : nullptr;
			if (TextureStreaming::BaseReady(view))
				return false;
			TextureStreaming::RequestBase(view);
			return true;
		}

		/**
		 * @brief Texture streaming: every pass of a lit object whose diffuse or normal map has no base window yet stays off the engine's lists
		 * until it has one, and its load is asked for: its depth and shadow passes too (Utility's, not Lighting's), or its depth without its colour
		 * would show the empty G-buffer. Only for a pass the engine is registering, never a sweep; the rarer slots wait in the bind (the safety net).
		 */
		bool WaitsForBaseMips(const RE::BSRenderPass* a_pass)
		{
			if (!TextureStreaming::Enabled() || TextureStreaming::FullMode() || !a_pass->shaderProperty || !netimmerse_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty))
				return false;
			const auto* material = static_cast<const RE::BSLightingShaderMaterialBase*>(a_pass->shaderProperty->material);
			if (!material)
				return false;
			const bool diffuse = ViewWaits(material->diffuseTexture), normal = ViewWaits(material->normalTexture);
			return diffuse || normal;
		}
	}

	struct PassCapture::Hook
	{
		static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_techniqueID)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (capture.bypassed.load(std::memory_order_acquire)) {
				func(a_this, a_pass, a_techniqueID);
				return;
			}
			// Withholding is the whole of ownership: the pass is built, lit and shadowed as the engine would - only the batch
			// renderer never receives it, so the native loop has nothing to draw. Record notes it for the observers.
			bool withheld = capture.WithholdMain(a_this, a_pass) || capture.WithholdOwn(a_this, a_pass) || capture.Withhold(a_this, a_pass);
			if (!withheld && WaitsForBaseMips(a_pass)) {
				capture.baseMipsWithheld.fetch_add(1, std::memory_order_relaxed);
				withheld = true;
			}
			capture.Record(a_this, a_pass, a_techniqueID, withheld);
			capture.NoteMapPass(a_this, a_pass, withheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_this, a_pass, withheld, 0 });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>())
				capture.NoteKept(a_this, a_pass, Census(RegistrationCensus::Site::RegisterPass));
			capture.KeepForEngine(a_this, a_pass);
			func(a_this, a_pass, a_techniqueID);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The shadow modes' registration inserting an accumulation hint 8 pass (a volumetric-only caster)
	 * into batch group 15: AE FUN_1414b2a60's direct call to FUN_1414f5090(batch, pass, 15, 0) at +0xFF. The
	 * pass is withheld when the batch renderer is a shadow view's and the geometry is in the frame's set with the phase of
	 * that view's render mode (SetPhaseOfMode).
	 */
	bool PassCapture::WithholdAtGroup(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::atomic<std::uint32_t>& a_counter)
	{
		if (!a_pass || !a_pass->geometry || bypassed.load(std::memory_order_acquire))
			return false;
		std::uint32_t mode = 0;
		if ((t_shadowRegistration || ShadowModeOfBatch(a_batch, mode)) && HeldByLoad(a_pass))
			return true;
		std::uint8_t phase = 0;
		const auto set = CastersForBatch(a_batch, &phase);
		const bool member = set && HeldPhase(*set, a_pass->geometry, phase);
		if (member)
			a_counter.fetch_add(1, std::memory_order_relaxed);
		return member;
	}

	struct PassCapture::VolumetricGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.volumetricWithheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_batch, a_pass, withheld, 8 });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::Volumetric));
			capture.KeepForEngine(a_batch, a_pass);
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The same registration's other direct insertions (FUN_1414b2a60): accumulation hint 11 into batch
	 * group 9 (+0xC9), hint 7 into group 1 (+0xDB) and hint 3, a decal-flagged caster such as an NPC's face
	 * part, into group 4 (+0xED). None of them reaches RegisterPass, so a member registered with one of these hints is
	 * withheld here.
	 */
	template <std::uint32_t Hint>
	struct PassCapture::DirectGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const bool withheld = (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) ||
			                      capture.WithholdAtGroup(a_batch, a_pass, capture.directWithheld);
			Diagnostics::Notify(Diagnostics::PointLightPass{ a_batch, a_pass, withheld, Hint });
			if (withheld)
				return;
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::ShadowDirect));
			capture.KeepForEngine(a_batch, a_pass);
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The main modes' registration (FUN_1414b2330, render modes 0-11: the main camera's accumulator among them) inserting a
	 * pass straight into a geometry group by its accumulation hint (FUN_1414f5090(batch, pass, group, flag)): hints 2-7, 9, 11,
	 * 13-18, and a multi-index shape's layer (hint 12, group 2). None of them reaches RegisterPass, so a member's pass with one of
	 * these hints (terrain LOD's 6 into group 0, object LOD's 7 into group 1) is withheld here. The Z-prepass's accumulator
	 * (render mode 0xC) registers through the shadow modes' function (FUN_1414b2a60), whose insertions are the hooks above.
	 */
	template <std::uint32_t Group>
	struct PassCapture::MainGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) {
				capture.NoteMapPass(a_batch, a_pass, true);
				if constexpr (Group == 5)
					Diagnostics::Notify(Diagnostics::MemberGroup5PassWithheld{ a_pass });
				return;
			}
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdOwn(a_batch, a_pass)) {
				capture.NoteMapPass(a_batch, a_pass, true);
				return;
			}
			capture.NoteMapPass(a_batch, a_pass, false);
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::MainGroup));
			capture.KeepForEngine(a_batch, a_pass);
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief The same registration's hint 1, into a pass list of the batch renderer (FUN_1414f50b0(batch, pass, list)). */
	struct PassCapture::MainListHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, void* a_list)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdMain(a_batch, a_pass)) {
				capture.NoteMapPass(a_batch, a_pass, true);
				Diagnostics::Notify(Diagnostics::MemberListPassWithheld{ a_pass });
				return;
			}
			if (!capture.bypassed.load(std::memory_order_acquire) && capture.WithholdOwn(a_batch, a_pass)) {
				capture.NoteMapPass(a_batch, a_pass, true);
				return;
			}
			capture.NoteMapPass(a_batch, a_pass, false);
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::MainList));
			capture.KeepForEngine(a_batch, a_pass);
			func(a_batch, a_pass, a_list);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/**
	 * @brief The occlusion maps' registration (FUN_1414b2c20, render mode 0x1C: Precipitation::SetupMask's accumulator) inserting a
	 * pass into geometry group 14 (FUN_1414f5090(batch, pass, 14), its one insertion): a member of the map's occluder phase is
	 * withheld, DCLF drawing it into the map (its shadow epoch, FrameDriver::AfterShadowMaps); the engine draws every other occluder.
	 */
	struct PassCapture::OcclusionGroupHook
	{
		static void thunk(RE::BSBatchRenderer* a_batch, RE::BSRenderPass* a_pass, std::uint32_t a_group, std::uint32_t a_arg)
		{
			++passesOnThisThread;
			auto& capture = PassCapture::Get();
			const std::uint8_t phase = capture.occlusionPhase.load(std::memory_order_acquire);
			const bool live = a_pass && a_pass->geometry && !capture.bypassed.load(std::memory_order_acquire);
			if (live && capture.HeldByLoad(a_pass))
				return;
			if (phase && live && !!Policy::WithholdFromEngine())
				if (const auto set = std::atomic_load(&capture.frameSet); set && (set->drawn & phase) && capture.HeldPhase(*set, a_pass->geometry, phase)) {
					capture.occlusionWithheld.fetch_add(1, std::memory_order_relaxed);
					return;
				}
			if (Diagnostics::Armed<Diagnostics::PassKept>() && !capture.bypassed.load(std::memory_order_acquire))
				capture.NoteKept(a_batch, a_pass, Census(RegistrationCensus::Site::Occlusion), phase);
			// Every occluder DCLF does not draw into the map and does not wait to: all of them while the map is not DCLF's (phase 0, the
			// engine draws it whole).
			capture.KeepForEngine(a_batch, a_pass);
			func(a_batch, a_pass, a_group, a_arg);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	namespace
	{
		// The main modes' registration's (FUN_1414b2330) group insertions and its hint 1 list insertion, by call site.
		// (SE FUN_1412ca300, the same code; the occlusion maps' FUN_1414b2c20 is SE FUN_1412cab70.)
		constexpr Engine::Offset kInsertGroup{ { 107668 }, { 100872 } }, kInsertList{ { 107669 }, { 100873 } };
		constexpr std::array<std::pair<Engine::Offset, Engine::Offset>, 16> kMainGroupSites{ {
			{ Engine::Offset{ { 106570, 0x172 }, { 99926, 0x172 } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x18d }, { 99926, 0x18d } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x1a8 }, { 99926, 0x1a8 } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x1c3 }, { 99926, 0x1c3 } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x1dd }, { 99926, 0x1dd } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x1f8 }, { 99926, 0x1f8 } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x213 }, { 99926, 0x213 } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x22e }, { 99926, 0x22e } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x249 }, { 99926, 0x249 } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x261 }, { 99926, 0x261 } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x279 }, { 99926, 0x279 } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x291 }, { 99926, 0x291 } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x2ab }, { 99926, 0x2ab } }, kInsertGroup }, { Engine::Offset{ { 106570, 0x337 }, { 99926, 0x337 } }, kInsertGroup },
			{ Engine::Offset{ { 106570, 0x148 }, { 99926, 0x148 } }, kInsertList }, { Engine::Offset{ { 106578, 0x51 }, { 99934, 0x44 } }, kInsertGroup },
		} };

		template <class Hook>
		void InstallMainGroup(std::size_t a_site)
		{
			stl::write_thunk_call<Hook>(REL::Offset(kMainGroupSites[a_site].first).address());
		}

		// The registration's (FUN_1414b2a60) group insertions: the volumetric copy's, and the direct hints 11, 7 and 3.
		// (SE FUN_1412ca9c0, the same code.)
		constexpr Engine::Offset kVolumetricGroupCall{ { 106576, 0xff }, { 99932, 0xff } };
		constexpr Engine::Offset kDirectGroupCalls[3] = { { { 106576, 0xc9 }, { 99932, 0xc9 } }, { { 106576, 0xdb }, { 99932, 0xdb } }, { { 106576, 0xed }, { 99932, 0xed } } };
	}

	void PassCapture::Preflight()
	{
		// Not chained: a withheld pass skips what the call or slot holds, another plugin's hook with it. So each needs the engine's own.
		HookSites::RequireSlot("BSBatchRenderer::RegisterPass", REL::Relocation<std::uintptr_t>{ RE::VTABLE_BSBatchRenderer[0] }.address(), 0x2);
		const auto group = REL::Offset(kInsertGroup).address();
		HookSites::RequireCallTo("the shadow registration's volumetric group insertion", REL::Offset(kVolumetricGroupCall).address(), group);
		for (const auto& site : kDirectGroupCalls)
			HookSites::RequireCallTo("the shadow registration's direct group insertions", REL::Offset(site).address(), group);
	}

	std::string PassCapture::SitesMissing()
	{
		for (const auto& [site, callee] : kMainGroupSites)
			if (!Engine::CallsTo(REL::Offset(site).address(), REL::Offset(callee).address())) {
				logger::warn("[DCLF] pass capture: the call at {:#x} is not the engine's group insertion", 0x140000000 + site);
				return "the main registration's group insertions";
			}
		return {};
	}

	void PassCapture::Install()
	{
		if (installed)
			return;
		// Preflight checked the slot and the shadow registration's insertions before any hook was written.
		stl::write_vfunc<0x2, Hook>(RE::VTABLE_BSBatchRenderer[0]);
		stl::write_thunk_call<VolumetricGroupHook>(REL::Offset(kVolumetricGroupCall).address());
		stl::write_thunk_call<DirectGroupHook<11>>(REL::Offset(kDirectGroupCalls[0]).address());
		stl::write_thunk_call<DirectGroupHook<7>>(REL::Offset(kDirectGroupCalls[1]).address());
		stl::write_thunk_call<DirectGroupHook<3>>(REL::Offset(kDirectGroupCalls[2]).address());
		// The main modes' group insertions (kMainGroupSites, in order), by the group each inserts into, and the hint 1 list insertion.
		// PostPostLoad checked every site against its callee (SitesMissing) before any hook was written.
		if (!SitesMissing().empty())
			stl::report_and_fail("Drawcall Limit Fix: the main registration's group insertions changed after their sites were checked");
		InstallMainGroup<MainGroupHook<3>>(0);
		InstallMainGroup<MainGroupHook<4>>(1);
		InstallMainGroup<MainGroupHook<5>>(2);
		InstallMainGroup<MainGroupHook<6>>(3);
		InstallMainGroup<MainGroupHook<0>>(4);
		InstallMainGroup<MainGroupHook<1>>(5);
		InstallMainGroup<MainGroupHook<7>>(6);
		InstallMainGroup<MainGroupHook<9>>(7);
		InstallMainGroup<MainGroupHook<10>>(8);
		InstallMainGroup<MainGroupHook<11>>(9);
		InstallMainGroup<MainGroupHook<8>>(10);
		InstallMainGroup<MainGroupHook<12>>(11);
		InstallMainGroup<MainGroupHook<13>>(12);
		InstallMainGroup<MainGroupHook<2>>(13);
		InstallMainGroup<MainListHook>(14);
		InstallMainGroup<OcclusionGroupHook>(15);
		// The shadow registration's scope (a caster's passes into any shadow view's renderer): what a load's hold withholds by.
		stl::detour_thunk<ShadowRegistration>(REL::RelocationID(99932, 106576));
		if (!ShadowRegistration::func.address())
			logger::warn("[DCLF] pass capture: the shadow registration's detour did not attach; a load's hold withholds only the shadow views DCLF published");
		installed = true;
		logger::info("[DCLF] pass capture installed on BSBatchRenderer::RegisterPass");
	}
}
