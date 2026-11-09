#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GraphResources.h"

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "Utils/D3D.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <cstring>

namespace GraphResources::SelfTest
{
	namespace
	{
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

		/** @brief What the pass writes into a texture of that width. */
		constexpr std::uint32_t ValueOf(UINT a_width) { return 0x6A5E0000u | a_width; }

		struct Resources
		{
			std::shared_ptr<const ComputeProgram> write;
			std::unique_ptr<Texture2D> texture;
			winrt::com_ptr<ID3D11Texture2D> staging;  // the texture's size: D3D11 copies the export into it
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

		// The compute pass: ValueOf(width) into every texel of the exported texture.
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
			// At the build, on the render thread: the texture as it is now (GraphResources' rule: read Graph() when the pass is made).
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
			client.onFailed = [] {
				Test().resources.reset();
				logger::error("[ORG] graph resource self-test: stopped, the render graph failed");
			};
			return client;
		}

		/** @brief The step's value came back: resize for the next step, or finish (which releases the texture through its export rule). */
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
		}
	}

	bool Requested()
	{
		static const bool requested = [] {
			char value[8]{};
			return GetEnvironmentVariableA("CS_ORG_GRAPHRES_SELFTEST", value, sizeof(value)) && std::strcmp(value, "1") == 0;
		}();
		return requested;
	}

	void Ensure()
	{
		auto& test = Test();
		if (!Requested() || std::exchange(test.attempted, true))
			return;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses))
			return;
		auto resources = std::make_shared<Resources>();
		resources->write = ComputeProgram::Load(host->Device(), { .source = kShader, .entry = L"CSMain", .constantWords = kConstantWords });
		// Transient, but exported: never aliased.
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

	void AfterRun()
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
			// Zero: DXVK's initial clear, before the rebuilt graph's first write.
			if (got != 0 || test.copies >= kPatience) {
				logger::error("[ORG] graph resource self-test: {}x{} MISMATCH: D3D11 read 0x{:08X}, expected 0x{:08X}", size, size, got, expected);
				FrameAnchors::Unregister(kClientId);
				test.resources.reset();
				return;
			}
		}
		// Right after the epoch in D3D11's stream: the graph gave the texture back in its home layout.
		context->CopyResource(resources.staging.get(), resources.texture->D3D11());
		test.copyPending = true;
		++test.copies;
	}
}

#endif
