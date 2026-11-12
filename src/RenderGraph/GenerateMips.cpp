#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GenerateMips.h"

#	include "RenderGraph/ComputeProgram.h"

#	include <Render/LatchBlock.h>
#	include <Render/PassBuilders.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <RenderPasses/GenerateMipsPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/GloballyIndexedResource.h>
#	include <rhi_helpers.h>

#	include <algorithm>
#	include <array>
#	include <span>
#	include <stdexcept>

namespace
{
	// ORG's shader, under Data/Shaders (org::kBuiltinShaderPrefix).
	constexpr const char* kShader = "OpenRenderGraph/GenerateMips.hlsl";
	// The shader's GenerateMipsConstants, in order: source and destination views, their sizes, the flags.
	constexpr std::uint32_t kConstantWords = 7;
	constexpr std::uint32_t kSrgbFlag = 1u;
	constexpr std::uint32_t kGroupSize = 8;

	struct MipsBindings
	{
		std::vector<org::DeclaredViewToken> mips;  // an unordered-access view of each mip, all slices
		std::uint32_t slices = 1;
	};

	struct MipsFrame
	{
		std::shared_ptr<const GraphPasses::GenerateMipsProgram> program;
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t offset = 0;
		std::vector<org::GenerateMipsStep> steps;
		std::uint32_t flags = 0;
	};

	class GenerateMipsGraphPass final : public org::TypedRenderGraphPass<GenerateMipsGraphPass, MipsFrame, MipsBindings>
	{
	public:
		GenerateMipsGraphPass(std::shared_ptr<const GraphPasses::GenerateMipsProgram> a_program, std::shared_ptr<org::Resource> a_texture,
			GraphPasses::GenerateMipsGate a_gate) :
			program(std::move(a_program)), texture(std::move(a_texture)), gate(std::move(a_gate)) {}

		MipsBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			rhi::ResourceDesc desc{};
			if (!texture->TryGetRHIResourceDesc(desc) || desc.type != rhi::ResourceType::Texture2D)
				throw std::invalid_argument("GraphPasses::MakeGenerateMips: '" + texture->GetName() + "' is not a 2D texture");
			MipsBindings bindings;
			bindings.slices = (std::max)(1u, static_cast<std::uint32_t>(desc.texture.depthOrLayers));
			const bool array = bindings.slices > 1;
			if (array != program->array)
				throw std::invalid_argument("GraphPasses::MakeGenerateMips: '" + texture->GetName() + "' needs the program built for " +
											(array ? "arrays" : "2D textures"));
			const std::uint32_t mips = desc.texture.mipLevels;
			if (mips < 2)
				return bindings;
			// One declaration for the whole chain: a dispatch reads one mip and writes the next, through unordered-access views.
			std::vector<org::UavView> views(mips);
			for (std::uint32_t mip = 0; mip < mips; ++mip)
				views[mip] = { array ? static_cast<std::uint32_t>(org::UAVViewType::Texture2DArrayFull) : UINT32_MAX, mip, 0 };
			bindings.mips = a_builder.UnorderedAccess(texture, std::span<const org::UavView>(views)).views;
			return bindings;
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(texture.get()));
			a_out.push_back(reinterpret_cast<std::uintptr_t>(gate.latch.get()));
			a_out.push_back(gate.offset);
		}

		MipsFrame Prepare(const MipsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
		{
			MipsFrame frame{ program, gate.latch, gate.offset };
			if (a_bindings.mips.size() < 2)
				return frame;
			// The frame's backing, which may differ from the one the pass was declared with.
			const auto& desc = a_preparation.Describe(a_bindings.mips.front()).texture;
			frame.steps = org::PlanGenerateMips(desc.width, desc.height,
				(std::min)(static_cast<std::uint32_t>(desc.mipLevels), static_cast<std::uint32_t>(a_bindings.mips.size())));
			if (rhi::helpers::IsSRGB(desc.format))
				frame.flags |= kSrgbFlag;
			return frame;
		}

		static void Record(const MipsBindings& a_bindings, const MipsFrame& a_frame, org::PassRecordContext& a_recording)
		{
			if (!a_frame.program || a_frame.steps.empty())
				return;
			const auto& compute = *a_frame.program->compute;
			for (std::size_t i = 0; i < a_frame.steps.size(); ++i) {
				const auto& step = a_frame.steps[i];
				// The previous dispatch wrote this one's source.
				if (i)
					a_recording.UavBarrier();
				const std::array<std::uint32_t, kConstantWords> constants{ a_recording.Index(a_bindings.mips[i]), a_recording.Index(a_bindings.mips[i + 1]),
					step.sourceWidth, step.sourceHeight, step.destinationWidth, step.destinationHeight, a_frame.flags };
				if (!a_frame.latch) {
					compute.Dispatch(a_recording, constants, (step.destinationWidth + kGroupSize - 1) / kGroupSize,
						(step.destinationHeight + kGroupSize - 1) / kGroupSize, a_bindings.slices);
					continue;
				}
				// The gate's group counts for this step, in the run's region.
				const auto offset = static_cast<std::uint32_t>(a_frame.offset + i * sizeof(GraphPasses::DispatchGroups));
				compute.DispatchIndirect(a_recording, constants, *a_frame.latch, offset);
			}
		}

	private:
		std::shared_ptr<const GraphPasses::GenerateMipsProgram> program;
		std::shared_ptr<org::Resource> texture;
		GraphPasses::GenerateMipsGate gate;
	};
}

namespace GraphPasses
{
	std::shared_ptr<const GenerateMipsProgram> LoadGenerateMips(rhi::Device a_device, bool a_array)
	{
		// ORG's own shader has no D3D11 build, so it takes none of CS's defines.
		ComputeProgram::Desc desc{ .source = kShader, .constantWords = kConstantWords, .commonDefines = false };
		if (a_array)
			desc.defines.push_back({ L"ORG_GENERATE_MIPS_ARRAY", L"1" });
		auto program = std::make_shared<GenerateMipsProgram>();
		program->array = a_array;
		program->compute = ComputeProgram::Load(a_device, desc);
		if (!program->compute)
			return nullptr;
		return program;
	}

	std::vector<DispatchGroups> GenerateMipsGroups(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, std::uint32_t a_slices)
	{
		std::vector<DispatchGroups> groups;
		for (const auto& step : org::PlanGenerateMips(a_width, a_height, a_mips))
			groups.push_back({ (step.destinationWidth + kGroupSize - 1) / kGroupSize, (step.destinationHeight + kGroupSize - 1) / kGroupSize,
				(std::max)(a_slices, 1u), 0 });
		return groups;
	}

	std::shared_ptr<org::RenderPass> MakeGenerateMips(std::shared_ptr<const GenerateMipsProgram> a_program, std::shared_ptr<org::Resource> a_texture,
		GenerateMipsGate a_gate)
	{
		if (!a_program || !a_texture)
			return nullptr;
		return std::make_shared<GenerateMipsGraphPass>(std::move(a_program), std::move(a_texture), std::move(a_gate));
	}
}

#endif
