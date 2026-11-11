#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/FrameAnchorSelfTest.h"

#	include "Globals.h"
#	include "State.h"

#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/RasterProgram.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"
#	include "RenderGraph/UavClearPass.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <Resources/TextureDescription.h>

#	include <array>
#	include <bit>
#	include <cctype>
#	include <cstring>

namespace FrameAnchors::SelfTest
{
	namespace
	{
		void Check(Anchor a_anchor);

		constexpr const char* kShader = "RenderGraph/AnchorSelfTest.hlsl";
		constexpr const char* kClientId = "cs.org.anchor-selftest";
		// CS_ORG_ANCHOR_SELFTEST_FAIL: a second client whose pass fails to prepare at kFailAnchor. The main client leaves that anchor
		// out, so only the failing client should be dropped. Present runs every frame, in menus too.
		constexpr const char* kFailClientId = "cs.org.anchor-selftest-fail";
		constexpr auto kFailAnchor = Anchor::Present;
		// Runs of the main client before the test reports that the failing client was never dropped.
		constexpr std::uint32_t kFailPatience = 600;
		constexpr std::uint32_t kAnchors = static_cast<std::uint32_t>(Anchor::Count);
		constexpr std::uint32_t kTextureSize = 16;
		constexpr std::uint32_t kOutputWords = 64;  // word a: the check; word 32 + a: kMAIN's texel
		constexpr std::uint32_t kMainWord = 32;
		constexpr std::uint32_t kNoIndex = 0xFFFFFFFFu;
		// Runs without a value before the test reports that nothing came back.
		constexpr std::uint32_t kPatience = 240;
		static_assert(kAnchors <= kMainWord);

		// Must match SelfTestConstants in AnchorSelfTest.hlsl.
		struct SelfTestConstants
		{
			std::uint32_t value;
			std::uint32_t textureIndex;
			std::uint32_t outputIndex;
			std::uint32_t anchor;
			std::uint32_t mainIndex;
			std::uint32_t padding[3];
		};
		static_assert(sizeof(SelfTestConstants) == 32);
		constexpr std::uint32_t kConstantWords = sizeof(SelfTestConstants) / sizeof(std::uint32_t);

		/** @brief What the raster pass writes at an anchor; the compute pass stores it plus one. */
		constexpr std::uint32_t ValueOf(std::uint32_t a_anchor) { return 0x0A5C0000u | (a_anchor << 8); }

		struct Resources
		{
			std::shared_ptr<const RasterProgram> draw;
			std::shared_ptr<const ComputeProgram> read;
			std::array<std::shared_ptr<org::PixelBuffer>, kAnchors> textures;
			std::shared_ptr<org::Buffer> output;                 // uint[kOutputWords]
			std::shared_ptr<org::Buffer> readback;               // host copy of output
			std::shared_ptr<org::ExternalTextureResource> main;  // the engine's kMAIN, or null
			// CS_ORG_ANCHOR_SELFTEST_FAIL: a texture with a shader UAV and no CPU-side UAV, which a UAV clear needs. Its clear fails
			// to prepare, as the HDR output anchor's import once did.
			std::shared_ptr<org::PixelBuffer> failTexture;
		};

		struct Status
		{
			bool attempted = false;
			std::shared_ptr<Resources> resources;
			std::array<std::uint32_t, kAnchors> runs{};
			std::array<bool, kAnchors> reported{};
			std::uint32_t passed = 0;  // a bit per anchor
			// CS_ORG_ANCHOR_SELFTEST_FAIL: the failing client was registered, it was dropped, the main client ran after the drop, and the
			// main client's runs since the failing client was registered.
			bool failRegistered = false;
			bool failDropped = false;
			bool isolationReported = false;
			std::uint32_t runsSinceFail = 0;
		};

		Status& Test()
		{
			static Status s_status;
			return s_status;
		}

		bool FailRequested()
		{
			static const bool requested = Switches::On("CS_ORG_ANCHOR_SELFTEST_FAIL");
			return requested;
		}

		std::string Lowercase(std::string_view a_name)
		{
			std::string out(a_name);
			for (auto& c : out)
				c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
			return out;
		}

		// The raster pass: the anchor's value into its texture, through RasterProgram.
		struct DrawBindings
		{
			org::DeclaredViewToken target;
		};

		struct DrawFrame
		{
			std::shared_ptr<const RasterProgram> program;
			std::uint32_t anchor = 0;
		};

		class DrawPass final : public org::TypedRenderGraphPass<DrawPass, DrawFrame, DrawBindings>
		{
		public:
			DrawPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_anchor) :
				resources(std::move(a_resources)), anchor(a_anchor) {}

			DrawBindings Declare(org::PassBuilder& a_builder)
			{
				return { a_builder.RenderTarget(resources->textures[anchor]).View() };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->draw.get())); }

			DrawFrame Prepare(const DrawBindings&, const org::PassPrepareContext&) const { return { resources->draw, anchor }; }

			static void Record(const DrawBindings& a_bindings, const DrawFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program)
					return;
				SelfTestConstants constants{};
				constants.value = ValueOf(a_frame.anchor);
				const RasterProgram::Colour colour{ .view = &a_bindings.target, .load = rhi::LoadOp::Load };
				a_frame.program->Draw(a_recording, { .colours = { &colour, 1 },
													   .viewport = { .width = kTextureSize, .height = kTextureSize },
													   .constants = { reinterpret_cast<const std::uint32_t*>(&constants), kConstantWords },
													   .debugName = "CS anchor self-test draw" });
			}

		private:
			std::shared_ptr<Resources> resources;
			std::uint32_t anchor;
		};

		// The compute pass: the texel into the anchor's word (plus one), the texel cleared, kMAIN's texel beside it.
		struct ReadBindings
		{
			org::DeclaredViewToken texture, output, main;
			bool hasMain = false;
		};

		struct ReadFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			std::uint32_t anchor = 0;
		};

		class ReadPass final : public org::TypedRenderGraphPass<ReadPass, ReadFrame, ReadBindings>
		{
		public:
			ReadPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_anchor) :
				resources(std::move(a_resources)), anchor(a_anchor) {}

			ReadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ReadBindings bindings{};
				bindings.texture = a_builder.UnorderedAccess(resources->textures[anchor]).View();
				bindings.output = a_builder.UnorderedAccess(resources->output).View();
				if (resources->main) {
					bindings.main = a_builder.ShaderResource(resources->main).View();
					bindings.hasMain = true;
				}
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->read.get())); }

			ReadFrame Prepare(const ReadBindings&, const org::PassPrepareContext&) const { return { resources->read, anchor }; }

			static void Record(const ReadBindings& a_bindings, const ReadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program)
					return;
				SelfTestConstants constants{};
				constants.value = ValueOf(a_frame.anchor);
				constants.textureIndex = a_recording.Index(a_bindings.texture);
				constants.outputIndex = a_recording.Index(a_bindings.output);
				constants.anchor = a_frame.anchor;
				constants.mainIndex = a_bindings.hasMain ? a_recording.Index(a_bindings.main) : kNoIndex;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch(1, 1, 1);
			}

		private:
			std::shared_ptr<Resources> resources;
			std::uint32_t anchor;
		};

		// The copy pass: the anchor's two words into the host copy, which Check reads.
		struct ReadbackBindings
		{
			org::ResourceBindingToken output;
		};

		struct ReadbackFrame
		{
			std::shared_ptr<org::Buffer> readback;
			std::uint32_t anchor = 0;
		};

		class ReadbackPass final : public org::TypedRenderGraphPass<ReadbackPass, ReadbackFrame, ReadbackBindings>
		{
		public:
			ReadbackPass(std::shared_ptr<Resources> a_resources, std::uint32_t a_anchor) :
				resources(std::move(a_resources)), anchor(a_anchor) {}

			ReadbackBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopySource(resources->output) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->readback.get())); }

			ReadbackFrame Prepare(const ReadbackBindings&, const org::PassPrepareContext&) const { return { resources->readback, anchor }; }

			static void Record(const ReadbackBindings& a_bindings, const ReadbackFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.readback)
					return;
				auto& commands = a_recording.Commands();
				const auto destination = a_frame.readback->GetAPIResource().GetHandle();
				const auto source = a_recording.Resolve(a_bindings.output).GetHandle();
				for (const std::uint32_t word : { a_frame.anchor, kMainWord + a_frame.anchor })
					commands.CopyBufferRegion(destination, word * 4ull, source, word * 4ull, 4);
			}

		private:
			std::shared_ptr<Resources> resources;
			std::uint32_t anchor;
		};

		std::shared_ptr<Resources> Create(rhi::Device a_device)
		{
			auto resources = std::make_shared<Resources>();
			resources->draw = RasterProgram::Load(a_device, { .pixelSource = kShader,
																.pixelEntry = L"PSMain",
																.constantWords = kConstantWords,
																.targets = { { .format = rhi::Format::R32_UInt } } });
			resources->read = ComputeProgram::Load(a_device, { .source = kShader, .entry = L"CSMain", .constantWords = kConstantWords });
			if (!resources->draw || !resources->read)
				return nullptr;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				org::TextureDescription desc{};
				desc.imageDimensions = { { kTextureSize, kTextureSize, 0, 0 } };
				desc.channels = 1;
				desc.format = rhi::Format::R32_UInt;
				desc.hasRTV = true;
				desc.rtvFormat = desc.format;
				desc.hasUAV = true;
				desc.uavFormat = desc.format;
				resources->textures[i] = org::PixelBuffer::CreateShared(desc);
				resources->textures[i]->SetName(fmt::format("{}.texture.{}", kClientId, Lowercase(Name(static_cast<Anchor>(i)))));
			}
			resources->output = org::Buffer::CreateUnmaterializedStructuredBuffer(kOutputWords, sizeof(std::uint32_t), true);
			resources->output->SetName(fmt::format("{}.output", kClientId));
			resources->output->Materialize();
			resources->readback = org::Buffer::CreateShared(rhi::HeapType::Readback, kOutputWords * sizeof(std::uint32_t));
			resources->readback->SetName(fmt::format("{}.readback", kClientId));
			// Zero: a word still zero has not come back yet.
			auto readback = resources->readback->GetAPIResource();
			void* mapped = nullptr;
			readback.Map(&mapped);
			if (!mapped)
				return nullptr;
			std::memset(mapped, 0, kOutputWords * sizeof(std::uint32_t));
			readback.Unmap(0, kOutputWords * sizeof(std::uint32_t));
			// The engine target import: a missing kMAIN only leaves its texel out.
			resources->main = EngineTarget(static_cast<std::uint32_t>(RE::RENDER_TARGET::kMAIN));
			if (FailRequested()) {
				org::TextureDescription desc{};
				desc.imageDimensions = { { kTextureSize, kTextureSize, 0, 0 } };
				desc.channels = 1;
				desc.format = rhi::Format::R32_UInt;
				desc.hasUAV = true;
				desc.uavFormat = desc.format;
				// No CPU-side UAV on purpose: the clear below asks for one, and its preparation fails.
				desc.hasNonShaderVisibleUAV = false;
				resources->failTexture = org::PixelBuffer::CreateShared(desc);
				resources->failTexture->SetName(fmt::format("{}.texture", kFailClientId));
			}
			return resources;
		}

		ClientDesc MakeClient(const std::shared_ptr<Resources>& a_resources)
		{
			ClientDesc client{};
			client.id = kClientId;
			for (std::uint32_t i = 0; i < kAnchors; ++i) {
				const auto anchor = static_cast<Anchor>(i);
				// With the failing client, its anchor is left to it: the main client must not be dropped with it.
				if (!HasEpoch(anchor) || (FailRequested() && anchor == kFailAnchor))
					continue;
				const std::string prefix = fmt::format("{}.{}", kClientId, Lowercase(Name(anchor)));
				client.passes.push_back({ .name = prefix + ".draw", .anchor = anchor, .order = 0, .kind = PassKind::Render, .make = [a_resources, i] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<DrawPass>(a_resources, i)); } });
				client.passes.push_back({ .name = prefix + ".read", .anchor = anchor, .order = 1, .kind = PassKind::Compute, .make = [a_resources, i] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ReadPass>(a_resources, i)); } });
				client.passes.push_back({ .name = prefix + ".readback", .anchor = anchor, .order = 2, .kind = PassKind::Copy, .make = [a_resources, i] { return std::static_pointer_cast<org::RenderPass>(std::make_shared<ReadbackPass>(a_resources, i)); } });
			}
			client.registerResources = [a_resources](org::RenderGraph& a_graph) {
				for (std::uint32_t i = 0; i < kAnchors; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(a_resources->textures[i]->GetName()), a_resources->textures[i]);
				a_graph.RegisterResource(org::ResourceIdentifier(a_resources->output->GetName()), a_resources->output);
			};
			client.onFailed = [] {
				Test().resources.reset();
				if (FailRequested())
					logger::error("[ORG] anchor self-test: isolation FAILED, the main client was dropped (the render graph failed, or the failing client's "
								  "failure reached it)");
				else
					logger::error("[ORG] anchor self-test: stopped, the render graph failed");
			};
			client.executed = [](const Point& a_point) { Check(a_point.anchor); };
			return client;
		}

		/** @brief CS_ORG_ANCHOR_SELFTEST_FAIL's client: one UAV clear at kFailAnchor, which fails to prepare. */
		ClientDesc MakeFailClient(const std::shared_ptr<Resources>& a_resources)
		{
			ClientDesc client{};
			client.id = kFailClientId;
			const auto texture = a_resources->failTexture;
			client.passes.push_back({ .name = fmt::format("{}.clear", kFailClientId), .anchor = kFailAnchor, .order = 0, .kind = PassKind::Compute,
				.make = [texture] { return GraphPasses::MakeUavClearUint(texture, { 0, 0, 0, 0 }); } });
			client.registerResources = [texture](org::RenderGraph& a_graph) {
				a_graph.RegisterResource(org::ResourceIdentifier(texture->GetName()), texture);
			};
			client.onFailed = [] {
				Test().failDropped = true;
				logger::info("[ORG] anchor self-test: the failing client was dropped as expected (CS_ORG_ANCHOR_SELFTEST_FAIL); the other anchors "
							 "must keep running");
			};
			return client;
		}
	}

	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_ANCHOR_SELFTEST");
		return requested;
	}

	void Reserve()
	{
		if (!Requested())
			return;
		for (std::uint32_t i = 0; i < kAnchors; ++i)
			FrameAnchors::Reserve(static_cast<Anchor>(i));
		logger::info("[ORG] anchor self-test: every anchor reserved (CS_ORG_ANCHOR_SELFTEST)");
	}

	void Setup()
	{
		auto& test = Test();
		if (!Requested() || std::exchange(test.attempted, true))
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		auto resources = Create(host->Device());
		if (!resources) {
			logger::error("[ORG] anchor self-test: its programs or buffers could not be created; off");
			return;
		}
		if (!Register(MakeClient(resources)))
			return;
		test.resources = std::move(resources);
		logger::info("[ORG] anchor self-test: on; each anchor logs once when its value comes back (kMAIN import {})",
			test.resources->main ? "made" : "unavailable");
		if (test.resources->failTexture) {
			test.failRegistered = Register(MakeFailClient(test.resources));
			logger::info("[ORG] anchor self-test: the failing client {} at {} (CS_ORG_ANCHOR_SELFTEST_FAIL); only it should be dropped",
				test.failRegistered ? "registered" : "could not be registered", Name(kFailAnchor));
		}
	}

	namespace
	{
		void Check(Anchor a_anchor)
		{
			auto& test = Test();
			const auto anchor = static_cast<std::uint32_t>(a_anchor);
			if (!test.resources || anchor >= kAnchors)
				return;
			// CS_ORG_ANCHOR_SELFTEST_FAIL: this anchor ran with the main client after the failing client was dropped.
			if (test.failRegistered && !test.isolationReported) {
				if (test.failDropped) {
					test.isolationReported = true;
					logger::info("[ORG] anchor self-test: isolation OK, {} ran with the main client after the failing client was dropped", Name(a_anchor));
				} else if (++test.runsSinceFail == kFailPatience) {
					test.isolationReported = true;
					logger::warn("[ORG] anchor self-test: the failing client was not dropped after {} runs of the main client", kFailPatience);
				}
			}
			if (test.reported[anchor])
				return;
			++test.runs[anchor];
			auto readback = test.resources->readback->GetAPIResource();
			void* mapped = nullptr;
			readback.Map(&mapped);
			if (!mapped)
				return;
			// The value is the same every run: a copy the GPU is still writing reads the same word.
			std::uint32_t words[kOutputWords]{};
			std::memcpy(words, mapped, sizeof(words));
			readback.Unmap(0, 0);
			const std::uint32_t expected = ValueOf(anchor) + 1;
			const std::uint32_t got = words[anchor];
			if (got == expected) {
				test.reported[anchor] = true;
				test.passed |= 1u << anchor;
				// The first value an anchor returns may come from the main menu (no scene in kMAIN yet, and the menu's frames may run off
				// the render thread): the line says so, so a 0 there is not read as a graph that sees no scene.
				logger::info("[ORG] anchor self-test: {} OK after {} runs (wrote 0x{:08X}, read back 0x{:08X}; kMAIN(0,0).r {}; game frame {}{})", Name(a_anchor),
					test.runs[anchor], ValueOf(anchor), got,
					test.resources->main ? fmt::format("{:.4f}", std::bit_cast<float>(words[kMainWord + anchor])) : std::string("not read"), globals::state->frameCount,
					globals::state->IsMainOrLoadingMenuOpen() ? ", main or loading menu open" : "");
				// With the failing client, the main client does not run at its anchor.
				const std::uint32_t wanted = ((1u << kAnchors) - 1) & ~(FailRequested() ? 1u << static_cast<std::uint32_t>(kFailAnchor) : 0u);
				if (test.passed == wanted)
					logger::info("[ORG] anchor self-test: every anchor OK");
			} else if (got != 0) {
				test.reported[anchor] = true;
				logger::error("[ORG] anchor self-test: {} MISMATCH: read back 0x{:08X}, expected 0x{:08X} ({})", Name(a_anchor), got, expected,
					got == 1 ? "the read ran before the draw" : "wrong value");
			} else if (test.runs[anchor] == kPatience) {
				test.reported[anchor] = true;
				logger::warn("[ORG] anchor self-test: {} read back nothing after {} runs", Name(a_anchor), kPatience);
			}
		}
	}
}

#endif
