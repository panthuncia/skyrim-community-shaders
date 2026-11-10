#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/UavClearPass.h"

#	include <Render/PassBuilders.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <cstdint>
#	include <cstring>
#	include <vector>

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
	};

	class UavClearPass final : public org::TypedRenderGraphPass<UavClearPass, ClearFrame, ClearBindings>
	{
	public:
		UavClearPass(std::shared_ptr<org::Resource> a_texture, std::array<float, 4> a_value) :
			texture(std::move(a_texture)), value(a_value) {}

		UavClearPass(std::shared_ptr<org::Resource> a_texture, std::array<std::uint32_t, 4> a_value) :
			texture(std::move(a_texture)), uintValue(a_value), asUint(true) {}

		ClearBindings Declare(org::PassBuilder& a_builder)
		{
			a_builder.PreferQueue(org::QueueKind::Graphics);
			const auto use = a_builder.UnorderedAccessClear(texture);
			return { use.resources.front(), use.views[0], use.views[1] };
		}

		void Revision(std::vector<std::uint64_t>& a_out) const
		{
			a_out.push_back(reinterpret_cast<std::uintptr_t>(texture.get()));
			for (const float component : value) {
				std::uint32_t bits = 0;
				std::memcpy(&bits, &component, sizeof(bits));
				a_out.push_back(bits);
			}
			for (const std::uint32_t component : uintValue)
				a_out.push_back(component);
			a_out.push_back(asUint ? 1 : 0);
		}

		ClearFrame Prepare(const ClearBindings&, const org::PassPrepareContext&) const { return { value, uintValue, asUint }; }

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
		std::shared_ptr<org::Resource> texture;
		std::array<float, 4> value{};
		std::array<std::uint32_t, 4> uintValue{};
		bool asUint = false;
	};
}

namespace GraphPasses
{
	std::shared_ptr<org::RenderPass> MakeUavClear(std::shared_ptr<org::Resource> a_texture, std::array<float, 4> a_value)
	{
		return std::make_shared<UavClearPass>(std::move(a_texture), a_value);
	}

	std::shared_ptr<org::RenderPass> MakeUavClearUint(std::shared_ptr<org::Resource> a_texture, std::array<std::uint32_t, 4> a_value)
	{
		return std::make_shared<UavClearPass>(std::move(a_texture), a_value);
	}
}

#endif
