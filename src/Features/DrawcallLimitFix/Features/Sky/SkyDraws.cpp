// The sky family's render graph side. One client per view, made once at setup, draws a batch's latched draws in engine order.
// With CS_ORG_OCCLUSION_QUERIES=1 a third client draws the sun's occlusion draw with an ORG query.
// The clients are declared at setup and registered by the first frame revision (frame revision plan, 6.19).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Constants/EngineConstants.h"
#include "RenderGraph/Engine/Queries/OcclusionQueries.h"
#include "RenderGraph/Frame/Declare.h"
#include "RenderGraph/Frame/FrameDriver.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/FrameStaging.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "Utils/Logging.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string_view>

namespace DCLF::Features::Sky::SkyDraws
{
	namespace
	{
		constexpr const char* kShader = "Sky.hlsl";
		// The most draws in one run. Cloudy weather draws about 40.
		constexpr std::uint32_t kMaxDraws = 128;
		constexpr std::uint32_t kFaces = 6;
		// One face instance per face the engine may draw in a frame. An anchor's own epoch uses instance 0 alone.
		constexpr std::uint32_t kFaceInstances = kFaces;
		// Two sun queries a frame at most, the world's and first person's.
		constexpr std::uint32_t kSunInstances = 2;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Sky.hlsl's push block (SKY_ORG_RESOURCES), in list order. */
		struct SkyBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle depth, sharedDepth, cloudShadows;
			std::array<ShaderBindings::Handle, kHeaderRegisters.size()> headers;  // EnvIBLTexture, SkyIBLTexture, EnvReflectionsTexture, EnvTexture
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(SkyBlock) == 52, "Sky.hlsl's push block is 52 bytes");

		enum class Part : std::uint8_t
		{
			Main,        // every draw, on the GBuffer targets with the depth target
			FaceSky,     // a face's draws that write no occlusion, with the face's depth target
			FaceClouds,  // a face's cloud draws that write Cloud Shadows' layers, without a depth target (as Cloud Shadows binds them)
			SunMain,     // the sun's occlusion draw on kMAIN with kMAIN's depth, reading the Z-prepass copy at t17
			SunCopy,     // the same with kPOST_ZPREPASS_COPY as the depth target, and a null view at t17 (D3D11 unbinds it)
		};

		bool IsSun(Part a_part) { return a_part == Part::SunMain || a_part == Part::SunCopy; }

		bool InPart(Part a_part, const Draw& a_draw)
		{
			return a_part == Part::Main || IsSun(a_part) || (a_part == Part::FaceClouds) == a_draw.occlusion;
		}

		namespace Queries = ::RenderGraph::Engine::OcclusionQueries;

		struct Client
		{
			View view = View::Main;
			Shape shape;
			std::string id;
			FrameAnchors::Anchor anchor = FrameAnchors::Anchor::Sky;
			std::shared_ptr<org::LatchBlock> latch;
			// Opened by the commit for its run only, so a recording replayed for another run draws nothing.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			std::array<FrameAnchors::Texture, 3> colours;
			FrameAnchors::Texture cubemap, depth, sceneDepth;
			FrameAnchors::Texture copyDepth;
			std::shared_ptr<Queries::QueryRing> queries;
			std::array<FrameAnchors::Texture, kCloudLayers> layers;
			// Null views for inputs the batch does not have.
			std::uint32_t null2D = GpuTextures::kInvalid, nullCube = GpuTextures::kInvalid;
			Family::Owners nullOwners;
			// Fixed at setup, so a new Publish needs no new client.
			Family::PublishedInputs inputs;
			// Written by the render thread, read by the host's thread.
			std::shared_ptr<const Run> run;
			// The run each commit drew. Passes may record on the submission thread after the render thread published the next run.
			std::array<::RenderGraph::RunSlots<Run>, kFaceInstances> runs;
			std::uint32_t instances = 1;  // kFaceInstances for the faces' client, else 1
			// In a CS stage the first face draw clears the face and its depth, and the engine's clears are dropped.
			bool clearsFace = false;
			bool pending = false;
			bool shapeLogged = false;
			std::atomic<std::uint64_t> executed{ 0 };  // runs submitted with this client's passes (its executed callback)
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			std::array<org::DeclaredViewToken, 3> colours{};
			std::array<org::DeclaredViewToken, kFaces> faces{};
			std::array<std::array<org::DeclaredViewToken, kFaces>, kCloudLayers> layers{};
			org::DeclaredViewToken depth{}, sceneDepth{}, cloudShadows{};
			bool hasDepth = false, hasSceneDepth = false, hasCloudShadows = false, hasLayers = false;
			std::size_t queryReadbacks = 0;  // the sun's parts: the query ring's readbacks declared (QueryRing::Resolve)
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;  // null when the pass records at execution: Record reads the client's run then
			std::shared_ptr<const Client> client;
			Part part = Part::Main;
			std::uint32_t instance = 0;
		};

		/** @brief Records one part of a run's draws in engine order. */
		class SkyPass final : public org::TypedRenderGraphPass<SkyPass, Frame, Bindings>
		{
		public:
			SkyPass(std::shared_ptr<Client> a_client, Part a_part, bool a_atExecution, std::uint32_t a_instance = 0) :
				client(std::move(a_client)), part(a_part), atExecution(a_atExecution), instance(a_instance) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				const auto& c = *client;
				if (IsSun(part)) {
					bindings.colours[0] = a_builder.RenderTarget(c.colours[0]).View();
					bindings.depth = a_builder.DepthReadWrite(part == Part::SunCopy ? c.copyDepth : c.depth).View();
					bindings.hasDepth = true;
					if (part == Part::SunMain) {
						bindings.sceneDepth = a_builder.ShaderResource(c.sceneDepth).View();
						bindings.hasSceneDepth = true;
					}
					if (c.queries)
						for (const auto& readback : c.queries->Readbacks()) {
							a_builder.CopyDestination(readback);
							++bindings.queryReadbacks;
						}
					return bindings;
				}
				if (part == Part::Main) {
					for (std::size_t i = 0; i < c.colours.size(); ++i)
						bindings.colours[i] = a_builder.RenderTarget(c.colours[i]).View();
				} else {
					for (std::uint32_t f = 0; f < kFaces; ++f)
						bindings.faces[f] = a_builder.RenderTarget(c.cubemap, org::RtvView{ UINT32_MAX, 0, f }).View();
				}
				if (part == Part::FaceClouds) {
					for (std::uint32_t l = 0; l < kCloudLayers; ++l)
						for (std::uint32_t f = 0; f < kFaces; ++f)
							bindings.layers[l][f] = a_builder.RenderTarget(c.layers[l], org::RtvView{ UINT32_MAX, 0, f }).View();
					bindings.hasLayers = true;
					// The face's depth is read, not tested, as Cloud Shadows binds it.
					bindings.sceneDepth = a_builder.ShaderResource(c.depth).View();
					bindings.hasSceneDepth = true;
				} else {
					bindings.depth = a_builder.DepthReadWrite(c.depth).View();
					bindings.hasDepth = true;
					if (part == Part::Main && c.sceneDepth) {
						bindings.sceneDepth = a_builder.ShaderResource(c.sceneDepth).View();
						bindings.hasSceneDepth = true;
					}
				}
				// Every registered input is declared, read this frame or not, so a new Publish needs no new client.
				client->inputs.Declare(a_builder);
				if (c.shape.cloudShadows) {
					bindings.cloudShadows = a_builder.ShaderResource(org::ResourceIdentifier(std::string(ORGCloudShadows::kOcclusionId))).View();
					bindings.hasCloudShadows = true;
				}
				return bindings;
			}

			// The latched values are not part of the revision. Recorded at execution, the pass reads the run itself, so the run is left out too.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(static_cast<std::uint64_t>(part));
				a_out.push_back(instance);
				if (!atExecution) {
					const auto run = std::atomic_load(&client->run);
					a_out.push_back(run ? run->revision : 0);
				}
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const
			{
				return { atExecution ? nullptr : std::atomic_load(&client->run), client, part, instance };
			}

			/** @brief A run whose commit opened none of the gate entries records only the pass's barriers. */
			static bool SkipsAtExecution(const Bindings&, const Frame& a_frame, const org::PassRecordContext& a_recording)
			{
				if (!a_frame.client)
					return true;
				const auto run = a_frame.run ? a_frame.run : a_frame.client->runs[a_frame.instance].Find(a_recording.Run());
				if (!run)
					return true;
				for (std::uint32_t i = 0; i < run->draws.size(); ++i)
					if (a_frame.client->gate->IsOpen(a_recording.Run(), FrameAnchors::InstanceEntry(a_frame.instance, kMaxDraws, i)))
						return false;
				return true;
			}

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.client)
					return;
				const auto runRef = a_frame.run ? a_frame.run : a_frame.client->runs[a_frame.instance].Find(a_recording.Run());
				if (!runRef)
					return;
				const auto& run = *runRef;
				const auto& c = *a_frame.client;
				const Part part = a_frame.part;
				// Only the sun part matching the run's depth target records, and only it touches the run's queries.
				if (IsSun(part) && (part == Part::SunCopy) != run.sunCopyDepth)
					return;
				SkyBlock block{};
				block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
				block.depth = part == Part::FaceClouds && a_bindings.hasSceneDepth ? a_recording.Index(a_bindings.sceneDepth) :
				              run.sceneDepthNull                                   ? c.null2D :
				              run.sceneDepthIndex != GpuTextures::kInvalid         ? run.sceneDepthIndex :
				              a_bindings.hasSceneDepth                             ? a_recording.Index(a_bindings.sceneDepth) :
				                                                                     c.null2D;
				block.sharedDepth = block.depth;
				block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : c.nullCube;
				block.headers = run.headerIndices;
				const std::uint32_t face = run.face < kFaces ? run.face : 0;
				const org::RunRef latchRun = a_recording.Run();

				// Clear as the engine does, depth 1 and stencil 0. The face colour clears to zero because the sky draws its background.
				bool clearFirst = part == Part::FaceSky && c.clearsFace;
				const auto draw = [&](const RasterProgram& a_program, std::uint32_t a_index, std::span<const RasterProgram::Colour> a_colours,
									  rhi::QueryPoolHandle a_queries = {}, std::uint32_t a_query = 0) {
					const auto& latched = run.draws[a_index];
					const std::uint32_t entry = FrameAnchors::InstanceEntry(a_frame.instance, kMaxDraws, a_index);
					block.draw = ShaderBindings::LatchOf(*c.latch, latchRun, entry * kDrawStride);
					const bool clears = std::exchange(clearFirst, false);
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false, .load = clears ? rhi::LoadOp::Clear : rhi::LoadOp::Load,
						.clear = 1.0f, .clearStencil = clears };
					std::array<RasterProgram::Colour, 3> colours{};
					std::copy(a_colours.begin(), a_colours.end(), colours.begin());
					if (clears && !colours.empty())
						colours[0].load = rhi::LoadOp::Clear;
					return a_program.Draw(a_recording, { .colours = { colours.data(), a_colours.size() },
													.depth = a_bindings.hasDepth ? &depth : nullptr,
													.viewport = { .x = latched.viewport.rect[0], .y = latched.viewport.rect[1], .width = latched.viewport.rect[2],
														.height = latched.viewport.rect[3], .minDepth = latched.viewport.depthRange[0], .maxDepth = latched.viewport.depthRange[1] },
													.constants = ShaderBindings::Data(block),
													.debugName = "Sky",
													.stencilReference = latched.stencilRef,
													.indirect = &c.gate->Latch(),
													.indirectOffset = c.gate->Offset(entry),
													.occlusionPool = a_queries,
													.occlusionIndex = a_query });
				};

				if (IsSun(part)) {
					// SunOcclude samples nothing, so headers and Cloud Shadows get null views.
					block.depth = a_bindings.hasSceneDepth ? a_recording.Index(a_bindings.sceneDepth) : c.null2D;
					block.sharedDepth = block.depth;
					block.cloudShadows = c.nullCube;
					block.headers = { c.null2D, c.null2D, c.nullCube, c.nullCube };
					const auto& queries = *c.queries;
					auto& commands = a_recording.Commands();
					const std::uint32_t perInstance = (std::max)(queries.PerRun() / c.instances, 1u);
					const std::uint32_t first = a_frame.instance * perInstance;
					queries.Reset(commands, latchRun, first, perInstance);
					std::uint32_t begun = 0;
					const RasterProgram::Colour colour{ .view = &a_bindings.colours[0] };
					for (std::uint32_t i = 0; i < run.draws.size() && i < perInstance; ++i) {
						const auto& latched = run.draws[i];
						if (latched.program &&
							draw(*static_cast<const RasterProgram*>(latched.program.get()), i, { &colour, 1 }, queries.Pool(latchRun), queries.Index(latchRun, first + i)))
							begun |= 1u << i;
					}
					queries.Resolve(commands, latchRun, begun, a_bindings.queryReadbacks, first, perInstance);
					return;
				}

				for (std::uint32_t i = 0; i < run.draws.size(); ++i) {
					const auto& latched = run.draws[i];
					if (!InPart(part, latched) || !latched.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(latched.program.get());
					std::array<RasterProgram::Colour, 3> colours{};
					std::uint32_t count = 0;
					if (part == Part::Main) {
						for (const auto& token : a_bindings.colours)
							colours[count++] = { .view = &token };
						draw(program, i, { colours.data(), count });
						continue;
					}
					colours[count++] = { .view = &a_bindings.faces[face] };
					if (part != Part::FaceClouds || !a_bindings.hasLayers || latched.layer < 0) {
						draw(program, i, { colours.data(), count });
						continue;
					}
					// Draw the occlusion into each later layer too, with the face target masked. That matches Cloud Shadows' D3D11 copy from the layer
					// below, so a layer holds the occlusion of every layer up to it.
					colours[count++] = { .view = &a_bindings.layers[latched.layer][face] };
					draw(program, i, { colours.data(), count });
					if (!latched.occlusionProgram)
						continue;
					const auto& occlusionProgram = *static_cast<const RasterProgram*>(latched.occlusionProgram.get());
					for (std::uint32_t j = i + 1; j < run.draws.size(); ++j) {
						const auto& later = run.draws[j];
						if (!later.occlusion || later.layer <= latched.layer)
							continue;
						colours[1] = { .view = &a_bindings.layers[later.layer][face] };
						draw(occlusionProgram, i, { colours.data(), count });
					}
				}
			}

		private:
			std::shared_ptr<Client> client;
			Part part;
			bool atExecution = false;  // FrameAnchors::RecordsAtExecution at the build
			std::uint32_t instance = 0;
		};

		/** @brief Whether a view's passes record after their run's commit, reading the run's draws then. */
		bool RecordsLate(FrameAnchors::Anchor a_anchor)
		{
			return FrameAnchors::RecordsAtExecution(a_anchor) || FrameAnchors::HeldByStage(a_anchor);
		}

		/** @brief ProgramKey::variant's defines: SKY_ORG_FACES for a face's programs. */
		Family::Defines VariantDefines(std::uint32_t a_variant)
		{
			if (a_variant & kVariantFaces)
				return { { L"SKY_ORG_FACES", L"" } };
			return {};
		}

		/**
		 * @brief The programs a sky pixel descriptor can be drawn with.
		 * The main view uses the DEFERRED form (| 256), the reflection faces the plain one.
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			// The sun glare is outside the sky batch and never drawn here. Its vertex stage reads SharedData, and with the vertex pull
			// DXC 1.9 emits invalid SPIR-V for it (VUID-10684).
			if (!a_pixel || (a_descriptor & 0xFF) == 1)
				return;
			a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel, .variant = (a_descriptor & 256) ? 0u : kVariantFaces });
		}

		/** @brief Render-thread state. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			std::array<::RenderGraph::Frame::DeclaredClient, kViews> declared;
			Family::EngineDrawPrograms programs{ "sky", { .source = kShader,
															.languageVersion = L"2018",
															.constantWords = ShaderBindings::Words<SkyBlock>(),
															.shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Sky),
															.variantDefines = &VariantDefines,
															.stagesOf = &StagesOf } };
			bool reserved = false;
			bool setUp = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		/**
		 * @brief Every published id a view's draws can read, listed at setup.
		 * Exporting an id not listed here is refused and logged once. Add the id here.
		 */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			constexpr std::array<std::uint32_t, kHeaderRegisters.size() + 1> kRegisters{ kHeaderRegisters[0], kHeaderRegisters[1], kHeaderRegisters[2],
				kHeaderRegisters[3], 17 };
			a_inputs.Add(Family::RegisteredIdsAt(kRegisters));
			a_inputs.Add(Family::RegisteredIdsAt(Integrations::PhysicalSky::kOrgTextureRegisters));
		}

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Makes the client state for a_shape's view and imports its targets. Null, logged, when a target cannot be imported. */
		std::shared_ptr<Client> MakeClient(const Shape& a_shape)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return nullptr;
			auto client = std::make_shared<Client>();
			client->view = a_shape.view;
			client->shape = a_shape;
			client->id = a_shape.view == View::Main ? "cs.sky.main" : a_shape.view == View::Face ? "cs.sky.faces" : "cs.sky.sun";
			client->anchor = a_shape.view == View::Main ? FrameAnchors::Anchor::Sky :
			                 a_shape.view == View::Face ? FrameAnchors::Anchor::SkyReflections :
			                                              FrameAnchors::Anchor::EngineQueries;
			if (a_shape.view == View::Face && FrameAnchors::HeldByStage(client->anchor)) {
				client->instances = kFaceInstances;
				client->clearsFace = true;
			}
			// The sun client in CS stages takes one instance per query a stage run may take. Its first tap in a run is instance 0.
			if (a_shape.view == View::Sun && FrameAnchors::NamedByStage(client->id))
				client->instances = kSunInstances;
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", client->instances * kMaxDraws * kDrawStride);
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", client->instances * kMaxDraws);
			if (!client->gate) {
				logger::warn("[DCLF] sky: the {} run gate could not be made; the sky stays the engine's", client->id);
				return nullptr;
			}
			auto& textures = GpuTextures::Get();
			for (const auto kind : { GpuTextures::ViewKind::Texture2D, GpuTextures::ViewKind::Cube }) {
				auto binding = textures.NullOf(kind);
				if (binding.index == GpuTextures::kInvalid) {
					logger::warn("[DCLF] sky: the null views could not be made; the sky stays the engine's");
					return nullptr;
				}
				(kind == GpuTextures::ViewKind::Cube ? client->nullCube : client->null2D) = binding.index;
				client->nullOwners.push_back(std::move(binding.owner));
			}
			if (a_shape.view == View::Sun) {
				client->colours[0] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
				client->copyDepth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(kSceneDepth));
				client->sceneDepth = client->copyDepth;
				client->queries = ::RenderGraph::Engine::OcclusionQueries::QueryRing::Create(client->id + ".queries", client->instances);
				if (!client->colours[0] || !client->copyDepth || !client->queries) {
					logger::warn("[DCLF] sky: the sun's occlusion client cannot be made (kMAIN, the Z-prepass copy or its query ring); the sun's queries stay the engine's");
					return nullptr;
				}
			} else if (a_shape.view == View::Main) {
				for (std::size_t i = 0; i < client->colours.size(); ++i) {
					client->colours[i] = a_shape.colours[i] >= 0 ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_shape.colours[i])) : nullptr;
					if (!client->colours[i]) {
						logger::warn("[DCLF] sky: the main pass's target {} ({}) cannot be imported; the sky stays the engine's", i, a_shape.colours[i]);
						return nullptr;
					}
				}
				client->sceneDepth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(kSceneDepth));
				if (!client->sceneDepth) {
					logger::warn("[DCLF] sky: the Z-prepass copy CS binds at t17 cannot be imported; the sky stays the engine's");
					return nullptr;
				}
			} else {
				client->cubemap = FrameAnchors::EngineCubemap(static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS));
				if (!client->cubemap) {
					logger::warn("[DCLF] sky: the reflection cubemap cannot be imported; the faces' sky stays the engine's");
					return nullptr;
				}
			}
			if (a_shape.view != View::Sun)
				ListInputs(client->inputs);
			if (a_shape.view == View::Main && a_shape.cloudShadows)
				for (std::uint32_t l = 0; l < kCloudLayers; ++l)
					client->inputs.Add(ORGCloudShadows::PublishedLayerId(static_cast<int>(l)));
			client->depth = a_shape.depth >= 0 ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_shape.depth)) : nullptr;
			if (!client->depth) {
				logger::warn("[DCLF] sky: the depth target {} cannot be imported; the sky stays the engine's", a_shape.depth);
				return nullptr;
			}
			const bool faceClouds = a_shape.view == View::Face && a_shape.cloudShadows && a_shape.clouds;
			if (faceClouds) {
				for (std::uint32_t l = 0; l < kCloudLayers; ++l) {
					client->layers[l] = ORGCloudShadows::ImportLayer(static_cast<int>(l));
					if (!client->layers[l]) {
						logger::warn("[DCLF] sky: Cloud Shadows' layer {} cannot be imported; the faces' sky stays the engine's", l);
						return nullptr;
					}
				}
			}
			return client;
		}

		/** @brief What a registration is made for: how many query readbacks the sun's passes declare (0 for the other views). */
		struct ClientKey
		{
			std::size_t readbacks = 0;
			bool operator==(const ClientKey&) const = default;
		};

		/** @brief The registration of a_client. Its passes declare the targets imported at setup and every readback the ring has now. */
		bool MakeDesc(const std::shared_ptr<Client>& a_client, FrameAnchors::ClientDesc& a_desc)
		{
			const auto& shape = a_client->shape;
			const bool faceClouds = shape.view == View::Face && shape.cloudShadows && shape.clouds;
			a_desc.id = a_client->id;
			a_desc.sharedConstants = true;
			a_desc.gates = { a_client->gate };
			const auto addPass = [&](const char* a_name, Part a_part, std::int32_t a_order) {
				FrameAnchors::PassDesc pass{ .name = a_client->id + "." + a_name,
					.anchor = a_client->anchor,
					.order = a_order,
					.kind = FrameAnchors::PassKind::Render,
					.make = [client = a_client, a_part]() -> std::shared_ptr<org::RenderPass> {
						return std::make_shared<SkyPass>(client, a_part, RecordsLate(client->anchor));
					},
					// Recorded after the commit, when the draws are known. A face pass's clears are its first draw's load ops, held back by the gate.
					.atExecution = true };
				if (a_client->instances > 1) {
					pass.make = nullptr;
					pass.makeInstance = [client = a_client, a_part](std::uint32_t a_instance) -> std::shared_ptr<org::RenderPass> {
						return std::make_shared<SkyPass>(client, a_part, RecordsLate(client->anchor), a_instance);
					};
				}
				a_desc.passes.push_back(std::move(pass));
			};
			if (shape.view == View::Sun) {
				addPass("occlusion", Part::SunMain, 0);
				addPass("occlusion-copy", Part::SunCopy, 1);
			} else if (shape.view == View::Main) {
				addPass("draws", Part::Main, 0);
			} else {
				addPass("draws", Part::FaceSky, 0);
				if (faceClouds)
					addPass("clouds", Part::FaceClouds, 1);
			}
			a_desc.commit = [client = a_client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!host)
					return;
				const org::RunRef latchRun = host->CurrentRun();
				// The k-th face the start stage's run draws this frame, 0 on an anchor's own epoch.
				const std::uint32_t instance = (std::min)(FrameAnchors::TapInstance(), client->instances - 1);
				client->runs[instance].Store(latchRun, run);
				if (!run)
					return;
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					const std::uint32_t entry = FrameAnchors::InstanceEntry(instance, kMaxDraws, i);
					client->latch->WriteValue(latchRun, entry * kDrawStride, draw.constants);
					if (draw.program)
						client->gate->OpenDraw(entry, draw.indexCount, 1);
				}
				// A new readback chunk changes the key, so the next revision registers the client again with it.
				if (client->queries && client->queries->Commit(latchRun, std::span<const std::int32_t>(&run->query, 1), instance))
					GetState().declared[static_cast<std::size_t>(client->view)].Invalidate();
			};
			a_desc.executed = [client = a_client](const FrameAnchors::Point&) {
				if (client->queries)
					client->queries->Executed();
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] sky: the first {} run drawn by the graph", client->id);
			};
			a_desc.active = [client = a_client] { return client->pending; };
			if (a_client->instances > 1) {
				a_desc.maxInstances = a_client->instances;
				// Every face instance is declared because the engine decides late how many faces it draws. Unused instances stay closed.
				a_desc.instances = [n = a_client->instances] { return n; };
			}
			const auto late = a_client->inputs.Unregistered();
			logger::info("[DCLF] sky: the {} client is made (targets {} {} {}, depth {}, cloud shadows {}, cloud layers {}, published inputs it may read: {}{})",
				a_client->id, shape.colours[0], shape.colours[1], shape.colours[2], shape.depth, shape.cloudShadows ? "on" : "off",
				shape.clouds ? "the graph's" : "the engine's", a_client->inputs.Describe(), late.empty() ? std::string() : std::format("; not published yet: {}", late));
			return true;
		}

		/** @brief What a run's recording depends on: everything but the latched values. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.face);
			mix(a_run.sunCopyDepth);
			mix(a_run.draws.size());
			mix(a_run.sceneDepthIndex);
			mix(a_run.sceneDepthNull);
			for (const auto index : a_run.headerIndices)
				mix(index);
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix(reinterpret_cast<std::uintptr_t>(draw.occlusionProgram.get()));
				mix((std::uint64_t(draw.occlusion) << 32) | static_cast<std::uint32_t>(draw.layer));
				mix(draw.stencilRef);
				for (const auto v : draw.viewport.rect)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.viewport.depthRange.data(), sizeof(range));
				mix((std::uint64_t(range[0]) << 32) | range[1]);
			}
			return hash;
		}
	}

	bool Available()
	{
		const auto& state = GetState();
		return state.reserved && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::Sky) &&
		       FrameAnchors::HasEpoch(FrameAnchors::Anchor::SkyReflections);
	}

	void Reserve()
	{
		auto& state = GetState();
		state.reserved = FrameAnchors::Reserve(FrameAnchors::Anchor::Sky) && FrameAnchors::Reserve(FrameAnchors::Anchor::SkyReflections);
		if (!state.reserved)
			logger::warn("[DCLF] sky: the Sky anchors could not be reserved; the sky stays the engine's");
		if (state.reserved && ::RenderGraph::Engine::OcclusionQueries::Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EngineQueries))
			logger::warn("[DCLF] sky: the EngineQueries anchor could not be reserved; the sun's occlusion queries stay the engine's");
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp)
			return true;
		if (!RenderGraphRuntime::Get().Host() || !FrameAnchors::IsRenderOwner() || !Available())
			return false;
		state.setUp = true;
		Family::StartStagePrecompile();
		state.programs.Prebuild();
		if (!::RenderGraph::Engine::Constants::Requested()) {
			logger::warn("[DCLF] sky: needs the engine constants capture (CS_ORG_ENGINE_CONSTANTS=1); the sky stays the engine's");
			return true;
		}
		const bool cloudShadows = globals::features::cloudShadows.loaded;
		if (cloudShadows && !ORGCloudShadows::Get().IsActive()) {
			logger::warn("[DCLF] sky: Cloud Shadows runs on D3D11 (its occlusion, t25, is not on the graph: CS_ORG_CLOUD_SHADOWS=1); the sky stays the engine's");
			return true;
		}
		// The shapes the views draw with. The main pass writes the deferred pass's first three targets with kMAIN's depth. A face writes
		// the reflection cubemap with its cube depth.
		Shape main;
		main.view = View::Main;
		main.colours = { static_cast<std::int32_t>(RE::RENDER_TARGETS::kMAIN), static_cast<std::int32_t>(RE::RENDER_TARGETS::kMOTION_VECTOR),
			static_cast<std::int32_t>(NORMALROUGHNESS) };
		main.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
		main.cloudShadows = cloudShadows;
		main.clouds = Address::kCloudGroup.Known();
		Shape face;
		face.view = View::Face;
		face.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS);
		face.cloudShadows = cloudShadows;
		face.clouds = Address::kCloudGroup.Known();
		std::vector<Shape> shapes{ main, face };
		if (::RenderGraph::Engine::OcclusionQueries::Active() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EngineQueries)) {
			Shape sun;
			sun.view = View::Sun;
			sun.colours = { static_cast<std::int32_t>(RE::RENDER_TARGETS::kMAIN), -1, -1 };
			sun.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			shapes.push_back(sun);
		}
		namespace RF = ::RenderGraph::Frame;
		for (const auto& shape : shapes) {
			auto client = MakeClient(shape);
			if (!client)
				continue;
			// The targets are the engine's, made once with its other targets, and imported above. The key is the sun ring's
			// readback count, so a chunk the ring makes later registers the client again with the next revision.
			auto declared = RF::DeclareClient<ClientKey>(
				client->id, RF::DependsOn(),
				[client](const RF::Inputs&) -> std::optional<ClientKey> { return ClientKey{ client->queries ? client->queries->Readbacks().size() : 0 }; },
				[client](const ClientKey&, FrameAnchors::ClientDesc& a_desc) { return MakeDesc(client, a_desc); });
			if (!declared) {
				logger::warn("[DCLF] sky: the {} client could not be declared; the sky stays the engine's", client->id);
				continue;
			}
			logger::info("[DCLF] sky: the {} client declared at setup; it registers with the next frame revision", client->id);
			state.declared[static_cast<std::size_t>(shape.view)] = std::move(declared);
			state.clients[static_cast<std::size_t>(shape.view)] = std::move(client);
		}
		return true;
	}

	bool CanTake(const Shape& a_shape)
	{
		auto& state = GetState();
		if (a_shape.view >= View::Count)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_shape.view)];
		// Until a revision registers the client, the engine draws the batch.
		if (!client || !state.declared[static_cast<std::size_t>(a_shape.view)].Registered())
			return false;
		// No client is registered for other targets during play, so a batch with other targets stays the engine's.
		if (client->shape != a_shape) {
			if (!std::exchange(client->shapeLogged, true))
				logger::warn("[DCLF] sky: a {} batch draws into targets {} {} {} with depth {} (cloud shadows {}, cloud layers {}), not its client's; those batches stay the engine's",
					client->id, a_shape.colours[0], a_shape.colours[1], a_shape.colours[2], a_shape.depth, a_shape.cloudShadows, a_shape.clouds);
			return false;
		}
		return Family::HeldForBatch("sky", client->id,
			static_cast<std::uint32_t>(a_shape.view == View::Face ? ::RenderGraph::Frame::SegmentId::Start : ::RenderGraph::Frame::SegmentId::PreMain), client->anchor);
	}

	bool HasSunClient()
	{
		const auto& state = GetState();
		return state.clients[static_cast<std::size_t>(View::Sun)] != nullptr;
	}

	bool CanTakeSun()
	{
		const auto& state = GetState();
		const auto& client = state.clients[static_cast<std::size_t>(View::Sun)];
		if (!client || !state.declared[static_cast<std::size_t>(View::Sun)].Registered())
			return false;
		return Family::HeldForBatch("sun", client->id, static_cast<std::uint32_t>(::RenderGraph::Frame::SegmentId::Sun), client->anchor);
	}

	Family::InputResult ReadPublished(View a_view, std::string_view a_id, std::uint32_t& a_index)
	{
		if (a_view >= View::Count)
			return Family::InputResult::NotListed;
		const auto& client = GetState().clients[static_cast<std::size_t>(a_view)];
		return client ? client->inputs.Resolve(a_id, a_index) : Family::InputResult::NotListed;
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader)
	{
		return GetState().programs.Get(a_key, a_shader);
	}

	void Keep(Family::Owners&& a_owners)
	{
		GetState().kept.Keep(std::move(a_owners));
	}

	bool Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_run->view)];
		if (!client)
			return false;
		a_run->revision = RevisionOf(*a_run);
		state.kept.Keep(std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		return Family::RunWithheld("sky", client->id, client->anchor, client->pending);
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_SKY)";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} programs; occlusion queries: {}", state.runs, executed, state.draws, state.programs.Built(),
			::RenderGraph::Engine::OcclusionQueries::Status());
	}
}
