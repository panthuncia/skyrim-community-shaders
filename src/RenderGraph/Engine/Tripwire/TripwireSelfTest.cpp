#include "RenderGraph/Engine/Tripwire/TripwireSelfTest.h"

#include "RenderGraph/Engine/Tripwire/TripwireHandles.h"
#include "Utils/D3D.h"

#include <cstring>
#include <thread>

namespace RenderGraph::Engine::Tripwire::SelfTest
{
	namespace
	{
		struct Checks
		{
			std::uint32_t total = 0;
			std::uint32_t failed = 0;

			void operator()(bool a_ok, std::string_view a_what)
			{
				++total;
				if (a_ok)
					return;
				++failed;
				logger::error("[ORG] tripwire self-test: FAIL {}", a_what);
			}
		};

		template <class T>
		bool SameBytes(const T& a_left, const T& a_right)
		{
			return std::memcmp(&a_left, &a_right, sizeof(T)) == 0;
		}

		// One forbidden call: a_call makes it and says whether it failed and zeroed its outputs; it must be counted once.
		template <class F>
		void CheckTrip(Checks& a_check, Kind a_kind, const char* a_method, F&& a_call)
		{
			const auto label = fmt::format("{}::{}", InterfaceName(a_kind), a_method);
			const auto before = Detail::Calls(a_kind, a_method);
			a_check(a_call(), label + " fails and zeroes its outputs");
			a_check(Detail::Calls(a_kind, a_method) == before + 1, label + " is recorded");
		}

		// What every handle answers: identity, refcount, QueryInterface, the debug name.
		template <class I>
		void CheckCommon(Checks& a_check, I* a_object, Kind a_kind, std::string_view a_name)
		{
			const std::string interfaceName = InterfaceName(a_kind);
			a_check(a_object != nullptr, interfaceName + " is made");
			if (!a_object)
				return;
			a_check(IsHandle(a_object), interfaceName + " is recognised as a handle");
			const auto info = Describe(a_object);
			a_check(info.kind == a_kind && info.name == a_name, interfaceName + " describes its kind and name");

			a_check(a_object->AddRef() == 2, interfaceName + " AddRef counts");
			a_check(a_object->Release() == 1, interfaceName + " Release counts");

			void* out = nullptr;
			a_check(a_object->QueryInterface(__uuidof(I), &out) == S_OK && out == a_object, interfaceName + " answers its own IID");
			if (out)
				static_cast<I*>(out)->Release();
			out = nullptr;
			a_check(a_object->QueryInterface(__uuidof(IUnknown), &out) == S_OK && out == static_cast<IUnknown*>(a_object), interfaceName + " answers IUnknown");
			if (out)
				static_cast<IUnknown*>(out)->Release();
			out = reinterpret_cast<void*>(1);
			const IID foreign = a_kind == Kind::Device ? __uuidof(ID3D11DeviceContext) : __uuidof(ID3D11Device);
			CheckTrip(a_check, a_kind, "QueryInterface", [&] { return a_object->QueryInterface(foreign, &out) == E_NOINTERFACE && out == nullptr; });
			a_check(a_object->AddRef() == 2 && a_object->Release() == 1, interfaceName + " QueryInterface leaves the count balanced");

			constexpr std::string_view renamed = "Tripwire::SelfTest renamed";
			a_check(a_object->SetPrivateData(Util::WKPDID_D3DDebugObjectNameT, static_cast<UINT>(renamed.size()), renamed.data()) == S_OK,
				interfaceName + " takes a debug name");
			char buffer[64]{};
			UINT size = sizeof(buffer);
			a_check(a_object->GetPrivateData(Util::WKPDID_D3DDebugObjectNameT, &size, buffer) == S_OK && std::string_view(buffer, size) == renamed,
				interfaceName + " returns its debug name");
			const std::uint32_t value = 7;
			CheckTrip(a_check, a_kind, "SetPrivateData",
				[&] { return a_object->SetPrivateData(__uuidof(ID3D11Texture2D), sizeof(value), &value) == E_FAIL; });
		}

		template <class I>
		void CheckGetDevice(Checks& a_check, I* a_object, Kind a_kind)
		{
			CheckTrip(a_check, a_kind, "GetDevice", [&] {
				auto* device = reinterpret_cast<ID3D11Device*>(1);
				a_object->GetDevice(&device);
				return device == nullptr;
			});
		}

		template <class I>
		void CheckFinalRelease(Checks& a_check, winrt::com_ptr<I>& a_object, Kind a_kind)
		{
			a_check(a_object.detach()->Release() == 0, std::string(InterfaceName(a_kind)) + " last Release reaches 0");
		}

		void Resources(Checks& a_check)
		{
			D3D11_TEXTURE1D_DESC desc1{ 64, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
			auto texture1 = CreateTexture1D(desc1, { .name = "SelfTest::Texture1D" });
			CheckCommon(a_check, texture1.get(), Kind::Texture1D, "SelfTest::Texture1D");
			D3D11_TEXTURE1D_DESC read1{};
			texture1->GetDesc(&read1);
			a_check(SameBytes(read1, desc1), "ID3D11Texture1D GetDesc returns its description");
			CheckGetDevice(a_check, texture1.get(), Kind::Texture1D);
			CheckFinalRelease(a_check, texture1, Kind::Texture1D);

			D3D11_TEXTURE2D_DESC desc2{ 128, 64, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, { 1, 0 }, D3D11_USAGE_DEFAULT,
				D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0 };
			auto texture2 = CreateTexture2D(desc2, { .name = "SelfTest::Texture2D" });
			CheckCommon(a_check, texture2.get(), Kind::Texture2D, "SelfTest::Texture2D");
			D3D11_TEXTURE2D_DESC read2{};
			texture2->GetDesc(&read2);
			a_check(SameBytes(read2, desc2), "ID3D11Texture2D GetDesc returns its description");
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			texture2->GetType(&dimension);
			a_check(dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D, "ID3D11Texture2D GetType answers TEXTURE2D");
			CheckGetDevice(a_check, texture2.get(), Kind::Texture2D);
			CheckTrip(a_check, Kind::Texture2D, "GetEvictionPriority", [&] { return texture2->GetEvictionPriority() == 0; });

			D3D11_TEXTURE3D_DESC desc3{ 32, 32, 8, 1, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0 };
			auto texture3 = CreateTexture3D(desc3, { .name = "SelfTest::Texture3D" });
			CheckCommon(a_check, texture3.get(), Kind::Texture3D, "SelfTest::Texture3D");
			D3D11_TEXTURE3D_DESC read3{};
			texture3->GetDesc(&read3);
			a_check(SameBytes(read3, desc3), "ID3D11Texture3D GetDesc returns its description");
			CheckGetDevice(a_check, texture3.get(), Kind::Texture3D);

			D3D11_BUFFER_DESC bufferDesc{ 256, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0 };
			auto buffer = CreateBuffer(bufferDesc, { .name = "SelfTest::Buffer" });
			CheckCommon(a_check, buffer.get(), Kind::Buffer, "SelfTest::Buffer");
			D3D11_BUFFER_DESC readBuffer{};
			buffer->GetDesc(&readBuffer);
			a_check(SameBytes(readBuffer, bufferDesc), "ID3D11Buffer GetDesc returns its description");
			CheckGetDevice(a_check, buffer.get(), Kind::Buffer);
			CheckFinalRelease(a_check, buffer, Kind::Buffer);

			// Views answer GetResource with the resource they were made with.
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc2.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D = { 0, 1 };
			auto srv = CreateShaderResourceView(texture2.get(), srvDesc, { .name = "SelfTest::SRV" });
			CheckCommon(a_check, srv.get(), Kind::ShaderResourceView, "SelfTest::SRV");
			D3D11_SHADER_RESOURCE_VIEW_DESC readSrv{};
			srv->GetDesc(&readSrv);
			a_check(SameBytes(readSrv, srvDesc), "ID3D11ShaderResourceView GetDesc returns its description");
			ID3D11Resource* viewed = nullptr;
			srv->GetResource(&viewed);
			a_check(viewed == texture2.get(), "ID3D11ShaderResourceView GetResource returns its texture");
			if (viewed)
				viewed->Release();
			CheckGetDevice(a_check, srv.get(), Kind::ShaderResourceView);

			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = desc2.Format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			auto rtv = CreateRenderTargetView(texture2.get(), rtvDesc, { .name = "SelfTest::RTV" });
			CheckCommon(a_check, rtv.get(), Kind::RenderTargetView, "SelfTest::RTV");
			D3D11_RENDER_TARGET_VIEW_DESC readRtv{};
			rtv->GetDesc(&readRtv);
			a_check(SameBytes(readRtv, rtvDesc), "ID3D11RenderTargetView GetDesc returns its description");

			// A view made without its resource trips on GetResource.
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
			auto dsv = CreateDepthStencilView(nullptr, dsvDesc, { .name = "SelfTest::DSV" });
			CheckCommon(a_check, dsv.get(), Kind::DepthStencilView, "SelfTest::DSV");
			D3D11_DEPTH_STENCIL_VIEW_DESC readDsv{};
			dsv->GetDesc(&readDsv);
			a_check(SameBytes(readDsv, dsvDesc), "ID3D11DepthStencilView GetDesc returns its description");
			CheckTrip(a_check, Kind::DepthStencilView, "GetResource", [&] {
				auto* resource = reinterpret_cast<ID3D11Resource*>(1);
				dsv->GetResource(&resource);
				return resource == nullptr;
			});

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = desc3.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
			uavDesc.Texture3D = { 0, 0, 8 };
			auto uav = CreateUnorderedAccessView(texture3.get(), uavDesc, { .name = "SelfTest::UAV" });
			CheckCommon(a_check, uav.get(), Kind::UnorderedAccessView, "SelfTest::UAV");
			D3D11_UNORDERED_ACCESS_VIEW_DESC readUav{};
			uav->GetDesc(&readUav);
			a_check(SameBytes(readUav, uavDesc), "ID3D11UnorderedAccessView GetDesc returns its description");

			// The views hold their resources: the textures outlive the caller's references.
			CheckFinalRelease(a_check, srv, Kind::ShaderResourceView);
			CheckFinalRelease(a_check, rtv, Kind::RenderTargetView);
			a_check(texture2.detach()->Release() == 0, "ID3D11Texture2D is freed after its views");
			texture3->AddRef();
			a_check(texture3->Release() == 2, "ID3D11UnorderedAccessView holds its texture");
			CheckFinalRelease(a_check, uav, Kind::UnorderedAccessView);
			CheckFinalRelease(a_check, texture3, Kind::Texture3D);
			CheckFinalRelease(a_check, dsv, Kind::DepthStencilView);
		}

		void ShadersAndStates(Checks& a_check)
		{
			const std::uint8_t bytecode[] = { 'D', 'X', 'B', 'C', 1, 2, 3, 4, 5, 6, 7, 8 };
			auto vs = CreateVertexShader(bytecode, { .name = "SelfTest::VS" });
			CheckCommon(a_check, vs.get(), Kind::VertexShader, "SelfTest::VS");
			const auto kept = ShaderBytecode(vs.get());
			a_check(kept.size() == sizeof(bytecode) && std::memcmp(kept.data(), bytecode, sizeof(bytecode)) == 0 && kept.data() != bytecode,
				"ID3D11VertexShader keeps a copy of its bytecode");
			CheckGetDevice(a_check, vs.get(), Kind::VertexShader);
			auto ps = CreatePixelShader(bytecode, { .name = "SelfTest::PS" });
			CheckCommon(a_check, ps.get(), Kind::PixelShader, "SelfTest::PS");
			a_check(ShaderBytecode(ps.get()).size() == sizeof(bytecode), "ID3D11PixelShader keeps its bytecode");
			auto cs = CreateComputeShader(bytecode, { .name = "SelfTest::CS" });
			CheckCommon(a_check, cs.get(), Kind::ComputeShader, "SelfTest::CS");
			a_check(ShaderBytecode(cs.get()).size() == sizeof(bytecode), "ID3D11ComputeShader keeps its bytecode");

			winrt::com_ptr<ID3D11InputLayout> layout;
			{
				std::string position = "POSITION", texcoord = "TEXCOORD";
				const D3D11_INPUT_ELEMENT_DESC elements[] = {
					{ position.c_str(), 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
					{ texcoord.c_str(), 0, DXGI_FORMAT_R16G16_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
				};
				layout = CreateInputLayout(elements, bytecode, { .name = "SelfTest::InputLayout" });
				position.assign("XXXXXXXX");
				texcoord.assign("XXXXXXXX");
			}
			CheckCommon(a_check, layout.get(), Kind::InputLayout, "SelfTest::InputLayout");
			const auto elements = InputElements(layout.get());
			a_check(elements.size() == 2 && std::string_view(elements[0].SemanticName) == "POSITION" && std::string_view(elements[1].SemanticName) == "TEXCOORD" &&
						elements[1].AlignedByteOffset == 12,
				"ID3D11InputLayout keeps its elements and semantic names");
			a_check(ShaderBytecode(layout.get()).empty() && InputElements(vs.get()).empty(), "accessors refuse handles of another kind");

			const CD3D11_BLEND_DESC blendDesc{ CD3D11_DEFAULT{} };
			auto blend = CreateBlendState(blendDesc, { .name = "SelfTest::Blend" });
			CheckCommon(a_check, blend.get(), Kind::BlendState, "SelfTest::Blend");
			D3D11_BLEND_DESC readBlend{};
			blend->GetDesc(&readBlend);
			a_check(SameBytes(readBlend, static_cast<const D3D11_BLEND_DESC&>(blendDesc)), "ID3D11BlendState GetDesc returns its description");

			const CD3D11_DEPTH_STENCIL_DESC depthDesc{ CD3D11_DEFAULT{} };
			auto depth = CreateDepthStencilState(depthDesc, { .name = "SelfTest::DepthStencil" });
			CheckCommon(a_check, depth.get(), Kind::DepthStencilState, "SelfTest::DepthStencil");
			D3D11_DEPTH_STENCIL_DESC readDepth{};
			depth->GetDesc(&readDepth);
			a_check(SameBytes(readDepth, static_cast<const D3D11_DEPTH_STENCIL_DESC&>(depthDesc)), "ID3D11DepthStencilState GetDesc returns its description");

			const CD3D11_RASTERIZER_DESC rasterDesc{ CD3D11_DEFAULT{} };
			auto raster = CreateRasterizerState(rasterDesc, { .name = "SelfTest::Rasterizer" });
			CheckCommon(a_check, raster.get(), Kind::RasterizerState, "SelfTest::Rasterizer");
			D3D11_RASTERIZER_DESC readRaster{};
			raster->GetDesc(&readRaster);
			a_check(SameBytes(readRaster, static_cast<const D3D11_RASTERIZER_DESC&>(rasterDesc)), "ID3D11RasterizerState GetDesc returns its description");

			const CD3D11_SAMPLER_DESC samplerDesc{ CD3D11_DEFAULT{} };
			auto sampler = CreateSamplerState(samplerDesc, { .name = "SelfTest::Sampler" });
			CheckCommon(a_check, sampler.get(), Kind::SamplerState, "SelfTest::Sampler");
			D3D11_SAMPLER_DESC readSampler{};
			sampler->GetDesc(&readSampler);
			a_check(SameBytes(readSampler, static_cast<const D3D11_SAMPLER_DESC&>(samplerDesc)), "ID3D11SamplerState GetDesc returns its description");
			CheckGetDevice(a_check, sampler.get(), Kind::SamplerState);

			const D3D11_QUERY_DESC queryDesc{ D3D11_QUERY_EVENT, 0 };
			auto query = CreateQuery(queryDesc, { .name = "SelfTest::Query" });
			CheckCommon(a_check, query.get(), Kind::Query, "SelfTest::Query");
			D3D11_QUERY_DESC readQuery{};
			query->GetDesc(&readQuery);
			a_check(SameBytes(readQuery, queryDesc), "ID3D11Query GetDesc returns its description");
			CheckTrip(a_check, Kind::Query, "GetDataSize", [&] { return query->GetDataSize() == 0; });
		}

		void DeviceObjects(Checks& a_check)
		{
			auto device = CreateDevice({ .name = "SelfTest::Device" });
			CheckCommon(a_check, device.get(), Kind::Device, "SelfTest::Device");
			CheckTrip(a_check, Kind::Device, "CreateBuffer", [&] {
				const D3D11_BUFFER_DESC desc{ 16, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
				auto* buffer = reinterpret_cast<ID3D11Buffer*>(1);
				return device->CreateBuffer(&desc, nullptr, &buffer) == E_FAIL && buffer == nullptr;
			});
			CheckTrip(a_check, Kind::Device, "GetImmediateContext", [&] {
				auto* context = reinterpret_cast<ID3D11DeviceContext*>(1);
				device->GetImmediateContext(&context);
				return context == nullptr;
			});
			CheckTrip(a_check, Kind::Device, "GetFeatureLevel", [&] { return device->GetFeatureLevel() == D3D_FEATURE_LEVEL{}; });

			auto context = CreateDeviceContext({ .name = "SelfTest::Context" });
			CheckCommon(a_check, context.get(), Kind::DeviceContext, "SelfTest::Context");
			CheckGetDevice(a_check, context.get(), Kind::DeviceContext);
			auto texture = CreateTexture2D({ 4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE, D3D11_CPU_ACCESS_WRITE, 0 },
				{ .name = "SelfTest::Mapped" });
			CheckTrip(a_check, Kind::DeviceContext, "Map", [&] {
				D3D11_MAPPED_SUBRESOURCE mapped;
				std::memset(&mapped, 0xCD, sizeof(mapped));
				return context->Map(texture.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == E_FAIL && mapped.pData == nullptr && mapped.RowPitch == 0;
			});
			CheckTrip(a_check, Kind::DeviceContext, "Draw", [&] {
				context->Draw(3, 0);
				return true;
			});
			// The array is zeroed for the count passed in, then the count.
			CheckTrip(a_check, Kind::DeviceContext, "RSGetViewports", [&] {
				D3D11_VIEWPORT viewports[2];
				std::memset(viewports, 0xCD, sizeof(viewports));
				UINT count = 2;
				context->RSGetViewports(&count, viewports);
				return count == 0 && viewports[0].Width == 0.0f && viewports[1].Width == 0.0f;
			});
			CheckTrip(a_check, Kind::DeviceContext, "GetData", [&] {
				std::uint32_t data = 0xCDCDCDCD;
				return context->GetData(nullptr, &data, sizeof(data), 0) == E_FAIL && data == 0;
			});

			DXGI_SWAP_CHAIN_DESC swapDesc{};
			swapDesc.BufferDesc = { 1920, 1080, { 60, 1 }, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED, DXGI_MODE_SCALING_UNSPECIFIED };
			swapDesc.SampleDesc = { 1, 0 };
			swapDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			swapDesc.BufferCount = 2;
			swapDesc.Windowed = TRUE;
			swapDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			auto swapChain = CreateSwapChain(swapDesc, { .name = "SelfTest::SwapChain" });
			CheckCommon(a_check, swapChain.get(), Kind::SwapChain, "SelfTest::SwapChain");
			DXGI_SWAP_CHAIN_DESC readSwap{};
			a_check(swapChain->GetDesc(&readSwap) == S_OK && SameBytes(readSwap, swapDesc), "IDXGISwapChain GetDesc returns its description");
			CheckTrip(a_check, Kind::SwapChain, "Present", [&] { return swapChain->Present(1, 0) == E_FAIL; });
			CheckTrip(a_check, Kind::SwapChain, "GetBuffer", [&] {
				void* surface = reinterpret_cast<void*>(1);
				return swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), &surface) == E_FAIL && surface == nullptr;
			});
			CheckFinalRelease(a_check, swapChain, Kind::SwapChain);
			CheckFinalRelease(a_check, context, Kind::DeviceContext);
			CheckFinalRelease(a_check, device, Kind::Device);
		}

		// Guest handles, wrapping native handles (no device exists this early): calls forward, watched ones are recorded on the way,
		// and handle arguments reach the wrapped object unwrapped.
		void Guest(Checks& a_check)
		{
			const D3D11_TEXTURE2D_DESC desc{ 16, 16, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
			auto inner = CreateTexture2D(desc, { .name = "SelfTest::Inner" });
			static const auto watch = WatchSet::Of(Kind::Texture2D, { "GetEvictionPriority" });
			auto outer = CreateTexture2D(desc, { .name = "SelfTest::Outer", .watch = watch }, inner.get());
			a_check(Describe(outer.get()).wrapped == inner.get(), "a guest handle holds the object it wraps");
			a_check(Unwrap(outer.get()) == inner.get(), "Unwrap returns the wrapped object");
			auto notAnObject = desc;
			auto* plain = reinterpret_cast<ID3D11Texture2D*>(&notAnObject);
			a_check(!IsHandle(plain) && Unwrap(plain) == plain, "Unwrap leaves other objects alone");

			const auto setBefore = Detail::Calls(Kind::Texture2D, "SetEvictionPriority");
			outer->SetEvictionPriority(5);
			a_check(Detail::Calls(Kind::Texture2D, "SetEvictionPriority") == setBefore + 1, "an unwatched call is forwarded, unrecorded");
			const auto getBefore = Detail::Calls(Kind::Texture2D, "GetEvictionPriority");
			(void)outer->GetEvictionPriority();
			a_check(Detail::Calls(Kind::Texture2D, "GetEvictionPriority") == getBefore + 2, "a watched call is recorded and forwarded");

			constexpr std::string_view name = "SelfTest::Renamed";
			outer->SetPrivateData(Util::WKPDID_D3DDebugObjectNameT, static_cast<UINT>(name.size()), name.data());
			a_check(Describe(inner.get()).name == name, "a guest handle's debug name reaches the wrapped object");

			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D = { 0, 1 };
			auto innerSrv = CreateShaderResourceView(inner.get(), srvDesc, { .name = "SelfTest::InnerSRV" });
			auto outerSrv = CreateShaderResourceView(outer.get(), srvDesc, { .name = "SelfTest::OuterSRV" }, innerSrv.get());
			ID3D11ShaderResourceView* const views[] = { outerSrv.get(), nullptr };
			{
				const auto unwrapped = Detail::UnwrapArray(views, 2);
				ID3D11ShaderResourceView* const* result = unwrapped;
				a_check(result != views && result[0] == innerSrv.get() && result[1] == nullptr, "handle arrays are unwrapped for the wrapped object");
			}
			ID3D11ShaderResourceView* const plainViews[] = { nullptr, nullptr };
			{
				const auto unwrapped = Detail::UnwrapArray(plainViews, 2);
				ID3D11ShaderResourceView* const* result = unwrapped;
				a_check(result == plainViews, "arrays without handles are passed as they are");
			}

			auto innerContext = CreateDeviceContext({ .name = "SelfTest::InnerContext" });
			auto outerContext = CreateDeviceContext({ .name = "SelfTest::OuterContext" }, innerContext.get());
			const auto bindBefore = Detail::Calls(Kind::DeviceContext, "PSSetShaderResources");
			outerContext->PSSetShaderResources(0, 2, views);
			a_check(Detail::Calls(Kind::DeviceContext, "PSSetShaderResources") == bindBefore + 1, "a guest context forwards to the context it wraps");
			D3D11_MAPPED_SUBRESOURCE mapped{};
			a_check(outerContext->Map(outer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == E_FAIL, "a guest context returns the wrapped context's result");
		}

		// Release from several threads at once: the count stays exact and the owner is retired once, after the last one.
		void Threads(Checks& a_check)
		{
			auto owner = std::make_shared<int>(1);
			std::weak_ptr<int> watched = owner;
			const D3D11_BUFFER_DESC desc{ 64, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0 };
			auto buffer = CreateBuffer(desc, { .name = "SelfTest::Threads", .owner = std::move(owner) });
			(void)TakeRetired();
			constexpr int kThreads = 4, kRounds = 20000;
			std::vector<std::thread> threads;
			for (int t = 0; t < kThreads; ++t)
				threads.emplace_back([object = buffer.get()] {
					for (int i = 0; i < kRounds; ++i) {
						object->AddRef();
						object->Release();
					}
				});
			for (auto& thread : threads)
				thread.join();
			a_check(buffer->AddRef() == 2 && buffer->Release() == 1, "AddRef and Release from several threads keep the count");
			auto* last = buffer.detach();
			std::thread([last] { last->Release(); }).join();
			auto retired = TakeRetired();
			a_check(retired.size() == 1 && !watched.expired(), "the last Release queues the owner for ServiceRetired");
			retired.clear();
			a_check(watched.expired(), "the owner is freed once taken from the queue");
		}

		void Callers(Checks& a_check)
		{
			const auto id = static_cast<std::uint64_t>(REL::Module::IsAE() ? 77253 : 75467);
			const auto address = REL::RelocationID(75467, 77253).address() + 0x10;
			const auto text = DescribeCaller(reinterpret_cast<const void*>(address));
			a_check(text.find(fmt::format("(ID {}+0x10)", id)) != std::string::npos, fmt::format("a game address names its Address Library ID ({})", text));
			const auto own = DescribeCaller(reinterpret_cast<const void*>(&Run));
			a_check(own.find("+0x") != std::string::npos && own.find("another plugin") == std::string::npos, fmt::format("CS's own address names its module ({})", own));
		}
	}

	bool Requested()
	{
		char value[8]{};
		return GetEnvironmentVariableA("CS_ORG_TRIPWIRE_SELFTEST", value, sizeof(value)) && std::strcmp(value, "1") == 0;
	}

	void Run()
	{
		Detail::SetSelfTestRunning(true);
		Checks check;
		Resources(check);
		ShadersAndStates(check);
		DeviceObjects(check);
		Guest(check);
		Threads(check);
		Callers(check);
		(void)TakeRetired();
		Detail::ClearSites();
		Detail::SetSelfTestRunning(false);
		if (check.failed)
			logger::error("[ORG] tripwire self-test: FAIL ({} of {} checks failed)", check.failed, check.total);
		else
			logger::info("[ORG] tripwire self-test: PASS ({} checks, {} interfaces)", check.total, static_cast<int>(Kind::Count));
	}
}
