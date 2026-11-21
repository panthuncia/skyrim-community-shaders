// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/UavClearPass.h"

#include "RenderGraph/AnchorPasses.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/RunGate.h"

#include <Render/PassBuilders.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>

#include <cstdint>
#include <cstring>
#include <vector>

namespace
{
	struct ClearBindings
	{
		org::ResourceBindingToken resource;
		org::DeclaredViewToken shaderVisible, cpuVisible;
	};

	struct ClearFrame
	{
		std::array<float, 4> value{};
		std::array<std::uint32_t, 4> uintValue{};
		bool asUint = false;
		GraphPasses::ClearGate gate;
	};

	class UavClearPass final : public org::TypedRenderGraphPass<UavClearPass, ClearFrame, ClearBindings>
	{
	public:
		UavClearPass(FrameAnchors::Texture a_texture, std::array<float, 4> a_value, GraphPasses::ClearGate a_gate) :
			texture(std::move(a_texture)), value(a_value), gate(std::move(a_gate)) {}

		UavClearPass(FrameAnchors::Texture a_texture, std::array<std::uint32_t, 4> a_value, GraphPasses::ClearGate a_gate) :
			texture(std::move(a_texture)), uintValue(a_value), asUint(true), gate(std::move(a_gate)) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = a_builder.UnorderedAccessClear(texture);
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(texture.Key()));
			for (const float component : value) {
				std::uint32_t bits = 0;
				std::memcpy(&bits, &component, sizeof(bits));
				a_out.push_back(bits);
			}
			for (const std::uint32_t component : uintValue)
				a_out.push_back(component);
			a_out.push_back(asUint ? 1 : 0);
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { value, uintValue, asUint, gate }; }

		// Its commands depend on the run only through SkipsAtExecution, so a kept late batch keeps its list.
		static constexpr bool kRecordKeyedBySkip = true;

		// A clear whose gate entry the run left closed is not recorded.
		static bool SkipsAtExecution(const ClearBindings&, const ClearFrame& a_frame, const org::PassRecordContext& a_recording)
		{
			return a_frame.gate.gate && !a_frame.gate.gate->IsOpen(a_recording.Run(), a_frame.gate.entry);
		}

		static void Record(const ClearBindings& a_bindings, const ClearFrame& a_frame, org::PassRecordContext& a_recording)
		{
			rhi::UavClearInfo info{};
			info.shaderVisible = a_recording.View(a_bindings.shaderVisible);
			info.cpuVisible = a_recording.View(a_bindings.cpuVisible);
			info.resource = a_recording.Resolve(a_bindings.resource);
			if (a_frame.asUint)
				a_recording.Commands().ClearUavUint(info, rhi::UavClearUint(a_frame.uintValue));
			else
				a_recording.Commands().ClearUavFloat(info, rhi::UavClearFloat(a_frame.value));
		}

	private:
		FrameAnchors::Texture texture;
		std::array<float, 4> value{};
		std::array<std::uint32_t, 4> uintValue{};
		bool asUint = false;
		GraphPasses::ClearGate gate;
	};
}

namespace GraphPasses
{
	std::shared_ptr<org::RenderPass> MakeUavClear(FrameAnchors::Texture a_texture, std::array<float, 4> a_value, ClearGate a_gate)
	{
		return std::make_shared<UavClearPass>(std::move(a_texture), a_value, std::move(a_gate));
	}

	std::shared_ptr<org::RenderPass> MakeUavClearUint(FrameAnchors::Texture a_texture, std::array<std::uint32_t, 4> a_value, ClearGate a_gate)
	{
		return std::make_shared<UavClearPass>(std::move(a_texture), a_value, std::move(a_gate));
	}

}

