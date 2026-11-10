// The registered frame bindings' values and registers (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "RenderGraph/PublishedResources.h"

namespace DCLF::Extensions
{
	const std::shared_ptr<org::VersionedBuffer>* FrameBindingBuffer(std::size_t a_binding, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		const auto bindings = RegisteredFrameBindings();
		if (a_binding >= bindings.size())
			return nullptr;
		Api::FrameBindingSelect select{ a_scene, a_frame };
		bindings[a_binding].select(select);
		return select.buffer;
	}

	void FrameBindingValuesOf(const org::services::NamedVersions& a_sealed, const Draws::SceneBuffers& a_scene, std::uint32_t a_frame, FrameBindingValues& a_out,
		bool a_renderThread)
	{
		const auto bindings = RegisteredFrameBindings();
		for (std::size_t i = 0; i < bindings.size(); ++i) {
			Api::FrameBindingSelect select{ a_scene, a_frame, a_renderThread };
			bindings[i].select(select);
			a_out[i] = select.buffer && *select.buffer ? a_sealed.Of(**select.buffer).srvIndex :
			           a_renderThread && select.index != 0 ? select.index :
			                                                 kNoFrameBinding;
		}
	}

	void WriteFrameBindings(const FrameBindingValues& a_values, std::span<std::uint32_t> a_textures)
	{
		const auto bindings = RegisteredFrameBindings();
		for (std::size_t i = 0; i < bindings.size(); ++i)
			if (a_values[i] != kNoFrameBinding && bindings[i].textureRegister < a_textures.size())
				a_textures[bindings[i].textureRegister] = a_values[i];
	}

	bool FrameGraphBufferRegister(std::uint32_t a_register)
	{
		for (const auto& buffers : RegisteredFrameGraphBuffers())
			if (a_register >= buffers.firstRegister && a_register - buffers.firstRegister < buffers.count)
				return true;
		return false;
	}

	bool FrameGraphBufferIndices(std::size_t a_buffers, std::span<std::uint32_t, kFrameGraphBufferRegisters> a_out)
	{
		const auto registered = RegisteredFrameGraphBuffers();
		if (a_buffers >= registered.size())
			return false;
		const auto& buffers = registered[a_buffers];
		return buffers.indices(a_out.first(buffers.count));
	}

	bool FrameBindingRegister(std::uint32_t a_register)
	{
		for (const auto& binding : RegisteredFrameBindings())
			if (binding.textureRegister == a_register)
				return true;
		return false;
	}

	FrameGraphTextureSet FrameGraphTextures()
	{
		FrameGraphTextureSet onGraph;
		// A texture is declared while its resource is published, not while its callback answers true: the view a callback reads can
		// change within the frame (Cloud Shadows binds t26 in the reflections capture, after this check), and the commit asks again.
		for (const auto& texture : RegisteredFrameTextures())
			if (texture.graph && PublishedResources::IsPublished(texture.graphResource))
				onGraph.set(texture.textureRegister);
		return onGraph;
	}

	bool FrameGraphTextureIndex(std::size_t a_texture, const FrameGraphTextureSet& a_onGraph, const ID3D11ShaderResourceView* a_view,
		std::uint32_t& a_index)
	{
		const auto textures = RegisteredFrameTextures();
		if (a_texture >= textures.size() || !textures[a_texture].graph)
			return false;
		const auto& texture = textures[a_texture];
		// The view the commit captured decides too: when it is the published texture's own export, the D3D11 view would be an import
		// that nothing orders after the producer's graph writes. The commit resolves its textures after the capture, so a callback
		// that reads the producer's current bind can already see a later one.
		const bool capturedExport = a_view && PublishedResources::ExportedId(a_view) == texture.graphResource &&
		                            PublishedResources::ShaderResourceIndex(texture.graphResource, a_index);
		const bool onGraph = a_onGraph.test(texture.textureRegister) && (capturedExport || texture.graph(a_index));
		// Each change of path, once (the render thread's alone).
		static std::array<std::int8_t, kFrameTextureSlots> s_logged{};
		if (auto& logged = s_logged[a_texture]; logged != (onGraph ? 1 : -1)) {
			logged = onGraph ? 1 : -1;
			if (onGraph)
				logger::info("[DCLF] frame texture {} (t{}) reads the graph resource '{}' (descriptor {})", texture.name, texture.textureRegister,
					texture.graphResource, a_index);
			else
				logger::info("[DCLF] frame texture {} (t{}) reads its D3D11 view", texture.name, texture.textureRegister);
		}
		return onGraph;
	}
}
