// The graph texture test (see the header): the copy pass at the Prepass anchor, its cubemap and its client.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Integrations/DynamicCubemaps/ReflectionsGraphTest.h"

#include "Features/DynamicCubemaps.h"
#include "Globals.h"
#include "RenderGraph/DxvkImageImport.h"
#include "RenderGraph/DxvkOrgInterop.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RenderGraphRuntime.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/TextureDescription.h>
#include <rhi_helpers.h>

#include <map>

namespace DCLF::Integrations::DynamicCubemaps::ReflectionsGraphTest
{
	namespace
	{
		constexpr const char* kClientId = "cs.dclf.test.reflections";
		constexpr const char* kTextureName = "DCLF::GraphTextureTest";
		constexpr const char* kSourceId = "cs.dclf.test.reflections.source";
		constexpr std::uint32_t kFaces = 6;

		/** @brief The client's state: the published cubemap, the source the pass copies from, and the sources imported so far. */
		struct Test
		{
			std::unique_ptr<GraphResources::Texture2D> target;
			std::shared_ptr<org::ExternalTextureResource> source;  // what the pass was made with; null: no copy
			std::map<ID3D11Texture2D*, std::pair<winrt::com_ptr<ID3D11Texture2D>, std::shared_ptr<org::ExternalTextureResource>>> imports;
		};

		struct Status
		{
			Mode mode = Mode::Off;
			bool attempted = false;
			std::shared_ptr<Test> test;
		};

		Status& State()
		{
			static Status s_status;
			return s_status;
		}

		/** @brief The D3D11 texture behind the view Dynamic Cubemaps last bound at t30, or null. */
		ID3D11Texture2D* BoundSource(winrt::com_ptr<ID3D11Texture2D>& a_texture)
		{
			const auto& dynamicCubemaps = globals::features::dynamicCubemaps;
			auto* view = dynamicCubemaps.loaded ? dynamicCubemaps.boundReflectionsView : nullptr;
			if (!view)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			a_texture = resource.try_as<ID3D11Texture2D>();
			return a_texture.get();
		}

		/** @brief The cubemap's description: the source's shape, read by SRVs only. */
		D3D11_TEXTURE2D_DESC TargetDesc(const D3D11_TEXTURE2D_DESC& a_source)
		{
			D3D11_TEXTURE2D_DESC desc = a_source;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
			desc.CPUAccessFlags = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			return desc;
		}

		bool SameShape(const D3D11_TEXTURE2D_DESC& a_left, const D3D11_TEXTURE2D_DESC& a_right)
		{
			return a_left.Width == a_right.Width && a_left.Height == a_right.Height && a_left.MipLevels == a_right.MipLevels &&
			       a_left.ArraySize == a_right.ArraySize && a_left.Format == a_right.Format;
		}

		/** @brief The source as a graph cubemap whose home is the layout DXVK rests it in, imported once per texture. */
		std::shared_ptr<org::ExternalTextureResource> Import(Test& a_test, ID3D11Texture2D* a_texture)
		{
			if (const auto found = a_test.imports.find(a_texture); found != a_test.imports.end())
				return found->second.second;
			auto& runtime = RenderGraphRuntime::Get();
			DxvkOrgInteropResourceInfo info{};
			if (!runtime.DescribeResource(a_texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE) {
				logger::warn("[DCLF] graph texture test: the reflections cubemap cannot be described");
				return nullptr;
			}
			D3D11_TEXTURE2D_DESC textureDesc{};
			a_texture->GetDesc(&textureDesc);
			org::TextureDescription desc{};
			desc.format = rhi::helpers::ToRHI(textureDesc.Format);
			desc.channels = 4;
			desc.hasSRV = true;
			desc.srvFormat = desc.format;
			desc.isCubemap = true;
			desc.arraySize = 1;
			for (UINT mip = 0; mip < textureDesc.MipLevels; ++mip)
				desc.imageDimensions.push_back({ (std::max)(textureDesc.Width >> mip, 1u), (std::max)(textureDesc.Height >> mip, 1u), 0, 0 });
			auto resource = RenderGraph::ImportDxvkTexture(runtime.Host()->Device(), info.image, std::move(desc), kSourceId);
			if (!resource) {
				logger::warn("[DCLF] graph texture test: the reflections cubemap could not be imported");
				return nullptr;
			}
			winrt::com_ptr<ID3D11Texture2D> held;
			held.copy_from(a_texture);
			a_test.imports.emplace(a_texture, std::pair{ std::move(held), resource });
			return resource;
		}

		struct CopyBindings
		{
			org::ResourceBindingToken source;
			org::ResourceBindingToken target;
		};

		struct CopyFrame
		{
			UINT size = 0;
			UINT mips = 0;
		};

		/** @brief Every face and mip of the source into the published cubemap. */
		class CopyPass final : public org::TypedRenderGraphPass<CopyPass, CopyFrame, CopyBindings>
		{
		public:
			CopyPass(std::shared_ptr<org::Resource> a_source, std::shared_ptr<org::Resource> a_target, UINT a_size, UINT a_mips) :
				source(std::move(a_source)), target(std::move(a_target)), size(a_size), mips(a_mips) {}

			CopyBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopySource(source), a_builder.CopyDestination(target) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(source.get()));
				a_out.push_back(reinterpret_cast<std::uintptr_t>(target.get()));
			}

			CopyFrame Prepare(const CopyBindings&, const org::PassPrepareContext&) const { return { size, mips }; }

			static void Record(const CopyBindings& a_bindings, const CopyFrame& a_frame, org::PassRecordContext& a_recording)
			{
				const auto from = a_recording.Resolve(a_bindings.source).GetHandle();
				const auto to = a_recording.Resolve(a_bindings.target).GetHandle();
				auto& commands = a_recording.Commands();
				for (std::uint32_t face = 0; face < kFaces; ++face)
					for (std::uint32_t mip = 0; mip < a_frame.mips; ++mip) {
						const std::uint32_t extent = (std::max)(a_frame.size >> mip, 1u);
						commands.CopyTextureRegion({ .texture = to, .mip = mip, .arraySlice = face, .width = extent, .height = extent },
							{ .texture = from, .mip = mip, .arraySlice = face, .width = extent, .height = extent });
					}
			}

		private:
			std::shared_ptr<org::Resource> source;
			std::shared_ptr<org::Resource> target;
			UINT size;
			UINT mips;
		};

		/**
		 * @brief At the hook, before the epoch: the source Dynamic Cubemaps bound now. A different one than the pass was made with rebuilds
		 * the anchor's graph with it. False (no copy this frame) without one of the cubemap's shape.
		 */
		bool Track(Test& a_test)
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			auto* bound = BoundSource(texture);
			D3D11_TEXTURE2D_DESC desc{};
			if (bound)
				bound->GetDesc(&desc);
			auto source = bound && SameShape(desc, a_test.target->Desc()) ? Import(a_test, bound) : nullptr;
			if (source != a_test.source) {
				a_test.source = std::move(source);
				FrameAnchors::Refresh(kClientId);
			}
			return a_test.source != nullptr;
		}

		FrameAnchors::ClientDesc MakeClient(const std::shared_ptr<Test>& a_test)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			client.passes.push_back({ .name = std::string(kClientId) + ".copy",
				.anchor = FrameAnchors::Anchor::Prepass,
				.kind = FrameAnchors::PassKind::Copy,
				.make = [a_test]() -> std::shared_ptr<org::RenderPass> {
					if (!a_test->source || !*a_test->target)
						return nullptr;
					const auto& desc = a_test->target->Desc();
					return std::make_shared<CopyPass>(a_test->source, a_test->target->Graph(), desc.Width, desc.MipLevels);
				} });
			client.registerResources = [a_test](org::RenderGraph& a_graph) {
				a_test->target->Register(a_graph);
				if (a_test->source)
					a_graph.RegisterResource(org::ResourceIdentifier(kSourceId), a_test->source);
			};
			client.active = [a_test] { return Track(*a_test); };
			// The graph stopped: t30 returns to the cubemap's export (last copied), the published texture is withdrawn.
			client.onFailed = [] {
				PublishedResources::Withdraw(kResourceId);
				logger::error("[DCLF] graph texture test: the render graph failed; t30 reads the test cubemap's D3D11 view");
			};
			return client;
		}

		/** @brief Once Dynamic Cubemaps bound its cubemap: the test's cubemap, published, and its client. */
		void Ensure()
		{
			auto& state = State();
			if (state.mode == Mode::Off || state.attempted || !RenderGraphRuntime::Get().IsActive())
				return;
			winrt::com_ptr<ID3D11Texture2D> texture;
			auto* bound = BoundSource(texture);
			if (!bound)
				return;  // tried again at the next check
			state.attempted = true;
			if (!FrameAnchors::HasEpoch(FrameAnchors::Anchor::Prepass)) {
				logger::error("[DCLF] graph texture test: the Prepass anchor has no epoch; off");
				return;
			}
			D3D11_TEXTURE2D_DESC source{};
			bound->GetDesc(&source);
			auto test = std::make_shared<Test>();
			// Written every frame before the colour passes read it, but read in other epochs: exported, so never aliased.
			test->target = std::make_unique<GraphResources::Texture2D>(TargetDesc(source), kTextureName,
				GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
			if (!*test->target || !test->target->SRV()) {
				logger::error("[DCLF] graph texture test: the test cubemap could not be created; off");
				return;
			}
			if (!PublishedResources::Publish(kResourceId, *test->target) || !FrameAnchors::Register(MakeClient(test))) {
				logger::error("[DCLF] graph texture test: the test cubemap could not be published or its client registered; off");
				PublishedResources::Withdraw(kResourceId);
				return;
			}
			state.test = std::move(test);
			logger::info("[DCLF] graph texture test: on ({}), t30 reads '{}' ({}x{}, {} mips), copied at the Prepass anchor",
				state.mode == Mode::Graph ? "through the graph" : "through its D3D11 view", kResourceId, source.Width, source.Height, source.MipLevels);
		}
	}

	void Install(Mode a_mode)
	{
		auto& state = State();
		state.mode = a_mode;
		if (a_mode != Mode::Off && !FrameAnchors::Reserve(FrameAnchors::Anchor::Prepass))
			logger::warn("[DCLF] graph texture test: the Prepass anchor could not be reserved");
	}

	Mode CurrentMode()
	{
		return State().mode;
	}

	void* View()
	{
		const auto& test = State().test;
		return test && test->target ? test->target->SRV() : nullptr;
	}

	bool GraphIndex(std::uint32_t& a_index)
	{
		Ensure();
		return State().mode == Mode::Graph && State().test && PublishedResources::ShaderResourceIndex(kResourceId, a_index);
	}
}
