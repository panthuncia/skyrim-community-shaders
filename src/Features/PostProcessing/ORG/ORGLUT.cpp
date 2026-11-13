#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/LUT.h"
#	include "Presets/UnifiedPresetCatalog.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <DirectXTex.h>
#	include <Resources/ExternalTextureResource.h>

#	include <vector>

namespace ORGPostChain
{
	namespace
	{
		// lut.ps.hlsl's push block: LUT_RESOURCES, in order.
		struct LutBlock
		{
			ShaderBindings::Handle texColor, texLut, texLut3D;
			ShaderBindings::Latch lut;
		};

		constexpr int kNoLut = -1;
		constexpr int kLut3D = 3;

		/**
		 * @brief LUT::Draw on the graph: one fullscreen draw over the dynamic-resolution area. The LUT stays a D3D11 texture loaded
		 * by LUT::ReadTexture: a 2D one is imported (FrameAnchors::D3D11Texture), a 3D one is written once into a graph texture
		 * through the graph's upload pass (the anchors import 2D textures only). No LUT loaded: nothing drawn, as on D3D11.
		 */
		class LUTEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& lut = static_cast<LUT&>(a_feature);
				a_key.push_back(static_cast<std::uint64_t>(lut.LutType - kNoLut));
				source = nullptr;
				if (lut.LutType == kNoLut)
					return nullptr;
				if (lut.LutType == kLut3D) {
					if (!lut.texLUT3D || !lut.texLUT3D->resource)
						return "the 3D LUT is not loaded";
					if (const char* why = CopyLut3D(lut))
						return why;
					source = copy->Graph();
				} else {
					if (!lut.texLUT2D || !lut.texLUT2D->resource)
						return "the LUT is not loaded";
					source = FrameAnchors::D3D11Texture("pp.lut", lut.texLUT2D->resource.get(), lut.texLUT2D->srv.get(), nullptr);
					if (!source)
						return "the LUT texture cannot be imported";
				}
				a_key.push_back(reinterpret_cast<std::uintptr_t>(source.get()));
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature& a_feature) override
			{
				lutType = static_cast<LUT&>(a_feature).LutType;
				if (lutType == kNoLut)
					return;
				// As LUT::SetupResources: the output takes kMAIN_COPY's format.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto image = a_builder.CreateImage("PostProcessing::LUT::Output", format, D3D11_BIND_RENDER_TARGET);
				constants = a_builder.Latch<LUT::LUTCB>();
				const bool is3D = lutType == kLut3D;
				if (is3D)
					a_builder.Register(*copy);
				// The shader reads only the LUT LutType names; the other handle is never read.
				a_builder.Draw({ .name = "draw",
					.program = a_builder.Raster("PostProcessing/LUT/lut.ps.hlsl", ShaderBindings::Words<LutBlock>(), { format }),
					.bindings = { Bind::Srv(a_builder.Current()), is3D ? Bind::Unused() : Bind::Srv(source), is3D ? Bind::Srv(source) : Bind::Unused(),
						Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				if (lutType != kNoLut)
					a_commit.Write(constants, static_cast<LUT&>(a_feature).Constants());
			}

		private:
			/**
			 * @brief The 3D LUT's graph copy, made again when LUT::ReadTexture loaded another. The file's mip 0 (all the shader
			 * loads) is read again on the CPU and written through the graph's upload pass, which declares the copy a copy
			 * destination: the graph orders it before the draw and tracks the texture. A D3D11 CopyResource would write a graph
			 * resource outside the graph. @return Null, or why it cannot be made.
			 */
			const char* CopyLut3D(const LUT& a_lut)
			{
				const auto& loaded = a_lut.texLUT3D->resource;
				if (copy && copiedFrom == loaded)
					return nullptr;
				copy.reset();
				copiedFrom = nullptr;
				// The file LUT::ReadTexture loaded: the same path, resolved the same way.
				const auto path = UnifiedPresetCatalog::GetSingleton().ResolveActivePackPath(a_lut.attemptedPath);
				DirectX::ScratchImage image;
				if (FAILED(DirectX::LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image)))
					return "the 3D LUT file cannot be read again for its graph copy";
				const auto& metadata = image.GetMetadata();
				if (metadata.dimension != DirectX::TEX_DIMENSION_TEXTURE3D)
					return "the 3D LUT file is no longer a 3D texture";
				D3D11_TEXTURE3D_DESC desc{};
				desc.Width = static_cast<UINT>(metadata.width);
				desc.Height = static_cast<UINT>(metadata.height);
				desc.Depth = static_cast<UINT>(metadata.depth);
				desc.MipLevels = 1;
				desc.Format = metadata.format;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				// Written once and read only by the chain: persistent, and graph-only.
				auto next = std::make_unique<GraphResources::Texture3D>(desc, "PostProcessing::LUT::Lut3D",
					GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent });
				if (!*next)
					return "the 3D LUT cannot be copied into a graph texture";
				std::vector<GraphResources::SubresourceData> slices;
				for (std::size_t z = 0; z < metadata.depth; ++z) {
					const auto* slice = image.GetImage(0, 0, z);
					if (!slice)
						return "the 3D LUT file has a missing slice";
					slices.push_back({ slice->pixels, static_cast<UINT>(slice->rowPitch), static_cast<UINT>(slice->slicePitch) });
				}
				if (!next->Upload(slices))
					return "the 3D LUT cannot be uploaded into its graph texture";
				copy = std::move(next);
				copiedFrom = loaded;
				return nullptr;
			}

			std::unique_ptr<GraphResources::Texture3D> copy;
			winrt::com_ptr<ID3D11Texture3D> copiedFrom;  // held, so a new LUT never takes the old one's address
			std::shared_ptr<org::Resource> source;
			LatchRegion<LUT::LUTCB> constants;
			int lutType = kNoLut;
		};
	}

	std::unique_ptr<Effect> MakeLUT() { return std::make_unique<LUTEffect>(); }
}

#endif
