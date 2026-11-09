#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GpuLoadPass.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/GpuTimingSource.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>

namespace GpuLoadPass
{
	namespace
	{
		constexpr const char* kClientId = "cs.gpu-load";
		constexpr std::uint32_t kTextureSize = 256;

		// Must match LoadConstants in GpuLoadCS.hlsl.
		struct LoadConstants
		{
			std::uint32_t outputIndex;
			std::uint32_t padding[3];
		};
		constexpr std::uint32_t kConstantWords = sizeof(LoadConstants) / sizeof(std::uint32_t);

		struct Resources
		{
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<org::PixelBuffer> output;
			std::uint32_t groups = 0;
		};

		struct LoadBindings
		{
			org::DeclaredViewToken output;
		};

		struct LoadFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			std::uint32_t groups = 0;
		};

		class LoadPass final : public org::TypedRenderGraphPass<LoadPass, LoadFrame, LoadBindings>
		{
		public:
			explicit LoadPass(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			LoadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(resources->output).View() };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->program.get()));
				a_out.push_back(resources->groups);
			}

			LoadFrame Prepare(const LoadBindings&, const org::PassPrepareContext&) const { return { resources->program, resources->groups }; }

			static void Record(const LoadBindings& a_bindings, const LoadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || !a_frame.groups)
					return;
				LoadConstants constants{};
				constants.outputIndex = a_recording.Index(a_bindings.output);
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch(a_frame.groups, 4, 1);
			}

		private:
			std::shared_ptr<Resources> resources;
		};

		struct State
		{
			std::shared_ptr<Resources> resources;
			bool armed = false;
		};

		State& Get()
		{
			static State state;
			return state;
		}

		bool OnGraph()
		{
			char value[32]{};
			return GpuTimingSource::SelectedTimers() == GpuTimingSource::Timers::Graph && GetEnvironmentVariableA("CS_GPU_LOAD", value, sizeof(value)) &&
			       std::strtoul(value, nullptr, 10) > 0;
		}
	}

	void ReserveAnchor()
	{
		if (OnGraph())
			FrameAnchors::Reserve(FrameAnchors::Anchor::Present);
	}

	bool Register(std::uint32_t a_groups)
	{
		auto& state = Get();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::Present)) {
			logger::warn("[PassSkip] the GPU load stays on D3D11: the render graph's Present anchor has no epoch");
			return false;
		}
		auto resources = std::make_shared<Resources>();
		resources->groups = a_groups;
		resources->program = ComputeProgram::Load(host->Device(), { .source = "RenderGraph/GpuLoadCS.hlsl", .constantWords = kConstantWords });
		if (!resources->program) {
			logger::warn("[PassSkip] the GPU load stays on D3D11: its render-graph program did not load");
			return false;
		}
		org::TextureDescription desc{};
		desc.imageDimensions = { { kTextureSize, kTextureSize, 0, 0 } };
		desc.channels = 1;
		desc.format = rhi::Format::R32_Float;
		desc.hasUAV = true;
		desc.uavFormat = desc.format;
		resources->output = org::PixelBuffer::CreateShared(desc);
		resources->output->SetName(fmt::format("{}.output", kClientId));

		FrameAnchors::ClientDesc client{};
		client.id = kClientId;
		client.passes.push_back({ .name = fmt::format("{}.dispatch", kClientId),
			.anchor = FrameAnchors::Anchor::Present,
			.kind = FrameAnchors::PassKind::Compute,
			.make = [resources] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<LoadPass>(resources)); } });
		client.registerResources = [resources](org::RenderGraph& a_graph) {
			a_graph.RegisterResource(org::ResourceIdentifier(resources->output->GetName()), resources->output);
		};
		// Armed for one Present at a time: a frame that did not arm it submits nothing.
		client.active = [] { return std::exchange(Get().armed, false); };
		client.onFailed = [] {
			Get().resources.reset();
			logger::error("[PassSkip] the GPU load stopped: the render graph failed");
		};
		if (!FrameAnchors::Register(std::move(client)))
			return false;
		state.resources = std::move(resources);
		logger::info("[PassSkip] GPU load: {} groups on the render graph (Present anchor)", a_groups);
		return true;
	}

	void Arm()
	{
		auto& state = Get();
		if (state.resources)
			state.armed = true;
	}
}

#else  // !CS_HAS_RENDER_GRAPH

#	include "RenderGraph/GpuLoadPass.h"

namespace GpuLoadPass
{
	void ReserveAnchor() {}
	bool Register(std::uint32_t) { return false; }
	void Arm() {}
}

#endif
