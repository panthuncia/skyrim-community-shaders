// Tree LOD's draw family: the mirror's drain, the frame's decision, the program source, and its draws in the main passes.
// A trees view's faces are in TreeLodFaces.cpp.

// volk must precede every Vulkan header.
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLod.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodTables.h"
#include "Features/DrawcallLimitFix/Features/TreeLod/TreeLodMirror.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/DrawFamilies/FamilySlots.h"

#include "ShaderCache.h"
#include <Render/RenderGraph/RenderGraph.h>
#include <rhi.h>

#include <cstdint>
#include <span>
#include <vector>

namespace DCLF::Features::TreeLod
{
	namespace
	{
		// The mirror is drained every frame, running or not.
		void Serve(const Api::DrawFamilyFrame& a_frame)
		{
			RenderMirror().Drain(a_frame.frame);
		}

		// DCLF's while there is something to draw, its pipelines are built and the atlas exists. Until then the engine draws tree LOD.
		bool Decide(const Api::DrawFamilyFrame& a_frame)
		{
			if (!RenderMirror().Size())
				return false;
			// Not ready yet: the family waits with its engine shapes' passes withheld. Nobody draws tree LOD for those frames, and a load's hold waits.
			const bool ready = a_frame.scene && TablesOf(*a_frame.scene) && TreeLodAtlasReady();
			const bool owned = a_frame.Pipelines(ready);
			if (!ready && a_frame.waits)
				*a_frame.waits = true;
			return owned;
		}

		const void* ProgramSource()
		{
			return Engine::Global<RE::BSShader*>(Address::kDistantTreeShader);
		}

		const void* EngineShapeVtable()
		{
			return reinterpret_cast<const void*>(RE::VTABLE_BSDistantTreeShaderProperty[0].address());
		}

		// DistantTree's descriptors as the engine's main view uses them: colour Deferred and AlphaTest, depth Depth and AlphaTest.
		// A view's forward program uses AlphaTest alone.
		using TreeTechnique = SIE::ShaderCache::DistantTreeShaderTechniques;
		using TreeFlag = SIE::ShaderCache::DistantTreeShaderFlags;
		constexpr std::uint32_t kTreeAlphaTest = static_cast<std::uint32_t>(TreeFlag::AlphaTest);
		constexpr Api::DrawFamilyProgram kProgram{ .shaderType = static_cast<std::uint8_t>(RE::BSShader::Type::DistantTree),
			.name = "DistantTree",
			.label = "tree LOD",
			.keyName = "tree-lod",
			.sourcePath = "Data/Shaders/DistantTree.hlsl",
			.nativeNote = "tree LOD stays native",
			.descriptors = { .colour = static_cast<std::uint32_t>(TreeFlag::Deferred) | kTreeAlphaTest,
				.depth = static_cast<std::uint32_t>(TreeTechnique::Depth) | kTreeAlphaTest,
				.forward = kTreeAlphaTest },
			.build = Api::DrawFamilyBuild::DepthThenEqualColour,
			.signature = Api::DrawFamilySignature::DrawInstanced };
		static_assert(kProgram.descriptors.colour == 0x10100 && kProgram.descriptors.depth == 0x10001 && kProgram.descriptors.forward == 0x10000,
			"tree LOD's descriptors are not the engine's (Deferred | AlphaTest, Depth | AlphaTest, AlphaTest)");

		/** @brief Records tree LOD's draw. Arguments come from the visible list's header. */
		void RecordTreeLodDraw(rhi::CommandList& a_commands, const FamilyPipelines& a_pipelines, bool a_colour, bool a_phaseTwo, RenderGraph::BufferRef a_draw,
			rhi::ResourceHandle a_visible)
		{
			using TreeInstances::VisibleHeader;
			std::uint32_t words[kDrawPushWords]{ a_draw.viewSlot, a_draw.byteOffset, a_phaseTwo ? 1u : 0u,
				a_colour && debugTint ? 1u : 0u };
			const std::uint64_t arguments = a_colour ? offsetof(VisibleHeader, colour) : a_phaseTwo ? offsetof(VisibleHeader, phaseTwo) : offsetof(VisibleHeader, phaseOne);
			a_commands.BindPipeline(a_colour ? a_pipelines.colour : a_pipelines.depth);
			a_commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kDrawPushBinding, 0, kDrawPushWords, words);
			a_commands.ExecuteIndirect(a_pipelines.drawSignature, a_visible, arguments, {}, 0, 1);
		}

		void MainDeclare(Api::DrawFamilyDeclare& a_declare)
		{
			const auto* tables = TablesOf(a_declare.scene);
			if (!tables)
				return;
			const std::span<const org::SrvView> noViews{};  // Device-address reads need ordering, not a descriptor.
			auto& builder = a_declare.builder;
			a_declare.arguments[0] = builder.IndirectArguments(*tables->visible).Resource();
			a_declare.row = tables->drawRef;
			for (const auto& table : { tables->shapes, tables->instances, tables->meshes })
				builder.ShaderResource(*table, noViews);
			builder.ShaderResource(tables->draw, noViews);
		}

		// The CPU pipeline's main draw: the visible list's adopted version and the draw row's ref.
		bool MainDirect(Api::DrawFamilyDirect& a_draw)
		{
			const auto* tables = TablesOf(a_draw.scene);
			if (!tables || !tables->visible || !tables->drawRef || !a_draw.adopted.Names(*tables->visible))
				return false;
			const auto& version = a_draw.adopted.Of(*tables->visible).version;
			if (!version || !version->buffer)
				return false;
			a_draw.row = tables->drawRef;
			a_draw.arguments = version->buffer;
			return true;
		}

		// Its pipelines, built late and rebuilt on a target change.
		void MainRevision(const Api::DrawFamilyRevision& a_revision, std::vector<std::uint64_t>& a_out)
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(a_revision.pipelines.get()));
		}

		void MainRecord(const Api::DrawFamilyRecord& a_record)
		{
			RecordTreeLodDraw(a_record.commands, a_record.Drawn<FamilyPipelines>(), a_record.pass == Api::DrawFamilyPass::Colour,
				a_record.pass == Api::DrawFamilyPass::DepthPhaseTwo, a_record.row, a_record.arguments);
		}
	}

	void RegisterFamily(Api::InstallContext& a_context)
	{
		a_context.RegisterDrawFamily({ .family = Api::DrawFamily::TreeInstances,
			.objectClass = Api::ObjectClass::TreeLod,
			.resources = TablesId(),
			.colourRange = "cs.dclf.colour.tree-lod",
			.frame = { .serve = &Serve, .decide = &Decide, .programSource = &ProgramSource },
			.main = { .declare = &MainDeclare, .revision = &MainRevision, .record = &MainRecord, .direct = &MainDirect },
			.faces = FacesContribution(),
			.program = kProgram,
			.engineShapes = { .propertyVtable = &EngineShapeVtable } });
	}
}
