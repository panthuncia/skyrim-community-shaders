// Engine-side hooks for the sky family (dclf-native-sky.md). At the engine's sky batch and cloud layer calls the draws are computed
// from the passes the engine registered (SkyNative.cpp), and the engine's calls find nothing of the sky to draw. In native the main
// view's sky and cloud layers come from the sky root at the sky batch, not from the registrations. The sun's occlusion
// query draw is computed in place of the engine's call. A water reflection face drawn without the engine's face render gets its
// run from PrepareFace and SubmitFace. Render thread only.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/Engine/Native/WorldPass.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Features/Sky/SkyNative.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "Snapshot/EngineReads.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "Utils/D3D.h"

#include <rhi_helpers.h>

#include <format>
#include <functional>
#include <optional>
#include <utility>

namespace DCLF::Features::Sky
{
	namespace
	{
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one run holds (SkyDraws' kMaxDraws).
		constexpr std::size_t kMaxRunDraws = 128;

		const char* NameOf(View a_view) { return a_view == View::Main ? "main" : a_view == View::Face ? "face" : "sun occlusion"; }

		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		/** @brief Why a sun occlusion draw was skipped, each logged once. */
		Family::RefusalLog& SunSkips()
		{
			static Family::RefusalLog log("[DCLF] sky: a sun occlusion draw is skipped, its query answers its last count");
			return log;
		}

		Family::RefusalLog& Refusals()
		{
			static Family::RefusalLog log("[DCLF] sky: the engine draws");
			return log;
		}

		/**
		 * @brief A sky batch whose run waits for the cloud group's call, on the render thread. The batch's draws are computed and the
		 * cloud layers are added at the cloud group, so sky and clouds are one run in engine order.
		 */
		struct Pending
		{
			std::shared_ptr<Run> run;
			std::array<std::uint32_t, 3> colourFormats{};  // the main pass's targets, or the face's (one)
			std::uint32_t depthFormat = 0;
			// Geometry group 13 when the run also takes the cloud layers. Set while the run waits for its call.
			const RE::BSBatchRenderer::GeometryGroup* cloudGroup = nullptr;
			// Set when the cloud group's call did not follow a waiting sky batch, for example another hook at that site.
			bool broken = false;
			RE::BSShaderAccumulator* accumulator = nullptr;  // the sky batch's, for the cloud layers' draws
			// Native: the main pass's geometry group 13 when the sky batch's run already holds the cloud layers from the sky root. Its
			// call then finds no cloud layer to draw.
			const RE::BSBatchRenderer::GeometryGroup* drawnClouds = nullptr;
		};

		Pending& GetPending()
		{
			static Pending pending;
			return pending;
		}

		/** @brief The face draws that could not be submitted, each reason logged once. */
		Family::RefusalLog& FaceFailures()
		{
			static Family::RefusalLog log("[DCLF] sky: a native reflection face's sky is not drawn");
			return log;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_rtv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			a_dsv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		bool IsTarget(std::int32_t a_target) { return a_target >= 0 && a_target < static_cast<std::int32_t>(RE::RENDER_TARGETS::kTOTAL); }
		bool IsDepth(std::int32_t a_depth) { return a_depth >= 0 && a_depth < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); }

		/** @brief The engine depth target whose depth SRV is a_view, or -1. */
		std::int32_t DepthOfView(ID3D11ShaderResourceView* a_view)
		{
			const auto& depths = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer)->GetDepthStencilData().depthStencils;
			for (std::int32_t d = 0; d < static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kTOTAL); ++d)
				if (a_view && Unwrap(depths[d].depthSRV) == a_view)
					return d;
			return -1;
		}

		/** @brief The published ids a batch's headers and t17 export. */
		struct PublishedInputIds
		{
			std::array<std::string, kHeaderRegisters.size()> headers;
			std::string sceneDepth;
		};

		/**
		 * @brief The view the feature that owns a_register binds there now (its DCLF frame texture: IBL's t76 and t77, Dynamic
		 * Cubemaps' t30 and t31), or null when it binds nothing.
		 */
		ID3D11ShaderResourceView* FeatureView(std::uint32_t a_register)
		{
			const auto at = Extensions::FrameTextureIndices()[a_register];
			if (at == Extensions::kNoFrameTexture)
				return nullptr;
			const auto& texture = Extensions::RegisteredFrameTextures()[at];
			return texture.view ? static_cast<ID3D11ShaderResourceView*>(texture.view()) : nullptr;
		}

		/** @brief The header textures (kHeaderRegisters), as imports or published ids. False when one cannot be read. */
		bool ResolveHeaders(std::array<std::uint32_t, 4>& a_indices, Family::Owners& a_owners, PublishedInputIds& a_published)
		{
			DCLF_ZONE("CS.DCLF.Sky.ResolveHeaders");
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				Family::Header header;
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				auto* view = FeatureView(kHeaderRegisters[h]);
				const auto result = Family::ResolveHeaderView(view, kHeaderKinds[h], GpuTextures::Tag(GpuTextures::Source::FamilyHeader, static_cast<std::uint32_t>(h)), header,
					a_owners, &failure);
				if (result != Family::HeaderResult::Ok) {
					Refusals().Report(std::format("a batch whose t{} cannot be read", kHeaderRegisters[h]),
						result == Family::HeaderResult::Buffer ? "a D3D11 buffer" : GpuTextures::DescribeFailure(view, kHeaderRegisters[h], failure));
					return false;
				}
				a_published.headers[h] = std::move(header.publishedId);
				a_indices[h] = header.index;
			}
			return true;
		}

		/**
		 * @brief Reads the batch's view, targets and inputs, for a water reflection face or the main GBuffer pass.
		 * Returns false for anything else, which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, Shape& a_shape, Run& a_run, PublishedInputIds& a_published, Pending& a_pending)
		{
			// Native's RenderWorld: the main pass's targets come from CS's frame, not the shadow state.
			const auto* world = ::RenderGraph::Engine::Native::WorldPass::Current();
			const auto& rss = CS_ENGINE_READ(ShadowState, CapturePoint, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
			auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
			const auto& depths = renderer->GetDepthStencilData().depthStencils;
			auto& cloudShadows = globals::features::cloudShadows;
			// The sky's pixel stages read Cloud Shadows' occlusion (t25), which the graph has only on its ORG path.
			if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive()) {
				Refusals().Report("a batch while Cloud Shadows runs on D3D11", "its occlusion (t25) is not on the graph");
				return false;
			}
			a_shape.cloudShadows = cloudShadows.loaded;
			a_shape.clouds = Address::kCloudGroup.Known();
			if (!ResolveHeaders(a_run.headerIndices, a_run.owners, a_published))
				return false;
			a_shape.depth = world ? world->depth : static_cast<std::int32_t>(rss.depthStencil);
			if (!IsDepth(a_shape.depth))
				return false;
			a_pending.colourFormats = {};
			a_pending.depthFormat = FormatOf(depths[a_shape.depth].views[0]);

			if (!world && rss.cubeMapRenderTarget == static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS)) {
				a_shape.view = View::Face;
				a_run.face = rss.cubeMapRenderTargetView;
				const auto& cube = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];
				a_pending.colourFormats[0] = FormatOf(cube.cubeSideRTV[0]);
				return a_run.face < 6 && a_pending.colourFormats[0] && a_shape.depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
			}
			if (!world && rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			// The main camera's accumulator, inside CS's deferred pass.
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || !globals::deferred || !globals::deferred->deferredPass)
				return false;
			a_shape.view = View::Main;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			for (std::size_t i = 0; i < a_shape.colours.size(); ++i) {
				a_shape.colours[i] = world ? world->colours[i] : static_cast<std::int32_t>(rss.renderTargets[i]);
				if (!IsTarget(a_shape.colours[i]))
					return false;
				a_pending.colourFormats[i] = FormatOf(targets[a_shape.colours[i]].RTV);
				if (!a_pending.colourFormats[i])
					return false;
			}
			// t17 is the scene depth CS binds. Any engine depth other than the prepass copy, the depth target or Terrain Blending's is refused.
			auto* sceneDepth = Util::GetCurrentSceneDepthSRV(true);
			const auto engineDepth = DepthOfView(sceneDepth);
			if (engineDepth == static_cast<std::int32_t>(kSceneDepth))
				return a_pending.depthFormat != 0;
			if (engineDepth >= 0) {
				if (engineDepth != a_shape.depth) {
					Refusals().Report(std::format("a main batch whose scene depth (t17) is engine depth {}", engineDepth), "the client imports the Z-prepass copy");
					return false;
				}
				a_run.sceneDepthNull = true;
				return a_pending.depthFormat != 0;
			}
			if (!sceneDepth) {
				a_run.sceneDepthNull = true;
				return a_pending.depthFormat != 0;
			}
			a_published.sceneDepth = PublishedResources::ExportedId(sceneDepth);
			// Terrain Blending's D3D11 depth is read by heap index. Without it the sun's depth test read zero and hid the sun.
			if (a_published.sceneDepth.empty()) {
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				auto binding = GpuTextures::Get().ResolveBinding(sceneDepth, GpuTextures::Tag(GpuTextures::Source::FrameSlot, 17), &failure);
				if (binding.index == GpuTextures::kInvalid) {
					Refusals().Report("a main batch whose scene depth (t17) cannot be read", GpuTextures::DescribeFailure(sceneDepth, 17, failure));
					return false;
				}
				a_run.sceneDepthIndex = binding.index;
				a_run.owners.push_back(std::move(binding.owner));
			}
			return a_pending.depthFormat != 0;
		}

		/**
		 * @brief Reads a batch's published inputs by id and Physical Sky's handles. False when the client cannot read one, and the batch
		 * stays the engine's.
		 */
		bool ReadInputs(View a_view, const PublishedInputIds& a_published, std::array<std::uint32_t, 4>& a_headerIndices, std::uint32_t& a_sceneDepthIndex,
			Integrations::PhysicalSky::OrgHandles& a_physSky, Family::Owners& a_owners)
		{
			DCLF_ZONE("CS.DCLF.Sky.ReadInputs");
			const auto read = [&](std::uint32_t a_register, const std::string& a_id, std::uint32_t& a_index) {
				if (a_id.empty())
					return true;
				const auto result = SkyDraws::ReadPublished(a_view, a_id, a_index);
				if (result == Family::InputResult::Graph)
					return true;
				Refusals().Report(std::format("a {} batch whose t{} holds '{}'", NameOf(a_view), a_register, a_id), Family::ReasonOf(result));
				return false;
			};
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h)
				if (!read(kHeaderRegisters[h], a_published.headers[h], a_headerIndices[h]))
					return false;
			if (!read(17, a_published.sceneDepth, a_sceneDepthIndex))
				return false;
			// Physical Sky's resources, which the sky's pixel stages read through the draw's latch.
			const auto readPhysSky = [a_view](std::string_view a_id, std::uint32_t& a_index, std::string& a_why) {
				const auto result = SkyDraws::ReadPublished(a_view, a_id, a_index);
				a_why = Family::ReasonOf(result);
				return result == Family::InputResult::Graph;
			};
			std::string why;
			if (!Integrations::PhysicalSky::ResolveOrgHandles(a_physSky, a_owners, readPhysSky, why, a_view == View::Face)) {
				Refusals().Report(std::format("a {} batch", NameOf(a_view)), why);
				return false;
			}
			return true;
		}

		/** @brief Submits a batch's run, or keeps its imports and leases when it has no draws. */
		void SubmitRun(std::shared_ptr<Run> a_run)
		{
			if (!a_run)
				return;
			if (a_run->draws.empty()) {
				SkyDraws::Keep(std::move(a_run->owners));
				return;
			}
			SkyDraws::Submit(std::move(a_run));
		}

		/**
		 * @brief Geometry group 13 when the run can take its cloud layers: a pass list whose sky passes are BSSkyShader TriShapes after
		 * any sky-object effects. FinishAccumulating calls the group exactly when it is non-null with flag 1 set (SE 99938 +0x260,
		 * AE 106583 +0x259).
		 */
		const RE::BSBatchRenderer::GeometryGroup* CloudGroupOf(RE::BSShaderAccumulator* a_accumulator, View a_view)
		{
			// BSShaderAccumulator::batchRenderer is at +0x130 on SE and AE.
			const auto* batch = a_accumulator->GetRuntimeData()->batchRenderer;
			const auto* group = batch ? batch->geometryGroups[kCloudGeometryGroup] : nullptr;
			if (!group || !group->passList.head)
				return nullptr;
			const char* reason = nullptr;
			if (!(group->flags & 1))
				reason = "it is not a pass list";
			// Sky-object effects (accumulation hint 0x12, BSEffectShader) are registered at the group's head, before the cloud layers:
			// the engine draws them first, and the run draws the cloud layers after them. One after a cloud layer would break that order.
			bool sky = false;
			for (const auto* pass = group->passList.head; pass && !reason; pass = pass->passGroupNext) {
				if (!pass->geometry)
					continue;
				if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Sky) {
					if (sky)
						reason = "it holds a pass of another shader after a cloud layer";
					continue;
				}
				sky = true;
				if (pass->geometry->GetType().get() != RE::BSGeometry::Type::kTriShape)
					reason = "it holds a sky pass that is not a TriShape";
			}
			if (!reason)
				return group;
			static Family::RefusalLog log("[DCLF] sky: the engine draws the cloud layers this batch");
			log.Report(std::format("{} view: {}", NameOf(a_view), reason));
			return nullptr;
		}

		/** @brief Ends a waiting run and submits it. */
		void Finish(Pending& a_pending)
		{
			a_pending.cloudGroup = nullptr;
			a_pending.accumulator = nullptr;
			SubmitRun(std::move(a_pending.run));
		}

		/**
		 * @brief The sky batch: FinishAccumulating's RenderBatches of the sky techniques (SE 99938 +0x247, AE 106583 +0x237) for the main
		 * GBuffer pass and the engine's reflection faces, or native's RenderWorld driver for the main pass (a_engine null). The draws are
		 * computed from the registered passes (native's main view: from the sky root), and the engine's call finds no sky pass to draw.
		 */
		void SkyBatchPoint(void* a_accumulator, std::uint32_t a_flags, const std::function<void()>* a_engine)
		{
			auto& pending = GetPending();
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			std::optional<::RenderGraph::Engine::Native::WorldPass::Scope> world;
			if (a_accumulator == *mainAccumulator)
				world.emplace(::RenderGraph::Engine::Native::WorldPass::Point::SkyBatch);
			// If another hook kept the cloud group's call from coming, finish late and never wait for the group again.
			if (pending.cloudGroup) {
				if (!std::exchange(pending.broken, true))
					logger::error("[DCLF] sky: the cloud group was not drawn after a sky batch that waited for it (another hook at its call?); the engine draws the cloud layers from now on");
				Finish(pending);
			}
			// Native's driver makes no engine call: a batch the graph cannot take draws nothing there.
			const auto engineDraws = [&] {
				if (a_engine)
					(*a_engine)();
				else
					Refusals().Report("a main batch in native's RenderWorld", "not drawn");
			};
			if (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available())
				return engineDraws();
			Family::BeginBatch();
			Shape shape;
			auto run = std::make_shared<Run>();
			PublishedInputIds published;
			if (!CurrentShape(a_accumulator, shape, *run, published, pending) || !SkyDraws::CanTake(shape) ||
				!ReadInputs(shape.view, published, run->headerIndices, run->sceneDepthIndex, run->physSky, run->owners)) {
				SkyDraws::Keep(std::move(run->owners));
				return engineDraws();
			}
			run->view = shape.view;
			auto* accumulator = static_cast<RE::BSShaderAccumulator*>(a_accumulator);
			const Native::BatchInputs inputs{ shape.view, pending.colourFormats, pending.depthFormat, a_flags, run->physSky };
			std::string why;
			// Native draws the main view's sky from the sky root, with its cloud layers, not from the engine's registrations.
			const bool fromRoot = shape.view == View::Main && ::RenderGraph::Engine::Native::Active();
			// The leases go straight to the run: a failed build's leases are only held a frame longer.
			const bool built = fromRoot ? Native::BuildMain(accumulator, inputs, run->draws, run->owners, why) : Native::BuildBatch(accumulator, inputs, run->draws, run->owners, why);
			if (!built || run->draws.size() > kMaxRunDraws) {
				Refusals().Report(std::format("a {} batch", NameOf(shape.view)), why.empty() ? std::string("more draws than one run holds") : why);
				SkyDraws::Keep(std::move(run->owners));
				return engineDraws();
			}
			if (a_engine) {
				auto detached = Native::DetachBatch(accumulator);
				(*a_engine)();
				Native::Reattach(detached);
			}
			pending.run = std::move(run);
			pending.cloudGroup = shape.clouds && !pending.broken && !fromRoot ? CloudGroupOf(accumulator, shape.view) : nullptr;
			// The engine's cloud group call comes next and must find the drawn cloud layers detached. Native's driver makes no such call.
			if (fromRoot && a_engine) {
				const auto* batchRenderer = accumulator->GetRuntimeData()->batchRenderer;
				pending.drawnClouds = batchRenderer ? batchRenderer->geometryGroups[kCloudGeometryGroup] : nullptr;
			}
			if (pending.cloudGroup) {
				pending.accumulator = accumulator;
				return;
			}
			Finish(pending);
		}

		/** @brief The engine's sky batch call (SkyBatchPoint). */
		struct SkyBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				const std::function<void()> engine = [&] { func(a_accumulator, a_first, a_last, a_flags, a_group); };
				SkyBatchPoint(a_accumulator, a_flags, &engine);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief Hook on the RenderPersistentPassList of geometry group 13 (SE 99938 +0x284, AE 106583 +0x274). Adds the cloud layers to
		 * a waiting sky batch's run, so sky and clouds draw in engine order. The engine's call then draws only the sky-object effects.
		 */
		struct CloudGroup
		{
			static void thunk(RE::BSBatchRenderer::GeometryGroup* a_group, std::uint32_t a_flags)
			{
				auto& pending = GetPending();
				if (pending.drawnClouds && pending.drawnClouds == a_group) {
					pending.drawnClouds = nullptr;
					Native::DetachClouds(a_group);
					func(a_group, a_flags);
					return;
				}
				if (!pending.cloudGroup || pending.cloudGroup != a_group || !pending.run) {
					func(a_group, a_flags);
					return;
				}
				auto& run = *pending.run;
				const Native::BatchInputs inputs{ run.view, pending.colourFormats, pending.depthFormat, a_flags, run.physSky };
				std::string why;
				std::int32_t lastLayer = -1;
				std::vector<Draw> clouds;
				if (!Native::BuildClouds(pending.accumulator, a_group, inputs, lastLayer, clouds, run.owners, why) || run.draws.size() + clouds.size() > kMaxRunDraws) {
					static Family::RefusalLog log("[DCLF] sky: the engine draws the cloud layers");
					log.Report(std::format("a {} cloud group", NameOf(run.view)), why.empty() ? std::string("more draws than one run holds") : why);
					// The sky batch's run first, as the engine drew it before the cloud layers.
					Finish(pending);
					func(a_group, a_flags);
					return;
				}
				auto& cloudShadows = globals::features::cloudShadows;
				run.draws.reserve(run.draws.size() + clouds.size());
				for (auto& draw : clouds) {
					if (draw.occlusion)
						cloudShadows.NoteGraphCloudLayer(static_cast<int>(run.face), draw.layer);
					run.draws.push_back(std::move(draw));
				}
				Native::DetachClouds(a_group);
				func(a_group, a_flags);
				Finish(pending);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief The sun occlusion draw's targets from the shadow state. Returns null when taken, else why not. */
		const char* SunShape(Native::BatchInputs& a_inputs, Run& a_run)
		{
			auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
			constexpr auto kMain = static_cast<std::int32_t>(RE::RENDER_TARGETS::kMAIN);
			std::int32_t depth = 0;
			// Native's RenderWorld: CS's frame's targets.
			if (const auto* world = ::RenderGraph::Engine::Native::WorldPass::Current()) {
				if (world->colours[0] != kMain)
					return "its first target is not kMAIN";
				depth = world->depth;
			} else {
				const auto& rss = CS_ENGINE_READ(ShadowState, CapturePoint, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
				if (rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
					return "it draws into a cubemap";
				if (static_cast<std::int32_t>(rss.renderTargets[0]) != kMain)
					return "its first target is not kMAIN";
				depth = static_cast<std::int32_t>(rss.depthStencil);
			}
			if (depth == static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN))
				a_run.sunCopyDepth = false;
			else if (depth == static_cast<std::int32_t>(kSceneDepth))
				a_run.sunCopyDepth = true;
			else
				return "its depth target is neither kMAIN's nor the Z-prepass copy";
			a_inputs.view = View::Sun;
			a_inputs.colourFormats = { FormatOf(renderer->GetRuntimeData().renderTargets[kMain].RTV), 0, 0 };
			a_inputs.depthFormat = FormatOf(renderer->GetDepthStencilData().depthStencils[depth].views[0]);
			return a_inputs.colourFormats[0] && a_inputs.depthFormat ? nullptr : "a target format cannot be read";
		}

		/** @brief The sun query's targets, or why the graph cannot take its draw now (logged once each). */
		const char* SunRefusal(Native::BatchInputs& a_inputs, Run& a_run)
		{
			const char* refusal = SunShape(a_inputs, a_run);
			if (!refusal && (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available()))
				refusal = "the graph cannot draw now";
			if (!refusal && !SkyDraws::CanTakeSun())
				refusal = "its anchor cannot run now";
			if (refusal)
				SunSkips().Report(refusal);
			return refusal;
		}

		/**
		 * @brief The sun's occlusion draw inside a bracket whose query has begun, as FUN_1414b4c00 draws it: the query's camera, then the
		 * draw computed for the graph's query. Returns null when the run was submitted, else why it was skipped (the bracket is skipped).
		 */
		template <class EndQuery>
		const char* DrawSun(RE::BSShaderAccumulator* a_accumulator, const Native::PassObject& a_object, RE::NiCamera* a_camera, Queries::Bracket& a_bracket,
			EndQuery&& a_endQuery)
		{
			Family::BeginBatch();
			auto run = std::make_shared<Run>();
			run->view = View::Sun;
			// The engine's call sets the query's camera before its draw, and later code keeps it. Native's RenderWorld sets none: the
			// world camera's data is already the shadow state's (without jitter in native, flag 0's data is the same).
			if (!::RenderGraph::Engine::Native::WorldPass::InRenderWorld())
				CS_ENGINE_READ(GraphicsState, CapturePoint, RE::BSGraphics::State::GetSingleton())->SetCameraData(a_camera, 0);
			Native::BatchInputs inputs;
			std::string why;
			const char* skip = SunRefusal(inputs, *run);
			if (!skip) {
				inputs.physSky = run->physSky;
				const auto built = Native::BuildSun(a_accumulator, a_object, inputs, run->draws.emplace_back(), run->owners, why);
				if (built != Native::PassResult::Drawn) {
					skip = built == Native::PassResult::LeftOut ? "its program is still building" : "its draw cannot be computed";
					SunSkips().Report(skip, why);
				}
			}
			a_endQuery();
			if (!skip && a_bracket.Slot() == Queries::kNoSlot)
				skip = "a draw with no query begun";
			if (skip) {
				SkyDraws::Keep(std::move(run->owners));
				a_bracket.Skip(skip);
				return skip;
			}
			run->query = a_bracket.Slot();
			if (!SkyDraws::Submit(std::move(run))) {
				a_bracket.Skip("its run was not submitted");
				return "its run was not submitted";
			}
			return nullptr;
		}

		/**
		 * @brief Hook on BSShaderAccumulator's occlusion query draw (SE 99958, AE 106604, FUN_1414b4c00). Only in graph mode. The
		 * query's bookkeeping as the engine keeps it, and the sun's occlusion draw computed for the graph's query, which answers the
		 * engine's GetQueryData. The engine's call is not made. A draw the graph cannot take is skipped. Native's main sun query does
		 * not come here (SubmitSunQuery).
		 */
		struct SunOcclusionQuery
		{
			static void thunk(void* a_accumulator, RE::BSGeometry* a_geometry, std::uint32_t a_query, RE::NiCamera* a_camera)
			{
				if (!Queries::Active() || !SkyDraws::HasSunClient()) {
					func(a_accumulator, a_geometry, a_query, a_camera);
					return;
				}
				if (GetPending().cloudGroup) {
					SunSkips().Report("a sun query between a sky batch and its cloud layers: the engine's call is left out");
					return;
				}
				// The accumulator's three query entries at +0x70, 0x18 apart: the ring entry, two counts and the pending flag (+0x14).
				// The engine skips a disabled accumulator (+0x64) and an entry whose last result is still pending.
				auto* bytes = static_cast<std::byte*>(a_accumulator);
				if (!a_geometry || static_cast<bool>(bytes[0x64]) || a_query >= 3)
					return;
				auto* entry = bytes + 0x70 + std::size_t(a_query) * 0x18;
				auto& pending = *reinterpret_cast<bool*>(entry + 0x14);
				if (pending)
					return;
				auto& query = *reinterpret_cast<void**>(entry);
				// The engine's occlusion query entries (the ring's), as FUN_1414b4c00 calls them.
				static const REL::Relocation<void* (*)(void*)> createQuery{ REL::RelocationID(75565, 77366) };
				static const REL::Relocation<void (*)(void*, void*)> beginQuery{ REL::RelocationID(75567, 77368) };
				static const REL::Relocation<void (*)(void*, void*)> endQuery{ REL::RelocationID(75568, 77369) };
				auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
				if (!query) {
					query = createQuery(renderer);
					if (!query) {
						pending = false;
						*reinterpret_cast<std::uint32_t*>(entry + 0x8) = 0;
						*reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;
						return;
					}
				}
				Queries::Bracket bracket;
				beginQuery(renderer, query);
				CS_ENGINE_READ_MARK(Scene, RecordTime);
				(void)DrawSun(static_cast<RE::BSShaderAccumulator*>(a_accumulator), Native::TakeSunPass(a_geometry), a_camera, bracket, [&] { endQuery(renderer, query); });
				pending = true;
				*reinterpret_cast<std::uint32_t*>(entry + 0x10) = 0;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void SubmitSunQuery(RE::BSShaderAccumulator* a_accumulator, const SunQueryBlock& a_block, std::uint64_t a_tag)
	{
		if (!Queries::Active() || !SkyDraws::HasSunClient()) {
			SunSkips().Report("the graph does not answer the sun's occlusion query");
			return;
		}
		// The engine skips an entry whose last answer the game has not read yet; CS's slot is that entry.
		// The ticket's sun while it is still the block's query geometry.
		const auto* sun = a_block.submit ? Native::TicketSunPass(a_block.geometry) : nullptr;
		if (!sun || Queries::Pending(Queries::kSunSlot))
			return;
		const ::RenderGraph::Engine::Native::WorldPass::Scope world(::RenderGraph::Engine::Native::WorldPass::Point::SunQuery);
		Queries::Bracket bracket;
		bracket.Begin(Queries::kSunSlot, a_tag);
		(void)DrawSun(a_accumulator, *sun, nullptr, bracket, [] {});
	}

	namespace
	{
		/** @brief The reflection cube's and its depth's formats, read again only when their views change. */
		struct FaceTargets
		{
			ID3D11RenderTargetView* rtv = nullptr;
			ID3D11DepthStencilView* dsv = nullptr;
			std::uint32_t colourFormat = 0, depthFormat = 0;

			void Read()
			{
				auto* renderer = CS_ENGINE_READ(Renderer, CapturePoint, globals::game::renderer);
				auto* nextRtv = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS].cubeSideRTV[0];
				auto* nextDsv = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS].views[0];
				if (nextRtv != rtv) {
					rtv = nextRtv;
					colourFormat = FormatOf(rtv);
				}
				if (nextDsv != dsv) {
					dsv = nextDsv;
					depthFormat = FormatOf(dsv);
				}
			}
		};
	}

	std::shared_ptr<Run> PrepareFace(const FaceView& a_view, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Sky.PrepareFace");
		if (!FrameAnchors::IsRenderOwner() || !SkyDraws::Available()) {
			a_why = "the sky family does not draw now";
			return nullptr;
		}
		auto& cloudShadows = globals::features::cloudShadows;
		if (cloudShadows.loaded && !ORGCloudShadows::Get().IsActive()) {
			a_why = "Cloud Shadows runs on D3D11";
			return nullptr;
		}
		Family::BeginBatch();
		// The face's shape, as CurrentShape reads it from the shadow state inside the engine's face render.
		Shape shape;
		shape.view = View::Face;
		shape.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
		shape.cloudShadows = cloudShadows.loaded;
		shape.clouds = Address::kCloudGroup.Known();
		if (!SkyDraws::CanTake(shape)) {
			a_why = "the faces' client does not take a face now";
			return nullptr;
		}
		static FaceTargets targets;
		targets.Read();
		const Native::BatchInputs inputs{ View::Face, { targets.colourFormat, 0u, 0u }, targets.depthFormat };
		if (!inputs.colourFormats[0] || !inputs.depthFormat) {
			a_why = "the reflection cube or its depth has no view";
			return nullptr;
		}
		auto run = std::make_shared<Run>();
		run->view = View::Face;
		run->face = a_view.face;
		if (!Native::BuildFace(a_view, inputs, run->draws, run->owners, a_why) || run->draws.size() > kMaxRunDraws) {
			if (a_why.empty())
				a_why = "more draws than one run holds";
			SkyDraws::Keep(std::move(run->owners));
			return nullptr;
		}
		// The sky pass's first draw clears the face and its depth, so a face needs one.
		if (run->draws.empty()) {
			a_why = "a face without sky draws";
			SkyDraws::Keep(std::move(run->owners));
			return nullptr;
		}
		return run;
	}

	FaceInputs ReadFaceInputs()
	{
		// What CS's reflection prepasses bound: the header textures, the published inputs and Physical Sky's resources.
		DCLF_ZONE("CS.DCLF.Sky.ReadFaceInputs");
		FaceInputs inputs;
		PublishedInputIds published;
		std::uint32_t sceneDepth = GpuTextures::kInvalid;
		inputs.readable = ResolveHeaders(inputs.headerIndices, inputs.owners, published) &&
		                  ReadInputs(View::Face, published, inputs.headerIndices, sceneDepth, inputs.physSky, inputs.owners);
		if (!inputs.readable) {
			FaceFailures().Report("its inputs cannot be read", "see the sky's refusals");
			SkyDraws::Keep(std::move(inputs.owners));
		}
		return inputs;
	}

	bool SubmitFace(std::shared_ptr<Run> a_run, const FaceInputs& a_inputs)
	{
		DCLF_ZONE("CS.DCLF.Sky.SubmitFace");
		if (!a_inputs.readable) {
			SkyDraws::Keep(std::move(a_run->owners));
			return false;
		}
		a_run->headerIndices = a_inputs.headerIndices;
		a_run->physSky = a_inputs.physSky;
		a_run->owners.insert(a_run->owners.end(), a_inputs.owners.begin(), a_inputs.owners.end());
		auto& cloudShadows = globals::features::cloudShadows;
		for (auto& draw : a_run->draws) {
			draw.constants.physSky = a_inputs.physSky;
			if (draw.occlusion)
				cloudShadows.NoteGraphCloudLayer(static_cast<int>(a_run->face), draw.layer);
		}
		return SkyDraws::Submit(std::move(a_run));
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kSkyBatch.Known();
		HookSites::RequireKnown("the sky batch", known);
		if (!known)
			return;
		HookSites::RequireCall("the sky batch (FinishAccumulating's RenderBatches of the sky techniques)", REL::Offset(Address::kSkyBatch).address());
		if (Address::kCloudGroup.Known())
			HookSites::RequireCall("the cloud group (FinishAccumulating's RenderPersistentPassList of group 13)", REL::Offset(Address::kCloudGroup).address());
	}

	void MainBatch(RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_flags)
	{
		SkyBatchPoint(a_accumulator, a_flags, nullptr);
	}

	void InstallHooks()
	{
		if (!Address::kSkyBatch.Known()) {
			logger::warn("[DCLF] sky: this runtime has no address for the sky batch; the sky stays the engine's");
			return;
		}
		stl::write_thunk_call<SkyBatch>(REL::Offset(Address::kSkyBatch).address());
		const bool clouds = Address::kCloudGroup.Known();
		if (clouds)
			stl::write_thunk_call<CloudGroup>(REL::Offset(Address::kCloudGroup).address());
		if (::RenderGraph::Engine::GraphMode() && Address::kSunOcclusionQuery.Known()) {
			stl::detour_thunk<SunOcclusionQuery>(REL::Offset(Address::kSunOcclusionQuery).address());
			HookSites::NoteDetour("BSShaderAccumulator's occlusion query draw (the sun's, on the graph)", SunOcclusionQuery::func.address());
		}
		// Its capture runs only in native.
		InstallSunQuery();
		Family::AddSetup("sky", [] { return SkyDraws::Setup(); });
		logger::info("[DCLF] sky: hooks installed (the sky batch{})", clouds ? ", the cloud group" : "");
	}
}
