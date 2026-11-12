#include "RenderGraph/Engine/D3D12SelfTest.h"

#include "RenderGraph/Switches.h"

#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_ORG_HAS_D3D12) && defined(CS_HAS_ORG_MODULE_SERVICES) && defined(ORG_MODULE_SERVICES_HAS_DXC)

#	include "Aftermath.h"
#	include "DxvkLoader.h"
#	include "RenderGraph/Engine/D3D12Device.h"
#	include "RenderGraph/RenderGraphRuntime.h"

#	include <ORGModuleServices/ShaderCompiler.h>
#	include <rhi.h>
#	include <rhi_helpers.h>

#	include <cstring>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <thread>

namespace RenderGraph::Engine::D3D12SelfTest
{
	namespace
	{
		constexpr std::uint32_t kThreads = 64;
		constexpr std::uint32_t kSeed = 0x5A17;
		constexpr std::uint32_t kPushWords = 8;  // the UAV handle, then SelfTestDispatch on its 16-byte boundary
		constexpr std::uint32_t kFrames = 120;
		constexpr std::uint32_t kFramesInFlight = 3;
		constexpr const char* kSource = "RenderGraph/D3D12SelfTest.hlsl";

		bool Ok(rhi::Result a_result, const char* a_what, std::string& a_failure)
		{
			if (a_result == rhi::Result::Ok)
				return true;
			a_failure = fmt::format("{} failed: {}", a_what, rhi::ResultName(a_result));
			return false;
		}

		/** @brief Step 1: a self-test shader (a_source under Data/Shaders) as DXIL, from the DXC CS ships beside the DXVK DLLs. Empty on failure. */
		std::vector<std::byte> CompileDxil(std::string& a_failure, const char* a_source = kSource)
		{
			const std::filesystem::path path = std::filesystem::path(RenderGraphRuntime::kShaderDirectory) / a_source;
			std::ifstream file(path, std::ios::binary);
			const std::vector<char> chars((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			if (chars.empty()) {
				a_failure = fmt::format("{} is missing", path.string());
				return {};
			}
			std::vector<std::byte> source(chars.size());
			std::memcpy(source.data(), chars.data(), chars.size());

			org::services::ShaderCompiler compiler(std::filesystem::path{}, DxvkLoader::GetRuntimeDir());
			if (!compiler.Available()) {
				a_failure = fmt::format("no dxcompiler.dll in {}", DxvkLoader::GetRuntimeDir().string());
				return {};
			}
			org::services::ShaderCompileRequest request{};
			request.sourceName = path.generic_string();
			request.source = source;
			request.entryPoint = L"main";
			request.target = L"cs_6_6";
			request.format = org::services::ShaderBinaryFormat::Dxil;
			request.warningsAsErrors = false;
			request.debugInfo = true;
			request.keepInMemory = false;
			request.includeDirectories = { RenderGraphRuntime::kShaderDirectory };
			request.defines.push_back({ L"BASICRHI_SHADER_API_D3D12", L"1" });
			request.defines.push_back({ L"CS_ORG_BINDLESS", L"1" });
			const auto artifact = compiler.Compile(std::move(request));
			if (!artifact) {
				a_failure = fmt::format("the DXIL build of {} failed:\n{}", a_source, artifact.diagnostics.substr(0, 2000));
				return {};
			}
			logger::info("[ORG] D3D12 self-test: {} built to DXIL ({} bytes)", a_source, artifact.binary.size());
			return artifact.binary;
		}

		/** @brief Step 2: one dispatch writes the buffer through the heap and root constants; the readback must match. */
		bool Dispatch(rhi::DevicePtr& a_device, const std::vector<std::byte>& a_dxil, std::string& a_failure)
		{
			rhi::Device device = a_device.Get();
			rhi::Queue queue = device.GetQueue(rhi::QueueKind::Graphics);
			if (!queue) {
				a_failure = "the device has no graphics queue";
				return false;
			}

			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::Compute;
			constants.num32BitValues = kPushWords;
			rhi::PipelineLayoutPtr layout;
			if (!Ok(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PF_None }, layout),
					"CreatePipelineLayout", a_failure))
				return false;
			rhi::PipelinePtr pipeline;
			const rhi::SubobjLayout subobjLayout{ layout->GetHandle() };
			const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { a_dxil.data(), static_cast<std::uint32_t>(a_dxil.size()) }, "main" };
			const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(shader) };
			if (!Ok(device.CreatePipeline(items, 2, pipeline), "CreatePipeline (DXIL compute)", a_failure))
				return false;

			constexpr std::uint64_t kBytes = kThreads * sizeof(std::uint32_t);
			rhi::ResourcePtr output;
			rhi::ResourcePtr readback;
			if (!Ok(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess,
							"CS D3D12 self-test output"),
						output),
					"CreateCommittedResource (output)", a_failure) ||
				!Ok(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::Readback, {}, "CS D3D12 self-test readback"),
						readback),
					"CreateCommittedResource (readback)", a_failure))
				return false;

			rhi::DescriptorHeapPtr heap;
			if (!Ok(device.CreateDescriptorHeap(rhi::DescriptorHeapDesc{ rhi::DescriptorHeapType::CbvSrvUav, 16, true, "CS D3D12 self-test heap" }, heap),
					"CreateDescriptorHeap", a_failure))
				return false;
			constexpr std::uint32_t kOutputSlot = 5;  // not 0, so a handle that never arrived reads the wrong descriptor
			rhi::UavDesc uav{};
			uav.dimension = rhi::UavDim::Buffer;
			uav.buffer.kind = rhi::BufferViewKind::Raw;
			uav.buffer.numElements = kThreads;
			if (!Ok(device.CreateUnorderedAccessView({ heap->GetHandle(), kOutputSlot }, output->GetHandle(), uav), "CreateUnorderedAccessView", a_failure))
				return false;

			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
			rhi::TimelinePtr timeline;
			if (!Ok(device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator", a_failure) ||
				!Ok(device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list), "CreateCommandList", a_failure) ||
				!Ok(device.CreateTimeline(timeline, 0, "CS D3D12 self-test"), "CreateTimeline", a_failure))
				return false;

			list->SetDescriptorHeaps(heap->GetHandle(), std::nullopt);
			rhi::BufferBarrier toWrite{};
			toWrite.buffer = output->GetHandle();
			toWrite.beforeSync = rhi::ResourceSyncState::None;
			toWrite.afterSync = rhi::ResourceSyncState::ComputeShading;
			toWrite.beforeAccess = rhi::ResourceAccessType::None;
			toWrite.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
			list->Barriers(rhi::BarrierBatch{ .buffers = { &toWrite, 1 } });
			list->BindLayout(layout->GetHandle());
			list->BindPipeline(pipeline->GetHandle());
			const std::uint32_t words[kPushWords] = { kOutputSlot, 0, 0, 0, kSeed, 0, 0, 0 };
			list->PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kPushWords, words);
			list->Dispatch(1, 1, 1);
			rhi::BufferBarrier toCopy = toWrite;
			toCopy.beforeSync = rhi::ResourceSyncState::ComputeShading;
			toCopy.afterSync = rhi::ResourceSyncState::Copy;
			toCopy.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
			toCopy.afterAccess = rhi::ResourceAccessType::CopySource;
			list->Barriers(rhi::BarrierBatch{ .buffers = { &toCopy, 1 } });
			list->CopyBufferRegion(readback->GetHandle(), 0, output->GetHandle(), 0, kBytes);
			list->End();

			const rhi::TimelinePoint signal{ timeline->GetHandle(), 1 };
			const rhi::CommandList lists[] = { list.Get() };
			rhi::SubmitDesc submit{};
			submit.signals = { &signal, 1 };
			if (!Ok(queue.Submit(lists, submit), "Queue::Submit", a_failure) || !Ok(timeline->HostWait(1, 5000), "Timeline::HostWait", a_failure))
				return false;
			queue.CheckDebugMessages();

			void* mapped = nullptr;
			readback->Map(&mapped, 0, kBytes);
			if (!mapped) {
				a_failure = "the readback buffer could not be mapped";
				return false;
			}
			std::uint32_t values[kThreads]{};
			std::memcpy(values, mapped, kBytes);
			readback->Unmap(0, 0);
			for (std::uint32_t i = 0; i < kThreads; ++i) {
				if (values[i] != kSeed * 3u + i) {
					a_failure = fmt::format("the dispatch wrote {:#x} at word {}, expected {:#x}: the descriptor heap or root constants did not arrive as "
											"the shader reads them",
						values[i], i, kSeed * 3u + i);
					return false;
				}
			}
			logger::info("[ORG] D3D12 self-test: the DXIL dispatch wrote all {} words through ResourceDescriptorHeap and root constants", kThreads);
			return true;
		}

		/**
		 * @brief Step 3: emulated device addresses (BasicRHI's device address table, Common/DeviceAddress.hlsli). A dispatch reads each
		 * word of a source buffer through its device address and writes word * 7 + 1 through the destination's; the readback must
		 * match. The heap has the device's full capacity, so it holds the table's region.
		 */
		bool DeviceAddresses(rhi::DevicePtr& a_device, const std::vector<std::byte>& a_dxil, std::string& a_failure)
		{
			constexpr std::uint32_t kWords = 256;
			constexpr std::uint32_t kAddressPushWords = 8;
			constexpr std::uint64_t kBytes = kWords * sizeof(std::uint32_t);
			rhi::Device device = a_device.Get();
			rhi::Queue queue = device.GetQueue(rhi::QueueKind::Graphics);

			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::Compute;
			constants.num32BitValues = kAddressPushWords;
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
			const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { a_dxil.data(), static_cast<std::uint32_t>(a_dxil.size()) }, "main" };
			if (!Ok(device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PF_None }, layout), "CreatePipelineLayout",
					a_failure))
				return false;
			const rhi::SubobjLayout subobjLayout{ layout->GetHandle() };
			const rhi::PipelineStreamItem items[] = { rhi::Make(subobjLayout), rhi::Make(shader) };
			if (!Ok(device.CreatePipeline(items, 2, pipeline), "CreatePipeline (device address test)", a_failure))
				return false;

			rhi::ResourcePtr source, destination, readback;
			if (!Ok(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::Upload, {}, "CS D3D12 self-test address source"), source),
					"CreateCommittedResource (source)", a_failure) ||
				!Ok(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::DeviceLocal, rhi::RF_AllowUnorderedAccess,
								"CS D3D12 self-test address destination"),
							destination),
					"CreateCommittedResource (destination)", a_failure) ||
				!Ok(device.CreateCommittedResource(rhi::helpers::ResourceDesc::Buffer(kBytes, rhi::HeapType::Readback, {}, "CS D3D12 self-test address readback"),
							readback),
					"CreateCommittedResource (readback)", a_failure))
				return false;
			void* mapped = nullptr;
			source->Map(&mapped, 0, kBytes);
			if (!mapped) {
				a_failure = "the source buffer could not be mapped";
				return false;
			}
			for (std::uint32_t i = 0; i < kWords; ++i)
				static_cast<std::uint32_t*>(mapped)[i] = 0x1000u + i * 3u;
			source->Unmap(0, kBytes);

			// The addresses join the device address table here (GetBufferDeviceAddress).
			const std::uint64_t from = device.GetBufferDeviceAddress({ source->GetHandle(), 0 });
			const std::uint64_t to = device.GetBufferDeviceAddress({ destination->GetHandle(), 0 });
			if (!from || !to) {
				a_failure = "GetBufferDeviceAddress gave no address";
				return false;
			}

			// The device's full capacity (rhi_shader_abi.h): only such a heap holds the table's region.
			rhi::DescriptorHeapPtr heap;
			if (!Ok(device.CreateDescriptorHeap(rhi::DescriptorHeapDesc{ rhi::DescriptorHeapType::CbvSrvUav, rhi::D3D12_DEVICE_ADDRESS_TABLE_SLOT, true,
											   "CS D3D12 self-test full heap" },
						heap),
					"CreateDescriptorHeap (full capacity)", a_failure))
				return false;

			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
			rhi::TimelinePtr timeline;
			if (!Ok(device.CreateCommandAllocator(rhi::QueueKind::Graphics, allocator), "CreateCommandAllocator", a_failure) ||
				!Ok(device.CreateCommandList(rhi::QueueKind::Graphics, allocator.Get(), list), "CreateCommandList", a_failure) ||
				!Ok(device.CreateTimeline(timeline, 0, "CS D3D12 self-test addresses"), "CreateTimeline", a_failure))
				return false;
			list->SetDescriptorHeaps(heap->GetHandle(), std::nullopt);
			rhi::BufferBarrier toWrite{};
			toWrite.buffer = destination->GetHandle();
			toWrite.beforeSync = rhi::ResourceSyncState::None;
			toWrite.afterSync = rhi::ResourceSyncState::ComputeShading;
			toWrite.beforeAccess = rhi::ResourceAccessType::None;
			toWrite.afterAccess = rhi::ResourceAccessType::UnorderedAccess;
			list->Barriers(rhi::BarrierBatch{ .buffers = { &toWrite, 1 } });
			list->BindLayout(layout->GetHandle());
			list->BindPipeline(pipeline->GetHandle());
			const std::uint32_t words[kAddressPushWords] = { static_cast<std::uint32_t>(from), static_cast<std::uint32_t>(from >> 32), static_cast<std::uint32_t>(to),
				static_cast<std::uint32_t>(to >> 32), kWords, 0, 0, 0 };
			list->PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kAddressPushWords, words);
			list->Dispatch(kWords / 64, 1, 1);
			rhi::BufferBarrier toCopy = toWrite;
			toCopy.beforeSync = rhi::ResourceSyncState::ComputeShading;
			toCopy.afterSync = rhi::ResourceSyncState::Copy;
			toCopy.beforeAccess = rhi::ResourceAccessType::UnorderedAccess;
			toCopy.afterAccess = rhi::ResourceAccessType::CopySource;
			list->Barriers(rhi::BarrierBatch{ .buffers = { &toCopy, 1 } });
			list->CopyBufferRegion(readback->GetHandle(), 0, destination->GetHandle(), 0, kBytes);
			list->End();
			const rhi::TimelinePoint signal{ timeline->GetHandle(), 1 };
			const rhi::CommandList lists[] = { list.Get() };
			rhi::SubmitDesc submit{};
			submit.signals = { &signal, 1 };
			if (!Ok(queue.Submit(lists, submit), "Queue::Submit", a_failure) || !Ok(timeline->HostWait(1, 5000), "Timeline::HostWait", a_failure))
				return false;
			queue.CheckDebugMessages();

			mapped = nullptr;
			readback->Map(&mapped, 0, kBytes);
			if (!mapped) {
				a_failure = "the readback buffer could not be mapped";
				return false;
			}
			std::vector<std::uint32_t> values(kWords);
			std::memcpy(values.data(), mapped, kBytes);
			readback->Unmap(0, 0);
			for (std::uint32_t i = 0; i < kWords; ++i) {
				const std::uint32_t expected = (0x1000u + i * 3u) * 7u + 1u;
				if (values[i] != expected) {
					a_failure = fmt::format("word {} read {:#x} through the device address table, expected {:#x} (source {:#x}, destination {:#x})", i, values[i],
						expected, from, to);
					return false;
				}
			}
			logger::info("[ORG] D3D12 self-test: {} words loaded and stored through emulated device addresses ({:#x} -> {:#x})", kWords, from, to);
			return true;
		}

		LRESULT CALLBACK WindowProc(HWND a_window, UINT a_message, WPARAM a_wParam, LPARAM a_lParam)
		{
			return ::DefWindowProcW(a_window, a_message, a_wParam, a_lParam);
		}

		struct Frame
		{
			rhi::CommandAllocatorPtr allocator;
			rhi::CommandListPtr list;
			std::uint64_t done = 0;
		};

		/** @brief Step 3: a swap chain on a small window, configured by ConfigureSwapchain, presents cleared frames. */
		bool Present(rhi::DevicePtr& a_device, std::string& a_failure)
		{
			rhi::Device device = a_device.Get();
			rhi::Queue queue = device.GetQueue(rhi::QueueKind::Graphics);
			const wchar_t* className = L"CommunityShadersD3D12SelfTest";
			const HINSTANCE instance = ::GetModuleHandleW(nullptr);
			WNDCLASSW windowClass{};
			windowClass.lpfnWndProc = WindowProc;
			windowClass.hInstance = instance;
			windowClass.lpszClassName = className;
			::RegisterClassW(&windowClass);
			const HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW, className, L"Community Shaders D3D12 self-test", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
				320, 240, nullptr, nullptr, instance, nullptr);
			if (!window) {
				a_failure = fmt::format("CreateWindowExW failed (error {})", ::GetLastError());
				return false;
			}
			::ShowWindow(window, SW_SHOWNOACTIVATE);
			RECT client{};
			::GetClientRect(window, &client);
			const auto width = static_cast<std::uint32_t>(client.right - client.left);
			const auto height = static_cast<std::uint32_t>(client.bottom - client.top);

			bool ok = true;
			{
				rhi::SwapchainPtr swapchain;
				rhi::DescriptorHeapPtr rtvHeap;
				rhi::TimelinePtr timeline;
				Frame frames[kFramesInFlight];
				ok = Ok(device.CreateDescriptorHeap(rhi::DescriptorHeapDesc{ rhi::DescriptorHeapType::RTV, 1, false, "CS D3D12 self-test RTV" }, rtvHeap),
						 "CreateDescriptorHeap (RTV)", a_failure) &&
				     Ok(device.CreateTimeline(timeline, 0, "CS D3D12 self-test frames"), "CreateTimeline", a_failure) &&
				     Ok(device.CreateSwapchain(window, width, height, rhi::Format::R8G8B8A8_UNorm, 3, true, swapchain), "CreateSwapchain", a_failure);
				for (Frame& frame : frames) {
					ok = ok && Ok(device.CreateCommandAllocator(rhi::QueueKind::Graphics, frame.allocator), "CreateCommandAllocator", a_failure) &&
					     Ok(device.CreateCommandList(rhi::QueueKind::Graphics, frame.allocator.Get(), frame.list), "CreateCommandList", a_failure);
					if (ok)
						frame.list->End();
				}
				D3D12PresentOptions options{};
				options.hdr = Switches::On("CS_ORG_D3D12_SELFTEST_HDR");
				options.maximumFrameLatency = 2;
				if (ok && !Engine::ConfigureSwapchain(swapchain.Get(), options))
					logger::warn("[ORG] D3D12 self-test: the swap chain refused part of its configuration (logged above); presenting anyway");

				std::uint64_t frameNumber = 0;
				for (std::uint32_t i = 0; ok && i < kFrames; ++i) {
					Frame& frame = frames[frameNumber % kFramesInFlight];
					if (frame.done && !Ok(timeline->HostWait(frame.done, 5000), "Timeline::HostWait", a_failure))
						break;
					// The waitable object: the frame latency the swap chain was given.
					const auto latency = swapchain->WaitForFrameLatency(1'000'000'000ull);
					if (latency != rhi::Result::Ok && latency != rhi::Result::Unsupported && !Ok(latency, "WaitForFrameLatency", a_failure))
						break;
					const rhi::ResourceHandle image = swapchain->Image(swapchain->CurrentImageIndex());
					if (!Ok(device.CreateRenderTargetView({ rtvHeap->GetHandle(), 0 }, image, {}), "CreateRenderTargetView", a_failure))
						break;
					frame.allocator->Recycle();
					frame.list->Recycle(frame.allocator.Get());
					rhi::TextureBarrier toTarget{};
					toTarget.texture = image;
					toTarget.beforeSync = rhi::ResourceSyncState::All;
					toTarget.afterSync = rhi::ResourceSyncState::RenderTarget;
					toTarget.beforeAccess = rhi::ResourceAccessType::Present;
					toTarget.afterAccess = rhi::ResourceAccessType::RenderTarget;
					toTarget.beforeLayout = rhi::ResourceLayout::Present;
					toTarget.afterLayout = rhi::ResourceLayout::RenderTarget;
					frame.list->Barriers(rhi::BarrierBatch{ .textures = { &toTarget, 1 } });
					rhi::ColorAttachment color{};
					color.rtv = { rtvHeap->GetHandle(), 0 };
					color.loadOp = rhi::LoadOp::Clear;
					color.storeOp = rhi::StoreOp::Store;
					color.resource = image;
					// A slow sweep, so a frozen present is visible.
					const float t = static_cast<float>(i) / static_cast<float>(kFrames);
					color.clear.rgba[0] = 0.1f + 0.4f * t;
					color.clear.rgba[1] = 0.2f;
					color.clear.rgba[2] = 0.5f - 0.4f * t;
					color.clear.rgba[3] = 1.0f;
					rhi::PassBeginInfo pass{};
					pass.colors = { &color, 1 };
					pass.width = width;
					pass.height = height;
					pass.debugName = "CS D3D12 self-test clear";
					frame.list->BeginPass(pass);
					frame.list->EndPass();
					rhi::TextureBarrier toPresent = toTarget;
					toPresent.beforeSync = rhi::ResourceSyncState::RenderTarget;
					toPresent.afterSync = rhi::ResourceSyncState::All;
					toPresent.beforeAccess = rhi::ResourceAccessType::RenderTarget;
					toPresent.afterAccess = rhi::ResourceAccessType::Present;
					toPresent.beforeLayout = rhi::ResourceLayout::RenderTarget;
					toPresent.afterLayout = rhi::ResourceLayout::Present;
					frame.list->Barriers(rhi::BarrierBatch{ .textures = { &toPresent, 1 } });
					frame.list->End();

					const rhi::TimelinePoint signal{ timeline->GetHandle(), ++frameNumber };
					const rhi::CommandList lists[] = { frame.list.Get() };
					rhi::SubmitDesc submit{};
					submit.signals = { &signal, 1 };
					if (!Ok(queue.Submit(lists, submit), "Queue::Submit", a_failure))
						break;
					frame.done = signal.value;
					rhi::PresentSyncDesc sync{};
					sync.queue = queue;
					sync.wait = signal;
					const auto presented = swapchain->Present(true, sync);
					queue.CheckDebugMessages();
					if (presented != rhi::Result::Ok && presented != rhi::Result::ModeChanged && !Ok(presented, "Swapchain::Present", a_failure))
						break;
					MSG message{};
					while (::PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
						::TranslateMessage(&message);
						::DispatchMessageW(&message);
					}
				}
				ok = ok && a_failure.empty();
				if (ok)
					logger::info("[ORG] D3D12 self-test: presented {} frames ({} presents counted), {}", kFrames, swapchain->PresentCount(),
						options.hdr ? "HDR10 asked for" : "SDR");
				if (device.WaitIdle() != rhi::Result::Ok)
					logger::warn("[ORG] D3D12 self-test: Device::WaitIdle failed");
			}
			::DestroyWindow(window);
			::UnregisterClassW(className, instance);
			return ok;
		}

		void Run()
		{
			std::string failure;
			const char* step = "device";
			bool passed = false;
			do {
				D3D12DeviceOptions options{};
				options.debug = Switches::On("CS_ORG_D3D12_DEBUG");
				options.crashMarkers = Switches::Equals("DXVK_DEBUG", "hang") || Aftermath::WantsCrashAnalysis();
				std::shared_ptr<rhi::DevicePtr> device;
				if (!Engine::CreateD3D12Device(options, device, failure))
					break;
				step = "DXIL build";
				const auto dxil = CompileDxil(failure);
				if (dxil.empty())
					break;
				step = "dispatch";
				if (!Dispatch(*device, dxil, failure))
					break;
				step = "device address build";
				const auto addressDxil = CompileDxil(failure, "RenderGraph/D3D12DeviceAddressTest.hlsl");
				if (addressDxil.empty())
					break;
				step = "device addresses";
				if (!DeviceAddresses(*device, addressDxil, failure))
					break;
				step = "present";
				if (!Present(*device, failure))
					break;
				passed = true;
			} while (false);
			if (passed)
				logger::info("[ORG] D3D12 self-test: passed (device, DXIL build, dispatch, device addresses, present)");
			else
				logger::error("[ORG] D3D12 self-test: FAILED at the {} step: {}", step, failure);
		}
	}

	void Start()
	{
		if (!Switches::On("CS_ORG_D3D12_SELFTEST"))
			return;
		logger::info("[ORG] D3D12 self-test: started on its own thread (CS_ORG_D3D12_SELFTEST=1)");
		std::thread(Run).detach();
	}
}

#else

namespace RenderGraph::Engine::D3D12SelfTest
{
	void Start()
	{
		if (Switches::On("CS_ORG_D3D12_SELFTEST"))
			logger::error("[ORG] D3D12 self-test: this build has no D3D12 backend or no runtime shader compiler (CS_ORG_D3D12, ORGModuleServices)");
	}
}

#endif
