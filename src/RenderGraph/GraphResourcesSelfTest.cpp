#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GraphResources.h"

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/Frame/BoundaryCheck.h"
#	include "RenderGraph/Frame/FrameDriver.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/VersionedTexture.h>

#	include <atomic>
#	include <chrono>
#	include <cstring>
#	include <future>
#	include <sstream>
#	include <thread>

namespace GraphResources::SelfTest
{
	namespace
	{
		void Check();
		void StartPrepared();

		constexpr const char* kShader = "RenderGraph/GraphResourcesSelfTest.hlsl";
		constexpr const char* kClientId = "cs.org.graphres-selftest";
		constexpr const char* kTextureName = "GraphResourcesSelfTest::Texture";
		// The texture's size, then the size it is resized to.
		constexpr UINT kSizes[] = { 16, 32 };
		constexpr UINT kSteps = static_cast<UINT>(std::size(kSizes));
		constexpr UINT kTexelX = 3;
		constexpr UINT kTexelY = 5;
		// Copies without the value before a step reports that nothing came back.
		constexpr std::uint32_t kPatience = 240;

		// Must match SelfTestConstants in GraphResourcesSelfTest.hlsl.
		struct SelfTestConstants
		{
			std::uint32_t value;
			std::uint32_t textureIndex;
			std::uint32_t width;
			std::uint32_t height;
		};
		constexpr std::uint32_t kConstantWords = sizeof(SelfTestConstants) / sizeof(std::uint32_t);

		constexpr std::uint32_t ValueOf(UINT a_width) { return 0x6A5E0000u | a_width; }

		struct Resources
		{
			std::shared_ptr<const ComputeProgram> write;
			std::unique_ptr<Texture2D> texture;
			winrt::com_ptr<ID3D11Texture2D> staging;  // D3D11 copies the export into it
		};

		struct Status
		{
			bool attempted = false;
			std::shared_ptr<Resources> resources;
			UINT step = 0;
			std::uint32_t copies = 0;  // within the step
			bool copyPending = false;  // a copy into staging not read yet
		};

		Status& Test()
		{
			static Status s_status;
			return s_status;
		}

		D3D11_TEXTURE2D_DESC TextureDesc(UINT a_size, bool a_staging)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_size;
			desc.Height = a_size;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_UINT;
			desc.SampleDesc.Count = 1;
			desc.Usage = a_staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
			desc.BindFlags = a_staging ? 0u : (D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
			desc.CPUAccessFlags = a_staging ? D3D11_CPU_ACCESS_READ : 0u;
			return desc;
		}

		bool CreateStaging(Resources& a_resources)
		{
			a_resources.staging = nullptr;
			const auto desc = TextureDesc(a_resources.texture->Desc().Width, true);
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_resources.staging.put())))
				return false;
			Util::SetResourceName(a_resources.staging.get(), "GraphResourcesSelfTest::Staging");
			return true;
		}

		// The compute pass writes ValueOf(width) into every texel of the exported texture.
		struct WriteBindings
		{
			org::DeclaredViewToken texture;
		};

		struct WriteFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			UINT width = 0;
			UINT height = 0;
		};

		class WritePass final : public org::TypedRenderGraphPass<WritePass, WriteFrame, WriteBindings>
		{
		public:
			// Read Graph() when the pass is made, on the render thread.
			explicit WritePass(const Resources& a_resources) :
				program(a_resources.write), texture(a_resources.texture->Graph()), width(a_resources.texture->Desc().Width),
				height(a_resources.texture->Desc().Height) {}

			WriteBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(texture).View() };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(program.get()));
				a_out.push_back(reinterpret_cast<std::uintptr_t>(texture.get()));
			}

			WriteFrame Prepare(const WriteBindings&, const org::PassPrepareContext&) const { return { program, width, height }; }

			static void Record(const WriteBindings& a_bindings, const WriteFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program)
					return;
				const SelfTestConstants constants{ ValueOf(a_frame.width), a_recording.Index(a_bindings.texture), a_frame.width, a_frame.height };
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch((a_frame.width + 7) / 8, (a_frame.height + 7) / 8, 1);
			}

		private:
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<org::Resource> texture;
			UINT width;
			UINT height;
		};

		FrameAnchors::ClientDesc MakeClient(const std::shared_ptr<Resources>& a_resources)
		{
			FrameAnchors::ClientDesc client{};
			client.id = kClientId;
			client.passes.push_back({ .name = std::string(kClientId) + ".write",
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [a_resources]() -> std::shared_ptr<org::RenderPass> {
					if (!*a_resources->texture)
						return nullptr;
					return std::make_shared<WritePass>(*a_resources);
				} });
			client.registerResources = [a_resources](org::RenderGraph& a_graph) { a_resources->texture->Register(a_graph); };
			client.executed = [](const FrameAnchors::Point&) { Check(); };
			return client;
		}

		/** @brief Resizes for the next step, or finishes and releases the texture. */
		void Advance(Status& a_test)
		{
			auto& resources = *a_test.resources;
			if (++a_test.step < kSteps) {
				const UINT size = kSizes[a_test.step];
				resources.texture->Resize(size, size);
				if (!*resources.texture || !CreateStaging(resources)) {
					logger::error("[ORG] graph resource self-test: the resized texture could not be created");
					FrameAnchors::Unregister(kClientId);
					a_test.resources.reset();
					return;
				}
				FrameAnchors::Refresh(kClientId);
				a_test.copies = 0;
				logger::info("[ORG] graph resource self-test: resized to {}x{}; the first texture goes once D3D11 is past its last copy", size, size);
				return;
			}
			logger::info("[ORG] graph resource self-test: every check OK; the texture is released");
			FrameAnchors::Unregister(kClientId);
			a_test.resources.reset();
			// Registering changes graphs, so the next part starts at a frame boundary.
			RenderGraph::Frame::Driver::AtNextBegin([] { StartPrepared(); });
		}
	}

	bool Requested()
	{
		static const bool requested = Switches::On("CS_ORG_GRAPHRES_SELFTEST");
		return requested;
	}

	void Reserve()
	{
		if (Requested())
			FrameAnchors::Reserve(FrameAnchors::Anchor::DeferredPasses);
	}

	void Setup()
	{
		auto& test = Test();
		if (!Requested() || std::exchange(test.attempted, true))
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses))
			return;
		auto resources = std::make_shared<Resources>();
		resources->write = ComputeProgram::Load(host->Device(), { .source = kShader, .entry = L"CSMain", .constantWords = kConstantWords });
		// Transient, but exported, so never aliased.
		resources->texture = std::make_unique<Texture2D>(TextureDesc(kSizes[0], false), kTextureName,
			Options{ .lifetime = Lifetime::Transient, .d3d11 = Export::SRV });
		if (!resources->write || !*resources->texture || !resources->texture->D3D11() || !resources->texture->SRV() || !CreateStaging(*resources)) {
			logger::error("[ORG] graph resource self-test: its program, exported texture or staging copy could not be created; off");
			return;
		}
		if (!FrameAnchors::Register(MakeClient(resources)))
			return;
		test.resources = std::move(resources);
		logger::info("[ORG] graph resource self-test: on; '{}' {}x{} exported (SRV made, aliasable {}), checked at DeferredPasses", kTextureName,
			kSizes[0], kSizes[0], test.resources->texture->Aliasable());
	}

	namespace
	{
		void Check()
		{
			auto& test = Test();
			if (!test.resources)
				return;
			auto& resources = *test.resources;
			auto* context = globals::d3d::context;
			if (test.copyPending) {
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const HRESULT hr = context->Map(resources.staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
				if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
					return;
				test.copyPending = false;
				if (FAILED(hr)) {
					logger::error("[ORG] graph resource self-test: the staging copy could not be mapped (0x{:08X}); off", static_cast<std::uint32_t>(hr));
					FrameAnchors::Unregister(kClientId);
					test.resources.reset();
					return;
				}
				std::uint32_t got = 0;
				std::memcpy(&got, static_cast<const std::byte*>(mapped.pData) + kTexelY * mapped.RowPitch + kTexelX * sizeof(std::uint32_t), sizeof(got));
				context->Unmap(resources.staging.get(), 0);
				const UINT size = resources.texture->Desc().Width;
				const std::uint32_t expected = ValueOf(size);
				if (got == expected) {
					logger::info("[ORG] graph resource self-test: {}x{} OK after {} copies (the graph wrote 0x{:08X}, D3D11 read 0x{:08X} through the export)",
						size, size, test.copies, expected, got);
					Advance(test);
					return;
				}
				// Zero is DXVK's initial clear, before the rebuilt graph's first write.
				if (got != 0 || test.copies >= kPatience) {
					logger::error("[ORG] graph resource self-test: {}x{} MISMATCH: D3D11 read 0x{:08X}, expected 0x{:08X}", size, size, got, expected);
					FrameAnchors::Unregister(kClientId);
					test.resources.reset();
					return;
				}
			}
			// The graph gave the texture back in its home layout, so D3D11 can copy it right after the epoch.
			context->CopyResource(resources.staging.get(), resources.texture->D3D11());
			test.copyPending = true;
			++test.copies;
		}
	}

	// The prepared part: three versioned history textures take a new size as revisions will give it. A worker thread prepares
	// the versions and makes them pending. A frame boundary adopts them, then the twins' own adoption follows (AdoptPrepared).
	// A pass writes the graph-only texture, copies take it into the exported ones, and D3D11 reads those back.
	// One export has a UAV and one does not, so in guest mode one rests in GENERAL (an import) and one in a read layout.
	namespace
	{
		void CheckPrepared();

		constexpr const char* kPreparedClientId = "cs.org.graphres-selftest.prepared";
		constexpr const char* kPreparedGraphName = "GraphResourcesSelfTest::PreparedGraph";
		constexpr const char* kPreparedExportName = "GraphResourcesSelfTest::PreparedExport";
		constexpr const char* kPreparedReadName = "GraphResourcesSelfTest::PreparedExportRead";

		struct PreparedResources
		{
			std::shared_ptr<const ComputeProgram> write;
			std::unique_ptr<Texture2D> graphOnly;     // versioned history, graph only
			std::unique_ptr<Texture2D> exported;      // versioned history, exported, with a UAV
			std::unique_ptr<Texture2D> exportedRead;  // the same without a UAV
			winrt::com_ptr<ID3D11Texture2D> staging, stagingRead;
			std::atomic<UINT> size{ 0 };  // the adopted size, which preparations of the passes read
		};

		/** @brief What the worker made: the versions, pending in their slots. */
		struct PreparedSet
		{
			PreparedVersion graphOnly, exported, exportedRead;
			std::shared_ptr<const org::TextureVersion> graphVersion, exportVersion, readVersion;
			std::string thread;
			double milliseconds = 0.0;
		};

		struct PreparedStatus
		{
			std::shared_ptr<PreparedResources> resources;
			UINT step = 0;
			std::uint32_t copies = 0;
			bool copyPending = false;
			std::future<PreparedSet> preparing;
			bool adoptQueued = false;
		};

		PreparedStatus& PreparedTest()
		{
			static PreparedStatus s_status;
			return s_status;
		}

		std::string SelfTestThreadName()
		{
			std::ostringstream out;
			out << std::this_thread::get_id();
			return out.str();
		}

		bool CreatePreparedStaging(PreparedResources& a_resources)
		{
			a_resources.staging = nullptr;
			a_resources.stagingRead = nullptr;
			const auto desc = TextureDesc(a_resources.exported->Desc().Width, true);
			if (FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_resources.staging.put())) ||
				FAILED(globals::d3d::device->CreateTexture2D(&desc, nullptr, a_resources.stagingRead.put())))
				return false;
			Util::SetResourceName(a_resources.staging.get(), "GraphResourcesSelfTest::PreparedStaging");
			Util::SetResourceName(a_resources.stagingRead.get(), "GraphResourcesSelfTest::PreparedStagingRead");
			return true;
		}

		struct PreparedWriteBindings
		{
			org::DeclaredViewToken texture;
		};

		struct PreparedWriteFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			UINT size = 0;
		};

		// Writes ValueOf(size) into the graph-only texture's version the preparation resolves.
		class PreparedWritePass final : public org::TypedRenderGraphPass<PreparedWritePass, PreparedWriteFrame, PreparedWriteBindings>
		{
		public:
			explicit PreparedWritePass(std::shared_ptr<PreparedResources> a_resources) :
				resources(std::move(a_resources)) {}

			PreparedWriteBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.UnorderedAccess(org::ResourceResolverAndRange(*resources->graphOnly->Versioned())).View() };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(reinterpret_cast<std::uintptr_t>(resources->write.get()));
				a_out.push_back(resources->size.load(std::memory_order_acquire));
			}

			PreparedWriteFrame Prepare(const PreparedWriteBindings&, const org::PassPrepareContext&) const
			{
				return { resources->write, resources->size.load(std::memory_order_acquire) };
			}

			static void Record(const PreparedWriteBindings& a_bindings, const PreparedWriteFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || !a_frame.size)
					return;
				const SelfTestConstants constants{ ValueOf(a_frame.size), a_recording.Index(a_bindings.texture), a_frame.size, a_frame.size };
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->Pipeline());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kConstantWords, reinterpret_cast<const std::uint32_t*>(&constants));
				commands.Dispatch((a_frame.size + 7) / 8, (a_frame.size + 7) / 8, 1);
			}

		private:
			std::shared_ptr<PreparedResources> resources;
		};

		struct PreparedCopyBindings
		{
			org::ResourceBindingToken source, target, targetRead;
		};

		struct PreparedCopyFrame
		{};

		// Copies the graph-only texture into the exported ones. Zero extents copy the whole mip, so each version is copied whole.
		class PreparedCopyPass final : public org::TypedRenderGraphPass<PreparedCopyPass, PreparedCopyFrame, PreparedCopyBindings>
		{
		public:
			explicit PreparedCopyPass(std::shared_ptr<PreparedResources> a_resources) :
				resources(std::move(a_resources)) {}

			PreparedCopyBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.CopySource(org::ResourceResolverAndRange(*resources->graphOnly->Versioned())),
					a_builder.CopyDestination(org::ResourceResolverAndRange(*resources->exported->Versioned())),
					a_builder.CopyDestination(org::ResourceResolverAndRange(*resources->exportedRead->Versioned())) };
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(resources->size.load(std::memory_order_acquire)); }

			PreparedCopyFrame Prepare(const PreparedCopyBindings&, const org::PassPrepareContext&) const { return {}; }

			static void Record(const PreparedCopyBindings& a_bindings, const PreparedCopyFrame&, org::PassRecordContext& a_recording)
			{
				const rhi::TextureCopyRegion from{ .texture = a_recording.Resolve(a_bindings.source).GetHandle(), .depth = 0 };
				for (const auto target : { a_bindings.target, a_bindings.targetRead }) {
					const rhi::TextureCopyRegion to{ .texture = a_recording.Resolve(target).GetHandle(), .depth = 0 };
					a_recording.Commands().CopyTextureRegion(to, from);
				}
			}

		private:
			std::shared_ptr<PreparedResources> resources;
		};

		void StopPrepared(PreparedStatus& a_test)
		{
			FrameAnchors::Unregister(kPreparedClientId);
			a_test.resources.reset();
		}

		void StartPrepared()
		{
			auto& test = PreparedTest();
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses))
				return;
			auto resources = std::make_shared<PreparedResources>();
			resources->write = ComputeProgram::Load(host->Device(), { .source = kShader, .entry = L"CSMain", .constantWords = kConstantWords });
			resources->graphOnly = std::make_unique<Texture2D>(TextureDesc(kSizes[0], false), kPreparedGraphName,
				Options{ .lifetime = Lifetime::History, .versioned = true });
			resources->exported = std::make_unique<Texture2D>(TextureDesc(kSizes[0], false), kPreparedExportName,
				Options{ .lifetime = Lifetime::History, .d3d11 = Export::SRV, .versioned = true });
			auto readDesc = TextureDesc(kSizes[0], false);
			readDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			resources->exportedRead = std::make_unique<Texture2D>(readDesc, kPreparedReadName,
				Options{ .lifetime = Lifetime::History, .d3d11 = Export::SRV, .versioned = true });
			const auto made = [](const std::unique_ptr<Texture2D>& a_texture) { return *a_texture && a_texture->Versioned(); };
			if (!resources->write || !made(resources->graphOnly) || !made(resources->exported) || !made(resources->exportedRead) ||
				!resources->exported->D3D11() || !resources->exportedRead->D3D11() || !CreatePreparedStaging(*resources)) {
				logger::error("[ORG] graph resource self-test: the prepared part's program, textures or staging copies could not be made; off");
				return;
			}
			resources->size.store(kSizes[0], std::memory_order_release);
			FrameAnchors::ClientDesc client{};
			client.id = kPreparedClientId;
			client.passes.push_back({ .name = std::string(kPreparedClientId) + ".write",
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<PreparedWritePass>(resources); } });
			client.passes.push_back({ .name = std::string(kPreparedClientId) + ".copy",
				.anchor = FrameAnchors::Anchor::DeferredPasses,
				.kind = FrameAnchors::PassKind::Compute,
				.make = [resources]() -> std::shared_ptr<org::RenderPass> { return std::make_shared<PreparedCopyPass>(resources); } });
			client.registerResources = [resources](org::RenderGraph& a_graph) {
				resources->graphOnly->Register(a_graph);
				resources->exported->Register(a_graph);
				resources->exportedRead->Register(a_graph);
			};
			client.executed = [](const FrameAnchors::Point&) { CheckPrepared(); };
			if (!FrameAnchors::Register(std::move(client)))
				return;
			test.resources = std::move(resources);
			// Another slice count is another shape class, which Prepare leaves to Resize.
			logger::info(
				"[ORG] graph resource self-test: prepared part on at {}x{} ('{}' graph only, '{}' and '{}' exported, versioned histories; "
				"another shape class preparable: {})",
				kSizes[0], kSizes[0], kPreparedGraphName, kPreparedExportName, kPreparedReadName,
				test.resources->graphOnly->Preparable({ kSizes[1], kSizes[1], 2 }));
		}

		/** @brief Off the render thread: every version at a_size, made pending in its slot as the revision lane will. */
		PreparedSet PrepareAll(std::shared_ptr<PreparedResources> a_resources, UINT a_size)
		{
			const auto start = std::chrono::steady_clock::now();
			PreparedSet set;
			set.thread = SelfTestThreadName();
			const Extent extent{ a_size, a_size, 1 };
			set.graphOnly = a_resources->graphOnly->Prepare(extent);
			set.exported = a_resources->exported->Prepare(extent);
			set.exportedRead = a_resources->exportedRead->Prepare(extent);
			if (set.graphOnly)
				set.graphVersion = set.graphOnly.slot->MakeImage(set.graphOnly.resource);
			if (set.exported)
				set.exportVersion = set.exported.slot->MakeImage(set.exported.resource);
			if (set.exportedRead)
				set.readVersion = set.exportedRead.slot->MakeImage(set.exportedRead.resource);
			set.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
			return set;
		}

		/** @brief At a frame boundary: the slots adopt the pending versions, then the twins follow. */
		void AdoptPreparedSet(PreparedSet a_set)
		{
			auto& test = PreparedTest();
			if (!test.resources)
				return;
			auto& resources = *test.resources;
			{
				const RenderGraph::Frame::Boundary::Caller caller("twin");
				a_set.graphOnly.slot->Adopt(a_set.graphVersion);
				a_set.exported.slot->Adopt(a_set.exportVersion);
				a_set.exportedRead.slot->Adopt(a_set.readVersion);
			}
			resources.graphOnly->AdoptPrepared(a_set.graphOnly);
			resources.exported->AdoptPrepared(a_set.exported);
			resources.exportedRead->AdoptPrepared(a_set.exportedRead);
			const UINT size = a_set.graphOnly.extent[0];
			resources.size.store(size, std::memory_order_release);
			// What the revision's recordings will do: preparations made before resolved the old versions.
			if (auto* host = RenderGraphRuntime::Get().Host())
				host->NoteNewVersions();
			test.adoptQueued = false;
			test.copies = 0;
			test.copyPending = false;
			if (!CreatePreparedStaging(resources) || resources.graphOnly->Desc().Width != size || resources.exported->Desc().Width != size ||
				resources.exportedRead->Desc().Width != size) {
				logger::error("[ORG] graph resource self-test: after adoption the textures are not {}x{} or the staging copies could not be made", size, size);
				StopPrepared(test);
				return;
			}
			logger::info(
				"[ORG] graph resource self-test: adopted the prepared versions at {}x{} on thread {}, prepared on thread {} in {:.3f} ms "
				"(contents: graph only {}, exported {} and {}; 0 undefined, 1 zero)",
				size, size, SelfTestThreadName(), a_set.thread, a_set.milliseconds, static_cast<int>(a_set.graphOnly.contents),
				static_cast<int>(a_set.exported.contents), static_cast<int>(a_set.exportedRead.contents));
		}

		/** @brief Prepares the next size off the render thread, or finishes. */
		void AdvancePrepared(PreparedStatus& a_test)
		{
			if (++a_test.step < kSteps) {
				const UINT size = kSizes[a_test.step];
				const Extent extent{ size, size, 1 };
				const auto& r = *a_test.resources;
				if (!r.graphOnly->Preparable(extent) || !r.exported->Preparable(extent) || !r.exportedRead->Preparable(extent)) {
					logger::error("[ORG] graph resource self-test: {}x{} keeps the shape class but is not preparable", size, size);
					StopPrepared(a_test);
					return;
				}
				a_test.preparing = std::async(std::launch::async, PrepareAll, a_test.resources, size);
				logger::info("[ORG] graph resource self-test: preparing {}x{} on a worker thread (render thread {})", size, size, SelfTestThreadName());
				return;
			}
			logger::info("[ORG] graph resource self-test: prepared versions OK; the textures are released");
			StopPrepared(a_test);
		}

		void CheckPrepared()
		{
			auto& test = PreparedTest();
			if (!test.resources)
				return;
			if (test.preparing.valid()) {
				if (test.preparing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
					return;
				PreparedSet set;
				try {
					set = test.preparing.get();
				} catch (const std::exception& e) {
					logger::error("[ORG] graph resource self-test: preparing failed: {}", e.what());
					StopPrepared(test);
					return;
				}
				if (!set.graphOnly || !set.exported || !set.exportedRead || !set.graphVersion || !set.exportVersion || !set.readVersion) {
					logger::error("[ORG] graph resource self-test: a twin made no prepared version");
					StopPrepared(test);
					return;
				}
				test.adoptQueued = true;
				RenderGraph::Frame::Driver::AtNextBegin([set = std::move(set)]() mutable { AdoptPreparedSet(std::move(set)); });
				return;
			}
			if (test.adoptQueued)
				return;
			auto& resources = *test.resources;
			auto* context = globals::d3d::context;
			const UINT size = resources.exported->Desc().Width;
			if (test.copyPending) {
				// Both copies went in one stream, so the later one being done means both are.
				D3D11_MAPPED_SUBRESOURCE mappedRead{};
				const HRESULT hrRead = context->Map(resources.stagingRead.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mappedRead);
				if (hrRead == DXGI_ERROR_WAS_STILL_DRAWING)
					return;
				D3D11_MAPPED_SUBRESOURCE mapped{};
				const HRESULT hr = SUCCEEDED(hrRead) ? context->Map(resources.staging.get(), 0, D3D11_MAP_READ, 0, &mapped) : hrRead;
				test.copyPending = false;
				if (FAILED(hr)) {
					if (SUCCEEDED(hrRead))
						context->Unmap(resources.stagingRead.get(), 0);
					logger::error("[ORG] graph resource self-test: a prepared staging copy could not be mapped (0x{:08X}); off", static_cast<std::uint32_t>(hr));
					StopPrepared(test);
					return;
				}
				const auto texel = [](const D3D11_MAPPED_SUBRESOURCE& a_mapped) {
					std::uint32_t value = 0;
					std::memcpy(&value, static_cast<const std::byte*>(a_mapped.pData) + kTexelY * a_mapped.RowPitch + kTexelX * sizeof(std::uint32_t), sizeof(value));
					return value;
				};
				const std::uint32_t got = texel(mapped);
				const std::uint32_t gotRead = texel(mappedRead);
				context->Unmap(resources.staging.get(), 0);
				context->Unmap(resources.stagingRead.get(), 0);
				const std::uint32_t expected = ValueOf(size);
				if (got == expected && gotRead == expected) {
					logger::info(
						"[ORG] graph resource self-test: prepared {}x{} OK after {} copies (the graph wrote 0x{:08X} into the graph-only version "
						"and copied it into the exported ones, D3D11 read 0x{:08X} and 0x{:08X})",
						size, size, test.copies, expected, got, gotRead);
					AdvancePrepared(test);
					return;
				}
				// Zero is the export's initial clear, before the first recording that binds the new versions.
				const bool early = (got == 0 || got == expected) && (gotRead == 0 || gotRead == expected);
				if (!early || test.copies >= kPatience) {
					logger::error("[ORG] graph resource self-test: prepared {}x{} MISMATCH: D3D11 read 0x{:08X} and 0x{:08X}, expected 0x{:08X}", size, size,
						got, gotRead, expected);
					StopPrepared(test);
					return;
				}
			}
			context->CopyResource(resources.staging.get(), resources.exported->D3D11());
			context->CopyResource(resources.stagingRead.get(), resources.exportedRead->D3D11());
			test.copyPending = true;
			++test.copies;
		}
	}
}

#endif
