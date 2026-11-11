// The sky family's render graph side: a client per view at the Sky and SkyReflections anchors, made once, whose passes draw a batch's
// latched draws in the engine's order with CS's Sky.hlsl built for ORG, and the faces' cloud draws into Cloud Shadows' layers.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Sky/Sky.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"

#include "Features/CloudShadows/ORGCloudShadows.h"
#include "Globals.h"
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

#include <atomic>
#include <cstddef>
#include <cstring>

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
		};

		bool InPart(Part a_part, const Draw& a_draw)
		{
			return a_part == Part::Main || (a_part == Part::FaceClouds) == a_draw.occlusion;
		}

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
			// Cloud Shadows' layers, which the faces' cloud draws write (ORGCloudShadows::ImportLayer).
			std::array<std::shared_ptr<org::ExternalTextureResource>, kCloudLayers> layers;
			// Null views of the dimensions the push block's handles declare, for an input the batch does not have.
			std::uint32_t null2D = GpuTextures::kInvalid, nullCube = GpuTextures::kInvalid;
			Family::Owners nullOwners;
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
			std::array<org::DeclaredViewToken, kHeaderRegisters.size()> headers{};
			bool hasDepth = false, hasSceneDepth = false, hasCloudShadows = false, hasLayers = false;
			std::array<bool, kHeaderRegisters.size()> hasHeaders{};
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
					if (part == Part::Main && (c.sceneDepth || !c.shape.sceneDepthId.empty())) {
						bindings.sceneDepth = c.sceneDepth ? a_builder.ShaderResource(c.sceneDepth).View() :
						                                     a_builder.ShaderResource(org::ResourceIdentifier(c.shape.sceneDepthId)).View();
						bindings.hasSceneDepth = true;
					}
				}
				for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
					if (c.shape.headerIds[h].empty())
						continue;
					bindings.headers[h] = a_builder.ShaderResource(org::ResourceIdentifier(c.shape.headerIds[h])).View();
					bindings.hasHeaders[h] = true;
				}
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
				SkyBlock block{};
				block.shared = SharedConstants::Index(a_recording, a_bindings.shared);
				block.depth = a_bindings.hasSceneDepth                       ? a_recording.Index(a_bindings.sceneDepth) :
				              run.sceneDepthIndex != GpuTextures::kInvalid ? run.sceneDepthIndex :
				                                                             c.null2D;
				block.sharedDepth = block.depth;
				block.cloudShadows = a_bindings.hasCloudShadows ? a_recording.Index(a_bindings.cloudShadows) : c.nullCube;
				for (std::size_t h = 0; h < block.headers.size(); ++h)
					block.headers[h] = a_bindings.hasHeaders[h] ? a_recording.Index(a_bindings.headers[h]) : run.headerIndices[h];
				const std::uint32_t face = run.face < kFaces ? run.face : 0;
				const std::uint32_t slot = a_recording.FrameSlot();

				const auto draw = [&](const RasterProgram& a_program, std::uint32_t a_index, std::span<const RasterProgram::Colour> a_colours) {
					const auto& latched = run.draws[a_index];
					block.draw = ShaderBindings::LatchOf(*c.latch, slot, a_index * kDrawStride);
					const RasterProgram::Depth depth{ .view = &a_bindings.depth, .readOnly = false };
					a_program.Draw(a_recording, { .colours = a_colours,
													.depth = a_bindings.hasDepth ? &depth : nullptr,
													.viewport = { .x = latched.viewport.rect[0], .y = latched.viewport.rect[1], .width = latched.viewport.rect[2],
														.height = latched.viewport.rect[3], .minDepth = latched.viewport.depthRange[0], .maxDepth = latched.viewport.depthRange[1] },
													.constants = ShaderBindings::Data(block),
													.debugName = "Sky",
													// The count comes from the draw's gate entry, which the client's commit opens for this run only.
													.indirect = &c.gate->Latch(),
													.indirectOffset = c.gate->Offset(a_index) });
				};

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

		/** @brief Everything here: the render thread's. */
		struct DrawsState
		{
			std::array<std::shared_ptr<Client>, kViews> clients;
			std::array<bool, kViews> refused{};  // the view's client could not be made: its batches stay the engine's
			Family::EngineDrawPrograms programs{ "sky", { .source = kShader, .languageVersion = L"2018", .constantWords = ShaderBindings::Words<SkyBlock>() } };
			bool reserved = false;
			bool failed = false;
			std::uint64_t runs = 0, draws = 0;
			Family::KeptOwners kept;
		};

		DrawsState& GetState()
		{
			static DrawsState state;
			return state;
		}

		/** @brief Runs that were not submitted after their draws were withheld, logged once per status. */
		Util::ReasonLog& LostRuns()
		{
			static Util::ReasonLog log("[DCLF] sky: a withheld batch was not drawn this frame (the view probes again)", spdlog::level::warn);
			return log;
		}

		const char* NameOf(FrameAnchors::RunStatus a_status)
		{
			switch (a_status) {
			case FrameAnchors::RunStatus::Idle:
				return "the anchor had nothing to run";
			case FrameAnchors::RunStatus::Refused:
				return "the anchor refused the run (not the render owner, or no runtime)";
			case FrameAnchors::RunStatus::RingFull:
				return "the anchor's ring was full";
			case FrameAnchors::RunStatus::Failed:
				return "the anchor's epoch failed";
			default:
				return "ran";
			}
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
			client->id = a_shape.view == View::Main ? "cs.sky.main" : "cs.sky.faces";
			client->anchor = a_shape.view == View::Main ? FrameAnchors::Anchor::Sky : FrameAnchors::Anchor::SkyReflections;
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
			if (a_shape.view == View::Main) {
				for (std::size_t i = 0; i < client->colours.size(); ++i) {
					client->colours[i] = a_shape.colours[i] >= 0 ? FrameAnchors::EngineTarget(static_cast<std::uint32_t>(a_shape.colours[i])) : nullptr;
					if (!client->colours[i]) {
						logger::warn("[DCLF] sky: the main pass's target {} ({}) cannot be imported; the sky stays the engine's", i, a_shape.colours[i]);
						return nullptr;
					}
				}
				if (a_shape.sceneDepth >= 0) {
					client->sceneDepth = FrameAnchors::EngineDepth(static_cast<std::uint32_t>(a_shape.sceneDepth));
					if (!client->sceneDepth) {
						logger::warn("[DCLF] sky: the scene depth CS binds at t17 cannot be imported; the sky stays the engine's");
						return nullptr;
					}
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
			if (a_shape.view == View::Main) {
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
			};
			desc.executed = [client](const FrameAnchors::Point&) {
				if (client->executed.fetch_add(1, std::memory_order_relaxed) == 0)
					logger::info("[DCLF] sky: the first {} run drawn by the graph", client->id);
			};
			desc.active = [client] { return client->pending; };
			desc.onFailed = [] { Fail("the render graph failed with the sky's passes in it"); };
			if (!FrameAnchors::Register(std::move(desc))) {
				logger::warn("[DCLF] sky: the {} client could not be registered; the sky stays the engine's", client->id);
				return nullptr;
			}
			std::string published;
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h)
				if (!a_shape.headerIds[h].empty())
					published += fmt::format(", t{} {}", kHeaderRegisters[h], a_shape.headerIds[h]);
			logger::info("[DCLF] sky: the {} client registered (depth {}, scene depth {}{}, cloud shadows {}, cloud layers {}{})", client->id, a_shape.depth,
				a_shape.sceneDepth, a_shape.sceneDepthId.empty() ? "" : " " + a_shape.sceneDepthId, a_shape.cloudShadows ? "on" : "off",
				a_shape.clouds ? "the graph's" : "the engine's", published);
			return client;
		}

		/** @brief What a run's recording depends on: everything of its draws but the latched values. */
		std::uint64_t RevisionOf(const Run& a_run)
		{
			std::uint64_t hash = 1469598103934665603ull;
			const auto mix = [&](std::uint64_t a_value) { hash = (hash ^ a_value) * 1099511628211ull; };
			mix(a_run.face);
			mix(a_run.draws.size());
			mix(a_run.sceneDepthIndex);
			for (const auto index : a_run.headerIndices)
				mix(index);
			for (const auto& draw : a_run.draws) {
				mix(reinterpret_cast<std::uintptr_t>(draw.program.get()));
				mix(reinterpret_cast<std::uintptr_t>(draw.occlusionProgram.get()));
				mix((std::uint64_t(draw.occlusion) << 32) | static_cast<std::uint32_t>(draw.layer));
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
	}

	bool Ready(const Shape& a_shape)
	{
		auto& state = GetState();
		if (state.failed || a_shape.view >= View::Count)
			return false;
		const auto index = static_cast<std::size_t>(a_shape.view);
		auto& client = state.clients[index];
		if (client) {
			if (client->shape == a_shape)
				return FrameAnchors::RequestJoin(client->id, client->anchor);
			// No client is registered again during play (a graph rebuild on the frame's path): the batch stays the engine's.
			if (!std::exchange(client->shapeLogged, true))
				logger::warn("[DCLF] sky: the {} batch's targets or published inputs changed since its client registered; those batches stay the engine's",
					client->id);
			return false;
		}
		if (state.refused[index])
			return false;
		client = MakeClient(a_shape);
		state.refused[index] = !client;
		return false;
	}

	bool CanRun(View a_view)
	{
		const auto& client = GetState().clients[static_cast<std::size_t>(a_view)];
		return client && FrameAnchors::CanRun(client->id, client->anchor);
	}

	std::shared_ptr<const void> Program(const Family::ProgramKey& a_key, const RE::BSShader& a_shader, Family::ProgramState& a_state)
	{
		static const std::array<std::pair<std::wstring, std::wstring>, 1> kFacesDefines{ { { L"SKY_ORG_FACES", L"" } } };
		const auto defines = (a_key.variant & kVariantFaces) ? std::span<const std::pair<std::wstring, std::wstring>>(kFacesDefines) :
		                                                       std::span<const std::pair<std::wstring, std::wstring>>();
		return GetState().programs.Get(a_key, a_shader, defines, a_state);
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
		client->pending = true;
		const auto result = FrameAnchors::Run(client->anchor);
		client->pending = false;
		if (!result)
			LostRuns().Report(NameOf(result.status), client->id);
		return result;
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
		return fmt::format("{} runs ({} drawn), {} draws, {} programs", state.runs, executed, state.draws, state.programs.Built());
	}
}
