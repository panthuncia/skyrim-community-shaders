// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "RenderGraph/ImGuiBasicRHI.h"
#include "RenderGraph/Engine/UiTargets.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/Revision.h"
#include "RenderGraph/FrameAnchors.h"

#include "RenderGraph/GraphResources.h"
#include "RenderGraph/ProgramService.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/Relocation/Holders.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/UndeclaredAccess.h"

#include <Resources/PixelBuffer.h>
#include <Resources/VersionedTexture.h>
#include <rhi_helpers.h>

#include <mutex>
#include <string>
#include <string_view>

namespace ImGuiBasicRHI
{
	namespace
	{
		namespace Imgui = org::services::imgui;

		/** @brief What the pipeline is built from: the target format and its description. Equal when the formats are. */
		struct ProgramKey
		{
			std::uint32_t format = 0;  // rhi::Format, as its value
			RasterProgram::Desc desc;

			bool operator==(const ProgramKey& a_other) const { return format == a_other.format; }
		};

		/** @brief Keyed on the interface's target format, built on the programs lane. */
		RenderGraph::Frame::DeclaredProgram<RasterProgram>& Program()
		{
			static RenderGraph::Frame::DeclaredProgram<RasterProgram> s_program;
			return s_program;
		}

		/** @brief A persistent graph-only RGBA8 texture, a GraphResources twin. A null image when it is not a texture. */
		Imgui::TextureStorage CreateTexture(std::uint32_t a_width, std::uint32_t a_height, std::uint32_t a_mips, const std::string& a_name)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = a_mips;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			auto twin = std::make_shared<GraphResources::Texture2D>(desc, a_name, GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
			// The backend makes raw views of its textures (createShaderResourceView). They would not follow a move.
			if (twin->Graph())
				::RenderGraph::Relocation::Pin(*twin->Graph());
			auto pixels = std::dynamic_pointer_cast<org::PixelBuffer>(twin->Graph());
			return { std::move(pixels), std::move(twin) };
		}

		void Log(Imgui::LogLevel a_level, std::string_view a_message)
		{
			switch (a_level) {
			case Imgui::LogLevel::Info:
				logger::info("[ORG] {}", a_message);
				break;
			case Imgui::LogLevel::Warning:
				logger::warn("[ORG] {}", a_message);
				break;
			case Imgui::LogLevel::Error:
				logger::error("[ORG] {}", a_message);
				break;
			}
		}

		/** @brief Gives the backend CS's host, once, before its first use. */
		void EnsureHost()
		{
			static std::once_flag s_once;
			std::call_once(s_once, [] {
				Imgui::HostDesc host;
				host.graphHost = [] { return RenderGraphRuntime::Get().Host(); };
				host.createTexture = &CreateTexture;
				host.createShaderResourceView = [](rhi::Resource a_resource, std::shared_ptr<const void> a_owner, const rhi::SrvDesc& a_desc) {
					return ShaderBindings::CreateShaderResourceView(a_resource, std::move(a_owner), a_desc);
				};
				host.createSampler = [](const rhi::SamplerDesc& a_desc) { return ShaderBindings::CreateSampler(a_desc); };
				host.publishPageVersion = [](const std::shared_ptr<org::VersionedTexture>& a_page, std::shared_ptr<org::GloballyIndexedResource> a_image,
											  std::shared_ptr<const void> a_owner) {
					RenderGraph::Frame::Revision::PublishTexture(a_page, std::move(a_image), std::move(a_owner));
				};
				host.noteImmutable = [](const std::shared_ptr<org::Resource>& a_resource) {
					if (RenderGraph::UndeclaredAccess::On())
						RenderGraph::UndeclaredAccess::NoteImmutable(a_resource);
				};
				host.onError = RenderGraph::FatalHandler("ImGui");
				host.log = &Log;
				host.passName = "CS ImGui";
				Imgui::SetHost(std::move(host));
			});
		}
	}

	void Init()
	{
		EnsureHost();
		Imgui::Init();
	}

	void InvalidateTextures() { Imgui::InvalidateTextures(); }

	void Shutdown()
	{
		if (!Imgui::Installed())
			return;
		Imgui::Shutdown();
		Program() = {};
	}

	bool Installed() { return Imgui::Installed(); }

	bool IsOwnTexture(std::uint64_t a_texture) { return Imgui::IsOwnTexture(a_texture); }

	std::shared_ptr<Texture> MakeTexture(std::uint32_t a_width, std::uint32_t a_height, std::span<const std::span<const std::byte>> a_levels, const char* a_name)
	{
		EnsureHost();
		return Imgui::MakeTexture(a_width, a_height, a_levels, a_name);
	}

	Binding BindingOf(const std::shared_ptr<Texture>& a_texture, int a_mip) { return Imgui::BindingOf(a_texture, a_mip); }

	void DeclareProgram()
	{
		namespace RF = RenderGraph::Frame;
		// The description is made on the render thread, when a revision starts. The build runs on the programs lane.
		Program() = RF::DeclareProgram(
			"ImGui", RF::DependsOn(RF::Vars::HdrEnabled()),
			[](const RF::Inputs& a_in) {
				const auto format = static_cast<std::uint32_t>(rhi::helpers::ToRHI(RenderGraph::Engine::UiTargets::Current(a_in).uiFormat));
				ProgramKey key{ format, Imgui::ProgramDesc(static_cast<rhi::Format>(format)) };
				RenderGraph::Programs::ResolveDefines(key.desc);
				return key;
			},
			[](const ProgramKey& a_key) { return RenderGraph::Programs::Load(a_key.desc); });
	}

	std::shared_ptr<const RasterProgram> ProgramFor(const RenderGraph::Frame::Inputs& a_in)
	{
		const auto& program = Program();
		return program ? program.For(a_in) : nullptr;
	}

	bool Commit(ImDrawData* a_drawData, const Target& a_target, const Resolver& a_resolve)
	{
		EnsureHost();
		return Imgui::Commit(a_drawData, a_target, a_resolve);
	}

	std::shared_ptr<org::RenderPass> MakePass(FrameAnchors::Texture a_target)
	{
		EnsureHost();
		if (!a_target)
			return nullptr;
		return Imgui::MakePass({ a_target.Slot(), a_target.Plain() });
	}

	std::shared_ptr<org::RenderPass> MakeUploadPass()
	{
		EnsureHost();
		return Imgui::MakeUploadPass();
	}
}
