#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing.h"
#	include "Features/PostProcessing/DoF.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"

#	include <Resources/ExternalTextureResource.h>

#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/DoF/dof.cs.hlsl";
		// As DoF::SetupResources: the CoC in R16F, the tiles in RGBA16F, the focus distance in R32F.
		constexpr DXGI_FORMAT kCoCFormat = DXGI_FORMAT_R16_FLOAT;
		constexpr DXGI_FORMAT kTileFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		constexpr DXGI_FORMAT kFocusFormat = DXGI_FORMAT_R32_FLOAT;
		constexpr std::size_t kPyramidLevels = 3;
		// Commits after a build that start the focus from this frame's focus, not the history: a few, because a commit cannot
		// tell whether its epoch runs the new build yet.
		constexpr std::uint32_t kFocusResetCommits = 4;

		// dof.cs.hlsl's push block: DOF_RESOURCES, then the DoFFrame latch, then the BokehSamples latch region.
		struct DoFBlock
		{
			ShaderBindings::Handle shared[4];
			ShaderBindings::Handle rwTexOut, rwFocus, rwTexCoC, rwTexCoCTile, linearSampler;
			ShaderBindings::Handle texColor, texPreviousFocus, depthTexture, texCoCInput, texFarBlur, texNearBlur, texPostSmoothInput,
				texBokehShape, texCoCHalf, texCoCTile, texCoCTileDilated;
			ShaderBindings::Handle texGatherColor[kPyramidLevels], texGatherCoC[kPyramidLevels], texReduceCoCInput;
			ShaderBindings::Latch dofFrame, bokehSamples;
		};

		// dof.cs.hlsl's DoFConstants under ORG: DoFCB, then OrgResetFocus and its row's padding.
		struct DoFLatch
		{
			DoF::DoFCB cb;
			std::uint32_t resetFocus = 0;
			std::uint32_t pad[3]{};
		};
		static_assert(sizeof(DoFLatch) == sizeof(DoF::DoFCB) + 16);

		using SampleTable = std::array<BokehResources::ShapeSample, BokehResources::GATHER_SAMPLE_COUNT>;

		/** @brief One dispatch's resources by the shader's names; a slot left empty is a handle the entry never reads. */
		struct Slots
		{
			Bind out, focus, coc, tile;  // u0..u3
			Bind color, previousFocus, depth, cocInput, farBlur, nearBlur, postSmoothInput, bokehShape, cocHalf, cocTile, cocTileDilated;
			std::array<Bind, kPyramidLevels> gatherColor, gatherCoC;
			Bind reduceCoCInput;
		};

		std::uint32_t Groups(std::uint32_t a_size) { return (a_size + 7) >> 3; }
		std::uint32_t Half(std::uint32_t a_size) { return (std::max)(1u, a_size / 2); }
		std::uint32_t HalfUp(std::uint32_t a_size) { return (std::max)(1u, (a_size + 1) / 2); }

		/**
		 * @brief DoF::Draw on the graph: focus, CoC, the half-resolution setup, the CoC pyramid and tiles, the far and near gathers,
		 * the postfilter, the combiner and the optional post smoothing, 11 to 18 dispatches as DoF::Draw issues them. What decides the
		 * passes (adaptive gather, ring count, post smoothing, the custom aperture) is in the key; everything else is in the latch,
		 * including the gather's sample table, so a blade or shape change needs no rebuild.
		 *
		 * The focus stays on the GPU: CS_UpdateFocus updates a 1x1 history texture owned here in place, and CS_CalculateCoC reads
		 * it. The custom aperture textures stay BokehResources' D3D11 textures (shared with Lens Flare), imported by name.
		 */
		class DoFEffect final : public Effect
		{
		public:
			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& dof = static_cast<DoF&>(a_feature);
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				if (!depth)
					return "the main depth cannot be imported";
				if (!dof.texOutput)
					return "its resources are not set up";
				// As DoF::Draw: the frame for the D3D11 output's size, which the chain checks is kMAIN's.
				frame = dof.PrepareFrame(static_cast<float>(dof.texOutput->desc.Width), static_cast<float>(dof.texOutput->desc.Height));
				if (!frame.run)
					return "Target Focus has no target";

				shape.reset();
				if (frame.constants.BokehMode == 1) {
					const auto& texture = dof.owner->bokehResources.texBokehShapes[frame.customShapeIndex];
					shape = texture ? FrameAnchors::D3D11Texture(std::format("PostProcessing::BokehShape{}", frame.customShapeIndex),
										  texture->resource.get(), texture->srv.get(), nullptr) :
					                  nullptr;
					if (!shape)
						return "its custom aperture cannot be imported";
				}

				for (const std::uint64_t word : { std::uint64_t(frame.adaptiveGather), std::uint64_t(frame.gatherQuality),
						 std::uint64_t(frame.postSmoothing), std::uint64_t(frame.constants.BokehMode),
						 std::uint64_t(reinterpret_cast<std::uintptr_t>(shape.get())), std::uint64_t(reinterpret_cast<std::uintptr_t>(depth.get())) })
					a_key.push_back(word);
				return nullptr;
			}

			void Build(Builder& a_builder, PostProcessFeature&) override
			{
				if (!sampler) {
					// DoF::linearSampler's description: linear, mirrored, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{
						.addressU = rhi::AddressMode::Mirror,
						.addressV = rhi::AddressMode::Mirror,
						.addressW = rhi::AddressMode::Mirror });
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return;
					}
				}
				if (!focus) {
					D3D11_TEXTURE2D_DESC desc{};
					desc.Width = desc.Height = 1;
					desc.MipLevels = 1;
					desc.ArraySize = 1;
					desc.Format = kFocusFormat;
					desc.SampleDesc = { 1, 0 };
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
					focus = std::make_unique<GraphResources::Texture2D>(desc, "PostProcessing::DoF::Focus",
						GraphResources::Options{ .lifetime = GraphResources::Lifetime::History });
				}
				a_builder.Register(*focus);
				focusReset = kFocusResetCommits;

				const auto& size = a_builder.GetShape();
				const DXGI_FORMAT colorFormat = size.mainCopyFormat;  // as DoF::SetupResources, kMAIN_COPY's
				const std::uint32_t width = size.width, height = size.height;
				const std::uint32_t halfWidth = Half(width), halfHeight = Half(height);
				const std::uint32_t tileWidth = (std::max)(1u, (halfWidth + 7) / 8), tileHeight = (std::max)(1u, (halfHeight + 7) / 8);
				constexpr UINT uav = D3D11_BIND_UNORDERED_ACCESS;
				const auto image = [&](const char* a_name, DXGI_FORMAT a_format, std::uint32_t a_width, std::uint32_t a_height) {
					return a_builder.CreateImage(std::format("PostProcessing::DoF::{}", a_name), a_format, uav, a_width, a_height);
				};

				const Image input = a_builder.Current();
				const Image output = image("Output", colorFormat, width, height);
				const Image coc = image("FullCoC", kCoCFormat, width, height);
				const Image setupColor = image("SetupColor", colorFormat, halfWidth, halfHeight);
				const Image setupCoC = image("SetupCoC", kCoCFormat, halfWidth, halfHeight);
				const Image farLayer = image("FarLayer", colorFormat, halfWidth, halfHeight);
				const Image nearLayer = image("NearLayer", colorFormat, halfWidth, halfHeight);
				const Image farFiltered = image("FarFiltered", colorFormat, halfWidth, halfHeight);
				const Image cocTile = image("CoCTile", kTileFormat, tileWidth, tileHeight);
				const Image cocTileTemporary = image("CoCTileTemporary", kTileFormat, tileWidth, tileHeight);
				const Image cocTileDilated = image("CoCTileDilated", kTileFormat, tileWidth, tileHeight);
				std::array<Image, kPyramidLevels> gatherColor, gatherCoC;
				std::array<std::array<std::uint32_t, 2>, kPyramidLevels> gatherSize{};
				if (frame.adaptiveGather) {
					std::uint32_t levelWidth = halfWidth, levelHeight = halfHeight;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						levelWidth = HalfUp(levelWidth);
						levelHeight = HalfUp(levelHeight);
						gatherSize[i] = { levelWidth, levelHeight };
						gatherColor[i] = image(std::format("GatherColor{}", i + 1).c_str(), colorFormat, levelWidth, levelHeight);
						gatherCoC[i] = image(std::format("GatherCoC{}", i + 1).c_str(), kCoCFormat, levelWidth, levelHeight);
					}
				}

				constants = a_builder.Latch<DoFLatch>();
				samples = a_builder.Latch<SampleTable>();
				const auto program = [&](const wchar_t* a_entry, bool a_gather = false) {
					std::vector<std::pair<std::string, std::string>> defines;
					if (a_gather)
						defines.emplace_back("GATHER_RING_COUNT", frame.gatherQuality == 0 ? "4" : "5");
					return a_builder.Compute(kShader, ShaderBindings::Words<DoFBlock>(), std::move(defines), a_entry);
				};
				const auto dispatch = [&](const char* a_name, std::shared_ptr<const ComputeProgram> a_program, const Slots& a_slots,
										  std::uint32_t a_groupsX, std::uint32_t a_groupsY) {
					std::vector<Bind> bindings{ Bind::Shared(), a_slots.out, a_slots.focus, a_slots.coc, a_slots.tile, Bind::Handle(sampler.Index()),
						a_slots.color, a_slots.previousFocus, a_slots.depth, a_slots.cocInput, a_slots.farBlur, a_slots.nearBlur,
						a_slots.postSmoothInput, a_slots.bokehShape, a_slots.cocHalf, a_slots.cocTile, a_slots.cocTileDilated };
					bindings.insert(bindings.end(), a_slots.gatherColor.begin(), a_slots.gatherColor.end());
					bindings.insert(bindings.end(), a_slots.gatherCoC.begin(), a_slots.gatherCoC.end());
					bindings.push_back(a_slots.reduceCoCInput);
					bindings.push_back(Bind::Latch(constants));
					bindings.push_back(Bind::Latch(samples));
					a_builder.Dispatch({ .name = a_name, .program = std::move(a_program), .bindings = std::move(bindings), .groups = { a_groupsX, a_groupsY, 1 } });
				};
				const Bind shapeBind = shape ? Bind::Srv(shape) : Bind::Unused();
				const auto gatherInputs = [&](Slots& a_slots) {
					if (!frame.adaptiveGather)
						return;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						a_slots.gatherColor[i] = Bind::Srv(gatherColor[i]);
						a_slots.gatherCoC[i] = Bind::Srv(gatherCoC[i]);
					}
				};

				// Update Focus: the history in place.
				dispatch("focus", program(L"CS_UpdateFocus"), { .focus = Bind::Uav(focus->Graph()), .color = Bind::Srv(input), .depth = Bind::Srv(depth) }, 1, 1);
				// Calculate CoC, from the focus just written.
				dispatch("coc", program(L"CS_CalculateCoC"),
					{ .coc = Bind::Uav(coc), .color = Bind::Srv(input), .previousFocus = Bind::Srv(focus->Graph()), .depth = Bind::Srv(depth) },
					Groups(width), Groups(height));
				// Half-resolution colour and CoC.
				dispatch("downsample", program(frame.adaptiveGather ? L"CS_Downsample" : L"CS_DownsampleLegacy"),
					{ .out = Bind::Uav(setupColor), .coc = Bind::Uav(setupCoC), .color = Bind::Srv(input), .cocInput = Bind::Srv(coc) },
					Groups(halfWidth), Groups(halfHeight));
				// The CoC-aware colour pyramid for the fixed gather.
				if (frame.adaptiveGather) {
					auto reduce = program(L"CS_ReduceColorCoC");
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						dispatch(std::format("reduce-{}", i + 1).c_str(), reduce,
							{ .out = Bind::Uav(gatherColor[i]), .coc = Bind::Uav(gatherCoC[i]), .color = Bind::Srv(i == 0 ? setupColor : gatherColor[i - 1]), .reduceCoCInput = Bind::Srv(i == 0 ? setupCoC : gatherCoC[i - 1]) },
							Groups(gatherSize[i][0]), Groups(gatherSize[i][1]));
					}
				}
				// CoC tiles: flatten, then the separable dilation.
				dispatch("tile-flatten", program(L"CS_CoCTileFlatten"), { .tile = Bind::Uav(cocTile), .cocInput = Bind::Srv(coc) }, Groups(tileWidth), Groups(tileHeight));
				dispatch("tile-dilate-h", program(L"CS_CoCTileDilateH"), { .tile = Bind::Uav(cocTileTemporary), .cocTile = Bind::Srv(cocTile) },
					Groups(tileWidth), Groups(tileHeight));
				dispatch("tile-dilate-v", program(L"CS_CoCTileDilateV"), { .tile = Bind::Uav(cocTileDilated), .cocTile = Bind::Srv(cocTileTemporary) },
					Groups(tileWidth), Groups(tileHeight));
				// Far gather.
				{
					Slots slots{ .out = Bind::Uav(farLayer), .color = Bind::Srv(setupColor), .bokehShape = shapeBind, .cocHalf = Bind::Srv(setupCoC), .cocTile = Bind::Srv(cocTile) };
					gatherInputs(slots);
					dispatch("far", frame.adaptiveGather ? program(L"CS_FarGather", true) : program(L"CS_FarBlur"), slots, Groups(halfWidth), Groups(halfHeight));
				}
				// The near gather reads a colour-only pyramid of the far result.
				if (frame.adaptiveGather) {
					auto reduce = program(L"CS_ReduceColor");
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						dispatch(std::format("reduce-near-{}", i + 1).c_str(), reduce,
							{ .out = Bind::Uav(gatherColor[i]), .color = Bind::Srv(i == 0 ? farLayer : gatherColor[i - 1]) }, Groups(gatherSize[i][0]),
							Groups(gatherSize[i][1]));
					}
				}
				{
					Slots slots{ .out = Bind::Uav(nearLayer), .color = Bind::Srv(farLayer), .bokehShape = shapeBind, .cocHalf = Bind::Srv(setupCoC), .cocTileDilated = Bind::Srv(cocTileDilated) };
					gatherInputs(slots);
					dispatch("near", frame.adaptiveGather ? program(L"CS_NearGather", true) : program(L"CS_NearBlur"), slots, Groups(halfWidth), Groups(halfHeight));
				}
				// Median postfilter of both layers; the setup colour takes the filtered near layer.
				dispatch("postfilter", program(L"CS_GatherPostfilter"),
					{ .out = Bind::Uav(farFiltered), .tile = Bind::Uav(setupColor), .color = Bind::Srv(farLayer), .nearBlur = Bind::Srv(nearLayer) },
					Groups(halfWidth), Groups(halfHeight));
				// Combiner, straight into the output unless post smoothing follows.
				const Image smooth = frame.postSmoothing ? image("PostSmooth", colorFormat, width, height) : Image{};
				dispatch("combine", program(L"CS_Combiner"),
					{ .out = Bind::Uav(frame.postSmoothing ? smooth : output), .color = Bind::Srv(input), .cocInput = Bind::Srv(coc), .farBlur = Bind::Srv(farFiltered), .nearBlur = Bind::Srv(setupColor) },
					Groups(width), Groups(height));
				if (frame.postSmoothing) {
					const Image smooth2 = image("PostSmooth2", colorFormat, width, height);
					dispatch("smooth-1", program(L"CS_PostSmoothing1"), { .out = Bind::Uav(smooth2), .color = Bind::Srv(smooth), .cocInput = Bind::Srv(coc) },
						Groups(width), Groups(height));
					dispatch("smooth-2", program(L"CS_PostSmoothing2AndFocusing"),
						{ .out = Bind::Uav(output), .color = Bind::Srv(smooth), .cocInput = Bind::Srv(coc), .postSmoothInput = Bind::Srv(smooth2) },
						Groups(width), Groups(height));
				}
				a_builder.SetCurrent(output);
			}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override
			{
				// Update made this frame's values; the chain commits only frames it runs, after Update.
				DoFLatch latch{ .cb = frame.constants, .resetFocus = focusReset > 0 ? 1u : 0u };
				a_commit.Write(constants, latch);
				a_commit.Write(samples, static_cast<DoF&>(a_feature).BokehSamples(frame));
				if (focusReset > 0)
					--focusReset;
			}

		private:
			std::shared_ptr<org::Resource> depth, shape;
			std::unique_ptr<GraphResources::Texture2D> focus;  // the focus distance in km, read and updated each frame
			org::OwnedDescriptorBinding sampler;
			DoF::Frame frame;
			LatchRegion<DoFLatch> constants;
			LatchRegion<SampleTable> samples;
			std::uint32_t focusReset = 0;
		};
	}

	std::unique_ptr<Effect> MakeDoF() { return std::make_unique<DoFEffect>(); }
}

#endif
