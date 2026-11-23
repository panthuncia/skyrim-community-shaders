// The DCLF pass capture's two halves: the copies recorded at a captured pass's start, and the frame-end dump.
#include "Features/DrawcallLimitFix/Diagnostics/ReplayCapture/ReplayCapture.h"

#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/FrameSource.h"
#include "RenderGraph/Backend.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include "RenderGraph/Switches.h"

#include <OpenRenderGraph/GraphHost.h>
#include <rhi_interop.h>

#if defined(CS_ORG_HAS_D3D12)
#	include <d3d12.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

namespace DCLF::ReplayCapture
{
	namespace
	{
		std::mutex notesMutex;
		std::unordered_map<std::uint64_t, PipelineNote> pipelineNotes;  // by handle index and generation
		std::unordered_map<std::string, std::string> stageLines;
		std::string optionsLine;

		std::uint64_t HandleKey(rhi::PipelineHandle a_handle) { return (std::uint64_t(a_handle.generation) << 32) | a_handle.index; }

		std::string Directory()
		{
			// Switches caches the value, so this is a map lookup.
			return ::Switches::Value("CS_DCLF_CAPTURE");
		}
	}

	bool Enabled()
	{
		// The backend is chosen after the first callers ask, so only the directory is cached.
		return !Directory().empty() && ::RenderGraph::Backend::IsD3D12();
	}

	std::string StageKey(bool a_pixel, bool a_depth, bool a_pulled, bool a_family, std::uint32_t a_descriptor, std::uint8_t a_literals, std::uint32_t a_runtimeVertex,
		std::uint32_t a_runtimePixel, bool a_depthTex)
	{
		const std::string key = fmt::format("{:d}{:d}{:d}{:d}{}.{:08X}", a_pixel, a_depth, a_pulled, a_family, a_depthTex ? "t" : "", a_descriptor);
		if (!a_literals)
			return key;
		// Only the words the stage takes, so equal stages share a key.
		return fmt::format("{}.{:d}.{:08X}.{:08X}", key, a_literals, (a_literals & 1) ? a_runtimeVertex : 0u, (a_literals & 2) ? a_runtimePixel : 0u);
	}

	void NotePipeline(rhi::PipelineHandle a_pipeline, PipelineNote a_note)
	{
		if (!Enabled() || !a_pipeline.valid())
			return;
		std::lock_guard lock(notesMutex);
		pipelineNotes[HandleKey(a_pipeline)] = std::move(a_note);
	}

	void NoteStageLine(const std::string& a_key, std::string a_line)
	{
		if (!Enabled())
			return;
		std::lock_guard lock(notesMutex);
		if (a_key.empty())
			optionsLine = std::move(a_line);
		else
			stageLines[a_key] = std::move(a_line);
	}
}

#if !defined(CS_ORG_HAS_D3D12)
namespace DCLF::ReplayCapture
{
	bool Arm(std::string& a_error)
	{
		a_error = "this build has no D3D12";
		return false;
	}
	bool Wanted(const char*) { return false; }
	void RecordPass(rhi::CommandList&, Pass) {}
	void OnFrameEnd() {}
}
#else
namespace DCLF::ReplayCapture
{
	namespace
	{
		using winrt::com_ptr;

		constexpr std::uint64_t kHeaderBytes = 512;  // the readback's marker, then 512-aligned regions (texture placement)
		constexpr std::uint64_t kBatchBytes = 256ull << 20;  // the frame end's readback batches
		// WKPDID_D3DDebugObjectNameW, spelled out so no GUID library is needed.
		constexpr GUID kDebugObjectNameW = { 0x4cca5fd8, 0x921f, 0x42c8, { 0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41 } };
		const char* const kPassOrder[] = { "Z-prepass", "Z-prepass phase two", "main opaque" };

		std::uint64_t Align(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) / a_alignment * a_alignment; }

		/** @brief A buffer range of a resource. */
		struct Range
		{
			com_ptr<ID3D12Resource> resource;
			std::uint64_t offset = 0, size = 0;
			std::uint64_t copyOffset = 0;  // in the pass's readback, for a range copied at the pass's start
		};

		/** @brief A pass as recorded, with its readback. */
		struct Recorded
		{
			Pass pass;
			Range frameConstants, sequences, counts, indexPool;
			com_ptr<ID3D12Resource> depth;
			D3D12_RESOURCE_DESC depthDesc{};
			std::uint64_t depthOffset = 0, depthBytes = 0;
			std::array<com_ptr<ID3D12Resource>, 8> colours;
			std::array<std::uint64_t, 8> colourOffsets{}, colourBytes{};
			com_ptr<ID3D12Resource> readback;
			std::uint32_t serial = 0;
		};

		struct State
		{
			std::mutex mutex;
			bool armed = false;
			bool markersCleared = false;  // the frame end after the arm cleared every marker, so a set one ran since
			std::uint32_t framesWaited = 0;
			std::uint32_t serial = 0;
			// Every recent recording of each pass. A kept recording replays its copies each frame it runs, so any of them may be the last.
			std::map<std::string, std::deque<Recorded>> recordings;
			std::map<std::string, com_ptr<ID3D12Resource>> readbacks;  // by pass, reused while big enough
			std::vector<com_ptr<ID3D12Resource>> retired;  // outgrown readbacks, which a kept recording may still copy into
		};
		State& Get()
		{
			static State state;
			return state;
		}

		ID3D12Device* NativeDevice(rhi::Device a_device)
		{
			rhi::D3D12DeviceInfo info{};
			return rhi::QueryNativeDevice(a_device, rhi::RHI_IID_D3D12_DEVICE, &info, sizeof(info)) ? static_cast<ID3D12Device*>(info.device) : nullptr;
		}

		Range BufferRange(rhi::Resource a_resource)
		{
			Range range;
			rhi::D3D12BufferRangeInfo info{};
			if (a_resource && rhi::QueryNativeResource(a_resource, rhi::RHI_IID_D3D12_BUFFER_RANGE, &info, sizeof(info)) && info.resource) {
				range.resource.copy_from(static_cast<ID3D12Resource*>(info.resource));
				range.offset = info.offset;
				range.size = info.size;
			}
			return range;
		}

		com_ptr<ID3D12Resource> ReadbackBuffer(ID3D12Device* a_device, std::uint64_t a_bytes, const wchar_t* a_name)
		{
			D3D12_HEAP_PROPERTIES heap{};
			heap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC desc{};
			desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			desc.Width = std::max<std::uint64_t>(a_bytes, 256);
			desc.Height = 1;
			desc.DepthOrArraySize = 1;
			desc.MipLevels = 1;
			desc.SampleDesc.Count = 1;
			desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			com_ptr<ID3D12Resource> buffer;
			if (FAILED(a_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(buffer.put()))))
				return nullptr;
			buffer->SetName(a_name);
			return buffer;
		}

		std::uint32_t PlaneCount(ID3D12Device* a_device, DXGI_FORMAT a_format)
		{
			D3D12_FEATURE_DATA_FORMAT_INFO info{ a_format, 0 };
			return SUCCEEDED(a_device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) && info.PlaneCount ? info.PlaneCount : 1;
		}

		std::uint32_t Subresources(ID3D12Device* a_device, const D3D12_RESOURCE_DESC& a_desc)
		{
			const std::uint32_t layers = a_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1u : a_desc.DepthOrArraySize;
			return std::uint32_t(a_desc.MipLevels) * layers * PlaneCount(a_device, a_desc.Format);
		}

		/** @brief Records a texture's every subresource into a_buffer at its copyable footprints from a_base. Returns the bytes. */
		std::uint64_t CopyTexture(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list, ID3D12Resource* a_texture, ID3D12Resource* a_buffer, std::uint64_t a_base)
		{
			const D3D12_RESOURCE_DESC desc = a_texture->GetDesc();
			const std::uint32_t count = Subresources(a_device, desc);
			std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
			std::uint64_t total = 0;
			a_device->GetCopyableFootprints(&desc, 0, count, a_base, footprints.data(), nullptr, nullptr, &total);
			if (a_list)
				for (std::uint32_t s = 0; s < count; ++s) {
					D3D12_TEXTURE_COPY_LOCATION to{};
					to.pResource = a_buffer;
					to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
					to.PlacedFootprint = footprints[s];
					D3D12_TEXTURE_COPY_LOCATION from{};
					from.pResource = a_texture;
					from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
					from.SubresourceIndex = s;
					a_list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
				}
			return total;
		}

		std::uint64_t TextureBytes(ID3D12Device* a_device, const D3D12_RESOURCE_DESC& a_desc)
		{
			std::uint64_t total = 0;
			a_device->GetCopyableFootprints(&a_desc, 0, Subresources(a_device, a_desc), 0, nullptr, nullptr, nullptr, &total);
			return total;
		}

		std::string ResourceName(ID3D12Resource* a_resource)
		{
			wchar_t name[256]{};
			UINT size = sizeof(name) - sizeof(wchar_t);
			if (FAILED(a_resource->GetPrivateData(kDebugObjectNameW, &size, name)))
				return {};
			return Util::WStringToString(std::wstring(name, size / sizeof(wchar_t)));
		}

		std::string JsonString(std::string_view a_text)
		{
			std::string out = "\"";
			for (const char c : a_text) {
				if (c == '"' || c == '\\')
					out += '\\';
				if (static_cast<unsigned char>(c) < 0x20)
					out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
				else
					out += c;
			}
			return out + "\"";
		}

		std::string Hex(std::uint64_t a_value) { return fmt::format("\"0x{:X}\"", a_value); }

		template <class T>
		std::string JsonArray(const T* a_values, std::size_t a_count)
		{
			std::string out = "[";
			for (std::size_t i = 0; i < a_count; ++i)
				out += fmt::format("{}{}", i ? "," : "", a_values[i]);
			return out + "]";
		}

		std::string SrvDesc(const D3D12_SHADER_RESOURCE_VIEW_DESC& d)
		{
			std::string out = fmt::format(R"({{"viewDimension":{},"format":{},"mapping":{})", int(d.ViewDimension), int(d.Format), d.Shader4ComponentMapping);
			switch (d.ViewDimension) {
			case D3D12_SRV_DIMENSION_BUFFER:
				out += fmt::format(R"(,"firstElement":{},"numElements":{},"structureByteStride":{},"flags":{})", d.Buffer.FirstElement, d.Buffer.NumElements,
					d.Buffer.StructureByteStride, int(d.Buffer.Flags));
				break;
			case D3D12_SRV_DIMENSION_TEXTURE1D:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"resourceMinLODClamp":{})", d.Texture1D.MostDetailedMip, int(d.Texture1D.MipLevels),
					d.Texture1D.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"firstArraySlice":{},"arraySize":{},"resourceMinLODClamp":{})", d.Texture1DArray.MostDetailedMip,
					int(d.Texture1DArray.MipLevels), d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize, d.Texture1DArray.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURE2D:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"planeSlice":{},"resourceMinLODClamp":{})", d.Texture2D.MostDetailedMip, int(d.Texture2D.MipLevels),
					d.Texture2D.PlaneSlice, d.Texture2D.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"firstArraySlice":{},"arraySize":{},"planeSlice":{},"resourceMinLODClamp":{})",
					d.Texture2DArray.MostDetailedMip, int(d.Texture2DArray.MipLevels), d.Texture2DArray.FirstArraySlice, d.Texture2DArray.ArraySize, d.Texture2DArray.PlaneSlice,
					d.Texture2DArray.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY:
				out += fmt::format(R"(,"firstArraySlice":{},"arraySize":{})", d.Texture2DMSArray.FirstArraySlice, d.Texture2DMSArray.ArraySize);
				break;
			case D3D12_SRV_DIMENSION_TEXTURE3D:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"resourceMinLODClamp":{})", d.Texture3D.MostDetailedMip, int(d.Texture3D.MipLevels),
					d.Texture3D.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURECUBE:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"resourceMinLODClamp":{})", d.TextureCube.MostDetailedMip, int(d.TextureCube.MipLevels),
					d.TextureCube.ResourceMinLODClamp);
				break;
			case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
				out += fmt::format(R"(,"mostDetailedMip":{},"mipLevels":{},"first2DArrayFace":{},"numCubes":{},"resourceMinLODClamp":{})", d.TextureCubeArray.MostDetailedMip,
					int(d.TextureCubeArray.MipLevels), d.TextureCubeArray.First2DArrayFace, d.TextureCubeArray.NumCubes, d.TextureCubeArray.ResourceMinLODClamp);
				break;
			default:
				break;
			}
			return out + "}";
		}

		std::string UavDesc(const D3D12_UNORDERED_ACCESS_VIEW_DESC& d)
		{
			std::string out = fmt::format(R"({{"viewDimension":{},"format":{})", int(d.ViewDimension), int(d.Format));
			switch (d.ViewDimension) {
			case D3D12_UAV_DIMENSION_BUFFER:
				out += fmt::format(R"(,"firstElement":{},"numElements":{},"structureByteStride":{},"counterOffsetInBytes":{},"flags":{})", d.Buffer.FirstElement,
					d.Buffer.NumElements, d.Buffer.StructureByteStride, d.Buffer.CounterOffsetInBytes, int(d.Buffer.Flags));
				break;
			case D3D12_UAV_DIMENSION_TEXTURE1D:
				out += fmt::format(R"(,"mipSlice":{})", d.Texture1D.MipSlice);
				break;
			case D3D12_UAV_DIMENSION_TEXTURE1DARRAY:
				out += fmt::format(R"(,"mipSlice":{},"firstArraySlice":{},"arraySize":{})", d.Texture1DArray.MipSlice, d.Texture1DArray.FirstArraySlice, d.Texture1DArray.ArraySize);
				break;
			case D3D12_UAV_DIMENSION_TEXTURE2D:
				out += fmt::format(R"(,"mipSlice":{},"planeSlice":{})", d.Texture2D.MipSlice, d.Texture2D.PlaneSlice);
				break;
			case D3D12_UAV_DIMENSION_TEXTURE2DARRAY:
				out += fmt::format(R"(,"mipSlice":{},"firstArraySlice":{},"arraySize":{},"planeSlice":{})", d.Texture2DArray.MipSlice, d.Texture2DArray.FirstArraySlice,
					d.Texture2DArray.ArraySize, d.Texture2DArray.PlaneSlice);
				break;
			case D3D12_UAV_DIMENSION_TEXTURE3D:
				out += fmt::format(R"(,"mipSlice":{},"firstWSlice":{},"wSize":{})", d.Texture3D.MipSlice, d.Texture3D.FirstWSlice, d.Texture3D.WSize);
				break;
			default:
				break;
			}
			return out + "}";
		}

		std::string SamplerDesc(const D3D12_SAMPLER_DESC& d)
		{
			return fmt::format(
				R"({{"filter":{},"addressU":{},"addressV":{},"addressW":{},"mipLODBias":{},"maxAnisotropy":{},"comparisonFunc":{},"borderColor":{},"minLOD":{},"maxLOD":{}}})",
				int(d.Filter), int(d.AddressU), int(d.AddressV), int(d.AddressW), d.MipLODBias, d.MaxAnisotropy, int(d.ComparisonFunc), JsonArray(d.BorderColor, 4), d.MinLOD,
				d.MaxLOD);
		}

		std::string PipelineState(const rhi::dx12::PipelineCaptureInfo& a_info)
		{
			D3D12_RASTERIZER_DESC r{};
			D3D12_DEPTH_STENCIL_DESC d{};
			D3D12_BLEND_DESC b{};
			std::memcpy(&r, a_info.rasterizer, sizeof(r));
			std::memcpy(&d, a_info.depthStencil, sizeof(d));
			std::memcpy(&b, a_info.blend, sizeof(b));
			const auto face = [](const D3D12_DEPTH_STENCILOP_DESC& f) {
				return fmt::format(R"({{"stencilFailOp":{},"stencilDepthFailOp":{},"stencilPassOp":{},"stencilFunc":{}}})", int(f.StencilFailOp), int(f.StencilDepthFailOp),
					int(f.StencilPassOp), int(f.StencilFunc));
			};
			std::string targets = "[";
			for (std::uint32_t i = 0; i < 8; ++i) {
				const auto& t = b.RenderTarget[i];
				targets += fmt::format(
					R"({}{{"blendEnable":{},"logicOpEnable":{},"srcBlend":{},"destBlend":{},"blendOp":{},"srcBlendAlpha":{},"destBlendAlpha":{},"blendOpAlpha":{},"logicOp":{},"renderTargetWriteMask":{}}})",
					i ? "," : "", t.BlendEnable ? 1 : 0, t.LogicOpEnable ? 1 : 0, int(t.SrcBlend), int(t.DestBlend), int(t.BlendOp), int(t.SrcBlendAlpha), int(t.DestBlendAlpha),
					int(t.BlendOpAlpha), int(t.LogicOp), int(t.RenderTargetWriteMask));
			}
			targets += "]";
			return fmt::format(
				R"({{"rasterizer":{{"fillMode":{},"cullMode":{},"frontCounterClockwise":{},"depthBias":{},"depthBiasClamp":{},"slopeScaledDepthBias":{},"depthClipEnable":{},"multisampleEnable":{},"antialiasedLineEnable":{},"forcedSampleCount":{},"conservativeRaster":{}}},)"
				R"("depthStencil":{{"depthEnable":{},"depthWriteMask":{},"depthFunc":{},"stencilEnable":{},"stencilReadMask":{},"stencilWriteMask":{},"frontFace":{},"backFace":{}}},)"
				R"("blend":{{"alphaToCoverageEnable":{},"independentBlendEnable":{},"renderTarget":{}}},)"
				R"("sampleMask":{},"primitiveTopologyType":{},"primitiveTopology":4,"numRenderTargets":{},"rtvFormats":{},"dsvFormat":{},"sampleCount":{}}})",
				int(r.FillMode), int(r.CullMode), r.FrontCounterClockwise ? 1 : 0, r.DepthBias, r.DepthBiasClamp, r.SlopeScaledDepthBias, r.DepthClipEnable ? 1 : 0,
				r.MultisampleEnable ? 1 : 0, r.AntialiasedLineEnable ? 1 : 0, r.ForcedSampleCount, int(r.ConservativeRaster), d.DepthEnable ? 1 : 0, int(d.DepthWriteMask),
				int(d.DepthFunc), d.StencilEnable ? 1 : 0, d.StencilReadMask, d.StencilWriteMask, face(d.FrontFace), face(d.BackFace), b.AlphaToCoverageEnable ? 1 : 0,
				b.IndependentBlendEnable ? 1 : 0, targets, a_info.sampleMask, a_info.topologyType, a_info.rtvCount, JsonArray(a_info.rtvFormats, 8), a_info.dsvFormat,
				a_info.sampleCount);
		}

		/** @brief The frame end's raw D3D12 copies: its own direct queue, list and fence, made once. */
		struct Copier
		{
			com_ptr<ID3D12CommandQueue> queue;
			com_ptr<ID3D12CommandAllocator> allocator;
			com_ptr<ID3D12GraphicsCommandList> list;
			com_ptr<ID3D12Fence> fence;
			std::uint64_t value = 0;
			HANDLE event = nullptr;

			~Copier()
			{
				if (event)
					::CloseHandle(event);
			}

			bool Ready(ID3D12Device* a_device)
			{
				if (list)
					return true;
				D3D12_COMMAND_QUEUE_DESC desc{};
				desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
				if (FAILED(a_device->CreateCommandQueue(&desc, IID_PPV_ARGS(queue.put()))) ||
					FAILED(a_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(allocator.put()))) ||
					FAILED(a_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.get(), nullptr, IID_PPV_ARGS(list.put()))) ||
					FAILED(a_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()))))
					return false;
				queue->SetName(L"DCLF::ReplayCapture copy queue");
				list->SetName(L"DCLF::ReplayCapture copies");
				list->Close();
				event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
				return event != nullptr;
			}
			/** @brief Opens the list for a batch. */
			ID3D12GraphicsCommandList* Begin()
			{
				allocator->Reset();
				list->Reset(allocator.get(), nullptr);
				return list.get();
			}
			/** @brief Runs the batch and waits for it. */
			bool Finish()
			{
				if (FAILED(list->Close()))
					return false;
				ID3D12CommandList* lists[] = { list.get() };
				queue->ExecuteCommandLists(1, lists);
				queue->Signal(fence.get(), ++value);
				if (fence->GetCompletedValue() < value) {
					fence->SetEventOnCompletion(value, event);
					if (::WaitForSingleObject(event, 20000) != WAIT_OBJECT_0)
						return false;
				}
				return true;
			}
		};

		const char* FileStem(const std::string& a_pass) { return a_pass == "main opaque" ? "main" : a_pass == "Z-prepass" ? "z" : "z2"; }

		/** @brief One frame end's dump. */
		class Dump
		{
		public:
			Dump(rhi::Device a_rhiDevice, ID3D12Device* a_device, std::map<std::string, Recorded>& a_passes, std::filesystem::path a_directory) :
				rhiDevice(a_rhiDevice), device(a_device), passes(a_passes), directory(std::move(a_directory)) {}

			bool Write(std::uint64_t a_frame, std::string& a_summary);

		private:
			struct Resource
			{
				com_ptr<ID3D12Resource> native;
				D3D12_RESOURCE_DESC desc{};
				std::uint32_t id = 0;
				std::uint64_t bytes = 0;  // the file's size
				bool read = false;
			};
			struct View
			{
				const char* type = "srv";
				std::int64_t resource = -1;
				std::string desc;
			};
			struct Upload
			{
				std::uint32_t resource = 0;
				std::uint64_t offset = 0;
				std::string file;
				std::vector<std::uint64_t> pointers;
			};

			std::uint32_t AddResource(ID3D12Resource* a_resource)
			{
				if (const auto found = ids.find(a_resource); found != ids.end())
					return found->second;
				Resource entry;
				entry.native.copy_from(a_resource);
				entry.desc = a_resource->GetDesc();
				entry.id = static_cast<std::uint32_t>(resources.size());
				ids.emplace(a_resource, entry.id);
				if (entry.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
					spans.push_back({ a_resource->GetGPUVirtualAddress(), entry.desc.Width, entry.id });
				resources.push_back(std::move(entry));
				return resources.back().id;
			}

			/** @brief A resource slot and, for a buffer view ref, the raw UAV beside it. */
			void AddSlot(std::uint32_t a_slot, bool a_pair = false)
			{
				if (a_slot == 0 || a_slot == ~0u)
					return;
				for (std::uint32_t slot = a_slot; slot <= a_slot + (a_pair ? 1u : 0u); ++slot) {
					if (views.contains(slot) || missing.contains(slot))
						continue;
					rhi::dx12::DescriptorSlotInfo info{};
					if (!rhi::dx12::DescribeDescriptorSlot(rhiDevice, resourceHeap, slot, info) || info.kind == rhi::dx12::DescriptorSlotKind::Sampler) {
						if (info.resource)
							static_cast<ID3D12Resource*>(info.resource)->Release();
						missing.insert(slot);
						continue;
					}
					com_ptr<ID3D12Resource> resource;
					resource.attach(static_cast<ID3D12Resource*>(info.resource));
					View view;
					view.resource = resource ? std::int64_t(AddResource(resource.get())) : std::int64_t(-1);
					switch (info.kind) {
					case rhi::dx12::DescriptorSlotKind::Srv:
						view.type = "srv";
						view.desc = SrvDesc(*reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(info.desc));
						break;
					case rhi::dx12::DescriptorSlotKind::Uav:
						view.type = "uav";
						view.desc = UavDesc(*reinterpret_cast<const D3D12_UNORDERED_ACCESS_VIEW_DESC*>(info.desc));
						break;
					default:
						{
							const auto& cbv = *reinterpret_cast<const D3D12_CONSTANT_BUFFER_VIEW_DESC*>(info.desc);
							view.type = "cbv";
							const std::uint64_t base = resource ? resource->GetGPUVirtualAddress() : 0;
							view.desc = fmt::format(R"({{"offset":{},"sizeInBytes":{},"bufferLocation":{}}})", cbv.BufferLocation - base, cbv.SizeInBytes, Hex(cbv.BufferLocation));
						}
						break;
					}
					views.emplace(slot, std::move(view));
				}
			}

			void AddSampler(std::uint32_t a_slot)
			{
				if (a_slot == ~0u || samplers.contains(a_slot) || missingSamplers.contains(a_slot))
					return;
				rhi::dx12::DescriptorSlotInfo info{};
				if (!rhi::dx12::DescribeDescriptorSlot(rhiDevice, samplerHeap, a_slot, info) || info.kind != rhi::dx12::DescriptorSlotKind::Sampler) {
					if (info.resource)
						static_cast<ID3D12Resource*>(info.resource)->Release();
					missingSamplers.insert(a_slot);
					return;
				}
				samplers.emplace(a_slot, SamplerDesc(*reinterpret_cast<const D3D12_SAMPLER_DESC*>(info.desc)));
			}

			/** @brief The buffer and offset a device address points into, or false. */
			bool Resolve(std::uint64_t a_address, std::uint32_t& a_resource, std::uint64_t& a_offset) const
			{
				for (const auto& span : spans)
					if (a_address >= span.address && a_address < span.address + span.bytes) {
						a_resource = span.id;
						a_offset = a_address - span.address;
						return true;
					}
				return false;
			}

			/**
			 * @brief Adds the buffer a device address points into, found among every live buffer when no listed one holds it.
			 * False when none does.
			 */
			bool AddAddress(std::uint64_t a_address)
			{
				std::uint32_t id = 0;
				std::uint64_t offset = 0;
				if (Resolve(a_address, id, offset))
					return true;
				if (!enumerated) {
					enumerated = true;
					rhi::dx12::EnumerateBufferResources(rhiDevice, allBuffers);
					std::ranges::sort(allBuffers, {}, &rhi::dx12::BufferResourceInfo::address);
				}
				auto next = std::ranges::upper_bound(allBuffers, a_address, {}, &rhi::dx12::BufferResourceInfo::address);
				if (next == allBuffers.begin())
					return false;
				const auto& found = *std::prev(next);
				if (a_address >= found.address + found.bytes)
					return false;
				AddResource(static_cast<ID3D12Resource*>(found.resource));
				return true;
			}

			/** @brief Copies every resource not read yet to its file, in batches. */
			bool ReadPending();

			/** @brief A resource's file contents as read, or null. */
			const std::vector<std::uint8_t>* Contents(std::uint32_t a_id) const
			{
				const auto found = contents.find(a_id);
				return found == contents.end() ? nullptr : &found->second;
			}

			rhi::Device rhiDevice;
			ID3D12Device* device;
			std::map<std::string, Recorded>& passes;
			std::filesystem::path directory;
			rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
			Copier copier;

			std::vector<Resource> resources;
			std::unordered_map<ID3D12Resource*, std::uint32_t> ids;
			struct Span
			{
				std::uint64_t address = 0, bytes = 0;
				std::uint32_t id = 0;
			};
			std::vector<Span> spans;
			std::map<std::uint32_t, View> views;
			std::set<std::uint32_t> missing;
			std::map<std::uint32_t, std::string> samplers;
			std::set<std::uint32_t> missingSamplers;
			// Buffers whose rows the walk reads after the first readback.
			std::unordered_map<std::uint32_t, std::vector<std::uint8_t>> contents;
			std::set<std::uint32_t> keepContents;
			std::uint64_t writtenBytes = 0;
			std::uint32_t unresolvedPointers = 0;
			std::uint32_t unreadRows = 0;  // rows whose header could not be read
			// Every live buffer, by address, for addresses no listed buffer holds. Each holds a reference.
			std::vector<rhi::dx12::BufferResourceInfo> allBuffers;
			bool enumerated = false;

		public:
			~Dump()
			{
				for (const auto& buffer : allBuffers)
					static_cast<ID3D12Resource*>(buffer.resource)->Release();
			}
		};

		bool Dump::ReadPending()
		{
			if (!copier.Ready(device))
				return false;
			std::vector<std::uint32_t> pending;
			for (auto& resource : resources)
				if (!resource.read) {
					resource.read = true;
					if (resource.desc.SampleDesc.Count > 1) {
						logger::warn("[DCLF] pass capture: resource {} ({}) is multisampled; its contents are not dumped", resource.id, ResourceName(resource.native.get()));
						continue;
					}
					resource.bytes = resource.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? resource.desc.Width : TextureBytes(device, resource.desc);
					pending.push_back(resource.id);
				}
			std::size_t next = 0;
			while (next < pending.size()) {
				// One batch: resources up to kBatchBytes, at least one.
				std::vector<std::pair<std::uint32_t, std::uint64_t>> batch;
				std::uint64_t bytes = 0;
				while (next < pending.size()) {
					const auto& resource = resources[pending[next]];
					const std::uint64_t at = Align(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
					if (!batch.empty() && at + resource.bytes > kBatchBytes)
						break;
					batch.emplace_back(resource.id, at);
					bytes = at + resource.bytes;
					++next;
				}
				auto readback = ReadbackBuffer(device, bytes, L"DCLF::ReplayCapture frame-end readback");
				if (!readback) {
					logger::error("[DCLF] pass capture: no readback buffer of {} MiB", bytes >> 20);
					return false;
				}
				auto* list = copier.Begin();
				// Textures are copied in the layout the frame left them in, which NVIDIA's copies do not depend on.
				for (const auto& [id, at] : batch) {
					auto& resource = resources[id];
					if (resource.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
						list->CopyBufferRegion(readback.get(), at, resource.native.get(), 0, resource.desc.Width);
					else
						CopyTexture(device, list, resource.native.get(), readback.get(), at);
				}
				if (!copier.Finish()) {
					logger::error("[DCLF] pass capture: the frame-end copies did not finish");
					return false;
				}
				void* mapped = nullptr;
				const D3D12_RANGE whole{ 0, static_cast<SIZE_T>(bytes) };
				if (FAILED(readback->Map(0, &whole, &mapped)) || !mapped)
					return false;
				for (const auto& [id, at] : batch) {
					const auto& resource = resources[id];
					const auto* data = static_cast<const std::uint8_t*>(mapped) + at;
					std::ofstream out(directory / "res" / fmt::format("{}.bin", id), std::ios::binary | std::ios::trunc);
					out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(resource.bytes));
					writtenBytes += resource.bytes;
					if (resource.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && keepContents.contains(id))
						contents[id].assign(data, data + resource.bytes);
				}
				const D3D12_RANGE none{ 0, 0 };
				readback->Unmap(0, &none);
			}
			return true;
		}

		bool Dump::Write(std::uint64_t a_frame, std::string& a_summary)
		{
			std::error_code error;
			std::filesystem::create_directories(directory / "res", error);
			if (error) {
				a_summary = fmt::format("cannot make {}: {}", directory.string(), error.message());
				return false;
			}
			// The passes in draw order, each with its mapped readback.
			struct Mapped
			{
				Recorded* recorded = nullptr;
				const std::uint8_t* data = nullptr;
				std::string name;
				std::vector<Upload> uploads;
				std::vector<std::uint32_t> counts;  // per bucket
			};
			std::vector<Mapped> mapped;
			for (const char* name : kPassOrder)
				if (const auto found = passes.find(name); found != passes.end()) {
					Mapped pass;
					pass.recorded = &found->second;
					pass.name = name;
					void* data = nullptr;
					if (FAILED(found->second.readback->Map(0, nullptr, &data)) || !data)
						continue;
					pass.data = static_cast<const std::uint8_t*>(data);
					mapped.push_back(std::move(pass));
				}
			if (mapped.empty()) {
				a_summary = "no pass readback could be mapped";
				return false;
			}
			resourceHeap = mapped.front().recorded->pass.resourceHeap;
			samplerHeap = mapped.front().recorded->pass.samplerHeap;

			// Pass 1: the passes' buffers, frame records and sequences.
			std::set<std::uint64_t> materialRows, pipelineRows;
			for (auto& pass : mapped) {
				auto& r = *pass.recorded;
				const std::uint32_t constantsId = AddResource(r.frameConstants.resource.get());
				const std::uint32_t sequencesId = AddResource(r.sequences.resource.get());
				const std::uint32_t countsId = AddResource(r.counts.resource.get());
				if (r.indexPool.resource)
					AddResource(r.indexPool.resource.get());
				AddSlot(r.pass.frameWords[kFramePushRecordRef], true);
				// The record address is the frame words' only pointer.
				if (const std::uint64_t address = r.pass.frameWords[kFramePushRecord] | (std::uint64_t(r.pass.frameWords[kFramePushRecord + 1]) << 32);
					address && !AddAddress(address))
					++unresolvedPointers;

				// The frame record, from the pass's copy of the frame constants.
				Upload constants{ constantsId, r.frameConstants.offset, fmt::format("res/{}-constants.bin", FileStem(pass.name)), {} };
				const auto* record = reinterpret_cast<const DrawBindings*>(pass.data + r.frameConstants.copyOffset + r.pass.recordOffset);
				const auto pointer = [&](Upload& a_upload, std::uint64_t a_at, std::uint64_t a_value) {
					a_upload.pointers.push_back(a_at);
					if (!AddAddress(a_value))
						++unresolvedPointers;
				};
				for (std::uint32_t i = 0; i < kConstantBufferRegisters; ++i) {
					if (record->vertexConstants[i])
						pointer(constants, r.pass.recordOffset + offsetof(DrawBindings, vertexConstants) + 8 * i, record->vertexConstants[i]);
					if (record->pixelConstants[i])
						pointer(constants, r.pass.recordOffset + offsetof(DrawBindings, pixelConstants) + 8 * i, record->pixelConstants[i]);
				}
				for (const auto texture : record->textures)
					AddSlot(texture);
				for (const auto sampler : record->samplers)
					AddSampler(sampler);
				for (const auto block : record->frameBlocks)
					AddSlot(block);
				for (std::uint32_t b = 0; b < kRecordBackings; ++b) {
					const auto& backing = record->backings[b];
					AddSlot(backing.ref.viewSlot, true);
					if (backing.address)
						pointer(constants, r.pass.recordOffset + offsetof(DrawBindings, backings) + sizeof(RecordBacking) * b + offsetof(RecordBacking, address), backing.address);
				}
				pass.uploads.push_back(std::move(constants));

				// The buckets' sequences, from the pass's copy.
				Upload sequences{ sequencesId, r.sequences.offset, fmt::format("res/{}-sequences.bin", FileStem(pass.name)), {} };
				const auto* countWords = reinterpret_cast<const std::uint32_t*>(pass.data + r.counts.copyOffset);
				const std::uint64_t countWordsHeld = r.counts.size / 4;
				for (const auto& bucket : r.pass.buckets) {
					AddSlot(bucket.drawWords[kZDrawPushSequences], true);
					const std::uint32_t count = bucket.countWord < countWordsHeld ? std::min(countWords[bucket.countWord], bucket.capacity) : 0u;
					pass.counts.push_back(count);
					for (std::uint64_t s = bucket.first; s < bucket.first + count; ++s) {
						const std::uint64_t at = s * sizeof(DrawSequence);
						if (at + sizeof(DrawSequence) > r.sequences.size)
							break;
						DrawSequence sequence;
						std::memcpy(&sequence, pass.data + r.sequences.copyOffset + at, sizeof(sequence));
						for (const std::uint64_t field : { offsetof(DrawSequence, pipelineRowAddress), offsetof(DrawSequence, materialRowAddress),
								 offsetof(DrawSequence, psTechniqueAddress), offsetof(DrawSequence, vsGeometryAddress), offsetof(DrawSequence, psGeometryAddress),
								 offsetof(DrawSequence, permutationAddress), offsetof(DrawSequence, psMaterialAddress), offsetof(DrawSequence, indexBufferAddress) }) {
							std::uint64_t value = 0;
							std::memcpy(&value, reinterpret_cast<const std::uint8_t*>(&sequence) + field, 8);
							if (value)
								pointer(sequences, at + field, value);
						}
						AddSlot(sequence.vertexBufferRef.viewSlot, true);
						AddSlot(sequence.streamBufferRef.viewSlot, true);
						if (sequence.materialRowAddress)
							materialRows.insert(sequence.materialRowAddress);
						if (sequence.pipelineRowAddress)
							pipelineRows.insert(sequence.pipelineRowAddress);
					}
				}
				pass.uploads.push_back(std::move(sequences));
				pass.uploads.push_back({ countsId, r.counts.offset, fmt::format("res/{}-counts.bin", FileStem(pass.name)), {} });
			}
			// The rows' buffers are read back first, so their headers can be walked.
			for (const auto* rows : { &materialRows, &pipelineRows })
				for (const auto address : *rows) {
					std::uint32_t id = 0;
					std::uint64_t offset = 0;
					if (Resolve(address, id, offset))
						keepContents.insert(id);
				}
			if (!ReadPending()) {
				a_summary = "the first readback failed";
				return false;
			}

			// Pass 2: the rows' headers name textures, samplers, the feedback buffer and the shadow mask.
			const auto rowBytes = [&](std::uint64_t a_address, std::uint64_t a_at, std::size_t a_size) -> const std::uint8_t* {
				std::uint32_t id = 0;
				std::uint64_t offset = 0;
				if (!Resolve(a_address, id, offset))
					return nullptr;
				const auto* data = Contents(id);
				return data && offset + a_at + a_size <= data->size() ? data->data() + offset + a_at : nullptr;
			};
			for (const auto address : materialRows) {
				const auto* bytes = rowBytes(address, kMaterialRowHeader, sizeof(MaterialRowHeader));
				if (!bytes) {
					++unreadRows;
					continue;
				}
				MaterialRowHeader header;
				std::memcpy(&header, bytes, sizeof(header));
				AddSlot(header.feedbackRef.viewSlot, true);
				for (const auto texture : header.textures)
					AddSlot(texture);
				for (const auto sampler : header.samplers)
					AddSampler(sampler);
				for (const auto texture : header.features)
					AddSlot(texture);
			}
			for (const auto address : pipelineRows) {
				const auto* bytes = rowBytes(address, kPipelineRowHeader, sizeof(PipelineRowHeader));
				if (!bytes) {
					++unreadRows;
					continue;
				}
				PipelineRowHeader header;
				std::memcpy(&header, bytes, sizeof(header));
				AddSlot(header.shadowMask);
				AddSampler(header.shadowMaskSampler);
			}
			if (!ReadPending()) {
				a_summary = "the second readback failed";
				return false;
			}

			// The passes' uploads, depth and colour targets, from their readbacks.
			for (auto& pass : mapped) {
				auto& r = *pass.recorded;
				const auto index = &pass - mapped.data();
				const auto write = [&](const std::string& a_file, std::uint64_t a_at, std::uint64_t a_bytes) {
					std::ofstream out(directory / a_file, std::ios::binary | std::ios::trunc);
					out.write(reinterpret_cast<const char*>(pass.data + a_at), static_cast<std::streamsize>(a_bytes));
					writtenBytes += a_bytes;
				};
				write(pass.uploads[0].file, r.frameConstants.copyOffset, r.frameConstants.size);
				write(pass.uploads[1].file, r.sequences.copyOffset, r.sequences.size);
				write(pass.uploads[2].file, r.counts.copyOffset, r.counts.size);
				if (r.depth)
					write(fmt::format("res/depth-{}.bin", index), r.depthOffset, r.depthBytes);
				for (std::uint32_t i = 0; i < 8; ++i)
					if (r.colours[i])
						write(fmt::format("res/colour-{}-{}.bin", index, i), r.colourOffsets[i], r.colourBytes[i]);
			}

			// Pipelines: each bucket's DXIL, written once per pipeline.
			std::unordered_map<std::uint64_t, std::string> pipelineJson;
			std::uint32_t pipelineFiles = 0, missingPipelines = 0;
			std::string firstColourTargets, depthTarget;
			for (auto& pass : mapped)
				for (const auto& bucket : pass.recorded->pass.buckets) {
					const std::uint64_t key = HandleKey(bucket.pipeline);
					if (pipelineJson.contains(key))
						continue;
					rhi::dx12::PipelineCaptureInfo info;
					if (!rhi::dx12::DescribePipeline(rhiDevice, bucket.pipeline, info)) {
						++missingPipelines;
						pipelineJson[key] = "null";
						continue;
					}
					const std::uint32_t n = pipelineFiles++;
					const auto writeStage = [&](const std::vector<std::uint8_t>& a_bytes, const char* a_stage) -> std::string {
						if (a_bytes.empty())
							return "null";
						const std::string file = fmt::format("res/pso{}.{}.dxil", n, a_stage);
						std::ofstream out(directory / file, std::ios::binary | std::ios::trunc);
						out.write(reinterpret_cast<const char*>(a_bytes.data()), static_cast<std::streamsize>(a_bytes.size()));
						writtenBytes += a_bytes.size();
						return JsonString(file);
					};
					PipelineNote note;
					std::string vertexLine = "null", pixelLine = "null";
					{
						std::lock_guard lock(notesMutex);
						if (const auto found = pipelineNotes.find(key); found != pipelineNotes.end())
							note = found->second;
						if (const auto found = stageLines.find(note.vertexStage); !note.vertexStage.empty() && found != stageLines.end())
							vertexLine = found->second;
						if (const auto found = stageLines.find(note.pixelStage); !note.pixelStage.empty() && found != stageLines.end())
							pixelLine = found->second;
					}
					pipelineJson[key] = fmt::format(R"({{"key":{},"vs":{},"ps":{},"vsStage":{},"psStage":{},"state":{}}})", JsonString(note.key), writeStage(info.vs, "vs"),
						writeStage(info.ps, "ps"), vertexLine, pixelLine, PipelineState(info));
					if (pass.name == "main opaque" && firstColourTargets.empty()) {
						firstColourTargets = "[";
						for (std::uint32_t i = 0; i < 8; ++i) {
							const auto& target = pass.recorded->colours[i];
							if (i < info.rtvCount && target) {
								const auto desc = target->GetDesc();
								firstColourTargets += fmt::format(R"({}{{"format":{},"width":{},"height":{}}})", i ? "," : "", info.rtvFormats[i], desc.Width, desc.Height);
							} else if (i < info.rtvCount)
								firstColourTargets += fmt::format(R"({}{{"format":{}}})", i ? "," : "", info.rtvFormats[i]);
							else
								firstColourTargets += fmt::format("{}null", i ? "," : "");
						}
						firstColourTargets += "]";
					}
					if (depthTarget.empty() && info.dsvFormat && pass.recorded->depth)
						depthTarget = fmt::format(R"({{"format":{},"resourceFormat":{},"width":{},"height":{}}})", info.dsvFormat, int(pass.recorded->depthDesc.Format),
							pass.recorded->depthDesc.Width, pass.recorded->depthDesc.Height);
				}

			// The manifest.
			std::string adapter;
			{
				rhi::D3D12DeviceInfo info{};
				if (rhi::QueryNativeDevice(rhiDevice, rhi::RHI_IID_D3D12_DEVICE, &info, sizeof(info)) && info.adapter) {
					com_ptr<IDXGIAdapter1> dxgi;
					if (SUCCEEDED(static_cast<IUnknown*>(info.adapter)->QueryInterface(IID_PPV_ARGS(dxgi.put())))) {
						DXGI_ADAPTER_DESC1 desc{};
						if (SUCCEEDED(dxgi->GetDesc1(&desc)))
							adapter = Util::WStringToString(desc.Description);
					}
				}
			}
			const auto& first = *mapped.front().recorded;
			std::string out;
			out += fmt::format("{{\n\"version\":1,\n\"frame\":{},\n\"adapter\":{},\n\"resourceHeapSlots\":1000000,\"samplerHeapSlots\":2048,\n", a_frame, JsonString(adapter));
			{
				std::lock_guard lock(notesMutex);
				if (!optionsLine.empty())
					out += fmt::format("\"stageOptions\":{},\n", optionsLine);
			}
			out += "\"resources\":[\n";
			for (const auto& resource : resources) {
				const auto& d = resource.desc;
				const std::string name = JsonString(ResourceName(resource.native.get()));
				const std::string file = resource.bytes ? JsonString(fmt::format("res/{}.bin", resource.id)) : "null";
				if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
					out += fmt::format(R"({{"id":{},"kind":"buffer","name":{},"bytes":{},"gpuVA":{},"file":{},"uav":{}}})", resource.id, name, d.Width,
						Hex(resource.native->GetGPUVirtualAddress()), file, (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? "true" : "false");
				else
					out += fmt::format(
						R"({{"id":{},"kind":"texture","name":{},"dimension":{},"format":{},"width":{},"height":{},"depthOrArraySize":{},"mips":{},"flags":{},"sampleCount":{},"file":{}}})",
						resource.id, name, int(d.Dimension), int(d.Format), d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, int(d.Flags), d.SampleDesc.Count, file);
				out += &resource == &resources.back() ? "\n" : ",\n";
			}
			out += "],\n\"views\":[\n";
			std::size_t index = 0;
			for (const auto& [slot, view] : views) {
				out += fmt::format(R"({{"slot":{},"type":"{}","resource":{},"desc":{}}})", slot, view.type, view.resource < 0 ? std::string("null") : std::to_string(view.resource),
					view.desc);
				out += ++index == views.size() ? "\n" : ",\n";
			}
			out += "],\n\"samplers\":[\n";
			index = 0;
			for (const auto& [slot, desc] : samplers) {
				out += fmt::format(R"({{"slot":{},"desc":{}}})", slot, desc);
				out += ++index == samplers.size() ? "\n" : ",\n";
			}
			out += "],\n\"pointers\":[],\n";
			// With bound frame blocks, a CBV descriptor table after the constants: register(b0.., space9) from the heap slot in frame word kFramePushBlocks.
			// One per bound space (FrameSource kBoundBlockSpaces), all over the same slots.
			std::string tables;
			if (BoundFrameBlocks()) {
				for (std::uint32_t s = 0; s < FrameSource::kBoundBlockSpaces; ++s)
					tables += fmt::format("{}{{\"space\":{},\"register\":0,\"count\":{},\"frameWord\":{}}}", s ? "," : "", FrameSource::kBoundBlockSpace + s, kFrameBlocks,
						kFramePushBlocks);
				tables = ",\"cbvTables\":[" + tables + "]";
			}
			out += fmt::format("\"rootSignature\":{{\"drawWords\":{},\"drawRegister\":{},\"frameWords\":{},\"frameRegister\":{}{}}},\n", kDrawPushWords, kDrawPushBinding,
				kFramePushWords, kFramePushBinding, tables);
			out += fmt::format("\"boundFrameBlocks\":{},\n", BoundFrameBlocks() ? "true" : "false");
			out += fmt::format("\"commandSignature\":{{\"stride\":{},\"instanceConstantWord\":{}}},\n", sizeof(DrawSequence),
				first.pass.instanceWord ? std::to_string(kDrawPushInstance) : std::string("null"));
			if (first.indexPool.resource)
				out += fmt::format("\"indexBuffer\":{{\"resource\":{},\"offset\":{},\"format\":{}}},\n", ids.at(first.indexPool.resource.get()), first.indexPool.offset,
					int(DXGI_FORMAT_R16_UINT));
			out += fmt::format("\"viewport\":{{\"width\":{},\"height\":{},\"minDepth\":{},\"maxDepth\":{}}},\n", first.pass.width, first.pass.height, first.pass.minDepth,
				first.pass.maxDepth);
			out += fmt::format("\"targets\":{{\"colour\":{},\"depth\":{}}},\n", firstColourTargets.empty() ? "[null,null,null,null,null,null,null,null]" : firstColourTargets,
				depthTarget.empty() ? "null" : depthTarget);
			out += "\"passes\":[\n";
			std::uint32_t bucketIndex = 0, pointerCount = 0, drawn = 0;
			for (auto& pass : mapped) {
				const auto& r = *pass.recorded;
				const auto passIndex = &pass - mapped.data();
				std::vector<std::uint32_t> framePointerWords;
				if (r.pass.frameWords[kFramePushRecord] || r.pass.frameWords[kFramePushRecord + 1])
					framePointerWords.push_back(kFramePushRecord);
				out += fmt::format("{{\"name\":{},\n\"depthBefore\":{},\n\"frameWords\":{},\n\"framePointerWords\":{},\n", JsonString(pass.name),
					r.depth ? JsonString(fmt::format("res/depth-{}.bin", passIndex)) : "null", JsonArray(r.pass.frameWords.data(), r.pass.frameWords.size()),
					JsonArray(framePointerWords.data(), framePointerWords.size()));
				std::string colourBefore = "[";
				bool anyColour = false;
				for (std::uint32_t i = 0; i < 8; ++i) {
					anyColour = anyColour || r.colours[i];
					colourBefore += fmt::format("{}{}", i ? "," : "", r.colours[i] ? JsonString(fmt::format("res/colour-{}-{}.bin", passIndex, i)) : "null");
				}
				colourBefore += "]";
				if (anyColour)
					out += fmt::format("\"colourBefore\":{},\n", colourBefore);
				out += "\"uploads\":[\n";
				for (const auto& upload : pass.uploads) {
					pointerCount += static_cast<std::uint32_t>(upload.pointers.size());
					out += fmt::format(R"({{"resource":{},"offset":{},"file":{},"pointers":{}}})", upload.resource, upload.offset, JsonString(upload.file),
						JsonArray(upload.pointers.data(), upload.pointers.size()));
					out += &upload == &pass.uploads.back() ? "\n" : ",\n";
				}
				out += "],\n\"buckets\":[\n";
				const std::uint32_t sequencesId = ids.at(r.sequences.resource.get());
				const std::uint32_t countsId = ids.at(r.counts.resource.get());
				for (std::size_t b = 0; b < r.pass.buckets.size(); ++b) {
					const auto& bucket = r.pass.buckets[b];
					drawn += pass.counts[b];
					out += fmt::format(
						R"({{"index":{},"set":{},"first":{},"capacity":{},"count":{},"drawWords":{},"drawPointerWords":[],"argsResource":{},"argsOffset":{},"countResource":{},"countOffset":{},"pipeline":{}}})",
						bucketIndex++, bucket.set == ~0u ? std::string("null") : std::to_string(bucket.set), bucket.first, bucket.capacity, pass.counts[b],
						JsonArray(bucket.drawWords.data(), bucket.drawWords.size()), sequencesId,
						r.sequences.offset + bucket.first * sizeof(DrawSequence) + r.pass.argumentsOffset, countsId,
						r.counts.offset + std::uint64_t(bucket.countWord) * 4, pipelineJson[HandleKey(bucket.pipeline)]);
					out += b + 1 == r.pass.buckets.size() ? "\n" : ",\n";
				}
				out += "]}";
				out += &pass == &mapped.back() ? "\n" : ",\n";
			}
			out += "]\n}\n";
			{
				std::ofstream manifest(directory / "manifest.json", std::ios::binary | std::ios::trunc);
				manifest.write(out.data(), static_cast<std::streamsize>(out.size()));
				writtenBytes += out.size();
			}
			for (auto& pass : mapped)
				pass.recorded->readback->Unmap(0, nullptr);
			a_summary = fmt::format("{} passes, {} buckets ({} draws), {} resources, {} views ({} slots not recorded), {} samplers ({} not recorded), {} pipelines ({} not recorded), "
									"{} pointers ({} in no buffer), {} rows unread, {:.1f} MiB",
				mapped.size(), bucketIndex, drawn, resources.size(), views.size(), missing.size(), samplers.size(), missingSamplers.size(), pipelineFiles, missingPipelines,
				pointerCount, unresolvedPointers, unreadRows, writtenBytes / 1048576.0);
			return true;
		}
	}

	bool Arm(std::string& a_error)
	{
		if (!Enabled()) {
			a_error = "the pass capture needs CS_DCLF_CAPTURE=<dir> and native D3D12";
			return false;
		}
		auto& state = Get();
		std::lock_guard lock(state.mutex);
		state.armed = true;
		state.markersCleared = false;
		state.framesWaited = 0;
		logger::info("[DCLF] pass capture: armed");
		return true;
	}

	bool Wanted(const char*)
	{
		// Every recording copies, because a kept recording replays without recording again.
		return Enabled();
	}

	void RecordPass(rhi::CommandList& a_commands, Pass a_pass)
	{
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return;
		a_pass.device = host->Device();
		auto* device = NativeDevice(a_pass.device);
		rhi::D3D12CmdListInfo listInfo{};
		if (!device || !rhi::QueryNativeCmdList(a_commands, rhi::RHI_IID_D3D12_CMD_LIST, &listInfo, sizeof(listInfo)) || !listInfo.cmdList) {
			logger::warn("[DCLF] pass capture: {} has no native command list; not captured", a_pass.name);
			return;
		}
		auto* list = static_cast<ID3D12GraphicsCommandList*>(listInfo.cmdList);
		com_ptr<ID3D12GraphicsCommandList2> list2;
		list->QueryInterface(IID_PPV_ARGS(list2.put()));

		Recorded recorded;
		recorded.sequences = BufferRange(a_pass.sequences);
		recorded.counts = BufferRange(a_pass.counts);
		recorded.indexPool = BufferRange(a_pass.indexPool);
		// The frame constants hold the record at recordOffset. The record's ref names the buffer's raw view, which starts where the buffer does.
		{
			rhi::dx12::DescriptorSlotInfo info{};
			if (rhi::dx12::DescribeDescriptorSlot(a_pass.device, a_pass.resourceHeap, a_pass.frameWords[kFramePushRecordRef], info) && info.resource) {
				recorded.frameConstants.resource.attach(static_cast<ID3D12Resource*>(info.resource));
				const auto& srv = *reinterpret_cast<const D3D12_SHADER_RESOURCE_VIEW_DESC*>(info.desc);
				recorded.frameConstants.offset = srv.Buffer.FirstElement * 4 + a_pass.frameWords[kFramePushRecordRef + 1] - a_pass.recordOffset;
				recorded.frameConstants.size = a_pass.frameConstantsBytes;
			}
		}
		if (!recorded.sequences.resource || !recorded.counts.resource || !recorded.frameConstants.resource || !list2) {
			logger::warn("[DCLF] pass capture: {} has no sequences, counts or frame constants to copy; not captured", a_pass.name);
			return;
		}
		if (a_pass.depth) {
			rhi::D3D12ResourceInfo info{};
			if (rhi::QueryNativeResource(a_pass.depth, rhi::RHI_IID_D3D12_RESOURCE, &info, sizeof(info)) && info.resource) {
				recorded.depth.copy_from(static_cast<ID3D12Resource*>(info.resource));
				recorded.depthDesc = recorded.depth->GetDesc();
			}
		}
		for (std::uint32_t i = 0; i < a_pass.colourCount && i < 8; ++i) {
			rhi::D3D12ResourceInfo info{};
			if (a_pass.colours[i] && rhi::QueryNativeResource(a_pass.colours[i], rhi::RHI_IID_D3D12_RESOURCE, &info, sizeof(info)) && info.resource)
				recorded.colours[i].copy_from(static_cast<ID3D12Resource*>(info.resource));
		}
		// The readback's layout: marker, frame constants, sequences, counts, depth, colours.
		std::uint64_t at = kHeaderBytes;
		for (auto* range : { &recorded.frameConstants, &recorded.sequences, &recorded.counts }) {
			range->copyOffset = at;
			at = Align(at + range->size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		}
		if (recorded.depth) {
			recorded.depthOffset = at;
			recorded.depthBytes = TextureBytes(device, recorded.depthDesc);
			at = Align(at + recorded.depthBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		}
		for (std::uint32_t i = 0; i < 8; ++i)
			if (recorded.colours[i]) {
				recorded.colourOffsets[i] = at;
				recorded.colourBytes[i] = TextureBytes(device, recorded.colours[i]->GetDesc());
				at = Align(at + recorded.colourBytes[i], D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
			}

		auto& state = Get();
		std::lock_guard lock(state.mutex);
		auto& readback = state.readbacks[a_pass.name];
		if (!readback || readback->GetDesc().Width < at) {
			if (readback)
				state.retired.push_back(readback);
			readback = ReadbackBuffer(device, at, L"DCLF::ReplayCapture pass readback");
			if (!readback) {
				logger::error("[DCLF] pass capture: no readback of {} MiB for {}", at >> 20, a_pass.name);
				return;
			}
		}
		recorded.readback = readback;
		recorded.serial = ++state.serial;

		// Every earlier write is visible to the copies. The depth and colour targets move to copy sources and back.
		rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
		std::vector<rhi::TextureBarrier> textures;
		if (recorded.depth) {
			rhi::TextureBarrier depth{};
			depth.texture = a_pass.depth.GetHandle();
			depth.range = { 0, recorded.depthDesc.MipLevels, 0, recorded.depthDesc.DepthOrArraySize, 0, PlaneCount(device, recorded.depthDesc.Format) };
			depth.beforeSync = rhi::ResourceSyncState::All;
			depth.afterSync = rhi::ResourceSyncState::Copy;
			depth.beforeAccess = rhi::ResourceAccessType::DepthReadWrite;
			depth.afterAccess = rhi::ResourceAccessType::CopySource;
			depth.beforeLayout = rhi::ResourceLayout::DepthReadWrite;
			depth.afterLayout = rhi::ResourceLayout::CopySource;
			textures.push_back(depth);
		}
		for (std::uint32_t i = 0; i < 8; ++i)
			if (recorded.colours[i]) {
				const auto desc = recorded.colours[i]->GetDesc();
				rhi::TextureBarrier colour{};
				colour.texture = a_pass.colours[i].GetHandle();
				colour.range = { 0, desc.MipLevels, 0, desc.DepthOrArraySize, 0, 1 };
				colour.beforeSync = rhi::ResourceSyncState::All;
				colour.afterSync = rhi::ResourceSyncState::Copy;
				colour.beforeAccess = rhi::ResourceAccessType::RenderTarget;
				colour.afterAccess = rhi::ResourceAccessType::CopySource;
				colour.beforeLayout = rhi::ResourceLayout::RenderTarget;
				colour.afterLayout = rhi::ResourceLayout::CopySource;
				textures.push_back(colour);
			}
		a_commands.Barriers({ { textures.data(), static_cast<std::uint32_t>(textures.size()) }, {}, { &global, 1 } });
		for (const auto* range : { &recorded.frameConstants, &recorded.sequences, &recorded.counts })
			list->CopyBufferRegion(readback.get(), range->copyOffset, range->resource.get(), range->offset, range->size);
		if (recorded.depth)
			CopyTexture(device, list, recorded.depth.get(), readback.get(), recorded.depthOffset);
		for (std::uint32_t i = 0; i < 8; ++i)
			if (recorded.colours[i])
				CopyTexture(device, list, recorded.colours[i].get(), readback.get(), recorded.colourOffsets[i]);
		// The marker says this recording's copies ran.
		D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker{ readback->GetGPUVirtualAddress(), recorded.serial };
		list2->WriteBufferImmediate(1, &marker, nullptr);
		for (auto& texture : textures) {
			std::swap(texture.beforeAccess, texture.afterAccess);
			std::swap(texture.beforeLayout, texture.afterLayout);
			texture.beforeSync = rhi::ResourceSyncState::Copy;
			texture.afterSync = rhi::ResourceSyncState::All;
		}
		a_commands.Barriers({ { textures.data(), static_cast<std::uint32_t>(textures.size()) }, {}, { &global, 1 } });
		static std::uint32_t logged = 0;
		if (logged < 8) {
			++logged;
			logger::info("[DCLF] pass capture: {} records its copies ({} buckets, {:.1f} MiB readback, serial {})", a_pass.name, a_pass.buckets.size(),
				at / 1048576.0, recorded.serial);
		}
		recorded.pass = std::move(a_pass);
		auto& recordings = state.recordings[recorded.pass.name];
		recordings.push_back(std::move(recorded));
		// A recording dropped here keeps its readback alive through the pass's buffer or the retired list.
		while (recordings.size() > 64)
			recordings.pop_front();
	}

	void OnFrameEnd()
	{
		static std::uint64_t frame = 0;
		++frame;
		if (!Enabled())
			return;
		auto& state = Get();
		std::unique_lock lock(state.mutex);
		if (!state.armed)
			return;
		if (++state.framesWaited > 600) {
			logger::warn("[DCLF] pass capture: the passes did not run within 600 frames; disarmed");
			state.armed = false;
			return;
		}
		const auto start = std::chrono::steady_clock::now();
		auto& runtime = RenderGraphRuntime::Get();
		auto* host = runtime.Host();
		if (!host)
			return;
		runtime.DrainSubmissions("the DCLF pass capture", true);
		if (!host->SubmittedPoint().Wait(10000)) {
			logger::warn("[DCLF] pass capture: the GPU did not go idle in 10 s; trying again next frame");
			return;
		}
		const auto marker = [](const Recorded& a_recorded) {
			void* data = nullptr;
			const D3D12_RANGE header{ 0, 4 };
			std::uint32_t value = 0;
			if (SUCCEEDED(a_recorded.readback->Map(0, &header, &data)) && data) {
				value = *static_cast<const std::uint32_t*>(data);
				const D3D12_RANGE none{ 0, 0 };
				a_recorded.readback->Unmap(0, &none);
			}
			return value;
		};
		// The first frame end clears every marker. A marker set at a later one names the recording that ran in that frame.
		if (!state.markersCleared) {
			for (const auto& [name, recordings] : state.recordings)
				for (const auto& recorded : recordings) {
					void* data = nullptr;
					const D3D12_RANGE none{ 0, 0 };
					if (SUCCEEDED(recorded.readback->Map(0, &none, &data)) && data) {
						*static_cast<std::uint32_t*>(data) = 0;
						const D3D12_RANGE header{ 0, 4 };
						recorded.readback->Unmap(0, &header);
					}
				}
			state.markersCleared = true;
			return;
		}
		std::map<std::string, Recorded> ran;
		for (const auto& [name, recordings] : state.recordings)
			for (auto recorded = recordings.rbegin(); recorded != recordings.rend(); ++recorded)
				if (marker(*recorded) == recorded->serial) {
					ran.emplace(name, *recorded);
					break;
				}
		if (!ran.contains("main opaque") || !ran.contains("Z-prepass")) {
			if (state.framesWaited % 30 == 2)
				logger::info("[DCLF] pass capture: waiting for a frame that runs both passes' copies ({} of {} recorded passes ran)", ran.size(),
					state.recordings.size());
			return;
		}
		const std::filesystem::path directory = std::filesystem::path(Directory()) / fmt::format("capture-{}", frame);
		std::string summary;
		Dump dump(host->Device(), NativeDevice(host->Device()), ran, directory);
		const bool written = dump.Write(frame, summary);
		const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (written)
			logger::info("[DCLF] pass capture: wrote {} in {:.0f} ms: {}", directory.string(), milliseconds, summary);
		else
			logger::error("[DCLF] pass capture: {} failed after {:.0f} ms: {}", directory.string(), milliseconds, summary);
		state.armed = false;
	}
}
#endif
