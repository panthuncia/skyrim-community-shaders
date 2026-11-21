// The view epochs' recording: their passes (cull, a trees view's draw family, draws), their PassDefs and graph extension.

#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Passes/DrawHelpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/GraphConditions.h"
#include "Features/DrawcallLimitFix/Core/Render/Epochs/ScheduleExecutor.h"
#include "Features/DrawcallLimitFix/Core/Extensions/PassContext.h"
#include "Features/DrawcallLimitFix/Core/Gpu/FrameValues.h"

namespace DCLF
{
	namespace Draws
	{
		namespace
		{
			std::shared_ptr<const ViewFrame> CurrentViewFrame(const org::PassPrepareContext& a_preparation, ViewKind a_kind)
			{
				if (const auto* shapes = RevisionShapesOf(a_preparation))
					return shapes->views[ViewIndex(a_kind)];
				return nullptr;
			}

			/** @brief The build state's part of the view a pass is made for. Empty when the build has no such view, and then no pass is added. */
			ViewBuild ViewBuildOf(const Api::PassContext& a_context)
			{
				if (a_context.build)
					if (const auto* view = a_context.build->ViewOf(a_context.View().get()))
						return *view;
				static std::atomic<bool> logged{ false };
				if (!logged.exchange(true))
					logger::error("[DCLF] a view pass was made without its view in the build state; it is left out");
				return {};
			}

			struct BuildBindings
			{
				org::DeclaredViewToken sequences, count, visibility, poolFirsts;
				std::array<org::DeclaredViewToken, kMaxViewFaces> bucketCounts;
			};

			struct BuildPrepared
			{
				std::shared_ptr<const ComputeProgram> program;
				std::shared_ptr<const org::LatchBlock> latch;
				rhi::CommandSignatureHandle signature{};
				BuildDrawsConstants constants{};
				std::uint32_t faces = 0;
			};

			/**
			 * @brief A view's cull: BuildDrawsCS's single phase, frustum only, one dispatch per face. A face not drawn gets a
			 * dispatch of 0 groups.
			 */
			class ViewBuildDrawsPass final : public org::TypedRenderGraphPass<ViewBuildDrawsPass, BuildPrepared, BuildBindings>
			{
			public:
				ViewBuildDrawsPass(std::shared_ptr<ViewResources> a_resources, std::shared_ptr<Resources> a_main) :
					resources(std::move(a_resources)), mainResources(std::move(a_main)) {}

				BuildBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const auto& main = *mainResources;
					BuildBindings bindings{};
					bindings.sequences = a_builder.UnorderedAccess(*resources->sequences).View();
					bindings.count = a_builder.UnorderedAccess(resources->count).View();
					// Only read in the single phase, never written.
					bindings.visibility = a_builder.UnorderedAccess(*main.visibility).View();
					bindings.poolFirsts = a_builder.ShaderResource(*main.scene->pool->firsts).View();
					for (std::uint32_t f = 0; f < resources->traits->faces; ++f)
						bindings.bucketCounts[f] = a_builder.UnorderedAccess(*resources->bucketCounts[f]).View();
					FrameValues::Get().DeclareReads(a_builder, FrameValues::kReadPlacements | FrameValues::kReadPools);
					DeclarePayloadReads(a_builder);
					return bindings;
				}

				// The view's shape and the pool's version come from the revision.
				void Revision(std::vector<std::uint64_t>&) const {}

				BuildPrepared Prepare(const BuildBindings&, const org::PassPrepareContext& a_preparation) const
				{
					BuildPrepared prepared{};
					const auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || !frame->latch || !resources->buildDraws || !resources->dispatchSignature)
						return prepared;
					prepared.program = resources->buildDraws;
					prepared.latch = frame->latch;
					prepared.signature = resources->dispatchSignature->GetHandle();
					prepared.faces = resources->traits->faces;
					auto& constants = prepared.constants;
					SetRowStrides(constants);
					constants.phaseBits = 0;  // the single phase, frustum only, nothing published
					// Bucket draws are bounded by their buckets. This bounds the other path, which nothing takes.
					constants.phaseTwoBase = prepared.faces * frame->sequenceDraws;
					return prepared;
				}

				static void Record(const BuildBindings& a_bindings, const BuildPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.program || !a_prepared.latch)
						return;
					auto viewed = a_prepared.constants;
					viewed.sequencesIndex = a_recording.Index(a_bindings.sequences);
					viewed.countIndex = a_recording.Index(a_bindings.count);
					viewed.visibilityIndex = a_recording.Index(a_bindings.visibility);
					viewed.poolFirstsIndex = a_recording.Index(a_bindings.poolFirsts);
					auto& commands = a_recording.Commands();
					commands.BindLayout(a_prepared.program->layout->GetHandle());
					commands.BindPipeline(a_prepared.program->Pipeline(a_recording));
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						auto constants = viewed;
						constants.bucketCountsIndex = a_recording.Index(a_bindings.bucketCounts[f]);
						RecordLatchedDispatch(constants, *a_prepared.latch, a_prepared.signature, f * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), a_recording);
					}
				}

			private:
				std::shared_ptr<ViewResources> resources;
				std::shared_ptr<Resources> mainResources;
			};

			/** @brief What a view's draw pass draws into. */
			enum class DrawMode : std::uint8_t
			{
				Faces,
				Depth,
				Colour,
			};

			struct DrawBindings
			{
				std::array<org::DeclaredViewToken, kMaxViewFaces> colours;  // a face's cube slice each, or the main targets
				org::DeclaredViewToken depth;
				org::ResourceBindingToken sequences, pool;
				std::array<org::ResourceBindingToken, kMaxViewFaces> bucketCounts;
				std::array<std::array<org::ResourceBindingToken, kMaxViewFaces>, Extensions::kDrawFamilies> faceLists;  // by draw family
			};

			struct DrawPrepared
			{
				std::shared_ptr<const ViewFrame> frame;
				DrawMode mode = DrawMode::Faces;
				std::uint32_t faces = 0;
				std::array<std::array<RenderGraph::BufferRef, kMaxViewFaces>, Extensions::kDrawFamilies> faceRows{};  // by draw family
			};

			/**
			 * @brief A view's draws, a pass per face and a plain indirect draw per bucket. Faces load the cube slice because the engine's
			 * sky is there. Colour uses kPOST_ZPREPASS_COPY, FUN_1414cb6d0's depth target 7, and the first-person depth range.
			 */
			class DrawPass final : public org::TypedRenderGraphPass<DrawPass, DrawPrepared, DrawBindings>
			{
			public:
				DrawPass(ViewBuild a_view, DrawMode a_mode) :
					resources(a_view.resources), view(std::move(a_view)), mode(a_mode) {}

				DrawBindings Declare(org::PassBuilder& a_builder)
				{
					a_builder.PreferQueue(org::QueueKind::Graphics);
					const std::span<const org::SrvView> noViews{};
					const auto& main = *view.main;
					const auto& scene = *main.scene;
					const std::uint32_t faces = resources->traits->faces;
					DrawBindings bindings{};
					if (mode == DrawMode::Faces) {
						for (std::uint32_t f = 0; f < faces; ++f)
							bindings.colours[f] = a_builder.RenderTarget(view.target, org::RtvView{ UINT32_MAX, 0, f }).View();
						bindings.depth = a_builder.DepthReadWrite(view.depth).View();
					} else if (mode == DrawMode::Colour) {
						for (std::uint32_t i = 0; i < 2; ++i)
							bindings.colours[i] = a_builder.RenderTarget(main.native[i]).View();
						bindings.depth = a_builder.DepthReadWrite(view.target).View();
					} else {
						bindings.depth = a_builder.DepthReadWrite(main.nativeDepth).View();
					}
					// The sequences are both the draw arguments and what the vertex stage reads by device address.
					bindings.sequences = a_builder.IndirectArguments(*resources->sequences).Resource();
					a_builder.ShaderResource(*resources->sequences, noViews);
					bindings.pool = a_builder.IndexBuffer(*scene.pool->indices);
					for (std::uint32_t f = 0; f < faces; ++f)
						bindings.bucketCounts[f] = a_builder.IndirectArguments(*resources->bucketCounts[f]).Resource();
					// Read through device addresses, but declared so the graph orders them after their uploads.
					a_builder.ShaderResource(resources->frameConstants ? resources->frameConstants : main.frameConstants, noViews);
					// A view with its own frame constants also has its own copies of the main frame buffers.
					if (const auto frameBuffers = resources->frameBuffers.load(std::memory_order_acquire))
						for (const auto& frameBuffer : *frameBuffers)
							a_builder.ShaderResource(frameBuffer.copy, noViews);
					if (scene.vertexStream)
						a_builder.ShaderResource(*scene.vertexStream, noViews);
					a_builder.ShaderResource(resources->blocks, noViews);
					// Every mode draws through a frame record, whose shaders read t117 and t121-t123 and the payload's rows.
					FrameValues::Get().DeclareReads(a_builder);
					DeclarePayloadReads(a_builder);
					if (mode == DrawMode::Colour)
						DeclareFrameBuffers(a_builder, main);
					// The faces read the published textures their frame textures' views export. Only a frame texture with a view can be read there.
					else if (mode == DrawMode::Faces) {
						Extensions::DeclareGraphTextures(a_builder, Extensions::FacesGraphTextureIds());
						// And the character light's noise, an HDR downsample level.
						Extensions::DeclareGraphTextures(a_builder, Extensions::CharacterLightGraphTextureIds());
					}
					for (std::size_t d = 0; d < Extensions::kDrawFamilies && mode == DrawMode::Faces; ++d)
						if (const auto& family = Extensions::DrawFamilies()[d].contribution.faces; Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d))) {
							Api::DrawFamilyDeclare declare{ a_builder, scene, resources.get(), std::span(bindings.faceLists[d]) };
							family.declare(declare);
						}
					return bindings;
				}

				void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(static_cast<std::uint64_t>(mode)); }

				DrawPrepared Prepare(const DrawBindings&, const org::PassPrepareContext& a_preparation) const
				{
					DrawPrepared prepared{};
					auto frame = CurrentViewFrame(a_preparation, resources->traits->kind);
					if (!frame || !frame->indirect.valid || !frame->width || !frame->height || (mode != DrawMode::Faces && frame->buckets.empty()))
						return prepared;
					prepared.faces = resources->traits->faces;
					for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
						if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)))
							for (std::uint32_t f = 0; f < prepared.faces; ++f)
								prepared.faceRows[d][f] = resources->faceRowsRef[d][f];
					prepared.frame = std::move(frame);
					prepared.mode = mode;
					return prepared;
				}

				static void Record(const DrawBindings& a_bindings, const DrawPrepared& a_prepared, org::PassRecordContext& a_recording)
				{
					if (!a_prepared.frame)
						return;
					const auto& frame = *a_prepared.frame;
					const auto mode = a_prepared.mode;
					auto& commands = a_recording.Commands();
					commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
					const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
					const RenderGraph::BufferRef sequencesRef = DCLF::Draws::RecordingBufferRef(a_recording, a_bindings.sequences);
					const rhi::IndexBufferView pool{ a_recording.Resolve(a_bindings.pool).GetHandle(), 0, 0, rhi::Format::R16_UInt };
					const auto& families = Extensions::DrawFamilies();
					static const std::shared_ptr<const void> kNothing;  // the face's pipeline is the shape's
					for (std::uint32_t f = 0; f < a_prepared.faces; ++f) {
						std::array<rhi::ColorAttachment, 2> colours{};
						rhi::PassBeginInfo begin{};
						if (mode == DrawMode::Faces) {
							colours[0].rtv = a_recording.View(a_bindings.colours[f]);
							colours[0].loadOp = rhi::LoadOp::Load;
							colours[0].storeOp = rhi::StoreOp::Store;
							begin.colors = { colours.data(), 1 };
						} else if (mode == DrawMode::Colour) {
							for (std::uint32_t i = 0; i < 2; ++i) {
								colours[i].rtv = a_recording.View(a_bindings.colours[i]);
								colours[i].loadOp = rhi::LoadOp::Load;
								colours[i].storeOp = rhi::StoreOp::Store;
								colours[i].resource = a_recording.Resolve(a_bindings.colours[i].Resource()).GetHandle();
							}
							begin.colors = { colours.data(), 2 };
						}
						rhi::DepthAttachment depth{};
						depth.dsv = a_recording.View(a_bindings.depth);
						depth.depthLoad = mode == DrawMode::Faces ? rhi::LoadOp::Clear : rhi::LoadOp::Load;
						depth.depthStore = rhi::StoreOp::Store;
						if (mode == DrawMode::Faces) {
							depth.clear.type = rhi::ClearValueType::DepthStencil;
							depth.clear.format = rhi::Format::D24_UNorm_S8_UInt;
							depth.clear.depthStencil = { 1.0f, 0 };
						} else {
							depth.stencilLoad = rhi::LoadOp::Load;
							depth.stencilStore = rhi::StoreOp::Store;
						}
						begin.depth = &depth;
						begin.width = frame.width;
						begin.height = frame.height;
						begin.minDepth = 0.0f;
						begin.maxDepth = frame.depthRange;
						begin.debugName = mode == DrawMode::Faces ? "DCLF reflection face" : mode == DrawMode::Colour ? "DCLF first person" : "DCLF first-person depth";
						commands.BeginPass(begin);
						commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
						commands.BindLayout(frame.indirect.zLayout);
						commands.PushConstants(rhi::ShaderStage::AllGraphics, 0, kFramePushBinding, 0, kFramePushWords, frame.push[f].data());
						commands.SetIndexBuffer(pool);
						const auto counts = a_recording.Resolve(a_bindings.bucketCounts[f]).GetHandle();
						for (std::uint32_t b = 0; b < frame.buckets.size(); ++b) {
							const auto& bucket = frame.buckets[b];
							if (!bucket.capacity)
								continue;
							RecordBucketDraw(commands, frame.indirect.zDrawSignature, sequences, sequencesRef, std::uint64_t(f) * frame.sequenceDraws + bucket.first,
								mode == DrawMode::Depth ? bucket.depth : bucket.colour, counts, b, bucket.capacity);
						}
						// The faces' tree LOD, one instanced draw of the face's list with the shape's pipeline.
						for (std::size_t d = 0; d < Extensions::kDrawFamilies && mode == DrawMode::Faces; ++d) {
							const auto& faces = families[d].contribution.faces;
							const auto& drawn = frame.faces[d];
							if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)) && drawn.pipeline.valid() && drawn.groups) {
								faces.record({ commands, Api::DrawFamilyPass::Face, kNothing, a_prepared.faceRows[d][f], a_recording.Resolve(a_bindings.faceLists[d][f]).GetHandle(),
									drawn.pipeline, drawn.signature });
							}
						}
						commands.EndPass();
					}
				}

			private:
				std::shared_ptr<ViewResources> resources;
				ViewBuild view;
				DrawMode mode;
			};


			class ViewExtension final : public org::RenderGraph::IRenderGraphExtension
			{
			public:
				ViewExtension(std::shared_ptr<const BuildState> a_build, ViewBuild a_view) :
					build(std::move(a_build)), view(std::move(a_view)), resources(view.resources) {}

				void PrepareForBuild(org::RenderGraph& a_graph) override
				{
					// The main pass's resources, under the main extension's identifiers.
					const auto& main = *view.main;
					Register(a_graph, org::ResourceIdentifier("cs.dclf.visibility"), main.visibility);
					RegisterSceneBuffers(a_graph, *main.scene);
					const auto& traits = *resources->traits;
					const auto id = [&](std::string_view a_name) { return org::ResourceIdentifier(fmt::format("{}.{}", traits.prefix, a_name)); };
					Register(a_graph, id("draw-count"), resources->count);
					Register(a_graph, id(traits.blocks), resources->blocks);
					if (resources->frameConstants)
						Register(a_graph, id("frame-constants"), resources->frameConstants);
					if (const auto frameBuffers = resources->frameBuffers.load(std::memory_order_acquire))
						for (const auto& frameBuffer : *frameBuffers)
							Register(a_graph, id(fmt::format("frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
					if (view.depth)
						Register(a_graph, id("depth"), view.depth);
					Register(a_graph, id(traits.target), view.target);
					for (std::size_t d = 0; d < Extensions::kDrawFamilies && traits.trees; ++d)
						if (Extensions::DrawFamilyHasFaces(static_cast<Api::DrawFamily>(d)))
							for (std::uint32_t f = 0; f < traits.faces; ++f)
								Register(a_graph, id(fmt::format("{}{}", Extensions::DrawFamilies()[d].rowName, f)), resources->faceRows[d][f]);
				}

				void GatherStructuralPasses(org::RenderGraph& a_graph, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
				{
					if (!view.main || !ViewPassesReady(view, *view.main))
						return;
					// A hosted epoch gets no passes here, since its host's extension adds them.
					Api::PassContext context{};
					context.main = &view.main;
					context.view = &resources;
					context.build = build.get();
					GatherScheduled(Api::EpochShape::View, resources->traits->kind, context, a_graph.PassEpochFilter(), a_out);
				}

			private:
				std::shared_ptr<const BuildState> build;  // held for the build
				ViewBuild view;
				std::shared_ptr<ViewResources> resources;
			};

			std::shared_ptr<org::RenderPass> ViewLatchedCopies(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				return MakeLatchedCopiesPass(a_context.Conditions(), [resources, scene = a_context.Main()->scene] { return ViewLatchedTargets(*resources, *scene); }, [kind = resources->traits->kind](const org::PassPrepareContext& a_preparation) {
					const auto frame = CurrentViewFrame(a_preparation, kind);
					return frame ? LatchedSource{ frame, frame->latched } : LatchedSource{};
				});
			}

			std::shared_ptr<org::RenderPass> ViewBuildDraws(const Api::PassContext& a_context)
			{
				auto view = ViewBuildOf(a_context);
				return view.main ? std::make_shared<ViewBuildDrawsPass>(a_context.View(), std::move(view.main)) : nullptr;
			}

			// A view without a depth pass, the reflection, draws its faces in one epoch. First person draws its depth in the depth
			// epoch after the HZB and both depth phases, then its colour in the colour epoch.
			std::shared_ptr<org::RenderPass> FacesDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				auto view = ViewBuildOf(a_context);
				return !resources->traits->depthPass && view.main ? std::make_shared<DrawPass>(std::move(view), DrawMode::Faces) : nullptr;
			}

			std::shared_ptr<org::RenderPass> DepthDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				auto view = ViewBuildOf(a_context);
				return resources->traits->depthPass && view.main ? std::make_shared<DrawPass>(std::move(view), DrawMode::Depth) : nullptr;
			}

			std::shared_ptr<org::RenderPass> ColourDraw(const Api::PassContext& a_context)
			{
				const auto& resources = a_context.View();
				auto view = ViewBuildOf(a_context);
				return resources->traits->depthPass && view.main ? std::make_shared<DrawPass>(std::move(view), DrawMode::Colour) : nullptr;
			}

			// Named with the view's prefix.
			constexpr Api::PassDef kViewPasses[] = {
				{ .key = "view.latched-copies", .passName = "{view}.latched-copies", .kind = PassKind::Copy, .make = &ViewLatchedCopies },
				{ .key = "view.build-draws", .passName = "{view}.build-draws", .make = &ViewBuildDraws },
				{ .key = "view.faces", .passName = "{view}.faces", .kind = PassKind::Render, .make = &FacesDraw },
				{ .key = "view.depth", .passName = "{view}.depth", .kind = PassKind::Render, .make = &DepthDraw },
				{ .key = "view.colour", .passName = "{view}.colour", .kind = PassKind::Render, .make = &ColourDraw },
			};
		}

		std::span<const Api::PassDef> ViewPassDefs()
		{
			return kViewPasses;
		}

		bool ViewPassesReady(const ViewBuild& a_view, const Resources& a_main)
		{
			return a_view.resources && a_view.main.get() == &a_main && a_main.scene && a_main.scene->pool && a_view.target &&
			       (!a_view.resources->traits->depthPass || (a_main.nativeDepth && a_main.native[0] && a_main.native[1]));
		}

		std::vector<LatchedTarget> ViewLatchedTargets(const ViewResources& a_view, const SceneBuffers& a_scene)
		{
			std::vector<LatchedTarget> out{ { a_view.count, nullptr }, { a_view.blocks, nullptr } };
			if (a_view.frameConstants)
				out.push_back({ a_view.frameConstants, nullptr });
			if (const auto frameBuffers = a_view.frameBuffers.load(std::memory_order_acquire))
				for (const auto& frameBuffer : *frameBuffers)
					out.push_back({ frameBuffer.copy, nullptr });
			std::array<bool, Extensions::kDrawFamilies> families{};
			for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
				families[d] = FamilySetUp(a_scene, static_cast<Api::DrawFamily>(d)) && a_view.faceLists[d][0];
			for (std::uint32_t f = 0; f < a_view.traits->faces; ++f) {
				out.push_back({ nullptr, a_view.bucketCounts[f] });
				for (std::size_t d = 0; d < Extensions::kDrawFamilies; ++d)
					if (families[d]) {
						out.push_back({ a_view.faceRows[d][f], nullptr });
						out.push_back({ nullptr, a_view.faceLists[d][f] });
					}
			}
			return out;
		}

		std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<const BuildState> a_build, ViewKind a_kind)
		{
			if (!a_build)
				return nullptr;
			auto view = a_build->View(a_kind);
			if (!view.resources)
				return nullptr;
			return std::make_unique<ViewExtension>(std::move(a_build), std::move(view));
		}
	}
}
