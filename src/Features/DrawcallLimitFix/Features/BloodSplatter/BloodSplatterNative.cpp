// The screen blood from game data, native only. The game-side capture runs Main::RenderWorld's light flare setup and copies the
// block; the render side builds the draws from ScreenSplatter's root and camera as the engine's cull, RenderBatches,
// BSBloodSplatterShader::SetupTechnique and SetupGeometry would make them (AE 1.6.1170: GetRenderPasses 0x1414c1490, SetupTechnique
// 0x1414c18b0, SetupGeometry 0x1414c1a60, the light flare setup 0x1414c1e00). See docs/development/engine/blood-splatter.md.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"
#include "Features/DrawcallLimitFix/Features/BloodSplatter/BloodSplatter.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/Publication.h"

#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Globals.h"
#include "RenderGraph/Engine/Native/Native.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Frame/Variables.h"
#include "State.h"
#include "Utils/Logging.h"

#include <rhi_helpers.h>

#include <cstring>
#include <format>

namespace DCLF::Features::BloodSplatter
{
	namespace
	{
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		template <class T>
		T Global(const Engine::Offset& a_offset)
		{
			T value;
			std::memcpy(&value, reinterpret_cast<const void*>(REL::Offset(a_offset).address()), sizeof(T));
			return value;
		}

		// BSShaderProperty::alpha.
		constexpr std::size_t kPropertyAlpha = 0x30;
		// BSGeometry's world transform (NiTransform, 13 floats).
		constexpr std::size_t kWorld = 0x7C;

		/** @brief FUN_1414aaf10: the object's matrix in rows, scaled rotation and translation less a_posAdjust. */
		std::array<float, 16> MatrixOf(const float* a_transform, const std::array<float, 3>& a_adjust)
		{
			const float* r = a_transform;
			const float s = a_transform[12];
			return { s * r[0], s * r[3], s * r[6], 0.0f,
				s * r[1], s * r[4], s * r[7], 0.0f,
				s * r[2], s * r[5], s * r[8], 0.0f,
				a_transform[9] - a_adjust[0], a_transform[10] - a_adjust[1], a_transform[11] - a_adjust[2], 1.0f };
		}

		/** @brief D3DXMatrixMultiplyTranspose: transpose(a * b), as rows. */
		void MultiplyTranspose(const std::array<float, 16>& a_a, const std::array<float, 16>& a_b, float (*a_out)[4])
		{
			for (int i = 0; i < 4; ++i)
				for (int j = 0; j < 4; ++j) {
					float sum = 0.0f;
					for (int k = 0; k < 4; ++k)
						sum += a_a[i * 4 + k] * a_b[k * 4 + j];
					a_out[j][i] = sum;
				}
		}

		/** @brief The D3D11 view of a NiSourceTexture's renderer texture (+0x48, then +0x10), or null. */
		ID3D11ShaderResourceView* ViewOf(const RE::NiSourceTexture* a_texture)
		{
			if (!a_texture)
				return nullptr;
			const auto* renderer = *reinterpret_cast<const std::byte* const*>(reinterpret_cast<const std::byte*>(a_texture) + 0x48);
			return renderer ? Unwrap(*reinterpret_cast<ID3D11ShaderResourceView* const*>(renderer + 0x10)) : nullptr;
		}

		/** @brief Appends a_object's blood geometry, depth first, as the cull meets it. Hidden subtrees are skipped. */
		void Walk(RE::NiAVObject* a_object, std::vector<RE::BSGeometry*>& a_out)
		{
			if (!a_object || a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden))
				return;
			if (auto* geometry = a_object->AsGeometry()) {
				a_out.push_back(geometry);
				return;
			}
			if (auto* node = a_object->AsNode())
				for (auto& child : node->GetChildren())
					Walk(child.get(), a_out);
		}
	}

	void CaptureBlock(Snapshot::Builder& a_builder)
	{
		if (!RenderGraph::Engine::Native::Active() || a_builder.Built().kind != Snapshot::FrameKind::World)
			return;
		auto* camera = CS_ENGINE_READ(Camera, CapturePoint, RE::Main::WorldRootCamera());
		if (!camera)
			return;
		// Game thread: the setup only does arithmetic on the camera and the sun and writes globals that only the blood shader reads.
		using FlareSetup = void (*)(RE::NiCamera*);
		static const REL::Relocation<FlareSetup> flareSetup{ REL::Offset(Address::kFlareSetup) };
		flareSetup(camera);
		Block block;
		block.lightLoc = Global<std::array<float, 4>>(Address::kLightLoc);
		block.flareScale = Global<float>(Address::kFlareScale);
		block.hdrTarget = Global<std::int32_t>(Address::kHdrTarget);
		block.ctrl = Global<float>(Address::kCtrl);
		block.alphaScale = Global<float>(Address::kAlphaScale);
		block.flareAlphaScale = Global<float>(Address::kFlareAlphaScale);
		a_builder.Set(block);
	}

	bool CollectDraws(std::vector<DrawInputs>& a_out, std::string& a_why)
	{
		a_out.clear();
		const auto& snapshot = Snapshot::Current();
		const Block* block = snapshot ? snapshot->Block<Block>() : nullptr;
		if (!block) {
			a_why = "no screen blood block in the adopted snapshot";
			return false;
		}
		auto* splatter = RE::ScreenSplatter::GetSingleton();
		if (!splatter || !splatter->root || !splatter->camera) {
			a_why = "no ScreenSplatter root or camera";
			return false;
		}
		// SetCameraData(ScreenSplatter's camera): its view-projection and posAdjust.
		ShadowCameras::Camera camera;
		ShadowCameras::CameraMatrices(*splatter->camera, camera);

		std::vector<RE::BSGeometry*> geometries;
		Walk(splatter->root.get(), geometries);
		std::vector<DrawInputs> splatters;
		for (auto* geometry : geometries) {
			auto* property = netimmerse_cast<RE::BSBloodSplatterShaderProperty*>(geometry->GetGeometryRuntimeData().shaderProperty.get());
			if (!property)
				continue;
			if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
				a_why = "a blood geometry that is not a TriShape";
				return false;
			}
			DrawInputs draw;
			// GetRenderPasses: the flare pass when the property has a flare texture, else the splatter pass.
			draw.flare = property->bloodTextures[RE::BSBloodSplatterShaderProperty::kFlareColor] != nullptr;
			draw.shape = reinterpret_cast<const Family::TriShape*>(geometry->GetGeometryRuntimeData().rendererData);
			draw.triangles = static_cast<RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
			const auto* world = reinterpret_cast<const float*>(reinterpret_cast<const std::byte*>(geometry) + kWorld);
			MultiplyTranspose(MatrixOf(world, camera.eye), camera.viewProj, draw.worldViewProj);
			std::memcpy(draw.lightLoc, block->lightLoc.data(), sizeof(draw.lightLoc));
			draw.ctrl = block->ctrl;
			float propertyAlpha = 0.0f;
			std::memcpy(&propertyAlpha, reinterpret_cast<const std::byte*>(property) + kPropertyAlpha, sizeof(float));
			draw.alpha = (property->alphaSource ? *property->alphaSource : 0.0f) * propertyAlpha * block->alphaScale;
			if (draw.flare) {
				draw.alpha *= block->flareAlphaScale * block->flareScale;
				draw.hdrTarget = block->hdrTarget;
				draw.views[2] = ViewOf(property->bloodTextures[RE::BSBloodSplatterShaderProperty::kFlareColor].get());
				const auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
				if (block->hdrTarget < 0 || block->hdrTarget >= RE::RENDER_TARGETS::kTOTAL) {
					a_why = std::format("a flare HDR target {} out of range", block->hdrTarget);
					return false;
				}
				draw.views[3] = Unwrap(renderer->GetRuntimeData().renderTargets[block->hdrTarget].SRV);
				a_out.push_back(draw);
			} else {
				draw.views[0] = ViewOf(property->bloodTextures[RE::BSBloodSplatterShaderProperty::kBloodColor].get());
				draw.views[1] = ViewOf(property->bloodTextures[RE::BSBloodSplatterShaderProperty::kBloodAlpha].get());
				splatters.push_back(draw);
			}
		}
		a_out.insert(a_out.end(), splatters.begin(), splatters.end());
		return true;
	}
}

namespace DCLF::Features::BloodSplatter
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;

		// SetupTechnique's blend modes: the splatter multiplies the frame (DEST_COLOR, ZERO), the flare adds (ONE, ONE). Depth and
		// stencil are off. The write mask, fill, cull, bias and scissor are what the frame left set (observed: 1, 0, 1, 0, 0).
		constexpr std::uint32_t kSplatterBlend = 4, kFlareBlend = 5;
		constexpr std::uint32_t kWriteMode = 1, kFill = 0, kCull = 1;
		// SetupGeometry's and SetupTechnique's samplers: CLAMP, LINEAR.
		constexpr std::uint32_t kAddress = 0, kFilter = 1;

		Family::RefusalLog& NativeRefusals()
		{
			static Family::RefusalLog log("[DCLF] blood splatter: a native draw is left out");
			return log;
		}

		std::uint64_t VertexDescOf(const RE::BSShader& a_shader, std::uint32_t a_technique)
		{
			for (const auto& entry : a_shader.vertexShaders)
				if (entry && entry->id == a_technique)
					return entry->vertexDesc;
			return 0;
		}

		/** @brief This frame's run. Null when there is nothing to draw. A draw that cannot be made is left out, logged once. */
		std::shared_ptr<Run> BuildRun()
		{
			std::vector<DrawInputs> inputs;
			std::string why;
			if (!CollectDraws(inputs, why)) {
				NativeRefusals().Report(why);
				return nullptr;
			}
			if (inputs.empty())
				return nullptr;
			auto* shader = *reinterpret_cast<RE::BSShader**>(REL::Offset(Address::kShader).address());
			auto* state = globals::state;
			if (!shader || !state) {
				NativeRefusals().Report("no BSBloodSplatterShader or CS state");
				return nullptr;
			}
			const auto& target = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer)->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
			std::uint32_t format = 0;
			if (target.RTV) {
				D3D11_RENDER_TARGET_VIEW_DESC desc{};
				target.RTV->GetDesc(&desc);
				format = static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
			}
			if (!format) {
				NativeRefusals().Report("no back buffer");
				return nullptr;
			}
			// The whole back buffer: SetRenderTarget(kFRAMEBUFFER) with the engine's viewport at full size. The shadow state's viewport
			// is CS's render resolution here, which the engine's draws only ever had when CS upscales.
			namespace Vars = ::RenderGraph::Frame::Vars;
			const auto display = Vars::Adopted(Vars::DisplaySize());
			if (!display.width || !display.height) {
				NativeRefusals().Report("no display size");
				return nullptr;
			}
			Family::Viewport viewport;
			viewport.rect = { 0, 0, display.width, display.height };
			auto run = std::make_shared<Run>();
			for (const auto& input : inputs) {
				if (run->draws.size() >= kMaxDraws) {
					NativeRefusals().Report(std::format("more than {} draws in a frame", kMaxDraws));
					break;
				}
				const std::uint32_t technique = input.flare ? 1u : 0u;
				Draw draw{};
				std::uint32_t vertexDescriptor = technique, pixelDescriptor = technique;
				state->ModifyShaderLookup(*shader, vertexDescriptor, pixelDescriptor);
				draw.key.descriptor = pixelDescriptor;
				draw.key.defineStage = Family::DefineStage::Pixel;
				if (!Family::DescribeStates(draw.key, Pipeline::EngineBlendState(input.flare ? kFlareBlend : kSplatterBlend, 0, kWriteMode, 0), nullptr,
						Pipeline::EngineRasterState(kFill, kCull, 0, 0))) {
					NativeRefusals().Report("a blend or rasterizer state with no description");
					continue;
				}
				draw.key.colourFormats[0] = format;
				draw.program = BloodSplatterDraws::Program(draw.key, *shader);
				if (!draw.program) {
					NativeRefusals().Report(std::format("descriptor {:#x}: its program is not built yet", pixelDescriptor));
					continue;
				}
				Family::Owners owners;
				Family::Geometry geometry;
				std::string detail;
				if (const auto result = Family::LatchGeometry(input.shape, 0, input.triangles, VertexDescOf(*shader, technique), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
						geometry, owners, &detail);
					result != Family::GeometryResult::Ok) {
					NativeRefusals().Report(std::format("geometry: {}", Family::ReasonOf(result)), detail);
					continue;
				}
				auto& c = draw.constants;
				std::memcpy(c.worldViewProj, input.worldViewProj, sizeof(c.worldViewProj));
				std::memcpy(c.lightLoc, input.lightLoc, sizeof(c.lightLoc));
				c.ctrl[0] = input.ctrl;
				c.alpha[0] = input.alpha;
				c.vertexRef = geometry.vertexRef;
				c.indexRef = geometry.indexRef;
				c.vertexLayout[0] = static_cast<std::uint32_t>(geometry.layout);
				c.vertexLayout[1] = static_cast<std::uint32_t>(geometry.layout >> 32);
				c.vertexStride = geometry.stride;
				c.firstIndex = geometry.firstIndex;
				draw.indexCount = geometry.indexCount;
				bool texturesOk = true;
				for (std::uint32_t t = 0; t < 4 && texturesOk; ++t) {
					c.samplers[t] = Family::SlotSampler(kAddress, kFilter, owners);
					if (c.samplers[t] == GpuTextures::kInvalid) {
						NativeRefusals().Report("no graph sampler for CLAMP, LINEAR");
						texturesOk = false;
						break;
					}
					// The flare's t3 is the adapted luminance, an engine target the graph publishes: read by its published id.
					if (t == 3 && input.flare && input.hdrTarget > 0) {
						const auto id = Family::EngineTargetId(static_cast<RE::RENDER_TARGET>(input.hdrTarget));
						if (const auto result = BloodSplatterDraws::ReadPublished(id, c.textures[t]); result != Family::InputResult::Graph) {
							NativeRefusals().Report(std::format("the flare's luminance {}: {}", id, Family::ReasonOf(result)));
							texturesOk = false;
						}
						continue;
					}
					GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
					if (Family::SlotTexture(input.views[t], GpuTextures::ViewKind::Texture2D, GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), c.textures[t], owners,
							&failure) != Family::SlotResult::Ok) {
						NativeRefusals().Report(std::format("t{} cannot be read", t), GpuTextures::DescribeFailure(input.views[t], t, failure));
						texturesOk = false;
					}
				}
				run->owners.insert(run->owners.end(), owners.begin(), owners.end());
				if (!texturesOk)
					continue;
				draw.viewport = viewport;
				run->draws.push_back(std::move(draw));
			}
			return run;
		}

		/**
		* @brief Main::Draw's call of Main::RenderScreenSplatter (SE 35560 +0xA6A, AE 36559 +0xA73), made when ScreenSplatter's root has
		* children. Native draws the screen blood from game data at the EngineDraws anchor; the engine's function is not entered.
		*/
		struct RenderScreenSplatter
		{
			static void thunk()
			{
				if (!BloodSplatterDraws::Available() || (Family::BeginBatch(), !BloodSplatterDraws::CanTake()))
					return;
				auto run = BuildRun();
				if (!run)
					return;
				if (run->draws.empty()) {
					BloodSplatterDraws::Keep(std::move(run->owners));
					return;
				}
				BloodSplatterDraws::Submit(std::move(run));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief Main::RenderWorld's screen splatter cull and SubmitAccumulator. Native draws the blood from the root, so they do nothing. */
		struct SkipSplatterCull
		{
			static void thunk([[maybe_unused]] void* a_context) {}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct SkipSplatterSubmit
		{
			static void thunk([[maybe_unused]] void* a_camera, [[maybe_unused]] void* a_accumulator, [[maybe_unused]] std::uint32_t a_flags) {}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void InstallNative()
	{
		if (!RenderGraph::Engine::Native::Requested())
			return;
		Snapshot::RegisterCapture(&CaptureBlock);
		const auto draw = REL::RelocationID(35560, 36559).address() + REL::Relocate(0xA6A, 0xA73);
		HookSites::RequireCall("Main::Draw's call of RenderScreenSplatter", draw);
		stl::write_thunk_call<RenderScreenSplatter>(draw);
		const auto world = REL::RelocationID(100424, 107142).address();
		HookSites::RequireCall("Main::RenderWorld's screen splatter cull", world + REL::Relocate(0x3D2, 0x3FE));
		HookSites::RequireCall("Main::RenderWorld's screen splatter submit", world + REL::Relocate(0x3E4, 0x410));
		stl::write_thunk_call<SkipSplatterCull>(world + REL::Relocate(0x3D2, 0x3FE));
		stl::write_thunk_call<SkipSplatterSubmit>(world + REL::Relocate(0x3E4, 0x410));
		logger::info("[DCLF] blood splatter: native: drawn from ScreenSplatter's root at Main::Draw's RenderScreenSplatter call");
	}
}
