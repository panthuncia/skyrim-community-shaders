#include "RenderGraph/Frame/FrameResources.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include <map>
#	include <string>

namespace RenderGraph::Frame::Resources
{
	namespace
	{
		struct Entry
		{
			std::shared_ptr<GraphResources::Texture2D> texture;
			GraphResources::Lifetime lifetime = GraphResources::Lifetime::Transient;
		};

		std::map<std::string, Entry, std::less<>>& Entries()
		{
			static std::map<std::string, Entry, std::less<>> entries;
			return entries;
		}

		/** @brief Everything but the size, which a new version of a versioned texture must keep. */
		bool SameClass(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
		{
			return a_left.Format == a_right.Format && a_left.MipLevels == a_right.MipLevels && a_left.ArraySize == a_right.ArraySize &&
			       a_left.SampleDesc.Count == a_right.SampleDesc.Count && a_left.BindFlags == a_right.BindFlags && a_left.MiscFlags == a_right.MiscFlags &&
			       a_left.Usage == a_right.Usage && a_left.CPUAccessFlags == a_right.CPUAccessFlags;
		}
	}

	Made Texture2D(std::string_view a_name, const D3D11_TEXTURE2D_DESC& a_desc, GraphResources::Lifetime a_lifetime)
	{
		auto& entries = Entries();
		const bool versioned = a_lifetime != GraphResources::Lifetime::Transient;
		if (const auto found = entries.find(a_name); found != entries.end() && found->second.texture && *found->second.texture &&
													 found->second.lifetime == a_lifetime) {
			auto& texture = *found->second.texture;
			const auto& desc = texture.Desc();
			if (SameClass(desc, a_desc) && desc.Width == a_desc.Width && desc.Height == a_desc.Height) {
				// Asking for its own size again drops an earlier size request.
				if (versioned)
					texture.RequestSize(a_desc.Width, a_desc.Height);
				return { found->second.texture, false };
			}
			// A new size becomes the slot's next version at the next frame boundary, before the new registration's build can switch in.
			if (versioned && SameClass(desc, a_desc)) {
				texture.RequestSize(a_desc.Width, a_desc.Height);
				logger::info("[ORG] frame resources: {} takes {}x{} at the next frame boundary (a new version of its slot)", a_name, a_desc.Width, a_desc.Height);
				return { found->second.texture, true };
			}
		}
		auto texture = std::make_shared<GraphResources::Texture2D>(a_desc, std::string(a_name), GraphResources::Options{ .lifetime = a_lifetime, .versioned = versioned });
		if (!*texture) {
			logger::error("[ORG] frame resources: {} could not be made ({}x{}, format {})", a_name, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
			return {};
		}
		logger::info("[ORG] frame resources: {} made ({}x{}, format {}, {})", a_name, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format),
			versioned ? "kept across frames, versioned" : "transient");
		entries[std::string(a_name)] = { texture, a_lifetime };
		return { std::move(texture), true };
	}
}

#else

namespace RenderGraph::Frame::Resources
{
	Made Texture2D(std::string_view, const D3D11_TEXTURE2D_DESC&, GraphResources::Lifetime) { return {}; }
}

#endif
