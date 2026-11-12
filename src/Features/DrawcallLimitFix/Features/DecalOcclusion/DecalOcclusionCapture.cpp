// The decal occlusion family's engine side: the test's detour, which brackets the engine's query and latches the test's draws; the
// family's DrawTriShape taker, which latches a draw in place of drawing it; and its BSUtilityShader::SetupGeometry watcher, which keeps
// the pass being drawn and counts the test's passes. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/FrameAnchors.h"
#include "State.h"
#include "Utils/Logging.h"

#include <cstring>
#include <format>

namespace DCLF::Features::DecalOcclusion
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		/** @brief Why a test is skipped (its query answers kSkipAnswer), each logged once. */
		Util::ReasonLog& Skips()
		{
			static Util::ReasonLog log("[DCLF] decal occlusion: a decal test is skipped, its query answers visible", spdlog::level::warn);
			return log;
		}

		/** @brief The test being latched (render thread). */
		struct Capture
		{
			bool active = false;
			bool failed = false;  // a draw was refused: every draw of the test is dropped, and the query answers kSkipAnswer
			RE::NiCamera* camera = nullptr;           // the decal camera, which the test places before it draws
			const RE::BSRenderPass* pass = nullptr;   // the pass SetupGeometry set up last: its draw comes next
			bool haveMatrices = false;                // matrices are the camera's (made at the first draw, once the camera is placed)
			ShadowCameras::Camera matrices;
			std::shared_ptr<Run> run;
			Family::Passes passes;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		/**
		 * @brief The draw's values: the geometry (leased), and the decal camera's view-projection times the shape's world, both relative to
		 * the camera, as BSUtilityShader's vertex stage takes them (FrameBuffer's CameraViewProj times PerGeometry's World). Null when
		 * latched; otherwise why the graph cannot draw it.
		 */
		const char* TryLatchDraw(Capture& a_capture, const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (run.draws.size() >= kMaxDraws)
				return "more draws than a test counts";
			const auto* state = globals::state;
			if (!state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Utility)
				return "the bound shader is not BSUtilityShader";
			const auto* geometry = a_capture.pass ? a_capture.pass->geometry : nullptr;
			if (!geometry)
				return "no pass was set up for the draw";
			auto& data = const_cast<RE::BSGeometry*>(geometry)->GetGeometryRuntimeData();
			if (static_cast<const void*>(data.rendererData) != static_cast<const void*>(a_shape))
				return "the draw is not its pass's shape";
			if (data.skinInstance)
				return "its target is skinned (the bones are not latched)";
			if (!a_capture.camera)
				return "the test has no camera";
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			if (!vs)
				return "no vertex shader is bound";

			Family::Owners owners;
			Family::Geometry latched;
			std::string detail;
			const auto engine = Pipeline::EngineDrawState::Latch();
			if (const auto result = Family::LatchGeometry(a_shape, a_start, a_triangles, vs->vertexDesc, engine.topology, latched, owners, &detail);
				result != Family::GeometryResult::Ok) {
				if (!detail.empty())
					Skips().Report(std::format("a draw whose geometry is refused: {}", Family::ReasonOf(result)), detail);
				return Family::ReasonOf(result);
			}

			// The camera as the test placed it (SetCameraData's matrices: ShadowCameras' port of it), once per test.
			if (!a_capture.haveMatrices) {
				ShadowCameras::CameraMatrices(*a_capture.camera, a_capture.matrices);
				a_capture.haveMatrices = true;
			}
			const auto& viewProj = a_capture.matrices.viewProj;  // row-major, a row vector times it
			const auto& eye = a_capture.matrices.eye;
			// The shape's world as rows (a column vector times it), its translation relative to the camera.
			const auto& world = geometry->world;
			const auto& r = world.rotate.entry;
			const float s = world.scale;
			const float t[3]{ world.translate.x - eye[0], world.translate.y - eye[1], world.translate.z - eye[2] };
			float rows[4][4]{};
			for (std::uint32_t row = 0; row < 3; ++row) {
				rows[row][0] = r[row][0] * s;
				rows[row][1] = r[row][1] * s;
				rows[row][2] = r[row][2] * s;
				rows[row][3] = t[row];
			}
			rows[3][3] = 1.0f;

			Draw draw{};
			auto& constants = draw.constants;
			// clip[i] = sum over k of viewProj[k][i] * (world * position)[k]: the transposed view-projection times the world.
			for (std::uint32_t i = 0; i < 4; ++i)
				for (std::uint32_t j = 0; j < 4; ++j) {
					float sum = 0.0f;
					for (std::uint32_t k = 0; k < 4; ++k)
						sum += viewProj[k * 4 + i] * rows[k][j];
					constants.clipRows[i * 4 + j] = sum;
				}
			constants.vertexAddress[0] = static_cast<std::uint32_t>(latched.vertexAddress);
			constants.vertexAddress[1] = static_cast<std::uint32_t>(latched.vertexAddress >> 32);
			constants.indexAddress[0] = static_cast<std::uint32_t>(latched.indexAddress);
			constants.indexAddress[1] = static_cast<std::uint32_t>(latched.indexAddress >> 32);
			constants.vertexLayout[0] = static_cast<std::uint32_t>(latched.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(latched.layout >> 32);
			constants.vertexStride = latched.stride;
			constants.firstIndex = latched.firstIndex;
			draw.indexCount = latched.indexCount;
			// The viewport the test's target change set (kDECAL_OCCLUSION's), with the camera's depth range.
			draw.viewport = Family::LatchViewport(0);
			if (!draw.viewport.rect[2] || !draw.viewport.rect[3])
				return "an empty viewport";
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(draw);
			return nullptr;
		}

		/** @brief The family's DrawTriShape taker: during a test, every draw is latched or dropped, never the engine's. */
		bool TakeDraw(const Family::TriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return false;
			capture.passes.OnDraw();
			if (capture.failed || !capture.run)
				return true;
			if (const char* reason = TryLatchDraw(capture, a_shape, a_start, a_triangles)) {
				capture.failed = true;
				Skips().Report(reason);
			}
			return true;
		}

		/**
		 * @brief After BSUtilityShader::SetupGeometry (chained after the shadow masks' watcher): keeps the pass for its draw, and counts it. A
		 * pass that will not reach DrawTriShape fails the test; that draw still reaches D3D11.
		 */
		void OnSetupGeometry(RE::BSRenderPass* a_pass)
		{
			auto& capture = GetCapture();
			if (!capture.active)
				return;
			capture.pass = a_pass;
			if (capture.passes.OnSetup(a_pass) || capture.failed)
				return;
			capture.failed = true;
			Skips().Report("a pass the dispatcher draws through another function than DrawTriShape", "that draw reaches D3D11");
		}

		/**
		 * @brief The engine's decal occlusion test (SE 35563, AE 36562; Ghidra 2026-11-12), called by Main's frame (SE 35556) for each decal
		 * of BGSDecalManager in state 3: it places the decal camera (a_camera) along the decal, sets no colour target and kDECAL_OCCLUSION
		 * as the depth target (cleared when the decal's +0x31 is set), acquires a query from the ring into the decal's +0x38, begins it,
		 * culls the decal's target (+0x178) into the decal manager's accumulator with render mode 0x1B, draws it (SubmitAccumulator) and
		 * ends the query. The same layout on SE and AE.
		 */
		struct DecalTest
		{
			static void thunk(void* a_main, std::byte* a_decal, RE::NiPointer<RE::NiCamera>* a_camera, void* a_accumulator)
			{
				if (!Queries::Active() || !DecalDraws::HasClient()) {
					func(a_main, a_decal, a_camera, a_accumulator);
					return;
				}
				auto& capture = GetCapture();
				if (capture.active) {
					Skips().Report("a test inside another test: the inner one is the engine's");
					func(a_main, a_decal, a_camera, a_accumulator);
					return;
				}
				Queries::Bracket bracket(kSkipAnswer);
				Family::BeginBatch();
				auto run = std::make_shared<Run>();
				run->clear = a_decal && *reinterpret_cast<const std::uint8_t*>(a_decal + 0x31) != 0;
				const char* refusal = nullptr;
				if (!ShellState::Running() || !FrameAnchors::IsRenderOwner() || !DecalDraws::Available())
					refusal = "the graph cannot draw now";
				else if (!DecalDraws::CanTake())
					refusal = "its anchor cannot run now";
				if (refusal)
					Skips().Report(refusal);
				// The capture takes every DrawTriShape of the call, latched or dropped (TakeDraw).
				capture.failed = refusal != nullptr;
				capture.camera = a_camera ? a_camera->get() : nullptr;
				capture.pass = nullptr;
				capture.haveMatrices = false;
				capture.run = run;
				capture.passes.Begin();
				capture.active = true;
				func(a_main, a_decal, a_camera, a_accumulator);
				capture.active = false;
				capture.run.reset();
				capture.camera = nullptr;
				capture.pass = nullptr;
				if (!capture.passes.End() && !capture.failed) {
					capture.failed = true;
					Skips().Report(capture.passes.Describe("decal occlusion"));
				}
				if (bracket.Slot() == Queries::kNoSlot) {
					// The test always begins its query; another plugin's hook in between could change that.
					if (!run->draws.empty())
						Skips().Report("draws with no query begun");
					DecalDraws::Keep(std::move(run->owners));
					return;
				}
				if (capture.failed) {
					DecalDraws::Keep(std::move(run->owners));
					bracket.Skip("its draws were refused");
					return;
				}
				// Nothing of the target was drawn (culled away, or no shape): no sample passed, as on D3D11.
				if (run->draws.empty()) {
					DecalDraws::Keep(std::move(run->owners));
					bracket.Answer(0);
					return;
				}
				run->query = bracket.Slot();
				if (!DecalDraws::Submit(std::move(run)))
					bracket.Skip("its run was not submitted");
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallHooks()
	{
		if (!Family::AddDrawTaker("decal occlusion", &TakeDraw)) {
			logger::warn("[DCLF] decal occlusion: no DrawTriShape detour on this runtime; the decal tests stay the engine's");
			return;
		}
		if (!Family::WatchSetupGeometry<RE::BSShader::Type::Utility>(RE::VTABLE_BSUtilityShader[0], &OnSetupGeometry)) {
			logger::warn("[DCLF] decal occlusion: BSUtilityShader::SetupGeometry has no watcher slot left; the decal tests stay the engine's");
			return;
		}
		stl::detour_thunk<DecalTest>(REL::RelocationID(35563, 36562));
		HookSites::NoteDetour("the decal occlusion test (on the graph)", DecalTest::func.address());
		// The client is made on the main thread once the graph runs (the main menu), before any world frame.
		Family::AddSetup("decal occlusion", [] { return DecalDraws::Setup(); });
		logger::info("[DCLF] decal occlusion: hooks installed (the decal occlusion test, BSUtilityShader::SetupGeometry)");
	}
}
