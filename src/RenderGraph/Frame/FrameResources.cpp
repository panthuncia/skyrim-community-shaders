#include "RenderGraph/Frame/FrameResources.h"

#if defined(CS_HAS_RENDER_GRAPH)

#	include <map>
#	include <string>

namespace RenderGraph::Frame::Resources
{
	namespace
	{
		std::map<std::string, std::shared_ptr<GraphResources::Texture2D>, std::less<>>& Entries()
		{
			static std::map<std::string, std::shared_ptr<GraphResources::Texture2D>, std::less<>> entries;
			return entries;
		}

		bool SameDesc(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
		{
			return a_left.Width == a_right.Width && a_left.Height == a_right.Height && a_left.Format == a_right.Format && a_left.MipLevels == a_right.MipLevels &&
			       a_left.ArraySize == a_right.ArraySize && a_left.SampleDesc.Count == a_right.SampleDesc.Count && a_left.BindFlags == a_right.BindFlags &&
			       a_left.MiscFlags == a_right.MiscFlags && a_left.Usage == a_right.Usage && a_left.CPUAccessFlags == a_right.CPUAccessFlags;
		}
	}

	std::shared_ptr<GraphResources::Texture2D> Texture2D(std::string_view a_name, const D3D11_TEXTURE2D_DESC& a_desc)
	{
		auto& entries = Entries();
		if (const auto found = entries.find(a_name); found != entries.end() && found->second && *found->second &&
													 SameDesc(found->second->Desc(), a_desc))
			return found->second;
		auto texture = std::make_shared<GraphResources::Texture2D>(a_desc, std::string(a_name), GraphResources::Options{ .lifetime = GraphResources::Lifetime::Transient });
		if (!*texture) {
			logger::error("[ORG] frame resources: {} could not be made ({}x{}, format {})", a_name, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
			return nullptr;
		}
		logger::info("[ORG] frame resources: {} made ({}x{}, format {}, transient)", a_name, a_desc.Width, a_desc.Height, static_cast<int>(a_desc.Format));
		entries[std::string(a_name)] = texture;
		return texture;
	}
}

#else

namespace RenderGraph::Frame::Resources
{
	std::shared_ptr<GraphResources::Texture2D> Texture2D(std::string_view, const D3D11_TEXTURE2D_DESC&) { return nullptr; }
}

#endif
