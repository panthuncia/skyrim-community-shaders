// The registered frame bindings' values and registers (see the header).

// Internal.h first: volk must precede every Vulkan header of the translation unit.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameBindings.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "RenderGraph/PublishedResources.h"

#include <Render/PassBuilders.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

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

	namespace
	{
		/** @brief Every frame texture's graph ids once each; with a_withView, only those of the textures with a view. */
		std::vector<std::string_view> GraphIdsOf(bool a_withView)
		{
			std::vector<std::string_view> ids;
			for (const auto& texture : RegisteredFrameTextures())
				if (!a_withView || texture.view)
					for (const auto id : texture.graphResources)
						if (!id.empty() && std::ranges::find(ids, id) == ids.end())
							ids.push_back(id);
			return ids;
		}

		/** @brief The ids a graph build declared (DeclareGraphTextures). Replaced whole, and only ever grows. */
		std::atomic<std::shared_ptr<const std::vector<std::string_view>>> s_declared;

		/** @brief Who a path log line names. */
		std::string GraphTextureWho(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name)
		{
			switch (a_reader) {
			case GraphTextureReader::Colour:
				return fmt::format("frame texture {} (t{})", a_name, a_register);
			case GraphTextureReader::Faces:
				return fmt::format("the faces' frame texture {} (t{})", a_name, a_register);
			default:
				return std::string(a_name);
			}
		}
	}

	std::span<const std::string_view> ColourGraphTextureIds()
	{
		static const auto s_ids = GraphIdsOf(false);
		return s_ids;
	}

	std::span<const std::string_view> FacesGraphTextureIds()
	{
		static const auto s_ids = GraphIdsOf(true);
		return s_ids;
	}

	void DeclareGraphTextures(org::RenderPassBuilder& a_builder, std::span<const std::string_view> a_ids)
	{
		const std::span<const org::SrvView> noViews{};
		const auto declared = s_declared.load(std::memory_order_acquire);
		std::vector<std::string_view> added;
		for (const auto id : a_ids) {
			// Only ids every graph registers: declaring another would fail the build. One skipped is built in once published.
			if (!PublishedResources::IsDeclarable(id))
				continue;
			a_builder.ShaderResource(org::ResourceIdentifier(id), noViews);
			if (!declared || std::ranges::find(*declared, id) == declared->end())
				added.push_back(id);
		}
		if (added.empty())
			return;
		auto next = std::make_shared<std::vector<std::string_view>>();
		if (declared)
			*next = *declared;
		for (const auto id : added)
			if (std::ranges::find(*next, id) == next->end())
				next->push_back(id);
		s_declared.store(std::move(next), std::memory_order_release);
	}

	bool GraphTextureIndex(GraphTextureReader a_reader, std::uint32_t a_register, std::string_view a_name, const ID3D11ShaderResourceView* a_view,
		std::span<const std::string_view> a_ids, std::uint32_t& a_index)
	{
		if (!a_view)
			return false;
		// The declared check comes first: it is cheap, and an undeclared id must never be read.
		const auto declared = s_declared.load(std::memory_order_acquire);
		std::string_view read;
		for (const auto id : a_ids) {
			if (id.empty() || !declared || std::ranges::find(*declared, id) == declared->end())
				continue;
			if (PublishedResources::IsExportOf(a_view, id) && PublishedResources::ShaderResourceIndex(id, a_index)) {
				read = id;
				break;
			}
		}
		// Each change of path, once per reader and register. Commits run on the render thread alone.
		constexpr auto kReaders = static_cast<std::size_t>(GraphTextureReader::Count);
		static std::array<std::array<std::string_view, kTextureRegisters>, kReaders> s_logged{};
		static std::array<std::array<bool, kTextureRegisters>, kReaders> s_seen{};
		const auto reader = static_cast<std::size_t>(a_reader);
		const auto slot = a_register < kTextureRegisters ? a_register : 0;
		if (!s_seen[reader][slot] || s_logged[reader][slot] != read) {
			s_seen[reader][slot] = true;
			s_logged[reader][slot] = read;
			const auto who = GraphTextureWho(a_reader, a_register, a_name);
			if (read.empty())
				logger::info("[DCLF] {} reads its D3D11 view", who);
			else
				logger::info("[DCLF] {} reads the graph resource '{}' (descriptor {})", who, read, a_index);
		}
		return !read.empty();
	}
}
