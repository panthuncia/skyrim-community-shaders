// The sky family's render graph side: a client per view at the Sky and SkyReflections anchors, made once at setup, whose passes draw a
// batch's latched draws in the engine's order with CS's Sky.hlsl built for ORG, and the faces' cloud draws into Cloud Shadows' layers.
// With CS_ORG_OCCLUSION_QUERIES=1 a third client at the EngineQueries anchor draws the sun's occlusion draw with an ORG occlusion query.

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
#include "RenderGraph/FrameAnchors.h"
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
#include <string_view>

namespace DCLF::Features::Sky::SkyDraws
{
	namespace
	{
		constexpr const char* kShader = "Sky.hlsl";
		// The most draws a batch latches; a batch's draws past it are the engine's. A cloudy weather draws about 40.
		constexpr std::uint32_t kMaxDraws = 128;
		constexpr std::uint32_t kFaces = 6;
		constexpr std::uint32_t kDrawStride = static_cast<std::uint32_t>(sizeof(DrawConstants));
		constexpr std::size_t kViews = static_cast<std::size_t>(View::Count);

		/** @brief Sky.hlsl's push block (SKY_ORG_RESOURCES), in list order. Everything per draw is in the latch (DrawConstants). */
		struct SkyBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle depth, sharedDepth, cloudShadows;
			std::array<ShaderBindings::Handle, kHeaderRegisters.size()> headers;  // EnvIBLTexture, SkyIBLTexture, EnvReflectionsTexture, EnvTexture
			ShaderBindings::Latch draw;
		};
		static_assert(sizeof(SkyBlock) == 52, "Sky.hlsl's push block is 52 bytes");

		/** @brief Which of a run's draws a pass records. */
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

		/** @brief A client's state, which its passes share. */
		struct Client
		{
			View view = View::Main;
			Shape shape;
			std::string id;
			FrameAnchors::Anchor anchor = FrameAnchors::Anchor::Sky;
			std::shared_ptr<org::LatchBlock> latch;
			// A draw's index count, opened by the commit for its run only: a recording replayed for a run that did not commit draws nothing.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The targets as imported at the registration (FrameAnchors keeps each while the engine's texture stays the same).
			std::array<std::shared_ptr<org::ExternalTextureResource>, 3> colours;
			std::shared_ptr<org::ExternalTextureResource> cubemap, depth, sceneDepth;
			// View::Sun: kPOST_ZPREPASS_COPY as a depth target (the import sceneDepth holds), and the ORG queries of its runs.
			std::shared_ptr<org::ExternalTextureResource> copyDepth;
			std::shared_ptr<Queries::QueryRing> queries;
			// Cloud Shadows' layers, which the faces' cloud draws write (ORGCloudShadows::ImportLayer).
			std::array<std::shared_ptr<org::ExternalTextureResource>, kCloudLayers> layers;
			// Null views of the dimensions the push block's handles declare, for an input the batch does not have.
			std::uint32_t null2D = GpuTextures::kInvalid, nullCube = GpuTextures::kInvalid;
			Family::Owners nullOwners;
			// Every published id the view's draws can read, fixed here (ListInputs). Each build declares the registered ones.
			Family::PublishedInputs inputs;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
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
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const Client> client;
			Part part = Part::Main;
		};

		/** @brief A run's draws of one part, recorded in the engine's order: a render pass and a draw each (RasterProgram::Draw). */
		class SkyPass final : public org::TypedRenderGraphPass<SkyPass, Frame, Bindings>
		{
		public:
			SkyPass(std::shared_ptr<Client> a_client, Part a_part) :
				client(std::move(a_client)), part(a_part) {}

			Bindings Declare(org::PassBuilder& a_builder)
			{
				Bindings bindings{};
				bindings.shared = SharedConstants::Declare(a_builder);
				const auto& c = *client;
				if (IsSun(part)) {
					// kMAIN alone (SunOcclude writes no colour), the depth target of the part, t17 for the main depth's part.
					bindings.colours[0] = a_builder.RenderTarget(c.colours[0]).View();
					bindings.depth = a_builder.DepthReadWrite(part == Part::SunCopy ? c.copyDepth : c.depth).View();
					bindings.hasDepth = true;
					if (part == Part::SunMain) {
						bindings.sceneDepth = a_builder.ShaderResource(c.sceneDepth).View();
						bindings.hasSceneDepth = true;
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
					// The face's depth is read (t17) instead of tested, as Cloud Shadows binds it for these draws.
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
				// The published inputs, declared for their order only: the run carries their indices. Every registered one is declared,
				// read this frame or not, so a new Publish needs no new client.
				client->inputs.Declare(a_builder);
				if (c.shape.cloudShadows) {
					bindings.cloudShadows = a_builder.ShaderResource(org::ResourceIdentifier(std::string(ORGCloudShadows::kOcclusionId))).View();
					bindings.hasCloudShadows = true;
				}
				return bindings;
			}

			// The run's draws (their programs, counts, layers and viewports), its face and its unpublished inputs, as the render thread
			// published it before the run. The latched values (constants, geometry, textures) are not in it.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(static_cast<std::uint64_t>(part));
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const { return { std::atomic_load(&client->run), client, part }; }

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.run || !a_frame.client)
					return;
				const auto& run = *a_frame.run;
				const auto& c = *a_frame.client;
				const Part part = a_frame.part;
				// The sun's parts: only the one of the run's depth target records, and it alone touches the run's queries.
				if (IsSun(part) && (part == Part::SunCopy) != run.sunCopyDepth)
					return;
				SkyBlock block{};
				block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
				// t17: the face's depth for its cloud draws; the main pass's own index (a D3D11 view, a published input), a null view, or the
				// client's Z-prepass copy.
				block.depth = part == Part::FaceClouds && a_bindings.hasSceneDepth ? a_recording.Index(a_bindings.sceneDepth) :
				              run.sceneDepthNull                                   ? c.null2D :
				              run.sceneDepthIndex != GpuTextures::kInvalid         ? run.sceneDepthIndex :
				              a_bindings.hasSceneDepth                             ? a_recording.Index(a_bindings.sceneDepth) :
				                                                                     c.null2D;
				block.sharedDepth = block.depth;
				block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : c.nullCube;
				block.headers = run.headerIndices;
				const std::uint32_t face = run.face < kFaces ? run.face : 0;
				const std::uint32_t slot = a_recording.FrameSlot();

				const auto draw = [&](const RasterProgram& a_program, std::uint32_t a_index, std::span<const RasterProgram::Colour> a_colours,
									  rhi::QueryPoolHandle a_queries = {}, std::uint32_t a_query = 0) {
					const auto& latched = run.draws[a_index];
					block.draw = ShaderBindings::LatchOf(*c.latch, slot, a_index * kDrawStride);
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					return a_program.Draw(a_recording, { .colours = a_colours,
													.depth = a_bindings.hasDepth ? &depth : nullptr,
													.viewport = { .x = latched.viewport.rect[0], .y = latched.viewport.rect[1], .width = latched.viewport.rect[2],
														.height = latched.viewport.rect[3], .minDepth = latched.viewport.depthRange[0], .maxDepth = latched.viewport.depthRange[1] },
													.constants = ShaderBindings::Data(block),
													.debugName = "Sky",
													.stencilReference = latched.stencilRef,
													// The count comes from the draw's gate entry, which the client's commit opens for this run only.
													.indirect = &c.gate->Latch(),
													.indirectOffset = c.gate->Offset(a_index),
													.occlusionPool = a_queries,
													.occlusionIndex = a_query });
				};

				if (IsSun(part)) {
					// Its inputs: t17 as the part declares it, null views for the headers and Cloud Shadows (SunOcclude samples none).
					block.depth = a_bindings.hasSceneDepth ? a_recording.Index(a_bindings.sceneDepth) : c.null2D;
					block.sharedDepth = block.depth;
					block.cloudShadows = c.nullCube;
					block.headers = { c.null2D, c.null2D, c.nullCube, c.nullCube };
					// Each draw counted by its own query of the frame slot's range: reset first, resolved after (every query available).
					const auto& queries = *c.queries;
					auto& commands = a_recording.Commands();
					queries.Reset(commands, slot);
					std::uint32_t begun = 0;
					const RasterProgram::Colour colour{ .view = &a_bindings.colours[0] };
					for (std::uint32_t i = 0; i < run.draws.size() && i < queries.PerRun(); ++i) {
						const auto& latched = run.draws[i];
						if (latched.program && draw(*static_cast<const RasterProgram*>(latched.program.get()), i, { &colour, 1 }, queries.Pool(), queries.Index(slot, i)))
							begun |= 1u << i;
					}
					queries.Resolve(commands, slot, begun);
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
					// A cloud draw writes its own layer, then its occlusion into each later layer the face draws (their order is the engine's,
					// strictly rising: SkyCapture), with the face's target masked: a layer then holds the occlusion of every layer up to it,
					// as Cloud Shadows' copy from the layer below makes it on D3D11. The layers start cleared (CloudShadows::NoteGraphCloudLayer).
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
		};

		/** @brief ProgramKey::variant's defines: SKY_ORG_FACES for a face's programs. */
		Family::Defines VariantDefines(std::uint32_t a_variant)
		{
			if (a_variant & kVariantFaces)
				return { { L"SKY_ORG_FACES", L"" } };
			return {};
		}

		/**
		 * @brief The programs a BSSkyShader pixel descriptor CS loads can be drawn with. CS loads each descriptor plain and with DEFERRED
		 * (| 256, State::ModifyShaderLookup's main pass form): the main view draws with the deferred ones, the reflection faces with the
		 * plain ones (variant SKY_ORG_FACES).
		 */
		void StagesOf(bool a_pixel, std::uint32_t a_descriptor, std::vector<Family::StageRequest>& a_out)
		{
			// The sun glare (raw technique 1, pass 0x5C006072) is outside the sky batch's techniques (0x5C00005D-0x5C000064): the family
			// never draws it. Its ORG vertex stage would not build either: its GetSunGlareVisibility reads SharedData in the vertex stage,
			// which with the vertex pull makes DXC 1.9 emit invalid SPIR-V (a ConstantBuffer<T> in Function storage, VUID-10684).
			if (!a_pixel || (a_descriptor & 0xFF) == 1)
				return;
			a_out.push_back({ .descriptor = a_descriptor, .stage = Family::DefineStage::Pixel, .variant = (a_descriptor & 256) ? 0u : kVariantFaces });
		}

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			Family::EngineDrawPrograms programs{ "sky", { .source = kShader,
															.languageVersion = L"2018",
															.constantWords = ShaderBindings::Words<SkyBlock>(),
															.shaderType = static_cast<std::uint32_t>(RE::BSShader::Type::Sky),
															.variantDefines = &VariantDefines,
															.stagesOf = &StagesOf } };
			bool reserved = false;
			bool setUp = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		/**
		 * @brief Every published id a view's draws can read, listed when its client is made at setup: what the ported features publish at
		 * the headers' registers and at t17 (DCLF's registry: IBL's, Dynamic Cubemaps' and Terrain Blending's ids). Cloud Shadows'
		 * occlusion is declared on its own (Shape::cloudShadows), and the Z-prepass copy at t17 is imported as a target. An export of an
		 * id not listed here is an inherent refusal, logged once with the id: add the id here.
		 */
		void ListInputs(Family::PublishedInputs& a_inputs)
		{
			constexpr std::array<std::uint32_t, kHeaderRegisters.size() + 1> kRegisters{ kHeaderRegisters[0], kHeaderRegisters[1], kHeaderRegisters[2],
				kHeaderRegisters[3], 17 };
			a_inputs.Add(Family::RegisteredIdsAt(kRegisters));
		}

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Stops the family for good: every batch is the engine's again. */
		void Fail(const char* a_why)
		{
			auto& state = GetState();
			if (!state.failed)
				logger::error("[DCLF] sky: back to the engine's draws for the session: {}", a_why);
			state.failed = true;
			for (auto& client : state.clients)
				if (client)
					FrameAnchors::Unregister(client->id);
		}

		/** @brief The client of a_shape's view, registered with the anchors for a_shape. Null, logged, when a target cannot be imported. */
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
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * kDrawStride, host->FrameSlots());
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kMaxDraws);
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
				// kMAIN, with kMAIN's depth or the Z-prepass copy as the depth target, and that copy at t17 with kMAIN's.
				client->colours[0] = FrameAnchors::EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGETS::kMAIN));
				client->copyDepth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(kSceneDepth));
				client->sceneDepth = client->copyDepth;
				client->queries = ::RenderGraph::Engine::OcclusionQueries::QueryRing::Create(client->id + ".queries", 1);
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
			client->depth = a_shape.depth >= 0 ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_shape.depth)) : nullptr;
			if (!client->depth) {
				logger::warn("[DCLF] sky: the depth target {} cannot be imported; the sky stays the engine's", a_shape.depth);
				return nullptr;
			}
			// The faces' cloud draws write Cloud Shadows' layers.
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

			FrameAnchors::ClientDesc desc{};
			desc.id = client->id;
			desc.sharedConstants = true;
			desc.gates = { client->gate };
			const auto addPass = [&](const char* a_name, Part a_part, std::int32_t a_order) {
				desc.passes.push_back({ .name = client->id + "." + a_name,
					.anchor = client->anchor,
					.order = a_order,
					.kind = FrameAnchors::PassKind::Render,
					.make = [client, a_part]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<SkyPass>(client, a_part); } });
			};
			if (a_shape.view == View::Sun) {
				addPass("occlusion", Part::SunMain, 0);
				addPass("occlusion-copy", Part::SunCopy, 1);
			} else if (a_shape.view == View::Main) {
				addPass("draws", Part::Main, 0);
			} else {
				addPass("draws", Part::FaceSky, 0);
				if (faceClouds)
					addPass("clouds", Part::FaceClouds, 1);
			}
			desc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!run || !host)
					return;
				const std::uint32_t slot = host->CurrentFrameSlot();
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(slot, i * kDrawStride, draw.constants);
					if (draw.program)
						client->gate->OpenDraw(i, draw.indexCount, 1);
				}
				// The engine ring slot the run's draw counts.
				if (client->queries)
					client->queries->Commit(slot, std::span<const std::int32_t>(&run->query, 1));
			};
			desc.executed = [client](const FrameAnchors::Point&) {
				if (client->queries)
					client->queries->Executed();
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] sky: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the sky's passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] sky: the {} client could not be registered; the sky stays the engine's", client->id);
				return nullptr;
			}
			const auto late = client->inputs.Unregistered();
			logger::info("[DCLF] sky: the {} client registered at setup (targets {} {} {}, depth {}, cloud shadows {}, cloud layers {}, published inputs it may read: {}{})",
				client->id, a_shape.colours[0], a_shape.colours[1], a_shape.colours[2], a_shape.depth, a_shape.cloudShadows ? "on" : "off",
				a_shape.clouds ? "the graph's" : "the engine's", client->inputs.Describe(), late.empty() ? std::string() : std::format("; not published yet: {}", late));
			// The one build request: the next build point (a menu frame's end) builds the graph with the client, before any world frame.
			FrameAnchors::RequestJoin(client->id, client->anchor);
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched values. */
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
		return state.reserved && !state.failed && RenderGraphRuntime::Get().IsActive() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::Sky) &&
		       FrameAnchors::HasEpoch(FrameAnchors::Anchor::SkyReflections);
	}

	void Reserve()
	{
		auto& state = GetState();
		state.reserved = FrameAnchors::Reserve(FrameAnchors::Anchor::Sky) && FrameAnchors::Reserve(FrameAnchors::Anchor::SkyReflections);
		if (!state.reserved)
			logger::warn("[DCLF] sky: the Sky anchors could not be reserved; the sky stays the engine's");
		// The sun's occlusion queries (CS_ORG_OCCLUSION_QUERIES): their anchor, or the engine keeps them.
		if (state.reserved && ::RenderGraph::Engine::OcclusionQueries::Requested() && !FrameAnchors::Reserve(FrameAnchors::Anchor::EngineQueries))
			logger::warn("[DCLF] sky: the EngineQueries anchor could not be reserved; the sun's occlusion queries stay the engine's");
	}

	bool Setup()
	{
		auto& state = GetState();
		if (state.setUp || state.failed)
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
		// The shapes the views draw with (CurrentShape reads them per batch, and a batch of another shape is the engine's): the main
		// pass inside CS's deferred pass writes its first three targets (Deferred::StartDeferred: kMAIN, kMOTION_VECTOR,
		// NORMALROUGHNESS) with kMAIN's depth; a reflection face writes the reflection cubemap with its cube depth.
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
		for (const auto& shape : { main, face })
			state.clients[static_cast<std::size_t>(shape.view)] = MakeClient(shape);
		// The sun's occlusion draw (CS_ORG_OCCLUSION_QUERIES with its detours in): kMAIN and the depth targets it draws with.
		if (::RenderGraph::Engine::OcclusionQueries::Active() && FrameAnchors::HasEpoch(FrameAnchors::Anchor::EngineQueries)) {
			Shape sun;
			sun.view = View::Sun;
			sun.colours = { static_cast<std::int32_t>(RE::RENDER_TARGETS::kMAIN), -1, -1 };
			sun.depth = static_cast<std::int32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			state.clients[static_cast<std::size_t>(View::Sun)] = MakeClient(sun);
		}
		return true;
	}

	bool CanTake(const Shape& a_shape)
	{
		auto& state = GetState();
		if (state.failed || a_shape.view >= View::Count)
			return false;
		const auto& client = state.clients[static_cast<std::size_t>(a_shape.view)];
		if (!client)
			return false;
		// No client is registered again during play (a graph rebuild on the frame's path): a batch of other targets is the engine's.
		if (client->shape != a_shape) {
			if (!std::exchange(client->shapeLogged, true))
				logger::warn("[DCLF] sky: a {} batch draws into targets {} {} {} with depth {} (cloud shadows {}, cloud layers {}), not its client's; those batches stay the engine's",
					client->id, a_shape.colours[0], a_shape.colours[1], a_shape.colours[2], a_shape.depth, a_shape.cloudShadows, a_shape.clouds);
			return false;
		}
		if (!FrameAnchors::CanRun(client->id, client->anchor)) {
			static Util::ReasonLog log("[DCLF] sky: the engine draws a batch", spdlog::level::err);
			log.Report(std::format("{}: its anchor cannot run now (not in the built graph, or its ring is full)", client->id));
			return false;
		}
		return true;
	}

	bool HasSunClient()
	{
		const auto& state = GetState();
		return !state.failed && state.clients[static_cast<std::size_t>(View::Sun)] != nullptr;
	}

	bool CanTakeSun()
	{
		const auto& state = GetState();
		const auto& client = state.clients[static_cast<std::size_t>(View::Sun)];
		if (state.failed || !client)
			return false;
		if (!FrameAnchors::CanRun(client->id, client->anchor)) {
			static Util::ReasonLog log("[DCLF] sky: a sun occlusion query answers its last count", spdlog::level::err);
			log.Report(std::format("{}: its anchor cannot run now (not in the built graph, or its ring is full)", client->id));
			return false;
		}
		return true;
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
		if (!a_run || state.failed)
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
		if (state.failed)
			return "failed: the engine draws the sky";
		std::uint64_t executed = 0;
		for (const auto& client : state.clients)
			if (client)
				executed += client->executed.load(std::memory_order_relaxed);
		return fmt::format("{} runs ({} drawn), {} draws, {} programs; occlusion queries: {}", state.runs, executed, state.draws, state.programs.Built(),
			::RenderGraph::Engine::OcclusionQueries::Status());
	}
}
