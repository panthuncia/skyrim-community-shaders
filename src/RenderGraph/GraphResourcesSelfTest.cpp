#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/GraphResources.h"

#	include "Globals.h"
#	include "RenderGraph/ComputeProgram.h"
#	include "RenderGraph/FrameAnchors.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"
#	include "Utils/D3D.h"
#	include "Utils/Setting.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>

#	include <cstring>

namespace GraphResources::SelfTest
{
	namespace
	{
		void Check();

		constexpr const char* kShader = "RenderGraph/GraphResourcesSelfTest.hlsl";
		constexpr const char* kClientId = "cs.org.graphres-selftest";
		constexpr const char* kTextureName = "GraphResourcesSelfTest::Texture";
		// The texture's size. A twin keeps its size; declared textures' new versions are the revision lane's (the resize tests).
		constexpr UINT kSize = 16;
		constexpr UINT kTexelX = 3;
		constexpr UINT kTexelY = 5;
		// Copies without the value before the check reports that nothing came back.
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
			std::uint32_t copies = 0;
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

		/** @brief Finishes and releases the texture. */
		void Finish(Status& a_test)
		{
			logger::info("[ORG] graph resource self-test: every check OK; the texture is released");
			FrameAnchors::Unregister(kClientId);
			a_test.resources.reset();
		}
	}

	// The CS::Setting pilot. A test-only settings struct takes the paths a ported feature field takes.
	namespace SettingPilot
	{
		struct PilotSettings
		{
			CS::Setting<std::uint32_t> gridSizeZ{ 64 };
			float plain = 1.0f;
		};
		NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(PilotSettings, gridSizeZ, plain)

		struct PilotRaises
		{
			CS::SettingVariableId id = 0;
			std::uint32_t count = 0;
			CS::SettingRegistry::RaiseHook previous = nullptr;
		};

		PilotRaises& Raises()
		{
			static PilotRaises s_raises;
			return s_raises;
		}

		// Counts the pilot's raises and passes every raise on to the hook it replaced.
		void CountPilotRaise(std::uint32_t a_id, std::string_view a_name, const void* a_bytes, std::size_t a_size)
		{
			auto& raises = Raises();
			if (a_id == raises.id)
				++raises.count;
			if (raises.previous)
				raises.previous(a_id, a_name, a_bytes, a_size);
		}

		/** @brief Runs the pilot's checks once. @return The first failed check, or empty when every one held. */
		std::string Run()
		{
			auto& raises = Raises();
			raises.id = CS::SettingRegistry::Declare("SelfTest.SettingPilot.gridSizeZ");
			raises.count = 0;
			raises.previous = CS::SettingRegistry::SetRaiseHook(CountPilotRaise);
			const SKSE::stl::scope_exit restoreHook([]() noexcept { CS::SettingRegistry::SetRaiseHook(Raises().previous); });

			PilotSettings settings;
			settings.gridSizeZ.Bind(raises.id);

			// The JSON holds the plain value.
			json saved = settings;
			if (!saved["gridSizeZ"].is_number_unsigned() || saved["gridSizeZ"].get<std::uint32_t>() != 64)
				return fmt::format("saved JSON is {}, not a plain 64", saved.dump());

			// LoadSettings assigns the whole struct. The member keeps its binding and raises once.
			saved["gridSizeZ"] = 96;
			settings = saved.get<PilotSettings>();
			if (settings.gridSizeZ.Get() != 96 || settings.gridSizeZ.Id() != raises.id || raises.count != 1)
				return fmt::format("loading 96 left value {} id {} raises {}", settings.gridSizeZ.Get(), settings.gridSizeZ.Id(), raises.count);

			// The same value again raises nothing.
			settings = saved.get<PilotSettings>();
			if (raises.count != 1)
				return fmt::format("loading the same value raised ({} raises)", raises.count);

			// A key missing from the JSON takes the default object's value.
			settings = json::object().get<PilotSettings>();
			if (settings.gridSizeZ.Get() != 64 || raises.count != 2)
				return fmt::format("loading defaults left value {} raises {}", settings.gridSizeZ.Get(), raises.count);

			// A copy is unbound, so its writes raise nothing.
			auto copy = settings.gridSizeZ;
			copy = 32u;
			if (copy.Id() != 0 || raises.count != 2)
				return fmt::format("a copy raised or kept the binding (id {} raises {})", copy.Id(), raises.count);

			// A raw write raises once through the address, as the scene widget interceptor reports it.
			*settings.gridSizeZ.Raw() = 128;
			const auto pilotUnraised = [&] { return std::ranges::contains(CS::SettingRegistry::Unraised(), raises.id); };
			if (!pilotUnraised())
				return "a raw write was not listed as unraised";
			CS::SettingRegistry::RaiseAt(settings.gridSizeZ.Raw());
			CS::SettingRegistry::RaiseAt(settings.gridSizeZ.Raw());
			if (raises.count != 3 || pilotUnraised())
				return fmt::format("a raw write raised {} times", raises.count - 2);
			return {};
		}
	}

	void CheckSettingPilot()
	{
		const auto failure = SettingPilot::Run();
		if (failure.empty())
			logger::info("[ORG] graph resource self-test: setting pilot OK (JSON round trip, whole-struct assign, copy, raw write)");
		else
			logger::error("[ORG] graph resource self-test: setting pilot FAILED: {}", failure);
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
		CheckSettingPilot();
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host || !FrameAnchors::HasEpoch(FrameAnchors::Anchor::DeferredPasses))
			return;
		auto resources = std::make_shared<Resources>();
		resources->write = ComputeProgram::Load(host->Device(), { .source = kShader, .entry = L"CSMain", .constantWords = kConstantWords });
		// Transient, but exported, so never aliased.
		resources->texture = std::make_unique<Texture2D>(TextureDesc(kSize, false), kTextureName,
			Options{ .lifetime = Lifetime::Transient, .d3d11 = Export::SRV });
		if (!resources->write || !*resources->texture || !resources->texture->D3D11() || !resources->texture->SRV() || !CreateStaging(*resources)) {
			logger::error("[ORG] graph resource self-test: its program, exported texture or staging copy could not be created; off");
			return;
		}
		if (!FrameAnchors::Register(MakeClient(resources)))
			return;
		test.resources = std::move(resources);
		logger::info("[ORG] graph resource self-test: on; '{}' {}x{} exported (SRV made, aliasable {}), checked at DeferredPasses", kTextureName,
			kSize, kSize, test.resources->texture->Aliasable());
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
					Finish(test);
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
}

#endif
