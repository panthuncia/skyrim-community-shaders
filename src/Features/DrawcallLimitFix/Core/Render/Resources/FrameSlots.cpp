// The main pass's captured bindings, the frame textures at t16 and up, and the names of the frame constants buffer's parts.
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Deferred.h"
#include "RenderGraph/Engine/Engine.h"
#include "RenderGraph/Engine/Native/WorldPass.h"
#include "RenderGraph/Engine/Tripwire/GuestDevice.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/Fatal.h"
#include "RenderGraph/Relocation/Holders.h"
#include "Snapshot/EngineReads.h"
#include "Snapshot/FrameCamera.h"

namespace DCLF::Draws
{
	std::shared_ptr<const FrameBlockViews> FrameBlockViews::Create(rhi::Device a_device, const std::shared_ptr<org::Buffer>& a_frameConstants,
		const std::shared_ptr<org::Buffer>& a_faceBlocks, std::uint32_t a_faceStride, std::uint32_t a_faces)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto service = host && host->Graph() ? host->Graph()->RetainDescriptorService() : nullptr;
		const std::uint32_t faces = a_faceBlocks ? std::max(a_faces, 1u) : 1u;
		if (!service || !a_frameConstants) {
			logger::error("[DCLF] frame block views: no descriptor service");
			return nullptr;
		}
		// One range, so a face's table is kFrameBlocks consecutive slots a PushIndex range binds from its first.
		auto range = service->AllocateDescriptorSlotRange(rhi::DescriptorHeapType::CbvSrvUav, true, faces * kFrameBlocks);
		if (range.size() != faces * kFrameBlocks) {
			for (const auto& slot : range)
				service->RetireDescriptorSlot(slot);
			logger::error("[DCLF] frame block views: the descriptor service gave no range of {} slots", faces * kFrameBlocks);
			return nullptr;
		}
		auto views = std::make_shared<FrameBlockViews>();
		views->service = std::move(service);
		views->slots = std::move(range);
		views->owner = std::make_shared<std::pair<std::shared_ptr<org::Buffer>, std::shared_ptr<org::Buffer>>>(a_frameConstants, a_faceBlocks);
		views->contiguous = true;
		views->faces = faces;
		// The views name the buffers' native resources, so the buffers never move.
		::RenderGraph::Relocation::Pin(*a_frameConstants);
		if (a_faceBlocks)
			::RenderGraph::Relocation::Pin(*a_faceBlocks);
		const auto constants = a_frameConstants->GetAPIResource().GetHandle();
		bool made = true;
		for (std::uint32_t f = 0; f < faces && made; ++f)
			ForEachFrameBlock([&](bool a_pixel, std::uint32_t a_register, std::uint32_t a_block) {
				if (!made)
					return;
				const rhi::DescriptorSlot& slot = views->slots[f * kFrameBlocks + a_block];
				const bool face = a_faceBlocks && a_register == kPerFrameVertexRegister;
				// The shared light block is half a slot.
				const std::uint64_t bytes = face ? a_faceStride : a_pixel && a_register == 3 ? kSharedLightBlockBytes : kFrameSlotBytes;
				const std::uint64_t offset = face ? std::uint64_t(f) * a_faceStride : FrameBlockOffset(a_pixel, a_register);
				const rhi::CbvDesc desc{ .byteOffset = offset, .byteSize = static_cast<std::uint32_t>(bytes) };
				if (a_device.CreateConstantBufferView(slot, face ? a_faceBlocks->GetAPIResource().GetHandle() : constants, desc) != rhi::Result::Ok) {
					logger::error("[DCLF] frame block views: face {} block {} ({} bytes at {}) could not be made", f, a_block, bytes, offset);
					made = false;
				}
			});
		if (!made)
			return nullptr;
		views->indices = views->FaceIndices(0);
		return views;
	}

	std::array<std::uint32_t, kFrameBlocks> FrameBlockViews::FaceIndices(std::uint32_t a_face) const
	{
		std::array<std::uint32_t, kFrameBlocks> out{};
		const std::uint32_t first = std::min(a_face, faces - 1) * kFrameBlocks;
		for (std::uint32_t b = 0; b < kFrameBlocks && first + b < slots.size(); ++b)
			out[b] = slots[first + b].index;
		return out;
	}

	std::shared_ptr<const FrameBlockViews> FrameBlockViews::CreateFor(rhi::Device a_device, const std::shared_ptr<org::Buffer>& a_buffer, std::span<const Block> a_blocks,
		const char* a_what)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		auto service = host && host->Graph() ? host->Graph()->RetainDescriptorService() : nullptr;
		if (!service || !a_buffer || a_blocks.size() > kFrameBlocks) {
			logger::error("[DCLF] {} views: no descriptor service", a_what);
			return nullptr;
		}
		auto views = std::make_shared<FrameBlockViews>();
		views->service = std::move(service);
		views->owner = a_buffer;
		// The views name the buffer's native resource, so the buffer never moves.
		::RenderGraph::Relocation::Pin(*a_buffer);
		const auto resource = a_buffer->GetAPIResource().GetHandle();
		for (std::uint32_t b = 0; b < a_blocks.size(); ++b) {
			const rhi::DescriptorSlot slot = views->service->AllocateDescriptorSlot(rhi::DescriptorHeapType::CbvSrvUav, true);
			views->slots.push_back(slot);
			const rhi::CbvDesc desc{ .byteOffset = a_blocks[b].offset, .byteSize = a_blocks[b].bytes };
			if (!slot.heap.valid() || a_device.CreateConstantBufferView(slot, resource, desc) != rhi::Result::Ok) {
				logger::error("[DCLF] {} views: the view of block {} ({} bytes at {}) could not be made", a_what, b, a_blocks[b].bytes, a_blocks[b].offset);
				return nullptr;
			}
			views->indices[b] = slot.index;
		}
		return views;
	}

	FrameBlockViews::~FrameBlockViews()
	{
		for (const auto& slot : slots)
			if (slot.heap.valid())
				service->RetireDescriptorSlotWithOwner(slot, owner);
	}

	std::string FrameConstantsPart(std::uint64_t a_offset)
	{
		const auto slot = static_cast<std::uint32_t>(a_offset / kFrameSlotBytes);
		const std::uint64_t within = a_offset % kFrameSlotBytes;
		if (slot == kFrameFogRegister)
			return within < sizeof(FrameFog) ? "frame constants VS b13 (fog)" : "frame constants VS b13 (extras frame)";
		if (slot < kConstantBufferRegisters)
			return fmt::format("frame constants VS b{}", slot);
		if (slot < 2 * kConstantBufferRegisters)
			return fmt::format("frame constants PS b{}", slot - kConstantBufferRegisters);
		if (slot == kFrameSlotSharedLight)
			return within < kSharedLightBlockBytes ? "frame constants: zero block" : "frame constants: shared light block (PS b3)";
		if (slot == kFrameSlotLighting)
			return within < sizeof(FrameLighting)                                   ? "frame constants: frame lighting" :
			       within < sizeof(FrameLighting) + sizeof(LodFadeFrame)           ? "frame constants: LOD fade frame" :
			       within < kExtrasPixelFrameOffset % kFrameSlotBytes               ? "frame constants: unused rows" :
			                                                                         "frame constants PS b13 (extras frame)";
		if (slot == kFrameSlotRecord)
			return "frame constants: frame record";
		return "frame constants: ?";
	}

	namespace
	{
		/**
		 * @brief In guest mode the engine's binds stay in its context handle and CS's reach DXVK's context. A slot takes CS's bind,
		 * as it did when both met on DXVK's context (CS binds after the engine's flush), or else the engine's, unwrapped to DXVK's
		 * object. Native mode and plain D3D11 have one context. a_out gets one reference per slot.
		 */
		template <class T, std::size_t N, class Get>
		void ReadBound(std::array<T*, N>& a_out, Get&& a_get)
		{
			auto* own = globals::d3d::context;
			a_get(own, a_out.data());
			auto* engine = RenderGraph::Engine::Tripwire::EngineContext();
			if (!engine || engine == own)
				return;
			std::array<T*, N> theirs{};
			a_get(engine, theirs.data());
			for (std::size_t i = 0; i < N; ++i) {
				if (!theirs[i])
					continue;
				if (!a_out[i]) {
					a_out[i] = RenderGraph::Engine::Tripwire::Unwrap(theirs[i]);
					if (a_out[i])
						a_out[i]->AddRef();
				}
				theirs[i]->Release();
			}
		}

		/** @brief The texture behind a bound view, as DXVK's object in guest mode. */
		template <class View>
		winrt::com_ptr<ID3D11Texture2D> TextureOf(View* a_view)
		{
			auto* view = RenderGraph::Engine::Tripwire::Unwrap(a_view);
			if (!view)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			return resource.try_as<ID3D11Texture2D>();
		}
	}

	Capture CaptureBindings()
	{
		CS_ENGINE_READ_MARK(BoundState, BoundState);
		RenderThreadBudget::Part budget(RenderThreadBudget::Bucket::Capture);
		// In graph mode the engine's binds stay in its context handle, and D3D11's context holds none of them.
		if (RenderGraph::Engine::GraphMode() && !RenderGraph::Engine::Tripwire::GuestContextInstalled())
			RenderGraph::Fatal("DCLF capture", "the engine's bindings were read before its context handle was in place");
		Capture capture;
		// Native supplies every pass-wide block itself: b12 from the frame camera, b5 and b6 from CS's state, VS b13 from DCLF. Nothing
		// else is bound at the main pass (observed), so it reads no bound constant buffer.
		const bool native = Snapshot::FrameCamera::Owned();
		if (!native) {
			ReadBound(capture.vsBuffers, [](ID3D11DeviceContext* a_context, ID3D11Buffer** a_out) { a_context->VSGetConstantBuffers(0, kConstantBufferRegisters, a_out); });
			ReadBound(capture.psBuffers, [](ID3D11DeviceContext* a_context, ID3D11Buffer** a_out) { a_context->PSGetConstantBuffers(0, kConstantBufferRegisters, a_out); });
		}
		ReadBound(capture.psViews, [](ID3D11DeviceContext* a_context, ID3D11ShaderResourceView** a_out) { a_context->PSGetShaderResources(0, kTextureRegisters, a_out); });
		// The targets and the viewport are the engine's alone. CS binds none for an engine draw.
		auto* engine = RenderGraph::Engine::Tripwire::EngineContext();
		if (RenderGraph::Engine::Native::WorldPass::InRenderWorld()) {
			// Native's RenderWorld: the deferred pass's targets from CS's frame (Deferred::StartDeferred's set, the main depth), which the
			// engine's state flush bound here (observed equal at every main pass).
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			const auto& set = Deferred::kTargets;
			for (std::uint32_t i = 0; i < set.size() && i < kColorTargets; ++i) {
				capture.targets[i] = TextureOf(renderer->GetRuntimeData().renderTargets[set[i]].RTV);
				if (capture.targets[i])
					capture.targetCount = i + 1;
			}
			capture.depth = TextureOf(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0]);
		} else if (RenderGraph::Engine::Native::WorldPass::InRenderDepth()) {
			// Native's RenderDepth: no colour target and the main depth, as the engine's depth pass binds them.
			auto* renderer = CS_ENGINE_READ(Renderer, RecordTime, globals::game::renderer);
			capture.depth = TextureOf(renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN].views[0]);
		} else {
			ID3D11RenderTargetView* views[kColorTargets] = {};
			ID3D11DepthStencilView* depth = nullptr;
			engine->OMGetRenderTargets(kColorTargets, views, &depth);
			for (std::uint32_t i = 0; i < kColorTargets; ++i) {
				if (!views[i])
					continue;
				capture.targets[i] = TextureOf(views[i]);
				capture.targetCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				capture.depth = TextureOf(depth);
				depth->Release();
			}
		}
		// CS binds b5 SharedData and b6 FeatureData from Renderer_ResetState, so whether they are bound here depends on the last reset.
		// They are CS's own buffers, so take them from CS when the capture lacks them.
		if (auto* state = globals::state; state && !native) {
			auto adopt = [&](std::uint32_t a_slot, ConstantBuffer* a_buffer) {
				if (capture.psBuffers[a_slot] || !a_buffer || !a_buffer->CB())
					return;
				capture.psBuffers[a_slot] = a_buffer->CB();
				capture.psBuffers[a_slot]->AddRef();  // Capture::Release owns what it holds
			};
			adopt(kSharedDataRegister, state->sharedDataCB.get());
			adopt(kFeatureDataRegister, state->featureDataCB.get());
		}
		if (native) {
			// The frame camera's eyes: rows are stored relative to the camera b12 draws with, and the previous one is the camera this
			// render side drew last, as the rows' previous transforms are (decision 43).
			const auto& frame = Snapshot::FrameCamera::Main().data;
			capture.eye = { frame.CameraPosAdjust.x, frame.CameraPosAdjust.y, frame.CameraPosAdjust.z };
			capture.previousEye = { frame.CameraPreviousPosAdjust.x, frame.CameraPreviousPosAdjust.y, frame.CameraPreviousPosAdjust.z };
		} else {
			auto& shadowState = CS_ENGINE_READ(ShadowState, CapturePoint, globals::game::shadowState)->GetRuntimeData();
			capture.eye = shadowState.posAdjust.getEye();
			capture.previousEye = shadowState.previousPosAdjust.getEye();
		}
		if (native) {
			// The size comes from the revision the recordings name (PostRevisionInputs). The main pass draws inside the pre-resolve's first
			// depth shift window, and the Z-prepass takes the main pass's range so both rasterise alike.
			const auto range = RenderGraph::Engine::Native::WorldPass::DepthRange(true);
			capture.minDepth = range[0];
			capture.maxDepth = range[1];
			return capture;
		}
		D3D11_VIEWPORT viewport{};
		UINT count = 1;
		engine->RSGetViewports(&count, &viewport);
		capture.viewportWidth = static_cast<std::uint32_t>(viewport.Width);
		capture.viewportHeight = static_cast<std::uint32_t>(viewport.Height);
		capture.minDepth = viewport.MinDepth;
		capture.maxDepth = viewport.MaxDepth;
		return capture;
	}

	namespace
	{
		/** @brief Whether a_register belongs to one of the frame graph buffers in a_graphBuffers. */
		bool GraphBufferHeld(std::uint32_t a_graphBuffers, std::uint32_t a_register)
		{
			const auto registered = Extensions::RegisteredFrameGraphBuffers();
			for (std::size_t b = 0; b < registered.size(); ++b)
				if (((a_graphBuffers >> b) & 1) && a_register >= registered[b].firstRegister && a_register - registered[b].firstRegister < registered[b].count)
					return true;
			return false;
		}
	}

	std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, std::uint32_t a_graphBuffers)
	{
		std::vector<FrameBuffer> result;
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
			auto* view = a_capture.psViews[t];
			if (!view || (a_graphBuffers && GraphBufferHeld(a_graphBuffers, t)))
				continue;
			D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc{};
			view->GetDesc(&viewDesc);
			if (viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && viewDesc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
				continue;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			auto buffer = resource.try_as<ID3D11Buffer>();
			D3D11_BUFFER_DESC bufferDesc{};
			buffer->GetDesc(&bufferDesc);
			if (!(bufferDesc.CPUAccessFlags & D3D11_CPU_ACCESS_WRITE) || !bufferDesc.StructureByteStride)
				continue;  // only CPU-written structured buffers are copied
			FrameBuffer frameBuffer;
			frameBuffer.textureRegister = t;
			frameBuffer.stride = bufferDesc.StructureByteStride;
			frameBuffer.firstElement = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.FirstElement : viewDesc.BufferEx.FirstElement;
			frameBuffer.elements = viewDesc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER ? viewDesc.Buffer.NumElements : viewDesc.BufferEx.NumElements;
			result.push_back(frameBuffer);
		}
		return result;
	}
}
