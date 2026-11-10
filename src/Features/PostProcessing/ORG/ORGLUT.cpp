#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing/LUT.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <Resources/ExternalTextureResource.h>

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
		 * by LUT::ReadTexture: a 2D one is imported (FrameAnchors::D3D11Texture), a 3D one is copied once into a graph texture (the
		 * anchors import 2D textures only). No LUT loaded: nothing drawn, as on D3D11.
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
					if (const char* why = CopyLut3D(*lut.texLUT3D))
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
			/** @brief The 3D LUT's graph copy, made again when LUT::ReadTexture loaded another. @return Null, or why it cannot be made. */
			const char* CopyLut3D(const Texture3D& a_lut)
			{
				if (copy && copiedFrom == a_lut.resource)
					return nullptr;
				copy.reset();
				copiedFrom = nullptr;
				D3D11_TEXTURE3D_DESC desc = a_lut.desc;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.CPUAccessFlags = 0;
				desc.MiscFlags = 0;
				// Filled once by D3D11 and read by the chain: persistent, and exported for that copy.
				auto next = std::make_unique<GraphResources::Texture3D>(desc, "PostProcessing::LUT::Lut3D",
					GraphResources::Options{ .lifetime = GraphResources::Lifetime::Persistent, .d3d11 = GraphResources::Export::SRV });
				if (!*next || !next->D3D11())
					return "the 3D LUT cannot be copied into a graph texture";
				// In the D3D11 stream before the anchor's epoch, which reads it.
				globals::d3d::context->CopyResource(next->D3D11(), a_lut.resource.get());
				copy = std::move(next);
				copiedFrom = a_lut.resource;
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
