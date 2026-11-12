#if defined(CS_HAS_RENDER_GRAPH)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "RenderGraph/SharedConstants.h"

#	include "Globals.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "RenderGraph/Switches.h"
#	include "State.h"

#	include <OpenRenderGraph/GraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>

#	include <array>
#	include <atomic>
#	include <cstring>
#	include <span>
#	include <vector>

namespace SharedConstants
{
	namespace
	{
		enum Block : std::uint32_t
		{
			kFrameBuffer,
			kSharedData,
			kFeatureData,
			kPermutation,
			kBlocks
		};

		constexpr std::array<const char*, kBlocks> kNames{ "cs.shared.frame-buffer", "cs.shared.shared-data", "cs.shared.feature-data",
			"cs.shared.permutation" };

		constexpr std::uint32_t Align256(std::uint32_t a_bytes) { return (a_bytes + 255u) & ~255u; }

		struct Buffers
		{
			std::array<std::shared_ptr<org::Buffer>, kBlocks> buffers;
			std::array<std::uint32_t, kBlocks> bytes{};    // each block's size
			std::array<std::uint32_t, kBlocks> offsets{};  // each block's offset in a latch region
			std::shared_ptr<org::LatchBlock> latch;
		};

		struct Store
		{
			std::shared_ptr<const Buffers> buffers;
			bool failed = false;
			// What the last Commit wrote, for CheckAfterRun.
			std::array<std::vector<std::byte>, kBlocks> committed;
			bool committedThisRun = false;
			std::uint32_t checks = 0;
			std::uint32_t mismatches = 0;
		};

		Store& Shared()
		{
			static Store s_state;
			return s_state;
		}

		bool CheckRequested()
		{
			static const bool requested = Switches::On("CS_ORG_SHARED_CONSTANTS_CHECK");
			return requested;
		}

		/** @brief The bytes D3D11 reads from each block now. Empty where CS has not uploaded one yet. */
		std::array<std::span<const std::byte>, kBlocks> Sources()
		{
			std::array<std::span<const std::byte>, kBlocks> sources{};
			const auto& frame = globals::game::frameBufferCached.data;
			sources[kFrameBuffer] = std::as_bytes(std::span(&frame, 1));
			if (auto* state = globals::state) {
				sources[kSharedData] = std::as_bytes(std::span(&state->lastSharedData, 1));
				sources[kFeatureData] = state->lastFeatureData;
				// What permutationCB holds: the last block it uploaded.
				sources[kPermutation] = std::as_bytes(std::span(&state->permutationDataPrevious, 1));
			}
			return sources;
		}

		struct UploadBindings
		{
			std::array<org::ResourceBindingToken, kBlocks> targets;
		};

		struct UploadFrame
		{
			std::shared_ptr<const Buffers> buffers;
		};

		// The execution's latch region into the four buffers.
		class UploadPass final : public org::TypedRenderGraphPass<UploadPass, UploadFrame, UploadBindings>
		{
		public:
			explicit UploadPass(std::shared_ptr<const Buffers> a_buffers) :
				buffers(std::move(a_buffers)) {}

			UploadBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				UploadBindings bindings{};
				for (std::uint32_t i = 0; i < kBlocks; ++i)
					bindings.targets[i] = a_builder.CopyDestination(buffers->buffers[i]);
				return bindings;
			}

			void Revision(std::vector<std::uint64_t>& a_out) const { a_out.push_back(reinterpret_cast<std::uintptr_t>(buffers.get())); }

			UploadFrame Prepare(const UploadBindings&, const org::PassPrepareContext&) const { return { buffers }; }

			static void Record(const UploadBindings& a_bindings, const UploadFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.buffers)
					return;
				// The run's region of the latch: its arena chunk and its offset there.
				const auto region = a_frame.buffers->latch->At(a_recording.Run());
				const auto source = region.buffer->GetAPIResource().GetHandle();
				for (std::uint32_t i = 0; i < kBlocks; ++i)
					a_recording.Commands().CopyBufferRegion(a_recording.Resolve(a_bindings.targets[i]).GetHandle(), 0, source,
						region.offset + a_frame.buffers->offsets[i], a_frame.buffers->bytes[i]);
			}

		private:
			std::shared_ptr<const Buffers> buffers;
		};

		/** @brief A staging copy of a D3D11 buffer's contents now (stalls). Empty on failure. */
		std::vector<std::byte> ReadBack(ID3D11Buffer* a_buffer)
		{
			std::vector<std::byte> bytes;
			if (!a_buffer)
				return bytes;
			D3D11_BUFFER_DESC desc{};
			a_buffer->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> staging;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
				return bytes;
			Util::SetResourceName(staging.get(), "SharedConstants::CheckStaging");
			auto* context = globals::d3d::context;
			context->CopyResource(staging.get(), a_buffer);
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
				return bytes;
			const auto* data = static_cast<const std::byte*>(mapped.pData);
			bytes.assign(data, data + desc.ByteWidth);
			context->Unmap(staging.get(), 0);
			return bytes;
		}
	}

	Bindings Declare(org::PassBuilder& a_builder)
	{
		const auto& buffers = *Shared().buffers;
		return {
			a_builder.ConstantBuffer(buffers.buffers[kFrameBuffer]).View(),
			a_builder.ConstantBuffer(buffers.buffers[kSharedData]).View(),
			a_builder.ConstantBuffer(buffers.buffers[kFeatureData]).View(),
			a_builder.ConstantBuffer(buffers.buffers[kPermutation]).View(),
		};
	}

	Handles Index(const org::RecordingContext& a_recording, const Bindings& a_bindings)
	{
		return { a_recording.Index(a_bindings.frameBuffer), a_recording.Index(a_bindings.sharedData), a_recording.Index(a_bindings.featureData),
			a_recording.Index(a_bindings.permutation) };
	}

	bool Ensure()
	{
		auto& state = Shared();
		if (state.buffers)
			return true;
		if (state.failed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		auto* cs = globals::state;
		if (!host || !RenderGraphRuntime::Get().IsActive() || !cs || !cs->featureDataBytes) {
			// Asked again at each anchor run until CS's first upload sizes FeatureData: said once.
			static std::atomic_flag s_logged;
			if (!s_logged.test_and_set())
				logger::info("[ORG] Shared constants: not available yet (render graph inactive, or FeatureData not sized); their clients wait");
			return false;
		}
		try {
			auto buffers = std::make_shared<Buffers>();
			buffers->bytes = { static_cast<std::uint32_t>(sizeof(globals::game::frameBufferCached.data)),
				static_cast<std::uint32_t>(sizeof(::State::SharedDataCB)), cs->featureDataBytes,
				static_cast<std::uint32_t>(sizeof(::State::PermutationCB)) };
			std::uint32_t regionBytes = 0;
			for (std::uint32_t i = 0; i < kBlocks; ++i) {
				buffers->offsets[i] = regionBytes;
				regionBytes += Align256(buffers->bytes[i]);
				// A CBV's range is whole 256-byte rows on D3D12; the shader reads only the block.
				const std::uint32_t size = Align256(buffers->bytes[i]);
				auto buffer = org::Buffer::CreateSharedUnmaterialized(rhi::HeapType::DeviceLocal, size);
				org::BufferBase::DescriptorRequirements views{};
				views.createCBV = true;
				views.cbvDesc = { 0, size };
				buffer->SetDescriptorRequirements(views);
				buffer->SetName(kNames[i]);
				buffer->Materialize();
				buffers->buffers[i] = std::move(buffer);
			}
			buffers->latch = std::make_shared<org::LatchBlock>("cs.shared.latch", regionBytes);
			state.buffers = std::move(buffers);
			logger::info("[ORG] Shared constants for graph passes: FrameBuffer {} B, SharedData {} B, FeatureData {} B, Permutation {} B",
				state.buffers->bytes[kFrameBuffer], state.buffers->bytes[kSharedData], state.buffers->bytes[kFeatureData],
				state.buffers->bytes[kPermutation]);
			return true;
		} catch (const std::exception& e) {
			logger::error("[ORG] Shared constants could not be created: {}", e.what());
			state.failed = true;
			return false;
		}
	}

	bool Ready()
	{
		return static_cast<bool>(Shared().buffers);
	}

	bool Failed()
	{
		return Shared().failed;
	}

	void Register(org::RenderGraph& a_graph)
	{
		const auto& buffers = Shared().buffers;
		if (!buffers)
			return;
		for (std::uint32_t i = 0; i < kBlocks; ++i)
			a_graph.RegisterResource(org::ResourceIdentifier(kNames[i]), buffers->buffers[i]);
	}

	std::shared_ptr<org::RenderPass> MakeUploadPass()
	{
		const auto& buffers = Shared().buffers;
		if (!buffers)
			return nullptr;
		return std::static_pointer_cast<org::RenderPass>(std::make_shared<UploadPass>(buffers));
	}

	void Commit()
	{
		auto& state = Shared();
		if (!state.buffers)
			return;
		const auto& buffers = *state.buffers;
		// This run's region: its own, which nothing else reads.
		const auto run = RenderGraphRuntime::Get().Host()->CurrentRun();
		const auto sources = Sources();
		for (std::uint32_t i = 0; i < kBlocks; ++i) {
			// A block CS has not uploaded yet stays as it was; one that grew is cut to the buffer.
			const auto bytes = sources[i].first((std::min)(sources[i].size(), std::size_t{ buffers.bytes[i] }));
			if (!bytes.empty())
				buffers.latch->Write(run, buffers.offsets[i], bytes);
			if (CheckRequested())
				state.committed[i].assign(bytes.begin(), bytes.end());
		}
		state.committedThisRun = true;
	}

	void CheckAfterRun()
	{
		auto& state = Shared();
		if (!CheckRequested() || !std::exchange(state.committedThisRun, false))
			return;
		auto* cs = globals::state;
		if (!cs)
			return;
		const std::array<ID3D11Buffer*, kBlocks> d3d11{ *globals::game::perFrame.get(), cs->sharedDataCB ? cs->sharedDataCB->CB() : nullptr,
			cs->featureDataCB ? cs->featureDataCB->CB() : nullptr, cs->permutationCB ? cs->permutationCB->CB() : nullptr };
		std::array<bool, kBlocks> equal{};
		bool all = true;
		for (std::uint32_t i = 0; i < kBlocks; ++i) {
			const auto gpu = ReadBack(d3d11[i]);
			const auto& ours = state.committed[i];
			equal[i] = !ours.empty() && gpu.size() >= ours.size() && std::memcmp(gpu.data(), ours.data(), ours.size()) == 0;
			all = all && equal[i];
		}
		++state.checks;
		if (!all)
			++state.mismatches;
		if (state.checks > 5 && (all || state.mismatches > 20))
			return;
		const auto& frame = globals::game::frameBufferCached.data;
		const auto& shared = cs->lastSharedData;
		logger::info(
			"[ORG] Shared constants check {}: FrameBuffer {}, SharedData {}, FeatureData {} ({} B), Permutation {}; "
			"CameraPosAdjust ({:.3f}, {:.3f}, {:.3f}), FrameParams ({:.4f}, {:.4f}, {:.4f}, {:.4f}), BufferDim ({}, {}), FrameCount {}, "
			"InInterior {}",
			state.checks, equal[kFrameBuffer] ? "equal" : "DIFFERS", equal[kSharedData] ? "equal" : "DIFFERS",
			equal[kFeatureData] ? "equal" : "DIFFERS", state.committed[kFeatureData].size(), equal[kPermutation] ? "equal" : "DIFFERS",
			frame.CameraPosAdjust.x, frame.CameraPosAdjust.y, frame.CameraPosAdjust.z, frame.FrameParams.x, frame.FrameParams.y,
			frame.FrameParams.z, frame.FrameParams.w, shared.BufferDim.x, shared.BufferDim.y, shared.FrameCount, shared.InInterior);
	}
}

#endif
