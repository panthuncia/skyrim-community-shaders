// The grass draw family (dclf-architecture.md, "Grass: the draws"). Decides each frame whether DCLF draws grass, and records the
// indirect draws, one per bucket and LOD tier, in the two depth phases and the colour pass.

// volk must precede every Vulkan header in the translation unit.
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/DrawFamilies/FamilySlots.h"
#include "Features/DrawcallLimitFix/Core/Render/LoadHold.h"

#include "GrassShared/GrassCapture.h"
#include "GrassShared/GrassFrame.h"

#include "ShaderCache.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace DCLF::Features::Grass
{
	namespace
	{
		// The engine's grass colour passes use the opaque write mode.
		constexpr std::uint8_t kWriteMode = 1;

		// Drained every frame, even when DCLF does not own grass.
		void Serve(const Api::DrawFamilyFrame& a_frame)
		{
			GrassShared::SetDclfOwned(false);
			RenderMirror().Drain(a_frame.frame);
		}

		// The tg console command hides BGSGrassManager's grass node (+0xF4 bit 0).
		bool EngineGrassShown()
		{
			const auto* manager = Engine::Global<const std::byte*>(Address::kGrassManager);
			const auto* node = manager ? *reinterpret_cast<const std::byte* const*>(manager + Address::kGrassManagerNode) : nullptr;
			return node && (*reinterpret_cast<const std::uint32_t*>(node + 0xF4) & 1u) == 0;
		}

		// DCLF takes grass only when it can draw all of it. A rejected group is still engine-drawn, and withholding grass would hide it.
		bool Decide(const Api::DrawFamilyFrame& a_frame)
		{
			const auto& mirror = RenderMirror();
			if (!a_frame.scene || !TablesOf(*a_frame.scene) || !mirror.BucketSlots())
				return false;
			return a_frame.Pipelines(mirror.BucketSlots() <= GrassInstances::kMaxBuckets && !mirror.RejectedLive() && EngineGrassShown());
		}

		// Grass Optimizations and Grass Collision must stand down on exactly the frames whose grass passes are withheld.
		// On those frames Grass Collision's update runs from here instead, before the depth commit reads it.
		void Owned(bool a_owned)
		{
			// A load hold withholds every grass pass, even before DCLF owns the family.
			const bool standDown = a_owned || LoadHold::Active();
			GrassShared::SetDclfOwned(standDown);
			if (!standDown)
				return;
			// Not the renderer's posAdjust, which belongs to the last pass here rather than the main view.
			auto* camera = RE::Main::WorldRootCamera();
			if (!camera)
				return;
			const auto& eye = camera->world.translate;
			GrassShared::RunStoodDownFrame({ .eye = { eye.x, eye.y, eye.z } });
		}

		// Falls back to the shader GrassShared saw until the BSGrassShader singleton is known.
		const void* ProgramSource()
		{
			if (auto* shader = Engine::Global<RE::BSShader*>(Address::kGrassShader))
				return shader;
			return GrassShared::GrassShader();
		}

		const void* EngineShapeVtable()
		{
			return reinterpret_cast<const void*>(RE::VTABLE_BSGrassShaderProperty[0].address());
		}

		// Colour has no AlphaTest because the EQUAL depth test already decides coverage. Grass has no forward program.
		using GrassTechnique = SIE::ShaderCache::GrassShaderTechniques;
		constexpr std::uint32_t kGrassColour = 0x3;
		constexpr Api::DrawFamilyProgram kProgram{ .shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::Grass),
			.name = "RunGrass",
			.label = "grass",
			.keyName = "grass",
			.sourcePath = "Data/Shaders/RunGrass.hlsl",
			.nativeNote = "grass stays native",
			.descriptors = { .colour = kGrassColour,
				.depth = static_cast<std::uint32_t>(GrassTechnique::RenderDepth),
				.forward = 0,
				.colour2 = static_cast<std::uint32_t>(GrassTechnique::TruePbr) },
			.colours = 2,
			.writeMode = kWriteMode,
			.build = Api::DrawFamilyBuild::DepthThenEqualColour,
			.signature = Api::DrawFamilySignature::InstanceWordDrawInstanced };
		static_assert(kProgram.descriptors.depth == 0x8 && kProgram.descriptors.colour2 == 0x9 &&
						  (kProgram.descriptors.colour & static_cast<std::uint32_t>(SIE::ShaderCache::GrassShaderFlags::AlphaTest)) == 0,
			"grass's descriptors are not the engine's (RenderDepth, TruePbr; the colour program without AlphaTest)");

		// Matches DCLFGrassPush in RunGrass.hlsl (b190).
		void Push(rhi::CommandList& a_commands, RenderGraph::BufferRef a_draw, std::uint32_t a_phase, std::uint32_t a_program)
		{
			std::uint32_t words[kDrawPushWords]{ a_draw.viewSlot, a_draw.byteOffset, a_phase, a_program };
			a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
		}

		void MainDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			auto* tables = TablesOf(a_declare.scene);
			if (!tables)
				return;
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			auto& builder = a_declare.builder;
			a_declare.arguments[0] = builder.IndirectArguments(tables->draws);
			a_declare.row = tables->drawRef;
			for (const auto& table : { tables->instances, tables->owners, tables->frame, tables->visible, tables->shapes, tables->buckets, tables->meshes })
				builder.ShaderResource(*table, noViews);
			builder.ShaderResource(tables->draw, noViews);
			static constexpr std::string_view kCollisionIds[] = { GrassShared::kCollisionGraphId };
			Extensions::DeclareGraphTextures(builder, kCollisionIds);
			Extensions::DeclareGraphTextures(builder, Extensions::ShadowMaskGraphTextureIds());
		}

		bool MainDirect(Api::DrawFamilyDirect& a_draw)
		{
			const auto* tables = TablesOf(a_draw.scene);
			if (!tables || !tables->draws || !tables->drawRef)
				return false;
			a_draw.row = tables->drawRef;
			a_draw.arguments = tables->draws;
			return true;
		}

		void MainRevision(const Api::DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out)
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(a_revision.pipelines.get()));
		}

		// argumentsOffset skips the instance word on Vulkan, which has no push-constant argument in its signature.
		void MainRecord(const Api::DrawFamilyRecord& a_record)
		{
			using namespace GrassInstances;
			const auto& pipelines = a_record.Drawn<FamilyPipelines>();
			auto& commands = a_record.commands;
			if (a_record.pass != Api::DrawFamilyPass::Colour) {
				const bool phaseTwo = a_record.pass == Api::DrawFamilyPass::DepthPhaseTwo;
				commands.BindPipeline(pipelines.depth);
				Push(commands, a_record.row, phaseTwo ? 2u : 1u, 0u);
				commands.ExecuteIndirect(pipelines.drawSignature, a_record.arguments, (phaseTwo ? kPhaseTwoArgs : kPhaseOneArgs) + pipelines.argumentsOffset, a_record.arguments,
					phaseTwo ? offsetof(DrawsHeader, phaseTwoDraws) : offsetof(DrawsHeader, phaseOneDraws), kMaxDrawSlots);
				return;
			}
			for (std::uint32_t program = 0; program < 2; ++program) {
				commands.BindPipeline(program ? pipelines.colour2 : pipelines.colour);
				Push(commands, a_record.row, 0u, program);
				commands.ExecuteIndirect(pipelines.drawSignature, a_record.arguments, kColourArgs[program] + pipelines.argumentsOffset, a_record.arguments,
					offsetof(DrawsHeader, colourDraws) + program * sizeof(std::uint32_t), kMaxDrawSlots);
			}
		}
	}

	void RegisterFamily(Api::InstallContext& a_context)
	{
		a_context.RegisterDrawFamily({ .family = Api::DrawFamily::Grass,
			.objectClass = Api::ObjectClass::Grass,
			.resources = TablesId(),
			.colourRange = "cs.dclf.colour.grass",
			.frame = { .serve = &Serve, .decide = &Decide, .owned = &Owned, .programSource = &ProgramSource },
			.main = { .declare = &MainDeclare, .revision = &MainRevision, .record = &MainRecord, .direct = &MainDirect },
			.program = kProgram,
			.engineShapes = { .propertyVtable = &EngineShapeVtable } });
	}
}
