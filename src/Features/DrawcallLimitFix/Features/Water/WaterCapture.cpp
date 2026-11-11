// The water family's engine side: the two water batches' hooks, which let the engine's batch run with its draws latched and hand them
// to the anchor's pass; DrawTriShape's detour, which latches a water draw in place of drawing it; and BSWaterShader::SetupGeometry's,
// which has ConstantMirror watch the constant buffers the water's draws read. Render thread.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Capture/ConstantMirror.h"
#include "Features/DrawcallLimitFix/Core/Engine/HookSites.h"
#include "Features/DrawcallLimitFix/Core/Engine/ShellState.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuResources.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Features/Water/TextureFailure.h"
#include "Features/DrawcallLimitFix/Features/Water/Water.h"

#include "Features/LightLimitFix.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/FrameAnchors.h"
#include "RenderGraph/PublishedResources.h"
#include "RenderGraph/Switches.h"
#include "State.h"
#include "Utils/D3D.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <span>

namespace DCLF::Features::Water
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		// The most draws one batch latches; past it a draw is the engine's (WaterDraws' latch has room for this many).
		constexpr std::size_t kMaxLatched = 64;
		// Batches a view's shape must stay the same before its client is made: the ported features publish their resources in the
		// first frames, and a client is made once (Ready), so it waits for them to settle.
		constexpr std::uint32_t kSettleBatches = 30;
		// The engine's depth bias viewport offsets (BSGraphics::CreateDepthBiasViewportOffsets, SE 0x141794530, AE 0x1419db270): a bias
		// mode takes its entry off the viewport's maximum depth (R2, SetDirtyStates' dirty bit 0x40).
		constexpr std::array<float, 12> kBiasViewportOffsets{ 0.0f, 1.5e-5f, 0.0f, 0.0f, 0.0f, 0.0f, 2.6e-5f, 1.5e-5f, 2.8e-5f, 1.7e-5f, 3e-5f, 1.9e-5f };

		/** @brief Why a water draw could not be latched: each logged once per technique and view (Refuse). */
		enum class Refusal : std::uint8_t
		{
			None,
			Geometry,          // no TriShape or no buffers
			GeometryBounds,    // the index range is past the index buffer, or a vertex is smaller than its position
			TooMany,           // past kMaxLatched draws in one batch
			Shader,            // the bound shader is not BSWaterShader, or no shader objects
			Topology,          // not a triangle list
			Targets,           // a colour target the view's client does not import, or none
			Constants,         // ConstantMirror has no copy of a constant group the draw reads yet
			States,            // no depth-stencil, blend or rasterizer state object for the draw
			Program,           // the ORG program could not be built
			Buffers,           // the vertex or index buffer could not be imported
			Texture,           // a texture the draw samples could not be imported
			PublishedTexture,  // a texture slot holds a published graph texture's export (which only a declared read orders)
			Sampler,           // the sampler for the slot's modes is not in the graph's table
			OtherDrawPath,     // a water pass of the batch drew through another draw function than DrawTriShape
		};

		const char* ReasonOf(Refusal a_refusal)
		{
			switch (a_refusal) {
			case Refusal::Geometry:
				return "no TriShape buffers";
			case Refusal::GeometryBounds:
				return "its index range or vertex stride is out of its buffers' bounds";
			case Refusal::TooMany:
				return "more draws than the batch latches";
			case Refusal::Shader:
				return "the bound shader is not BSWaterShader";
			case Refusal::Topology:
				return "its topology is not a triangle list";
			case Refusal::Targets:
				return "a colour target its view's client does not import";
			case Refusal::Constants:
				return "ConstantMirror has no copy of its constants yet";
			case Refusal::States:
				return "no depth-stencil, blend or rasterizer state object";
			case Refusal::Program:
				return "its ORG program could not be built";
			case Refusal::Buffers:
				return "its vertex or index buffer could not be imported";
			case Refusal::Texture:
				return "a texture it samples could not be imported";
			case Refusal::PublishedTexture:
				return "a texture slot holds a published graph texture";
			case Refusal::Sampler:
				return "its sampler modes have no graph sampler";
			case Refusal::OtherDrawPath:
				return "a pass of the batch drew through another draw function than DrawTriShape";
			default:
				return "none";
			}
		}

		const char* NameOf(View a_view) { return a_view == View::Mask ? "mask" : "surface"; }

		/** @brief BSGraphics::TriShape's head (R1): the static vertex and index buffers and the VertexDesc. */
		struct EngineTriShape
		{
			ID3D11Buffer* vertexBuffer;
			ID3D11Buffer* indexBuffer;
			std::uint64_t vertexDesc;
		};

		/**
		 * @brief The batch being latched (render thread).
		 *
		 * A view's batches are the graph's only after a probe: a batch the engine draws while every draw is checked as the graph would
		 * take it (its program built, its constants mirrored, its textures and buffers imported). A probe with every draw latchable
		 * makes the view's next batches the graph's; a draw a later batch cannot latch makes that batch's remaining draws the engine's
		 * and the view probes again. So nothing is withheld until its program exists.
		 */
		struct Capture
		{
			bool active = false;
			bool probe = false;                                                                // the engine draws the batch; the draws are only checked
			bool failed = false;                                                               // a draw of the batch could not be latched: the rest are the engine's
			std::array<bool, static_cast<std::size_t>(View::Count)> clean{};                   // the view's last probe latched every draw
			std::array<std::optional<Shape>, static_cast<std::size_t>(View::Count)> settling;  // the shape a view waits on (kSettleBatches)
			std::array<std::uint32_t, static_cast<std::size_t>(View::Count)> settled{};
			View view = View::Surface;
			std::shared_ptr<Run> run;
			std::uint32_t depthFormat = 0;
			std::uint64_t fallbacks = 0;      // draws the engine drew during a latched batch
			std::uint32_t passes = 0;         // the batch's SetupGeometry calls
			std::uint32_t drawn = 0;          // the batch's DrawTriShape calls
			std::uint32_t lastTechnique = 0;  // the last pass's technique (State::currentPixelDescriptor >> 11 & 0xF)
			// The last pass's geometry type (BSGeometry +0x150, R1's dispatcher) while its draw has not reached DrawTriShape; 0xFF none.
			std::uint32_t pendingType = 0xFF;
			std::uint32_t otherTypes = 0;    // the geometry types of the batch's passes that drew elsewhere, a bit each
			std::set<std::uint32_t> logged;  // (refusal, technique, view) logged once each
			std::set<std::string> failures;  // texture import failures (TextureFailure.h), logged once each
		};

		Capture& GetCapture()
		{
			static Capture capture;
			return capture;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			Unwrap(a_rtv)->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		std::uint32_t FormatOf(ID3D11DepthStencilView* a_dsv)
		{
			if (!a_dsv)
				return 0;
			D3D11_DEPTH_STENCIL_VIEW_DESC desc{};
			Unwrap(a_dsv)->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		std::uint32_t TechniqueOf(std::uint32_t a_descriptor) { return (a_descriptor >> 11) & 0xF; }

		/**
		 * @brief Zero textures of each dimension the water's shader declares (1x1, RGBA32F), made on first use. GpuTextures' null view is
		 * a 2D one, and a cube, 3D or array read through a 2D descriptor is undefined on the GPU (a device loss on AMD), so an empty slot
		 * reads one of these instead.
		 */
		struct NullTextures
		{
			winrt::com_ptr<ID3D11ShaderResourceView> texture2D, texture2DArray, texture3D, cube;
			bool made = false;
		};

		NullTextures& GetNullTextures()
		{
			static NullTextures nulls;
			if (nulls.made)
				return nulls;
			nulls.made = true;
			auto* device = globals::d3d::device;
			const float zero[4 * 6]{};
			std::array<D3D11_SUBRESOURCE_DATA, 6> data{};
			for (auto& slice : data)
				slice = { .pSysMem = zero, .SysMemPitch = 16, .SysMemSlicePitch = 16 };
			const auto make2D = [&](std::uint32_t a_slices, std::uint32_t a_misc, const D3D11_SHADER_RESOURCE_VIEW_DESC* a_view, const char* a_name,
									winrt::com_ptr<ID3D11ShaderResourceView>& a_out) {
				const D3D11_TEXTURE2D_DESC desc{ .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = a_slices, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .SampleDesc = { .Count = 1 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE, .MiscFlags = a_misc };
				winrt::com_ptr<ID3D11Texture2D> texture;
				if (FAILED(device->CreateTexture2D(&desc, data.data(), texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), a_view, a_out.put())))
					return;
				Util::SetResourceName(texture.get(), a_name);
			};
			make2D(1, 0, nullptr, "DCLF::Water::Null2D", nulls.texture2D);
			D3D11_SHADER_RESOURCE_VIEW_DESC arrayView{ .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY };
			arrayView.Texture2DArray = { .MostDetailedMip = 0, .MipLevels = 1, .FirstArraySlice = 0, .ArraySize = 1 };
			make2D(1, 0, &arrayView, "DCLF::Water::Null2DArray", nulls.texture2DArray);
			make2D(6, D3D11_RESOURCE_MISC_TEXTURECUBE, nullptr, "DCLF::Water::NullCube", nulls.cube);
			const D3D11_TEXTURE3D_DESC volume{ .Width = 1, .Height = 1, .Depth = 1, .MipLevels = 1, .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_SHADER_RESOURCE };
			winrt::com_ptr<ID3D11Texture3D> texture3D;
			if (SUCCEEDED(device->CreateTexture3D(&volume, data.data(), texture3D.put())) &&
				SUCCEEDED(device->CreateShaderResourceView(texture3D.get(), nullptr, nulls.texture3D.put())))
				Util::SetResourceName(texture3D.get(), "DCLF::Water::Null3D");
			if (!nulls.texture2D || !nulls.texture2DArray || !nulls.cube || !nulls.texture3D)
				logger::error("[DCLF] water: the null textures could not be made; a water draw with an empty slot stays the engine's");
			return nulls;
		}

		/** @brief An empty slot's binding: a zero view of the slot's declared dimension (kInvalid when it could not be made). */
		GpuTextures::Binding NullOf(ResourceKind a_kind)
		{
			auto& textures = GpuTextures::Get();
			if (a_kind == ResourceKind::Buffer)
				return { .index = textures.NullBufferIndex(false) };
			auto& nulls = GetNullTextures();
			ID3D11ShaderResourceView* view = a_kind == ResourceKind::Cube           ? nulls.cube.get() :
			                                 a_kind == ResourceKind::Texture3D      ? nulls.texture3D.get() :
			                                 a_kind == ResourceKind::Texture2DArray ? nulls.texture2DArray.get() :
			                                                                          nulls.texture2D.get();
			return view ? textures.ResolveBinding(view, 120 + static_cast<std::uint32_t>(a_kind)) : GpuTextures::Binding{};
		}

		/** @brief Whether a_view's resource is a buffer. */
		bool IsBufferView(ID3D11ShaderResourceView* a_view)
		{
			D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
			a_view->GetDesc(&desc);
			return desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFER || desc.ViewDimension == D3D11_SRV_DIMENSION_BUFFEREX;
		}

		/**
		 * @brief The batch's view and its headers' inputs: the main camera's accumulator with kMAIN's depth bound (the only view the
		 * family takes), the headers' published graph textures (the shape) and the others' heap indices (a_run). False for anything
		 * else (another accumulator, a cube face, a header buffer the graph cannot read), which stays the engine's.
		 */
		bool CurrentShape(const void* a_accumulator, View a_view, Shape& a_shape, Run& a_run, Capture& a_capture)
		{
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			static REL::Relocation<void**> mainAccumulator{ REL::Offset(Engine::Address::kMainAccumulator) };
			if (a_accumulator != *mainAccumulator || rss.cubeMapRenderTarget != static_cast<std::uint32_t>(-1))
				return false;
			const auto main = static_cast<std::uint32_t>(RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN);
			if (rss.depthStencil != main) {
				if (a_capture.logged.insert(0xFD000000u | (rss.depthStencil & 0xFF) << 1 | static_cast<std::uint32_t>(a_view)).second)
					logger::warn("[DCLF] water: the main camera's {} batch has depth target {} bound, not kMAIN's; it stays the engine's", NameOf(a_view),
						rss.depthStencil);
				return false;
			}
			const auto& depths = globals::game::renderer->GetDepthStencilData().depthStencils;
			a_capture.depthFormat = FormatOf(depths[main].views[0]);
			if (!a_capture.depthFormat)
				return false;

			a_shape.view = a_view;
			auto& textures = GpuTextures::Get();
			for (std::size_t h = 0; h < kHeaderRegisters.size(); ++h) {
				ID3D11ShaderResourceView* view = nullptr;
				globals::d3d::context->PSGetShaderResources(kHeaderRegisters[h], 1, &view);
				if (!view) {
					auto binding = NullOf(kHeaderKinds[h]);
					if (binding.index == GpuTextures::kInvalid)
						return false;
					a_run.headerIndices[h] = binding.index;
					if (binding.owner)
						a_run.owners.push_back(std::move(binding.owner));
					continue;
				}
				a_shape.headerIds[h] = PublishedResources::ExportedId(view);
				bool ok = true;
				if (a_shape.headerIds[h].empty()) {
					// A D3D11 buffer cannot be imported (Light Limit Fix's D3D11 path): the water stays the engine's.
					if (IsBufferView(view)) {
						ok = false;
						if (a_capture.logged.insert(0xFE000000u | kHeaderRegisters[h]).second)
							logger::warn("[DCLF] water: t{} is a D3D11 buffer the graph cannot read (turn on its feature's ORG path); the water stays the engine's",
								kHeaderRegisters[h]);
					} else {
						const auto rejections = RejectionCount();
						auto binding = textures.ResolveBinding(view, 96 + static_cast<std::uint32_t>(h));
						ok = binding.index != GpuTextures::kInvalid;
						if (!ok)
							if (auto failure = DescribeTextureFailure(view, kHeaderRegisters[h], rejections); a_capture.failures.insert(failure).second)
								logger::warn("[DCLF] water: the {} batch's header texture could not be imported: {}", NameOf(a_view), failure);
						a_run.headerIndices[h] = binding.index;
						if (binding.owner)
							a_run.owners.push_back(std::move(binding.owner));
					}
				}
				// The binding keeps the view alive; the run keeps its import.
				view->Release();
				if (!ok)
					return false;
			}
			return true;
		}

		/**
		 * @brief A constant buffer's bytes as the draw reads them, copied into a_out (a_rows 16-byte rows; the rows past the buffer's
		 * contents are zero, which a technique whose vanilla shader declares fewer rows does not read). False when the buffer is bound
		 * but ConstantMirror has no copy of it yet.
		 */
		bool CopyGroup(const RE::BSGraphics::ConstantGroup& a_group, float (*a_out)[4], std::uint32_t a_rows)
		{
			std::memset(a_out, 0, a_rows * 16);
			const auto* buffer = reinterpret_cast<const ID3D11Buffer*>(a_group.buffer);
			if (!buffer)
				return true;
			const auto bytes = ConstantMirror::Get().Contents(buffer);
			if (bytes.empty())
				return false;
			std::memcpy(a_out, bytes.data(), (std::min)(bytes.size(), static_cast<std::size_t>(a_rows) * 16));
			return true;
		}

		/** @brief A buffer's mirrored bytes into a_out (zeros past them); false when it is watched but not copied yet. */
		bool CopyBuffer(ID3D11Buffer* a_buffer, std::span<std::byte> a_out)
		{
			std::memset(a_out.data(), 0, a_out.size());
			if (!a_buffer)
				return true;
			const auto bytes = ConstantMirror::Get().Contents(a_buffer);
			if (bytes.empty())
				return false;
			std::memcpy(a_out.data(), bytes.data(), (std::min)(bytes.size(), a_out.size()));
			return true;
		}

		/** @brief The buffer behind the view bound at PS t98 (Deferred's directional shadow record), or null. Not kept. */
		ID3D11Buffer* DirectionalShadowBuffer()
		{
			ID3D11ShaderResourceView* view = nullptr;
			globals::d3d::context->PSGetShaderResources(kDirectionalShadowRegister, 1, &view);
			if (!view)
				return nullptr;
			winrt::com_ptr<ID3D11Resource> resource;
			view->GetResource(resource.put());
			view->Release();
			const auto buffer = resource.try_as<ID3D11Buffer>();
			// The binding keeps it alive for the draw.
			return buffer.get();
		}

		/** @brief Light Limit Fix's strict light buffer (PS b3), or null without the feature. */
		ID3D11Buffer* StrictLightBuffer()
		{
			auto& lightLimitFix = globals::features::lightLimitFix;
			return lightLimitFix.loaded && lightLimitFix.strictLightDataCB ? lightLimitFix.strictLightDataCB->CB() : nullptr;
		}

		/**
		 * @brief The draw the engine is about to make, latched: the shadow state SetDirtyStates would apply, the constants the shaders'
		 * Setup functions wrote (ConstantMirror), the textures and samplers, the geometry. A refusal when it cannot be drawn by the graph:
		 * the engine draws it then.
		 */
		Refusal TryLatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			auto& run = *a_capture.run;
			if (!a_shape || !a_shape->vertexBuffer || !a_shape->indexBuffer)
				return Refusal::Geometry;
			if (run.draws.size() >= kMaxLatched)
				return Refusal::TooMany;
			const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
			const auto* vs = rss.currentVertexShader;
			const auto* ps = rss.currentPixelShader;
			const auto* state = globals::state;
			if (!vs || !ps || !state || !state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Water)
				return Refusal::Shader;

			const auto engine = Pipeline::EngineDrawState::Latch();
			if (engine.topology != D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST)
				return Refusal::Topology;

			Draw draw{};
			draw.key.view = a_capture.view;
			// The colour targets the view's pixel stage writes, looked up in its client's imports.
			const auto& targets = a_capture.view == View::Mask ? kMaskTargets : kSurfaceTargets;
			const auto& engineTargets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (std::uint32_t i = 0; i < TargetsOf(a_capture.view); ++i) {
				const auto target = static_cast<std::int32_t>(rss.renderTargets[i]);
				const auto found = std::ranges::find(targets, target);
				if (target < 0 || found == targets.end())
					return Refusal::Targets;
				draw.colours[i] = static_cast<std::uint8_t>(found - targets.begin());
				draw.key.colourFormats[i] = FormatOf(engineTargets[target].RTV);
				if (!draw.key.colourFormats[i])
					return Refusal::Targets;
			}
			draw.key.depthFormat = a_capture.depthFormat;

			// The depth-stencil state: the water's per-type stencil bits, tested or written with the shadow state's reference.
			draw.key.depthStencil = Pipeline::EngineDepthStencilState(engine.depthMode, engine.stencilMode);
			draw.key.blend = Pipeline::EngineBlendState(engine.blendMode, engine.alphaToCoverage, engine.writeMode, engine.blendExtra);
			draw.key.raster = Pipeline::EngineRasterState(engine.fill, engine.cull, engine.depthBias, engine.scissor);
			draw.key.alphaToCoverage = engine.alphaToCoverage;
			draw.stencilRef = engine.stencilRef;
			if (!draw.key.depthStencil || !draw.key.blend || !draw.key.raster)
				return Refusal::States;
			draw.key.vertexDescriptor = state->currentVertexDescriptor;
			draw.key.pixelDescriptor = state->currentPixelDescriptor;

			// The six constant groups as the Setup functions wrote them, Light Limit Fix's strict lights and the directional shadow record.
			auto& constants = draw.latch.constants;
			if (!CopyGroup(vs->constantBuffers[0], constants.vsTechnique, kGroupRows[0]) || !CopyGroup(vs->constantBuffers[1], constants.vsMaterial, kGroupRows[1]) ||
				!CopyGroup(vs->constantBuffers[2], constants.vsGeometry, kGroupRows[2]) || !CopyGroup(ps->constantBuffers[0], constants.psTechnique, kGroupRows[3]) ||
				!CopyGroup(ps->constantBuffers[1], constants.psMaterial, kGroupRows[4]) || !CopyGroup(ps->constantBuffers[2], constants.psGeometry, kGroupRows[5]))
				return Refusal::Constants;
			// The vertex stage's matrices and PerGeometry are every technique's: a draw without them is not one the engine made.
			if (!vs->constantBuffers[2].buffer)
				return Refusal::Constants;
			if (!CopyBuffer(StrictLightBuffer(), draw.latch.strictLights) || !CopyBuffer(DirectionalShadowBuffer(), draw.latch.directionalShadow))
				return Refusal::Constants;
			// CS's permutation block for the draw (State's SetupGeometry hooks filled it): the pixel stage branches on its descriptors.
			const auto& permutation = state->permutationData;
			constants.descriptors[0] = permutation.VertexShaderDescriptor;
			constants.descriptors[1] = permutation.PixelShaderDescriptor;
			constants.descriptors[2] = permutation.ExtraShaderDescriptor;
			constants.descriptors[3] = permutation.ExtraFeatureDescriptor;
			constants.effectRadius = permutation.EffectRadius;

			draw.program = WaterDraws::Program(draw.key, state->currentShader);
			if (!draw.program)
				return Refusal::Program;

			// The geometry, read by device address while its leases are held (the run keeps them until the GPU is done).
			auto& buffers = GpuResources::Frame();
			auto vertices = buffers.Acquire(a_shape->vertexBuffer);
			auto indices = buffers.Acquire(a_shape->indexBuffer);
			if (!vertices || !indices || !vertices->buffer.address || !indices->buffer.address)
				return Refusal::Buffers;
			// The draw's 16-bit indices must lie in the index buffer, and a vertex must hold at least the position the stage pulls
			// (R32G32B32A32: DCLFPullPosition). The index stage loads 4-byte words, so the last one may reach 2 bytes past the range.
			const std::uint32_t stride = static_cast<std::uint32_t>(a_shape->vertexDesc & 0xF) * 4;
			const std::uint64_t lastIndexByte = (std::uint64_t(a_start) + std::uint64_t(a_triangles) * 3) * 2;
			if (!a_triangles || stride < 16 || ((lastIndexByte + 3) & ~std::uint64_t(3)) > indices->buffer.size || vertices->buffer.size < stride)
				return Refusal::GeometryBounds;
			const std::uint64_t layout = vs->vertexDesc & a_shape->vertexDesc;
			constants.vertexAddress[0] = static_cast<std::uint32_t>(vertices->buffer.address);
			constants.vertexAddress[1] = static_cast<std::uint32_t>(vertices->buffer.address >> 32);
			constants.indexAddress[0] = static_cast<std::uint32_t>(indices->buffer.address);
			constants.indexAddress[1] = static_cast<std::uint32_t>(indices->buffer.address >> 32);
			constants.vertexLayout[0] = static_cast<std::uint32_t>(layout);
			constants.vertexLayout[1] = static_cast<std::uint32_t>(layout >> 32);
			constants.vertexStride = stride;
			constants.firstIndex = a_start;
			draw.indexCount = a_triangles * 3;

			// The twelve texture slots as the shadow state holds them (an empty slot reads zero, as on D3D11) and every slot's sampler.
			auto& textures = GpuTextures::Get();
			std::vector<std::shared_ptr<const void>> owners{ vertices->owner, indices->owner };
			for (std::uint32_t t = 0; t < 12; ++t) {
				auto* view = Unwrap(rss.PSTexture[t]);
				if (view && !PublishedResources::ExportedId(view).empty())
					return Refusal::PublishedTexture;
				const auto rejections = RejectionCount();
				auto binding = view ? textures.ResolveBinding(view, 64 + t) : NullOf(MaterialKind(t));
				if (binding.index == GpuTextures::kInvalid) {
					if (view)
						if (auto failure = DescribeTextureFailure(view, t, rejections); a_capture.failures.insert(failure).second)
							logger::warn("[DCLF] water: a {} draw's texture could not be imported: {}", NameOf(a_capture.view), failure);
					return Refusal::Texture;
				}
				constants.textures[t] = binding.index;
				owners.push_back(std::move(binding.owner));
				auto sampler = textures.SamplerBinding(rss.PSTextureAddressMode[t].underlying(), rss.PSTextureFilterMode[t].underlying());
				if (sampler.index == GpuTextures::kInvalid)
					return Refusal::Sampler;
				constants.samplers[t] = sampler.index;
				owners.push_back(std::move(sampler.owner));
			}

			// The viewport SetDirtyStates would set, with the depth bias mode's offset taken off its maximum depth.
			static REL::Relocation<const float*> cameraDepthRange{ REL::Offset(Engine::Address::kCameraDepthRange) };
			draw.viewport[0] = static_cast<std::uint32_t>(rss.viewPort.TopLeftX);
			draw.viewport[1] = static_cast<std::uint32_t>(rss.viewPort.TopLeftY);
			draw.viewport[2] = static_cast<std::uint32_t>(rss.viewPort.Width);
			draw.viewport[3] = static_cast<std::uint32_t>(rss.viewPort.Height);
			draw.depthRange[0] = cameraDepthRange.get()[0];
			draw.depthRange[1] = cameraDepthRange.get()[1] - kBiasViewportOffsets[(std::min)(engine.depthBias, 11u)];

			// Diagnostics: a technique CS_ORG_WATER_TECHNIQUES leaves out is withheld from the engine and not drawn.
			draw.skip = !GetDiagnostics().Draws(TechniqueOf(draw.key.pixelDescriptor));

			// A probe only checks: the engine draws it.
			if (a_capture.probe)
				return Refusal::None;
			run.draws.push_back(std::move(draw));
			for (auto& owner : owners)
				if (owner)
					run.owners.push_back(std::move(owner));
			return Refusal::None;
		}

		/** @brief A refusal noted: the batch fails, and the reason is logged once per technique and view. */
		void Refuse(Capture& a_capture, Refusal a_refusal, std::uint32_t a_technique)
		{
			a_capture.failed = true;
			const std::uint32_t key = (static_cast<std::uint32_t>(a_refusal) << 16) | ((a_technique & 0xFF) << 8) | static_cast<std::uint32_t>(a_capture.view);
			if (a_capture.logged.insert(key).second)
				logger::warn("[DCLF] water: a {} water draw of technique {} stays the engine's: {}{}", NameOf(a_capture.view), a_technique, ReasonOf(a_refusal),
					a_capture.probe ? " (probe)" : "");
		}

		/**
		 * @brief A draw latched (true: the graph draws it) or the engine's. A draw that fails marks the batch: its later draws are the
		 * engine's too, so the engine's draws of the batch stay in order after the ones latched before (which the anchor draws after the
		 * batch: one batch drawn out of order, then the view probes again).
		 */
		bool LatchDraw(Capture& a_capture, const EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
		{
			// A probe checks every draw, so one batch builds every program the view needs; a batch the graph draws stops at the first
			// draw it cannot take.
			if (a_capture.failed && !a_capture.probe)
				return false;
			const auto refusal = TryLatchDraw(a_capture, a_shape, a_start, a_triangles);
			if (refusal != Refusal::None) {
				const auto* state = globals::state;
				Refuse(a_capture, refusal, state ? TechniqueOf(state->currentPixelDescriptor) : 0xFF);
				return false;
			}
			return !a_capture.probe;
		}

		/**
		 * @brief A water batch: for the main camera's accumulator, the batch runs with its draws latched, then the anchor's pass draws them
		 * at this point.
		 */
		template <View kView>
		struct WaterBatch
		{
			static void thunk(void* a_accumulator, std::uint32_t a_first, std::uint32_t a_last, std::uint32_t a_flags, std::int32_t a_group)
			{
				auto& capture = GetCapture();
				if (capture.active || !ShellState::Running() || !FrameAnchors::OnRenderThread() || !WaterDraws::Available()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				// Diagnostics: CS_ORG_WATER_PARTS takes only the views it names on the graph.
				Shape shape;
				auto run = std::make_shared<Run>();
				run->view = kView;
				if (!GetDiagnostics().Takes(kView) || !CurrentShape(a_accumulator, kView, shape, *run, capture)) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const auto index = static_cast<std::size_t>(kView);
				auto& clean = capture.clean[index];
				// A view's client is made once, for a shape that held for kSettleBatches batches.
				auto& settling = capture.settling[index];
				if (!settling || *settling != shape) {
					settling = shape;
					capture.settled[index] = 0;
				}
				const bool settled = ++capture.settled[index] >= kSettleBatches;
				if (!settled || !WaterDraws::Ready(shape)) {
					// The client is new or is joining the graph: an empty run builds and prepares it, the engine draws this batch, and the
					// view probes again.
					clean = false;
					if (settled)
						WaterDraws::Submit(std::move(run));
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const bool probe = !clean;
				capture.view = kView;
				capture.run = run;
				capture.probe = probe;
				capture.failed = false;
				capture.passes = 0;
				capture.drawn = 0;
				capture.pendingType = 0xFF;
				capture.otherTypes = 0;
				capture.active = true;
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				capture.active = false;
				capture.run.reset();
				// Every pass must draw through DrawTriShape: one that drew through another function was the engine's, at its place in the
				// batch, ahead of the graph's draws.
				if (capture.pendingType != 0xFF)
					capture.otherTypes |= 1u << (capture.pendingType & 31);
				if (capture.drawn < capture.passes || capture.otherTypes) {
					if (capture.logged.insert(0xFF000000u | (capture.otherTypes << 1) | static_cast<std::uint32_t>(index)).second)
						logger::warn("[DCLF] water: {} of {} {} water passes drew without DrawTriShape (geometry types {:#x}, R1's dispatcher)",
							capture.passes > capture.drawn ? capture.passes - capture.drawn : 0, capture.passes, NameOf(kView), capture.otherTypes);
					Refuse(capture, Refusal::OtherDrawPath, capture.lastTechnique);
				}
				if (probe) {
					clean = !capture.failed;
					if (clean)
						logger::info("[DCLF] water: every {} water draw can be drawn by the graph; its batches are the graph's from the next", NameOf(kView));
					return;
				}
				if (capture.failed) {
					clean = false;
					logger::warn("[DCLF] water: a {} water draw could not be drawn by the graph; the engine draws the batches while the view probes again",
						NameOf(kView));
				}
				WaterDraws::Submit(std::move(run));
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/** @brief BSGraphics::Renderer::DrawTriShape (SE 75477, AE 77263): during a latched water batch, the draw is latched instead. */
		struct DrawTriShape
		{
			static void thunk(void* a_renderer, EngineTriShape* a_shape, std::uint32_t a_start, std::uint32_t a_triangles)
			{
				auto& capture = GetCapture();
				if (capture.active && capture.run) {
					++capture.drawn;
					capture.pendingType = 0xFF;
					if (LatchDraw(capture, a_shape, a_start, a_triangles))
						return;
					++capture.fallbacks;
				}
				func(a_renderer, a_shape, a_start, a_triangles);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/**
		 * @brief BSWaterShader::SetupGeometry (vtable slot 6, chained after Unified Water's, Light Limit Fix's, the cubemap fix's and the
		 * frame annotations'): during a latched batch, ConstantMirror watches the buffers the draw reads back (both stages' three groups,
		 * Light Limit Fix's strict lights, the directional shadow record). A buffer is mirrored from its next write on, so a batch's first
		 * sight of one draws on the engine.
		 */
		struct SetupGeometry
		{
			static void thunk(RE::BSShader* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_flags)
			{
				func(a_this, a_pass, a_flags);
				auto& capture = GetCapture();
				if (!capture.active)
					return;
				++capture.passes;
				if (capture.pendingType != 0xFF)
					capture.otherTypes |= 1u << (capture.pendingType & 31);
				capture.pendingType = a_pass && a_pass->geometry ? Engine::At<std::uint8_t>(a_pass->geometry, 0x150) : 0xFE;
				if (const auto* state = globals::state)
					capture.lastTechnique = TechniqueOf(state->currentPixelDescriptor);
				const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				auto& mirror = ConstantMirror::Get();
				for (std::uint32_t g = 0; g < 3; ++g) {
					if (const auto* vs = rss.currentVertexShader; vs && vs->constantBuffers[g].buffer)
						mirror.Watch(reinterpret_cast<ID3D11Buffer*>(vs->constantBuffers[g].buffer));
					if (const auto* ps = rss.currentPixelShader; ps && ps->constantBuffers[g].buffer)
						mirror.Watch(reinterpret_cast<ID3D11Buffer*>(ps->constantBuffers[g].buffer));
				}
				if (auto* strictLights = StrictLightBuffer())
					mirror.Watch(strictLights);
				if (auto* directionalShadow = DirectionalShadowBuffer())
					mirror.Watch(directionalShadow);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void PreflightHooks()
	{
		if (!Enabled())
			return;
		const bool known = Address::kMaskBatch.Known() && Address::kSurfaceBatch.Known() && Address::kDrawTriShape.Known();
		HookSites::RequireKnown("the water batches", known);
		if (!known)
			return;
		HookSites::RequireCall("the water mask batch (FinishAccumulating's RenderBatches of the Stencil technique)", REL::Offset(Address::kMaskBatch).address());
		HookSites::RequireCall("the water batch (FinishAccumulatingPostResolveDepth's RenderBatches of the water techniques)",
			REL::Offset(Address::kSurfaceBatch).address());
	}

	void InstallHooks()
	{
		if (!Address::kMaskBatch.Known() || !Address::kSurfaceBatch.Known() || !Address::kDrawTriShape.Known()) {
			logger::warn("[DCLF] water: this runtime has no address for the water batches; the water stays the engine's");
			return;
		}
		stl::write_thunk_call<WaterBatch<View::Mask>>(REL::Offset(Address::kMaskBatch).address());
		stl::write_thunk_call<WaterBatch<View::Surface>>(REL::Offset(Address::kSurfaceBatch).address());
		stl::detour_thunk<DrawTriShape>(REL::Offset(Address::kDrawTriShape).address());
		HookSites::NoteDetour("BSGraphics::Renderer::DrawTriShape (the water's draws)", DrawTriShape::func.address());
		stl::write_vfunc<0x6, SetupGeometry>(RE::VTABLE_BSWaterShader[0]);
		logger::info("[DCLF] water: hooks installed (the water mask and water batches, DrawTriShape, BSWaterShader::SetupGeometry)");
	}
}
