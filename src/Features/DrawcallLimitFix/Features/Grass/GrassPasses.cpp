// Grass's compute passes. The cull and draw-argument passes run before each depth phase's draw. The colour arguments pass runs after
// both phases and zeroes the counts for the next frame. A readback pass copies the draws header for diagnostics.

// Main/Resources.h first. volk must precede every Vulkan header in the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "Features/DrawcallLimitFix/Features/Grass/GrassTables.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Shapes.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <rhi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace DCLF::Draws
{
	struct GrassCullBindings
	{
		org::DeclaredViewToken draw, instances, owners, shapes, buckets, frame, visible, draws, hzb;
	};

	struct GrassCullPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::Grass::GrassCullConstants constants{};
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t groups = 0;
		bool occlusion = false;  // true when the HZB view is bound
	};

	/**
	 * @brief Two-phase cull, like BuildDraws. Phase 1 tests every instance against last frame's HZB and queues occluded ones for retest.
	 * Phase 2 retests those against this frame's HZB and appends survivors after phase 1's.
	 */
	class GrassCullPass final : public org::TypedRenderGraphPass<GrassCullPass, GrassCullPrepared, GrassCullBindings>
	{
	public:
		GrassCullPass(std::shared_ptr<Resources> a_resources, std::shared_ptr<const Features::Grass::GrassTables> a_tables, std::uint32_t a_phase,
			RenderGraphRuntime::Segment a_segment, const GraphConditions& a_conditions) :
			resources(std::move(a_resources)), tables(std::move(a_tables)), phase(a_phase), segment(a_segment), conditions(a_conditions) {}

		GrassCullBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			GrassCullBindings bindings{};
			bindings.draw = a_builder.ShaderResource(tables->draw).View();
			bindings.instances = a_builder.ShaderResource(*tables->instances).View();
			bindings.owners = a_builder.ShaderResource(*tables->owners).View();
			bindings.shapes = a_builder.ShaderResource(*tables->shapes).View();
			bindings.buckets = a_builder.ShaderResource(*tables->buckets).View();
			bindings.frame = a_builder.UnorderedAccess(*tables->frame).View();
			bindings.visible = a_builder.UnorderedAccess(*tables->visible).View();
			bindings.draws = a_builder.UnorderedAccess(tables->draws).View();
			// Which HZB a phase sees depends on where it sits relative to the HZB build.
			if (resources->hzb)
				bindings.hzb = a_builder.ShaderResource(resources->hzb).View();
			return bindings;
		}

		// Table versions are already part of the revision, so nothing is added here.
		void Revision(std::vector<std::uint64_t>&) const {}

		GrassCullPrepared Prepare(const GrassCullBindings&, const org::PassPrepareContext& a_preparation) const
		{
			GrassCullPrepared prepared{};
			const auto frame = CurrentFrame(a_preparation, segment);
			const std::uint64_t slots = std::min({ NamedUnitsOf(a_preparation, tables->instances), NamedUnitsOf(a_preparation, tables->owners),
				NamedUnitsOf(a_preparation, tables->frame), NamedUnitsOf(a_preparation, tables->visible) });
			if (!tables->cull || !slots || !frame || !frame->latch)
				return prepared;
			prepared.program = tables->cull;
			prepared.latch = frame->latch;
			auto& constants = prepared.constants;
			constants.phase = phase;
			constants.retestOffset = static_cast<std::uint32_t>(NamedUnitsOf(a_preparation, tables->visible) * GrassInstances::kRetestOffsetPerUnit);
			if (Draws::OcclusionTested(conditions, *resources, *frame)) {
				prepared.occlusion = true;
				constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
				constants.hzbMips = resources->hzbMips;
			}
			// Dispatched for full capacity. The shader stops at the live slot count or the retest count.
			prepared.groups = static_cast<std::uint32_t>((slots + Features::Grass::kGrassCullGroup - 1) / Features::Grass::kGrassCullGroup);
			return prepared;
		}

		static void Record(const GrassCullBindings& a_bindings, const GrassCullPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups || !a_frame.latch)
				return;
			auto constants = a_frame.constants;
			constants.drawIndex = a_recording.Index(a_bindings.draw);
			constants.instancesIndex = a_recording.Index(a_bindings.instances);
			constants.ownersIndex = a_recording.Index(a_bindings.owners);
			constants.shapesIndex = a_recording.Index(a_bindings.shapes);
			constants.bucketsIndex = a_recording.Index(a_bindings.buckets);
			constants.frameIndex = a_recording.Index(a_bindings.frame);
			constants.visibleIndex = a_recording.Index(a_bindings.visible);
			constants.drawsIndex = a_recording.Index(a_bindings.draws);
			if (a_frame.occlusion)
				constants.hzbIndex = a_recording.Index(a_bindings.hzb);
			SetLatchWords(constants, *a_frame.latch, a_recording.Run());
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline(a_recording));
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::Grass::kGrassCullConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<Resources> resources;
		std::shared_ptr<const Features::Grass::GrassTables> tables;
		std::uint32_t phase = 1;
		RenderGraphRuntime::Segment segment;
		GraphConditions conditions;  // the build's
	};

	struct GrassArgsBindings
	{
		org::DeclaredViewToken draw, buckets, meshes, draws;
	};

	struct GrassArgsPrepared
	{
		std::shared_ptr<const ComputeProgram> program;
		Features::Grass::GrassArgsConstants constants{};
		std::uint32_t groups = 0;
	};

	/**
	 * @brief Packs one draw per non-empty draw slot into the indirect arguments. Mode 3, the colour pass, also zeroes the slot's
	 * counts for the next frame.
	 */
	class GrassArgsPass final : public org::TypedRenderGraphPass<GrassArgsPass, GrassArgsPrepared, GrassArgsBindings>
	{
	public:
		GrassArgsPass(std::shared_ptr<const Features::Grass::GrassTables> a_tables, std::uint32_t a_mode) :
			tables(std::move(a_tables)), mode(a_mode) {}

		GrassArgsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			GrassArgsBindings bindings{};
			bindings.draw = a_builder.ShaderResource(tables->draw).View();
			bindings.buckets = a_builder.ShaderResource(*tables->buckets).View();
			bindings.meshes = a_builder.ShaderResource(*tables->meshes).View();
			bindings.draws = a_builder.UnorderedAccess(tables->draws).View();
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>&) const {}

		GrassArgsPrepared Prepare(const GrassArgsBindings&, const org::PassPrepareContext& a_preparation) const
		{
			GrassArgsPrepared prepared{};
			const auto buckets = std::min<std::uint64_t>(NamedUnitsOf(a_preparation, tables->buckets), GrassInstances::kMaxBuckets);
			if (!tables->args || !buckets)
				return prepared;
			prepared.program = tables->args;
			auto& constants = prepared.constants;
			constants.mode = mode;
			constants.drawSlots = static_cast<std::uint32_t>(buckets * GrassInstances::kTiers);
			// Dispatched for capacity. The shader stops at the live bucket count.
			prepared.groups = (constants.drawSlots + Features::Grass::kGrassArgsGroup - 1) / Features::Grass::kGrassArgsGroup;
			return prepared;
		}

		static void Record(const GrassArgsBindings& a_bindings, const GrassArgsPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || !a_frame.groups)
				return;
			auto constants = a_frame.constants;
			constants.drawIndex = a_recording.Index(a_bindings.draw);
			constants.bucketsIndex = a_recording.Index(a_bindings.buckets);
			constants.meshesIndex = a_recording.Index(a_bindings.meshes);
			constants.drawsIndex = a_recording.Index(a_bindings.draws);
			auto& commands = a_recording.Commands();
			commands.BindLayout(a_frame.program->layout->GetHandle());
			commands.BindPipeline(a_frame.program->Pipeline(a_recording));
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, Features::Grass::kGrassArgsConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
			commands.Dispatch(a_frame.groups, 1, 1);
		}

	private:
		std::shared_ptr<const Features::Grass::GrassTables> tables;
		std::uint32_t mode = 1;
	};

	struct GrassReadbackBindings
	{
		org::ResourceBindingToken draws;
	};

	struct GrassReadbackPrepared
	{
		std::shared_ptr<Features::Grass::GrassCounts> counts;
	};

	/** @brief Sums the slot's previous readback, then copies this frame's draws header into it. */
	class GrassReadbackPass final : public org::TypedRenderGraphPass<GrassReadbackPass, GrassReadbackPrepared, GrassReadbackBindings>
	{
	public:
		explicit GrassReadbackPass(std::shared_ptr<const Features::Grass::GrassTables> a_tables) :
			tables(std::move(a_tables)) {}

		GrassReadbackBindings Declare(org::PassBuilder& a_builder)
		{
			return { a_builder.CopySource(tables->draws) };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(0); }

		GrassReadbackPrepared Prepare(const GrassReadbackBindings&, const org::PassPrepareContext&) const
		{
			return { tables->counts };
		}

		static void Sum(Features::Grass::GrassCounts& a_counts, const GrassInstances::DrawsHeader& a_header)
		{
			auto add = [](std::atomic<std::uint64_t>& a_sum, std::uint32_t a_value) { a_sum.fetch_add(a_value, std::memory_order_relaxed); };
			add(a_counts.frames, 1);
			add(a_counts.live, a_header.stats[GrassInstances::kStatLive]);
			add(a_counts.beyond, a_header.stats[GrassInstances::kStatFar]);
			add(a_counts.outside, a_header.stats[GrassInstances::kStatOutside]);
			add(a_counts.thinned, a_header.stats[GrassInstances::kStatThinned]);
			add(a_counts.faded, a_header.stats[GrassInstances::kStatFaded]);
			add(a_counts.tested, a_header.tested);
			add(a_counts.phaseOne, a_header.phaseOneInstances);
			add(a_counts.retests, a_header.retests);
			add(a_counts.phaseTwo, a_header.phaseTwoInstances);
			add(a_counts.phaseOneDraws, a_header.phaseOneDraws);
			add(a_counts.phaseTwoDraws, a_header.phaseTwoDraws);
			add(a_counts.colourDraws, a_header.colourDraws[0]);
			add(a_counts.pbrDraws, a_header.colourDraws[1]);
		}

		static void Record(const GrassReadbackBindings& a_bindings, const GrassReadbackPrepared& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.counts)
				return;
			auto& counts = *a_frame.counts;
			// The region's lease retires before it records again, so its last copy has finished.
			const std::uint32_t region = counts.readback->RegionOf(a_recording.Run());
			if (region >= counts.filled.size())
				return;
			if (counts.filled[region]) {
				counts.readback->ReadRegion(region, [&](const std::byte* a_bytes) {
					GrassInstances::DrawsHeader header;
					std::memcpy(&header, a_bytes, sizeof(header));
					Sum(counts, header);
				});
			}
			counts.readback->RecordCopy(a_recording.Commands(), region, 0, a_recording.Resolve(a_bindings.draws).GetHandle(), 0, sizeof(GrassInstances::DrawsHeader));
			counts.filled[region] = 1;
		}

	private:
		std::shared_ptr<const Features::Grass::GrassTables> tables;
	};
}

namespace DCLF::Features::Grass
{
	namespace
	{
		std::shared_ptr<const void> TablesSetup(const Api::PassSetup& a_setup)
		{
			return SharedTablesOf(a_setup.scene);
		}

		std::shared_ptr<const void> CountsSetup(const Api::PassSetup& a_setup)
		{
			auto tables = SharedTablesOf(a_setup.scene);
			if (!tables || !tables->counts)
				return nullptr;
			return tables;
		}

		std::shared_ptr<org::RenderPass> MakeCullPhaseOne(const Api::PassContext& a_context)
		{
			if (a_context.Interior())  // interiors have no grass
				return nullptr;
			return std::make_shared<Draws::GrassCullPass>(a_context.Main(), a_context.State<GrassTables>(), 1, a_context.segment, a_context.Conditions());
		}

		std::shared_ptr<org::RenderPass> MakeArgsPhaseOne(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsPhaseOne);
		}

		// Phase 2 exists only when the HZB is rebuilt this frame.
		std::shared_ptr<org::RenderPass> MakeCullPhaseTwo(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::GrassCullPass>(a_context.Main(), a_context.State<GrassTables>(), 2, a_context.segment, a_context.Conditions());
		}

		std::shared_ptr<org::RenderPass> MakeArgsPhaseTwo(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			if (!a_context.PhaseTwo())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsPhaseTwo);
		}

		// Always runs, because it zeroes the counts the next frame's cull appends to.
		std::shared_ptr<org::RenderPass> MakeArgsColour(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassArgsPass>(a_context.State<GrassTables>(), kArgsColour);
		}

		std::shared_ptr<org::RenderPass> MakeCounts(const Api::PassContext& a_context)
		{
			if (a_context.Interior())
				return nullptr;
			return std::make_shared<Draws::GrassReadbackPass>(a_context.State<GrassTables>());
		}
	}

	void RegisterPasses(Api::InstallContext& a_context)
	{
		a_context.RegisterPass({ .key = "grass.cull", .passName = "cs.dclf.z.grass-cull", .make = &MakeCullPhaseOne, .setup = &TablesSetup, .cpuPipeline = true });
		a_context.RegisterPass({ .key = "grass.args", .passName = "cs.dclf.z.grass-args", .make = &MakeArgsPhaseOne, .setup = &TablesSetup, .cpuPipeline = true });
		a_context.RegisterPass({ .key = "grass.cull-phase2", .passName = "cs.dclf.grass-cull-phase2", .make = &MakeCullPhaseTwo, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.args-phase2", .passName = "cs.dclf.grass-args-phase2", .make = &MakeArgsPhaseTwo, .setup = &TablesSetup });
		a_context.RegisterPass({ .key = "grass.args-colour", .passName = "cs.dclf.z.grass-args-colour", .make = &MakeArgsColour, .setup = &TablesSetup, .cpuPipeline = true });
		a_context.RegisterPass({ .key = "grass.counts", .passName = "cs.dclf.z.grass-counts", .kind = Api::PassKind::Copy, .make = &MakeCounts, .setup = &CountsSetup });
	}
}
