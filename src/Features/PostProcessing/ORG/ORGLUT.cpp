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
		// LUT_RESOURCES order.
		struct LutBlock
		{
			ShaderBindings::Handle texColor, texLut, texLut3D;
			ShaderBindings::Latch lut;
		};

		constexpr int kNoLut = -1;
		constexpr int kLut3D = 3;

		/**
		* @brief LUT::Draw on the graph, one draw over the dynamic-resolution area. A 2D LUT is imported from D3D11.
		* A 3D LUT is copied into a graph texture by a declared copy pass, since the anchors import 2D textures only.
		*/
		/** @brief One build's draw: its constants region, or nothing without a LUT. */
		class LUTRun final : public Run
		{
		public:
			LUTRun(LatchRegion<LUT::LUTCB> a_constants, bool a_drawn) :
				constants(a_constants), drawn(a_drawn) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				if (drawn)
					a_commit.Write(constants, static_cast<LUT&>(a_feature).Constants());
			}

		private:
			LatchRegion<LUT::LUTCB> constants;
			bool drawn = false;
		};

		/**
		* The LUT file (LutPath, a string) is no variable, so the loaded texture's identity is observed. A new file is a new
		* texture of its own shape and format, so it cannot be a declared texture's version.
		*/
		class LUTEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& lut = static_cast<LUT&>(a_feature);
				lutType = lut.LutType;
				a_key.push_back(static_cast<std::uint64_t>(lutType - kNoLut));
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
					source = FrameAnchors::ImmutableTexture("pp.lut", lut.texLUT2D->resource.get(), lut.texLUT2D->srv.get());
					if (!source)
						return "the LUT texture cannot be imported";
				}
				a_key.push_back(reinterpret_cast<std::uintptr_t>(source.Key()));
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				if (lutType != kNoLut)
					LoadProgram(a_programs);
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t>) override
			{
				// Observed in the key with the source it reads.
				if (lutType == kNoLut)
					return std::make_shared<LUTRun>(LatchRegion<LUT::LUTCB>{}, false);
				// As LUT::SetupResources, the output takes kMAIN_COPY's format.
				const auto format = a_builder.GetShape().mainCopyFormat;
				auto image = a_builder.CreateImage("PostProcessing::LUT::Output", format, D3D11_BIND_RENDER_TARGET);
				const auto constants = a_builder.Latch<LUT::LUTCB>();
				const bool is3D = lutType == kLut3D;
				if (is3D) {
					a_builder.Register(*copy);
					// The chain has no one-shot pass, so the copy of a few hundred KB runs every run.
					a_builder.Add("upload", FrameAnchors::PassKind::Copy, [staged = staged]() { return GraphResources::MakeUploadPass({ staged }); });
				}
				// The shader reads only the LUT LutType names.
				a_builder.Draw({ .name = "draw",
					.program = LoadProgram(a_builder),
					.bindings = { Bind::Srv(a_builder.Current()), is3D ? Bind::Unused() : Bind::Srv(source), is3D ? Bind::Srv(source) : Bind::Unused(),
						Bind::Latch(constants) },
					.target = image,
					.viewport = a_builder.DynamicViewport() });
				a_builder.SetCurrent(image);
				return std::make_shared<LUTRun>(constants, true);
			}

		private:
			/** @brief Listed in Programs and built on the programs lane. As LUT::SetupResources, the output takes kMAIN_COPY's format. */
			static std::shared_ptr<const RasterProgram> LoadProgram(ProgramSource& a_programs)
			{
				return a_programs.Raster("PostProcessing/LUT/lut.ps.hlsl", ShaderBindings::Words<LutBlock>(), { a_programs.GetShape().mainCopyFormat });
			}

			/**
			* @brief Re-reads the file's mip 0 on the CPU and stages it for the upload pass, so the graph orders and tracks the copy.
			* A D3D11 CopyResource would write a graph resource outside the graph. @return Null, or why it cannot be made.
			*/
			const char* CopyLut3D(const LUT& a_lut)
			{
				const auto& loaded = a_lut.texLUT3D->resource;
				if (copy && copiedFrom == loaded)
					return nullptr;
				copy.reset();
				staged.reset();
				copiedFrom = nullptr;
				// The same path LUT::ReadTexture resolves.
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
				// Written once and read only by the chain, so persistent and graph-only.
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
				staged = next->Stage(slices);
				if (!staged)
					return "the 3D LUT cannot be staged for its graph texture";
				copy = std::move(next);
				copiedFrom = loaded;
				return nullptr;
			}

			std::unique_ptr<GraphResources::Texture3D> copy;
			std::shared_ptr<const GraphResources::StagedUpload> staged;  // for the upload pass
			winrt::com_ptr<ID3D11Texture3D> copiedFrom;  // held so a new LUT never takes the old one's address
			FrameAnchors::Texture source;
			int lutType = kNoLut;  // this frame's, from Update
		};
	}

	std::unique_ptr<Effect> MakeLUT() { return std::make_unique<LUTEffect>(); }
}

#endif
