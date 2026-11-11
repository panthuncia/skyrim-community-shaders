// The sky family's render graph side: a client per view at the Sky and SkyReflections anchors, whose passes draw a batch's latched draws
// in the engine's order with CS's Sky.hlsl built for ORG, and the faces' cloud occlusion target Cloud Shadows' ORG copy reads.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "Features/CloudShadows.h"
#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/StateConversion.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/GraphResources.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/RasterProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/RunGate.h"
#include "RenderGraph/ShaderBindings.h"
#include "RenderGraph/SharedConstants.h"
#include "ShaderCache.h"

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/ExternalTextureResource.h>
#include <rhi_helpers.h>

#include <atomic>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>

namespace DCLF::Features::Sky::SkyDraws
{
	namespace
	{
		constexpr const char* kShader = "Sky.hlsl";
		// The most draws a batch latches; a batch's draws past it are the engine's. A cloudy weather draws about 40.
		constexpr std::uint32_t kMaxDraws = 128;
		constexpr std::uint32_t kFaces = 6;
		// Runs whose imports and leases are kept: the frames the graph may run behind, times the runs a frame has (the main pass and
		// six faces), with a margin.
		constexpr std::uint32_t kKeptFrames = RenderGraphRuntime::kGameFramesInFlight + 2;

		/** @brief Sky.hlsl's push block (SKY_ORG_RESOURCES), in list order. */
		struct SkyBlock
		{
			SharedConstants::Handles shared;
			ShaderBindings::Handle base, blend, noise, depth;
			ShaderBindings::Handle sharedDepth, cloudShadows, cloudSelfShadow;
			std::array<ShaderBindings::Handle, kHeaderRegisters.size()> headers;  // EnvIBLTexture, SkyIBLTexture, EnvReflectionsTexture, EnvTexture
			ShaderBindings::Handle baseSampler, blendSampler, noiseSampler;
			ShaderBindings::Latch draw;
			std::uint32_t vertexAddress[2], indexAddress[2], vertexLayout[2];
			std::uint32_t vertexStride, firstIndex;
		};
		static_assert(sizeof(SkyBlock) == 112, "Sky.hlsl's push block is 112 bytes");
		static_assert(offsetof(SkyBlock, vertexAddress) == 80, "the PUSH struct starts on a 16-byte row");

		/** @brief Which of a run's draws a pass records. */
		enum class Part : std::uint8_t
		{
			Main,        // every draw, on the GBuffer targets with the depth target
			FaceSky,     // a face's draws that write no occlusion, with the face's depth target
			FaceClouds,  // a face's cloud draws that write Cloud Shadows' occlusion, without a depth target (as Cloud Shadows binds them)
		};

		bool InPart(Part a_part, const Draw& a_draw)
		{
			return a_part == Part::Main || (a_part == Part::FaceClouds) == a_draw.key.occlusion;
		}

		/** @brief A client's state, which its passes share. */
		struct Client
		{
			View view = View::Main;
			Shape shape;
			std::string id;
			FrameAnchors::Anchor anchor = FrameAnchors::Anchor::Sky;
			std::shared_ptr<org::LatchBlock> latch;
			// A draw's vertex count, opened by the commit for its run only: a recording replayed for a run that did not commit draws nothing.
			std::shared_ptr<::RenderGraph::RunGate> gate;
			// The targets as imported at the registration (FrameAnchors keeps each while the engine's texture stays the same).
			std::array<std::shared_ptr<org::ExternalTextureResource>, 3> colours;
			std::shared_ptr<org::ExternalTextureResource> cubemap, depth, sceneDepth;
			std::array<std::shared_ptr<org::ExternalTextureResource>, kHeaderRegisters.size()> headers;  // imported D3D11 views (Shape::headerViews)
			std::shared_ptr<org::Resource> cloudFaces;
			// Render thread writes, the host's thread reads.
			std::shared_ptr<const Run> run;
			std::atomic<bool> prepared{ false };
			bool pending = false;
		};

		struct Bindings
		{
			SharedConstants::Bindings shared;
			std::array<org::DeclaredViewToken, 3> colours{};
			std::array<org::DeclaredViewToken, kFaces> faces{};
			std::array<org::DeclaredViewToken, kFaces> occlusion{};
			org::DeclaredViewToken depth{}, sceneDepth{}, cloudShadows{};
			std::array<org::DeclaredViewToken, kHeaderRegisters.size()> headers{};
			bool hasDepth = false, hasSceneDepth = false, hasCloudShadows = false;
			std::array<bool, kHeaderRegisters.size()> hasHeaders{};
		};

		struct Frame
		{
			std::shared_ptr<const Run> run;
			std::shared_ptr<const org::LatchBlock> latch;
			std::shared_ptr<const ::RenderGraph::RunGate> gate;
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
				if (part == Part::Main) {
					for (std::size_t i = 0; i < c.colours.size(); ++i)
						bindings.colours[i] = a_builder.RenderTarget(c.colours[i]).View();
				} else {
					for (std::uint32_t f = 0; f < kFaces; ++f)
						bindings.faces[f] = a_builder.RenderTarget(c.cubemap, org::RtvView{ UINT32_MAX, 0, f }).View();
				}
				if (part == Part::FaceClouds) {
					for (std::uint32_t f = 0; f < kFaces; ++f)
						bindings.occlusion[f] = a_builder.RenderTarget(c.cloudFaces, org::RtvView{ UINT32_MAX, 0, f }).View();
					// The face's depth is read (t17) instead of tested, as Cloud Shadows binds it for these draws.
					bindings.sceneDepth = a_builder.ShaderResource(c.depth).View();
					bindings.hasSceneDepth = true;
				} else {
					bindings.depth = a_builder.DepthReadWrite(c.depth).View();
					bindings.hasDepth = true;
					if (part == Part::Main && (c.sceneDepth || !c.shape.sceneDepthId.empty())) {
						bindings.sceneDepth = c.sceneDepth ? a_builder.ShaderResource(c.sceneDepth).View() :
						                                     a_builder.ShaderResource(org::ResourceIdentifier(c.shape.sceneDepthId)).View();
						bindings.hasSceneDepth = true;
					}
				}
				for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
					if (c.headers[h]) {
						bindings.headers[h] = a_builder.ShaderResource(c.headers[h]).View();
						bindings.hasHeaders[h] = true;
					} else if (!c.shape.headerIds[h].empty()) {
						bindings.headers[h] = a_builder.ShaderResource(org::ResourceIdentifier(c.shape.headerIds[h])).View();
						bindings.hasHeaders[h] = true;
					}
				}
				if (c.shape.cloudShadows) {
					bindings.cloudShadows = a_builder.ShaderResource(org::ResourceIdentifier(std::string(ORGCloudShadows::kOcclusionId))).View();
					bindings.hasCloudShadows = true;
				}
				return bindings;
			}

			// The run's draws (their programs, buffers, textures, counts and face), as the render thread published it before the run.
			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				const auto run = std::atomic_load(&client->run);
				a_out.push_back(static_cast<std::uint64_t>(part));
				a_out.push_back(run ? run->revision : 0);
			}

			Frame Prepare(const Bindings&, const org::PassPrepareContext&) const
			{
				client->prepared.store(true, std::memory_order_release);
				return { std::atomic_load(&client->run), client->latch, client->gate, part };
			}

			static void Record(const Bindings& a_bindings, const Frame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.run || !a_frame.latch || !a_frame.gate)
					return;
				const auto& run = *a_frame.run;
				const Part part = a_frame.part;
				const auto shared = SharedConstants::Index(a_recording, a_bindings.shared);
				const std::uint32_t nullIndex = GpuTextures::Get().NullIndex();
				const std::uint32_t sceneDepth = a_bindings.hasSceneDepth ? a_recording.Index(a_bindings.sceneDepth) : nullIndex;
				const std::uint32_t cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : nullIndex;
				const std::uint32_t face = run.face < kFaces ? run.face : 0;
				std::array<ShaderBindings::Handle, kHeaderRegisters.size()> headers{};
				for (std::size_t h = 0; h < headers.size(); ++h)
					headers[h] = a_bindings.hasHeaders[h] ? a_recording.Index(a_bindings.headers[h]) : nullIndex;
				bool occlusionCleared = false;
				for (std::uint32_t i = 0; i < run.draws.size(); ++i) {
					const auto& draw = run.draws[i];
					if (!InPart(part, draw) || !draw.program)
						continue;
					const auto& program = *static_cast<const RasterProgram*>(draw.program.get());
					SkyBlock block{};
					block.shared = shared;
					block.base = draw.textures[0];
					block.blend = draw.textures[1];
					block.noise = draw.textures[2];
					block.depth = sceneDepth;
					block.sharedDepth = sceneDepth;
					block.cloudShadows = cloudShadows;
					block.cloudSelfShadow = cloudShadows;
					block.headers = headers;
					block.baseSampler = draw.samplers[0];
					block.blendSampler = draw.samplers[1];
					block.noiseSampler = draw.samplers[2];
					block.draw = ShaderBindings::LatchOf(*a_frame.latch, a_recording.FrameSlot(), i * static_cast<std::uint32_t>(sizeof(DrawConstants)));
					block.vertexAddress[0] = static_cast<std::uint32_t>(draw.vertexAddress);
					block.vertexAddress[1] = static_cast<std::uint32_t>(draw.vertexAddress >> 32);
					block.indexAddress[0] = static_cast<std::uint32_t>(draw.indexAddress);
					block.indexAddress[1] = static_cast<std::uint32_t>(draw.indexAddress >> 32);
					block.vertexLayout[0] = static_cast<std::uint32_t>(draw.vertexLayout);
					block.vertexLayout[1] = static_cast<std::uint32_t>(draw.vertexLayout >> 32);
					block.vertexStride = draw.vertexStride;
					block.firstIndex = draw.firstIndex;

					std::array<RasterProgram::Colour, 3> colours{};
					std::uint32_t colourCount = 0;
					if (part == Part::Main) {
						for (const auto& token : a_bindings.colours)
							colours[colourCount++] = { .view = &token };
					} else {
						colours[colourCount++] = { .view = &a_bindings.faces[face] };
						if (part == Part::FaceClouds) {
							// The face's first cloud draw starts its occlusion, as CheckResourcesSide clears Cloud Shadows' first layer.
							colours[colourCount++] = { .view = &a_bindings.occlusion[face], .load = occlusionCleared ? rhi::LoadOp::Load : rhi::LoadOp::Clear };
							occlusionCleared = true;
						}
					}
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					program.Draw(a_recording, { .colours = std::span<const RasterProgram::Colour>(colours.data(), colourCount),
												  .depth = a_bindings.hasDepth ? &depth : nullptr,
												  .viewport = { .x = draw.viewport[0], .y = draw.viewport[1], .width = draw.viewport[2], .height = draw.viewport[3],
													  .minDepth = draw.depthRange[0], .maxDepth = draw.depthRange[1] },
												  .constants = ShaderBindings::Data(block),
												  .debugName = "Sky",
												  // The count comes from the draw's gate entry, which the client's commit opens for this run only.
												  .indirect = &a_frame.gate->Latch(),
												  .indirectOffset = a_frame.gate->Offset(i) });
				}
			}

		private:
			std::shared_ptr<Client> client;
			Part part;
		};

		struct ProgramEntry
		{
			PipelineKey key;
			std::shared_ptr<const RasterProgram> program;
		};

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, static_cast<std::size_t>(View::Count)> clients;
			// The last shape a view's client could not be made for: not tried again until the shape changes.
			std::array<std::optional<Shape>, static_cast<std::size_t>(View::Count)> refused;
			std::vector<ProgramEntry> programs;
			std::vector<PipelineKey> failedPrograms;
			bool reserved = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			std::deque<std::pair<std::uint32_t, std::vector<std::shared_ptr<const void>>>> kept;  // (game frame, owners)
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		std::uint32_t GameFrame()
		{
			static REL::Relocation<std::uint32_t*> counter{ REL::Offset(Engine::Address::kFrameCounter) };
			return *counter;
		}

		/**
		 * @brief Stops the family for good: every batch is the engine's again. Cloud Shadows' copy reads a face from its D3D11 layer again
		 * once the engine draws that face's clouds (ORGCloudShadows::NoteFace).
		 */
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
			client->id = a_shape.view == View::Main ? "cs.sky.main" : "cs.sky.faces";
			client->anchor = a_shape.view == View::Main ? FrameAnchors::Anchor::Sky : FrameAnchors::Anchor::SkyReflections;
			client->latch = std::make_shared<org::LatchBlock>(client->id + ".latch", kMaxDraws * static_cast<std::uint32_t>(sizeof(DrawConstants)), host->FrameSlots());
			client->gate = ::RenderGraph::RunGate::Create(client->id + ".gate", kMaxDraws);
			if (!client->gate) {
				logger::warn("[DCLF] sky: the {} run gate could not be made; the sky stays the engine's", client->id);
				return nullptr;
			}
			if (a_shape.view == View::Main) {
				for (std::size_t i = 0; i < client->colours.size(); ++i) {
					client->colours[i] = a_shape.colours[i] >= 0 ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_shape.colours[i])) : nullptr;
					if (!client->colours[i]) {
						logger::warn("[DCLF] sky: the main pass's target {} ({}) cannot be imported; the sky stays the engine's", i, a_shape.colours[i]);
						return nullptr;
					}
				}
				if (a_shape.sceneDepth >= 0)
					client->sceneDepth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_shape.sceneDepth));
				if (auto* view = static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(a_shape.sceneDepthView))) {
					winrt::com_ptr<ID3D11Resource> resource;
					view->GetResource(resource.put());
					const auto texture = resource.try_as<ID3D11Texture2D>();
					client->sceneDepth = texture ? FrameAnchors::D3D11Texture("Sky::SceneDepth", texture.get(), view, nullptr) : nullptr;
				}
				if ((a_shape.sceneDepth >= 0 || a_shape.sceneDepthView) && !client->sceneDepth) {
					logger::warn("[DCLF] sky: the scene depth CS binds at t17 cannot be imported; the sky stays the engine's");
					return nullptr;
				}
			} else {
				client->cubemap = FrameAnchors::EngineCubemap(static_cast<std::uint32_t>(RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS));
				if (!client->cubemap) {
					logger::warn("[DCLF] sky: the reflection cubemap cannot be imported; the faces' sky stays the engine's");
					return nullptr;
				}
			}
			client->depth = a_shape.depth >= 0 ? FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_shape.depth)) : nullptr;
			if (!client->depth) {
				logger::warn("[DCLF] sky: the depth target {} cannot be imported; the sky stays the engine's", a_shape.depth);
				return nullptr;
			}
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				auto* view = static_cast<ID3D11ShaderResourceView*>(const_cast<void*>(a_shape.headerViews[h]));
				if (!view)
					continue;
				winrt::com_ptr<ID3D11Resource> resource;
				view->GetResource(resource.put());
				const auto texture = resource.try_as<ID3D11Texture2D>();
				client->headers[h] = texture ? FrameAnchors::D3D11Texture(fmt::format("Sky::t{}", kHeaderRegisters[h]), texture.get(), view, nullptr) : nullptr;
				if (!client->headers[h]) {
					logger::warn("[DCLF] sky: the texture CS binds at t{} cannot be imported; the sky stays the engine's", kHeaderRegisters[h]);
					return nullptr;
				}
			}
			// The faces' cloud draws write Cloud Shadows' occlusion into its graph faces (ORGCloudShadows::GraphFaces).
			const bool faceClouds = a_shape.view == View::Face && a_shape.cloudShadows && a_shape.clouds;
			if (faceClouds) {
				auto* faces = ORGCloudShadows::Get().GraphFaces();
				client->cloudFaces = faces ? faces->Graph() : nullptr;
				if (!client->cloudFaces) {
					logger::warn("[DCLF] sky: Cloud Shadows has no graph faces; the faces' sky stays the engine's");
					return nullptr;
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
			if (a_shape.view == View::Main) {
				addPass("draws", Part::Main, 0);
			} else {
				addPass("draws", Part::FaceSky, 0);
				// A weather without cloud layers clears Cloud Shadows' D3D11 last layer, and its copy then reads every face from it.
				if (faceClouds)
					addPass("clouds", Part::FaceClouds, 1);
			}
			if (faceClouds)
				desc.registerResources = [](org::RenderGraph& a_graph) {
					if (auto* faces = ORGCloudShadows::Get().GraphFaces())
						faces->Register(a_graph);
				};
			desc.commit = [client](const FrameAnchors::Point&) {
				const auto run = std::atomic_load(&client->run);
				auto* host = RenderGraphRuntime::Get().Host();
				if (!run || !host)
					return;
				const std::uint32_t slot = host->CurrentFrameSlot();
				for (std::uint32_t i = 0; i < run->draws.size(); ++i) {
					const auto& draw = run->draws[i];
					client->latch->WriteValue(slot, i * static_cast<std::uint32_t>(sizeof(DrawConstants)), draw.constants);
					if (draw.program)
						client->gate->OpenDraw(i, draw.indexCount, 1);
				}
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the sky's passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] sky: the {} client could not be registered; the sky stays the engine's", client->id);
				return nullptr;
			}
			logger::info("[DCLF] sky: the {} client registered (depth {}, scene depth {}{}{}, cloud shadows {}, cloud layers {})", client->id, a_shape.depth,
				a_shape.sceneDepth, a_shape.sceneDepthId.empty() ? "" : " " + a_shape.sceneDepthId, a_shape.sceneDepthView ? " (a D3D11 view)" : "",
				a_shape.cloudShadows ? "on" : "off", a_shape.clouds ? "the graph's" : "the engine's");
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched constants. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.face);
			mix(a_run.draws.size());
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix(draw.key.occlusion);
				mix(draw.vertexAddress);
				mix(draw.indexAddress);
				mix(draw.vertexLayout);
				mix((std::uint64_t(draw.vertexStride) << 32) | draw.firstIndex);
				mix(draw.indexCount);
				for (std::size_t t = 0; t < 3; ++t)
					mix((std::uint64_t(draw.textures[t]) << 32) | draw.samplers[t]);
				for (const auto v : draw.viewport)
					mix(v);
				std::uint32_t range[2];
				std::memcpy(range, draw.depthRange, sizeof(range));
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
	}

	bool Ready(const Shape& a_shape)
	{
		auto& state = GetState();
		if (state.failed || a_shape.view >= View::Count)
			return false;
		auto& client = state.clients[static_cast<std::size_t>(a_shape.view)];
		if (client && client->shape == a_shape)
			return client->prepared.load(std::memory_order_acquire);
		auto& refused = state.refused[static_cast<std::size_t>(a_shape.view)];
		if (refused && *refused == a_shape)
			return false;
		if (client)
			FrameAnchors::Unregister(client->id);
		client = MakeClient(a_shape);
		if (client)
			refused.reset();
		else
			refused = a_shape;
		return false;
	}

	std::shared_ptr<const void> Program(const PipelineKey& a_key, const void* a_shader)
	{
		auto& state = GetState();
		for (const auto& entry : state.programs)
			if (entry.key == a_key)
				return entry.program;
		for (const auto& failed : state.failedPrograms)
			if (failed == a_key)
				return nullptr;
		auto* host = RenderGraphRuntime::Get().Host();
		const auto* shader = static_cast<const RE::BSShader*>(a_shader);
		auto* blend = static_cast<ID3D11BlendState*>(const_cast<void*>(a_key.blend));
		auto* depthStencil = static_cast<ID3D11DepthStencilState*>(const_cast<void*>(a_key.depthStencil));
		auto* raster = static_cast<ID3D11RasterizerState*>(const_cast<void*>(a_key.raster));
		if (!host || !shader || !blend || !raster || (!a_key.occlusion && !depthStencil)) {
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}

		RasterProgram::Desc desc{};
		desc.pixelSource = kShader;
		desc.vertexSource = kShader;
		// Sky.hlsl is an FXC-era source (vector ?:).
		desc.languageVersion = L"2018";
		desc.constantWords = ShaderBindings::Words<SkyBlock>();
		// The pixel descriptor's defines serve both stages: the vertex stage's descriptor is the same technique, without DEFERRED, which
		// it does not read. Each stage's VSHADER or PSHADER is RasterProgram's (commonDefines).
		for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(*shader, SIE::ShaderClass::Pixel, a_key.pixelDescriptor)) {
			if (name == "VSHADER" || name == "PSHADER")
				continue;
			desc.defines.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
		}
		if (a_key.view == View::Face)
			desc.defines.emplace_back(L"SKY_ORG_FACES", L"");

		// The engine's state objects for the draw, as SetDirtyStates would bind them. GetDesc answers on a tripwire handle too.
		D3D11_BLEND_DESC blendDesc{};
		blend->GetDesc(&blendDesc);
		rhi::BlendState blendState{};
		std::string error;
		if (!::RenderGraph::Engine::Pipeline::BlendFromD3D11(blendDesc, blendState, error) || ::RenderGraph::Engine::Pipeline::UsesDualSource(blendDesc)) {
			logger::warn("[DCLF] sky: a draw's blend state has no ORG form ({}); its technique stays the engine's", error);
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}
		for (std::size_t i = 0; i < a_key.colourFormats.size() && a_key.colourFormats[i]; ++i)
			desc.targets.push_back({ .format = static_cast<rhi::Format>(a_key.colourFormats[i]), .blend = blendState.attachments[i] });
		desc.alphaToCoverage = blendState.alphaToCoverage;
		if (!a_key.occlusion) {
			D3D11_DEPTH_STENCIL_DESC depthDesc{};
			depthStencil->GetDesc(&depthDesc);
			desc.depth = ::RenderGraph::Engine::Pipeline::DepthStencilFromD3D11(depthDesc);
			desc.depthFormat = static_cast<rhi::Format>(a_key.depthFormat);
		}
		D3D11_RASTERIZER_DESC rasterDesc{};
		raster->GetDesc(&rasterDesc);
		desc.raster = ::RenderGraph::Engine::Pipeline::RasterFromD3D11(rasterDesc);

		auto program = RasterProgram::Load(host->Device(), desc);
		if (!program) {
			logger::warn("[DCLF] sky: the program for technique {:#x} (pixel descriptor {:#x}, {}) could not be built; it stays the engine's",
				a_key.pixelDescriptor & 0xFF, a_key.pixelDescriptor, a_key.view == View::Main ? "main" : "faces");
			state.failedPrograms.push_back(a_key);
			return nullptr;
		}
		logger::info("[DCLF] sky: built the program for technique {} (pixel descriptor {:#x}, {}{})", a_key.pixelDescriptor & 0xFF, a_key.pixelDescriptor,
			a_key.view == View::Main ? "main" : "faces", a_key.occlusion ? ", cloud occlusion" : "");
		state.programs.push_back({ a_key, program });
		return program;
	}

	void Keep(std::vector<std::shared_ptr<const void>> a_owners)
	{
		auto& state = GetState();
		if (a_owners.empty())
			return;
		const std::uint32_t frame = GameFrame();
		while (!state.kept.empty() && frame - state.kept.front().first > kKeptFrames)
			state.kept.pop_front();
		state.kept.emplace_back(frame, std::move(a_owners));
	}

	void Submit(std::shared_ptr<Run> a_run)
	{
		auto& state = GetState();
		if (!a_run || state.failed)
			return;
		const auto& client = state.clients[static_cast<std::size_t>(a_run->view)];
		if (!client)
			return;
		a_run->revision = RevisionOf(*a_run);
		const std::uint32_t frame = GameFrame();
		while (!state.kept.empty() && frame - state.kept.front().first > kKeptFrames)
			state.kept.pop_front();
		state.kept.emplace_back(frame, std::move(a_run->owners));
		++state.runs;
		state.draws += a_run->draws.size();
		std::atomic_store(&client->run, std::shared_ptr<const Run>(std::move(a_run)));
		client->pending = true;
		FrameAnchors::Run(client->anchor);
		client->pending = false;
	}

	std::string Status()
	{
		const auto& state = GetState();
		if (!Enabled())
			return "off (CS_ORG_SKY)";
		if (state.failed)
			return "failed: the engine draws the sky";
		return fmt::format("{} runs, {} draws, {} programs", state.runs, state.draws, state.programs.size());
	}
}
