// The engine side of decal occlusion: the test's detour, the DrawTriShape taker that latches draws instead of
// drawing them, and a SetupGeometry watcher that tracks the current pass. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/DecalOcclusion/DecalOcclusion.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Snapshot/EngineReads.h"

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

		Family::RefusalLog& Skips()
		{
			static Family::RefusalLog log("[DCLF] decal occlusion: a decal test is skipped, its query answers visible");
			return log;
		}

		struct Capture
		{
			bool active = false;
			bool failed = false;  // One refused draw drops the whole test.
			RE::NiCamera* camera = nullptr;
			const RE::BSRenderPass* pass = nullptr;   // Set by SetupGeometry. Its draw comes next.
			bool haveMatrices = false;                // Built at the first draw, once the test has placed the camera.
			ShadowCameras::Camera matrices;
			std::shared_ptr<Run> run;
			Family::Passes passes;
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		// Returns null when latched, otherwise the reason the graph cannot draw it. Matrices are camera-relative, as BSUtilityShader expects.
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
			const auto& rss = CS_ENGINE_READ(ShadowState, CapturePoint, RE::BSGraphics::RendererShadowState::GetSingleton())->GetRuntimeData();
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

			if (!a_capture.haveMatrices) {
				ShadowCameras::CameraMatrices(*a_capture.camera, a_capture.matrices);
				a_capture.haveMatrices = true;
			}
			const auto& viewProj = a_capture.matrices.viewProj;  // Row-major, multiplied by a row vector.
			const auto& eye = a_capture.matrices.eye;
			// Multiplied by a column vector. The translation is relative to the camera.
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
			// clip[i] = sum over k of viewProj[k][i] * (world * position)[k]
			for (std::uint32_t i = 0; i < 4; ++i)
				for (std::uint32_t j = 0; j < 4; ++j) {
					float sum = 0.0f;
					for (std::uint32_t k = 0; k < 4; ++k)
						sum += viewProj[k * 4 + i] * rows[k][j];
					constants.clipRows[i * 4 + j] = sum;
				}
			constants.vertexRef = latched.vertexRef;
			constants.indexRef = latched.indexRef;
			constants.vertexLayout[0] = static_cast<std::uint32_t>(latched.layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(latched.layout >> 32);
			constants.vertexStride = latched.stride;
			constants.firstIndex = latched.firstIndex;
			draw.indexCount = latched.indexCount;
			draw.viewport = Family::LatchViewport(0);
			if (!draw.viewport.rect[2] || !draw.viewport.rect[3])
				return "an empty viewport";
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			run.draws.push_back(draw);
			return nullptr;
		}

		// During a test, every draw is latched or dropped. None reaches the engine.
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

		// A pass that will not reach DrawTriShape fails the test, and its draw still reaches D3D11.
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

		// SE 35563, AE 36562, same layout on both. The query slot lands in the decal's +0x38 and its target is at +0x178.
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
				if (!FrameAnchors::IsRenderOwner() || !DecalDraws::Available())
					refusal = "the graph cannot draw now";
				else if (!DecalDraws::CanTake())
					refusal = "its anchor cannot run now";
				if (refusal)
					Skips().Report(refusal);
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
					// The engine always begins a query, but another plugin's hook could prevent it.
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
				// Nothing drawn means no sample passed, as on D3D11.
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
		// Runs on the main thread once the graph is up, before any world frame.
		Family::AddSetup("decal occlusion", [] { return DecalDraws::Setup(); });
		logger::info("[DCLF] decal occlusion: hooks installed (the decal occlusion test, BSUtilityShader::SetupGeometry)");
	}
}
