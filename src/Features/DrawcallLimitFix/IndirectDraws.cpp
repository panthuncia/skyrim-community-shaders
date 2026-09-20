#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "IndirectDraws.h"

#	include "ConstantMirror.h"
#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "GpuResources.h"
#	include "GpuTextures.h"
#	include "LightingConstants.h"
#	include "SceneStore.h"
#	include "ShaderPrograms.h"
#	include "Switches.h"

#	include "Deferred.h"
#	include "Features/LinearLighting.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "ShaderCache.h"
#	include "State.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/UploadServiceAccess.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <rhi_helpers.h>

#	include <chrono>
#	include <filesystem>
#	include <fstream>
#	include <iterator>
#	include <optional>
#	include <cstring>

namespace DCLF
{
	namespace
	{
		constexpr const char* kExtensionId = "cs.dclf.main-opaque";
		constexpr std::uint32_t kMaxDraws = 16384;
		constexpr std::uint64_t kConstantBytes = 48ull << 20;
		constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
		constexpr std::uint32_t kColorTargets = 8;
		constexpr std::uint32_t kMaxGeometries = kMaxDraws;
		constexpr const wchar_t* kBuildDrawsShader = L"Data\\Shaders\\DrawcallLimitFix\\ORG\\BuildDrawsCS.spv";

		// BuildDrawsCS.hlsl's inputs (byte-address buffers).
		struct DrawInput
		{
			std::uint32_t pipelineIndex;  // in the pipeline sets
			std::uint32_t recordIndex;    // DrawBindings record
			std::uint32_t geometryIndex;  // GeometryDraw
			std::uint32_t flags;
			float boundCentre[3];  // absolute world space (the eye is subtracted in the shader)
			float boundRadius;
		};
		static_assert(sizeof(DrawInput) == 32);

#pragma pack(push, 4)
		struct GeometryDraw
		{
			std::uint64_t vertexBufferAddress;
			std::uint32_t vertexBufferSize;
			std::uint32_t vertexStride;
			std::uint64_t indexBufferAddress;
			std::uint32_t indexBufferSize;
			std::uint32_t indexCount;
			std::uint32_t firstIndex;
			std::uint32_t pad;
		};
#pragma pack(pop)
		static_assert(sizeof(GeometryDraw) == 40);

		struct BuildDrawsConstants
		{
			std::uint32_t drawCount;
			std::uint32_t inputsIndex;
			std::uint32_t geometriesIndex;
			std::uint32_t sequencesIndex;
			std::uint32_t countIndex;
			std::uint32_t recordsAddressLo;
			std::uint32_t recordsAddressHi;
			std::uint32_t recordStride;
			std::uint32_t cullMode;  // CullMode
			float eye[3];            // the camera ViewProj is relative to
			float viewProj[16];      // row-major, as the shader's float4x4 with mul(M, v)
		};
		constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

		struct ComputeProgram
		{
			rhi::PipelineLayoutPtr layout;
			rhi::PipelinePtr pipeline;
		};

		std::shared_ptr<const ComputeProgram> LoadComputeProgram(rhi::Device a_device, const wchar_t* a_path, std::uint32_t a_constantWords)
		{
			std::ifstream file(a_path, std::ios::binary);
			std::vector<char> spirv((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
			if (spirv.empty()) {
				logger::error("[DCLF] Missing SPIR-V {}", std::filesystem::path(a_path).string());
				return {};
			}
			auto program = std::make_shared<ComputeProgram>();
			rhi::PushConstantRangeDesc constants{};
			constants.visibility = rhi::ShaderStage::Compute;
			constants.num32BitValues = a_constantWords;
			if (a_device.CreatePipelineLayout(rhi::PipelineLayoutDesc{ .pushConstants = { &constants, 1 }, .flags = rhi::PipelineLayoutFlags::PF_None }, program->layout) !=
				rhi::Result::Ok)
				return {};
			const rhi::SubobjLayout layout{ program->layout->GetHandle() };
			const rhi::SubobjShader shader{ rhi::ShaderStage::Compute, { spirv.data(), static_cast<std::uint32_t>(spirv.size()) }, "main" };
			const rhi::PipelineStreamItem items[] = { rhi::Make(layout), rhi::Make(shader) };
			if (a_device.CreatePipeline(items, 2, program->pipeline) != rhi::Result::Ok)
				return {};
			return program;
		}

		std::shared_ptr<org::Buffer> CreateWords(std::uint64_t a_words, bool a_unorderedAccess, const char* a_name)
		{
			auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(a_words), sizeof(std::uint32_t), a_unorderedAccess);
			buffer->SetName(a_name);
			buffer->Materialize();
			return buffer;
		}

		// Constant buffers the native draw rebinds per object (b0-b2 are the Lighting groups): everything
		// else bound in the main pass is per frame and comes from its CPU mirror.
		constexpr std::uint32_t kPerDrawVS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 4) | (1u << 9) | (1u << 10);
		constexpr std::uint32_t kPerDrawPS = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 8) | (1u << 11);
		constexpr std::uint32_t kPerFrameVertexRegister = 12;  // VS_PerFrame (Lighting.hlsl): ViewProj at c8
		// Bytes per texel, for printing exactly the pixel a readback holds and nothing beyond it.
		std::uint32_t FormatBytes(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_R32G32B32A32_FLOAT:
				return 16;
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R32G32_FLOAT:
				return 8;
			case DXGI_FORMAT_R16_FLOAT:
			case DXGI_FORMAT_R16_UNORM:
			case DXGI_FORMAT_D16_UNORM:
				return 2;
			case DXGI_FORMAT_R8_UNORM:
				return 1;
			default:
				return 4;  // the 32-bit packed formats the G-buffer uses (RGBA8, R10G10B10A2, R11G11B10)
			}
		}

		// The probe's sample points, in the order they happen within a frame.
		constexpr const char* kProbeFirstLabel = "before z-prepass";
		constexpr const char* kProbeLastLabel = "after colour";

		constexpr std::uint32_t kSharedDataRegister = 5;   // SharedData (SharedData.hlsli), bound by Community Shaders
		constexpr std::uint32_t kFeatureDataRegister = 6;  // FeatureData, likewise
		constexpr std::uint32_t kLinearLightingRegister = 8;  // LLPerGeometry: Linear Lighting's per-object emissive multiplier
		constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
		constexpr std::uint32_t kLightsRegister = 35;          // t35-t37: Light Limit Fix's lights, list and grid
		constexpr std::uint32_t kInvalidIndex = GpuTextures::kInvalid;

		// What the pass of one epoch draws; published before the epoch prepares.
		struct PassFrame
		{
			std::uint64_t serial = 0;
			std::uint32_t drawCount = 0;
			std::uint32_t cullMode = 0;
			bool offscreen = false;  // colour drawn into DCLF's own targets even on the hybrid path
			bool probePixel = false;
			std::uint32_t probeX = 0, probeY = 0;
			bool hasViewProj = false;
			RE::NiPoint3 eye;
			std::array<float, 16> viewProj{};
			std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
			float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
			bool hybrid = false;
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			IndirectState indirect{};
		};

		// A CPU-written buffer the main pass reads as a structured buffer SRV (t16 and up): the graph reads a
		// copy, refilled every epoch from the buffer's CPU mirror.
		struct FrameBuffer
		{
			std::uint32_t textureRegister = 0;
			std::uint32_t stride = 0;
			std::uint32_t firstElement = 0;
			std::uint32_t elements = 0;
			std::shared_ptr<org::Buffer> copy;

			bool SameShape(const FrameBuffer& a_other) const
			{
				return textureRegister == a_other.textureRegister && stride == a_other.stride && firstElement == a_other.firstElement && elements == a_other.elements;
			}
		};

		struct Resources
		{
			std::vector<FrameBuffer> frameBuffers;
			std::shared_ptr<org::Buffer> constants, records;
			std::shared_ptr<org::Buffer> inputs, geometries, sequences, count;  // BuildDraws: in, in, out, out
			std::shared_ptr<const ComputeProgram> buildDraws;
			winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
			std::uint64_t constantsAddress = 0, recordsAddress = 0;
			std::array<std::shared_ptr<org::PixelBuffer>, kColorTargets> targets;
			std::uint32_t targetCount = 0;
			std::shared_ptr<org::Resource> depth;  // DCLF's own Z-prepass (the objects it draws)
			// The native main-pass targets: overwritten with DCLF's before the composite (CS_DCLF_DEBUG_VIEW),
			// or drawn into directly (CS_DCLF_HYBRID, where the native loop skips DCLF's objects instead).
			std::array<std::shared_ptr<org::ExternalTextureResource>, kColorTargets> native;
			std::shared_ptr<org::ExternalTextureResource> nativeDepth;
			// What the passes bind: the native targets on the hybrid path, DCLF's own copies otherwise.
			std::array<std::shared_ptr<org::Resource>, kColorTargets> drawTargets;
			std::shared_ptr<org::Resource> drawDepth;
			// CS_DCLF_GBUFFER_PROBE: one texel of every target, copied inside the epoch both before and
			// after the colour draws. The copies are graph passes so the graph orders them against the
			// draws; a D3D11 readback issued around the epoch is not ordered against ORG's submissions at
			// all, and silently reports that nothing changed.
			std::shared_ptr<org::Buffer> probe;
			winrt::com_ptr<ID3D11Buffer> probeD3D11;
			bool hybrid = false;
			bool offscreen = false;
			std::uint32_t width = 0, height = 0;
			bool lightLimitFix = false;  // LLF's graph buffers are registered (they are read at t35-t37)
			std::atomic<std::shared_ptr<const PassFrame>> frame;
		};

		struct PassBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> targets{};
			org::ResourceBindingToken depth, sequences, count, records, constants;
			org::ResourceBindingToken lights, lightIndexList, lightGrid;
			std::vector<org::ResourceBindingToken> frameBuffers;
		};

		struct PreparedDraws
		{
			std::shared_ptr<const PassFrame> frame;
			std::array<org::PreparedDescriptorReference, kColorTargets> targetViews{};
			org::PreparedDescriptorReference depthView{};
			std::uint32_t targetCount = 0;
		};

		bool DrawingSegment()
		{
			const auto segment = RenderGraphRuntime::Get().CurrentSegment();
			return segment == RenderGraphRuntime::Segment::ZPrepass || segment == RenderGraphRuntime::Segment::MainOpaque;
		}

		std::shared_ptr<const PassFrame> CurrentFrame(const Resources& a_resources)
		{
			if (!DrawingSegment())
				return nullptr;
			return a_resources.frame.load(std::memory_order_acquire);
		}

		class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
		{
		public:
			explicit MainOpaquePass(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			PassBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				PassBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					bindings.targets[i] = a_builder.BindRenderTarget(resources->drawTargets[i]);
				bindings.depth = a_builder.BindDepthReadWrite(resources->drawDepth);
				bindings.sequences = a_builder.BindIndirectArguments(resources->sequences);
				bindings.count = a_builder.BindIndirectArguments(resources->count);
				// Read through device addresses; declared so the graph orders them after their uploads.
				bindings.records = a_builder.BindShaderResource(resources->records);
				bindings.constants = a_builder.BindShaderResource(resources->constants);
				for (const auto& frameBuffer : resources->frameBuffers)
					bindings.frameBuffers.push_back(a_builder.BindShaderResource(frameBuffer.copy));
				if (resources->lightLimitFix) {
					bindings.lights = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.lights"));
					bindings.lightIndexList = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.light-index-list"));
					bindings.lightGrid = a_builder.BindShaderResource(org::ResourceIdentifier("cs.llf.light-grid"));
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentFrame(*resources);
				a_out.push_back(frame ? frame->serial : 0);
				a_out.push_back(static_cast<std::uint64_t>(RenderGraphRuntime::Get().CurrentSegment()));
			}

			PreparedDraws Prepare(const PassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				PreparedDraws prepared{};
				auto frame = CurrentFrame(*resources);
				if (!frame || !frame->drawCount || !frame->indirect.valid)
					return prepared;
				prepared.frame = std::move(frame);
				prepared.targetCount = resources->targetCount;
				for (std::uint32_t i = 0; i < prepared.targetCount; ++i)
					prepared.targetViews[i] = a_preparation.CaptureView(a_bindings.targets[i], { org::BindlessViewKind::RenderTarget });
				prepared.depthView = a_preparation.CaptureView(a_bindings.depth, { org::BindlessViewKind::DepthStencil });
				return prepared;
			}

			static void Record(const PassBindings& a_bindings, const PreparedDraws& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return;
				const auto& frame = *a_prepared.frame;
				auto& commands = a_recording.Commands();
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				std::array<rhi::ColorAttachment, kColorTargets> colors{};
				for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i) {
					colors[i].rtv = a_recording.Resolve(a_prepared.targetViews[i]);
					colors[i].loadOp = (frame.hybrid && !frame.offscreen) ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
					colors[i].storeOp = rhi::StoreOp::Store;
					colors[i].resource = a_recording.Resolve(a_bindings.targets[i]).GetHandle();
				}
				const bool zPrepass = RenderGraphRuntime::Get().CurrentSegment() == RenderGraphRuntime::Segment::ZPrepass;
				if (zPrepass && !frame.hybrid)
					return;  // off the hybrid path both passes run together in the main segment
				const auto sequences = a_recording.Resolve(a_bindings.sequences).GetHandle();
				const auto count = a_recording.Resolve(a_bindings.count).GetHandle();
				rhi::PassBeginInfo begin{};
				begin.width = frame.width;
				begin.height = frame.height;
				begin.minDepth = frame.minDepth;
				begin.maxDepth = frame.maxDepth;

				// DCLF's Z-prepass: depth only, like the native one. On the hybrid path it adds DCLF's objects
				// to the depth the native passes already wrote, so they occlude and are occluded correctly.
				rhi::DepthAttachment depth{};
				depth.dsv = a_recording.Resolve(a_prepared.depthView);
				depth.depthLoad = frame.hybrid ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
				depth.depthStore = rhi::StoreOp::Store;
				depth.stencilLoad = frame.hybrid ? rhi::LoadOp::Load : rhi::LoadOp::Clear;
				depth.stencilStore = frame.hybrid ? rhi::StoreOp::Store : rhi::StoreOp::DontCare;
				depth.clear.depthStencil.depth = 1.0f;
				begin.depth = &depth;
				begin.debugName = "DCLF depth";
				// DCLF's Z-prepass. On the hybrid path it runs in its own segment, at the first draw of the
				// native main pass, so that the rest of the frame - the native draws that test depth, the sky
				// and everything that reads the depth buffer afterwards - sees DCLF's objects.
				if (!frame.hybrid || zPrepass) {
					commands.BeginPass(begin);
					commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					commands.BindLayout(frame.indirect.layout);
					// CS_DCLF_ZPREPASS_EMPTY=1: begin and end the pass but draw nothing, to tell apart damage
					// done by the depth writes from damage done by the epoch merely running here (its
					// submission, and the layout the attachment is left in).
					static const bool empty = SwitchEnabled("CS_DCLF_ZPREPASS_EMPTY");
					if (!(zPrepass && empty))
						commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, 0, count, 0, frame.drawCount);
					commands.EndPass();
				}

				// The main pass: depth test EQUAL against it (its pipelines do not write depth; the attachment
				// stays in the layout the pass declared).
				depth.depthLoad = rhi::LoadOp::Load;
				depth.stencilLoad = rhi::LoadOp::Load;
				if (zPrepass)
					return;  // the colour pass belongs to the main segment
				begin.colors = { colors.data(), a_prepared.targetCount };
				begin.debugName = "DCLF main opaque";
				commands.BeginPass(begin);
				commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
				commands.BindLayout(frame.indirect.layout);
				commands.ExecuteIndirect(frame.indirect.signatures[kColorVariant], sequences, 0, count, 0, frame.drawCount);
				commands.EndPass();
			}

		private:
			std::shared_ptr<Resources> resources;
		};

		struct BuildDrawsBindings
		{
			org::ResourceBindingToken inputs, geometries, sequences, count;
		};

		struct BuildDrawsFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			BuildDrawsConstants constants{};
			std::uint32_t groups = 0;
		};

		// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
		class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
		{
		public:
			explicit BuildDrawsPass(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			BuildDrawsBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				return { a_builder.BindShaderResource(resources->inputs), a_builder.BindShaderResource(resources->geometries),
					a_builder.BindUnorderedAccess(resources->sequences), a_builder.BindUnorderedAccess(resources->count) };
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentFrame(*resources);
				a_out.push_back(frame ? frame->serial : 0);
			}

			BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				BuildDrawsFrame prepared{};
				const auto frame = CurrentFrame(*resources);
				if (!frame || !frame->drawCount || !resources->buildDraws)
					return prepared;
				prepared.program = resources->buildDraws;
				auto& constants = prepared.constants;
				constants.drawCount = frame->drawCount;
				constants.inputsIndex = a_preparation.ResolveView(a_bindings.inputs, { org::BindlessViewKind::ShaderResource }).index;
				constants.geometriesIndex = a_preparation.ResolveView(a_bindings.geometries, { org::BindlessViewKind::ShaderResource }).index;
				constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.countIndex = a_preparation.ResolveView(a_bindings.count, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.recordsAddressLo = static_cast<std::uint32_t>(resources->recordsAddress);
				constants.recordsAddressHi = static_cast<std::uint32_t>(resources->recordsAddress >> 32);
				constants.recordStride = sizeof(DrawBindings);
				constants.cullMode = frame->hasViewProj ? frame->cullMode : 0u;
				constants.eye[0] = frame->eye.x;
				constants.eye[1] = frame->eye.y;
				constants.eye[2] = frame->eye.z;
				std::memcpy(constants.viewProj, frame->viewProj.data(), sizeof(constants.viewProj));
				prepared.groups = (frame->drawCount + 63) / 64;
				return prepared;
			}

			static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || !a_frame.groups)
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_frame.constants));
				commands.Dispatch(a_frame.groups, 1, 1);
			}

		private:
			std::shared_ptr<Resources> resources;
		};

		// Bytes reserved per sampled texel; a copy footprint's row pitch wants generous alignment.
		constexpr std::uint32_t kProbeSlotBytes = 256;
		// Slots: the colour targets before the draws, the same after, then the depth after the Z-prepass and
		// again just before the colour draws - the two numbers the colour pass's depth test compares.
		constexpr std::uint32_t kProbeDepthAfterPrepass = 2 * kColorTargets;
		constexpr std::uint32_t kProbeDepthBeforeColour = kProbeDepthAfterPrepass + 1;
		constexpr std::uint32_t kProbeDepthAfterColour = kProbeDepthBeforeColour + 1;
		constexpr std::uint32_t kProbeSlots = kProbeDepthAfterColour + 1;

		struct ProbeBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> sources;
			org::ResourceBindingToken depth;
			org::ResourceBindingToken destination;
		};

		struct ProbeFrame
		{
			std::uint32_t count = 0;
			std::uint32_t base = 0;        // first slot this pass writes
			std::uint32_t depthSlot = ~0u;  // where this pass puts the depth texel, if it samples it
			std::uint32_t x = 0, y = 0;
		};

		// Copies one texel of each main-pass target into the probe buffer. Two of these are declared, one
		// either side of the opaque pass, so the same texel is sampled before and after DCLF's colour draws
		// within a single frame.
		class ProbePass final : public org::TypedRenderGraphPass<ProbePass, ProbeFrame, ProbeBindings>
		{
		public:
			ProbePass(std::shared_ptr<Resources> a_resources, bool a_after) :
				resources(std::move(a_resources)), after(a_after) {}

			ProbeBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ProbeBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					bindings.sources[i] = a_builder.BindCopySource(resources->drawTargets[i]);
				bindings.depth = a_builder.BindCopySource(resources->drawDepth);
				bindings.destination = a_builder.BindCopyDestination(resources->probe);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentFrame(*resources);
				a_out.push_back(frame ? frame->serial : 0);
				a_out.push_back(static_cast<std::uint64_t>(RenderGraphRuntime::Get().CurrentSegment()));
			}

			ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const
			{
				ProbeFrame prepared{};
				const auto segment = RenderGraphRuntime::Get().CurrentSegment();
				const bool zPrepass = segment == RenderGraphRuntime::Segment::ZPrepass;
				if (segment != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after))
					return prepared;
				const auto frame = CurrentFrame(*resources);
				if (!frame || !frame->probePixel)
					return prepared;
				prepared.x = frame->probeX;
				prepared.y = frame->probeY;
				if (zPrepass) {
					prepared.depthSlot = kProbeDepthAfterPrepass;  // what the Z-prepass left in the buffer
					return prepared;
				}
				prepared.count = resources->targetCount;
				prepared.base = after ? kColorTargets : 0;
				// What the colour pass tests against, and - with CS_DCLF_COLOUR_DEPTH_WRITE - what it
				// computes, so the two can be compared directly instead of inferred from whether it drew.
				prepared.depthSlot = after ? kProbeDepthAfterColour : kProbeDepthBeforeColour;
				return prepared;
			}

			static void Record(const ProbeBindings& a_bindings, const ProbeFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.count && a_frame.depthSlot == ~0u)
					return;
				auto& commands = a_recording.Commands();
				const auto buffer = a_recording.Resolve(a_bindings.destination).GetHandle();
				if (a_frame.depthSlot != ~0u) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = a_recording.Resolve(a_bindings.depth).GetHandle();
					copy.buffer = buffer;
					copy.x = a_frame.x;
					copy.y = a_frame.y;
					copy.footprint.offset = std::uint64_t(a_frame.depthSlot) * kProbeSlotBytes;
					copy.footprint.rowPitch = kProbeSlotBytes;
					copy.footprint.width = 1;
					copy.footprint.height = 1;
					copy.footprint.depth = 1;
					commands.CopyTextureToBuffer(copy);
				}
				for (std::uint32_t i = 0; i < a_frame.count; ++i) {
					rhi::BufferTextureCopyFootprint copy{};
					copy.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
					copy.buffer = buffer;
					copy.mip = 0;
					copy.arraySlice = 0;
					copy.x = a_frame.x;
					copy.y = a_frame.y;
					copy.z = 0;
					copy.footprint.offset = std::uint64_t(a_frame.base + i) * kProbeSlotBytes;
					copy.footprint.rowPitch = kProbeSlotBytes;
					copy.footprint.width = 1;
					copy.footprint.height = 1;
					copy.footprint.depth = 1;
					commands.CopyTextureToBuffer(copy);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			bool after = false;
		};

		bool DebugViewEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_DEBUG_VIEW");
			}();
			return enabled;
		}

		// CS_DCLF_CULL=off|frustum: how BuildDrawsCS filters this frame's draws before it writes their
		// sequences. The objects come from the engine's accumulator, which has already frustum-culled them,
		// so frustum mode should reject almost nothing - a large rejection count means the projection here
		// disagrees with the one the draws use.
		std::uint32_t CullingMode()
		{
			static const std::uint32_t mode = [] {
				return SwitchValue("CS_DCLF_CULL") == "frustum" ? 1u : 0u;
			}();
			return mode;
		}

		// CS_DCLF_HYBRID=1: DCLF draws into the main pass's own targets and depth, and the native loop skips
		// the objects it drew (DrawcallLimitFix's RenderPassImmediately hooks).
		bool HybridEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_HYBRID");
			}();
			return enabled;
		}

		// CS_DCLF_BUILD_PARITY=1: compare BuildDraws' output with the CPU templates every 300 epochs.
		bool BuildParityEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_BUILD_PARITY");
			}();
			return enabled;
		}

		struct DebugViewBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> sources{}, destinations{};
		};

		struct DebugViewFrame
		{
			std::uint32_t count = 0, width = 0, height = 0;
		};

		// CS_DCLF_DEBUG_VIEW=1: copies DCLF's targets over the native ones, so the composited frame shows
		// only what the indirect draws produced.
		class DebugViewPass final : public org::TypedRenderGraphPass<DebugViewPass, DebugViewFrame, DebugViewBindings>
		{
		public:
			explicit DebugViewPass(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			DebugViewBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				DebugViewBindings bindings{};
				for (std::uint32_t i = 0; i < resources->targetCount; ++i) {
					bindings.sources[i] = a_builder.BindCopySource(resources->targets[i]);
					bindings.destinations[i] = a_builder.BindCopyDestination(resources->native[i]);
				}
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				a_out.push_back(RenderGraphRuntime::Get().CurrentSegment() == RenderGraphRuntime::Segment::DebugView);
			}

			DebugViewFrame Prepare(const DebugViewBindings&, const org::PassPrepareContext&) const
			{
				if (RenderGraphRuntime::Get().CurrentSegment() != RenderGraphRuntime::Segment::DebugView)
					return {};
				return { resources->targetCount, resources->width, resources->height };
			}

			static void Record(const DebugViewBindings& a_bindings, const DebugViewFrame& a_frame, org::PassRecordContext& a_recording)
			{
				auto& commands = a_recording.Commands();
				for (std::uint32_t i = 0; i < a_frame.count; ++i) {
					rhi::TextureCopyRegion destination{};
					destination.texture = a_recording.Resolve(a_bindings.destinations[i]).GetHandle();
					destination.width = a_frame.width;
					destination.height = a_frame.height;
					auto source = destination;
					source.texture = a_recording.Resolve(a_bindings.sources[i]).GetHandle();
					commands.CopyTextureRegion(destination, source);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
		};

		// A DXVK image the graph uses in place: imported without ownership, with simultaneous access (DXVK
		// keeps the images it hands out in GENERAL and uses them between the graph's commands).
		std::shared_ptr<org::ExternalTextureResource> ImportImage(rhi::Device a_device, const DxvkOrgInteropImageInfo& a_image, org::TextureDescription a_desc,
			const char* a_name)
		{
			rhi::vulkan::ImportedImageDesc import{};
			import.image = a_image.image;
			import.createInfo.flags = a_image.flags;
			import.createInfo.imageType = a_image.type;
			import.createInfo.format = a_image.format;
			import.createInfo.extent = a_image.extent;
			import.createInfo.mipLevels = a_image.mipLevels;
			import.createInfo.arrayLayers = a_image.arrayLayers;
			import.createInfo.samples = a_image.samples;
			import.createInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
			import.createInfo.usage = a_image.usage;
			import.createInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
			import.createInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			import.currentLayout = a_image.layout;
			import.simultaneousAccess = true;
			import.debugName = a_name;
			rhi::ResourcePtr resource;
			if (a_image.layout != VK_IMAGE_LAYOUT_GENERAL || rhi::vulkan::import_image(a_device, import, resource) != rhi::Result::Ok)
				return nullptr;
			a_desc.imageDimensions.clear();
			a_desc.imageDimensions.push_back({ a_image.extent.width, a_image.extent.height, 0, 0 });
			a_desc.initialLayout = rhi::ResourceLayout::Common;
			auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
			if (result)
				result->SetName(a_name);
			return result;
		}

		class MainOpaqueExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit MainOpaqueExtension(std::shared_ptr<Resources> a_resources) :
				resources(std::move(a_resources)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.constants"), resources->constants);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.records"), resources->records);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.sequences"), resources->sequences);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-inputs"), resources->inputs);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.geometries"), resources->geometries);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.draw-count"), resources->count);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.depth"), resources->depth);
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.target{}", i)), resources->targets[i]);
				for (const auto& frameBuffer : resources->frameBuffers)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
				for (std::uint32_t i = 0; i < resources->targetCount && resources->native[0]; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
				if (resources->nativeDepth)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources)))
						.PreferQueue(org::QueueKind::Graphics));
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, false))));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.main-opaque",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources))));
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, true))));
				if (resources->native[0] && !resources->hybrid)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.debug-view",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<DebugViewPass>(resources))));
			}

		private:
			std::shared_ptr<Resources> resources;
		};

		// The main pass's bindings at its first lighting draw.
		struct Capture
		{
			std::array<ID3D11Buffer*, kConstantBufferRegisters> vsBuffers{};
			std::array<ID3D11Buffer*, kConstantBufferRegisters> psBuffers{};
			std::array<ID3D11ShaderResourceView*, kTextureRegisters> psViews{};
			winrt::com_ptr<ID3D11Texture2D> depth;
			std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> targets;
			std::uint32_t targetCount = 0;
			std::uint32_t viewportWidth = 0, viewportHeight = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			// The camera the main pass draws with: the world transforms are stored relative to it, and the
			// engine has moved it on by the time the epoch packs the constants.
			RE::NiPoint3 eye, previousEye;

			void Release()
			{
				for (auto* buffer : vsBuffers)
					if (buffer)
						buffer->Release();
				for (auto* buffer : psBuffers)
					if (buffer)
						buffer->Release();
				for (auto* view : psViews)
					if (view)
						view->Release();
			}
		};

		Capture CaptureBindings()
		{
			auto* context = globals::d3d::context;
			Capture capture;
			context->VSGetConstantBuffers(0, kConstantBufferRegisters, capture.vsBuffers.data());
			context->PSGetConstantBuffers(0, kConstantBufferRegisters, capture.psBuffers.data());
			context->PSGetShaderResources(0, kTextureRegisters, capture.psViews.data());
			ID3D11RenderTargetView* views[kColorTargets] = {};
			ID3D11DepthStencilView* depth = nullptr;
			context->OMGetRenderTargets(kColorTargets, views, &depth);
			for (std::uint32_t i = 0; i < kColorTargets; ++i) {
				if (!views[i])
					continue;
				winrt::com_ptr<ID3D11Resource> resource;
				views[i]->GetResource(resource.put());
				capture.targets[i] = resource.try_as<ID3D11Texture2D>();
				capture.targetCount = i + 1;
				views[i]->Release();
			}
			if (depth) {
				winrt::com_ptr<ID3D11Resource> resource;
				depth->GetResource(resource.put());
				capture.depth = resource.try_as<ID3D11Texture2D>();
				depth->Release();
			}
			// Community Shaders binds its own per-frame pixel buffers - b5 SharedData and b6 FeatureData -
			// from Renderer_ResetState, so whether they happen to be bound when this capture is taken
			// depends on where the engine last reset its state. The hybrid path changes that, by skipping
			// the native draws in between, and the objects whose shaders read b5 were then dropped for
			// missing constants and rendered untextured. They are CS's own buffers with known identities,
			// so take them from CS instead of from whatever is bound.
			if (auto* state = globals::state) {
				auto adopt = [&](std::uint32_t a_slot, ConstantBuffer* a_buffer) {
					if (capture.psBuffers[a_slot] || !a_buffer || !a_buffer->CB())
						return;
					capture.psBuffers[a_slot] = a_buffer->CB();
					capture.psBuffers[a_slot]->AddRef();  // Capture::Release owns what it holds
				};
				adopt(kSharedDataRegister, state->sharedDataCB);
				adopt(kFeatureDataRegister, state->featureDataCB);
			}
			auto& shadowState = globals::game::shadowState->GetRuntimeData();
			capture.eye = shadowState.posAdjust.getEye();
			capture.previousEye = shadowState.previousPosAdjust.getEye();
			D3D11_VIEWPORT viewport{};
			UINT count = 1;
			context->RSGetViewports(&count, &viewport);
			capture.viewportWidth = static_cast<std::uint32_t>(viewport.Width);
			capture.viewportHeight = static_cast<std::uint32_t>(viewport.Height);
			capture.minDepth = viewport.MinDepth;
			capture.maxDepth = viewport.MaxDepth;
			return capture;
		}

		// The CPU-written structured buffers the main pass binds at t16 and up (Light Limit Fix's come from the graph).
		std::vector<FrameBuffer> FrameBuffersOf(const Capture& a_capture, bool a_lightLimitFix)
		{
			std::vector<FrameBuffer> result;
			for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters; ++t) {
				auto* view = a_capture.psViews[t];
				if (!view || (a_lightLimitFix && t >= kLightsRegister && t < kLightsRegister + 3))
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

		ID3D11Buffer* BufferOf(ID3D11ShaderResourceView* a_view)
		{
			winrt::com_ptr<ID3D11Resource> resource;
			a_view->GetResource(resource.put());
			return static_cast<ID3D11Buffer*>(resource.get());  // the view keeps it alive
		}

		// A growing byte arena of 256-byte aligned constant blocks.
		class ConstantArena
		{
		public:
			void Reset() { bytes.clear(); }

			// Offset of a zeroed block, or ~0 when the arena is full.
			std::uint64_t Allocate(std::size_t a_size)
			{
				const std::uint64_t offset = (bytes.size() + kConstantAlignment - 1) & ~(kConstantAlignment - 1);
				if (offset + a_size > kConstantBytes)
					return ~0ull;
				bytes.resize(offset + std::max<std::size_t>(a_size, 16));
				return offset;
			}

			std::span<std::byte> At(std::uint64_t a_offset, std::size_t a_size) { return { bytes.data() + a_offset, a_size }; }
			const std::vector<std::byte>& Bytes() const { return bytes; }

		private:
			std::vector<std::byte> bytes;
		};
	}

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::uint32_t pipelineGeneration = ~0u;
		std::uint64_t serial = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		ConstantArena arena;
		std::vector<DrawBindings> records;
		std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
		std::vector<DrawInput> inputs;
		std::vector<GeometryDraw> geometryDraws;

		// The frame each geometry was last drawn by DCLF: the native loop skips a pass whose geometry the
		// epoch drew. Only the colour epoch records it, so the native loop never skips an object that the
		// Z-prepass drew but the colour pass then left out (a missing texture, say), which would leave a
		// hole that writes depth and shows the background.
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> drawnFrame;
		// The vertex-stage inputs the Z-prepass wrote its depth with. The colour pass tests EQUAL against
		// that depth, so it has to transform the geometry to exactly the same place - and it cannot simply
		// read the buffers again, because the engine rewrites the per-frame constants between the depth
		// pass and the composite, and the camera moves on. Reusing the bytes makes the two epochs agree.
		std::array<std::vector<std::byte>, kConstantBufferRegisters> prepassVS;
		RE::NiPoint3 prepassEye, prepassPreviousEye;
		// The viewport transform the depth was written with. Skyrim's depth pass and its main pass do not
		// use the same depth range - 0.999968 against 0.999998 - and the range scales the value that lands
		// in the buffer, so the same vertex is written about 500 D24 units apart in the two passes. The
		// colour pass tests EQUAL against that depth, so it has to rasterise with the prepass's range.
		float prepassMinDepth = 0.0f, prepassMaxDepth = 1.0f;
		// The main pass's viewport depth range, kept from its capture. The Z-prepass writes its depth with
		// this rather than with the depth pass's own range, so that the depth in the buffer is what the
		// main pass - DCLF's colour draws and the native draws alike - tests EQUAL against. It is stable
		// frame to frame, so the previous frame's value is right for this frame's prepass.
		float mainMinDepth = 0.0f, mainMaxDepth = 0.0f;
		bool prepassInputs = false;
		std::uint32_t loggedDepthViewport = 0;
		std::uint32_t loggedColourViewport = 0;

		winrt::com_ptr<ID3D11Texture2D> mainPassDepth;  // what the main pass binds, to check the depth pass against
		bool loggedDepthMismatch = false;
		bool loggedNoViewProj = false;
		bool loggedFrameConstants = false;

		// CS_DCLF_GBUFFER_PROBE=<x>x<y>: the texel the in-epoch probe passes copy out, read back a few
		// frames later from the buffer they wrote.
		winrt::com_ptr<ID3D11Buffer> gbufferStaging;
		std::uint32_t gbufferFramesLeft = 0;
		std::uint32_t gbufferEpochs = 0;
		std::uint32_t gbufferCount = 0;
		std::uint32_t gbufferX = 0, gbufferY = 0;
		std::array<std::uint32_t, kColorTargets> gbufferBytes{};
		// The main pass's targets, kept past the capture so their formats can be named in the report.
		std::array<winrt::com_ptr<ID3D11Texture2D>, kColorTargets> probeTargets;
		winrt::com_ptr<ID3D11Texture2D> probeDepth;
		std::uint32_t probeTargetCount = 0;

		void ProbeGBuffer(const char* a_label);

		// CS_DCLF_BUILD_PARITY: GPU output copied to staging after an epoch, compared a few frames later.
		struct ParityReadback
		{
			winrt::com_ptr<ID3D11Buffer> sequences, count;
			std::vector<DrawSequence> expected;
			std::uint32_t framesLeft = 0;
		};
		std::optional<ParityReadback> parity;
		std::uint32_t parityEpochs = 0;

		// The culling's own counters, read back from the draw-count buffer a few frames after the epoch
		// wrote them. Always on: the per-phase counts are what says whether the culling is doing anything,
		// and the buffer is two words.
		struct CullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t framesLeft = 0;
		};
		std::optional<CullReadback> cullReadback;
		std::uint32_t cullEpochs = 0;

		void ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats);

		void CheckBuildParity(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats);

		std::uint32_t loggedReasons = 0;

		bool NotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((loggedReasons >> a_reason) & 1)) {
				loggedReasons |= 1u << a_reason;
				logger::info("[DCLF] Main-pass draws not ready: {}", a_what);
			}
			return false;
		}

		bool Setup(const Capture& a_capture, bool a_depthOnly = false)
		{
			auto* host = RenderGraphRuntime::Get().Host();
			if (!host)
				return NotReady(0, "no render graph");
			if (a_depthOnly) {
				// The native depth pass binds no colour targets, so there is nothing to build from here. The
				// Z-prepass reuses what the main pass built last frame; on the first frame there is none yet
				// and the prepass simply does not run.
				return resources != nullptr;
			}
			if (a_capture.targetCount == 0)
				return NotReady(1, "no render targets bound");
			D3D11_TEXTURE2D_DESC target{};
			a_capture.targets[0]->GetDesc(&target);
			const auto& targets = DrawPipelines::Get().Targets();
			std::uint32_t lights, list, grid;
			const bool lightLimitFix = ORGLightCulling::Get().GetShaderResourceIndices(lights, list, grid);
			auto frameBuffers = FrameBuffersOf(a_capture, lightLimitFix);
			const bool sameFrameBuffers = resources && resources->frameBuffers.size() == frameBuffers.size() &&
			                              std::equal(frameBuffers.begin(), frameBuffers.end(), resources->frameBuffers.begin(), [](const auto& a, const auto& b) { return a.SameShape(b); });
			if (resources && targets == formats && target.Width == width && target.Height == height && sameFrameBuffers &&
				resources->lightLimitFix == lightLimitFix)
				return true;

			auto device = host->GetDesc().device;
			auto state = std::make_shared<Resources>();
			auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
				auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
				created->SetName(a_name);
				return created;
			};
			state->constants = buffer(kConstantBytes, "cs.dclf.constants");
			state->records = buffer(std::uint64_t(kMaxDraws) * sizeof(DrawBindings), "cs.dclf.records");
			state->sequences = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
			state->count = CreateWords(4, true, "cs.dclf.draw-count");  // [0] draws written, [1] culled, [2] tested
			state->inputs = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs");
			state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries");
			state->buildDraws = LoadComputeProgram(device, kBuildDrawsShader, kBuildDrawsConstantWords);
			if (!state->buildDraws)
				return NotReady(7, "the BuildDraws program could not be created");
			if (BuildParityEnabled()) {
				auto wrap = [](org::Buffer& a_buffer, std::uint64_t a_bytes) {
					D3D11_BUFFER_DESC desc{};
					desc.ByteWidth = static_cast<UINT>(a_bytes);
					desc.Usage = D3D11_USAGE_DEFAULT;
					desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
					desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
					desc.StructureByteStride = sizeof(std::uint32_t);
					return RenderGraphRuntime::Get().WrapBuffer(a_buffer, desc);
				};
				state->sequencesD3D11 = wrap(*state->sequences, std::uint64_t(kMaxDraws) * sizeof(DrawSequence));

			}
			if (!SwitchValue("CS_DCLF_GBUFFER_PROBE").empty()) {
				state->probe = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, std::uint64_t(kProbeSlots) * kProbeSlotBytes, false);
				state->probe->SetName("cs.dclf.gbuffer-probe");
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kProbeSlots * kProbeSlotBytes;
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->probeD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->probe, desc);
			}
			{
				// The draw-count buffer is read back for the culling counters whether or not parity is on.
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = 4 * sizeof(std::uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->countD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->count, desc);
			}
			state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
			state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
			if (!state->constantsAddress || !state->recordsAddress) {
				logger::error("[DCLF] Draw data buffers have no device address");
				return false;
			}

			// Off-screen copies of the main pass's targets.
			state->targetCount = targets.colorCount;
			for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
				org::TextureDescription desc{};
				desc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
				desc.format = rhi::helpers::ToRHI(targets.colors[i]);
				desc.channels = 4;
				desc.hasRTV = true;
				desc.rtvFormat = desc.format;
				desc.hasSRV = true;
				desc.srvFormat = desc.format;
				desc.clearColor[3] = 0.0f;
				state->targets[i] = org::PixelBuffer::CreateSharedUnmaterialized(desc);  // the graph materializes them
				state->targets[i]->SetName(fmt::format("cs.dclf.target{}", i));
			}

			// DCLF's own depth, with the main depth's format.
			org::TextureDescription depthDesc{};
			depthDesc.imageDimensions.push_back({ target.Width, target.Height, 0, 0 });
			depthDesc.format = rhi::helpers::ToRHI(targets.depth);
			depthDesc.channels = 1;
			depthDesc.hasDSV = true;
			depthDesc.dsvFormat = depthDesc.format;
			{
				auto depth = org::PixelBuffer::CreateSharedUnmaterialized(depthDesc);
				depth->SetName("cs.dclf.depth");
				state->depth = std::move(depth);
			}
			state->width = target.Width;
			state->height = target.Height;

			state->hybrid = HybridEnabled();
			if (DebugViewEnabled() || state->hybrid) {
				for (std::uint32_t i = 0; i < targets.colorCount; ++i) {
					DxvkOrgInteropResourceInfo info{};
					if (!a_capture.targets[i] || !RenderGraphRuntime::Get().DescribeResource(a_capture.targets[i].get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
						return NotReady(5, "a main-pass target cannot be described");
					org::TextureDescription desc{};
					desc.format = rhi::helpers::ToRHI(targets.colors[i]);
					desc.channels = 4;
					desc.hasRTV = true;
					desc.rtvFormat = desc.format;
					state->native[i] = ImportImage(device, info.image, desc, "DCLF native target");
					if (!state->native[i])
						return NotReady(6, "a main-pass target could not be imported");
				}
			}
			if (state->hybrid) {
				DxvkOrgInteropResourceInfo info{};
				if (!a_capture.depth || !RenderGraphRuntime::Get().DescribeResource(a_capture.depth.get(), info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
					return NotReady(10, "the main-pass depth cannot be described (hybrid)");
				state->nativeDepth = ImportImage(device, info.image, depthDesc, "DCLF native depth");
				if (!state->nativeDepth)
					return NotReady(11, "the main-pass depth could not be imported (hybrid)");
			}
			// Bisect: with the debug view on as well, the hybrid path keeps its native depth and its skipping
			// but draws colour into DCLF's own targets, which are then copied over the native ones. If that
			// is correct while drawing straight into the native targets is not, the difference is the
			// destination rather than anything DCLF assembles or draws.
			const bool offscreenColour = !state->hybrid || DebugViewEnabled();
			state->offscreen = offscreenColour;
			for (std::uint32_t i = 0; i < targets.colorCount; ++i)
				state->drawTargets[i] = offscreenColour ? std::static_pointer_cast<org::Resource>(state->targets[i]) : std::static_pointer_cast<org::Resource>(state->native[i]);
			state->drawDepth = state->hybrid ? std::static_pointer_cast<org::Resource>(state->nativeDepth) : state->depth;

			state->lightLimitFix = lightLimitFix;
			for (auto& frameBuffer : frameBuffers) {
				frameBuffer.copy = org::Buffer::CreateUnmaterializedStructuredBuffer(frameBuffer.elements, frameBuffer.stride, false);
				frameBuffer.copy->SetName(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister));
				frameBuffer.copy->Materialize();
			}
			state->frameBuffers = std::move(frameBuffers);

			resources = state;
			formats = targets;
			width = target.Width;
			height = target.Height;
			// The graph is rebuilt with the new resources on its next epoch.
			host->AddExtension(kExtensionId, [state] { return std::make_unique<MainOpaqueExtension>(state); });
			logger::info("[DCLF] Main-pass graph resources: {} targets {}x{}, depth format {}", targets.colorCount, width, height, static_cast<int>(targets.depth));
			return true;
		}
	};

	IndirectDraws::IndirectDraws() :
		impl(std::make_unique<Impl>())
	{}

	IndirectDraws::~IndirectDraws() = default;

	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}

	bool IndirectDraws::Enabled() const
	{
		return DrawPipelines::Get().Enabled();
	}

	bool IndirectDraws::Hybrid()
	{
		return HybridEnabled();
	}

	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry* a_geometry, std::uint32_t a_frame) const
	{
		const auto drawn = impl->drawnFrame.find(a_geometry);
		return drawn != impl->drawnFrame.end() && a_frame - drawn->second <= 1;
	}

	void IndirectDraws::CaptureMainPass()
	{
		if (impl->pending)
			impl->pending->Release();
		impl->pending = CaptureBindings();
		impl->mainPassDepth = impl->pending->depth;
		impl->mainMinDepth = impl->pending->minDepth;
		impl->mainMaxDepth = impl->pending->maxDepth;
		impl->probeTargetCount = std::min<std::uint32_t>(impl->pending->targetCount, kColorTargets);
		for (std::uint32_t i = 0; i < impl->probeTargetCount; ++i)
			impl->probeTargets[i] = impl->pending->targets[i];
		impl->probeDepth = impl->pending->depth;
	}


	void IndirectDraws::CaptureDepthPass()
	{
		// CS_DCLF_NO_ZPREPASS=1: leave the depth to the native pass, so the hybrid path runs a single epoch
		// per frame again. Its objects are then missing from the depth the rest of the frame reads, which is
		// only useful for telling a one-epoch frame apart from a two-epoch one.
		static const bool skip = SwitchEnabled("CS_DCLF_NO_ZPREPASS");
		if (!Hybrid() || skip)
			return;
		auto capture = CaptureBindings();
		// The Z-prepass writes into the depth the native pass just finished, which has to be the one the
		// main pass then tests against; otherwise DCLF's objects would be written somewhere nothing reads.
		if (!capture.depth || (impl->mainPassDepth && capture.depth.get() != impl->mainPassDepth.get())) {
			if (!impl->loggedDepthMismatch) {
				impl->loggedDepthMismatch = true;
				logger::warn("[DCLF] The depth pass and the main pass bind different depth textures; the Z-prepass stays with the main pass");
			}
			capture.Release();
			return;
		}
		if (impl->pending)
			impl->pending->Release();
		impl->pending = std::move(capture);
		impl->probeDepth = impl->pending->depth;
		ProbeTargets(kProbeFirstLabel);
		RunEpoch(RenderGraphRuntime::Segment::ZPrepass);
		ProbeTargets("after z-prepass");
	}

	void IndirectDraws::Execute()
	{
		if (Hybrid())
			return;  // the hybrid path runs the two segments separately
		RunEpoch(RenderGraphRuntime::Segment::MainOpaque);
	}

	void IndirectDraws::ProbeTargets(const char* a_label)
	{
		impl->ProbeGBuffer(a_label);
	}

	void IndirectDraws::ExecuteColour()
	{
		// The colour epoch assembles from the main pass's own capture: the Z-prepass ran off the depth
		// pass's, where the pixel-stage bindings were not available. Both draw the same tables with the
		// same camera, so the depths agree and the colour pass can test EQUAL.
		if (Hybrid())
			RunEpoch(RenderGraphRuntime::Segment::MainOpaque);
	}

	void IndirectDraws::RunEpoch(RenderGraphRuntime::Segment a_segment)
	{
		const auto start = std::chrono::steady_clock::now();
		if (!impl->pending)
			return;
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (failed || !lighting || !pipelines.Enabled() || !GetIndirectState().valid) {
			capture.Release();
			return;
		}
		const bool depthOnly = a_segment == RenderGraphRuntime::Segment::ZPrepass;
		bool ready = false;
		try {
			ready = impl->Setup(capture, depthOnly);
		} catch (const std::exception& e) {
			// Never retried: the feature stays on the native path.
			logger::error("[DCLF] Main-pass graph resources could not be created: {}", e.what());
			failed = true;
		}
		if (!ready) {
			capture.Release();
			++stats.notReady;
			return;
		}

		auto& store = SceneStore::Get();
		const std::uint32_t frameNumber = store.GetFrame();
		const auto& tables = store.GetTables();
		auto& programs = ShaderPrograms::Get();
		auto& cache = SIE::ShaderCache::Instance();
		auto& textures = GpuTextures::Get();
		auto resources = impl->resources;
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;
		std::uint32_t missingNext = 0;

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) {
			auto& arena = impl->arena;
			auto& records = impl->records;
			auto& sequences = impl->sequences;
			arena.Reset();
			records.clear();
			sequences.clear();
			impl->inputs.clear();
			// CS_DCLF_NO_PREPASS_TEXTURES=1: the Z-prepass epoch does not touch the texture system at all, to
			// tell apart a colour epoch whose descriptors are its own from one that inherits slots the depth
			// epoch allocated earlier in the same frame.
			static const bool bareDepthTextures = SwitchEnabled("CS_DCLF_NO_PREPASS_TEXTURES");
			const bool resolveTextures = !depthOnly || !bareDepthTextures;
			if (resolveTextures)
				textures.BeginFrame(store.GetFrame());
			const std::uint64_t base = resources->constantsAddress;

			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			// Per-frame constant buffers: whatever the main pass binds outside the per-draw slots.
			// On the Z-prepass the pixel-stage per-frame bindings are skipped entirely: the native depth pass
			// has not bound the main pass's yet, and the DCLF_DEPTH_ONLY build of the pixel stage compiles
			// away everything that would read them.
			std::array<std::uint64_t, kConstantBufferRegisters> frameVS{}, framePS{};
			auto& mirror = ConstantMirror::Get();
			// On the hybrid path the colour epoch replays the vertex-stage bytes the Z-prepass used, so the
			// two agree to the bit and the colour pass's EQUAL test passes.
			const bool replayVertexInputs = !depthOnly && resources->hybrid && impl->prepassInputs;
			if (depthOnly)
				for (auto& bytes : impl->prepassVS)
					bytes.clear();
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if (replayVertexInputs && !((kPerDrawVS >> slot) & 1)) {
					const auto& bytes = impl->prepassVS[slot];
					if (!bytes.empty())
						frameVS[slot] = block(bytes.data(), bytes.size());
				}
				for (auto [buffers, perDraw, out] : { std::tuple{ &capture.vsBuffers, kPerDrawVS, &frameVS }, std::tuple{ &capture.psBuffers, kPerDrawPS, &framePS } }) {
					if (depthOnly && out == &framePS)
						continue;
					if (replayVertexInputs && out == &frameVS)
						continue;
					auto* buffer = (*buffers)[slot];
					if (!buffer || ((perDraw >> slot) & 1))
						continue;
					mirror.Watch(buffer);
					const auto contents = mirror.Contents(buffer);
					if (!contents.empty())
						(*out)[slot] = block(contents.data(), contents.size());
					if (depthOnly && out == &frameVS && !contents.empty())
						impl->prepassVS[slot].assign(contents.begin(), contents.end());
				}
			}

			// b12 is the game's PerFrame buffer. The constant mirror does not always hold its contents - the
			// engine does not rewrite it through the hooked context every frame - and an empty slot drops
			// every object whose shaders read it, which left distant buildings unshaded. Community Shaders
			// already caches this buffer between Map and Unmap for its own use, so take it from there.
			{
				const auto& cached = globals::game::frameBufferCached.data;
				if (!frameVS[kPerFrameVertexRegister])
					frameVS[kPerFrameVertexRegister] = block(&cached, sizeof(cached));
				if (!framePS[kPerFrameVertexRegister])
					framePS[kPerFrameVertexRegister] = block(&cached, sizeof(cached));
			}

			// b5 is Community Shaders' own SharedData, written through its ConstantBuffer helper rather than
			// through the hooked device context, so the constant mirror never observes the write and the
			// slot would stay empty - which dropped every object whose shader reads it (the untextured
			// architecture in Dragonsreach). CS keeps the struct it uploaded, so pack that directly.
			// These three are supplied in the Z-prepass too. Its pixel stage is the DCLF_DEPTH_ONLY build,
			// which still reads them in the code that runs before the alpha test, and they all come from
			// Community Shaders or the game's own cache rather than from the main pass's bindings - so they
			// are available during the native depth pass, where the main pass's bindings are not.
			if (auto* state = globals::state) {
				framePS[kSharedDataRegister] = block(&state->lastSharedData, sizeof(State::SharedDataCB));
				if (!state->lastFeatureData.empty())
					framePS[kFeatureDataRegister] = block(state->lastFeatureData.data(), state->lastFeatureData.size());
			}

			// One line saying, per pixel register, whether the pass had a buffer bound and whether the
			// constant mirror could supply its contents: a slot that is bound but not mirrored ends up as a
			// null address and drops every object whose shader reads it.
			if (!depthOnly && !impl->loggedFrameConstants) {
				impl->loggedFrameConstants = true;
				std::string text;
				for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
					if ((kPerDrawPS >> slot) & 1)
						continue;
					text += fmt::format("{}b{}={}/{}", text.empty() ? "" : " ", slot, capture.psBuffers[slot] ? "bound" : "unbound",
						framePS[slot] ? "supplied" : "empty");
				}
				logger::info("[DCLF] main-pass per-frame pixel constants: {}", text);
			}

			// Per-frame textures (t16 and up), and Light Limit Fix's graph buffers.
			std::array<std::uint32_t, kTextureRegisters> frameTextures;
			frameTextures.fill(kInvalidIndex);
			for (std::uint32_t t = 16; t < kTextureRegisters && !depthOnly; ++t)
				frameTextures[t] = textures.Resolve(capture.psViews[t]);
			if (resources->lightLimitFix && !depthOnly)
				ORGLightCulling::Get().GetShaderResourceIndices(frameTextures[kLightsRegister], frameTextures[kLightsRegister + 1], frameTextures[kLightsRegister + 2]);
			for (const auto& frameBuffer : depthOnly ? decltype(resources->frameBuffers){} : resources->frameBuffers) {
				auto* buffer = BufferOf(capture.psViews[frameBuffer.textureRegister]);
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
				const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
				if (contents.size() < offset + bytes)
					continue;  // not written since it is watched
				BUFFER_UPLOAD(contents.data() + offset, bytes, org::runtime::UploadTarget::FromShared(frameBuffer.copy), 0);
				frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
			}

			// Blocks shared by many objects.
			struct PipelineBlocks
			{
				std::uint32_t setIndex = DrawPipelines::kNotReady;
				RE::BSGraphics::VertexShader* vs = nullptr;
				RE::BSGraphics::PixelShader* ps = nullptr;
				std::uint64_t techniqueVS = 0, techniquePS = 0;
			};
			std::vector<PipelineBlocks> pipelineBlocks(tables.pipelines.size());
			for (std::size_t p = 0; p < tables.pipelines.size(); ++p) {
				const auto& key = tables.pipelines[p];
				auto& blocks = pipelineBlocks[p];
				const auto* program = programs.Find(key, *lighting);
				blocks.setIndex = program ? pipelines.Find(key, *program) : DrawPipelines::kNotReady;
				blocks.vs = cache.GetVertexShader(*lighting, key.vertexDescriptor);
				blocks.ps = cache.GetPixelShader(*lighting, key.pixelDescriptor);
				if (blocks.setIndex == DrawPipelines::kNotReady || !blocks.vs || !blocks.ps) {
					blocks.setIndex = DrawPipelines::kNotReady;
					continue;
				}
				const auto& technique = tables.techniqueConstants[p];
				auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first) {
					const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
					const auto address = block(nullptr, size);
					if (address)
						PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, arena.At(address - base, std::max<std::size_t>(size, 16)));
					return address;
				};
				blocks.techniqueVS = pack(technique.vs, LightingVSLayout(), blocks.vs->constantTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique]);
				blocks.techniquePS = pack(technique.ps, LightingPSLayout(), blocks.ps->constantTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique]);
			}

			ankerl::unordered_dense::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> materialBlocks;  // (material, pipeline) -> VS, PS
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> lightBlocks;        // (room, shadow mask)
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationBlocks;  // (pipeline, extra bits)
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> alphaBlocks;        // threshold
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> emissiveBlocks;     // Linear Lighting multiplier bits
			const bool linearLighting = globals::features::linearLighting.loaded && globals::features::linearLighting.settings.enableLinearLighting;
			const auto renderFlags = store.GetMainPassRenderFlags();

			auto skip = [&](Skip a_reason) { ++stats.skipped[static_cast<std::size_t>(a_reason)]; };
			const bool frameHybrid = resources->hybrid;
			for (std::uint32_t o = 0; o < tables.objects.size(); ++o) {
				const auto& object = tables.objects[o];
				const auto& blocks = pipelineBlocks[object.pipelineIndex];
				if (blocks.setIndex == DrawPipelines::kNotReady) {
					skip(Skip::Pipeline);
					continue;
				}
				const auto& geometry = tables.geometries[object.geometryIndex];
				if (!geometry.vertexAddress || !geometry.indexAddress) {
					skip(Skip::Geometry);
					continue;
				}
				// The Z-prepass must write depth for exactly the objects the native loop is leaving to DCLF,
				// which is the set the colour epoch drew in the frame before (SkipNativePass uses the same
				// rule). Writing depth for anything else strands it: the colour epoch may not draw it, and
				// the native draw that would have cannot either, because its own EQUAL test now compares
				// against a depth DCLF computed rather than the one the native prepass wrote. Such an object
				// keeps its depth but is never shaded, which is what left the architecture flat and grey.
				if (depthOnly && frameHybrid && !(o < tables.objectGeometry.size() && DrewLastFrame(tables.objectGeometry[o], frameNumber))) {
					skip(Skip::NotSkippedNatively);
					continue;
				}
				if (records.size() >= kMaxDraws) {
					skip(Skip::Capacity);
					continue;
				}
				const auto& usage = pipelines.Usage(blocks.setIndex, depthOnly ? kDepthVariant : kColorVariant);
				const auto& material = tables.materials[object.materialIndex];
				const auto& technique = tables.techniqueConstants[object.pipelineIndex];
				DrawBindings bindings{};

				// Textures: the material's, the technique's shadow mask, then the frame's.
				bool texturesOk = true;
				for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
					// Slots below 16 the material and technique leave alone read a null view (natively: whatever
					// an earlier draw left bound; Phase 3 parity checks it).
					std::uint32_t index = t < kPixelTextureSlots ? textures.NullIndex() : frameTextures[t];
					if (!resolveTextures)
						index = 0;
					else if (t < kPixelTextureSlots && ((material.textureWritten >> t) & 1))
						index = textures.Resolve(material.textures[t]);
					else if (t == kShadowMaskSlot && technique.shadowMask)
						index = textures.Resolve(technique.shadowMaskTexture);
					if (index == kInvalidIndex && usage.UsesTexture(t)) {
						texturesOk = false;
						if (missingNext < stats.missingTextures.size())
							stats.missingTextures[missingNext++] = t;
						break;
					}
					bindings.textures[t] = index == kInvalidIndex ? 0 : index;
				}
				if (!texturesOk) {
					skip(Skip::Texture);
					continue;
				}

				// Samplers: the modes the material (or, for the shadow mask, the technique) sets.
				bool samplersOk = true;
				for (std::uint32_t s = 0; s < kSamplerRegisters; ++s) {
					std::uint32_t address = 0, filter = 0;
					if (s < kPixelTextureSlots && ((material.textureWritten >> s) & 1)) {
						address = material.addressModes[s];
						filter = material.filterModes[s] != kUnwrittenFilterMode ? material.filterModes[s] : technique.filterModes[s];
					} else if (s == kShadowMaskSlot && technique.shadowMask) {
						filter = technique.filterModes[s];
					}
					if (filter == kUnwrittenFilterMode)
						filter = 0;
					const auto index = resolveTextures ? textures.Sampler(address, filter) : 0u;
					if (index == kInvalidIndex && ((usage.samplers >> s) & 1)) {
						samplersOk = false;
						break;
					}
					bindings.samplers[s] = index == kInvalidIndex ? 0 : index;
				}
				if (!samplersOk) {
					skip(Skip::Sampler);
					continue;
				}

				// Constant buffers.
				auto& materialBlock = materialBlocks[(std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex];
				if (!materialBlock.first && !materialBlock.second) {
					auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first) {
						const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
						const auto address = block(nullptr, size);
						if (address)
							PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, arena.At(address - base, std::max<std::size_t>(size, 16)));
						return address;
					};
					materialBlock.first = pack(material.vs, LightingVSLayout(), blocks.vs->constantTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial]);
					materialBlock.second = pack(material.ps, LightingPSLayout(), blocks.ps->constantTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial]);
				}
				// Camera-relative world matrices: the colour epoch must use the eye the Z-prepass used, or the
				// same vertex lands somewhere else and the EQUAL test rejects it.
				const auto& eye = replayVertexInputs ? impl->prepassEye : capture.eye;
				const auto& previousEye = replayVertexInputs ? impl->prepassPreviousEye : capture.previousEye;
				const auto geometryConstants = ObjectGeometryConstants(tables, o, renderFlags, eye, previousEye);
				std::uint64_t geometryVS = 0, geometryPS = 0;
				{
					const auto vsSize = ConstantGroupSize(LightingVSLayout(), blocks.vs->constantTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry]);
					const auto psSize = ConstantGroupSize(LightingPSLayout(), blocks.ps->constantTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry]);
					geometryVS = block(nullptr, vsSize);
					geometryPS = block(nullptr, psSize);
					if (geometryVS)
						PackConstantGroup(geometryConstants.vs, LightingVSLayout(), blocks.vs->constantTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry],
							arena.At(geometryVS - base, std::max<std::size_t>(vsSize, 16)));
					if (geometryPS)
						PackConstantGroup(geometryConstants.ps, LightingPSLayout(), blocks.ps->constantTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry],
							arena.At(geometryPS - base, std::max<std::size_t>(psSize, 16)));
				}
				const auto& lights = tables.lights[o];
				auto& lightBlock = lightBlocks[(std::uint64_t(static_cast<std::uint32_t>(lights.roomIndex)) << 32) | lights.shadowBitMask];
				if (!lightBlock) {
					const std::uint32_t header[4] = { 0, static_cast<std::uint32_t>(lights.roomIndex), lights.shadowBitMask, 0 };
					lightBlock = block(nullptr, kStrictLightDataBytes);
					if (lightBlock)
						std::memcpy(arena.At(lightBlock - base, sizeof(header)).data(), header, sizeof(header));
				}
				const auto& permutation = tables.permutations[object.pipelineIndex];
				const std::uint32_t extra = permutation.extraShaderDescriptor |
				                            ((object.flags & kObjectSuppressExternalEmittance) ? static_cast<std::uint32_t>(State::ExtraShaderDescriptors::SuppressExternalEmittance) : 0u);
				auto& permutationBlock = permutationBlocks[(std::uint64_t(object.pipelineIndex) << 32) | extra];
				if (!permutationBlock) {
					const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, extra, permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
					permutationBlock = block(data, sizeof(data));
				}
				const std::uint32_t threshold = (object.flags & kObjectAlphaTest) ? (object.flags >> kObjectAlphaThresholdShift) & 0xFF : 0;
				auto& alphaBlock = alphaBlocks[threshold];
				if (!alphaBlock) {
					const float data[4] = { threshold / 255.0f, 0, 0, 0 };
					alphaBlock = block(data, sizeof(data));
				}

				std::copy(frameVS.begin(), frameVS.end(), bindings.vertexConstants);
				std::copy(framePS.begin(), framePS.end(), bindings.pixelConstants);
				bindings.vertexConstants[kPerTechnique] = blocks.techniqueVS;
				bindings.vertexConstants[kPerMaterial] = materialBlock.first;
				bindings.vertexConstants[kPerGeometry] = geometryVS;
				bindings.vertexConstants[4] = permutationBlock;
				bindings.pixelConstants[kPerTechnique] = blocks.techniquePS;
				bindings.pixelConstants[kPerMaterial] = materialBlock.second;
				bindings.pixelConstants[kPerGeometry] = geometryPS;
				bindings.pixelConstants[3] = lightBlock;
				bindings.pixelConstants[4] = permutationBlock;
				bindings.pixelConstants[11] = alphaBlock;
				// Linear Lighting binds its multiplier per draw only while enabled; otherwise the shader
				// does not read it.
				{
					const auto* property = static_cast<const RE::BSLightingShaderProperty*>(tables.objectGeometry[o]->GetGeometryRuntimeData().shaderProperty.get());
					const float multiplier = linearLighting ? property->emissiveMult : 1.0f;
					auto& emissiveBlock = emissiveBlocks[std::bit_cast<std::uint32_t>(multiplier)];
					if (!emissiveBlock) {
						const float data[4] = { multiplier, 0, 0, 0 };
						emissiveBlock = block(data, sizeof(data));
					}
					bindings.pixelConstants[kLinearLightingRegister] = emissiveBlock;
				}
				bool constantsOk = true;
				for (std::uint32_t b = 0; b < kConstantBufferRegisters; ++b) {
					if (((usage.vertexConstants >> b) & 1) && !bindings.vertexConstants[b]) {
						constantsOk = false;
						stats.missingVertexConstants |= 1u << b;
					}
					if (((usage.pixelConstants >> b) & 1) && !bindings.pixelConstants[b]) {
						constantsOk = false;
						stats.missingPixelConstants |= 1u << b;
					}
				}
				if (!constantsOk) {
					skip(Skip::Constants);
					continue;
				}

				// The CPU template of what BuildDraws writes (checked with CS_DCLF_BUILD_PARITY).
				auto sequence = tables.draws[o];
				sequence.pipelineIndex = blocks.setIndex;
				sequence.bindingsAddress = resources->recordsAddress + records.size() * sizeof(DrawBindings);
				impl->inputs.push_back({ blocks.setIndex, static_cast<std::uint32_t>(records.size()), object.geometryIndex, object.flags,
					{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius });
				records.push_back(bindings);
				sequences.push_back(sequence);
				if (!depthOnly && o < tables.objectGeometry.size())
					impl->drawnFrame[tables.objectGeometry[o]] = frameNumber;
				// The indirect draw fetches vertices and indices through the buffer's device address and size:
				// a slice that does not cover the draw reads zeros, and the object collapses without any
				// other sign. Checked here because nothing on the D3D11 side sees the Vulkan slice.
				const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
				const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
				if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
					++stats.shortBuffers;
					if (stats.shortBuffers == 1)
						logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
							tables.objectGeometry[o] ? tables.objectGeometry[o]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride, vertexNeeded,
							geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, indexNeeded, geometry.indexBytes);
				}
			}

			// Upload (the graph's upload pass runs ahead of every pass of this epoch).
			const auto& bytes = arena.Bytes();
			if (!bytes.empty())
				BUFFER_UPLOAD(bytes.data(), bytes.size(), org::runtime::UploadTarget::FromShared(resources->constants), 0);
			// Draw inputs and the geometry table for BuildDraws, and its count reset.
			auto& geometryDraws = impl->geometryDraws;
			geometryDraws.resize(std::min<std::size_t>(tables.geometries.size(), kMaxGeometries));
			for (std::size_t g = 0; g < geometryDraws.size(); ++g) {
				const auto& geometry = tables.geometries[g];
				geometryDraws[g] = { geometry.vertexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.vertexBytes, UINT32_MAX)), geometry.vertexStride,
					geometry.indexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.indexBytes, UINT32_MAX)), geometry.indexCount, geometry.firstIndex, 0 };
			}
			const std::uint32_t zero[4] = { 0, 0, 0, 0 };
			BUFFER_UPLOAD(zero, sizeof(zero), org::runtime::UploadTarget::FromShared(resources->count), 0);
			if (!records.empty()) {
				BUFFER_UPLOAD(records.data(), records.size() * sizeof(DrawBindings), org::runtime::UploadTarget::FromShared(resources->records), 0);
				BUFFER_UPLOAD(impl->inputs.data(), impl->inputs.size() * sizeof(DrawInput), org::runtime::UploadTarget::FromShared(resources->inputs), 0);
				BUFFER_UPLOAD(geometryDraws.data(), geometryDraws.size() * sizeof(GeometryDraw), org::runtime::UploadTarget::FromShared(resources->geometries), 0);
			}
			stats.uploadBytes = bytes.size() + records.size() * (sizeof(DrawBindings) + sizeof(DrawInput)) + geometryDraws.size() * sizeof(GeometryDraw);

			auto frame = std::make_shared<PassFrame>();
			frame->serial = ++impl->serial;
			frame->drawCount = static_cast<std::uint32_t>(sequences.size());
			frame->width = capture.viewportWidth;
			frame->height = capture.viewportHeight;
			// Both epochs rasterise with the main pass's depth range; see Impl::mainMinDepth.
			const bool useMainRange = resources->hybrid && impl->mainMaxDepth > 0.0f;
			frame->minDepth = useMainRange ? impl->mainMinDepth : capture.minDepth;
			frame->maxDepth = useMainRange ? impl->mainMaxDepth : capture.maxDepth;
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = GetIndirectState();
			frame->cullMode = CullingMode();
			frame->eye = capture.eye;
			// The main pass's ViewProj (VS_PerFrame c8), which the draws project with: the culling has to
			// use the same matrix or it would reject what the draws would have put on screen.
			if (auto* buffer = capture.vsBuffers[kPerFrameVertexRegister]) {
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				if (contents.size() >= 48 * sizeof(float)) {
					std::memcpy(frame->viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
					frame->hasViewProj = true;
				}
			}
			if (!impl->loggedNoViewProj) {
				impl->loggedNoViewProj = true;
				logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", frame->cullMode, frame->hasViewProj ? "yes" : "no",
					kPerFrameVertexRegister, capture.vsBuffers[kPerFrameVertexRegister] ? "bound" : "not bound");
			}
			frame->hybrid = resources->hybrid;
			frame->offscreen = resources->offscreen;
			if (const auto pixel = SwitchValue("CS_DCLF_GBUFFER_PROBE"); !pixel.empty()) {
				if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
					frame->probeX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
					frame->probeY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
					frame->probePixel = frame->probeX < frame->width && frame->probeY < frame->height;
				}
			}
			// The two epochs must rasterise into the same pixels for the colour pass's EQUAL test to have any
			// chance: a different viewport or depth range between the depth pass and the main pass puts the
			// same vertex on a different pixel, at a different depth.
			if ((depthOnly ? impl->loggedDepthViewport : impl->loggedColourViewport)++ % 480 == 0) {
				// The z row of the ViewProj each epoch actually packs: this is what turns a vertex into the
				// depth the test compares, so if the two epochs differ it shows up here.
				std::string viewProjZ = "(none)";
				{
					std::span<const std::byte> perFrame;
					if (replayVertexInputs)
						perFrame = impl->prepassVS[kPerFrameVertexRegister];
					else if (auto* buffer = capture.vsBuffers[kPerFrameVertexRegister])
						perFrame = mirror.Contents(buffer);
					if (perFrame.size() >= 48 * sizeof(float)) {
						const auto* floats = reinterpret_cast<const float*>(perFrame.data());
						viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
					}
				}
				logger::info("[DCLF] {} epoch: render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
					depthOnly ? "z-prepass" : "colour", frame->width, frame->height, frame->minDepth, frame->maxDepth,
					capture.minDepth, capture.maxDepth, replayVertexInputs ? "on" : "off", capture.eye.x, capture.eye.y, capture.eye.z, viewProjZ);
			}
			if (depthOnly) {
				impl->prepassEye = capture.eye;
				impl->prepassPreviousEye = capture.previousEye;
				impl->prepassMinDepth = capture.minDepth;
				impl->prepassMaxDepth = capture.maxDepth;
				impl->prepassInputs = true;
			}
			stats.drawn = frame->drawCount;
			resources->frame.store(std::move(frame), std::memory_order_release);
		});
		capture.Release();
		++stats.epochs;
		if (ok && BuildParityEnabled())
			impl->CheckBuildParity(resources, stats);
		if (ok)
			impl->ReadCullCounters(resources, stats);

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	void IndirectDraws::Impl::ProbeGBuffer(const char* a_label)
	{
		(void)a_label;
		if (!resources || !resources->probeD3D11)
			return;
		auto* context = globals::d3d::context;
		if (gbufferStaging) {
			if (--gbufferFramesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(gbufferStaging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
				auto slots = [&](std::uint32_t a_base) {
					std::string text;
					for (std::uint32_t i = 0; i < gbufferCount; ++i) {
						const auto* texel = bytes + std::size_t(a_base + i) * kProbeSlotBytes;
						std::string hex;
						for (std::uint32_t b = 0; b < gbufferBytes[i]; ++b)
							hex += fmt::format("{:02X}", texel[b]);
						text += fmt::format("{}rt{}={}", text.empty() ? "" : " | ", i, hex);
					}
					return text;
				};
				auto depthAt = [&](std::uint32_t a_slot) {
					std::uint32_t packed = 0;
					std::memcpy(&packed, bytes + std::size_t(a_slot) * kProbeSlotBytes, sizeof(packed));
					return packed & 0x00FFFFFFu;
				};
				logger::info("[DCLF] G-buffer at ({}, {}) before DCLF's colour draws: {}", gbufferX, gbufferY, slots(0));
				logger::info("[DCLF] G-buffer at ({}, {}) after  DCLF's colour draws: {}", gbufferX, gbufferY, slots(kColorTargets));
				logger::info("[DCLF] depth at ({}, {}): {:06X} after the z-prepass, {:06X} when the colour pass tests it, {:06X} after it",
					gbufferX, gbufferY, depthAt(kProbeDepthAfterPrepass), depthAt(kProbeDepthBeforeColour), depthAt(kProbeDepthAfterColour));
				context->Unmap(gbufferStaging.get(), 0);
			}
			gbufferStaging = nullptr;
			return;
		}
		if ((gbufferEpochs++ % 480) != 0)
			return;
		D3D11_BUFFER_DESC desc{};
		resources->probeD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		winrt::com_ptr<ID3D11Buffer> staging;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, staging.put())))
			return;
		context->CopyResource(staging.get(), resources->probeD3D11.get());
		gbufferStaging = std::move(staging);
		gbufferFramesLeft = 4;
		gbufferCount = probeTargetCount;
		for (std::uint32_t i = 0; i < gbufferCount; ++i) {
			D3D11_TEXTURE2D_DESC textureDesc{};
			if (probeTargets[i])
				probeTargets[i]->GetDesc(&textureDesc);
			gbufferBytes[i] = FormatBytes(textureDesc.Format);
		}
		if (const auto pixel = SwitchValue("CS_DCLF_GBUFFER_PROBE"); !pixel.empty()) {
			if (const auto sep = pixel.find_first_of(",x"); sep != std::string::npos) {
				gbufferX = static_cast<std::uint32_t>(std::strtoul(pixel.substr(0, sep).c_str(), nullptr, 10));
				gbufferY = static_cast<std::uint32_t>(std::strtoul(pixel.substr(sep + 1).c_str(), nullptr, 10));
			}
		}
	}

	void IndirectDraws::Impl::ReadCullCounters(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (cullReadback) {
			if (--cullReadback->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(cullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				context->Unmap(cullReadback->count.get(), 0);
			}
			cullReadback.reset();
			return;
		}
		if ((cullEpochs++ % 120) != 0 || !a_resources->countD3D11)
			return;
		D3D11_BUFFER_DESC desc{};
		a_resources->countD3D11->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		CullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		context->CopyResource(readback.count.get(), a_resources->countD3D11.get());
		readback.framesLeft = 3;
		cullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::CheckBuildParity(const std::shared_ptr<Resources>& a_resources, IndirectDraws::Stats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (parity) {
			if (--parity->framesLeft)
				return;
			D3D11_MAPPED_SUBRESOURCE countMap{}, sequencesMap{};
			if (FAILED(context->Map(parity->count.get(), 0, D3D11_MAP_READ, 0, &countMap)) ||
				FAILED(context->Map(parity->sequences.get(), 0, D3D11_MAP_READ, 0, &sequencesMap))) {
				parity.reset();
				return;
			}
			const std::uint32_t count = static_cast<const std::uint32_t*>(countMap.pData)[0];
			const std::uint32_t culled = static_cast<const std::uint32_t*>(countMap.pData)[1];
			std::vector<DrawSequence> built(static_cast<const DrawSequence*>(sequencesMap.pData), static_cast<const DrawSequence*>(sequencesMap.pData) + std::min<std::size_t>(count, kMaxDraws));
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order: compare as sets, keyed by the record address.
			auto byRecord = [](const DrawSequence& a, const DrawSequence& b) { return a.bindingsAddress < b.bindingsAddress; };
			std::sort(built.begin(), built.end(), byRecord);
			auto& expected = parity->expected;
			std::sort(expected.begin(), expected.end(), byRecord);
			std::size_t differing = 0;
			std::size_t first = SIZE_MAX;
			for (std::size_t i = 0; i < std::min(built.size(), expected.size()); ++i) {
				if (std::memcmp(&built[i], &expected[i], sizeof(DrawSequence)) != 0) {
					first = std::min(first, i);
					++differing;
				}
			}
			++a_stats.buildParityChecks;
			if (count == expected.size() && !differing) {
				logger::info("[DCLF] BuildDraws parity OK: {} sequences match the CPU templates ({} culled on the GPU)", count, culled);
			} else {
				++a_stats.buildParityMismatches;
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU count {}, CPU {} ({} culled on the GPU); {} sequences differ{}", count, expected.size(), culled, differing,
					first != SIZE_MAX ? fmt::format(" (first: pipeline {} vs {}, index count {} vs {})", built[first].pipelineIndex, expected[first].pipelineIndex,
											built[first].indexCount, expected[first].indexCount) :
										"");
			}
			parity.reset();
			return;
		}
		if ((parityEpochs++ % 300) != 0 || !a_resources->sequencesD3D11 || !a_resources->countD3D11)
			return;
		// After the epoch in D3D11 stream order: the copies see what BuildDraws wrote.
		auto staging = [&](ID3D11Buffer* a_source) {
			D3D11_BUFFER_DESC desc{};
			a_source->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			winrt::com_ptr<ID3D11Buffer> buffer;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, buffer.put())))
				return buffer;
			context->CopyResource(buffer.get(), a_source);
			return buffer;
		};
		ParityReadback readback;
		readback.sequences = staging(a_resources->sequencesD3D11.get());
		readback.count = staging(a_resources->countD3D11.get());
		readback.expected = sequences;
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}

	void IndirectDraws::ShowDebugView()
	{
		if (failed || !impl->resources || !impl->resources->native[0])
			return;
		if (!RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::DebugView))
			logger::error("[DCLF] The debug view epoch failed; the render graph is disabled");
	}
}

#else  // no render graph

#	include "IndirectDraws.h"

namespace DCLF
{
	struct IndirectDraws::Impl
	{};
	IndirectDraws::IndirectDraws() = default;
	IndirectDraws::~IndirectDraws() = default;
	IndirectDraws& IndirectDraws::Get()
	{
		static IndirectDraws draws;
		return draws;
	}
	bool IndirectDraws::Enabled() const { return false; }
	bool IndirectDraws::Hybrid() { return false; }
	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry*, std::uint32_t) const { return false; }
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::Execute() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ProbeTargets(const char*) {}
	void IndirectDraws::RunEpoch(RenderGraphRuntime::Segment) {}
	void IndirectDraws::ShowDebugView() {}
}

#endif
