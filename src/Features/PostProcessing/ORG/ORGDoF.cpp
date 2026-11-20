#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "Features/PostProcessing/ORGPostEffect.h"

#	include "Features/PostProcessing.h"
#	include "Features/PostProcessing/DoF.h"
#	include "Globals.h"
#	include "RenderGraph/GraphResources.h"
#	include "RenderGraph/ShaderBindings.h"
#	include "State.h"

#	include <Resources/ExternalTextureResource.h>

#	include <algorithm>
#	include <format>

namespace ORGPostChain
{
	namespace
	{
		constexpr const char* kShader = "PostProcessing/DoF/dof.cs.hlsl";
		// As DoF::SetupResources: CoC in R16F, tiles in RGBA16F, focus distance in R32F.
		constexpr DXGI_FORMAT kCoCFormat = DXGI_FORMAT_R16_FLOAT;
		constexpr DXGI_FORMAT kTileFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
		constexpr DXGI_FORMAT kFocusFormat = DXGI_FORMAT_R32_FLOAT;
		constexpr std::size_t kPyramidLevels = 3;

		// DOF_RESOURCES, then the DoFFrame latch, then the BokehSamples latch region.
		struct DoFBlock
		{
			ShaderBindings::Handle shared[4];
			ShaderBindings::Handle rwTexOut, rwFocus, rwTexCoC, rwTexCoCTile, linearSampler;
			ShaderBindings::Handle texColor, texPreviousFocus, depthTexture, texCoCInput, texFarBlur, texNearBlur, texPostSmoothInput,
				texBokehShape, texCoCHalf, texCoCTile, texCoCTileDilated;
			ShaderBindings::Handle texGatherColor[kPyramidLevels], texGatherCoC[kPyramidLevels], texReduceCoCInput;
			ShaderBindings::Latch dofFrame, bokehSamples;
		};

		// DoFCB, then OrgResetFocus and its row's padding.
		struct DoFLatch
		{
			DoF::DoFCB cb;
			std::uint32_t resetFocus = 0;
			std::uint32_t pad[3]{};
		};
		static_assert(sizeof(DoFLatch) == sizeof(DoF::DoFCB) + 16);

		using SampleTable = std::array<BokehResources::ShapeSample, BokehResources::GATHER_SAMPLE_COUNT>;

		/** @brief An empty slot is a handle the entry never reads. */
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

		/** @brief What one build's passes are: the gather path, its ring count, post smoothing and whether it binds a custom shape. */
		struct DoFShape
		{
			bool adaptiveGather = false;
			bool postSmoothing = false;
			bool customShape = false;
			std::size_t gatherQuality = 0;
		};

		class DoFEffect;

		/** @brief One build's latch regions and pass list. Its commit keeps the frame's constants to what the build binds. */
		class DoFRun final : public Run
		{
		public:
			DoFRun(DoFEffect& a_effect, DoFShape a_shape, LatchRegion<DoFLatch> a_constants, LatchRegion<SampleTable> a_samples) :
				effect(a_effect), shape(a_shape), constants(a_constants), samples(a_samples) {}

			void Commit(const CommitContext& a_commit, PostProcessFeature& a_feature) override;

		private:
			DoFEffect& effect;
			DoFShape shape;
			LatchRegion<DoFLatch> constants;
			LatchRegion<SampleTable> samples;
		};

		/**
		* @brief DoF::Draw on the graph, 11 to 18 dispatches. Only what changes the pass list is in the key: the gather settings and the
		* bokeh mode from the revision (CS::Setting variables), and what is no variable yet (a Cinematic Camera lens, post smoothing,
		* the custom aperture's import) observed by Update. The rest, including the sample table, is latched, so a blade or shape
		* change needs no build. The focus history stays on the GPU in a 1x1 texture owned here. The custom aperture textures stay
		* BokehResources' D3D11 textures, imported by name.
		* The ring count is a define (GATHER_RING_COUNT). Its programs are listed in Programs and built on the programs lane.
		*/
		class DoFEffect final : public Effect
		{
		public:
			void Declare(PostProcessFeature& a_feature, RenderGraph::Frame::Dependencies& a_dependencies) override
			{
				const auto& settings = static_cast<DoF&>(a_feature).settings;
				for (const auto& dependency : RenderGraph::Frame::DependsOn(settings.UseAdaptiveGather, settings.GatherQuality, settings.BokehMode, settings.HighlightShape))
					a_dependencies.push_back(dependency);
			}

			void Key(const RenderGraph::Frame::Inputs& a_values, PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				const auto& settings = static_cast<DoF&>(a_feature).settings;
				a_key.push_back(a_values.Get(settings.UseAdaptiveGather) ? 1u : 0u);
				a_key.push_back(static_cast<std::uint64_t>(std::clamp(a_values.Get(settings.GatherQuality), 0, 1)));
				a_key.push_back(static_cast<std::uint64_t>(std::clamp(a_values.Get(settings.BokehMode), 0, 1)));
				a_key.push_back(static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_values.Get(settings.HighlightShape))));
			}

			const char* Update(PostProcessFeature& a_feature, std::vector<std::uint64_t>& a_key) override
			{
				auto& dof = static_cast<DoF&>(a_feature);
				depth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN));
				if (!depth)
					return "the main depth cannot be imported";
				// As DoF::Draw, sized for kMAIN_COPY, since the graph path does not make the D3D11 output.
				auto* mainCopy = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY].texture;
				if (!mainCopy)
					return "kMAIN_COPY is missing";
				D3D11_TEXTURE2D_DESC mainCopyDesc{};
				mainCopy->GetDesc(&mainCopyDesc);
				frame = dof.PrepareFrame(static_cast<float>(mainCopyDesc.Width), static_cast<float>(mainCopyDesc.Height));
				if (!frame.run)
					return "Target Focus has no target";

				shape = {};
				if (frame.constants.BokehMode == 1) {
					const auto& texture = dof.owner->bokehResources.texBokehShapes[frame.customShapeIndex];
					shape = texture ? FrameAnchors::ImmutableTexture(std::format("PostProcessing::BokehShape{}", frame.customShapeIndex),
										  texture->resource.get(), texture->srv.get()) :
					                  nullptr;
					if (!shape)
						return "its custom aperture cannot be imported";
				}

				// The settings come from the revision (Key). These are no variables yet.
				for (const std::uint64_t word : { std::uint64_t(frame.cameraLens), std::uint64_t(frame.postSmoothing),
						 std::uint64_t(reinterpret_cast<std::uintptr_t>(shape.Key())), std::uint64_t(reinterpret_cast<std::uintptr_t>(depth.Key())) })
					a_key.push_back(word);
				return nullptr;
			}

			void Programs(ProgramSource& a_programs, PostProcessFeature&, std::span<const std::uint64_t> a_key) override
			{
				// The build fails on an incomplete key.
				if (a_key.size() < 4)
					return;
				LoadPrograms(a_programs, Built(a_key));
				a_programs.Publishes(Published::kDoFFocus);
			}

			std::shared_ptr<Run> BuildRun(Builder& a_builder, PostProcessFeature&, std::span<const std::uint64_t> a_key) override
			{
				if (a_key.size() < 4) {
					a_builder.Fail("its key is incomplete");
					return nullptr;
				}
				const DoFShape built = Built(a_key);
				if (!sampler) {
					// DoF::linearSampler: linear, mirrored, every mip.
					sampler = ShaderBindings::CreateSampler(rhi::SamplerDesc{
						.addressU = rhi::AddressMode::Mirror,
						.addressV = rhi::AddressMode::Mirror,
						.addressW = rhi::AddressMode::Mirror });
					if (!sampler) {
						a_builder.Fail("its sampler could not be made");
						return nullptr;
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
				a_builder.Publish(Published::kDoFFocus, Image{ focus->Graph(), kFocusFormat });

				const auto& size = a_builder.GetShape();
				const DXGI_FORMAT colorFormat = size.mainCopyFormat;  // kMAIN_COPY's, as DoF::SetupResources
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
				if (built.adaptiveGather) {
					std::uint32_t levelWidth = halfWidth, levelHeight = halfHeight;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						levelWidth = HalfUp(levelWidth);
						levelHeight = HalfUp(levelHeight);
						gatherSize[i] = { levelWidth, levelHeight };
						gatherColor[i] = image(std::format("GatherColor{}", i + 1).c_str(), colorFormat, levelWidth, levelHeight);
						gatherCoC[i] = image(std::format("GatherCoC{}", i + 1).c_str(), kCoCFormat, levelWidth, levelHeight);
					}
				}

				const auto constants = a_builder.Latch<DoFLatch>();
				const auto samples = a_builder.Latch<SampleTable>();
				const auto programs = LoadPrograms(a_builder, built);
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
				const Bind shapeBind = built.customShape ? Bind::Srv(shape) : Bind::Unused();
				const auto gatherInputs = [&](Slots& a_slots) {
					if (!built.adaptiveGather)
						return;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						a_slots.gatherColor[i] = Bind::Srv(gatherColor[i]);
						a_slots.gatherCoC[i] = Bind::Srv(gatherCoC[i]);
					}
				};

				dispatch("focus", programs.focus, { .focus = Bind::Uav(focus->Graph()), .color = Bind::Srv(input), .depth = Bind::Srv(depth) }, 1, 1);
				dispatch("coc", programs.coc,
					{ .coc = Bind::Uav(coc), .color = Bind::Srv(input), .previousFocus = Bind::Srv(focus->Graph()), .depth = Bind::Srv(depth) },
					Groups(width), Groups(height));
				dispatch("downsample", programs.downsample,
					{ .out = Bind::Uav(setupColor), .coc = Bind::Uav(setupCoC), .color = Bind::Srv(input), .cocInput = Bind::Srv(coc) },
					Groups(halfWidth), Groups(halfHeight));
				// The CoC-aware colour pyramid for the fixed gather.
				if (built.adaptiveGather) {
					const auto& reduce = programs.reduceColorCoC;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						dispatch(std::format("reduce-{}", i + 1).c_str(), reduce,
							{ .out = Bind::Uav(gatherColor[i]), .coc = Bind::Uav(gatherCoC[i]), .color = Bind::Srv(i == 0 ? setupColor : gatherColor[i - 1]), .reduceCoCInput = Bind::Srv(i == 0 ? setupCoC : gatherCoC[i - 1]) },
							Groups(gatherSize[i][0]), Groups(gatherSize[i][1]));
					}
				}
				// Flatten, then the separable dilation.
				dispatch("tile-flatten", programs.tileFlatten, { .tile = Bind::Uav(cocTile), .cocInput = Bind::Srv(coc) }, Groups(tileWidth), Groups(tileHeight));
				dispatch("tile-dilate-h", programs.tileDilateH, { .tile = Bind::Uav(cocTileTemporary), .cocTile = Bind::Srv(cocTile) },
					Groups(tileWidth), Groups(tileHeight));
				dispatch("tile-dilate-v", programs.tileDilateV, { .tile = Bind::Uav(cocTileDilated), .cocTile = Bind::Srv(cocTileTemporary) },
					Groups(tileWidth), Groups(tileHeight));
				{
					Slots slots{ .out = Bind::Uav(farLayer), .color = Bind::Srv(setupColor), .bokehShape = shapeBind, .cocHalf = Bind::Srv(setupCoC), .cocTile = Bind::Srv(cocTile) };
					gatherInputs(slots);
					dispatch("far", programs.farPass, slots, Groups(halfWidth), Groups(halfHeight));
				}
				// The near gather reads a colour-only pyramid of the far result.
				if (built.adaptiveGather) {
					const auto& reduce = programs.reduceColor;
					for (std::size_t i = 0; i < kPyramidLevels; ++i) {
						dispatch(std::format("reduce-near-{}", i + 1).c_str(), reduce,
							{ .out = Bind::Uav(gatherColor[i]), .color = Bind::Srv(i == 0 ? farLayer : gatherColor[i - 1]) }, Groups(gatherSize[i][0]),
							Groups(gatherSize[i][1]));
					}
				}
				{
					Slots slots{ .out = Bind::Uav(nearLayer), .color = Bind::Srv(farLayer), .bokehShape = shapeBind, .cocHalf = Bind::Srv(setupCoC), .cocTileDilated = Bind::Srv(cocTileDilated) };
					gatherInputs(slots);
					dispatch("near", programs.nearPass, slots, Groups(halfWidth), Groups(halfHeight));
				}
				// The setup colour takes the filtered near layer.
				dispatch("postfilter", programs.postfilter,
					{ .out = Bind::Uav(farFiltered), .tile = Bind::Uav(setupColor), .color = Bind::Srv(farLayer), .nearBlur = Bind::Srv(nearLayer) },
					Groups(halfWidth), Groups(halfHeight));
				// Straight into the output unless post smoothing follows.
				const Image smooth = built.postSmoothing ? image("PostSmooth", colorFormat, width, height) : Image{};
				dispatch("combine", programs.combine,
					{ .out = Bind::Uav(built.postSmoothing ? smooth : output), .color = Bind::Srv(input), .cocInput = Bind::Srv(coc), .farBlur = Bind::Srv(farFiltered), .nearBlur = Bind::Srv(setupColor) },
					Groups(width), Groups(height));
				if (built.postSmoothing) {
					const Image smooth2 = image("PostSmooth2", colorFormat, width, height);
					dispatch("smooth-1", programs.smooth1, { .out = Bind::Uav(smooth2), .color = Bind::Srv(smooth), .cocInput = Bind::Srv(coc) },
						Groups(width), Groups(height));
					dispatch("smooth-2", programs.smooth2,
						{ .out = Bind::Uav(output), .color = Bind::Srv(smooth), .cocInput = Bind::Srv(coc), .postSmoothInput = Bind::Srv(smooth2) },
						Groups(width), Groups(height));
				}
				a_builder.SetCurrent(output);
				return std::make_shared<DoFRun>(*this, built, constants, samples);
			}

		private:
			friend class DoFRun;

			/** @brief The programs one build's passes use. Those its shape does not use stay null. */
			struct DoFPrograms
			{
				std::shared_ptr<const ComputeProgram> focus, coc, downsample, reduceColorCoC, tileFlatten, tileDilateH, tileDilateV, farPass, reduceColor, nearPass,
					postfilter, combine, smooth1, smooth2;
			};

			/** @brief The build's passes from its key and this frame's Update. a_key has at least 4 words. */
			DoFShape Built(std::span<const std::uint64_t> a_key) const
			{
				// As DoF::PrepareFrame: a Cinematic Camera lens always gathers and draws no custom shape.
				DoFShape built;
				built.adaptiveGather = frame.cameraLens || a_key[0] != 0;
				built.gatherQuality = static_cast<std::size_t>(a_key[1]);
				built.postSmoothing = frame.postSmoothing;
				built.customShape = !frame.cameraLens && a_key[2] == 1 && static_cast<bool>(shape);
				return built;
			}

			/** @brief Listed in Programs and built on the programs lane, for the passes a_built has. */
			static DoFPrograms LoadPrograms(ProgramSource& a_programs, const DoFShape& a_built)
			{
				const auto program = [&](const wchar_t* a_entry, bool a_gather = false) {
					std::vector<std::pair<std::string, std::string>> defines;
					if (a_gather)
						defines.emplace_back("GATHER_RING_COUNT", a_built.gatherQuality == 0 ? "4" : "5");
					return a_programs.Compute(kShader, ShaderBindings::Words<DoFBlock>(), std::move(defines), a_entry);
				};
				DoFPrograms programs;
				const bool gather = a_built.adaptiveGather;
				programs.focus = program(L"CS_UpdateFocus");
				programs.coc = program(L"CS_CalculateCoC");
				programs.downsample = program(gather ? L"CS_Downsample" : L"CS_DownsampleLegacy");
				if (gather)
					programs.reduceColorCoC = program(L"CS_ReduceColorCoC");
				programs.tileFlatten = program(L"CS_CoCTileFlatten");
				programs.tileDilateH = program(L"CS_CoCTileDilateH");
				programs.tileDilateV = program(L"CS_CoCTileDilateV");
				programs.farPass = gather ? program(L"CS_FarGather", true) : program(L"CS_FarBlur");
				if (gather)
					programs.reduceColor = program(L"CS_ReduceColor");
				programs.nearPass = gather ? program(L"CS_NearGather", true) : program(L"CS_NearBlur");
				programs.postfilter = program(L"CS_GatherPostfilter");
				programs.combine = program(L"CS_Combiner");
				if (a_built.postSmoothing) {
					programs.smooth1 = program(L"CS_PostSmoothing1");
					programs.smooth2 = program(L"CS_PostSmoothing2AndFocusing");
				}
				return programs;
			}

			FrameAnchors::Texture depth, shape;
			// Focus distance in km, updated in place each frame. It is no declared texture: Physical Lens reads it as a plain
			// resource (its focus readback), and it never changes size.
			std::unique_ptr<GraphResources::Texture2D> focus;
			org::OwnedDescriptorBinding sampler;
			DoF::Frame frame;                       // this frame's, from Update
			std::uint32_t lastCommit = UINT32_MAX;  // the frame count of the last commit, for the focus snap
		};

		void DoFRun::Commit(const CommitContext& a_commit, PostProcessFeature& a_feature)
		{
			// The chain commits only frames it runs, after Update. Until the revision with a new build is adopted, the frame's
			// constants may name a shape this build does not bind, so they keep to the build's.
			DoF::Frame frame = effect.frame;
			if (!shape.customShape) {
				frame.constants.BokehMode = 0;
				frame.constants.HighlightShape = 0;
			}
			// As DoF::Draw: the focus snaps when DoF did not run the previous frame or the history resets.
			const std::uint32_t frameCount = globals::state->frameCount;
			const bool resetFocus = effect.lastCommit == UINT32_MAX || frameCount != effect.lastCommit + 1 || globals::state->ShouldResetHistory();
			effect.lastCommit = frameCount;
			a_commit.Write(constants, DoFLatch{ .cb = frame.constants, .resetFocus = resetFocus ? 1u : 0u });
			a_commit.Write(samples, static_cast<DoF&>(a_feature).BokehSamples(frame));
		}
	}

	std::unique_ptr<Effect> MakeDoF() { return std::make_unique<DoFEffect>(); }
}

#endif
