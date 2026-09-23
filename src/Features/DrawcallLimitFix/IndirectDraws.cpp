#if defined(CS_HAS_RENDER_GRAPH) && defined(CS_HAS_ORG_MODULE_SERVICES)

// volk must precede every Vulkan header in this translation unit.
#	include <rhi_interop_vulkan.h>

#	include "IndirectDraws.h"

#	include "AsyncWorker.h"

#	include "ConstantMirror.h"
#	include "DrawPipelines.h"
#	include "DrawPipelinesRhi.h"
#	include "GpuResources.h"
#	include "GpuTextures.h"
#	include "LightingConstants.h"
#	include "PassCapture.h"
#	include "SceneStore.h"
#	include "ShaderPrograms.h"
#	include "ShadowViews.h"
#	include "Toggles.h"
#	include "Switches.h"
#	include "VertexInput.h"

#	include "Deferred.h"
#	include "Features/LinearLighting.h"
#	include "Features/LightLimitFix/ORGLightCulling.h"
#	include "RenderGraph/DxvkOrgInterop.h"
#	include "RenderGraph/RenderGraphRuntime.h"
#	include "ShaderCache.h"
#	include "State.h"

#	include <OpenRenderGraph/PersistentGraphHost.h>
#	include <Render/LatchBlock.h>
#	include <Render/Runtime/StagedUploadBatch.h>
#	include <Render/RenderGraph/RenderGraph.h>
#	include <Render/Runtime/DescriptorServiceAccess.h>
#	include <Render/Runtime/UploadServiceAccess.h>
#	include <RenderPasses/Base/TypedRenderGraphPass.h>
#	include <Resources/Buffers/Buffer.h>
#	include <Resources/ExternalTextureResource.h>
#	include <Resources/PixelBuffer.h>
#	include <rhi_helpers.h>

#	include <bit>
#	include <chrono>
#	include <cstddef>
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
		constexpr const char* kShadowExtensionId = "cs.dclf.shadow";
		// The shadow views' binding records: one for every draw without alpha testing, and one per material
		// of the alpha-tested casters (their diffuse and texture offset).
		constexpr std::uint32_t kShadowRecordCapacity = 512;  // per view slot (the exterior needs ~160)
		// The views one frame's shadow epoch can hold: the exterior has four (two cascades, twice); an
		// interior with several shadow-casting point lights has two hemispheres per light.
		constexpr std::uint32_t kMaxShadowViews = 16;
		constexpr std::uint32_t kShadowModeCount = 3;  // render modes 0xD plain, 0xE clamped, 0xF paraboloid
		constexpr std::uint64_t kShadowConstantBytes = 1ull << 20;
		// Fixed slots at the head of the shadow constants: the view's Utility PerTechnique block and its
		// VS_PerFrame block, rewritten per view so that the records built once a frame can point at them.
		// The arena's head holds one slot per view: its PerTechnique block (b0) then its VS_PerFrame copy (b12).
		constexpr std::uint64_t kShadowPerFrameOffset = 256;
		constexpr std::uint64_t kShadowViewSlotBytes = 256 + 1024;
		constexpr std::uint64_t kShadowMaterialBlocksOffset = kShadowViewSlotBytes * kMaxShadowViews;
		// [0] kSHADOWMAPS_ESRAM (cascades, spot lights), [1] kSHADOWMAPS (point and focus lights), and
		// [2] kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM: the engine draws the cascades a second time into it for
		// the volumetric lighting, through the same batch renderer, so a caster withheld from the renderer
		// is missing from both draws and DCLF has to draw both (S2: the two "not ready" views per frame).
		constexpr std::uint32_t kShadowDepthTargets = 3;
		constexpr std::uint32_t kMaxDraws = 16384;
		constexpr std::uint64_t kConstantBytes = 48ull << 20;
		constexpr std::uint64_t kConstantAlignment = 256;  // uniform buffer address alignment (conservative)
		constexpr std::uint32_t kColorTargets = 8;
		constexpr std::uint32_t kMaxGeometries = kMaxDraws;
		// The per-object record table (ObjectRecord) is indexed by a table index, not a draw index, so it
		// covers every candidate the frame tracks rather than only the ones drawn.
		constexpr std::uint32_t kMaxObjects = 32768;
		// Draw inputs and the visibility buffer are sized by the table, not by the draw count: the depth
		// segment submits a cull-only input for every candidate it may not draw, and BuildDraws indexes the
		// visibility word by the object's TABLE index. Sizing either by kMaxDraws is an overrun waiting for
		// a cell with more than kMaxDraws tracked objects.
		constexpr std::uint32_t kMaxInputs = kMaxObjects;
		// Distinct binding records an epoch may hold, and the size of the buffer behind them. Deduplicated
		// they are one per (material, pipeline) pair: 85 behind 727 candidates in the Bannered Mare, 105
		// behind 1616 in Dragonsreach. 2048 is about twenty times the worst seen, and 1.6 MB against the
		// 13.1 MB the undeduplicated buffer needs. Without CS_DCLF_BINDLESS_DRAW there is a record per
		// draw, so the cap has to stay at the draw cap - the record's contents are still per-object then.
		constexpr std::uint32_t kMaxRecordsDeduplicated = 2048;
		// The 32-bit record offset BuildDraws computes (input.y * RecordStride) has to address all of it.
		static_assert(std::uint64_t(kMaxDraws) * sizeof(DrawBindings) < (std::uint64_t(1) << 32));
		constexpr std::uint32_t kNoRecord = ~0u;
		constexpr std::uint32_t kNoSkip = ~0u;
		// BuildDrawsCS's counter words: [0] drawn, [1] culled, [2] tested, [3] engine-culled and gated out,
		// [4] false negatives (the engine kept it, the culling rejected it), [5] rescued.
		constexpr std::uint32_t kCountWords = 24;
		// Decals: the two groups' slot counts, written by the CPU and read by their draws, then the culling's
		// own tallies. Byte offsets must match BuildDrawsCS.hlsl.
		constexpr std::uint32_t kCountDecalGroupWord = 19;  // group 1 at word 19, group 2 at word 20
		constexpr std::uint32_t kCountDecalsCulledWord = 21;
		constexpr std::uint32_t kCountDecalsTestedWord = 22;
		// Byte offsets of the count words the indirect draws read; they must match BuildDrawsCS.hlsl.
		constexpr std::uint64_t kCountDrawnPhaseTwoBytes = 68;
		constexpr const wchar_t* kBuildDrawsShader = L"Data\\Shaders\\DrawcallLimitFix\\ORG\\BuildDrawsCS.spv";
		constexpr const wchar_t* kHzbShader = L"Data\\Shaders\\DrawcallLimitFix\\ORG\\HzbCS.spv";
		// HzbCS.hlsl's constants: source, target, target size, source size, from-depth, padding.
		constexpr std::uint32_t kHzbConstantWords = 8;

		// The format that reads a depth resource's depth aspect in a shader, as DXGI names it.
		rhi::Format DepthReadFormat(rhi::Format a_format)
		{
			switch (a_format) {
			case rhi::Format::D24_UNorm_S8_UInt:
			case rhi::Format::R24G8_Typeless:
				return rhi::Format::R24_UNorm_X8_Typeless;
			case rhi::Format::D32_Float_S8X24_UInt:
			case rhi::Format::R32G8X24_Typeless:
				return rhi::Format::R32_Float_X8X24_Typeless;
			case rhi::Format::D32_Float:
				return rhi::Format::R32_Float;
			case rhi::Format::D16_UNorm:
				return rhi::Format::R16_UNorm;
			default:
				return a_format;
			}
		}

		// BuildDrawsCS.hlsl's inputs (byte-address buffers).
		struct DrawInput
		{
			std::uint32_t pipelineIndex;  // in the pipeline sets
			std::uint32_t recordIndex;    // DrawBindings record
			std::uint32_t geometryIndex;  // GeometryDraw
			std::uint32_t flags;          // object flags, plus kInputDrawable for this epoch
			float boundCentre[3];         // absolute world space; the camera is folded into the matrix
			float boundRadius;
			// Index into the frame's object table. The two segments emit different subsets in different
			// orders, so this is what lets the colour segment look up the visibility the depth segment
			// published for the same object.
			std::uint32_t objectIndex;
			std::uint32_t decalOrdinal;  // decals only: the slot in the group's range
		};
		static_assert(sizeof(DrawInput) == 40);
		// BuildDrawsCS.hlsl: set on an input the epoch has built a bindings record for. The depth segment
		// submits an input for every candidate so the culling covers them all, but builds records only for
		// the ones it may draw.
		constexpr std::uint32_t kInputDrawable = 1u << 16;
		// Where phase 2 appends its sequences; see BuildDrawsCS.hlsl.
		constexpr std::uint32_t kPhaseTwoSequenceBase = kMaxDraws;
		// The decal ranges: one of kMaxDecalDraws fixed slots per group after phase 2's range. A decal's
		// sequence goes to the slot of its ordinal in the engine's draw order (SceneStore::Tables::
		// decalOrdinal), so the second pass draws decals in that order every frame; a culled one is the
		// same sequence with an index count of zero.
		constexpr std::uint32_t kMaxDecalDraws = 2048;
		// The bones buffer (VS t126): every skinned object's palette rows, current then previous, per epoch.
		// 65,536 float4 rows is 1 MB; the exterior needs ~13,500.
		constexpr std::uint32_t kMaxBoneRows = 65536;
		constexpr std::uint32_t kDecalGroups = 2;
		constexpr std::uint32_t kDecalSequenceBase = 2 * kMaxDraws;
		constexpr std::uint32_t kSequenceSlots = kDecalSequenceBase + kDecalGroups * kMaxDecalDraws;

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

		// BuildDrawsCS's push constants: only what is fixed for a pass across executions. The execution's own
		// values are in its BuildDrawsLatch (below), so the recorded dispatch never changes with the frame.
		struct BuildDrawsConstants
		{
			std::uint32_t latchIndex;        // the latch block's SRV
			std::uint32_t inputsIndex;
			std::uint32_t geometriesIndex;
			std::uint32_t sequencesIndex;
			std::uint32_t countIndex;
			std::uint32_t recordsAddressLo;
			std::uint32_t recordsAddressHi;
			std::uint32_t recordStride;
			std::uint32_t phaseBits;         // the culling phase in bits 4-7 (CullFlags' phase field)
			std::uint32_t visibilityIndex;   // RWByteAddressBuffer: one uint per object in the tables
			std::uint32_t latchOffset;       // set at record time: the slot's region plus the dispatch's latch
			std::uint32_t padding;
			std::uint32_t hzbIndex;        // 0 when there is no HZB to test against
			std::uint32_t hzbSizePacked;   // mip 0: width in the low 16 bits, height in the high 16
			std::uint32_t hzbMips;
			std::uint32_t hzbPadding;
		};
		static_assert(sizeof(BuildDrawsConstants) == 64);
		constexpr std::uint32_t kBuildDrawsConstantWords = sizeof(BuildDrawsConstants) / 4;

		/**
		 * @brief One BuildDrawsCS dispatch's values for one execution, in its slot of a latch block
		 * (org::LatchBlock): written by the render thread in the epoch, read by the dispatch - which takes its
		 * own group count from the first three words (ExecuteIndirect with a dispatch signature).
		 */
		struct BuildDrawsLatch
		{
			std::uint32_t dispatch[3];      // groups x, y, z
			std::uint32_t drawCount;        // inputs to cull
			std::uint32_t cullFlags;        // mode in bits 0-3, RequireNativeVisible at 8 (the phase is pushed)
			std::uint32_t visibilityStamp;  // marks the verdicts as this frame's; see BuildDrawsCS.hlsl
			std::uint32_t hzbUvScalePacked; // rendered area over the area the HZB covers, 16-bit fixed point
			std::uint32_t padding;
			// Row-major, as the shader's float4x4 with mul(M, v), with the camera translation folded in so
			// that an absolute world position projects directly.
			float viewProj[16];
		};
		static_assert(sizeof(BuildDrawsLatch) == 96 && offsetof(BuildDrawsLatch, viewProj) == 32);

		// A draw's ExecuteIndirect max count: a power of two that only grows, so the recording settles while
		// the preprocess memory stays near what the frame draws (the GPU count buffer says how many run).
		std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
		{
			if (a_needed <= a_current)
				return a_current;
			return std::min(a_limit, std::max(256u, std::bit_ceil(a_needed)));
		}

		template <class H>
		bool SameHandle(const H& a, const H& b)
		{
			return a.index == b.index && a.generation == b.generation;
		}

		bool SameIndirect(const IndirectState& a, const IndirectState& b)
		{
			if (a.valid != b.valid || !SameHandle(a.layout, b.layout))
				return false;
			for (std::size_t i = 0; i < a.sets.size(); ++i)
				if (!SameHandle(a.sets[i], b.sets[i]) || !SameHandle(a.signatures[i], b.signatures[i]))
					return false;
			return true;
		}

		// The dispatch signature every BuildDraws pass records with: one Dispatch argument, read from a latch.
		rhi::CommandSignaturePtr CreateDispatchSignature(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout)
		{
			rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
			rhi::CommandSignaturePtr signature;
			if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, a_layout, signature) != rhi::Result::Ok)
				return {};
			return signature;
		}

		/**
		 * @brief The segment a pass instance serves. With epochs every instance belongs to one epoch and its
		 * segment is fixed when it is registered, so nothing it prepares or records reads the runtime's
		 * current segment (a pass prepared ahead of its epoch would see another one). Without epochs one
		 * instance serves every segment and takes it from the runtime, as before.
		 */
		struct SegmentBinding
		{
			bool fixed = false;
			RenderGraphRuntime::Segment segment = RenderGraphRuntime::Segment::MainOpaque;

			static SegmentBinding Of(RenderGraphRuntime::Segment a_segment)
			{
				return { RenderGraphRuntime::EpochsEnabled(), a_segment };
			}
			RenderGraphRuntime::Segment Now() const { return fixed ? segment : RenderGraphRuntime::Get().CurrentSegment(); }
		};

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

		// CS_DCLF_BINDLESS_PARITY: the per-object record against the packed constant group, variable by
		// variable. Both are produced from tables.objects and tables.shading by the same rules, so the
		// comparison is exact rather than tolerant - a tolerance here would only hide a layout mistake.
		void CheckBindlessRecord(const SceneStore::Tables& a_tables, std::uint32_t a_objectIndex, const BindlessObject& a_record, const RE::NiPoint3& a_eye,
			const RE::NiPoint3& a_previousEye, const GeometryPatchOffsets& a_offsets, std::span<const std::byte> a_vs, std::span<const std::byte> a_ps,
			IndirectDraws::Stats& a_stats)
		{
			auto compare = [&](std::span<const std::byte> a_group, std::uint32_t a_offset, std::uint32_t a_count, const float* a_expected, const char* a_name) {
				if (a_offset == ~0u)
					return;  // the pipeline's shaders do not declare it, so neither form carries a value
				for (std::uint32_t c = 0; c < a_count; ++c) {
					const std::size_t at = (std::size_t(a_offset) + c) * 4;
					if (at + 4 > a_group.size())
						return;
					float packed = 0.0f;
					std::memcpy(&packed, a_group.data() + at, 4);
					++a_stats.bindlessParityChecks;
					if (std::bit_cast<std::uint32_t>(packed) == std::bit_cast<std::uint32_t>(a_expected[c]))
						continue;
					if (a_stats.bindlessParityMismatches++ == 0)
						logger::warn("[DCLF] bindless record parity: {}[{}] is {} in the record and {} in the constant group", a_name, c, a_expected[c], packed);
				}
			};
			// The record is absolute; the shader makes it relative with the same single subtraction as this.
			float relative[16] = {};
			StoreRelativeTo(relative, a_record.world, a_eye);
			compare(a_vs, a_offsets.vsWorld, std::min(a_offsets.vsWorldSize, 12u), relative, "World");
			StoreRelativeTo(relative, a_record.previousWorld, a_previousEye);
			compare(a_vs, a_offsets.vsPreviousWorld, std::min(a_offsets.vsPreviousWorldSize, 12u), relative, "PreviousWorld");
			compare(a_ps, a_offsets.psMaterialData, std::min(a_offsets.psMaterialDataSize, 4u), a_record.shading.materialData, "MaterialData");
			compare(a_ps, a_offsets.psEmitColor, std::min(a_offsets.psEmitColorSize, 3u), a_record.shading.emitColor, "EmitColor");
			if (a_offsets.psSSRParams != ~0u && a_offsets.psSSRParamsSize > 3)
				compare(a_ps, a_offsets.psSSRParams + 3, 1, &a_record.shading.ssrSpecular, "SSRParams.w");

			// The tail has no constant group to compare against once its buffers are gone, so it is checked
			// against the expressions the epoch used to build those buffers from - which is exactly the
			// thing being replaced.
			const auto& object = a_tables.objects[a_objectIndex];
			const auto& lights = a_tables.lights[a_objectIndex];
			auto expect = [&](bool a_equal, const char* a_name) {
				++a_stats.bindlessParityChecks;
				if (!a_equal && a_stats.bindlessParityMismatches++ == 0)
					logger::warn("[DCLF] bindless record parity: {} differs for object {}", a_name, a_objectIndex);
			};
			expect(a_record.roomIndex == lights.roomIndex, "RoomIndex");
			expect(a_record.shadowBitMask == lights.shadowBitMask, "ShadowBitMask");
			const float threshold = (object.flags & kObjectAlphaTest) ? ((object.flags >> kObjectAlphaThresholdShift) & 0xFF) / 255.0f : 0.0f;
			expect(std::bit_cast<std::uint32_t>(a_record.alphaTestRef) == std::bit_cast<std::uint32_t>(threshold), "AlphaTestRef");
			const bool skinned = (object.flags & kObjectSkinned) && a_objectIndex < a_tables.boneOffset.size();
			expect(a_record.boneOffset == (skinned ? a_tables.boneOffset[a_objectIndex] : 0u), "BoneOffset");
			expect(a_record.boneRows == (skinned ? a_tables.boneRows[a_objectIndex] : 0u), "BoneRows");
			expect(a_record.previousBoneOffset == (skinned ? a_tables.boneOffset[a_objectIndex] + static_cast<std::uint32_t>(a_tables.bones.size() / 4) : 0u), "PreviousBoneOffset");
			const bool extras = a_objectIndex < a_tables.extraOffset.size() && a_tables.extraOffset[a_objectIndex] != kNoExtraRows;
			expect(a_record.extraOffset == (extras ? static_cast<std::uint32_t>(a_tables.bones.size() / 4) * 2 + a_tables.extraOffset[a_objectIndex] : 0u), "ExtraOffset");
			// EmissiveMult is the only one of the four whose SOURCE changes, from the scene graph to the
			// tables, so it was tempting to check it against a live read of the property here. That check
			// was built, and it fired: ~100 components in half a billion, always on flickering emissives.
			// It was the check that was wrong. The multiplier is animated, and the shader divides it back
			// out of the emissive colour before re-applying it - so the record's multiplier has to be the
			// same SAMPLE that produced this object's emitColor, which is why MakeShading hands both back
			// together. A live read at epoch time is a strictly later sample, and matching it would break
			// the cancellation rather than prove anything.
			//
			// What is worth checking is the plumbing, because emissiveMult is a new parallel array and a
			// gap in one of those shifts every later object's index. Whether it is sampled at the right
			// point in the frame is already covered, and better, by capture parity's EmitColor comparison
			// against the engine's own draw - which is the check RefreshFrameConstants exists to satisfy.
			expect(a_tables.emissiveMult.size() == a_tables.objects.size(), "emissiveMult table length");
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

		/**
		 * @brief What one segment's passes record against: everything their recorded commands depend on,
		 * and nothing that changes every frame. The frame's own values (the counts, the view-projection, the
		 * culling stamp) are in the segment's BuildDrawsLatch, and the draw counts on the GPU. Published by
		 * the commit before the epoch prepares and replaced only when it changes, so its identity is what
		 * the passes' invocation revisions carry.
		 */
		struct PassFrame
		{
			std::uint64_t generation = 0;
			// ExecuteIndirect max counts (GrowCapacity): the phase-1/colour draws, and each decal group.
			std::uint32_t drawCapacity = 0;
			std::array<std::uint32_t, kDecalGroups> decalCapacity{};
			std::uint32_t cullMode = 0;
			bool offscreen = false;  // colour drawn into DCLF's own targets even on the hybrid path
			bool probePixel = false;
			std::uint32_t probeX = 0, probeY = 0;
			std::uint32_t width = 0, height = 0;  // render area: the main pass viewport
			float minDepth = 0.0f, maxDepth = 1.0f;  // its depth range (the engine uses [0, 0.999998])
			bool hybrid = false;
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			IndirectState indirect{};

			bool SameShape(const PassFrame& o) const
			{
				return drawCapacity == o.drawCapacity && decalCapacity == o.decalCapacity && cullMode == o.cullMode && offscreen == o.offscreen &&
				       probePixel == o.probePixel && probeX == o.probeX && probeY == o.probeY && width == o.width && height == o.height &&
				       minDepth == o.minDepth && maxDepth == o.maxDepth && hybrid == o.hybrid && SameHandle(resourceHeap, o.resourceHeap) &&
				       SameHandle(samplerHeap, o.samplerHeap) && SameIndirect(indirect, o.indirect);
			}
		};

		// Index of a drawing segment's shape (Resources::frames).
		constexpr std::size_t kDepthShape = 0, kColourShape = 1;

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
			// The per-object records the DCLF_BINDLESS builds read, at t127 of every draw of the epoch.
			std::shared_ptr<org::Buffer> objects;
			std::uint32_t objectsIndex = 0;  // its SRV's descriptor heap index
			// The bone palette rows the skinned draws read, at t126 (kBonesBufferRegister).
			std::shared_ptr<org::Buffer> bones;
			std::uint32_t bonesIndex = 0;
			std::shared_ptr<org::Buffer> inputs, geometries, sequences, count;  // BuildDraws: in, in, out, out
			std::shared_ptr<const ComputeProgram> buildDraws;
			winrt::com_ptr<ID3D11Buffer> sequencesD3D11, countD3D11;  // CS_DCLF_BUILD_PARITY readback
			winrt::com_ptr<ID3D11Buffer> visibilityD3D11;              // CS_DCLF_SET_PARITY readback
			std::uint64_t constantsAddress = 0, recordsAddress = 0;
			std::uint32_t recordCapacity = 0;  // entries in `records`, which deduplication makes far fewer
			// The per-frame constant blocks at fixed slots (FrameSlotOffset), so a build can name them before
			// their contents exist.
			std::shared_ptr<org::Buffer> frameConstants;
			std::uint64_t frameConstantsAddress = 0;
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
			// Phase 4's hierarchical depth buffer. Its source is nativeDepth, which is imported with a
			// shader-resource view as well as a depth-stencil one so that the graph sees the depth the draws
			// write and the depth the build reads as one resource and orders them.
			std::shared_ptr<org::Buffer> visibility;  // per-object verdict, published by the depth segment
			std::shared_ptr<org::PixelBuffer> hzb;
			std::shared_ptr<const ComputeProgram> hzbProgram;
			std::uint32_t hzbWidth = 0, hzbHeight = 0, hzbMips = 0;
			bool hybrid = false;
			bool offscreen = false;
			std::uint32_t width = 0, height = 0;
			bool lightLimitFix = false;  // LLF's graph buffers are registered (they are read at t35-t37)
			// Per drawing segment (kDepthShape, kColourShape): the shape its passes record against, null while
			// the segment has nothing to draw this epoch. published keeps the last one across the null.
			std::array<std::atomic<std::shared_ptr<const PassFrame>>, 2> frames;
			std::array<std::shared_ptr<const PassFrame>, 2> published;
			std::uint64_t shapeGenerations = 0;
			// Per frame slot, one BuildDrawsLatch: all BuildDraws dispatches of an epoch share the values.
			std::shared_ptr<org::LatchBlock> latch;
			rhi::CommandSignaturePtr dispatchSignature;
		};

		struct PassBindings
		{
			std::array<org::ResourceBindingToken, kColorTargets> targets{};
			org::ResourceBindingToken depth, sequences, count, records, constants, objects, bones;
			org::ResourceBindingToken lights, lightIndexList, lightGrid;
			std::vector<org::ResourceBindingToken> frameBuffers;
		};

		struct PreparedDraws
		{
			std::shared_ptr<const PassFrame> frame;
			std::array<org::PreparedDescriptorReference, kColorTargets> targetViews{};
			org::PreparedDescriptorReference depthView{};
			std::uint32_t targetCount = 0;
			bool phaseTwo = false;
			bool zPrepass = false;
		};

		// The shape of the segment a pass serves, or null when that segment does not draw (or, without epochs,
		// when the current segment is not a drawing one).
		std::shared_ptr<const PassFrame> CurrentFrame(const Resources& a_resources, RenderGraphRuntime::Segment a_segment)
		{
			if (a_segment == RenderGraphRuntime::Segment::ZPrepass)
				return a_resources.frames[kDepthShape].load(std::memory_order_acquire);
			if (a_segment == RenderGraphRuntime::Segment::MainOpaque)
				return a_resources.frames[kColourShape].load(std::memory_order_acquire);
			return nullptr;
		}

		class MainOpaquePass final : public org::TypedRenderGraphPass<MainOpaquePass, PreparedDraws, PassBindings>
		{
		public:
			// phaseTwo: the second depth draw of the two-phase culling, which draws only what the rebuilt HZB
			// brought back, from the reserved part of the sequence buffer.
			MainOpaquePass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, bool a_phaseTwo = false) :
				resources(std::move(a_resources)), segment(a_segment), phaseTwo(a_phaseTwo) {}

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
				if (resources->objects)
					bindings.objects = a_builder.BindShaderResource(resources->objects);
				if (resources->bones)
					bindings.bones = a_builder.BindShaderResource(resources->bones);
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
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
				a_out.push_back(phaseTwo ? 1 : 0);
			}

			PreparedDraws Prepare(const PassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				PreparedDraws prepared{};
				const auto now = segment.Now();
				auto frame = CurrentFrame(*resources, now);
				if (!frame || (!frame->drawCapacity && !frame->decalCapacity[0] && !frame->decalCapacity[1]) || !frame->indirect.valid)
					return prepared;
				// The rescue draw belongs to the depth segment only.
				if (phaseTwo && now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				prepared.phaseTwo = phaseTwo;
				prepared.zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
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
				const bool zPrepass = a_prepared.zPrepass;
				if (zPrepass && !frame.hybrid)
					return;  // off the hybrid path both passes run together in the main segment
				if (a_prepared.phaseTwo && !(zPrepass && frame.hybrid))
					return;
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
					if (!(zPrepass && empty)) {
						// Phase 2 draws only the rescues, from the reserved half of the sequence buffer and
						// its own counter word. Its argument offset has to be a constant the CPU knows, which
						// is why the two phases have fixed ranges instead of sharing one.
						const std::uint64_t argumentOffset = a_prepared.phaseTwo ? std::uint64_t(kPhaseTwoSequenceBase) * sizeof(DrawSequence) : 0;
						const std::uint64_t countOffset = a_prepared.phaseTwo ? kCountDrawnPhaseTwoBytes : 0;
						commands.ExecuteIndirect(frame.indirect.signatures[kDepthVariant], sequences, argumentOffset, count, countOffset, frame.drawCapacity);
					}
					commands.EndPass();
				}
				if (a_prepared.phaseTwo)
					return;

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
				if (frame.drawCapacity)
					commands.ExecuteIndirect(frame.indirect.signatures[kColorVariant], sequences, 0, count, 0, frame.drawCapacity);
				commands.EndPass();

				// The second pass: decals, after every opaque draw, in the engine's order - its opaque decal
				// group and then its blended one, each from its own fixed-slot range and its own count word.
				// The pipelines test depth LESS_EQUAL with the engine's decal bias and write none, so this
				// pass changes nothing the depth buffer's readers see. Same attachments, all loaded.
				if (frame.decalCapacity[0] || frame.decalCapacity[1]) {
					for (std::uint32_t i = 0; i < a_prepared.targetCount; ++i)
						colors[i].loadOp = rhi::LoadOp::Load;
					begin.debugName = "DCLF decals";
					commands.BeginPass(begin);
					commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					commands.BindLayout(frame.indirect.layout);
					for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
						if (!frame.decalCapacity[group])
							continue;
						const std::uint64_t argumentOffset = std::uint64_t(kDecalSequenceBase + group * kMaxDecalDraws) * sizeof(DrawSequence);
						const std::uint64_t countOffset = std::uint64_t(kCountDecalGroupWord + group) * sizeof(std::uint32_t);
						commands.ExecuteIndirect(frame.indirect.signatures[kColorVariant], sequences, argumentOffset, count, countOffset, frame.decalCapacity[group]);
					}
					commands.EndPass();
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
			bool phaseTwo = false;
		};

		/**
		 * @brief Folds the camera translation into the view-projection, so that a bound's absolute world
		 * position can be projected without carrying the camera separately.
		 *
		 * The draws are camera-relative (the engine's posAdjust), so the culling used to subtract the camera
		 * from every bound before projecting. Doing it here instead is exact - it is the same subtraction,
		 * one matrix column earlier - and it freed three words of a push-constant block that had reached
		 * Vulkan's guaranteed 128 bytes.
		 *
		 * Row-major, as the shader's `mul(M, v)` reads it: M'(p,1) = M(p-e,1) needs only column 3 changed,
		 * by subtracting M's upper-left 3x3 applied to the camera.
		 */
		void FoldEyeIntoViewProj(const std::array<float, 16>& a_viewProj, const RE::NiPoint3& a_eye, float (&a_out)[16])
		{
			std::memcpy(a_out, a_viewProj.data(), sizeof(a_out));
			for (std::uint32_t row = 0; row < 4; ++row) {
				a_out[row * 4 + 3] = a_viewProj[row * 4 + 3] -
				                     (a_viewProj[row * 4 + 0] * a_eye.x + a_viewProj[row * 4 + 1] * a_eye.y +
									     a_viewProj[row * 4 + 2] * a_eye.z);
			}
		}

		struct BuildDrawsBindings
		{
			org::ResourceBindingToken inputs, geometries, sequences, count, hzb, visibility;
		};

		struct BuildDrawsFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			BuildDrawsConstants constants{};
			std::shared_ptr<const org::LatchBlock> latch;
			rhi::CommandSignatureHandle signature{};
		};

		// Records one BuildDrawsCS dispatch whose values and group count are the slot's latch at a_latchOffset.
		void RecordLatchedDispatch(BuildDrawsConstants a_constants, const org::LatchBlock& a_latch,
			rhi::CommandSignatureHandle a_signature, std::uint32_t a_latchOffset, org::PassRecordContext& a_recording)
		{
			auto& commands = a_recording.Commands();
			const std::uint64_t offset = a_latch.Offset(a_recording.FrameSlot()) + a_latchOffset;
			a_constants.latchOffset = static_cast<std::uint32_t>(offset);
			commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kBuildDrawsConstantWords, reinterpret_cast<const std::uint32_t*>(&a_constants));
			commands.ExecuteIndirect(a_signature, a_latch.Resource()->GetAPIResource().GetHandle(), offset, {}, 0, 1);
		}

		// Writes this frame's draw sequences and their count from the draw inputs (BuildDrawsCS.hlsl).
		class BuildDrawsPass final : public org::TypedRenderGraphPass<BuildDrawsPass, BuildDrawsFrame, BuildDrawsBindings>
		{
		public:
			// phase 0 picks itself from the segment (1 in the depth segment, 3 in the colour one); phase 2 is
			// the rebuilt-HZB pass, registered separately after the HZB build.
			BuildDrawsPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, std::uint32_t a_phase = 0) :
				resources(std::move(a_resources)), segment(a_segment), fixedPhase(a_phase) {}

			BuildDrawsBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				BuildDrawsBindings bindings{};
				bindings.inputs = a_builder.BindShaderResource(resources->inputs);
				bindings.geometries = a_builder.BindShaderResource(resources->geometries);
				bindings.sequences = a_builder.BindUnorderedAccess(resources->sequences);
				bindings.count = a_builder.BindUnorderedAccess(resources->count);
				bindings.visibility = a_builder.BindUnorderedAccess(resources->visibility);
				// Phase 1 sees the HZB the previous frame left, phase 2 the one just rebuilt from this
				// frame's depth. Both read the same resource; what differs is where they sit relative to
				// the build, which is why the ordering below is the whole design.
				if (resources->hzb)
					bindings.hzb = a_builder.BindShaderResource(resources->hzb);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
				a_out.push_back(fixedPhase);
			}

			BuildDrawsFrame Prepare(const BuildDrawsBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				BuildDrawsFrame prepared{};
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				if (!frame || !resources->buildDraws || !resources->latch || !resources->dispatchSignature)
					return prepared;
				const std::uint32_t phase = Phase(now);
				// The second phase belongs to the depth segment only: it is what re-tests phase 1's rejects
				// against the HZB that has just been rebuilt from this frame's depth.
				if (fixedPhase == 2 && now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				prepared.program = resources->buildDraws;
				prepared.latch = resources->latch;
				prepared.signature = resources->dispatchSignature->GetHandle();
				auto& constants = prepared.constants;
				constants.latchIndex = resources->latch->SrvIndex();
				constants.inputsIndex = a_preparation.ResolveView(a_bindings.inputs, { org::BindlessViewKind::ShaderResource }).index;
				constants.geometriesIndex = a_preparation.ResolveView(a_bindings.geometries, { org::BindlessViewKind::ShaderResource }).index;
				constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.countIndex = a_preparation.ResolveView(a_bindings.count, { org::BindlessViewKind::UnorderedAccess }).index;
				constants.recordsAddressLo = static_cast<std::uint32_t>(resources->recordsAddress);
				constants.recordsAddressHi = static_cast<std::uint32_t>(resources->recordsAddress >> 32);
				constants.recordStride = sizeof(DrawBindings);
				constants.phaseBits = (phase & 0xFu) << 4;
				constants.visibilityIndex = a_preparation.ResolveView(a_bindings.visibility, { org::BindlessViewKind::UnorderedAccess }).index;
				if (resources->hzb && frame->cullMode >= 2 && frame->width && frame->height) {
					constants.hzbIndex = a_preparation.ResolveView(a_bindings.hzb, { org::BindlessViewKind::ShaderResource }).index;
					constants.hzbSizePacked = (resources->hzbWidth & 0xFFFF) | (resources->hzbHeight << 16);
					constants.hzbMips = resources->hzbMips;
				}
				return prepared;
			}

			static void Record(const BuildDrawsBindings&, const BuildDrawsFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || !a_frame.latch)
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				RecordLatchedDispatch(a_frame.constants, *a_frame.latch, a_frame.signature, 0, a_recording);
			}

			std::uint32_t Phase(RenderGraphRuntime::Segment a_segment) const
			{
				if (fixedPhase)
					return fixedPhase;
				// Off the hybrid path there is no depth segment to decide anything, so the colour segment
				// has to do its own culling exactly as it did before the phases existed.
				if (!resources->hybrid)
					return 0;
				return a_segment == RenderGraphRuntime::Segment::ZPrepass ? 1u : 3u;
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
			std::uint32_t fixedPhase = 0;
		};

		struct HzbConstants
		{
			std::uint32_t sourceIndex;
			std::uint32_t targetIndex;
			std::uint32_t targetSize[2];
			std::uint32_t sourceSize[2];
			std::uint32_t fromDepth;
			std::uint32_t padding;
		};
		static_assert(sizeof(HzbConstants) / 4 == kHzbConstantWords);

		struct HzbBindings
		{
			org::ResourceBindingToken depth, hzb;
		};

		struct HzbFrame
		{
			std::shared_ptr<const ComputeProgram> program;
			// One dispatch per mip, each reading the level above (mip 0 reads the scene depth).
			struct Level
			{
				HzbConstants constants{};
				std::uint32_t groupsX = 0, groupsY = 0;
			};
			std::vector<Level> levels;
		};

		/**
		 * @brief Builds the hierarchical depth buffer at the end of the native depth pass.
		 *
		 * It runs in the ZPrepass segment, after DCLF's own depth draws, so the HZB describes the depth the
		 * frame actually has: the native occluders the engine drew (terrain and everything ineligible) plus
		 * the objects DCLF drew itself.
		 *
		 * The whole mip chain is one pass with a full memory barrier between the dispatches, rather than one
		 * pass per level. Levels of a single texture are not separate resources to the graph, so a per-level
		 * pass would declare the same resource as both its input and its output and the ordering would not
		 * mean what it reads as.
		 */
		class HzbPass final : public org::TypedRenderGraphPass<HzbPass, HzbFrame, HzbBindings>
		{
		public:
			HzbPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

			HzbBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				HzbBindings bindings{};
				bindings.depth = a_builder.BindShaderResource(resources->nativeDepth);
				bindings.hzb = a_builder.BindUnorderedAccess(resources->hzb);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
			}

			HzbFrame Prepare(const HzbBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				HzbFrame prepared{};
				// Only at the end of the depth pass: anywhere else the depth is not final.
				const auto now = segment.Now();
				if (now != RenderGraphRuntime::Segment::ZPrepass)
					return prepared;
				if (!resources->hzb || !resources->hzbProgram)
					return prepared;
				const auto frame = CurrentFrame(*resources, now);
				const std::uint32_t renderWidth = frame && frame->width ? frame->width : resources->width;
				const std::uint32_t renderHeight = frame && frame->height ? frame->height : resources->height;
				prepared.program = resources->hzbProgram;
				const std::uint32_t depthIndex = a_preparation.ResolveView(a_bindings.depth, { org::BindlessViewKind::ShaderResource }).index;
				for (std::uint32_t mip = 0; mip < resources->hzbMips; ++mip) {
					HzbFrame::Level level{};
					level.constants.targetIndex = a_preparation.ResolveView(a_bindings.hzb, { org::BindlessViewKind::UnorderedAccess, UINT32_MAX, mip }).index;
					level.constants.targetSize[0] = std::max(1u, resources->hzbWidth >> mip);
					level.constants.targetSize[1] = std::max(1u, resources->hzbHeight >> mip);
					if (mip == 0) {
						level.constants.fromDepth = 1;
						level.constants.sourceIndex = depthIndex;
						// The rendered area, not the texture: the depth image can be larger than the viewport
						// the draws use, and everything past that viewport is untouched. Bounding the read
						// here makes those texels answer the far plane, which suppresses culling.
						level.constants.sourceSize[0] = renderWidth;
						level.constants.sourceSize[1] = renderHeight;
					} else {
						level.constants.fromDepth = 0;
						level.constants.sourceIndex = prepared.levels.back().constants.targetIndex;
						level.constants.sourceSize[0] = prepared.levels.back().constants.targetSize[0];
						level.constants.sourceSize[1] = prepared.levels.back().constants.targetSize[1];
					}
					level.groupsX = (level.constants.targetSize[0] + 7) / 8;
					level.groupsY = (level.constants.targetSize[1] + 7) / 8;
					prepared.levels.push_back(level);
				}
				return prepared;
			}

			static void Record(const HzbBindings&, const HzbFrame& a_frame, org::PassRecordContext& a_recording)
			{
				if (!a_frame.program || a_frame.levels.empty())
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_frame.program->layout->GetHandle());
				commands.BindPipeline(a_frame.program->pipeline->GetHandle());
				for (std::size_t i = 0; i < a_frame.levels.size(); ++i) {
					const auto& level = a_frame.levels[i];
					if (i != 0) {
						// The level below must be complete before this one reads it.
						const rhi::GlobalBarrier global = rhi::FullMemoryBarrier();
						const rhi::BarrierBatch batch{ {}, {}, { &global, 1 } };
						commands.Barriers(batch);
					}
					commands.PushConstants(rhi::ShaderStage::Compute, 0, 0, 0, kHzbConstantWords, reinterpret_cast<const std::uint32_t*>(&level.constants));
					commands.Dispatch(level.groupsX, level.groupsY, 1);
				}
			}

		private:
			std::shared_ptr<Resources> resources;
			SegmentBinding segment;
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
			ProbePass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment, bool a_after) :
				resources(std::move(a_resources)), segment(a_segment), after(a_after) {}

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
				const auto now = segment.Now();
				const auto frame = CurrentFrame(*resources, now);
				a_out.push_back(frame ? frame->generation : 0);
				a_out.push_back(static_cast<std::uint64_t>(now));
			}

			ProbeFrame Prepare(const ProbeBindings&, const org::PassPrepareContext&) const
			{
				ProbeFrame prepared{};
				const auto now = segment.Now();
				const bool zPrepass = now == RenderGraphRuntime::Segment::ZPrepass;
				if (now != RenderGraphRuntime::Segment::MainOpaque && !(zPrepass && after))
					return prepared;
				const auto frame = CurrentFrame(*resources, now);
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
			SegmentBinding segment;
			bool after = false;
		};

		bool DebugViewEnabled()
		{
			return Toggles::Get().Active().debugView;
		}

		// CS_DCLF_CULL=off|frustum: how BuildDrawsCS filters this frame's draws before it writes their
		// sequences. The draw inputs are the whole tracked set, including what the engine's own culling
		// rejected, so frustum mode has real work to do and its rejection count is meaningful. What it must
		// never reject is an object the engine kept (kObjectNativeVisible): those are counted separately as
		// false negatives, and any non-zero count is a defect in the projection here.
		std::uint32_t CullingMode()
		{
			// 0 off, 1 frustum, 2 frustum then the HZB (Toggles: live from the menu).
			return Toggles::Get().Active().cullMode;
		}

		// CS_DCLF_CULL_INPUT=native|tracked: which of the candidates may actually be drawn.
		//
		// native (the default) draws only what the engine's culling kept, so the frame contains exactly what
		// it does today while the culling counters still measure the whole tracked set. tracked hands the
		// decision to the GPU culling alone, which is what Phase 4 is building towards and what Phase 5
		// needs; until the HZB lands it draws everything the engine occluded, so it costs frames and the
		// objects outside the accumulator carry derived per-frame bits rather than measured ones.
		bool RequireNativeVisible()
		{
			return !Toggles::Get().Active().cullTracked;
		}

		// CS_DCLF_HYBRID=1: DCLF draws into the main pass's own targets and depth, and the native loop skips
		// the objects it drew (DrawcallLimitFix's RenderPassImmediately hooks).
		bool HybridEnabled()
		{
			return Toggles::Get().Active().hybrid;
		}

		// CS_DCLF_BUILD_PARITY=1: compare BuildDraws' output with the CPU templates every 300 epochs.
		// MainPayload::objectState values besides the Skip reasons.
		constexpr std::uint8_t kObjectStateDrawable = 0xF0;
		constexpr std::uint8_t kObjectStateDecal = 0xF1;
		constexpr std::uint8_t kObjectStateAbsent = 0xFF;

		/**
		 * CS_DCLF_SET_PARITY=1: every frame, the per-object visibility words read back after the colour epoch,
		 * with BuildDrawsCS's two drawn bits (depth, colour), against what the two builds and the native
		 * withholding decided on the CPU. See CheckSetParity.
		 */
		bool SetParityEnabled()
		{
			static const bool enabled = [] {
				return SwitchEnabled("CS_DCLF_SET_PARITY");
			}();
			return enabled;
		}

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
			DebugViewPass(std::shared_ptr<Resources> a_resources, SegmentBinding a_segment) :
				resources(std::move(a_resources)), segment(a_segment) {}

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
				a_out.push_back(segment.Now() == RenderGraphRuntime::Segment::DebugView);
			}

			DebugViewFrame Prepare(const DebugViewBindings&, const org::PassPrepareContext&) const
			{
				if (segment.Now() != RenderGraphRuntime::Segment::DebugView)
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
			SegmentBinding segment;
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
			// An array image publishes a view per slice (ORG builds one DSV per slice for arrays), which is
			// how a shadow view's epoch attaches the one slice the engine gave that view.
			a_desc.isArray = a_image.arrayLayers > 1;
			a_desc.arraySize = a_desc.isArray ? a_image.arrayLayers : 1;
			a_desc.initialLayout = rhi::ResourceLayout::Common;
			auto result = org::ExternalTextureResource::CreateShared(std::move(resource), a_desc, true);
			if (result)
				result->SetName(a_name);
			return result;
		}

		/**
		 * @brief What one shadow view's epoch draws; published before the epoch prepares (as PassFrame is).
		 */
		/**
		 * @brief One captured view of the frame's shadow epoch: where to draw its render mode's inputs and
		 * with which per-view blocks (its record region names them).
		 */
		struct ShadowFrameView
		{
			std::uint32_t slot = 0;       // the view's sequence and count buffers, record region and latch
			std::uint32_t modeIndex = 0;  // the inputs buffer (render mode 0xD, 0xE, 0xF)
			std::uint32_t capacity = 0;   // its draw's ExecuteIndirect max count (GrowCapacity, per slot)
			std::uint32_t x = 0, y = 0, width = 0, height = 0;  // the view's viewport, in its slice
			float minDepth = 0.0f, maxDepth = 1.0f;
			std::uint32_t target = 0;  // index into ShadowResources::depth
			std::uint32_t slice = 0;
			std::uint64_t recordsAddress = 0;  // the slot's copy of the binding records

			bool operator==(const ShadowFrameView&) const = default;
		};

		/**
		 * @brief The frame's shadow epoch: every view the hooks captured, drawn by one graph execution at
		 * AfterShadowMaps. One epoch rather than one per view because the graph's own execution costs
		 * ~0.9 ms of CPU per epoch whatever it draws (the main epochs' "graph execute" part), and the
		 * exterior has four views a frame.
		 */
		// The shape of the frame's shadow epoch: which views, where they draw and with what capacity. Each
		// view's matrices and input count are in its BuildDrawsLatch; this changes only when the engine's view
		// layout or the pipelines do, and its identity is the shadow passes' revision.
		struct ShadowFrame
		{
			std::uint64_t generation = 0;
			rhi::DescriptorHeapHandle resourceHeap{};
			rhi::DescriptorHeapHandle samplerHeap{};
			ShadowIndirectState indirect{};
			std::vector<ShadowFrameView> views;

			bool SameShape(const ShadowFrame& o) const
			{
				return SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) && indirect.valid == o.indirect.valid &&
				       SameHandle(indirect.layout, o.indirect.layout) && SameHandle(indirect.set, o.indirect.set) && SameHandle(indirect.signature, o.indirect.signature) &&
				       views == o.views;
			}
		};

		/** @brief The shadow views' graph resources: the main path's set, without targets or an HZB, per view slot. */
		struct ShadowResources
		{
			std::shared_ptr<org::Buffer> constants, records, objects, bones, geometries, visibility;
			std::array<std::shared_ptr<org::Buffer>, kShadowModeCount> inputs;               // per render mode
			std::array<std::shared_ptr<org::Buffer>, kMaxShadowViews> sequences, count;      // per view slot
			std::array<winrt::com_ptr<ID3D11Buffer>, kMaxShadowViews> countD3D11;             // the counters, read back
			std::uint32_t objectsIndex = 0, bonesIndex = 0;
			std::uint64_t constantsAddress = 0, recordsAddress = 0;
			std::shared_ptr<const ComputeProgram> buildDraws;
			// The engine's shadow map arrays, imported once each (re-imported when the engine recreates
			// them), with a depth-stencil view per slice.
			std::array<std::shared_ptr<org::ExternalTextureResource>, kShadowDepthTargets> depth;
			std::array<ID3D11Texture2D*, kShadowDepthTargets> depthTexture{};
			std::array<std::uint32_t, kShadowDepthTargets> depthLayers{};
			std::atomic<std::shared_ptr<const ShadowFrame>> frame;
			std::shared_ptr<const ShadowFrame> published;  // survives a frame without views
			std::uint64_t shapeGenerations = 0;
			// Per frame slot, one BuildDrawsLatch per view slot.
			std::shared_ptr<org::LatchBlock> latch;
			rhi::CommandSignaturePtr dispatchSignature;
			// This frame's views as the engine named them (render thread), for the culling readback's report.
			struct ViewLabel
			{
				std::uint32_t viewId = 0, renderMode = 0, slot = 0;
			};
			std::vector<ViewLabel> labels;
		};

		// With epochs these passes run only in the shadow epoch; without, every epoch runs them and they are
		// empty outside it.
		std::shared_ptr<const ShadowFrame> CurrentShadowFrame(const ShadowResources& a_resources)
		{
			if (!RenderGraphRuntime::EpochsEnabled() && RenderGraphRuntime::Get().CurrentSegment() != RenderGraphRuntime::Segment::ShadowView)
				return nullptr;
			return a_resources.frame.load(std::memory_order_acquire);
		}

		struct ShadowBuildBindings
		{
			std::array<org::ResourceBindingToken, kShadowModeCount> inputs;
			std::array<org::ResourceBindingToken, kMaxShadowViews> sequences, count;
			org::ResourceBindingToken geometries, visibility;
		};

		struct ShadowBuildPrepared
		{
			std::shared_ptr<const ComputeProgram> program;
			std::shared_ptr<const org::LatchBlock> latch;
			rhi::CommandSignatureHandle signature{};
			struct Dispatch
			{
				BuildDrawsConstants constants{};
				std::uint32_t latchOffset = 0;  // the view's BuildDrawsLatch within the slot's region
			};
			std::vector<Dispatch> dispatches;  // one per captured view
		};

		/** @brief The shadow views' culling: BuildDrawsCS in its single phase, frustum only, one dispatch per view. */
		class ShadowBuildDrawsPass final : public org::TypedRenderGraphPass<ShadowBuildDrawsPass, ShadowBuildPrepared, ShadowBuildBindings>
		{
		public:
			explicit ShadowBuildDrawsPass(std::shared_ptr<ShadowResources> a_resources) :
				resources(std::move(a_resources)) {}

			ShadowBuildBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ShadowBuildBindings bindings{};
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					bindings.inputs[m] = a_builder.BindShaderResource(resources->inputs[m]);
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					bindings.sequences[s] = a_builder.BindUnorderedAccess(resources->sequences[s]);
					bindings.count[s] = a_builder.BindUnorderedAccess(resources->count[s]);
				}
				bindings.geometries = a_builder.BindShaderResource(resources->geometries);
				bindings.visibility = a_builder.BindUnorderedAccess(resources->visibility);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentShadowFrame(*resources);
				a_out.push_back(frame ? frame->generation : 0);
			}

			ShadowBuildPrepared Prepare(const ShadowBuildBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ShadowBuildPrepared prepared{};
				const auto frame = CurrentShadowFrame(*resources);
				if (!frame || frame->views.empty() || !resources->buildDraws || !resources->latch || !resources->dispatchSignature)
					return prepared;
				prepared.program = resources->buildDraws;
				prepared.latch = resources->latch;
				prepared.signature = resources->dispatchSignature->GetHandle();
				const auto geometriesIndex = a_preparation.ResolveView(a_bindings.geometries, { org::BindlessViewKind::ShaderResource }).index;
				const auto visibilityIndex = a_preparation.ResolveView(a_bindings.visibility, { org::BindlessViewKind::UnorderedAccess }).index;
				for (const auto& view : frame->views) {
					if (view.slot >= kMaxShadowViews || view.modeIndex >= kShadowModeCount)
						continue;
					ShadowBuildPrepared::Dispatch dispatch{};
					dispatch.latchOffset = view.slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch));
					auto& constants = dispatch.constants;
					constants.latchIndex = resources->latch->SrvIndex();
					constants.inputsIndex = a_preparation.ResolveView(a_bindings.inputs[view.modeIndex], { org::BindlessViewKind::ShaderResource }).index;
					constants.geometriesIndex = geometriesIndex;
					constants.sequencesIndex = a_preparation.ResolveView(a_bindings.sequences[view.slot], { org::BindlessViewKind::UnorderedAccess }).index;
					constants.countIndex = a_preparation.ResolveView(a_bindings.count[view.slot], { org::BindlessViewKind::UnorderedAccess }).index;
					constants.recordsAddressLo = static_cast<std::uint32_t>(view.recordsAddress);
					constants.recordsAddressHi = static_cast<std::uint32_t>(view.recordsAddress >> 32);
					constants.recordStride = sizeof(DrawBindings);
					// The single phase (the latch holds the frustum-only mode, with no engine-visibility gate).
					constants.phaseBits = 0;
					// The visibility words are written per object by every dispatch; nothing reads them here.
					constants.visibilityIndex = visibilityIndex;
					prepared.dispatches.push_back(dispatch);
				}
				return prepared;
			}

			static void Record(const ShadowBuildBindings&, const ShadowBuildPrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.program || !a_prepared.latch || a_prepared.dispatches.empty())
					return;
				auto& commands = a_recording.Commands();
				commands.BindLayout(a_prepared.program->layout->GetHandle());
				commands.BindPipeline(a_prepared.program->pipeline->GetHandle());
				for (const auto& dispatch : a_prepared.dispatches)
					RecordLatchedDispatch(dispatch.constants, *a_prepared.latch, a_prepared.signature, dispatch.latchOffset, a_recording);
			}

		private:
			std::shared_ptr<ShadowResources> resources;
		};

		struct ShadowPassBindings
		{
			std::array<org::ResourceBindingToken, kShadowDepthTargets> depth{};
			std::array<org::ResourceBindingToken, kMaxShadowViews> sequences, count;
			org::ResourceBindingToken records, constants, objects, bones;
		};

		struct ShadowPrepared
		{
			std::shared_ptr<const ShadowFrame> frame;
			struct View
			{
				std::uint32_t index = 0;  // into frame->views
				org::PreparedDescriptorReference depthView{};
			};
			std::vector<View> views;
		};

		/** @brief The shadow views' draws: each view's culled sequences into its slice and viewport, in one pass. */
		class ShadowViewPass final : public org::TypedRenderGraphPass<ShadowViewPass, ShadowPrepared, ShadowPassBindings>
		{
		public:
			explicit ShadowViewPass(std::shared_ptr<ShadowResources> a_resources) :
				resources(std::move(a_resources)) {}

			ShadowPassBindings Declare(org::PassBuilder& a_builder)
			{
				a_builder.PreferQueue(org::QueueKind::Graphics);
				ShadowPassBindings bindings{};
				for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
					if (resources->depth[i])
						bindings.depth[i] = a_builder.BindDepthReadWrite(resources->depth[i]);
				}
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					bindings.sequences[s] = a_builder.BindIndirectArguments(resources->sequences[s]);
					bindings.count[s] = a_builder.BindIndirectArguments(resources->count[s]);
				}
				bindings.records = a_builder.BindShaderResource(resources->records);
				bindings.constants = a_builder.BindShaderResource(resources->constants);
				bindings.objects = a_builder.BindShaderResource(resources->objects);
				bindings.bones = a_builder.BindShaderResource(resources->bones);
				return bindings;
			}

			void InvocationRevision(const org::PassPrepareContext&, std::vector<std::uint64_t>& a_out) const
			{
				const auto frame = CurrentShadowFrame(*resources);
				a_out.push_back(frame ? frame->generation : 0);
			}

			ShadowPrepared Prepare(const ShadowPassBindings& a_bindings, const org::PassPrepareContext& a_preparation) const
			{
				ShadowPrepared prepared{};
				auto frame = CurrentShadowFrame(*resources);
				if (!frame || frame->views.empty() || !frame->indirect.valid)
					return prepared;
				for (std::uint32_t i = 0; i < frame->views.size(); ++i) {
					const auto& view = frame->views[i];
					if (!view.capacity || view.slot >= kMaxShadowViews || view.target >= kShadowDepthTargets || !resources->depth[view.target])
						continue;
					if (view.slice >= resources->depthLayers[view.target])
						continue;
					org::BindlessViewRequest request{};
					request.kind = org::BindlessViewKind::DepthStencil;
					request.slice = view.slice;
					prepared.views.push_back({ i, a_preparation.CaptureView(a_bindings.depth[view.target], request) });
				}
				if (!prepared.views.empty())
					prepared.frame = std::move(frame);
				return prepared;
			}

			static void Record(const ShadowPassBindings& a_bindings, const ShadowPrepared& a_prepared, org::PassRecordContext& a_recording)
			{
				if (!a_prepared.frame)
					return;
				const auto& frame = *a_prepared.frame;
				auto& commands = a_recording.Commands();
				commands.SetDescriptorHeaps(frame.resourceHeap, frame.samplerHeap);
				for (const auto& prepared : a_prepared.views) {
					const auto& view = frame.views[prepared.index];
					rhi::PassBeginInfo begin{};
					begin.x = view.x;
					begin.y = view.y;
					begin.width = view.width;
					begin.height = view.height;
					begin.minDepth = view.minDepth;
					begin.maxDepth = view.maxDepth;
					// The slice the engine drew its own casters into: loaded, added to, stored.
					rhi::DepthAttachment depth{};
					depth.dsv = a_recording.Resolve(prepared.depthView);
					depth.depthLoad = rhi::LoadOp::Load;
					depth.depthStore = rhi::StoreOp::Store;
					depth.stencilLoad = rhi::LoadOp::Load;
					depth.stencilStore = rhi::StoreOp::Store;
					begin.depth = &depth;
					begin.debugName = "DCLF shadow view";
					commands.BeginPass(begin);
					commands.SetPrimitiveTopology(rhi::PrimitiveTopology::TriangleList);
					commands.BindLayout(frame.indirect.layout);
					commands.ExecuteIndirect(frame.indirect.signature, a_recording.Resolve(a_bindings.sequences[view.slot]).GetHandle(), 0,
						a_recording.Resolve(a_bindings.count[view.slot]).GetHandle(), 0, view.capacity);
					commands.EndPass();
				}
			}

		private:
			std::shared_ptr<ShadowResources> resources;
		};

		class ShadowExtension final : public org::RenderGraph::IRenderGraphExtension
		{
		public:
			explicit ShadowExtension(std::shared_ptr<ShadowResources> a_resources) :
				resources(std::move(a_resources)) {}

			void PrepareForBuild(org::RenderGraph& a_graph) override
			{
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.constants"), resources->constants);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.records"), resources->records);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.objects"), resources->objects);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.bones"), resources->bones);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.geometries"), resources->geometries);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.shadow.visibility"), resources->visibility);
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-inputs{}", m)), resources->inputs[m]);
				for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.sequences{}", s)), resources->sequences[s]);
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.draw-count{}", s)), resources->count[s]);
				}
				for (std::uint32_t i = 0; i < kShadowDepthTargets; ++i) {
					if (resources->depth[i])
						a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.shadow.depth{}", i)), resources->depth[i]);
				}
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				const auto epoch = RenderGraphRuntime::EpochOf(RenderGraphRuntime::Segment::ShadowView);
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.shadow.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowBuildDrawsPass>(resources)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(epoch));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.shadow.view",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<ShadowViewPass>(resources)))
						.Epoch(epoch));
			}

		private:
			std::shared_ptr<ShadowResources> resources;
		};

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
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.visibility"), resources->visibility);
				if (resources->bones)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.bones"), resources->bones);
				a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.depth"), resources->depth);
				for (std::uint32_t i = 0; i < resources->targetCount; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.target{}", i)), resources->targets[i]);
				for (const auto& frameBuffer : resources->frameBuffers)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.frame-buffer.t{}", frameBuffer.textureRegister)), frameBuffer.copy);
				for (std::uint32_t i = 0; i < resources->targetCount && resources->native[0]; ++i)
					a_graph.RegisterResource(org::ResourceIdentifier(fmt::format("cs.dclf.native-target{}", i)), resources->native[i]);
				if (resources->nativeDepth)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.native-depth"), resources->nativeDepth);
				if (resources->hzb)
					a_graph.RegisterResource(org::ResourceIdentifier("cs.dclf.hzb"), resources->hzb);
			}

			void GatherStructuralPasses(org::RenderGraph&, std::vector<org::RenderGraph::ExternalPassDesc>& a_out) override
			{
				using Segment = RenderGraphRuntime::Segment;
				const auto colour = RenderGraphRuntime::EpochOf(Segment::MainOpaque);
				const auto depth = RenderGraphRuntime::EpochOf(Segment::ZPrepass);
				const auto colourSegment = SegmentBinding::Of(Segment::MainOpaque);
				const auto depthSegment = SegmentBinding::Of(Segment::ZPrepass);
				// With epochs, a pass instance runs only in its own epoch and its segment is fixed here, so the
				// Z-prepass has its own build-draws and draw pass. Without, one instance of each serves both
				// segments and takes its segment from the runtime, as it always has.
				if (RenderGraphRuntime::EpochsEnabled() && resources->hybrid) {
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.z.build-draws",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.z.depth",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment)))
							.Epoch(depth));
				}
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, colourSegment)))
						.PreferQueue(org::QueueKind::Graphics)
						.Epoch(colour));
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-before",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, false)))
							.Epoch(colour));
				a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.main-opaque",
					std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, colourSegment)))
						.Epoch(colour));
				if (resources->probe)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Copy("cs.dclf.probe-after",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<ProbePass>(resources, colourSegment, true)))
							.Epoch(colour));
				// The two-phase tail, all of it inside the depth segment and all of it in this order:
				//
				//   build-draws (phase 1, against the HZB the previous frame left)
				//   main-opaque (the phase-1 depth draw)
				//   hzb         (rebuilt from the depth that draw has just finished)
				//   build-draws-phase2 (phase 1's rejects, re-tested against the rebuilt HZB)
				//   depth-phase2       (the rescues, so their depth is in the frame too)
				//
				// Phase 1 tests against a depth buffer that is a frame old, which is what makes it cheap and
				// also what makes it wrong at the edges: anything that has just come out from behind an
				// occluder was hidden in that buffer. Phase 2 exists to take those back, after a rebuild
				// that can see them.
				if (resources->hzb) {
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.hzb",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<HzbPass>(resources, depthSegment)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Compute("cs.dclf.build-draws-phase2",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<BuildDrawsPass>(resources, depthSegment, 2)))
							.PreferQueue(org::QueueKind::Graphics)
							.Epoch(depth));
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.depth-phase2",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<MainOpaquePass>(resources, depthSegment, true)))
							.Epoch(depth));
				}
				if (resources->native[0] && !resources->hybrid)
					a_out.push_back(org::RenderGraph::ExternalPassDesc::Render("cs.dclf.debug-view",
						std::static_pointer_cast<org::RenderPass>(std::make_shared<DebugViewPass>(resources, SegmentBinding::Of(Segment::DebugView))))
							.Epoch(RenderGraphRuntime::EpochOf(Segment::DebugView)));
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

		// ---------------------------------------------------------------------------------------------
		// The epoch's inputs, their build, and their commit.
		//
		// An epoch's CPU work is in three parts, so that the build - the part proportional to the scene -
		// can run off the render thread (docs: asynchronous tables). Prepare, on the render thread, snapshots
		// everything the build needs that is not immutable for the frame. Build is a pure function of that
		// snapshot, the tables and the lookups (Lookups.h), and writes only its payload: nothing in it may
		// call the engine, D3D11, DXVK or ORG. Commit, on the render thread inside the epoch's preparation,
		// validates the payload against what it was built for, resolves what only the services can resolve,
		// and issues the uploads.
		// ---------------------------------------------------------------------------------------------

		// The per-frame constant blocks - whatever the main pass binds outside the per-draw registers - live
		// in Resources::frameConstants at fixed slots, so that a record can name a block before its contents
		// exist: the vertex stage's b0-b13, then the pixel stage's, then the zeroed StrictLightData block
		// every bindless draw's b3 reads. A slot is D3D11's constant buffer maximum, so no block overflows.
		constexpr std::uint64_t kFrameSlotBytes = 65536;
		constexpr std::uint32_t kFrameSlotSharedLight = 2 * kConstantBufferRegisters;
		constexpr std::uint32_t kFrameSlotCount = kFrameSlotSharedLight + 1;
		constexpr std::uint64_t kFrameConstantBytes = std::uint64_t(kFrameSlotCount) * kFrameSlotBytes;

		constexpr std::uint64_t FrameSlotOffset(bool a_pixelStage, std::uint32_t a_register)
		{
			return (std::uint64_t(a_pixelStage ? kConstantBufferRegisters : 0u) + a_register) * kFrameSlotBytes;
		}

		// The blocks the render thread packs into the frame slots for one epoch.
		struct FrameBlocks
		{
			std::array<std::vector<std::byte>, kConstantBufferRegisters> vs, ps;
			std::uint32_t vsMask = 0, psMask = 0;  // the slots supplied
		};

		// What a build addresses in an epoch's buffers; a change means the resources were recreated under it.
		struct ResourceAddresses
		{
			std::uint64_t constants = 0, records = 0, frameConstants = 0;
			std::uint32_t objectsIndex = 0, bonesIndex = 0, recordCapacity = 0;
			const void* identity = nullptr;

			bool operator==(const ResourceAddresses&) const = default;
		};

		struct MainInputs
		{
			std::uint32_t frameNumber = 0;
			bool depthOnly = false, hybrid = false, resolveTextures = true;
			bool bindless = false, bindlessDraws = false, dedupParity = false, bindlessParity = false;
			bool withholding = false, requireNativeVisible = true, linearLighting = false;
			std::uint32_t renderFlags = 0;
			RE::NiPoint3 eye, previousEye;
			std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			std::vector<std::uint8_t> drewLastFrame;  // per object; the Z-prepass's hybrid gate without withholding
			// The PS PerMaterial floats that are the frame's rather than the material's (SceneStore::
			// GetMaterialPatchedFloats): the build cache leaves them out of a pair's signature and repacks the
			// PS group, so a drifting IBLParams does not rebuild every pair every frame.
			std::vector<std::uint32_t> materialPatchedFloats;
			ResourceAddresses addresses{};
			std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		};

		struct MainPayload
		{
			MainInputs inputs;
			ConstantArena arena;
			std::vector<DrawBindings> records;
			std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
			std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
			std::vector<DrawInput> inputList;
			std::vector<GeometryDraw> geometryDraws;
			std::vector<BindlessObject> objectRecords;  // the DCLF_BINDLESS per-object table
			std::vector<float> boneRows;                // eye-relative, current then previous, then the extras
			// The frame's textures (t16 and up) are the epoch's own descriptor indices, which only the commit
			// can resolve: the build leaves these (record, register) pairs for it.
			std::vector<std::pair<std::uint32_t, std::uint32_t>> framePatches;
			std::vector<std::uint32_t> drawn;  // objects the colour epoch drew: the native loop's skip set
			// Per object in the tables: what this build did with it - kObjectStateDrawable, kObjectStateDecal,
			// a Skip reason, or kObjectStateAbsent when it never reached the inputs (CS_DCLF_SET_PARITY explains
			// its mismatches with this).
			std::vector<std::uint8_t> objectState;
			std::array<std::uint32_t, kDecalGroups> decalCount{};
			// The build's stats, merged into IndirectDraws::Stats by the commit.
			std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipped{};
			std::array<std::uint32_t, 4> missingTextures{};
			std::uint32_t missingNext = 0;
			std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
			std::uint32_t decalsDrawn = 0, shortBuffers = 0, deferredTextures = 0;
			std::uint32_t bindlessParityChecks = 0, bindlessParityMismatches = 0;
			std::uint32_t recordParityChecks = 0, recordParityMismatches = 0;
			std::array<double, 7> partMs{};
			// The first draw past its buffers, for the commit to log with the geometry's name.
			struct ShortBuffer
			{
				std::uint32_t object = ~0u;
				std::uint64_t vertexNeeded = 0, indexNeeded = 0;
			} shortBuffer;
			// The worker's build stages its uploads itself (StageMainPayload), so the commit on the render thread
			// only submits the batch and patches the frame textures into the staged records. For the resources
			// it was staged against; a build made on the render thread has none.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			DrawBindings* stagedRecords = nullptr;
			const void* stagedFor = nullptr;

			void Reset()
			{
				staged.reset();
				stagedRecords = nullptr;
				stagedFor = nullptr;
				arena.Reset();
				records.clear();
				sequences.clear();
				inputList.clear();
				geometryDraws.clear();
				framePatches.clear();
				drawn.clear();
				objectState.clear();
				for (auto& templates : decalTemplates)
					templates.clear();
				decalCount = {};
				skipped = {};
				missingTextures = {};
				missingNext = 0;
				missingVertexConstants = missingPixelConstants = 0;
				decalsDrawn = shortBuffers = deferredTextures = 0;
				bindlessParityChecks = bindlessParityMismatches = recordParityChecks = recordParityMismatches = 0;
				partMs = {};
				shortBuffer = {};
			}
		};

		struct ShadowInputs
		{
			std::uint32_t frameNumber = 0;
			std::uint32_t renderFlags = 0;
			RE::NiPoint3 refEye;
			std::array<bool, kShadowModeCount> modeUsed{};
			ResourceAddresses addresses{};
			// Community Shaders' SharedData (b5) and FeatureData (b6), copied from the structs CS keeps.
			std::vector<std::byte> sharedData, featureData;
			std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
			std::uint32_t sceneRebuilds = 0;  // SceneStore::GetSceneRebuilds: the walk the build read was replaced
		};

		struct ShadowPayload
		{
			ShadowInputs inputs;
			ConstantArena arena;  // the view slots' head is reserved; the commit writes the views into it
			std::vector<DrawBindings> records;  // per-view registers (b0, b12) unset
			std::vector<std::uint32_t> objectRecord;  // per object: its binding record, or ~0u when it cannot draw
			std::array<std::vector<DrawInput>, kShadowModeCount> inputList;  // per render mode
			std::vector<BindlessObject> objects;
			std::vector<float> boneRows;
			std::vector<GeometryDraw> geometries;
			std::uint32_t skippedTexture = 0, skippedPipeline = 0, deferredTextures = 0, deferredPipelines = 0;
			// The worker's build stages its uploads itself (StageShadowPayload): everything but the arena's view
			// head, and the records of the first stagedSlots view slots. For the resources it was staged against.
			std::shared_ptr<org::runtime::StagedUploadBatch> staged;
			const void* stagedFor = nullptr;
			std::uint32_t stagedSlots = 0;
			// The worker's build also builds each used mode's claim set (ShadowClaimSet), which the epoch
			// publishes; empty for a build made on the render thread, which builds them at the publish.
			std::array<std::shared_ptr<const PassCapture::ClaimSet>, kShadowModeCount> claims;

			void Reset()
			{
				staged.reset();
				stagedFor = nullptr;
				stagedSlots = 0;
				claims = {};
				arena.Reset();
				records.clear();
				objectRecord.clear();
				for (auto& modeInputs : inputList)
					modeInputs.clear();
				objects.clear();
				boneRows.clear();
				geometries.clear();
				skippedTexture = skippedPipeline = deferredTextures = deferredPipelines = 0;
			}
		};

		// The bone palettes as the engine keeps them, absolute (the shaders take the drawing camera's eye off
		// each bone, as the engine's own skinning does with its pivot): current rows first, then the previous
		// frame's, then the extras rows - the layout BuildObjectRecord's offsets assume.
		void PackBoneRows(const SceneStore::Tables& a_tables, std::vector<float>& a_out)
		{
			a_out.clear();
			if (a_tables.bones.empty() && a_tables.extraRows.empty())
				return;
			a_out.reserve(a_tables.bones.size() + a_tables.previousBones.size() + a_tables.extraRows.size());
			a_out.insert(a_out.end(), a_tables.bones.begin(), a_tables.bones.end());
			a_out.insert(a_out.end(), a_tables.previousBones.begin(), a_tables.previousBones.end());
			a_out.insert(a_out.end(), a_tables.extraRows.begin(), a_tables.extraRows.end());
		}

		void PackGeometryDraws(const SceneStore::Tables& a_tables, std::vector<GeometryDraw>& a_out)
		{
			a_out.resize(std::min<std::size_t>(a_tables.geometries.size(), kMaxGeometries));
			for (std::size_t g = 0; g < a_out.size(); ++g) {
				const auto& geometry = a_tables.geometries[g];
				a_out[g] = { geometry.vertexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.vertexBytes, UINT32_MAX)), geometry.vertexStride,
					geometry.indexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.indexBytes, UINT32_MAX)), geometry.indexCount, geometry.firstIndex, 0 };
			}
		}

		// Resolved descriptor heap indices per (material, pipeline) pair. The texture and sampler loops depend on
		// nothing per-object: the material's textures, the pipeline's technique (the shadow mask) and its
		// register usage, plus this frame's shared textures. Running them per draw meant 128 + 16 iterations and
		// a heap lookup each for every one of ~950 draws, when there are only about 150 distinct pairs.
		struct ResolvedBindings
		{
			std::array<std::uint32_t, kTextureRegisters> textures{};
			std::array<std::uint32_t, kSamplerRegisters> samplers{};
			std::vector<std::uint32_t> patchRegisters;  // the frame textures the pair's record needs
			bool texturesOk = false;
			bool samplersOk = false;
			bool deferred = false;
			std::uint32_t missingTexture = 0;  // the register that failed, for the skip sample
			// Deduplication: once nothing in the binding record is per-object, every draw of a (material,
			// pipeline) pair wants the same record, so the pair keeps its index here and the assembly runs once.
			// A pair that could not be assembled keeps the reason instead, because the skip counters are per
			// DRAW and have to be raised for each of them, not once per pair.
			std::uint32_t recordIndex = kNoRecord;
			std::uint32_t skipReason = kNoSkip;
		};

		// The PerGeometry group, packed once per pipeline. Every object on a pipeline shares all of it but five
		// variables (the two world matrices and three shading values), so an object copies this and rewrites
		// only those instead of walking the whole variable table twice.
		struct GeometryTemplate
		{
			std::vector<std::byte> vs;
			std::vector<std::byte> ps;
			GeometryPatchOffsets offsets;
			// With DCLF_BINDLESS nothing in the group is per-object, so the pipeline's objects share one pair of
			// arena blocks instead of allocating, copying and patching a pair each.
			std::uint64_t vsAddress = 0, psAddress = 0;
		};

		/**
		 * @brief CS_DCLF_BUILD_CACHE (default on, =0 off): what BuildMainPayload derives per (material, pipeline)
		 * pair and per pipeline, kept across frames.
		 *
		 * The pair's texture and sampler indices and its packed PerMaterial groups, and the pipeline's packed
		 * PerTechnique groups and PerGeometry template, were rebuilt by every epoch although they change only
		 * when a slot, a lookup or an evaluated constant does - they were most of the build's cost. An entry
		 * keeps a copy of every input it was derived from, member by member (MaterialRecord has padding that
		 * nothing writes), and is reused only when this build's inputs are byte-identical; anything else
		 * rebuilds it. So the cache cannot serve a stale value, and a build with the cache produces the same
		 * bytes as one without: CS_DCLF_ASYNC=probe compares the worker's (cached) build with an uncached
		 * inline one every epoch.
		 *
		 * One per epoch kind (colour, Z-prepass), used by one build at a time: the worker's, or the render
		 * thread's when it builds inline (the worker's job for that kind is then done or waited for).
		 */
		struct BuildCache
		{
			struct PackedGroup
			{
				std::size_t size = 0;           // what ConstantGroupSize said, the size the block is allocated with
				std::vector<std::byte> bytes;   // the block's contents as packed (at least 16 bytes)
				bool valid = false;
			};
			struct Pair
			{
				std::vector<std::byte> sources;
				ResolvedBindings resolved;  // the resolution fields only; recordIndex and skipReason are per build
				bool hasResolved = false;
				PackedGroup vs, ps;         // PerMaterial
				// Where the frame's floats (MainInputs::materialPatchedFloats, in that order) sit in the packed PS
				// group, as dword offsets or ~0: a reused group gets this frame's values written there.
				std::vector<std::uint32_t> psPatchPositions;
				std::uint32_t lastUsed = 0;
			};
			struct Pipeline
			{
				std::vector<std::byte> sources;
				PackedGroup techniqueVS, techniquePS;
				GeometryTemplate geometry;  // without its addresses, which are per build
				bool hasGeometry = false;
				std::uint32_t lastUsed = 0;
			};
			ankerl::unordered_dense::map<std::uint64_t, Pair> pairs;
			ankerl::unordered_dense::map<std::uint32_t, Pipeline> pipelines;
			std::vector<std::byte> scratch;
			std::uint64_t pairHits = 0, pairMisses = 0, pipelineHits = 0, pipelineMisses = 0;

			void Sweep(std::uint32_t a_frame)
			{
				static constexpr std::uint32_t kIdleFrames = 64;
				for (auto it = pairs.begin(); it != pairs.end();)
					it = a_frame - it->second.lastUsed > kIdleFrames ? pairs.erase(it) : std::next(it);
				for (auto it = pipelines.begin(); it != pipelines.end();)
					it = a_frame - it->second.lastUsed > kIdleFrames ? pipelines.erase(it) : std::next(it);
			}
		};

		bool BuildCacheEnabled()
		{
			static const bool enabled = SwitchValue("CS_DCLF_BUILD_CACHE") != "0";
			return enabled;
		}

		// The cache's source signatures: plain bytes of trivially copyable values, appended member by member.
		template <class T>
		void AppendSource(std::vector<std::byte>& a_out, const T& a_value)
		{
			static_assert(std::is_trivially_copyable_v<T>);
			const auto* bytes = reinterpret_cast<const std::byte*>(&a_value);
			a_out.insert(a_out.end(), bytes, bytes + sizeof(T));
		}

		void AppendSource(std::vector<std::byte>& a_out, std::span<const std::int8_t> a_table)
		{
			AppendSource(a_out, a_table.size());
			const auto* bytes = reinterpret_cast<const std::byte*>(a_table.data());
			a_out.insert(a_out.end(), bytes, bytes + a_table.size());
		}

		// Whether the entry's sources equal the ones just written to the scratch; when not, they replace them.
		bool SameSources(std::vector<std::byte>& a_entry, const std::vector<std::byte>& a_current)
		{
			if (a_entry.size() == a_current.size() && std::memcmp(a_entry.data(), a_current.data(), a_current.size()) == 0)
				return true;
			a_entry.assign(a_current.begin(), a_current.end());
			return false;
		}

		/**
		 * @brief The main-pass epoch's draws, from the tables and the lookups: pure.
		 *
		 * Everything the epoch used to fetch from a service as it went - the pipeline set indices, the
		 * shaders' constant tables, the descriptor indices of textures and samplers - comes from the lookups;
		 * an entry the render thread has not resolved yet defers the draw (deferredTextures), and the frame's
		 * own textures are left as patches for the commit. The per-frame constant blocks are addressed by
		 * their fixed slots (FrameSlotOffset) for the slots the inputs say the commit supplies.
		 */
		void BuildMainPayload(const MainInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, MainPayload& a_out,
			BuildCache* a_cache = nullptr)
		{
			if (a_cache && (a_in.frameNumber % 64) == 0)
				a_cache->Sweep(a_in.frameNumber);
			using Skip = IndirectDraws::Skip;
			a_out.Reset();
			a_out.inputs = a_in;
			auto& arena = a_out.arena;
			auto& records = a_out.records;
			auto& sequences = a_out.sequences;
			auto& drawInputs = a_out.inputList;
			const bool depthOnly = a_in.depthOnly;
			const std::uint64_t base = a_in.addresses.constants;
			const std::uint32_t frameNumber = a_in.frameNumber;

			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			// The frame registers name their slots; a slot the commit does not supply stays at zero, which is
			// what the constants check below rejects for a pipeline that reads it.
			std::array<std::uint64_t, kConstantBufferRegisters> frameVS{}, framePS{};
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((a_in.vsFrameMask >> slot) & 1)
					frameVS[slot] = a_in.addresses.frameConstants + FrameSlotOffset(false, slot);
				if ((a_in.psFrameMask >> slot) & 1)
					framePS[slot] = a_in.addresses.frameConstants + FrameSlotOffset(true, slot);
			}
			const std::uint64_t sharedLightBlock = a_in.addresses.frameConstants + std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes;

			// Blocks shared by many objects.
			struct PipelineBlocks
			{
				std::uint32_t setIndex = Lookups::kNone;
				std::span<const std::int8_t> vsTable, psTable;
				const Lookups::RegisterUsageBits* usage = nullptr;
				std::uint32_t shadowMaskIndex = Lookups::kNone;
				std::uint64_t techniqueVS = 0, techniquePS = 0;
			};
			std::vector<PipelineBlocks> pipelineBlocks(a_tables.pipelines.size());
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
				auto& blocks = pipelineBlocks[p];
				// The pipeline table keeps its slots across frames; only the ones this frame's objects use
				// get blocks (a swept or idle slot has no object pointing at it).
				if (!a_tables.PipelineUsed(p, frameNumber) || p >= a_lookups.pipelines.size())
					continue;
				const auto& entry = a_lookups.pipelines[p];
				if (entry.setIndex == Lookups::kNone || !(entry.key == a_tables.pipelines[p]))
					continue;
				blocks.setIndex = entry.setIndex;
				blocks.vsTable = entry.vsTable;
				blocks.psTable = entry.psTable;
				blocks.usage = &entry.usage[depthOnly ? kDepthVariant : kColorVariant];
				blocks.shadowMaskIndex = entry.shadowMaskIndex;
				const auto& technique = a_tables.techniqueConstants[p];
				BuildCache::Pipeline* cached = nullptr;
				if (a_cache) {
					// Everything the pipeline's technique groups and PerGeometry template are packed from.
					auto& sources = a_cache->scratch;
					sources.clear();
					AppendSource(sources, technique.vs.floats);
					AppendSource(sources, technique.ps.floats);
					AppendSource(sources, a_tables.geometryConstants[p].vs.floats);
					AppendSource(sources, a_tables.geometryConstants[p].ps.floats);
					AppendSource(sources, blocks.vsTable);
					AppendSource(sources, blocks.psTable);
					cached = &a_cache->pipelines[static_cast<std::uint32_t>(p)];
					cached->lastUsed = frameNumber;
					if (SameSources(cached->sources, sources)) {
						++a_cache->pipelineHits;
					} else {
						++a_cache->pipelineMisses;
						cached->techniqueVS.valid = cached->techniquePS.valid = cached->hasGeometry = false;
					}
				}
				auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
								BuildCache::PackedGroup* a_packed) {
					if (a_packed && a_packed->valid) {
						const auto address = block(nullptr, a_packed->size);
						if (address)
							std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
						return address;
					}
					const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
					const auto address = block(nullptr, size);
					if (address) {
						const auto out = arena.At(address - base, std::max<std::size_t>(size, 16));
						PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, out);
						if (a_packed) {
							a_packed->size = size;
							a_packed->bytes.assign(out.begin(), out.end());
							a_packed->valid = true;
						}
					}
					return address;
				};
				blocks.techniqueVS = pack(technique.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerTechnique], kVSFirstVariable[kPerTechnique],
					cached ? &cached->techniqueVS : nullptr);
				blocks.techniquePS = pack(technique.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerTechnique], kPSFirstVariable[kPerTechnique],
					cached ? &cached->techniquePS : nullptr);
			}

			ankerl::unordered_dense::map<std::uint64_t, std::pair<std::uint64_t, std::uint64_t>> materialBlocks;  // (material, pipeline) -> VS, PS
			// Resolved descriptor heap indices per (material, pipeline) pair (ResolvedBindings, above).
			ankerl::unordered_dense::map<std::uint64_t, ResolvedBindings> resolvedBindings;  // (material, pipeline)
			ankerl::unordered_dense::map<std::uint32_t, GeometryTemplate> geometryTemplates;  // pipeline
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> lightBlocks;        // (room, shadow mask)
			ankerl::unordered_dense::map<std::uint64_t, std::uint64_t> permutationBlocks;  // (pipeline, extra bits)
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> alphaBlocks;        // threshold
			ankerl::unordered_dense::map<std::uint32_t, std::uint64_t> emissiveBlocks;     // Linear Lighting multiplier bits
			const bool linearLighting = a_in.linearLighting;
			const auto renderFlags = a_in.renderFlags;

			a_out.objectState.assign(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects), kObjectStateAbsent);
			std::uint32_t currentObject = ~0u;
			auto skip = [&](Skip a_reason) {
				++a_out.skipped[static_cast<std::size_t>(a_reason)];
				if (currentObject < a_out.objectState.size())
					a_out.objectState[currentObject] = static_cast<std::uint8_t>(a_reason);
			};
			auto partStart = std::chrono::steady_clock::now();
			auto mark = [&](std::size_t a_part) {
				const auto now = std::chrono::steady_clock::now();
				a_out.partMs[a_part] += std::chrono::duration<double, std::milli>(now - partStart).count();
				partStart = now;
			};
			const bool frameHybrid = a_in.hybrid;
			const bool bindless = a_in.bindless;
			const bool bindlessDraws = a_in.bindlessDraws;
			// Only with bindless draws is the record identical for every draw of a (material, pipeline) pair:
			// without it the light, alpha and emissive blocks still differ per object, and so would the
			// record the cache handed back.
			const bool dedup = bindlessDraws;
			const bool dedupParity = a_in.dedupParity;
			const bool bindlessParity = a_in.bindlessParity;
			// Scratch for that check only, reused across objects so it costs no allocation per draw.
			std::vector<std::byte> parityVS, parityPS;
			const auto& eye = a_in.eye;
			const auto& previousEye = a_in.previousEye;

			// The per-object record table the DCLF_BINDLESS builds read. It is indexed by the object's table
			// index, which the draw carries as its third root constant word, so it is filled for every
			// candidate rather than only the drawn ones - a candidate skipped here still reaches the
			// culling, and an index that addressed nothing would be worse than one that addresses a record
			// no draw reads.
			auto& objectRecords = a_out.objectRecords;
			objectRecords.resize(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects));
			for (std::size_t r = 0; r < objectRecords.size(); ++r)
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), renderFlags, objectRecords[r]);

			// Everything before the loop: the per-epoch maps and the object records.
			mark(6);
			auto& decalCount = a_out.decalCount;
			auto& decalTemplates = a_out.decalTemplates;
			if (!depthOnly) {
				for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
					decalCount[group] = std::min(a_tables.decalCount[group], kMaxDecalDraws);
					decalTemplates[group].resize(decalCount[group]);
				}
			}
			for (std::uint32_t o = 0; o < a_tables.objects.size(); ++o) {
				currentObject = o;
				const auto& object = a_tables.objects[o];
				if (object.flags & kObjectNoBindings) {
					// A culling candidate with no material or pipeline entry; its indices are meaningless.
					// The depth segment still submits it cull-only, with its bounds: that is what the tables
					// carry the whole tracked set for, and what the culling is measured against the engine
					// with.
					if (depthOnly && frameHybrid && o < kMaxObjects && drawInputs.size() < kMaxInputs) {
						drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
							{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
							static_cast<std::uint32_t>(o), 0 });
					}
					skip(Skip::CandidateOnly);
					continue;
				}
				// Decals never reach the depth segment: they are not occluders, and they are drawn by the
				// colour segment's second pass (BuildDrawsCS.hlsl, MainOpaquePass::Record).
				const std::uint32_t decalGroup = ObjectDecalGroup(object.flags);
				if (decalGroup && (depthOnly || o >= a_tables.decalOrdinal.size() || a_tables.decalOrdinal[o] >= decalCount[(decalGroup - 1) & 1]))
					continue;
				// A decal that cannot be drawn this epoch must still reach BuildDraws, so that its slot is
				// written as a zero-count draw rather than left holding whatever a previous frame put there.
				// This guard does that on every `continue` between here and the drawable push below.
				struct DecalSlot
				{
					std::vector<DrawInput>* inputs = nullptr;
					DrawInput blank{};
					~DecalSlot()
					{
						if (inputs)
							inputs->push_back(blank);
					}
				} decalSlot;
				if (decalGroup) {
					decalSlot.inputs = &drawInputs;
					decalSlot.blank = { 0, 0, object.geometryIndex, object.flags,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), a_tables.decalOrdinal[o] };
				}
				const auto& blocks = pipelineBlocks[object.pipelineIndex];
				if (blocks.setIndex == Lookups::kNone) {
					skip(Skip::Pipeline);
					continue;
				}
				const auto& geometry = a_tables.geometries[object.geometryIndex];
				if (!geometry.vertexAddress || !geometry.indexAddress) {
					skip(Skip::Geometry);
					continue;
				}
				// The object's table index addresses its record and its visibility word, and it travels to
				// the shaders as the draw's third root constant. Past the table's capacity it addresses
				// neither, and robust buffer access turns that into zeros - a world matrix of zeros collapses
				// the object to a point at the eye with no other sign. Both caps come before the cull-only
				// push below, which used to reach the inputs buffer without passing any check at all.
				if (o >= kMaxObjects || drawInputs.size() >= kMaxInputs) {
					skip(Skip::Capacity);
					continue;
				}
				// The Z-prepass must write depth for exactly the objects the native loop is leaving to DCLF,
				// which is the set the colour epoch drew in the frame before (SkipNativePass uses the same
				// rule). Writing depth for anything else strands it: the colour epoch may not draw it, and
				// the native draw that would have cannot either, because its own EQUAL test now compares
				// against a depth DCLF computed rather than the one the native prepass wrote. Such an object
				// keeps its depth but is never shaded, which is what left the architecture flat and grey.
				if (depthOnly && frameHybrid && !a_in.withholding && !(o < a_in.drewLastFrame.size() && a_in.drewLastFrame[o])) {
					// Cull-only: the object still goes to the culling, because the depth segment is where the
					// verdict for every candidate is decided and published, and a candidate left out here
					// would reach the colour segment with no verdict at all. What it does not get is a
					// bindings record, which is the expensive part and the only part a draw needs.
					drawInputs.push_back({ 0, 0, object.geometryIndex, object.flags,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), 0 });
					skip(Skip::NotSkippedNatively);
					continue;
				}
				// The draw cap: BuildDraws writes into the first half of the sequence buffer, and the count
				// is ExecuteIndirect's maxCount. Decals have their own ranges.
				if (!decalGroup && sequences.size() >= kMaxDraws) {
					skip(Skip::Capacity);
					continue;
				}
				const auto& usage = *blocks.usage;
				const auto& material = a_tables.materials[object.materialIndex];
				const auto& technique = a_tables.techniqueConstants[object.pipelineIndex];
				// A ProjectedUV pipeline binds the engine's four projected textures (SceneStore captured them
				// from a native draw) at the slots SetupGeometry fills, with its wrap/anisotropic modes.
				const bool projectedPipeline = (a_tables.pipelines[object.pipelineIndex].passDescriptor & 0x8000u) != 0;
				auto projectedSlot = [&](std::uint32_t a_slot) -> std::int32_t {
					if (!projectedPipeline)
						return -1;
					for (std::size_t i = 0; i < SceneStore::ProjectedTextures::kSlots.size(); ++i) {
						if (SceneStore::ProjectedTextures::kSlots[i] == a_slot)
							return static_cast<std::int32_t>(i);
					}
					return -1;
				};
				DrawBindings bindings{};

				auto [resolvedIt, newResolved] = resolvedBindings.try_emplace((std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex);
				auto& resolved = resolvedIt->second;
				// Assembled once per (material, pipeline) pair when the record no longer varies per object, and per
				// draw otherwise. Everything in here - the texture and sampler heap indices, the material,
				// technique, geometry, permutation, light, alpha and emissive blocks, and the per-frame defaults
				// - is then a property of the pair or of the epoch, so a second draw of the same pair needs
				// nothing but its index.
				auto fail = [&](Skip a_reason) {
					if (dedup)
						resolved.skipReason = static_cast<std::uint32_t>(a_reason);
					skip(a_reason);
				};
				if (dedup && resolved.skipReason != kNoSkip) {
					skip(static_cast<Skip>(resolved.skipReason));  // per draw, not once per pair
					if (resolved.deferred)
						++a_out.deferredTextures;
					continue;
				}
				std::uint32_t recordIndex = resolved.recordIndex;
				// Per DRAW: the loop entry and the resolvedBindings probe above, which every candidate pays
				// whether or not it assembles a record.
				mark(4);
				if (!dedup || recordIndex == kNoRecord || dedupParity) {
					const std::uint64_t pairKey = (std::uint64_t(object.materialIndex) << 32) | object.pipelineIndex;
					bool pairFromCache = false;
					if (newResolved && a_cache) {
						// Everything the pair's resolution and PerMaterial groups are derived from.
						auto& sources = a_cache->scratch;
						sources.clear();
						// The material record by its version (Tables::materialVersion), which is new whenever the record is
						// rewritten; the frame's floats RefreshMaterialPatch writes into it are not part of it, and a reused
						// PS group gets them written over below.
						AppendSource(sources, object.materialIndex < a_tables.materialVersion.size() ? a_tables.materialVersion[object.materialIndex] : 0u);
						AppendSource(sources, a_tables.materialSlotKey[object.materialIndex].first);
						AppendSource(sources, a_tables.materialSlotKey[object.materialIndex].second);
						if (object.materialIndex < a_lookups.materials.size()) {
							const auto& entry = a_lookups.materials[object.materialIndex];
							AppendSource(sources, entry.key.first);
							AppendSource(sources, entry.key.second);
							AppendSource(sources, entry.textureIndex);
							AppendSource(sources, entry.resolved);
						} else {
							AppendSource(sources, ~0u);
						}
						AppendSource(sources, technique.filterModes);
						AppendSource(sources, technique.shadowMask);
						AppendSource(sources, blocks.shadowMaskIndex);
						AppendSource(sources, usage.vertexConstants);
						AppendSource(sources, usage.pixelConstants);
						AppendSource(sources, usage.textures);
						AppendSource(sources, usage.samplers);
						AppendSource(sources, blocks.vsTable);
						AppendSource(sources, blocks.psTable);
						AppendSource(sources, projectedPipeline);
						AppendSource(sources, a_lookups.nullTexture);
						AppendSource(sources, a_lookups.samplers);
						AppendSource(sources, a_lookups.projectedTextures);
						AppendSource(sources, a_in.addresses.objectsIndex);
						AppendSource(sources, a_in.addresses.bonesIndex);
						AppendSource(sources, a_in.resolveTextures);
						AppendSource(sources, depthOnly);
						auto& entry = a_cache->pairs[pairKey];
						entry.lastUsed = frameNumber;
						if (SameSources(entry.sources, sources) && entry.hasResolved) {
							++a_cache->pairHits;
							resolved.textures = entry.resolved.textures;
							resolved.samplers = entry.resolved.samplers;
							resolved.patchRegisters = entry.resolved.patchRegisters;
							resolved.texturesOk = entry.resolved.texturesOk;
							resolved.samplersOk = entry.resolved.samplersOk;
							resolved.deferred = entry.resolved.deferred;
							resolved.missingTexture = entry.resolved.missingTexture;
							pairFromCache = true;
						} else {
							++a_cache->pairMisses;
							entry.hasResolved = false;
							entry.vs.valid = entry.ps.valid = false;
						}
					}
					if (newResolved && !pairFromCache) {
						// Textures: the material's, the technique's shadow mask, then the frame's.
						resolved.texturesOk = true;
						const Lookups::Material* materialLookup = object.materialIndex < a_lookups.materials.size() ? &a_lookups.materials[object.materialIndex] : nullptr;
						const bool materialResolved = materialLookup && materialLookup->resolved &&
						                              materialLookup->key.first == a_tables.materialSlotKey[object.materialIndex].first &&
						                              materialLookup->key.second == a_tables.materialSlotKey[object.materialIndex].second;
						for (std::uint32_t t = 0; t < kTextureRegisters; ++t) {
							// Slots below 16 the material and technique leave alone read a null view (natively: whatever
							// an earlier draw left bound; Phase 3 parity checks it). A frame register the pipeline reads
							// is the epoch's own descriptor, patched in by the commit; on the Z-prepass the pixel stage's
							// per-frame textures are not bound yet, so a depth pipeline reading one is skipped as before.
							std::uint32_t index = t < kPixelTextureSlots ? a_lookups.nullTexture : kInvalidIndex;
							bool patch = false;
							if (t == kObjectBufferRegister)
								index = a_in.addresses.objectsIndex;
							else if (t == kBonesBufferRegister)
								index = a_in.addresses.bonesIndex;
							else if (!a_in.resolveTextures)
								index = 0;
							else if (t < kPixelTextureSlots && ((material.textureWritten >> t) & 1)) {
								if (!materialResolved) {
									resolved.deferred = true;
									resolved.texturesOk = false;
									resolved.missingTexture = t;
									break;
								}
								index = materialLookup->textureIndex[t];
							} else if (t == kShadowMaskSlot && technique.shadowMask)
								index = blocks.shadowMaskIndex;
							else if (const auto p = projectedSlot(t); p >= 0)
								index = a_lookups.projectedTextures[p];
							else if (t >= kPixelTextureSlots && !depthOnly) {
								index = 0;
								patch = usage.UsesTexture(t);
							}
							if (!a_in.resolveTextures && t != kObjectBufferRegister)
								index = 0;
							if (index == kInvalidIndex && usage.UsesTexture(t)) {
								resolved.texturesOk = false;
								resolved.missingTexture = t;
								break;
							}
							resolved.textures[t] = index == kInvalidIndex ? 0 : index;
							if (patch)
								resolved.patchRegisters.push_back(t);
						}

						// Samplers: the modes the material (or, for the shadow mask, the technique) sets.
						resolved.samplersOk = true;
						for (std::uint32_t s = 0; s < kSamplerRegisters; ++s) {
							std::uint32_t address = 0, filter = 0;
							if (s < kPixelTextureSlots && ((material.textureWritten >> s) & 1)) {
								address = material.addressModes[s];
								filter = material.filterModes[s] != kUnwrittenFilterMode ? material.filterModes[s] : technique.filterModes[s];
							} else if (s == kShadowMaskSlot && technique.shadowMask) {
								filter = technique.filterModes[s];
							} else if (projectedSlot(s) >= 0) {
								address = 3;  // SetupGeometry: wrap, anisotropic (engine notes: samplers)
								filter = 1;
							}
							if (filter == kUnwrittenFilterMode)
								filter = 0;
							const auto index = a_in.resolveTextures ? a_lookups.Sampler(address, filter) : 0u;
							if (index == kInvalidIndex && ((usage.samplers >> s) & 1)) {
								resolved.samplersOk = false;
								break;
							}
							resolved.samplers[s] = index == kInvalidIndex ? 0 : index;
						}
						if (a_cache) {
							auto& entry = a_cache->pairs[pairKey];
							entry.resolved = resolved;
							entry.resolved.recordIndex = kNoRecord;
							entry.resolved.skipReason = kNoSkip;
							entry.hasResolved = true;
						}
					}
					if (!resolved.texturesOk) {
						// The sample is recorded per skipped draw, as it was before, not per distinct pair.
						if (a_out.missingNext < a_out.missingTextures.size())
							a_out.missingTextures[a_out.missingNext++] = resolved.missingTexture;
						if (resolved.deferred)
							++a_out.deferredTextures;
						fail(Skip::Texture);
						continue;
					}
					if (!resolved.samplersOk) {
						fail(Skip::Sampler);
						continue;
					}
					std::copy(resolved.textures.begin(), resolved.textures.end(), bindings.textures);
					std::copy(resolved.samplers.begin(), resolved.samplers.end(), bindings.samplers);
					mark(0);

					// Constant buffers.
					auto& materialBlock = materialBlocks[pairKey];
					if (!materialBlock.first && !materialBlock.second) {
						// The pair's entry was checked (or rebuilt) when the pair was first met this build.
						BuildCache::Pair* cachedPair = nullptr;
						if (a_cache) {
							const auto found = a_cache->pairs.find(pairKey);
							cachedPair = found != a_cache->pairs.end() ? &found->second : nullptr;
						}
						auto pack = [&](const ConstantBlock& a_block, const StageLayout& a_layout, std::span<const std::int8_t> a_table, std::uint64_t a_variables, std::uint32_t a_first,
										BuildCache::PackedGroup* a_packed) {
							if (a_packed && a_packed->valid) {
								const auto address = block(nullptr, a_packed->size);
								if (address)
									std::memcpy(arena.At(address - base, a_packed->bytes.size()).data(), a_packed->bytes.data(), a_packed->bytes.size());
								return address;
							}
							const auto size = ConstantGroupSize(a_layout, a_table, a_variables, a_first);
							const auto address = block(nullptr, size);
							if (address) {
								const auto out = arena.At(address - base, std::max<std::size_t>(size, 16));
								PackConstantGroup(a_block, a_layout, a_table, a_variables, a_first, out);
								if (a_packed) {
									a_packed->size = size;
									a_packed->bytes.assign(out.begin(), out.end());
									a_packed->valid = true;
								}
							}
							return address;
						};
						materialBlock.first = pack(material.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerMaterial], kVSFirstVariable[kPerMaterial],
							cachedPair ? &cachedPair->vs : nullptr);
						// The PS group carries the frame's floats (IBLParams): a reused group has this frame's values
						// written over them, as PackConstantGroup would write them (an unwritten float packs as zero).
						const bool psReused = cachedPair && cachedPair->ps.valid && cachedPair->psPatchPositions.size() == a_in.materialPatchedFloats.size();
						materialBlock.second = pack(material.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial],
							cachedPair ? &cachedPair->ps : nullptr);
						if (cachedPair && !psReused) {
							cachedPair->psPatchPositions.clear();
							for (const auto index : a_in.materialPatchedFloats)
								cachedPair->psPatchPositions.push_back(PackedPositionOf(LightingPSLayout(), blocks.psTable, kPSGroups[kPerMaterial], kPSFirstVariable[kPerMaterial], index));
						}
						if (psReused && materialBlock.second) {
							auto out = arena.At(materialBlock.second - base, cachedPair->ps.bytes.size());
							for (std::size_t i = 0; i < cachedPair->psPatchPositions.size(); ++i) {
								const auto position = cachedPair->psPatchPositions[i];
								const auto index = a_in.materialPatchedFloats[i];
								if (position == ~0u || std::size_t(position) * 4 + 4 > out.size() || index >= material.ps.floats.size())
									continue;
								const float value = material.ps.Written(index) ? material.ps.floats[index] : 0.0f;
								std::memcpy(out.data() + std::size_t(position) * 4, &value, 4);
							}
						}
					}
					std::uint64_t geometryVS = 0, geometryPS = 0;
					{
						auto [templateIt, newTemplate] = geometryTemplates.try_emplace(object.pipelineIndex);
						auto& geometryTemplate = templateIt->second;
						BuildCache::Pipeline* cachedPipeline = nullptr;
						if (newTemplate && a_cache) {
							const auto found = a_cache->pipelines.find(object.pipelineIndex);
							cachedPipeline = found != a_cache->pipelines.end() ? &found->second : nullptr;
						}
						if (newTemplate && cachedPipeline && cachedPipeline->hasGeometry) {
							geometryTemplate.vs = cachedPipeline->geometry.vs;
							geometryTemplate.ps = cachedPipeline->geometry.ps;
							geometryTemplate.offsets = cachedPipeline->geometry.offsets;
							if (bindless) {
								auto upload = [&](const std::vector<std::byte>& a_group) {
									if (a_group.empty())
										return std::uint64_t{ 0 };
									const auto address = block(nullptr, a_group.size());
									if (address)
										std::memcpy(arena.At(address - base, std::max<std::size_t>(a_group.size(), 16)).data(), a_group.data(), a_group.size());
									return address;
								};
								geometryTemplate.vsAddress = upload(geometryTemplate.vs);
								geometryTemplate.psAddress = upload(geometryTemplate.ps);
							}
						} else if (newTemplate) {
							const auto vsSize = ConstantGroupSize(LightingVSLayout(), blocks.vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry]);
							const auto psSize = ConstantGroupSize(LightingPSLayout(), blocks.psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry]);
							geometryTemplate.vs.assign(std::max<std::size_t>(vsSize, 16), std::byte{});
							geometryTemplate.ps.assign(std::max<std::size_t>(psSize, 16), std::byte{});
							// The pipeline's own values, which is everything the objects do not override.
							const auto& pipelineConstants = a_tables.geometryConstants[object.pipelineIndex];
							PackConstantGroup(pipelineConstants.vs, LightingVSLayout(), blocks.vsTable, kVSGroups[kPerGeometry], kVSFirstVariable[kPerGeometry],
								geometryTemplate.vs);
							PackConstantGroup(pipelineConstants.ps, LightingPSLayout(), blocks.psTable, kPSGroups[kPerGeometry], kPSFirstVariable[kPerGeometry],
								geometryTemplate.ps);
							geometryTemplate.offsets = GeometryPatchOffsetsOf(blocks.vsTable, blocks.psTable);
							geometryTemplate.vs.resize(vsSize);
							geometryTemplate.ps.resize(psSize);
							if (cachedPipeline) {
								cachedPipeline->geometry.vs = geometryTemplate.vs;
								cachedPipeline->geometry.ps = geometryTemplate.ps;
								cachedPipeline->geometry.offsets = geometryTemplate.offsets;
								cachedPipeline->hasGeometry = true;
							}
							if (bindless) {
								auto upload = [&](const std::vector<std::byte>& a_group) {
									if (a_group.empty())
										return std::uint64_t{ 0 };  // the stage does not declare the buffer
									const auto address = block(nullptr, a_group.size());
									if (address)
										std::memcpy(arena.At(address - base, std::max<std::size_t>(a_group.size(), 16)).data(), a_group.data(), a_group.size());
									return address;
								};
								geometryTemplate.vsAddress = upload(geometryTemplate.vs);
								geometryTemplate.psAddress = upload(geometryTemplate.ps);
							}
						}
						if (bindless) {
							// One pair of blocks for the whole pipeline, written when the template was built.
							geometryVS = geometryTemplate.vsAddress;
							geometryPS = geometryTemplate.psAddress;
						} else {
							geometryVS = block(nullptr, geometryTemplate.vs.size());
							geometryPS = block(nullptr, geometryTemplate.ps.size());
							if (geometryVS && geometryPS) {
								auto vsOut = arena.At(geometryVS - base, std::max<std::size_t>(geometryTemplate.vs.size(), 16));
								auto psOut = arena.At(geometryPS - base, std::max<std::size_t>(geometryTemplate.ps.size(), 16));
								std::memcpy(vsOut.data(), geometryTemplate.vs.data(), geometryTemplate.vs.size());
								std::memcpy(psOut.data(), geometryTemplate.ps.data(), geometryTemplate.ps.size());
								PatchObjectGeometry(a_tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets,
									vsOut.subspan(0, geometryTemplate.vs.size()), psOut.subspan(0, geometryTemplate.ps.size()));
							}
						}
						// CS_DCLF_BINDLESS_PARITY=1: the record the shaders read against the group the constant
						// buffer path packs for the same object. The two derive from the same inputs through the
						// same unwritten-component rule, so anything but bit equality is a defect in the record's
						// layout or in the way it is filled, caught on the CPU with no readback and without
						// needing both forms in one run.
						if (bindlessParity && o < objectRecords.size()) {
							parityVS.assign(geometryTemplate.vs.begin(), geometryTemplate.vs.end());
							parityPS.assign(geometryTemplate.ps.begin(), geometryTemplate.ps.end());
							PatchObjectGeometry(a_tables, o, renderFlags, eye, previousEye, geometryTemplate.offsets, parityVS, parityPS);
							IndirectDraws::Stats parityStats{};
							CheckBindlessRecord(a_tables, o, objectRecords[o], eye, previousEye, geometryTemplate.offsets, parityVS, parityPS, parityStats);
							a_out.bindlessParityChecks += parityStats.bindlessParityChecks;
							a_out.bindlessParityMismatches += parityStats.bindlessParityMismatches;
						}
					}
					std::uint64_t lightBlock = 0;
					if (bindlessDraws) {
						// NumStrictLights 0, and nothing else is read: the zeroed frame slot, for every draw.
						lightBlock = sharedLightBlock;
					} else {
						const auto& lights = a_tables.lights[o];
						auto& cached = lightBlocks[(std::uint64_t(static_cast<std::uint32_t>(lights.roomIndex)) << 32) | lights.shadowBitMask];
						if (!cached) {
							const std::uint32_t header[4] = { 0, static_cast<std::uint32_t>(lights.roomIndex), lights.shadowBitMask, 0 };
							cached = block(nullptr, kStrictLightDataBytes);
							if (cached)
								std::memcpy(arena.At(cached - base, sizeof(header)).data(), header, sizeof(header));
						}
						lightBlock = cached;
					}
					const auto& permutation = a_tables.permutations[object.pipelineIndex];
					// SuppressExternalEmittance is the only per-object bit DCLF puts in this block, and it is
					// read at exactly one place in the whole shader tree - Effect.hlsl's GetLightingColor -
					// never by Lighting.hlsl or anything it includes. So for these pipelines it is dead, and
					// under bindless the block keys on the pipeline alone, which is what makes the binding
					// record identical for a (material, pipeline) pair. The non-bindless control keeps the bit,
					// so CaptureParity::ComparePermutation goes on measuring the real thing against the engine.
					const std::uint32_t extra = permutation.extraShaderDescriptor |
					                            ((!bindless && (object.flags & kObjectSuppressExternalEmittance)) ?
												        static_cast<std::uint32_t>(State::ExtraShaderDescriptors::SuppressExternalEmittance) :
												        0u);
					auto& permutationBlock = permutationBlocks[(std::uint64_t(object.pipelineIndex) << 32) | extra];
					if (!permutationBlock) {
						const std::uint32_t data[8] = { permutation.vertexShaderDescriptor, permutation.pixelShaderDescriptor, extra, permutation.extraFeatureDescriptor, 0, 0, 0, 0 };
						permutationBlock = block(data, sizeof(data));
					}
					std::uint64_t alphaBlock = 0;
					if (!bindlessDraws) {
						const std::uint32_t threshold = (object.flags & kObjectAlphaTest) ? (object.flags >> kObjectAlphaThresholdShift) & 0xFF : 0;
						auto& cached = alphaBlocks[threshold];
						if (!cached) {
							const float data[4] = { threshold / 255.0f, 0, 0, 0 };
							cached = block(data, sizeof(data));
						}
						alphaBlock = cached;
					}

					mark(1);
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
					// does not read it. It comes from the tables rather than off the property: the value is
					// animated and belongs to the same sample as the emissive colour.
					if (!bindlessDraws) {
						const float multiplier = (linearLighting && o < a_tables.emissiveMult.size()) ? a_tables.emissiveMult[o] : 1.0f;
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
							a_out.missingVertexConstants |= 1u << b;
						}
						if (((usage.pixelConstants >> b) & 1) && !bindings.pixelConstants[b]) {
							constantsOk = false;
							a_out.missingPixelConstants |= 1u << b;
						}
					}
					if (!constantsOk) {
						fail(Skip::Constants);
						continue;
					}

					if (dedup && recordIndex != kNoRecord) {
						// CS_DCLF_DEDUP_PARITY=1: the pair already has a record and this draw just rebuilt
						// one from scratch, so they must be byte-identical. This is the direct answer to
						// "is the record really the same for every draw of a pair", and the only check that
						// would catch a per-object dependency nobody has noticed.
						++a_out.recordParityChecks;
						if (std::memcmp(&records[recordIndex], &bindings, sizeof(DrawBindings)) != 0 && a_out.recordParityMismatches++ == 0)
							logger::warn("[DCLF] record dedup parity: object {} rebuilds a different record than its (material {}, pipeline {}) pair holds",
								o, object.materialIndex, object.pipelineIndex);
					} else {
						if (records.size() >= a_in.addresses.recordCapacity) {
							fail(Skip::RecordCapacity);
							continue;
						}
						recordIndex = static_cast<std::uint32_t>(records.size());
						records.push_back(bindings);
						for (const auto t : resolved.patchRegisters)
							a_out.framePatches.emplace_back(recordIndex, t);
						if (dedup)
							resolved.recordIndex = recordIndex;
					}
				}

				// The CPU template of what BuildDraws writes (checked with CS_DCLF_BUILD_PARITY).
				mark(2);
				auto sequence = a_tables.draws[o];
				sequence.pipelineIndex = blocks.setIndex;
				sequence.objectIndex = o;
				sequence.bindingsAddress = a_in.addresses.records + std::uint64_t(recordIndex) * sizeof(DrawBindings);
				if (decalGroup) {
					const std::uint32_t ordinal = a_tables.decalOrdinal[o];
					decalSlot.inputs = nullptr;  // drawn: the blank is not needed
					drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
						object.flags | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), ordinal });
					decalTemplates[(decalGroup - 1) & 1][ordinal] = sequence;
					++a_out.decalsDrawn;
					if (o < a_out.objectState.size())
						a_out.objectState[o] = kObjectStateDecal;
				} else {
					if (o < a_out.objectState.size())
						a_out.objectState[o] = kObjectStateDrawable;
					drawInputs.push_back({ blocks.setIndex, recordIndex, object.geometryIndex,
						object.flags | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius,
						static_cast<std::uint32_t>(o), 0 });
					sequences.push_back(sequence);
				}
				// Only what BuildDraws will actually write a sequence for counts as drawn. The tables now hold
				// the whole tracked set, so a candidate the gate drops must not be recorded here: the native
				// loop would skip its pass (it has none while the engine culls it, but it regains one the
				// moment the engine sees it again) and, worse, the Z-prepass draws exactly what the colour
				// epoch drew last frame, so a stale mark would write depth for an object nothing then shades.
				// The gate is a per-object flag test and so is predictable here; frustum rejection is not,
				// which is what the false-negative counter exists to catch.
				if (!depthOnly && o < a_tables.objectGeometry.size() && (!a_in.requireNativeVisible || (object.flags & kObjectNativeVisible)))
					a_out.drawn.push_back(o);
				// The indirect draw fetches vertices and indices through the buffer's device address and size:
				// a slice that does not cover the draw reads zeros, and the object collapses without any
				// other sign. Checked here because nothing on the D3D11 side sees the Vulkan slice.
				const std::uint64_t vertexNeeded = std::uint64_t(geometry.vertexCount) * geometry.vertexStride;
				const std::uint64_t indexNeeded = (std::uint64_t(geometry.firstIndex) + geometry.indexCount) * sizeof(std::uint16_t);
				if (geometry.vertexBytes < vertexNeeded || geometry.indexBytes < indexNeeded) {
					if (a_out.shortBuffers++ == 0)
						a_out.shortBuffer = { o, vertexNeeded, indexNeeded };
				}
				// Per DRAW: the sequence, the draw input, the drawn mark and the slice check.
				mark(3);
			}

			PackGeometryDraws(a_tables, a_out.geometryDraws);
			PackBoneRows(a_tables, a_out.boneRows);
			mark(5);
		}

		/**
		 * @brief The shadow epoch's frame-shared part and its per-mode inputs, from the tables and the lookups:
		 * pure. The per-view blocks are the commit's, because the views are captured while the engine draws
		 * them, after the build may have started.
		 */
		// The objects a mode's inputs draw, as the geometry the native shadow loop withholds (PassCapture).
		std::shared_ptr<const PassCapture::ClaimSet> ShadowClaimSet(const std::vector<DrawInput>& a_inputs, const SceneStore::Tables& a_tables)
		{
			auto claims = std::make_shared<PassCapture::ClaimSet>();
			claims->reserve(a_inputs.size());
			for (const auto& input : a_inputs) {
				if (input.objectIndex < a_tables.objectGeometry.size())
					if (const auto* geometry = a_tables.objectGeometry[input.objectIndex])
						claims->insert(geometry);
			}
			return claims;
		}

		void BuildShadowPayload(const ShadowInputs& a_in, const SceneStore::Tables& a_tables, const Lookups& a_lookups, ShadowPayload& a_out)
		{
			a_out.Reset();
			a_out.inputs = a_in;
			const std::uint64_t base = a_in.addresses.constants;
			auto& arena = a_out.arena;
			auto& records = a_out.records;
			auto block = [&](const void* a_data, std::size_t a_size) -> std::uint64_t {
				const auto offset = arena.Allocate(a_size);
				if (offset == ~0ull)
					return 0;
				if (a_data)
					std::memcpy(arena.At(offset, a_size).data(), a_data, a_size);
				return base + offset;
			};

			// ---- What every view shares: the object records, bone rows, geometry table, binding records.
			auto& objects = a_out.objects;
			objects.resize(std::min<std::size_t>(a_tables.objects.size(), kMaxObjects));
			for (std::size_t r = 0; r < objects.size(); ++r)
				BuildObjectRecord(a_tables, static_cast<std::uint32_t>(r), a_in.renderFlags, objects[r]);
			PackBoneRows(a_tables, a_out.boneRows);
			PackGeometryDraws(a_tables, a_out.geometries);
			// The binding records, built once with the per-view registers (b0, b12) unset; each view slot
			// uploads its own copy of them naming its blocks at the head of the arena.
			(void)arena.Allocate(kShadowMaterialBlocksOffset);
			DrawBindings plain{};
			// No register the Utility shaders declare may be left at address zero: a pipeline that reads
			// one arrives from the background compiler seconds after the first epoch, and a null read is
			// a device loss. Everything not supplied below reads zeros.
			static constexpr std::size_t kZeroBlockBytes = 1024;
			const std::uint64_t zeros = block(nullptr, kZeroBlockBytes);
			for (auto& address : plain.vertexConstants)
				address = zeros;
			for (auto& address : plain.pixelConstants)
				address = zeros;
			// Community Shaders' SharedData (b5) and FeatureData (b6): the alpha-tested pixel stage samples
			// its diffuse with SharedData::MipBias. Packed from the copies the inputs carry, as the main epochs do.
			if (!a_in.sharedData.empty()) {
				plain.pixelConstants[kSharedDataRegister] = block(a_in.sharedData.data(), a_in.sharedData.size());
				plain.vertexConstants[kSharedDataRegister] = plain.pixelConstants[kSharedDataRegister];
			}
			if (!a_in.featureData.empty()) {
				plain.pixelConstants[kFeatureDataRegister] = block(a_in.featureData.data(), a_in.featureData.size());
				plain.vertexConstants[kFeatureDataRegister] = plain.pixelConstants[kFeatureDataRegister];
			}
			const std::uint32_t nullIndex = a_lookups.nullTexture == Lookups::kNone ? 0u : a_lookups.nullTexture;
			for (auto& index : plain.textures)
				index = nullIndex;
			plain.textures[kObjectBufferRegister] = a_in.addresses.objectsIndex;
			plain.textures[kBonesBufferRegister] = a_in.addresses.bonesIndex;
			const std::uint32_t wrapAnisotropic = a_lookups.Sampler(static_cast<std::uint32_t>(RE::BSGraphics::TextureAddressMode::kWrapSWrapT),
				static_cast<std::uint32_t>(RE::BSGraphics::TextureFilterMode::kAnisotropic));
			for (auto& index : plain.samplers)
				index = wrapAnisotropic == Lookups::kNone ? 0u : wrapAnisotropic;
			records.push_back(plain);  // record 0: every caster without alpha testing
			auto& objectRecord = a_out.objectRecord;
			objectRecord.assign(a_tables.objects.size(), ~0u);
			ankerl::unordered_dense::map<const RE::BSShaderMaterial*, std::uint32_t> recordByMaterial;
			const bool haveShadowMaterials = a_tables.shadowMaterial.size() == a_tables.objects.size();
			for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects; ++o) {
				const auto& object = a_tables.objects[o];
				if (object.flags & kObjectNoShadow)
					continue;
				if (!(a_tables.shadowTechnique[o] & 0x80)) {
					objectRecord[o] = 0;
					continue;
				}
				// Alpha-tested: the material's diffuse and texture offset, one record per material.
				const auto* material = haveShadowMaterials ? a_tables.shadowMaterial[o] : nullptr;
				if (!material) {
					++a_out.skippedTexture;
					continue;
				}
				if (auto it = recordByMaterial.find(material); it != recordByMaterial.end()) {
					objectRecord[o] = it->second;
					continue;
				}
				auto* srv = a_tables.shadowDiffuse[o];
				const auto textureIt = srv ? a_lookups.shadowTextures.find(srv) : a_lookups.shadowTextures.end();
				if (textureIt == a_lookups.shadowTextures.end()) {
					if (srv)
						++a_out.deferredTextures;
					++a_out.skippedTexture;
					continue;
				}
				if (textureIt->second == Lookups::kNone || records.size() >= kShadowRecordCapacity) {
					++a_out.skippedTexture;
					continue;
				}
				DrawBindings bindings = plain;
				// The texture transform off the material now, not from the walk: shader-property controllers
				// (BSLightingShaderPropertyFloatController::Update) move it between Main::Draw, where the walk
				// starts, and BeforeShadowMaps, where this build is kicked.
				const std::array<float, 4> texcoord{ material->texCoordOffset[0].x, material->texCoordOffset[0].y, material->texCoordScale[0].x,
					material->texCoordScale[0].y };
				bindings.vertexConstants[1] = block(texcoord.data(), sizeof(texcoord));
				bindings.textures[0] = textureIt->second;
				const auto index = static_cast<std::uint32_t>(records.size());
				records.push_back(bindings);
				recordByMaterial.emplace(material, index);
				objectRecord[o] = index;
			}

			// ---- The inputs per render mode among the captured views: every caster with a ready pipeline
			// for its technique under that mode. The cascades share one set; a spot light has its own.
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				auto& inputs = a_out.inputList[m];
				if (!a_in.modeUsed[m])
					continue;
				const std::uint32_t modeBits = ShadowModeBits(PassCapture::kFirstShadowMode + m);
				for (std::size_t o = 0; o < a_tables.objects.size() && o < kMaxObjects && inputs.size() < kMaxInputs; ++o) {
					const auto& object = a_tables.objects[o];
					if ((object.flags & kObjectNoShadow) || objectRecord[o] == ~0u)
						continue;
					const std::uint32_t technique = a_tables.shadowTechnique[o] | modeBits;
					const ShadowPipelineKey key{ technique, (object.flags & kObjectTwoSided) ? kRasterTwoSided : 0u,
						VertexLayoutOf(a_tables.geometries[object.geometryIndex].vertexDesc) };
					const auto pipelineIt = a_lookups.shadowPipelines.find(key);
					if (pipelineIt == a_lookups.shadowPipelines.end()) {
						++a_out.deferredPipelines;
						++a_out.skippedPipeline;
						continue;
					}
					if (pipelineIt->second == Lookups::kNone) {
						++a_out.skippedPipeline;
						continue;
					}
					inputs.push_back({ pipelineIt->second, objectRecord[o], object.geometryIndex, (object.flags & ~kObjectDecal) | kInputDrawable,
						{ object.boundCenter[0], object.boundCenter[1], object.boundCenter[2] }, object.boundRadius, static_cast<std::uint32_t>(o), 0 });
				}
			}
		}

		/**
		 * @brief Resolves the descriptor entries an epoch's build reads (render thread, descriptor service
		 * active): the null texture, the sampler table, the projected textures, and every material slot used
		 * this frame. Also the keep-alive: GpuTextures evicts an entry unused for kEvictFrames, and only the
		 * render thread's Resolve counts as a use, so every used slot is touched here each epoch.
		 */
		void RefreshMaterialLookups(const SceneStore::Tables& a_tables, std::uint32_t a_frame, const SceneStore::ProjectedTextures& a_projected, Lookups& a_lookups)
		{
			auto& textures = GpuTextures::Get();
			// Any change a build can observe bumps the generation, including an entry resolved for the first
			// time: a job built before it deferred those draws, and must not stand in for a build made after.
			auto note = [&](std::uint32_t& a_slot, std::uint32_t a_value) {
				if (a_slot != a_value)
					++a_lookups.generation;
				a_slot = a_value;
			};
			note(a_lookups.nullTexture, textures.NullIndex());
			if (!a_lookups.samplersResolved) {
				for (std::uint32_t address = 0; address < 4; ++address)
					for (std::uint32_t filter = 0; filter < 5; ++filter)
						a_lookups.samplers[Lookups::SamplerIndex(address, filter)] = textures.Sampler(address, filter);
				a_lookups.samplersResolved = true;
			}
			for (std::size_t i = 0; i < a_lookups.projectedTextures.size(); ++i) {
				const std::uint32_t index = a_projected.valid ? textures.Resolve(a_projected.views[i]) : Lookups::kNone;
				note(a_lookups.projectedTextures[i], index);
			}
			a_lookups.materials.resize(a_tables.materials.size());
			for (std::size_t slot = 0; slot < a_tables.materials.size(); ++slot) {
				if (slot >= a_tables.materialLastUsed.size() || a_tables.materialLastUsed[slot] != a_frame)
					continue;
				auto& entry = a_lookups.materials[slot];
				const auto& key = a_tables.materialSlotKey[slot];
				if (entry.key != key) {
					entry.key = key;
					entry.resolved = false;
				}
				const auto& material = a_tables.materials[slot];
				// Resolved from these same views, with none evicted since, recently enough that they are still
				// marked used: the indices stand (the shadow, Z-prepass and colour epochs each refresh).
				if (entry.resolved && entry.written == material.textureWritten && entry.texturesGeneration == textures.Generation() &&
					a_frame - entry.resolvedFrame < GpuTextures::kRestampFrames) {
					bool same = true;
					for (std::uint32_t t = 0; t < kPixelTextureSlots && same; ++t)
						same = !((material.textureWritten >> t) & 1) || entry.views[t] == material.textures[t];
					if (same)
						continue;
				}
				for (std::uint32_t t = 0; t < kPixelTextureSlots; ++t) {
					if (!((material.textureWritten >> t) & 1))
						continue;
					const std::uint32_t index = textures.Resolve(material.textures[t]);
					note(entry.textureIndex[t], index);
					entry.views[t] = material.textures[t];
				}
				if (!entry.resolved)
					++a_lookups.generation;
				entry.resolved = true;
				entry.written = material.textureWritten;
				entry.texturesGeneration = textures.Generation();
				entry.resolvedFrame = a_frame;
			}
			// The technique's shadow mask, per used pipeline: the frame's view, so it is refreshed every epoch.
			a_lookups.pipelines.resize(std::max(a_lookups.pipelines.size(), a_tables.pipelines.size()));
			for (std::size_t p = 0; p < a_tables.pipelines.size(); ++p) {
				if (!a_tables.PipelineUsed(p, a_frame))
					continue;
				const auto& technique = a_tables.techniqueConstants[p];
				auto& entry = a_lookups.pipelines[p];
				const std::uint32_t index = technique.shadowMask ? textures.Resolve(technique.shadowMaskTexture) : Lookups::kNone;
				note(entry.shadowMaskIndex, index);
			}
		}

		/** @brief The shadow epoch's entries: the alpha-tested casters' diffuse textures, and the pipelines of the modes in use. */
		void RefreshShadowLookups(const SceneStore::Tables& a_tables, const std::array<bool, kShadowModeCount>& a_modeUsed, DXGI_FORMAT a_dsvFormat, Lookups& a_lookups)
		{
			auto& textures = GpuTextures::Get();
			auto& pipelines = DrawPipelines::Get();
			auto& programs = ShaderPrograms::Get();
			auto* utility = globals::game::utilityShader;
			for (auto* srv : a_tables.shadowTextureSet) {
				const std::uint32_t index = textures.Resolve(srv);
				auto [it, inserted] = a_lookups.shadowTextures.try_emplace(srv, index);
				if (inserted) {
					++a_lookups.generation;
				} else if (it->second != index) {
					it->second = index;
					++a_lookups.generation;
				}
			}
			if (!utility)
				return;
			for (const auto& key : a_tables.shadowKeysUsed) {
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (!a_modeUsed[m])
						continue;
					const std::uint32_t modeBits = ShadowModeBits(PassCapture::kFirstShadowMode + m);
					const ShadowPipelineKey viewKey{ key.technique | modeBits, key.rasterFlags, key.vertexLayout };
					const auto* program = programs.FindShadow(viewKey.technique, *utility);
					const std::uint32_t set = program ? pipelines.FindShadow(viewKey, *program, a_dsvFormat) : DrawPipelines::kNotReady;
					const std::uint32_t index = set == DrawPipelines::kNotReady ? Lookups::kNone : set;
					auto [it, inserted] = a_lookups.shadowPipelines.try_emplace(viewKey, index);
					if (inserted || it->second != index) {
						++a_lookups.generation;
						it->second = index;
					}
				}
			}
		}

		constexpr std::size_t kAsyncColour = 0;
		constexpr std::size_t kAsyncZPrepass = 1;
		constexpr std::size_t kAsyncShadow = 2;

		// Whether a build made for `a_job` serves an epoch whose inputs are `a_epoch`: every scalar the build
		// read has to agree, the lookup generation is checked separately after the commit's refresh.
		// Whether BuildMainPayload reads MainInputs::eye/previousEye: the non-bindless path writes eye-relative
		// PerGeometry groups; a bindless build only for CS_DCLF_BINDLESS_PARITY.
		bool BuildReadsEye(const MainInputs& a_in)
		{
			return !a_in.bindless || a_in.bindlessParity;
		}

		bool SameInputs(const MainInputs& a_job, const MainInputs& a_epoch)
		{
			auto sameEye = [](const RE::NiPoint3& a, const RE::NiPoint3& b) { return std::memcmp(&a, &b, sizeof(RE::NiPoint3)) == 0; };
			return a_job.frameNumber == a_epoch.frameNumber && a_job.depthOnly == a_epoch.depthOnly && a_job.hybrid == a_epoch.hybrid &&
			       a_job.resolveTextures == a_epoch.resolveTextures && a_job.bindless == a_epoch.bindless && a_job.bindlessDraws == a_epoch.bindlessDraws &&
			       a_job.dedupParity == a_epoch.dedupParity && a_job.bindlessParity == a_epoch.bindlessParity && a_job.withholding == a_epoch.withholding &&
			       a_job.requireNativeVisible == a_epoch.requireNativeVisible && a_job.linearLighting == a_epoch.linearLighting &&
			       a_job.renderFlags == a_epoch.renderFlags && sameEye(a_job.eye, a_epoch.eye) && sameEye(a_job.previousEye, a_epoch.previousEye) &&
			       a_job.vsFrameMask == a_epoch.vsFrameMask && a_job.psFrameMask == a_epoch.psFrameMask && a_job.decalCount == a_epoch.decalCount &&
			       a_job.drewLastFrame == a_epoch.drewLastFrame && a_job.materialPatchedFloats == a_epoch.materialPatchedFloats &&
			       a_job.addresses == a_epoch.addresses &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration;
		}

		bool SameShadowInputs(const ShadowInputs& a_job, const ShadowInputs& a_epoch)
		{
			return a_job.frameNumber == a_epoch.frameNumber && a_job.renderFlags == a_epoch.renderFlags &&
			       std::memcmp(&a_job.refEye, &a_epoch.refEye, sizeof(RE::NiPoint3)) == 0 && a_job.modeUsed == a_epoch.modeUsed &&
			       a_job.addresses == a_epoch.addresses && a_job.sharedData == a_epoch.sharedData && a_job.featureData == a_epoch.featureData &&
			       a_job.lookupGeneration == a_epoch.lookupGeneration && a_job.tablesGeneration == a_epoch.tablesGeneration &&
			       a_job.sceneRebuilds == a_epoch.sceneRebuilds;
		}

		// CS_DCLF_ASYNC=probe: two builds of the same inputs, compared byte for byte. The first difference is
		// named by buffer and offset.
		bool SamePayload(const MainPayload& a, const MainPayload& b, std::string& a_difference)
		{
			auto bytesDiffer = [&](const char* a_name, const void* a_lhs, std::size_t a_lhsBytes, const void* a_rhs, std::size_t a_rhsBytes) {
				if (a_lhsBytes != a_rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} bytes", a_name, a_lhsBytes, a_rhsBytes);
					return true;
				}
				const auto* l = static_cast<const std::uint8_t*>(a_lhs);
				const auto* r = static_cast<const std::uint8_t*>(a_rhs);
				for (std::size_t i = 0; i < a_lhsBytes; ++i) {
					if (l[i] != r[i]) {
						a_difference = fmt::format("{}: byte {} of {} ({:#x} vs {:#x})", a_name, i, a_lhsBytes, l[i], r[i]);
						return true;
					}
				}
				return false;
			};
			auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(a_lhs[0])>;
				return bytesDiffer(a_name, a_lhs.data(), a_lhs.size() * sizeof(T), a_rhs.data(), a_rhs.size() * sizeof(T));
			};
			if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || vectorDiffers("records", a.records, b.records) ||
				vectorDiffers("sequences", a.sequences, b.sequences) || vectorDiffers("inputs", a.inputList, b.inputList) ||
				vectorDiffers("geometries", a.geometryDraws, b.geometryDraws) || vectorDiffers("objects", a.objectRecords, b.objectRecords) ||
				vectorDiffers("bones", a.boneRows, b.boneRows) || vectorDiffers("patches", a.framePatches, b.framePatches) ||
				vectorDiffers("drawn", a.drawn, b.drawn))
				return false;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				if (vectorDiffers("decal templates", a.decalTemplates[group], b.decalTemplates[group]))
					return false;
			}
			if (a.decalCount != b.decalCount || a.skipped != b.skipped || a.missingVertexConstants != b.missingVertexConstants ||
				a.missingPixelConstants != b.missingPixelConstants || a.decalsDrawn != b.decalsDrawn || a.shortBuffers != b.shortBuffers ||
				a.deferredTextures != b.deferredTextures) {
				a_difference = "counts";
				return false;
			}
			return true;
		}

		bool SameShadowPayload(const ShadowPayload& a, const ShadowPayload& b, std::string& a_difference)
		{
			auto vectorDiffers = [&](const char* a_name, const auto& a_lhs, const auto& a_rhs) {
				using T = std::remove_cvref_t<decltype(a_lhs[0])>;
				const std::size_t lhsBytes = a_lhs.size() * sizeof(T), rhsBytes = a_rhs.size() * sizeof(T);
				if (lhsBytes != rhsBytes) {
					a_difference = fmt::format("{}: {} vs {} bytes", a_name, lhsBytes, rhsBytes);
					return true;
				}
				if (lhsBytes && std::memcmp(a_lhs.data(), a_rhs.data(), lhsBytes) != 0) {
					const auto* l = reinterpret_cast<const std::uint8_t*>(a_lhs.data());
					const auto* r = reinterpret_cast<const std::uint8_t*>(a_rhs.data());
					std::size_t k = 0;
					while (l[k] == r[k])
						++k;
					a_difference = fmt::format("{}: byte {} of {} ({:#x} vs {:#x})", a_name, k, lhsBytes, l[k], r[k]);
					return true;
				}
				return false;
			};
			if (vectorDiffers("constants", a.arena.Bytes(), b.arena.Bytes()) || vectorDiffers("records", a.records, b.records) ||
				vectorDiffers("object records", a.objectRecord, b.objectRecord) || vectorDiffers("objects", a.objects, b.objects) ||
				vectorDiffers("bones", a.boneRows, b.boneRows) || vectorDiffers("geometries", a.geometries, b.geometries))
				return false;
			for (std::size_t m = 0; m < a.inputList.size(); ++m) {
				if (vectorDiffers("inputs", a.inputList[m], b.inputList[m]))
					return false;
			}
			if (a.skippedTexture != b.skippedTexture || a.skippedPipeline != b.skippedPipeline || a.deferredTextures != b.deferredTextures ||
				a.deferredPipelines != b.deferredPipelines) {
				a_difference = "the skip counters";
				return false;
			}
			return true;
		}
	}

	namespace
	{
		// A commit's own uploads on the render thread, staged directly (StagedUploadBatch) and submitted when the
		// commit ends: each is one memcpy into mapped staging here and one copy in the submission, instead of a
		// trip through the upload pass's per-upload bookkeeping. The batch comes from a pool of released ones.
		class CommitUploads
		{
		public:
			explicit CommitUploads(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
			{
				for (const auto& candidate : a_pool) {
					if (candidate.use_count() == 1) {
						batch = candidate;
						break;
					}
				}
				if (!batch) {
					batch = org::runtime::StagedUploadBatch::Create(std::size_t{ 1 } << 20);
					a_pool.push_back(batch);
				}
				batch->Reset();
			}
			~CommitUploads()
			{
				if (!batch->Entries().empty())
					org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(batch));
			}
			CommitUploads(const CommitUploads&) = delete;
			CommitUploads& operator=(const CommitUploads&) = delete;

			template <class Target>
			void operator()(const Target& a_target, const void* a_data, std::size_t a_bytes, std::uint64_t a_offset)
			{
				if (a_data && a_bytes)
					batch->Stage(org::runtime::UploadTarget::FromShared(a_target), static_cast<std::size_t>(a_offset), a_data, a_bytes);
			}

		private:
			std::shared_ptr<org::runtime::StagedUploadBatch> batch;
		};

	}

	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool);

	struct IndirectDraws::Impl
	{
		std::shared_ptr<Resources> resources;
		// What the resources were created for; a change rebuilds them (and the graph).
		TargetFormats formats{};
		std::uint32_t width = 0, height = 0;
		std::uint32_t pipelineGeneration = ~0u;
		std::uint64_t serial = 0;
		std::optional<Capture> pending;  // this frame's main-pass bindings, until the epoch runs

		// The shadow views (CS_DCLF_SHADOWS). Their own resources and CPU staging, so the main path's are
		// untouched; the frame's shared uploads happen in the first view's epoch.
		std::shared_ptr<ShadowResources> shadow;
		bool shadowSetupFailed = false;
		// One view's culling counters, sampled every so many epochs and mapped a few frames later.
		struct ShadowCullReadback
		{
			winrt::com_ptr<ID3D11Buffer> count;
			std::uint32_t copiedFrame = 0;
			std::uint32_t view = 0, mode = 0;
		};
		std::optional<ShadowCullReadback> shadowCullReadback;
		std::uint32_t shadowCullEpochs = 0;
		std::uint32_t shadowLoggedTargets = 0;
		void ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats);
		/** @brief A view the hook captured for the frame's epoch (ExecuteShadowView, ExecuteShadowFrame). */
		struct PendingView
		{
			std::uint32_t viewId = 0, renderMode = 0, modeIndex = 0, targetIndex = 0, slice = 0;
			std::uint32_t x = 0, y = 0, width = 0, height = 0;
			float minDepth = 0.0f, maxDepth = 1.0f;
			RE::NiPoint3 eye;
			bool hasViewProj = false;
			std::array<float, 16> viewProj{};
			float viewBlock[12] = {};  // PerTechnique: HighDetailRange, ParabolaParam, EyeDelta
			std::array<std::byte, 1024> perFrame{};
			std::uint32_t perFrameBytes = 0;
			DXGI_FORMAT dsvFormat = DXGI_FORMAT_UNKNOWN;
		};
		std::vector<PendingView> pendingViews;
		std::vector<DrawBindings> shadowSlotRecords;  // one slot's copy of the records, while it uploads
		// CS_DCLF_SHADOW_OWNERSHIP=static: the claim set built from the inputs of a mode, published once per
		// input rebuild (the views of one frame that share a mode share the inputs and the claims).
		void PublishShadowClaims(std::uint32_t a_renderMode, const std::vector<DrawInput>& a_inputs, std::shared_ptr<const PassCapture::ClaimSet> a_built,
			IndirectDraws::ShadowStats& a_stats);
		RE::NiPoint3 shadowRefEye;
		std::uint64_t shadowSerial = 0;
		ShadowPayload shadowPayload;
		// The shadow job (CS_DCLF_ASYNC): kicked at BeforeShadowMaps, joined by ExecuteShadowFrame. The render
		// modes are known only once the views are captured, so the job builds last frame's set; the epoch's
		// own set is checked like every other input.
		struct ShadowJob
		{
			AsyncWorker::JobHandle handle;
			ShadowInputs inputs;
			std::array<bool, kShadowModeCount> modes{};
			bool modesKnown = false;
			std::uint32_t views = 0;  // last frame's view count: the record slots the job stages
			std::uint32_t loggedStale = 0;
			std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> stagedPool;
		} shadowJob;
		ShadowPayload shadowProbePayload;
		ShadowInputs PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed) const;
		void DropShadowJob(IndirectDraws::Stats& a_stats);
		std::uint32_t shadowLoggedReasons = 0;
		bool ShadowNotReady(std::uint32_t a_reason, const char* a_what)
		{
			if (!((shadowLoggedReasons >> a_reason) & 1)) {
				shadowLoggedReasons |= 1u << a_reason;
				logger::info("[DCLF] shadow views not ready: {}", a_what);
			}
			return false;
		}
		bool SetupShadow();
		bool ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target);

		// The two main epochs' payloads (colour, then the Z-prepass): what their builds produce and their
		// commits upload. Kept past the commit, because the upload pass reads them when the epoch executes.
		std::array<MainPayload, 2> mainPayload;
		// Per main job: the staged upload batches its builds write (StageMainPayload), reused once released.
		std::array<std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>, 2> stagedPools;
		// The commits' own uploads on the render thread (CommitUploads).
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>> commitStagedPool;
		std::array<BuildCache, 2> buildCaches;  // indexed like mainPayload
		BuildCache* CacheFor(std::size_t a_job) { return BuildCacheEnabled() ? &buildCaches[a_job] : nullptr; }
		std::array<std::uint32_t, 4> decalWords{};  // the count buffer's decal words, uploaded per colour epoch

		/** @brief The per-frame constant blocks of an epoch from the capture's mirrors (render thread; records the Z-prepass's bytes for the replay). */
		void PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, const Resources& a_resources, FrameBlocks& a_out);
		/** @brief The build's inputs, snapshotted on the render thread. Without a capture (a job kicked ahead of the epoch) the eye is the replayed one. */
		MainInputs PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store);

		// The main epochs' jobs (CS_DCLF_ASYNC), indexed like mainPayload (kAsyncColour, kAsyncZPrepass): the
		// colour job is kicked at Prepass, the Z-prepass job at the end of EarlyPrepass, each joined by its
		// epoch. The inputs are kept to check the epoch's against; the frame-slot masks the epoch will supply
		// are predicted from the same epoch's last ones and validated the same way.
		struct MainJob
		{
			AsyncWorker::JobHandle handle;
			MainInputs inputs;
			std::uint32_t vsMask = 0, psMask = 0;
			bool masksKnown = false;
			std::uint32_t loggedStale = 0;
		};
		std::array<MainJob, 2> mainJobs;
		MainPayload probePayload;  // CS_DCLF_ASYNC=probe: the inline build to compare the worker's against
		void KickMainJob(bool a_depthOnly, const RE::NiPoint3* a_eye, const RE::NiPoint3* a_previousEye, IndirectDraws::Stats& a_stats);
		void DropMainJob(std::size_t a_job, IndirectDraws::Stats& a_stats);
		void LogStaleMainJob(std::size_t a_job, const MainInputs& a_actual);
		/** @brief Inside the epoch's preparation: the frame textures and blocks, the uploads, the PassFrame. */
		bool CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
			const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats);

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
			// Per decal group, by slot: what the CPU expects there (a culled slot reads back with an index
			// count of zero and is otherwise identical).
			std::array<std::vector<DrawSequence>, kDecalGroups> expectedDecals;
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

		// CS_DCLF_SET_PARITY: one snapshot per frame in flight, read back a few frames later.
		struct SetParityFrame
		{
			winrt::com_ptr<ID3D11Buffer> staging;
			std::uint32_t frame = 0;
			std::uint32_t framesLeft = 0;
			std::vector<std::uint8_t> depthState, colourState;  // MainPayload::objectState of the two builds
			// Per object: bit 0 withheld from the native loop this frame (claimed, and registered by the
			// engine), bit 1 native-visible, bit 2 alpha tested, bit 3 claimed.
			std::vector<std::uint8_t> flags;
			std::vector<const RE::BSGeometry*> geometry;
		};
		std::deque<SetParityFrame> setParityFrames;
		std::vector<winrt::com_ptr<ID3D11Buffer>> setParityStaging;  // released stagings, reused
		struct SetParityCounts
		{
			std::uint32_t frames = 0, framesWithDamage = 0, skipped = 0;
			std::uint32_t depthOnly = 0, colourOnly = 0, colourUnpublished = 0, withheldUndrawn = 0, withheldCulled = 0;
			std::uint32_t colourDrawnTotal = 0, depthDrawnTotal = 0;
			std::uint32_t alphaDepthOnly = 0, alphaColourOnly = 0, alphaWithheldUndrawn = 0;
			std::uint32_t samples = 0;
		} setParity;
		void CheckSetParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_depth, const MainPayload& a_colour);

		void CheckBuildParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_payload, IndirectDraws::Stats& a_stats);

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
			state->recordCapacity = BindlessDraws() ? kMaxRecordsDeduplicated : kMaxDraws;
			state->records = buffer(std::uint64_t(state->recordCapacity) * sizeof(DrawBindings), "cs.dclf.records");
			// Structured rather than raw, because the shaders read it through an SRV at t127 instead of
			// through a device address the way the constants and the binding records are read.
			state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
			state->objects->SetName("cs.dclf.objects");
			state->objects->Materialize();
			state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
			state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
			state->bones->SetName("cs.dclf.bones");
			state->bones->Materialize();
			state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
			// Twice kMaxDraws: phase 1 and the colour segment write the first half, phase 2 the second. The
			// CPU records where phase 2's draw starts, so the two need ranges fixed in advance rather than
			// one range shared through an atomic counter.
			state->sequences = CreateWords(std::uint64_t(kSequenceSlots) * sizeof(DrawSequence) / 4, true, "cs.dclf.sequences");
			state->count = CreateWords(kCountWords, true, "cs.dclf.draw-count");
			// One word per object in the frame's tables: what the depth segment's culling decided, read by
			// the colour segment so that it draws exactly the same set.
			state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.visibility");
			state->inputs = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, "cs.dclf.draw-inputs");
			state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.geometries");
			state->buildDraws = LoadComputeProgram(device, kBuildDrawsShader, kBuildDrawsConstantWords);
			if (!state->buildDraws)
				return NotReady(7, "the BuildDraws program could not be created");
			state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
			if (!state->dispatchSignature)
				return NotReady(7, "the BuildDraws dispatch signature could not be created");
			state->latch = std::make_shared<org::LatchBlock>("cs.dclf.latch", static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), host->FrameSlots());
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
				state->sequencesD3D11 = wrap(*state->sequences, std::uint64_t(kSequenceSlots) * sizeof(DrawSequence));

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
				// The draw-count buffer is read back for the culling counters whether or not parity is on. The
				// width must cover every counter word: a view that stops short reads zeros for the rest, and
				// a counter that is always zero reads exactly like a clean result.
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->countD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->count, desc);
			}
			if (SetParityEnabled()) {
				D3D11_BUFFER_DESC desc{};
				desc.ByteWidth = kMaxObjects * sizeof(std::uint32_t);
				desc.Usage = D3D11_USAGE_DEFAULT;
				desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				desc.StructureByteStride = sizeof(std::uint32_t);
				state->visibilityD3D11 = RenderGraphRuntime::Get().WrapBuffer(*state->visibility, desc);
			}
			state->frameConstants = buffer(kFrameConstantBytes, "cs.dclf.frame-constants");
			state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
			state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
			state->frameConstantsAddress = device.GetBufferDeviceAddress({ state->frameConstants->GetAPIResource().GetHandle(), 0 });
			if (!state->constantsAddress || !state->recordsAddress || !state->frameConstantsAddress) {
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
				// Attachable and readable, from one import. The HZB build reads the same image the draws
				// write, and importing it twice would give the graph two resources it believes are unrelated:
				// it would order nothing between the depth draws and the read, and insert no barrier, so the
				// build would reduce whatever happened to be there - in practice the cleared far plane.
				org::TextureDescription nativeDepthDesc = depthDesc;
				nativeDepthDesc.hasSRV = true;
				nativeDepthDesc.srvFormat = DepthReadFormat(nativeDepthDesc.format);
				state->nativeDepth = ImportImage(device, info.image, nativeDepthDesc, "DCLF native depth");
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

			// Phase 4: the hierarchical depth buffer.
			//
			// Mip 0 is half the next power of two of the depth, so the chain is a clean sequence of halvings
			// and a mip level can be chosen from a screen-space extent by log2 alone. Padding to a power of
			// two is what makes that true; the padded texels are outside the real depth and the build fills
			// them with the far plane, which suppresses culling rather than causing it.
			if (state->hybrid) {
				auto nextPowerOfTwo = [](std::uint32_t a_value) {
					std::uint32_t result = 1;
					while (result < a_value)
						result <<= 1;
					return result;
				};
				state->hzbWidth = std::max(1u, nextPowerOfTwo(state->width) / 2);
				state->hzbHeight = std::max(1u, nextPowerOfTwo(state->height) / 2);
				state->hzbMips = 1;
				for (std::uint32_t size = std::max(state->hzbWidth, state->hzbHeight); size > 1; size >>= 1)
					++state->hzbMips;

				org::TextureDescription hzbDesc{};
				for (std::uint32_t mip = 0; mip < state->hzbMips; ++mip)
					hzbDesc.imageDimensions.push_back({ std::max(1u, state->hzbWidth >> mip), std::max(1u, state->hzbHeight >> mip), 0, 0 });
				hzbDesc.format = rhi::Format::R32_Float;
				hzbDesc.channels = 1;
				hzbDesc.hasUAV = true;
				hzbDesc.uavFormat = hzbDesc.format;
				hzbDesc.hasSRV = true;
				hzbDesc.srvFormat = hzbDesc.format;
				state->hzb = org::PixelBuffer::CreateSharedUnmaterialized(hzbDesc);
				state->hzb->SetName("cs.dclf.hzb");

				state->hzbProgram = LoadComputeProgram(device, kHzbShader, kHzbConstantWords);
				if (!state->hzbProgram) {
					// The culling falls back to frustum only; the frame is unaffected.
					logger::warn("[DCLF] The HZB compute program could not be created; occlusion culling stays off");
					state->hzb.reset();
				}
			}

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

	bool IndirectDraws::Impl::SetupShadow()
	{
		if (shadow)
			return true;
		if (shadowSetupFailed)
			return false;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!host)
			return ShadowNotReady(0, "no render graph");
		auto device = host->GetDesc().device;
		auto state = std::make_shared<ShadowResources>();
		auto buffer = [&](std::uint64_t a_bytes, const char* a_name) {
			auto created = org::Buffer::CreateShared(rhi::HeapType::DeviceLocal, a_bytes, false);
			created->SetName(a_name);
			return created;
		};
		state->constants = buffer(kShadowConstantBytes, "cs.dclf.shadow.constants");
		state->records = buffer(std::uint64_t(kMaxShadowViews) * kShadowRecordCapacity * sizeof(DrawBindings), "cs.dclf.shadow.records");
		state->objects = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxObjects, sizeof(BindlessObject), false);
		state->objects->SetName("cs.dclf.shadow.objects");
		state->objects->Materialize();
		state->objectsIndex = state->objects->GetSRVInfo(0).slot.index;
		state->bones = org::Buffer::CreateUnmaterializedStructuredBuffer(kMaxBoneRows, 16, false);
		state->bones->SetName("cs.dclf.shadow.bones");
		state->bones->Materialize();
		state->bonesIndex = state->bones->GetSRVInfo(0).slot.index;
		for (std::uint32_t s = 0; s < kMaxShadowViews; ++s) {
			state->sequences[s] = CreateWords(std::uint64_t(kMaxDraws) * sizeof(DrawSequence) / 4, true, fmt::format("cs.dclf.shadow.sequences{}", s).c_str());
			state->count[s] = CreateWords(kCountWords, true, fmt::format("cs.dclf.shadow.draw-count{}", s).c_str());
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = kCountWords * sizeof(std::uint32_t);
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(std::uint32_t);
			state->countD3D11[s] = RenderGraphRuntime::Get().WrapBuffer(*state->count[s], desc);
		}
		state->visibility = CreateWords(kMaxObjects, true, "cs.dclf.shadow.visibility");
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
			state->inputs[m] = CreateWords(std::uint64_t(kMaxInputs) * sizeof(DrawInput) / 4, false, fmt::format("cs.dclf.shadow.draw-inputs{}", m).c_str());
		state->geometries = CreateWords(std::uint64_t(kMaxGeometries) * sizeof(GeometryDraw) / 4, false, "cs.dclf.shadow.geometries");
		state->buildDraws = LoadComputeProgram(device, kBuildDrawsShader, kBuildDrawsConstantWords);
		if (!state->buildDraws) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws compute program could not be created");
		}
		state->dispatchSignature = CreateDispatchSignature(device, state->buildDraws->layout->GetHandle());
		if (!state->dispatchSignature) {
			shadowSetupFailed = true;
			return ShadowNotReady(1, "the BuildDraws dispatch signature could not be created");
		}
		state->latch = std::make_shared<org::LatchBlock>("cs.dclf.shadow.latch", static_cast<std::uint32_t>(kMaxShadowViews * sizeof(BuildDrawsLatch)), host->FrameSlots());
		state->constantsAddress = device.GetBufferDeviceAddress({ state->constants->GetAPIResource().GetHandle(), 0 });
		state->recordsAddress = device.GetBufferDeviceAddress({ state->records->GetAPIResource().GetHandle(), 0 });
		if (!state->constantsAddress || !state->recordsAddress) {
			shadowSetupFailed = true;
			return ShadowNotReady(2, "no device address for the shadow buffers");
		}
		shadow = state;
		host->AddExtension(kShadowExtensionId, [state] { return std::make_unique<ShadowExtension>(state); });
		logger::info("[DCLF] shadow view graph resources created");
		return true;
	}

	bool IndirectDraws::Impl::ImportShadowDepth(std::uint32_t a_index, std::uint32_t a_target)
	{
		auto* renderer = globals::game::renderer;
		auto* host = RenderGraphRuntime::Get().Host();
		if (!renderer || !host || !shadow || a_index >= kShadowDepthTargets)
			return false;
		const auto& data = renderer->GetDepthStencilData().depthStencils[a_target];
		if (!data.texture || !data.views[0])
			return ShadowNotReady(3, "the shadow map has no texture");
		if (shadow->depth[a_index] && shadow->depthTexture[a_index] == data.texture)
			return true;
		DxvkOrgInteropResourceInfo info{};
		if (!RenderGraphRuntime::Get().DescribeResource(data.texture, info) || info.kind != DXVK_ORG_INTEROP_RESOURCE_IMAGE)
			return ShadowNotReady(4, "the shadow map cannot be described");
		D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
		data.views[0]->GetDesc(&dsvDesc);
		org::TextureDescription desc{};
		desc.format = rhi::helpers::ToRHI(dsvDesc.Format);
		desc.channels = 1;
		desc.hasDSV = true;
		desc.dsvFormat = desc.format;
		static constexpr const char* kNames[kShadowDepthTargets] = { "DCLF shadow maps (ESRAM)", "DCLF shadow maps", "DCLF volumetric shadow maps (ESRAM)" };
		auto imported = ImportImage(host->GetDesc().device, info.image, desc, kNames[a_index]);
		if (!imported)
			return ShadowNotReady(5, "the shadow map could not be imported (not in the general layout?)");
		shadow->depth[a_index] = std::move(imported);
		shadow->depthTexture[a_index] = data.texture;
		shadow->depthLayers[a_index] = info.image.arrayLayers;
		// A new resource for the passes to bind: the graph is rebuilt on the next epoch.
		host->AddExtension(kShadowExtensionId, [state = shadow] { return std::make_unique<ShadowExtension>(state); });
		logger::info("[DCLF] shadow map {} imported: {}x{}, {} slices, format {}", a_target, info.image.extent.width, info.image.extent.height,
			info.image.arrayLayers, static_cast<int>(dsvDesc.Format));
		return true;
	}

	bool IndirectDraws::ShadowsEnabled()
	{
		return Toggles::Get().Active().shadows;
	}

	void IndirectDraws::BeginShadowFrame(const RE::NiPoint3& a_eye)
	{
		impl->shadowRefEye = a_eye;
		impl->pendingViews.clear();
	}

	void IndirectDraws::ExecuteShadowView(std::uint32_t a_viewId, std::uint32_t a_renderMode)
	{
		// The hook's half: capture the view - where the engine has just drawn, and the constants it drew
		// with - for the frame's single epoch (ExecuteShadowFrame). Nothing is drawn here.
		if (!ShadowsEnabled() || failed)
			return;
		ScopedPerfEvent event("CS DCLF: shadow view capture");
		const auto start = std::chrono::steady_clock::now();
		++shadowStats.views;
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		auto notReady = [&](ShadowNotReady a_reason) {
			++shadowStats.notReady;
			++shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)];
		};
		if (!pipelines.Enabled() || !utility || !impl->SetupShadow())
			return notReady(ShadowNotReady::Setup);
		// The tables are not read here: under CS_DCLF_ASYNC the scene walk is still writing them while the
		// engine draws the shadow maps. ExecuteShadowFrame checks them after the join.
		const auto* shadowView = ShadowViews::Get().At(a_viewId);
		if (!shadowView)
			return notReady(ShadowNotReady::Tables);
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + kShadowModeCount)
			return notReady(ShadowNotReady::Tables);
		// A focus shadow holds one actor's casters, not the scene's: it stays native until S4 decides its set.
		if (shadowView->focus) {
			++shadowStats.focusSkipped;
			return;
		}
		// Where the engine has just drawn: the target and slice come from the renderer's state, because the
		// descriptor's own fields are filled only when the draw allocates them (engine notes: shadow maps).
		auto& shadowState = globals::game::shadowState->GetRuntimeData();
		const std::uint32_t target = shadowState.depthStencil;
		const std::uint32_t slice = shadowState.depthStencilSlice;
		const std::uint32_t targetIndex = target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM                     ? 0u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS                           ? 1u :
		                                  target == RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM ? 2u :
		                                                                                                                     ~0u;
		if (targetIndex == ~0u || !impl->ImportShadowDepth(targetIndex, target)) {
			// Which target, once per target: anything here is a view the design has not met.
			if (target < 32 && !((impl->shadowLoggedTargets >> target) & 1)) {
				impl->shadowLoggedTargets |= 1u << target;
				logger::info("[DCLF] shadow view {} ({}, light {} descriptor {}, mode {:#x}) draws into depth target {} slice {}; it stays native",
					a_viewId, ShadowViews::KindName(shadowView->kind), shadowView->lightIndex, shadowView->descriptor, a_renderMode, target, slice);
			}
			return notReady(ShadowNotReady::Depth);
		}
		if (impl->pendingViews.size() >= kMaxShadowViews)
			return notReady(ShadowNotReady::Capacity);

		auto& view = impl->pendingViews.emplace_back();
		view.viewId = a_viewId;
		view.renderMode = a_renderMode;
		view.modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		view.targetIndex = targetIndex;
		view.slice = slice;
		if (auto* dsv = globals::game::renderer->GetDepthStencilData().depthStencils[target].views[0]) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsv->GetDesc(&dsvDesc);
			view.dsvFormat = dsvDesc.Format;
		}
		view.x = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftX));
		view.y = static_cast<std::uint32_t>(std::max(0.0f, shadowState.viewPort.TopLeftY));
		view.width = static_cast<std::uint32_t>(shadowState.viewPort.Width);
		view.height = static_cast<std::uint32_t>(shadowState.viewPort.Height);
		view.minDepth = shadowState.viewPort.MinDepth;
		view.maxDepth = shadowState.viewPort.MaxDepth;
		view.eye = shadowState.posAdjust.getEye();
		// The view's PerTechnique block (b0): HighDetailRange (LOD landscape only; zero until owned),
		// ParabolaParam from the engine's two globals as its SetupTechnique reads them, and the eye delta
		// from the frame's reference eye to this view's.
		{
			static const REL::Relocation<float*> parabolaRadius{ REL::Offset(0x2035df8) };
			static const REL::Relocation<float*> parabolaSide{ REL::Offset(0x2035dfc) };
			const float radius = *parabolaRadius.get();
			view.viewBlock[4] = radius != 0.0f ? 1.0f / radius : 0.0f;
			view.viewBlock[5] = *parabolaSide.get();
			// c2 (DCLFEyeDelta) is no longer read: the records are absolute and Utility.hlsl subtracts the view's
			// own CameraPosAdjust. Left zero.
			view.viewBlock[8] = view.viewBlock[9] = view.viewBlock[10] = 0.0f;
		}
		// VS_PerFrame (b12) as the engine wrote it for this view, taken now because the next view rewrites
		// it: from the mirror, or from Community Shaders' copy of the same buffer (Globals: CacheFramebuffer)
		// until the mirror has seen a write.
		auto& mirror = ConstantMirror::Get();
		if (auto* perFrame = *globals::game::perFrame.get()) {
			mirror.Watch(perFrame);
			const auto contents = mirror.Contents(perFrame);
			if (contents.size() >= 48 * sizeof(float)) {
				view.perFrameBytes = static_cast<std::uint32_t>(std::min<std::size_t>(contents.size(), view.perFrame.size()));
				std::memcpy(view.perFrame.data(), contents.data(), view.perFrameBytes);
			}
		}
		if (!view.perFrameBytes) {
			const auto& cached = globals::game::frameBufferCached.data;
			static_assert(sizeof(cached) >= 48 * sizeof(float) && sizeof(cached) <= 1024);
			view.perFrameBytes = sizeof(cached);
			std::memcpy(view.perFrame.data(), &cached, sizeof(cached));
		}
		std::memcpy(view.viewProj.data(), reinterpret_cast<const float*>(view.perFrame.data()) + 32, sizeof(float) * 16);
		view.hasViewProj = true;
		shadowStats.captureMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

	void IndirectDraws::ExecuteShadowFrame()
	{
		auto& pending = impl->pendingViews;
		if (!ShadowsEnabled() || failed || pending.empty()) {
			pending.clear();
			impl->DropShadowJob(stats);
			return;
		}
		ScopedPerfEvent event("CS DCLF: shadow views (CPU)");
		const auto start = std::chrono::steady_clock::now();
		auto notReady = [&](ShadowNotReady a_reason) {
			shadowStats.notReady += static_cast<std::uint32_t>(pending.size());
			shadowStats.notReadyReasons[static_cast<std::size_t>(a_reason)] += static_cast<std::uint32_t>(pending.size());
			pending.clear();
			impl->DropShadowJob(stats);
		};
		auto& pipelines = DrawPipelines::Get();
		auto* utility = globals::game::utilityShader;
		if (!pipelines.Enabled() || !utility || !impl->shadow)
			return notReady(ShadowNotReady::Setup);
		const auto indirect = GetShadowIndirectState();
		if (!indirect.valid) {
			impl->ShadowNotReady(6, "no shadow pipeline in the set yet");
			return notReady(ShadowNotReady::Pipelines);
		}
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		if (tables.objects.empty() || tables.shadowTechnique.size() != tables.objects.size())
			return notReady(ShadowNotReady::Tables);
		const std::uint32_t frameNumber = store.GetFrame();
		auto resources = impl->shadow;
		auto& textures = GpuTextures::Get();
		std::array<bool, kShadowModeCount> modeUsed{};
		for (const auto& view : pending)
			modeUsed[view.modeIndex] = true;
		// One pipeline set per shadow map format: every target the engine has is D16 (engine notes), and a
		// view whose target differed would rebuild the set on every epoch, so the first view's is taken.
		const DXGI_FORMAT dsvFormat = pending.front().dsvFormat;
		double prepareMs = 0.0, inputsMs = 0.0, blocksMs = 0.0, bodyMs = 0.0;

		ShadowInputs in = impl->PrepareShadowInputs(store, *resources, modeUsed);
		impl->shadowJob.modes = modeUsed;
		impl->shadowJob.modesKnown = true;
		impl->shadowJob.views = static_cast<std::uint32_t>(pending.size());
		auto& payload = impl->shadowPayload;
		auto& async = stats.async[kAsyncShadow];
		bool usedWorkerBuild = false;

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(RenderGraphRuntime::Segment::ShadowView, [&](org::RenderGraph&) {
			struct BodyTimer
			{
				std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
				double& out;
				~BodyTimer() { out = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); }
			} bodyTimer{ {}, bodyMs };
			bodyTimer.start = std::chrono::steady_clock::now();

			// The worker's build if one was kicked and it was built for exactly these inputs, else the build
			// here. Joined before the lookups are refreshed: the worker reads them until it is done.
			const auto prepareStart = std::chrono::steady_clock::now();
			textures.BeginFrame(frameNumber);
			auto& job = impl->shadowJob;
			auto& worker = AsyncWorker::Get();
			AsyncWorker::WaitResult joined = AsyncWorker::WaitResult::None;
			if (job.handle) {
				joined = worker.Wait(job.handle, AsyncWaitBudget());
				if (joined == AsyncWorker::WaitResult::Late)
					worker.Cancel(job.handle);
			}
			auto& lookups = store.MutableLookups();
			RefreshMaterialLookups(tables, frameNumber, store.GetProjectedTextures(), lookups);
			RefreshShadowLookups(tables, modeUsed, dsvFormat, lookups);
			in.lookupGeneration = lookups.generation;
			bool useAsync = false;
			if (job.handle) {
				switch (joined) {
				case AsyncWorker::WaitResult::Done:
					if (SameShadowInputs(job.inputs, in)) {
						useAsync = true;
					} else {
						++async.stale;
						if (job.loggedStale++ < 4) {
							const auto& k = job.inputs;
							logger::info("[DCLF] async shadow: the job's inputs are stale (frame {} vs {}, render flags {:#x} vs {:#x}, modes {}{}{} vs {}{}{}, shared data {}, feature data {}, tables {} vs {}, lookups {} vs {}, resources {})",
								k.frameNumber, in.frameNumber, k.renderFlags, in.renderFlags, int(k.modeUsed[0]), int(k.modeUsed[1]), int(k.modeUsed[2]),
								int(in.modeUsed[0]), int(in.modeUsed[1]), int(in.modeUsed[2]), k.sharedData == in.sharedData ? "same" : "differs",
								k.featureData == in.featureData ? "same" : "differs", k.tablesGeneration, in.tablesGeneration, k.lookupGeneration,
								in.lookupGeneration, k.addresses == in.addresses ? "same" : "changed");
						}
					}
					break;
				case AsyncWorker::WaitResult::Late:
					++async.late;
					break;
				case AsyncWorker::WaitResult::Failed:
					++async.failed;
					break;
				default:
					++async.cancelled;
					break;
				}
				job.handle = {};
			}
			usedWorkerBuild = useAsync;
			if (useAsync) {
				++async.used;
				if (AsyncModeSetting() == AsyncMode::Probe) {
					BuildShadowPayload(job.inputs, tables, lookups, impl->shadowProbePayload);
					++async.probeCompared;
					std::string difference;
					if (!SameShadowPayload(payload, impl->shadowProbePayload, difference) && async.probeDiffer++ == 0)
						logger::warn("[DCLF] async shadow probe: the worker's build differs from the inline one: {}", difference);
				}
			} else {
				++async.builtInline;
				BuildShadowPayload(in, tables, lookups, payload);
			}
			prepareMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - prepareStart).count();

			// Nothing to draw until this commit publishes the shape again (so a failed one draws nothing, rather
			// than a reused recording reading latch values this execution never wrote).
			resources->frame.store(nullptr, std::memory_order_release);
			CommitUploads uploads(impl->commitStagedPool);
			// ---- The commit: the shared uploads, the per-mode inputs, then per view its blocks at its slot of
			// the arena's head, its copy of the records naming them, its count buffer zeroed, and the view.
			const auto inputsStart = std::chrono::steady_clock::now();
			const std::uint64_t base = resources->constantsAddress;
			auto& arena = payload.arena;
			auto& records = payload.records;
			// The worker's build staged what does not depend on the views (StageShadowPayload): one submission,
			// ahead of this commit's own uploads. A build made here, or staged against resources since recreated,
			// is uploaded from its vectors.
			const bool staged = useAsync && payload.staged && payload.stagedFor == resources.get();
			const std::uint32_t stagedSlots = staged ? payload.stagedSlots : 0;
			if (staged) {
				org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(payload.staged));
			} else {
				if (!payload.objects.empty())
					uploads(resources->objects, payload.objects.data(), payload.objects.size() * sizeof(BindlessObject), 0);
				if (!payload.boneRows.empty()) {
					const std::size_t boneBytes = std::min<std::size_t>(payload.boneRows.size() * sizeof(float), std::size_t(kMaxBoneRows) * 16);
					uploads(resources->bones, payload.boneRows.data(), boneBytes, 0);
				}
				if (!payload.geometries.empty())
					uploads(resources->geometries, payload.geometries.data(), std::min<std::size_t>(payload.geometries.size(), kMaxGeometries) * sizeof(GeometryDraw), 0);
			}
			shadowStats.records = static_cast<std::uint32_t>(records.size());
			shadowStats.skippedTexture = payload.skippedTexture;
			shadowStats.skippedPipeline = payload.skippedPipeline;
			shadowStats.deferredTextures = payload.deferredTextures;
			shadowStats.deferredPipelines = payload.deferredPipelines;
			for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
				const auto& inputs = payload.inputList[m];
				if (!modeUsed[m])
					continue;
				if (!staged && !inputs.empty())
					uploads(resources->inputs[m], inputs.data(), inputs.size() * sizeof(DrawInput), 0);
				shadowStats.inputs = static_cast<std::uint32_t>(inputs.size());
			}
			inputsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - inputsStart).count();

			const auto blocksStart = std::chrono::steady_clock::now();
			auto frame = std::make_shared<ShadowFrame>();
			const auto& previousShape = resources->published;
			const std::uint32_t latchSlot = RenderGraphRuntime::Get().Host()->CurrentFrameSlot();
			resources->labels.clear();
			frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
			frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
			frame->indirect = indirect;
			auto& slotRecords = impl->shadowSlotRecords;
			static const std::uint32_t zero[kCountWords] = {};
			for (std::uint32_t slot = 0; slot < pending.size(); ++slot) {
				const auto& view = pending[slot];
				const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
				const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
				std::memcpy(arena.At(viewBlockOffset, sizeof(view.viewBlock)).data(), view.viewBlock, sizeof(view.viewBlock));
				std::memcpy(arena.At(perFrameOffset, view.perFrameBytes).data(), view.perFrame.data(), view.perFrameBytes);
				const std::uint64_t recordsOffset = std::uint64_t(slot) * kShadowRecordCapacity * sizeof(DrawBindings);
				if (slot >= stagedSlots) {
					slotRecords = records;
					for (auto& record : slotRecords) {
						record.vertexConstants[0] = base + viewBlockOffset;
						record.pixelConstants[0] = base + viewBlockOffset;
						record.vertexConstants[kPerFrameVertexRegister] = base + perFrameOffset;
						record.pixelConstants[kPerFrameVertexRegister] = base + perFrameOffset;
					}
					uploads(resources->records, slotRecords.data(), slotRecords.size() * sizeof(DrawBindings), recordsOffset);
					uploads(resources->count[slot], zero, sizeof(zero), 0);
				}
				const auto inputCount = static_cast<std::uint32_t>(payload.inputList[view.modeIndex].size());
				// The view's values into its latch: frustum culling alone (mode 1), the single phase, and no
				// engine-visibility gate - a caster is drawn whether or not the main camera kept it.
				BuildDrawsLatch latch{};
				latch.dispatch[0] = (inputCount + 63) / 64;
				latch.dispatch[1] = 1;
				latch.dispatch[2] = 1;
				latch.drawCount = inputCount;
				latch.cullFlags = view.hasViewProj ? 1u : 0u;
				latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
				FoldEyeIntoViewProj(view.viewProj, view.eye, latch.viewProj);
				resources->latch->WriteValue(latchSlot, slot * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)), latch);
				resources->labels.push_back({ view.viewId, view.renderMode, slot });
				ShadowFrameView out{};
				out.slot = slot;
				out.modeIndex = view.modeIndex;
				std::uint32_t previousCapacity = 0;
				if (previousShape)
					for (const auto& previous : previousShape->views)
						if (previous.slot == slot)
							previousCapacity = previous.capacity;
				out.capacity = GrowCapacity(previousCapacity, inputCount, kMaxDraws);
				out.x = view.x;
				out.y = view.y;
				out.width = view.width;
				out.height = view.height;
				out.minDepth = view.minDepth;
				out.maxDepth = view.maxDepth;
				out.target = view.targetIndex;
				out.slice = view.slice;
				out.recordsAddress = resources->recordsAddress + recordsOffset;
				frame->views.push_back(out);
			}
			// Staged, only the view head this epoch wrote goes up from here; the rest of the arena is the worker's.
			const auto& bytes = arena.Bytes();
			const std::size_t arenaBytes = staged ? std::min<std::size_t>(bytes.size(), pending.size() * kShadowViewSlotBytes) : bytes.size();
			if (arenaBytes)
				uploads(resources->constants, bytes.data(), arenaBytes, 0);
			blocksMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blocksStart).count();
			static std::uint32_t logged = 0;
			if (logged++ % 600 == 0) {
				std::string views;
				for (std::size_t v = 0; v < frame->views.size() && v < pending.size(); ++v) {
					const auto& view = frame->views[v];
					views += fmt::format("{}view {} mode {:#x} target {} slice {} at ({} {}) {}x{} {} inputs (capacity {})", views.empty() ? "" : "; ", pending[v].viewId,
						pending[v].renderMode, view.target, view.slice, view.x, view.y, view.width, view.height, payload.inputList[view.modeIndex].size(), view.capacity);
				}
				logger::info("[DCLF] shadow epoch: {} views ({} without a pipeline, {} without a texture), {} records: {}", frame->views.size(),
					shadowStats.skippedPipeline, shadowStats.skippedTexture, shadowStats.records, views);
			}
			if (previousShape && previousShape->SameShape(*frame)) {
				resources->frame.store(previousShape, std::memory_order_release);
			} else {
				frame->generation = ++resources->shapeGenerations;
				resources->published = frame;
				resources->frame.store(std::move(frame), std::memory_order_release);
			}
		});
		const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		shadowStats.prepareMs += prepareMs;
		shadowStats.inputsMs += inputsMs;
		shadowStats.blocksMs += blocksMs;
		shadowStats.executeMs += totalMs - bodyMs;  // the graph's own compile, prepare and record
		if (ok) {
			++shadowStats.epochs;
			shadowStats.viewsDrawn += static_cast<std::uint32_t>(pending.size());
			impl->ReadShadowCullCounters(frameNumber, shadowStats);
			// Static shadow ownership: what this frame's epoch drew for a mode is what that mode's views'
			// registrations are withheld for, from the next frame on.
			if (PassCapture::ShadowWithholdingEnabled()) {
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
					if (modeUsed[m])
						impl->PublishShadowClaims(PassCapture::kFirstShadowMode + m, payload.inputList[m], usedWorkerBuild ? payload.claims[m] : nullptr, shadowStats);
				}
			}
			pending.clear();
		} else {
			logger::error("[DCLF] the shadow epoch failed; the render graph is disabled");
			notReady(ShadowNotReady::Epoch);
		}
		shadowStats.cpuMs += totalMs;
	}

	ShadowInputs IndirectDraws::Impl::PrepareShadowInputs(const SceneStore& a_store, const ShadowResources& a_resources, const std::array<bool, kShadowModeCount>& a_modeUsed) const
	{
		ShadowInputs in;
		in.frameNumber = a_store.GetFrame();
		in.renderFlags = a_store.GetMainPassRenderFlags();
		in.refEye = shadowRefEye;
		in.modeUsed = a_modeUsed;
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.records = a_resources.recordsAddress;
		in.addresses.objectsIndex = a_resources.objectsIndex;
		in.addresses.bonesIndex = a_resources.bonesIndex;
		in.addresses.recordCapacity = kShadowRecordCapacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.sceneRebuilds = a_store.GetSceneRebuilds();
		if (auto* csState = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&csState->lastSharedData);
			in.sharedData.assign(shared, shared + sizeof(State::SharedDataCB));
			if (!csState->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(csState->lastFeatureData.data());
				in.featureData.assign(feature, feature + csState->lastFeatureData.size());
			}
		}
		return in;
	}

	void IndirectDraws::KickShadowBuild()
	{
		// The shadow epoch's inputs are final from here to AfterShadowMaps: the scene phase has just built the
		// tables and the frame's reference eye is set. The build runs on the worker while the engine draws
		// the shadow maps, for last frame's render modes (a change is stale, and built inline).
		impl->DropShadowJob(stats);
		if (!ShadowsEnabled() || failed || !AsyncJobEnabled("shadow"))
			return;
		auto& async = stats.async[kAsyncShadow];
		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto& job = impl->shadowJob;
		// With the scene walk on the worker the tables are its until the join, and this build is queued behind
		// it: they are checked by the epoch instead.
		const bool tablesReady = store.ScenePending() || (!tables.objects.empty() && tables.shadowTechnique.size() == tables.objects.size());
		if (!impl->shadow || !job.modesKnown || !DrawPipelines::Get().Enabled() || !globals::game::utilityShader || !tablesReady) {
			++async.notKicked;
			return;
		}
		job.inputs = impl->PrepareShadowInputs(store, *impl->shadow, job.modes);
		++async.kicked;
		const auto* tablesPtr = &tables;
		const auto* lookups = &store.GetLookups();
		auto* payload = &impl->shadowPayload;
		auto* pool = &job.stagedPool;
		const ShadowInputs inputs = job.inputs;
		const bool claims = PassCapture::ShadowWithholdingEnabled();
		job.handle = AsyncWorker::Get().Submit("shadow", [inputs, tablesPtr, lookups, payload, pool, target = impl->shadow, slots = job.views, claims](std::stop_token) {
			BuildShadowPayload(inputs, *tablesPtr, *lookups, *payload);
			StageShadowPayload(*payload, *target, slots, *pool);
			if (claims)
				for (std::uint32_t m = 0; m < kShadowModeCount; ++m)
					if (inputs.modeUsed[m])
						payload->claims[m] = ShadowClaimSet(payload->inputList[m], *tablesPtr);
		});
	}

	void IndirectDraws::Impl::DropShadowJob(IndirectDraws::Stats& a_stats)
	{
		if (!shadowJob.handle)
			return;
		AsyncWorker::Get().Cancel(shadowJob.handle);
		++a_stats.async[kAsyncShadow].dropped;
		shadowJob.handle = {};
	}

	void IndirectDraws::Impl::ReadShadowCullCounters(std::uint32_t a_frame, IndirectDraws::ShadowStats& a_stats)
	{
		auto* context = globals::d3d::context;
		if (shadowCullReadback) {
			// Frames, not epochs: several views run per frame, and the copy needs the GPU to have finished
			// the sampled view's epoch, which three Presents later it has.
			if (a_frame - shadowCullReadback->copiedFrame < 3)
				return;
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (SUCCEEDED(context->Map(shadowCullReadback->count.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
				a_stats.cullDrawn = words[0];
				a_stats.cullRejected = words[1];
				a_stats.cullTested = words[2];
				a_stats.cullSampledView = shadowCullReadback->view;
				a_stats.cullSampledMode = shadowCullReadback->mode;
				context->Unmap(shadowCullReadback->count.get(), 0);
			}
			shadowCullReadback.reset();
			return;
		}
		// One epoch in 127, and within it the views in turn, so every view of the frame gets sampled.
		const auto epoch = shadowCullEpochs++;
		if ((epoch % 127) != 0 || !shadow || shadow->labels.empty())
			return;
		const auto& view = shadow->labels[(epoch / 127) % shadow->labels.size()];
		if (view.slot >= kMaxShadowViews || !shadow->countD3D11[view.slot])
			return;
		D3D11_BUFFER_DESC desc{};
		shadow->countD3D11[view.slot]->GetDesc(&desc);
		desc.Usage = D3D11_USAGE_STAGING;
		desc.BindFlags = 0;
		desc.MiscFlags = 0;
		desc.StructureByteStride = 0;
		desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ShadowCullReadback readback;
		if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, readback.count.put())))
			return;
		ScopedPerfEvent event("CS DCLF: shadow culling readback");
		context->CopyResource(readback.count.get(), shadow->countD3D11[view.slot].get());
		readback.copiedFrame = a_frame;
		readback.view = view.viewId;
		readback.mode = view.renderMode;
		shadowCullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::PublishShadowClaims(std::uint32_t a_renderMode, const std::vector<DrawInput>& a_inputs, std::shared_ptr<const PassCapture::ClaimSet> a_built,
		IndirectDraws::ShadowStats& a_stats)
	{
		if (a_renderMode < PassCapture::kFirstShadowMode || a_renderMode >= PassCapture::kFirstShadowMode + PassCapture::kShadowModes)
			return;
		const auto start = std::chrono::steady_clock::now();
		const auto modeIndex = a_renderMode - PassCapture::kFirstShadowMode;
		// The worker's set when its build was the one drawn, else built here from the same inputs.
		auto claims = a_built ? std::move(a_built) : ShadowClaimSet(a_inputs, SceneStore::Get().GetTables());
		a_stats.claimed[modeIndex] = static_cast<std::uint32_t>(claims->size());
		PassCapture::Get().PublishShadowClaims(modeIndex, std::move(claims));
		a_stats.claimMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
	}

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

	void IndirectDraws::PublishClaims()
	{
		if (!PassCapture::WithholdingEnabled())
			return;
		const auto frame = SceneStore::Get().GetFrame();
		auto& capture = PassCapture::Get();

		// Hole detector. Everything claimed when this frame's passes were registered should have been
		// drawn by the colour epoch that has just run; the native loop was told not to draw it. Counted on
		// the CPU, exactly, with no readback.
		// A hole is an object that was withheld *and* not drawn. Being claimed is not enough on its own:
		// an object the engine culled this frame is never registered, so it is never withheld either, and
		// nobody was going to draw it. The test for "the engine would have drawn it" is that its pass was
		// captured this frame, which is exactly what the tables are built from.
		auto& store = SceneStore::Get();
		auto& captureStats = capture.MutableStats();
		captureStats.claimed = captureStats.holes = 0;
		// The report's own interval, so a hole in any frame is seen rather than only in the last one.
		struct HoleReport
		{
			std::uint32_t frames = 0, framesWithHoles = 0, holes = 0, samples = 0;
			std::array<std::uint32_t, static_cast<std::size_t>(Ineligible::Count)> byReason{};
			std::uint32_t inTables = 0, handedBack = 0;
		};
		static HoleReport report;
		std::uint32_t frameHoles = 0;
		if (const auto previous = capture.CurrentClaims()) {
			for (const auto* geometry : *previous) {
				const auto* accumulated = store.FindAccumulatedPass(geometry);
				if (!accumulated)
					continue;  // the engine culled it; withholding never came into it
				++captureStats.claimed;
				if (!capture.WithheldThisFrame(geometry))
					continue;  // not withheld (fading as it registered), or handed back at EarlyPrepass: native
				const auto drawn = impl->drawnFrame.find(geometry);
				if (drawn != impl->drawnFrame.end() && drawn->second == frame)
					continue;
				++captureStats.holes;
				++frameHoles;
				bool fromAccumulate = false;
				const Ineligible reason = store.ReasonThisFrame(geometry, &fromAccumulate);
				++report.byReason[static_cast<std::size_t>(reason)];
				const std::int32_t object = store.FindObject(geometry);
				report.inTables += object >= 0;
				if (report.samples++ < 30) {
					logger::info("[DCLF] hole, frame {}: '{}' withheld and not drawn - {} ({}), {}, pass technique {:#x} list {}, last drawn by DCLF {}", frame,
						geometry->name.c_str(), kIneligibleNames[static_cast<std::size_t>(reason)], fromAccumulate ? "this frame's accumulate phase" : "the scene phase",
						object >= 0 ? fmt::format("object {} in the tables", object) : std::string("not in the tables"), accumulated->technique, accumulated->subPass,
						drawn == impl->drawnFrame.end() ? std::string("never") : fmt::format("{} frames ago", frame - drawn->second));
				}
			}
		}
		++report.frames;
		report.handedBack += captureStats.handedBack;
		report.holes += frameHoles;
		report.framesWithHoles += frameHoles != 0;
		if (report.frames == 300) {
			std::string reasons;
			for (std::size_t r = 0; r < report.byReason.size(); ++r)
				if (report.byReason[r])
					reasons += fmt::format(" {}={}", kIneligibleNames[r], report.byReason[r]);
			logger::info("[DCLF] holes over {} frames: {} in {} frames ({} of them in the tables); by reason:{}; {} withheld passes handed back to the native loop",
				report.frames, report.holes, report.framesWithHoles, report.inTables, reasons.empty() ? " -" : reasons, report.handedBack);
			report = {};
		}

		auto claims = std::make_shared<PassCapture::ClaimSet>();
		claims->reserve(impl->drawnFrame.size());
		for (const auto& [geometry, drawn] : impl->drawnFrame) {
			// The same one-frame tolerance the native skip used: an object drawn last frame is still ours.
			if (frame - drawn <= 1)
				claims->insert(geometry);
		}
		// Churn against the set that was in force this frame.
		captureStats.claimsAdded = captureStats.claimsDropped = captureStats.droppedAfterCull = 0;
		if (const auto previous = capture.CurrentClaims()) {
			for (const auto* geometry : *previous) {
				if (!claims->contains(geometry)) {
					++captureStats.claimsDropped;
					// Still registered by the engine, so the engine will draw it from now on: this is the
					// signature of culling undoing itself.
					if (store.FindAccumulatedPass(geometry))
						++captureStats.droppedAfterCull;
				}
			}
			for (const auto* geometry : *claims) {
				if (!previous->contains(geometry))
					++captureStats.claimsAdded;
			}
		}
		capture.PublishClaims(std::move(claims));
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
		const bool skip = Toggles::Get().Active().noZPrepass;
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
		RenderGraphRuntime::EpochBodyScope body(a_segment);
		ScopedPerfEvent event(a_segment == RenderGraphRuntime::Segment::ZPrepass ? "CS DCLF: Z-prepass (CPU)" : "CS DCLF: main opaque (CPU)");
		const auto start = std::chrono::steady_clock::now();
		const bool depthOnly = a_segment == RenderGraphRuntime::Segment::ZPrepass;
		const std::size_t jobIndex = depthOnly ? kAsyncZPrepass : kAsyncColour;
		if (!impl->pending) {
			impl->DropMainJob(jobIndex, stats);
			return;
		}
		auto capture = std::move(*impl->pending);
		impl->pending.reset();
		auto& pipelines = DrawPipelines::Get();
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (failed || !lighting || !pipelines.Enabled() || !GetIndirectState().valid) {
			capture.Release();
			impl->DropMainJob(jobIndex, stats);
			return;
		}
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
			impl->DropMainJob(jobIndex, stats);
			return;
		}

		auto& store = SceneStore::Get();
		const auto& tables = store.GetTables();
		auto resources = impl->resources;
		stats.skipped = {};
		stats.missingTextures = {};
		stats.missingVertexConstants = stats.missingPixelConstants = 0;

		// The per-frame constant blocks, from the capture's mirrors: render thread, before the epoch. Their
		// slot mask is an input of the build; their bytes are uploaded by the commit.
		FrameBlocks blocks;
		impl->PackFrameBlocks(capture, depthOnly, *resources, blocks);
		MainInputs in = impl->PrepareMainInputs(&capture, depthOnly, *resources, blocks.vsMask, blocks.psMask, store);
		auto& job = impl->mainJobs[jobIndex];
		job.vsMask = blocks.vsMask;
		job.psMask = blocks.psMask;
		job.masksKnown = true;
		auto& payload = impl->mainPayload[jobIndex];
		auto& async = stats.async[jobIndex];
		bool builtOnWorker = false;

		const bool ok = RenderGraphRuntime::Get().ExecuteEpoch(a_segment, [&](org::RenderGraph&) {
			// The descriptor entries the build reads, resolved now that the descriptor service is active.
			if (in.resolveTextures)
				GpuTextures::Get().BeginFrame(in.frameNumber);

			// The worker's build, if one was kicked for this epoch and it was built for exactly these inputs;
			// otherwise the build runs here. A late or stale job is dropped (its payload is the one this
			// build overwrites, so a late job is waited for before the inline build; counted). Joined before
			// the lookup refresh below: the worker reads the lookups until it is done.
			const auto joinStart = std::chrono::steady_clock::now();
			auto& worker = AsyncWorker::Get();
			AsyncWorker::WaitResult joined = AsyncWorker::WaitResult::None;
			if (job.handle) {
				joined = worker.Wait(job.handle, AsyncWaitBudget());
				if (joined == AsyncWorker::WaitResult::Late)
					worker.Cancel(job.handle);
			}
			const auto lookupsStart = std::chrono::steady_clock::now();
			stats.commitUs[0] += std::chrono::duration<double, std::micro>(lookupsStart - joinStart).count();
			auto& lookups = store.MutableLookups();
			RefreshMaterialLookups(tables, in.frameNumber, store.GetProjectedTextures(), lookups);
			in.lookupGeneration = lookups.generation;
			stats.commitUs[1] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - lookupsStart).count();

			bool useAsync = false;
			if (job.handle) {
				switch (joined) {
				case AsyncWorker::WaitResult::Done:
					// A refresh that added or changed an entry the build read bumps the generation: stale.
					if (SameInputs(job.inputs, in)) {
						useAsync = true;
					} else {
						++async.stale;
						impl->LogStaleMainJob(jobIndex, in);
					}
					break;
				case AsyncWorker::WaitResult::Late:
					++async.late;
					break;
				case AsyncWorker::WaitResult::Failed:
					++async.failed;
					break;
				default:
					++async.cancelled;
					break;
				}
				// The Z-prepass job's eye is a prediction: counted apart from the other reasons for staleness.
				if (depthOnly && joined == AsyncWorker::WaitResult::Done) {
					const bool eyeDiffers = std::memcmp(&job.inputs.eye, &in.eye, sizeof(RE::NiPoint3)) != 0;
					const bool previousDiffers = std::memcmp(&job.inputs.previousEye, &in.previousEye, sizeof(RE::NiPoint3)) != 0;
					if (eyeDiffers || previousDiffers)
						++async.eyeMismatches;
					if (!eyeDiffers && previousDiffers)
						++async.previousEyeMismatches;
				}
				job.handle = {};
			}
			builtOnWorker = useAsync;
			if (useAsync) {
				++async.used;
				if (AsyncModeSetting() == AsyncMode::Probe) {
					// The same build on this thread, against the same inputs, compared byte for byte.
					BuildMainPayload(job.inputs, tables, lookups, impl->probePayload);
					++async.probeCompared;
					std::string difference;
					if (!SamePayload(payload, impl->probePayload, difference)) {
						if (async.probeDiffer++ == 0)
							logger::warn("[DCLF] async {} probe: the worker's build differs from the inline one: {}", depthOnly ? "zprepass" : "colour", difference);
					}
				}
			} else {
				++async.builtInline;
				BuildMainPayload(in, tables, lookups, payload, impl->CacheFor(jobIndex));
			}
			impl->CommitMainPayload(capture, blocks, payload, resources, store, stats);
		});
		capture.Release();
		++stats.epochs;
		if (ok && BuildParityEnabled())
			impl->CheckBuildParity(resources, payload, stats);
		// Only the colour epoch's counters. Both epochs run BuildDraws into the same count buffer, and the
		// Z-prepass builds from the far smaller set the colour epoch drew last frame, so sampling whichever
		// ran most recently alternates between two unrelated populations.
		if (ok && !depthOnly)
			impl->ReadCullCounters(resources, stats);
		if (ok && !depthOnly && SetParityEnabled())
			impl->CheckSetParity(resources, impl->mainPayload[1], payload);

		stats.cpuMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
		// The remainder is the epoch execution itself and the commit. It stays a subtraction, but it is now
		// a small one rather than the bucket the per-draw work hid in. When the worker built the payload, the
		// build's parts are the worker's time and the render thread's is all remainder (the join included).
		stats.partMs[7] = builtOnWorker ? stats.cpuMs :
		                                  stats.cpuMs - (stats.partMs[0] + stats.partMs[1] + stats.partMs[2] +
		                                                    stats.partMs[3] + stats.partMs[4] + stats.partMs[5] + stats.partMs[6]);
		if (!ok)
			logger::error("[DCLF] The main-pass epoch failed; the render graph is disabled");
	}

	void IndirectDraws::KickColourBuild()
	{
		// The colour epoch's inputs are final from here (RefreshFrameConstants was the frame's last writer of
		// the tables), and the epoch itself is ~1.5-2 ms of native rendering away: the build runs on the
		// worker in between, so the render thread only commits when the epoch comes. Only the hybrid path
		// replays the Z-prepass's eye and constants; without the replay the eye comes from a capture that
		// does not exist yet.
		impl->DropMainJob(kAsyncColour, stats);
		if (!AsyncJobEnabled("colour"))
			return;
		if (!impl->resources || !impl->resources->hybrid || !impl->prepassInputs) {
			++stats.async[kAsyncColour].notKicked;
			return;
		}
		impl->KickMainJob(false, nullptr, nullptr, stats);
	}

	void IndirectDraws::KickZPrepassBuild()
	{
		// The Z-prepass epoch's inputs are final from here to the end of Main_RenderDepth - the accumulate
		// phase was the tables' last writer, RefreshFrameConstants runs after the epoch - except the eye,
		// which the epoch captures from posAdjust. posAdjust still holds the shadow cameras' here, so the eye
		// is predicted: the main camera's world position, and last frame's captured eye as the previous one.
		// The epoch compares both exactly with its capture.
		impl->DropMainJob(kAsyncZPrepass, stats);
		if (!AsyncJobEnabled("zprepass"))
			return;
		auto* camera = RE::Main::WorldRootCamera();
		if (!impl->resources || !impl->resources->hybrid || !impl->prepassInputs || !camera) {
			++stats.async[kAsyncZPrepass].notKicked;
			return;
		}
		const RE::NiPoint3 eye = camera->world.translate;
		const RE::NiPoint3 previousEye = impl->prepassEye;
		impl->KickMainJob(true, &eye, &previousEye, stats);
	}

	void IndirectDraws::Impl::KickMainJob(bool a_depthOnly, const RE::NiPoint3* a_eye, const RE::NiPoint3* a_previousEye, IndirectDraws::Stats& a_stats)
	{
		// Only the bindless records are per pair, which is what keeps the build a function of the tables.
		const std::size_t index = a_depthOnly ? kAsyncZPrepass : kAsyncColour;
		auto& job = mainJobs[index];
		auto& async = a_stats.async[index];
		auto* lighting = ConstantEvaluator::Get().GetLightingShader();
		if (!lighting || !DrawPipelines::Get().Enabled() || !GetIndirectState().valid || !job.masksKnown || !BindlessObjects() || !BindlessDraws()) {
			++async.notKicked;
			return;
		}
		auto& store = SceneStore::Get();
		job.inputs = PrepareMainInputs(nullptr, a_depthOnly, *resources, job.vsMask, job.psMask, store);
		// A bindless build without parity checks does not read the eye (PrepareMainInputs leaves it zero), so
		// the prediction only matters where it does.
		if (a_eye && BuildReadsEye(job.inputs))
			job.inputs.eye = *a_eye;
		if (a_previousEye && BuildReadsEye(job.inputs))
			job.inputs.previousEye = *a_previousEye;
		++async.kicked;
		const auto* tables = &store.GetTables();
		const auto* lookups = &store.GetLookups();
		auto* payload = &mainPayload[index];
		auto* cache = CacheFor(index);
		auto* pool = &stagedPools[index];
		const MainInputs inputs = job.inputs;
		job.handle = AsyncWorker::Get().Submit(a_depthOnly ? "zprepass" : "colour", [inputs, tables, lookups, payload, cache, pool, target = resources](std::stop_token) {
			BuildMainPayload(inputs, *tables, *lookups, *payload, cache);
			if (target)
				StageMainPayload(*payload, *target, *pool);
		});
	}

	void IndirectDraws::EndFrame()
	{
		// A job kicked this frame and never joined (the epoch did not run: a load screen, a failed setup) must
		// not outlive the frame: its inputs name this frame's tables.
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j) {
			if (impl->mainJobs[j].handle) {
				++stats.async[j].leaked;
				impl->DropMainJob(j, stats);
			}
		}
		if (impl->shadowJob.handle) {
			++stats.async[kAsyncShadow].leaked;
			impl->DropShadowJob(stats);
		}
	}

	void IndirectDraws::DrainAsync()
	{
		for (std::size_t j = 0; j < impl->mainJobs.size(); ++j)
			impl->DropMainJob(j, stats);
		impl->DropShadowJob(stats);
		AsyncWorker::Get().Drain();
	}

	void IndirectDraws::Impl::DropMainJob(std::size_t a_job, IndirectDraws::Stats& a_stats)
	{
		auto& job = mainJobs[a_job];
		if (!job.handle)
			return;
		AsyncWorker::Get().Cancel(job.handle);
		++a_stats.async[a_job].dropped;
		job.handle = {};
	}

	void IndirectDraws::Impl::LogStaleMainJob(std::size_t a_job, const MainInputs& a_actual)
	{
		auto& job = mainJobs[a_job];
		if (job.loggedStale++ >= 4)
			return;
		const auto& k = job.inputs;
		logger::info("[DCLF] async {}: the job's inputs are stale (frame {} vs {}, VS mask {:#x} vs {:#x}, PS mask {:#x} vs {:#x}, eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), previous eye ({:.3f} {:.3f} {:.3f}) vs ({:.3f} {:.3f} {:.3f}), render flags {:#x} vs {:#x}, drew last frame {}, tables {} vs {}, lookups {} vs {}, resources {})",
			a_job == kAsyncZPrepass ? "zprepass" : "colour",
			k.frameNumber, a_actual.frameNumber, k.vsFrameMask, a_actual.vsFrameMask, k.psFrameMask, a_actual.psFrameMask,
			k.eye.x, k.eye.y, k.eye.z, a_actual.eye.x, a_actual.eye.y, a_actual.eye.z,
			k.previousEye.x, k.previousEye.y, k.previousEye.z, a_actual.previousEye.x, a_actual.previousEye.y, a_actual.previousEye.z,
			k.renderFlags, a_actual.renderFlags, k.drewLastFrame == a_actual.drewLastFrame ? "same" : "differs",
			k.tablesGeneration, a_actual.tablesGeneration, k.lookupGeneration, a_actual.lookupGeneration,
			k.addresses == a_actual.addresses ? "same" : "changed");
	}

	void IndirectDraws::Impl::PackFrameBlocks(const Capture& a_capture, bool a_depthOnly, const Resources& a_resources, FrameBlocks& a_out)
	{
		// Per-frame constant buffers: whatever the main pass binds outside the per-draw slots.
		// On the Z-prepass the pixel-stage per-frame bindings are skipped entirely: the native depth pass
		// has not bound the main pass's yet, and the DCLF_DEPTH_ONLY build of the pixel stage compiles
		// away everything that would read them.
		auto& mirror = ConstantMirror::Get();
		// On the hybrid path the colour epoch replays the vertex-stage bytes the Z-prepass used, so the
		// two agree to the bit and the colour pass's EQUAL test passes.
		const bool replayVertexInputs = !a_depthOnly && a_resources.hybrid && prepassInputs;
		if (a_depthOnly)
			for (auto& bytes : prepassVS)
				bytes.clear();
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (replayVertexInputs && !((kPerDrawVS >> slot) & 1)) {
				const auto& bytes = prepassVS[slot];
				if (!bytes.empty())
					a_out.vs[slot].assign(bytes.begin(), bytes.end());
			}
			for (auto [buffers, perDraw, out] : { std::tuple{ &a_capture.vsBuffers, kPerDrawVS, &a_out.vs }, std::tuple{ &a_capture.psBuffers, kPerDrawPS, &a_out.ps } }) {
				if (a_depthOnly && out == &a_out.ps)
					continue;
				if (replayVertexInputs && out == &a_out.vs)
					continue;
				auto* buffer = (*buffers)[slot];
				if (!buffer || ((perDraw >> slot) & 1))
					continue;
				mirror.Watch(buffer);
				const auto contents = mirror.Contents(buffer);
				if (!contents.empty())
					(*out)[slot].assign(contents.begin(), contents.end());
				if (a_depthOnly && out == &a_out.vs && !contents.empty())
					prepassVS[slot].assign(contents.begin(), contents.end());
			}
		}

		// b12 is the game's PerFrame buffer. The constant mirror does not always hold its contents - the
		// engine does not rewrite it through the hooked context every frame - and an empty slot drops
		// every object whose shaders read it, which left distant buildings unshaded. Community Shaders
		// already caches this buffer between Map and Unmap for its own use, so take it from there.
		{
			const auto& cached = globals::game::frameBufferCached.data;
			const auto* bytes = reinterpret_cast<const std::byte*>(&cached);
			if (a_out.vs[kPerFrameVertexRegister].empty())
				a_out.vs[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
			if (a_out.ps[kPerFrameVertexRegister].empty())
				a_out.ps[kPerFrameVertexRegister].assign(bytes, bytes + sizeof(cached));
		}

		// b5 is Community Shaders' own SharedData, written through its ConstantBuffer helper rather than
		// through the hooked device context, so the constant mirror never observes the write and the
		// slot would stay empty - which dropped every object whose shader reads it (the untextured
		// architecture in Dragonsreach). CS keeps the struct it uploaded, so pack that directly.
		// These are supplied in the Z-prepass too. Its pixel stage is the DCLF_DEPTH_ONLY build,
		// which still reads them in the code that runs before the alpha test, and they all come from
		// Community Shaders or the game's own cache rather than from the main pass's bindings - so they
		// are available during the native depth pass, where the main pass's bindings are not.
		if (auto* state = globals::state) {
			const auto* shared = reinterpret_cast<const std::byte*>(&state->lastSharedData);
			a_out.ps[kSharedDataRegister].assign(shared, shared + sizeof(State::SharedDataCB));
			if (!state->lastFeatureData.empty()) {
				const auto* feature = reinterpret_cast<const std::byte*>(state->lastFeatureData.data());
				a_out.ps[kFeatureDataRegister].assign(feature, feature + state->lastFeatureData.size());
			}
		}
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_out.vs[slot].empty())
				a_out.vsMask |= 1u << slot;
			if (!a_out.ps[slot].empty())
				a_out.psMask |= 1u << slot;
		}

		// One line saying, per pixel register, whether the pass had a buffer bound and whether the
		// constant mirror could supply its contents: a slot that is bound but not mirrored ends up as a
		// null address and drops every object whose shader reads it.
		if (!a_depthOnly && !loggedFrameConstants) {
			loggedFrameConstants = true;
			std::string text;
			for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
				if ((kPerDrawPS >> slot) & 1)
					continue;
				text += fmt::format("{}b{}={}/{}", text.empty() ? "" : " ", slot, a_capture.psBuffers[slot] ? "bound" : "unbound",
					((a_out.psMask >> slot) & 1) ? "supplied" : "empty");
			}
			logger::info("[DCLF] main-pass per-frame pixel constants: {}", text);
		}
	}

	MainInputs IndirectDraws::Impl::PrepareMainInputs(const Capture* a_capture, bool a_depthOnly, const Resources& a_resources, std::uint32_t a_vsMask, std::uint32_t a_psMask, const SceneStore& a_store)
	{
		// CS_DCLF_NO_PREPASS_TEXTURES=1: the Z-prepass epoch does not touch the texture system at all, to
		// tell apart a colour epoch whose descriptors are its own from one that inherits slots the depth
		// epoch allocated earlier in the same frame.
		static const bool bareDepthTextures = SwitchEnabled("CS_DCLF_NO_PREPASS_TEXTURES");
		static const bool dedupParity = SwitchEnabled("CS_DCLF_DEDUP_PARITY");
		static const bool bindlessParity = SwitchEnabled("CS_DCLF_BINDLESS_PARITY");
		const auto& tables = a_store.GetTables();
		MainInputs in;
		in.frameNumber = a_store.GetFrame();
		in.depthOnly = a_depthOnly;
		in.hybrid = a_resources.hybrid;
		in.resolveTextures = !a_depthOnly || !bareDepthTextures;
		in.bindless = BindlessObjects();
		in.bindlessDraws = BindlessDraws();
		in.dedupParity = dedupParity;
		in.bindlessParity = bindlessParity;
		in.withholding = PassCapture::WithholdingEnabled();
		in.requireNativeVisible = RequireNativeVisible();
		in.linearLighting = globals::features::linearLighting.loaded && globals::features::linearLighting.settings.enableLinearLighting;
		in.renderFlags = a_store.GetMainPassRenderFlags();
		// Camera-relative world matrices: the colour epoch must use the eye the Z-prepass used, or the
		// same vertex lands somewhere else and the EQUAL test rejects it. Without a capture (the job kicked
		// ahead of the epoch) the replay is the only source, which KickColourBuild requires.
		const bool replayVertexInputs = !a_depthOnly && a_resources.hybrid && prepassInputs;
		if (replayVertexInputs || !a_capture) {
			in.eye = prepassEye;
			in.previousEye = prepassPreviousEye;
		} else {
			in.eye = a_capture->eye;
			in.previousEye = a_capture->previousEye;
		}
		// The records are absolute and the shaders subtract the eye, so a bindless build reads it only for its
		// parity checks against the constant-group form, which is relative. Where it is not read it is not an
		// input: the Z-prepass job needs no eye prediction, and nothing goes stale on it.
		if (!BuildReadsEye(in))
			in.eye = in.previousEye = {};
		in.vsFrameMask = a_vsMask;
		in.psFrameMask = a_psMask;
		in.addresses.constants = a_resources.constantsAddress;
		in.addresses.records = a_resources.recordsAddress;
		in.addresses.frameConstants = a_resources.frameConstantsAddress;
		in.addresses.objectsIndex = a_resources.objectsIndex;
		in.addresses.bonesIndex = a_resources.bonesIndex;
		in.addresses.recordCapacity = a_resources.recordCapacity;
		in.addresses.identity = &a_resources;
		in.tablesGeneration = a_store.GetTablesGeneration();
		in.lookupGeneration = a_store.GetLookups().generation;
		in.materialPatchedFloats = a_store.GetMaterialPatchedFloats();
		// The Z-prepass's gate without withholding: what the colour epoch drew last frame, per object. The
		// map stays the render thread's; the build reads this snapshot.
		if (a_depthOnly && a_resources.hybrid && !in.withholding) {
			in.drewLastFrame.assign(tables.objects.size(), 0);
			for (std::size_t o = 0; o < tables.objects.size() && o < tables.objectGeometry.size(); ++o) {
				const auto drawn = drawnFrame.find(tables.objectGeometry[o]);
				in.drewLastFrame[o] = drawn != drawnFrame.end() && in.frameNumber - drawn->second <= 1;
			}
		}
		return in;
	}

	std::string IndirectDraws::AsyncReport()
	{
		std::string text = AsyncWorker::Get().Report();
		for (std::size_t i = 0; i < impl->buildCaches.size(); ++i) {
			auto& cache = impl->buildCaches[i];
			if (!(cache.pairHits + cache.pairMisses + cache.pipelineHits + cache.pipelineMisses))
				continue;
			text += fmt::format("[DCLF] build cache ({}): pairs {} reused, {} rebuilt; pipelines {} reused, {} rebuilt; {} pairs and {} pipelines held\n",
				i == kAsyncZPrepass ? "zprepass" : "colour", cache.pairHits, cache.pairMisses, cache.pipelineHits, cache.pipelineMisses, cache.pairs.size(),
				cache.pipelines.size());
			cache.pairHits = cache.pairMisses = cache.pipelineHits = cache.pipelineMisses = 0;
		}
		static constexpr const char* kNames[3] = { "colour", "zprepass", "shadow" };
		for (std::size_t i = 0; i < stats.async.size(); ++i) {
			auto& a = stats.async[i];
			if (!a.kicked && !a.notKicked && !a.builtInline && !a.leaked)
				continue;
			text += fmt::format("[DCLF] async {} epochs: {} used the worker's build, {} built inline ({} not kicked, {} stale, {} late, {} failed, {} cancelled), {} dropped, {} leaked; probe: {} compared, {} differ\n",
				kNames[i], a.used, a.builtInline, a.notKicked, a.stale, a.late, a.failed, a.cancelled, a.dropped, a.leaked, a.probeCompared, a.probeDiffer);
			if (i == kAsyncZPrepass && a.kicked)
				text += fmt::format("[DCLF] async zprepass eye: the predicted eye pair missed the captured one {} times ({} the previous eye only)\n", a.eyeMismatches, a.previousEyeMismatches);
			a = {};
		}
		AsyncWorker::Get().ResetStats();
		return text;
	}

	namespace
	{
		// The payload's own uploads: the constant arena, the per-object records and bone rows, the binding records,
		// and the draw inputs with their geometry. Three buffers, three conditions for the last ones: they used to
		// share one, and a depth epoch where every candidate is cull-only has no records and plenty of inputs, so
		// BuildDraws dispatched inputCount work items over whatever the previous epoch left in the buffer.
		template <class Emit>
		void ForEachMainPayloadUpload(const MainPayload& a_payload, const Resources& a_resources, Emit&& a_emit)
		{
			const auto& bytes = a_payload.arena.Bytes();
			if (!bytes.empty())
				a_emit(a_resources.constants, bytes.data(), bytes.size(), false);
			if (!a_payload.objectRecords.empty())
				a_emit(a_resources.objects, a_payload.objectRecords.data(), a_payload.objectRecords.size() * sizeof(BindlessObject), false);
			if (!a_payload.boneRows.empty() && a_resources.bones)
				a_emit(a_resources.bones, a_payload.boneRows.data(), std::min<std::size_t>(a_payload.boneRows.size() * sizeof(float), std::size_t(kMaxBoneRows) * 16), false);
			if (!a_payload.records.empty())
				a_emit(a_resources.records, a_payload.records.data(), a_payload.records.size() * sizeof(DrawBindings), true);
			if (!a_payload.inputList.empty()) {
				a_emit(a_resources.inputs, a_payload.inputList.data(), a_payload.inputList.size() * sizeof(DrawInput), false);
				a_emit(a_resources.geometries, a_payload.geometryDraws.data(), a_payload.geometryDraws.size() * sizeof(GeometryDraw), false);
			}
		}

		// Render thread: the payload through the commit's uploads, copied now.
		void UploadMainPayload(const MainPayload& a_payload, const Resources& a_resources, CommitUploads& a_uploads)
		{
			ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool) {
				a_uploads(a_target, a_data, a_bytes, 0);
			});
		}
	}

	// On the worker, after the build: the payload's uploads into a staged batch (a released one from the job's
	// pool, or a new one), so the commit copies nothing. The records' staging is kept for the commit's patches.
	namespace
	{
		// A released batch from a job's pool (neither a payload nor the upload service still holds it), or a new one.
		std::shared_ptr<org::runtime::StagedUploadBatch> AcquireStagedBatch(std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
		{
			std::shared_ptr<org::runtime::StagedUploadBatch> batch;
			for (const auto& candidate : a_pool) {
				if (candidate.use_count() == 1) {
					batch = candidate;
					break;
				}
			}
			if (!batch) {
				batch = org::runtime::StagedUploadBatch::Create();
				a_pool.push_back(batch);
			}
			batch->Reset();
			return batch;
		}
	}

	void StageMainPayload(MainPayload& a_payload, const Resources& a_resources, std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		auto batch = AcquireStagedBatch(a_pool);
		ForEachMainPayloadUpload(a_payload, a_resources, [&](const auto& a_target, const void* a_data, std::size_t a_bytes, bool a_records) {
			auto* staging = batch->Stage(org::runtime::UploadTarget::FromShared(a_target), 0, a_data, a_bytes);
			if (a_records)
				a_payload.stagedRecords = reinterpret_cast<DrawBindings*>(staging);
		});
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	// On the worker, after the shadow build: what the commit would upload that does not depend on the views it
	// captures - the shared tables, the used modes' inputs, the arena past its view head - and, per view slot the
	// job expects, the records naming that slot's blocks and its zeroed counters. The commit uploads the view
	// head and any slot past a_slots itself.
	void StageShadowPayload(ShadowPayload& a_payload, const ShadowResources& a_resources, std::uint32_t a_slots,
		std::vector<std::shared_ptr<org::runtime::StagedUploadBatch>>& a_pool)
	{
		using org::runtime::UploadTarget;
		auto batch = AcquireStagedBatch(a_pool);
		if (!a_payload.objects.empty())
			batch->Stage(UploadTarget::FromShared(a_resources.objects), 0, a_payload.objects.data(), a_payload.objects.size() * sizeof(BindlessObject));
		if (!a_payload.boneRows.empty())
			batch->Stage(UploadTarget::FromShared(a_resources.bones), 0, a_payload.boneRows.data(),
				std::min<std::size_t>(a_payload.boneRows.size() * sizeof(float), std::size_t(kMaxBoneRows) * 16));
		if (!a_payload.geometries.empty())
			batch->Stage(UploadTarget::FromShared(a_resources.geometries), 0, a_payload.geometries.data(),
				std::min<std::size_t>(a_payload.geometries.size(), kMaxGeometries) * sizeof(GeometryDraw));
		for (std::uint32_t m = 0; m < kShadowModeCount; ++m) {
			const auto& inputs = a_payload.inputList[m];
			if (a_payload.inputs.modeUsed[m] && !inputs.empty())
				batch->Stage(UploadTarget::FromShared(a_resources.inputs[m]), 0, inputs.data(), inputs.size() * sizeof(DrawInput));
		}
		const auto& bytes = a_payload.arena.Bytes();
		if (bytes.size() > kShadowMaterialBlocksOffset)
			batch->Stage(UploadTarget::FromShared(a_resources.constants), kShadowMaterialBlocksOffset, bytes.data() + kShadowMaterialBlocksOffset,
				bytes.size() - kShadowMaterialBlocksOffset);
		const std::uint32_t slots = std::min<std::uint32_t>(a_slots, kMaxShadowViews);
		const std::uint64_t base = a_payload.inputs.addresses.constants;
		static const std::uint32_t zero[kCountWords] = {};
		for (std::uint32_t slot = 0; slot < slots; ++slot) {
			const std::uint64_t viewBlockOffset = std::uint64_t(slot) * kShadowViewSlotBytes;
			const std::uint64_t perFrameOffset = viewBlockOffset + kShadowPerFrameOffset;
			const std::uint64_t recordsOffset = std::uint64_t(slot) * kShadowRecordCapacity * sizeof(DrawBindings);
			if (auto* staging = batch->Stage(UploadTarget::FromShared(a_resources.records), recordsOffset, a_payload.records.size() * sizeof(DrawBindings))) {
				// Whole records into write-combined staging, in order: written, never read.
				for (std::size_t r = 0; r < a_payload.records.size(); ++r) {
					DrawBindings record = a_payload.records[r];
					record.vertexConstants[0] = base + viewBlockOffset;
					record.pixelConstants[0] = base + viewBlockOffset;
					record.vertexConstants[kPerFrameVertexRegister] = base + perFrameOffset;
					record.pixelConstants[kPerFrameVertexRegister] = base + perFrameOffset;
					std::memcpy(staging + r * sizeof(DrawBindings), &record, sizeof(DrawBindings));
				}
			}
			batch->Stage(UploadTarget::FromShared(a_resources.count[slot]), 0, zero, sizeof(zero));
		}
		a_payload.stagedSlots = slots;
		a_payload.stagedFor = &a_resources;
		a_payload.staged = std::move(batch);
	}

	bool IndirectDraws::Impl::CommitMainPayload(const Capture& a_capture, const FrameBlocks& a_blocks, MainPayload& a_payload,
		const std::shared_ptr<Resources>& a_resources, SceneStore& a_store, IndirectDraws::Stats& a_stats)
	{
		const auto& in = a_payload.inputs;
		const auto& tables = a_store.GetTables();
		const bool depthOnly = in.depthOnly;
		const std::uint32_t frameNumber = in.frameNumber;
		auto& textures = GpuTextures::Get();
		auto& mirror = ConstantMirror::Get();
		const bool replayVertexInputs = !depthOnly && a_resources->hybrid && prepassInputs;
		CommitUploads uploads(commitStagedPool);
		auto lap = [&, last = std::chrono::steady_clock::now()](std::size_t a_part) mutable {
			const auto now = std::chrono::steady_clock::now();
			a_stats.commitUs[a_part] += std::chrono::duration<double, std::micro>(now - last).count();
			last = now;
		};
		// Nothing to draw until this commit publishes the segment's shape again (a failed commit leaves it so).
		const std::size_t shapeIndex = depthOnly ? kDepthShape : kColourShape;
		a_resources->frames[shapeIndex].store(nullptr, std::memory_order_release);

		// The frame's textures (t16 and up), resolved now and patched into the records that read them.
		std::array<std::uint32_t, kTextureRegisters> frameTextures;
		frameTextures.fill(kInvalidIndex);
		for (std::uint32_t t = kPixelTextureSlots; t < kTextureRegisters && !depthOnly; ++t)
			frameTextures[t] = textures.Resolve(a_capture.psViews[t]);
		if (a_resources->lightLimitFix && !depthOnly)
			ORGLightCulling::Get().GetShaderResourceIndices(frameTextures[kLightsRegister], frameTextures[kLightsRegister + 1], frameTextures[kLightsRegister + 2]);
		for (const auto& frameBuffer : depthOnly ? decltype(a_resources->frameBuffers){} : a_resources->frameBuffers) {
			auto* buffer = BufferOf(a_capture.psViews[frameBuffer.textureRegister]);
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			const std::size_t offset = std::size_t(frameBuffer.firstElement) * frameBuffer.stride;
			const std::size_t bytes = std::size_t(frameBuffer.elements) * frameBuffer.stride;
			if (contents.size() < offset + bytes)
				continue;  // not written since it is watched
			uploads(frameBuffer.copy, contents.data() + offset, bytes, 0);
			frameTextures[frameBuffer.textureRegister] = frameBuffer.copy->GetSRVInfo(0).slot.index;
		}
		std::uint32_t frameTexturesMissing = 0;
		for (const auto& [record, t] : a_payload.framePatches) {
			const std::uint32_t index = frameTextures[t];
			// A frame texture the pipeline reads but the pass did not bind reads zero, as an unbound view does
			// natively; counted, because the build could not skip the draw for it.
			if (index == kInvalidIndex)
				++frameTexturesMissing;
			const std::uint32_t value = index == kInvalidIndex ? (in.resolveTextures && textures.NullIndex() != kInvalidIndex ? textures.NullIndex() : 0u) : index;
			a_payload.records[record].textures[t] = value;
			if (a_payload.stagedRecords)
				a_payload.stagedRecords[record].textures[t] = value;  // write-combined: written, never read
		}
		a_stats.frameTexturesMissing = frameTexturesMissing;
		lap(2);

		// The frame slots: each block into its slot, and the zeroed light block every bindless draw's b3 reads.
		for (std::uint32_t slot = 0; slot < kConstantBufferRegisters; ++slot) {
			if (!a_blocks.vs[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.vs[slot].data(), a_blocks.vs[slot].size(), FrameSlotOffset(false, slot));
			if (!a_blocks.ps[slot].empty())
				uploads(a_resources->frameConstants, a_blocks.ps[slot].data(), a_blocks.ps[slot].size(), FrameSlotOffset(true, slot));
		}
		if (in.bindlessDraws) {
			static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
			uploads(a_resources->frameConstants, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		}

		lap(3);
		// Upload (the graph's upload pass runs ahead of every pass of this epoch). The worker's build staged its
		// payload itself: one submission, no copies here. A build made here, or staged against resources since
		// recreated, is uploaded from its vectors.
		const auto& bytes = a_payload.arena.Bytes();
		const bool staged = a_payload.staged && a_payload.stagedFor == a_resources.get();
		if (staged) {
			org::runtime::GetActiveUploadService()->SubmitStagedUploads(std::move(a_payload.staged));
			a_payload.stagedRecords = nullptr;
		} else if (!bytes.empty()) {
			uploads(a_resources->constants, bytes.data(), bytes.size(), 0);
		}
		// The depth segment clears every counter; the colour segment clears only the word its own draws
		// append through. On the hybrid path the culling happens in the depth segment, so clearing the
		// whole buffer again here would erase the phase 1 and phase 2 numbers before anything read them
		// - they are written earlier in the same frame.
		static const std::uint32_t zero[kCountWords] = {};
		const std::size_t zeroBytes = (depthOnly || !a_resources->hybrid) ? sizeof(zero) : sizeof(std::uint32_t);
		uploads(a_resources->count, zero, zeroBytes, 0);
		// The decal words: each group's slot count for its draw, and the tallies zeroed. Written by the
		// colour segment only, which is the one that submits decals.
		if (!depthOnly) {
			decalWords = { a_payload.decalCount[0], a_payload.decalCount[1], 0u, 0u };
			uploads(a_resources->count, decalWords.data(), decalWords.size() * sizeof(std::uint32_t), kCountDecalGroupWord * sizeof(std::uint32_t));
		}
		if (!staged)
			UploadMainPayload(a_payload, *a_resources, uploads);
		a_stats.boneRows = static_cast<std::uint32_t>(a_payload.boneRows.size() / 4);

		lap(4);
		// The build's stats.
		a_stats.skipped = a_payload.skipped;
		a_stats.missingTextures = a_payload.missingTextures;
		a_stats.missingVertexConstants = a_payload.missingVertexConstants;
		a_stats.missingPixelConstants = a_payload.missingPixelConstants;
		a_stats.deferredTextures = a_payload.deferredTextures;
		a_stats.bindlessParityChecks += a_payload.bindlessParityChecks;
		a_stats.bindlessParityMismatches += a_payload.bindlessParityMismatches;
		a_stats.recordParityChecks += a_payload.recordParityChecks;
		a_stats.recordParityMismatches += a_payload.recordParityMismatches;
		for (std::size_t i = 0; i < a_payload.partMs.size(); ++i)
			a_stats.partMs[i] = a_payload.partMs[i];
		if (!depthOnly)
			a_stats.decalsDrawn = a_payload.decalsDrawn;
		if (a_payload.shortBuffers) {
			const bool first = a_stats.shortBuffers == 0;
			a_stats.shortBuffers += a_payload.shortBuffers;
			const auto& shortBuffer = a_payload.shortBuffer;
			if (first && shortBuffer.object < tables.objects.size()) {
				const auto& geometry = tables.geometries[tables.objects[shortBuffer.object].geometryIndex];
				logger::warn("[DCLF] '{}' draws past its buffers: {} vertices x {} bytes needs {}, the slice holds {}; indices to {} need {}, the slice holds {}",
					tables.objectGeometry[shortBuffer.object] ? tables.objectGeometry[shortBuffer.object]->name.c_str() : "?", geometry.vertexCount, geometry.vertexStride,
					shortBuffer.vertexNeeded, geometry.vertexBytes, geometry.firstIndex + geometry.indexCount, shortBuffer.indexNeeded, geometry.indexBytes);
			}
		}
		a_stats.uploadBytes = bytes.size() + a_payload.records.size() * sizeof(DrawBindings) + a_payload.inputList.size() * sizeof(DrawInput) +
		                      a_payload.geometryDraws.size() * sizeof(GeometryDraw) + a_payload.objectRecords.size() * sizeof(BindlessObject);
		a_stats.records = static_cast<std::uint32_t>(a_payload.records.size());
		lap(6);
		// What the colour epoch drew: the native loop's skip set, kept by the render thread.
		for (const auto o : a_payload.drawn) {
			if (o < tables.objectGeometry.size())
				drawnFrame[tables.objectGeometry[o]] = frameNumber;
		}
		lap(5);

		const std::uint32_t drawCount = static_cast<std::uint32_t>(a_payload.sequences.size());
		const auto decalCount = depthOnly ? std::array<std::uint32_t, kDecalGroups>{} : a_payload.decalCount;
		// Inputs the culling dispatch covers. In the depth segment this exceeds drawCount, because that
		// segment submits a cull-only input for every candidate it is not allowed to draw.
		const std::uint32_t inputCount = static_cast<std::uint32_t>(a_payload.inputList.size());
		const auto& previousShape = a_resources->published[shapeIndex];
		auto frame = std::make_shared<PassFrame>();
		frame->drawCapacity = GrowCapacity(previousShape ? previousShape->drawCapacity : 0u, drawCount, kMaxDraws);
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			frame->decalCapacity[group] = GrowCapacity(previousShape ? previousShape->decalCapacity[group] : 0u, decalCount[group], kMaxDecalDraws);
		frame->width = a_capture.viewportWidth;
		frame->height = a_capture.viewportHeight;
		// Both epochs rasterise with the main pass's depth range; see Impl::mainMinDepth.
		const bool useMainRange = a_resources->hybrid && mainMaxDepth > 0.0f;
		frame->minDepth = useMainRange ? mainMinDepth : a_capture.minDepth;
		frame->maxDepth = useMainRange ? mainMaxDepth : a_capture.maxDepth;
		frame->resourceHeap = org::runtime::GetActiveSRVDescriptorHeap().GetHandle();
		frame->samplerHeap = org::runtime::GetActiveSamplerDescriptorHeap().GetHandle();
		frame->indirect = GetIndirectState();
		frame->cullMode = CullingMode();
		// The main pass's ViewProj (VS_PerFrame c8), which the draws project with: the culling has to
		// use the same matrix or it would reject what the draws would have put on screen.
		std::array<float, 16> viewProj{};
		bool hasViewProj = false;
		if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister]) {
			mirror.Watch(buffer);
			const auto contents = mirror.Contents(buffer);
			if (contents.size() >= 48 * sizeof(float)) {
				std::memcpy(viewProj.data(), reinterpret_cast<const float*>(contents.data()) + 32, sizeof(float) * 16);
				hasViewProj = true;
			}
		}
		if (!loggedNoViewProj) {
			loggedNoViewProj = true;
			logger::info("[DCLF] culling setup: mode {}, ViewProj {}, VS_PerFrame b{} {}", frame->cullMode, hasViewProj ? "yes" : "no",
				kPerFrameVertexRegister, a_capture.vsBuffers[kPerFrameVertexRegister] ? "bound" : "not bound");
		}
		frame->hybrid = a_resources->hybrid;
		frame->offscreen = a_resources->offscreen;
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
		if ((depthOnly ? loggedDepthViewport : loggedColourViewport)++ % 480 == 0) {
			// The z row of the ViewProj each epoch actually packs: this is what turns a vertex into the
			// depth the test compares, so if the two epochs differ it shows up here.
			std::string viewProjZ = "(none)";
			{
				std::span<const std::byte> perFrame;
				if (replayVertexInputs)
					perFrame = prepassVS[kPerFrameVertexRegister];
				else if (auto* buffer = a_capture.vsBuffers[kPerFrameVertexRegister])
					perFrame = mirror.Contents(buffer);
				if (perFrame.size() >= 48 * sizeof(float)) {
					const auto* floats = reinterpret_cast<const float*>(perFrame.data());
					viewProjZ = fmt::format("({:.6f} {:.6f} {:.6f} {:.6f})", floats[40], floats[41], floats[42], floats[43]);
				}
			}
			logger::info("[DCLF] {} epoch: tables frame {} holding {} objects; render area {}x{}, depth range [{}, {}] (captured [{}, {}]), replay {}, eye ({:.2f} {:.2f} {:.2f}), ViewProj z row {}",
				depthOnly ? "z-prepass" : "colour", frameNumber, tables.objects.size(), frame->width, frame->height, frame->minDepth, frame->maxDepth,
				a_capture.minDepth, a_capture.maxDepth, replayVertexInputs ? "on" : "off", a_capture.eye.x, a_capture.eye.y, a_capture.eye.z, viewProjZ);
		}
		if (depthOnly) {
			prepassEye = a_capture.eye;
			prepassPreviousEye = a_capture.previousEye;
			prepassMinDepth = a_capture.minDepth;
			prepassMaxDepth = a_capture.maxDepth;
			prepassInputs = true;
		}
		a_stats.drawn = drawCount;
		++a_stats.commitEpochs;

		// The execution's values, into its slot of the latch (this runs inside the epoch, after the host
		// waited for the slot): what the BuildDraws dispatches of the segment read instead of push constants.
		BuildDrawsLatch latch{};
		latch.dispatch[0] = (inputCount + 63) / 64;
		latch.dispatch[1] = 1;
		latch.dispatch[2] = 1;
		latch.drawCount = inputCount;
		latch.cullFlags = (hasViewProj ? frame->cullMode : 0u) | (in.requireNativeVisible ? 0x100u : 0u);
		// The frame number, not the epoch: the depth segment publishes and the colour segment reads within one
		// frame, so the stamp has to be the thing they share.
		latch.visibilityStamp = frameNumber & 0x0FFFFFFFu;  // 28 bits: BuildDrawsCS keeps flags below it
		if (a_resources->hzb && frame->width && frame->height) {
			// Mip 0 covers twice its own size in source pixels, of which only the rendered area holds real
			// depth. A texture coordinate in the image scales by that ratio to reach the HZB.
			const auto scale = [](std::uint32_t a_rendered, std::uint32_t a_covered) {
				const double ratio = a_covered ? std::min(1.0, double(a_rendered) / double(a_covered)) : 0.0;
				return static_cast<std::uint32_t>(std::lround(ratio * 65535.0)) & 0xFFFFu;
			};
			latch.hzbUvScalePacked = scale(frame->width, a_resources->hzbWidth * 2) | (scale(frame->height, a_resources->hzbHeight * 2) << 16);
		}
		FoldEyeIntoViewProj(viewProj, a_capture.eye, latch.viewProj);
		a_resources->latch->WriteValue(RenderGraphRuntime::Get().Host()->CurrentFrameSlot(), 0, latch);

		// The shape: the previous one while nothing the recordings depend on changed, so the passes reuse
		// their invocations; a new generation otherwise.
		if (previousShape && previousShape->SameShape(*frame)) {
			a_resources->frames[shapeIndex].store(previousShape, std::memory_order_release);
		} else {
			frame->generation = ++a_resources->shapeGenerations;
			a_resources->published[shapeIndex] = frame;
			a_resources->frames[shapeIndex].store(std::move(frame), std::memory_order_release);
		}
		lap(6);
		return true;
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
		ScopedPerfEvent event("CS DCLF: G-buffer probe readback");
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

	void IndirectDraws::Impl::CheckSetParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_depth, const MainPayload& a_colour)
	{
		auto* context = globals::d3d::context;
		auto& store = SceneStore::Get();
		auto& counts = setParity;
		auto stateName = [](std::uint8_t a_state) -> std::string {
			if (a_state == kObjectStateDrawable)
				return "drawable";
			if (a_state == kObjectStateDecal)
				return "decal";
			if (a_state == kObjectStateAbsent)
				return "absent";
			return a_state < kSkipNames.size() ? std::string("skipped: ") + kSkipNames[a_state] : fmt::format("state {}", a_state);
		};

		// The oldest snapshot, once its frame's copy has had time to land.
		for (auto& waiting : setParityFrames)
			if (waiting.framesLeft)
				--waiting.framesLeft;
		while (!setParityFrames.empty() && setParityFrames.front().framesLeft == 0) {
			auto snapshot = std::move(setParityFrames.front());
			setParityFrames.pop_front();
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (FAILED(context->Map(snapshot.staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
				++counts.skipped;
				continue;
			}
			const auto* words = static_cast<const std::uint32_t*>(mapped.pData);
			const std::uint32_t stamp = snapshot.frame & 0x0FFFFFFFu;
			bool damaged = false;
			for (std::size_t o = 0; o < snapshot.flags.size(); ++o) {
				const std::uint32_t word = words[o];
				const bool current = (word >> 4) == stamp;
				const std::uint32_t verdict = current ? (word & 3) : ~0u;
				const bool depthDrawn = current && (word & 4);
				const bool colourDrawn = current && (word & 8);
				const std::uint8_t flags = snapshot.flags[o];
				const bool withheld = flags & 1;
				const bool alpha = flags & 4;
				counts.depthDrawnTotal += depthDrawn;
				counts.colourDrawnTotal += colourDrawn;
				const char* kind = nullptr;
				const bool decal = o < snapshot.colourState.size() && snapshot.colourState[o] == kObjectStateDecal;
				// A decal is drawn by the colour segment's decal pass only (the depth segment never draws one), so it
				// is only checked for being withheld and undrawn.
				if (!decal && depthDrawn && !colourDrawn) {
					kind = "depth without colour";
					++counts.depthOnly;
					counts.alphaDepthOnly += alpha;
				} else if (!decal && colourDrawn && !depthDrawn) {
					kind = verdict == 3 ? "colour without depth (no verdict this frame)" : "colour without depth";
					++counts.colourOnly;
					counts.colourUnpublished += verdict == 3;
					counts.alphaColourOnly += alpha;
				} else if (withheld && !colourDrawn) {
					if (verdict == 0 || verdict == 2) {
						++counts.withheldCulled;  // the GPU culling rejected it: not drawn by anyone, as intended
					} else {
						kind = "withheld natively, drawn by nobody";
						++counts.withheldUndrawn;
						counts.alphaWithheldUndrawn += alpha;
					}
				}
				if (!kind)
					continue;
				damaged = true;
				if (counts.samples++ < 40) {
					const auto* geometry = o < snapshot.geometry.size() ? snapshot.geometry[o] : nullptr;
					const bool alive = geometry && store.IsTracked(geometry);
					static constexpr const char* kVerdicts[] = { "occluded (retest)", "visible", "rejected", "no verdict" };
					logger::info("[DCLF] set parity, frame {}: {} - object {} '{}'{}: verdict {}, depth build {}, colour build {}, {}{}{}", snapshot.frame, kind, o,
						alive ? geometry->name.c_str() : "?", alpha ? " (alpha tested)" : "", current ? kVerdicts[verdict] : "not this frame's",
						stateName(o < snapshot.depthState.size() ? snapshot.depthState[o] : kObjectStateAbsent),
						stateName(o < snapshot.colourState.size() ? snapshot.colourState[o] : kObjectStateAbsent),
						(flags & 2) ? "engine kept it" : "engine culled it", (flags & 8) ? ", claimed" : "", withheld ? ", withheld" : "");
				}
			}
			context->Unmap(snapshot.staging.get(), 0);
			setParityStaging.push_back(std::move(snapshot.staging));
			++counts.frames;
			counts.framesWithDamage += damaged;
			if (counts.frames == 300) {
				logger::info("[DCLF] set parity over {} frames ({} with damage, {} unread): depth without colour {} ({} alpha tested), colour without depth {} ({} alpha tested, {} with no verdict), withheld and drawn by nobody {} ({} alpha tested); withheld and GPU-culled {}; per frame {:.0f} depth draws, {:.0f} colour draws",
					counts.frames, counts.framesWithDamage, counts.skipped, counts.depthOnly, counts.alphaDepthOnly, counts.colourOnly, counts.alphaColourOnly,
					counts.colourUnpublished, counts.withheldUndrawn, counts.alphaWithheldUndrawn, counts.withheldCulled, double(counts.depthDrawnTotal) / counts.frames,
					double(counts.colourDrawnTotal) / counts.frames);
				counts = {};
			}
		}

		// This frame: the words as the colour epoch left them, and the CPU side that explains them. Only when
		// the depth build is this frame's too.
		if (!a_resources->visibilityD3D11 || a_depth.inputs.frameNumber != a_colour.inputs.frameNumber || setParityFrames.size() >= 8)
			return;
		SetParityFrame snapshot;
		if (!setParityStaging.empty()) {
			snapshot.staging = std::move(setParityStaging.back());
			setParityStaging.pop_back();
		} else {
			D3D11_BUFFER_DESC desc{};
			a_resources->visibilityD3D11->GetDesc(&desc);
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.MiscFlags = 0;
			desc.StructureByteStride = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			if (FAILED(globals::d3d::device->CreateBuffer(&desc, nullptr, snapshot.staging.put())))
				return;
		}
		context->CopyResource(snapshot.staging.get(), a_resources->visibilityD3D11.get());
		snapshot.frame = a_colour.inputs.frameNumber;
		snapshot.framesLeft = 3;
		snapshot.depthState = a_depth.objectState;
		snapshot.colourState = a_colour.objectState;
		const auto& tables = store.GetTables();
		const auto claims = PassCapture::Get().CurrentClaims();
		const std::size_t objects = std::min<std::size_t>(tables.objects.size(), kMaxObjects);
		snapshot.flags.assign(objects, 0);
		snapshot.geometry.assign(objects, nullptr);
		for (std::size_t o = 0; o < objects && o < tables.objectGeometry.size(); ++o) {
			const auto* geometry = tables.objectGeometry[o];
			snapshot.geometry[o] = geometry;
			const auto objectFlags = tables.objects[o].flags;
			std::uint8_t flags = 0;
			if (objectFlags & kObjectNativeVisible)
				flags |= 2;
			if (objectFlags & kObjectAlphaTest)
				flags |= 4;
			if (geometry && claims && claims->contains(geometry)) {
				flags |= 8;
				if (PassCapture::Get().WithheldThisFrame(geometry))
					flags |= 1;
			}
			snapshot.flags[o] = flags;
		}
		setParityFrames.push_back(std::move(snapshot));
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
				a_stats.cullEngineCulled = words[3];
				a_stats.cullFalseNegatives = words[4];
				a_stats.cullRescued = words[5];
				a_stats.cullOccluded = words[6];
				a_stats.hzbNear = words[7];
				a_stats.hzbFar = words[8];
				a_stats.hzbSampled = words[9];
				if (words[10]) {
					a_stats.hzbSample.valid = true;
					std::memcpy(&a_stats.hzbSample.farthest, &words[11], sizeof(float));
					std::memcpy(&a_stats.hzbSample.nearestZ, &words[12], sizeof(float));
					a_stats.hzbSample.uvMin[0] = (words[13] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMin[1] = (words[13] >> 16) / 65535.0f;
					a_stats.hzbSample.uvMax[0] = (words[14] & 0xFFFF) / 65535.0f;
					a_stats.hzbSample.uvMax[1] = (words[14] >> 16) / 65535.0f;
					a_stats.hzbSample.mip = words[15] & 0xFF;
					a_stats.hzbSample.nativeVisible = (words[15] >> 8) != 0;
				} else {
					a_stats.hzbSample.valid = false;
				}
				a_stats.cullOccludedVisible = words[16];
				a_stats.cullDrawnPhaseTwo = words[17];
				a_stats.cullRescuedByPhaseTwo = words[18];
				a_stats.decalsCulled = words[kCountDecalsCulledWord];
				a_stats.decalsTested = words[kCountDecalsTestedWord];
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
		ScopedPerfEvent event("CS DCLF: culling readback");
		context->CopyResource(readback.count.get(), a_resources->countD3D11.get());
		readback.framesLeft = 3;
		cullReadback = std::move(readback);
	}

	void IndirectDraws::Impl::CheckBuildParity(const std::shared_ptr<Resources>& a_resources, const MainPayload& a_payload, IndirectDraws::Stats& a_stats)
	{
		const auto& sequences = a_payload.sequences;
		const auto& decalTemplates = a_payload.decalTemplates;
		const auto& inputs = a_payload.inputList;
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
			const auto* gpuSequences = static_cast<const DrawSequence*>(sequencesMap.pData);
			std::vector<DrawSequence> built(gpuSequences, gpuSequences + std::min<std::size_t>(count, kMaxDraws));
			// The decal slots: fixed, so they compare in place. A culled slot is the template with an index
			// count of zero; anything else differing is a defect.
			std::size_t decalDiffering = 0, decalCulled = 0, decalSlots = 0;
			for (std::uint32_t group = 0; group < kDecalGroups; ++group) {
				const auto& expectedDecals = parity->expectedDecals[group];
				const auto* slots = gpuSequences + kDecalSequenceBase + group * kMaxDecalDraws;
				for (std::size_t slot = 0; slot < expectedDecals.size() && slot < kMaxDecalDraws; ++slot, ++decalSlots) {
					// A zero-count slot draws nothing whatever its other fields hold: it is either a decal
					// the culling rejected or a blank the epoch pushed for one it could not build a record
					// for (whose template is then empty). Either way only the count matters.
					if (slots[slot].indexCount == 0) {
						++decalCulled;
						continue;
					}
					if (std::memcmp(&slots[slot], &expectedDecals[slot], sizeof(DrawSequence)) != 0)
						++decalDiffering;
				}
			}
			context->Unmap(parity->count.get(), 0);
			context->Unmap(parity->sequences.get(), 0);
			// BuildDraws appends in any order: compare as sets. The key has to be unique per sequence, which
			// the object index is and the record address is not - once records deduplicate, dozens of
			// sequences share an address, the sort stops being a total order, and equal-key runs land in
			// arbitrary relative order on the two sides. That reports mismatches that are not mismatches.
			auto byObject = [](const DrawSequence& a, const DrawSequence& b) { return a.objectIndex < b.objectIndex; };
			std::sort(built.begin(), built.end(), byObject);
			auto& expected = parity->expected;
			std::sort(expected.begin(), expected.end(), byObject);
			// What the GPU writes is a SUBSET of the CPU's templates whenever the culling rejects anything, so
			// this is a subsequence check, not an element-wise one: every sequence BuildDraws wrote must
			// appear in the CPU list under the same object index, byte for byte. Comparing position by
			// position instead reported every sequence past the first culled object as differing - a
			// mismatch that says nothing, on the configuration DCLF actually ships.
			std::size_t differing = 0, missing = 0;
			std::size_t first = SIZE_MAX, firstExpected = 0;
			for (std::size_t b = 0, e = 0; b < built.size(); ++b) {
				while (e < expected.size() && expected[e].objectIndex < built[b].objectIndex)
					++e;  // the CPU built a template the culling rejected
				if (e == expected.size() || expected[e].objectIndex != built[b].objectIndex) {
					++missing;  // a sequence with no template at all, which no culling can explain
					continue;
				}
				if (std::memcmp(&built[b], &expected[e], sizeof(DrawSequence)) != 0) {
					if (first == SIZE_MAX) {
						first = b;
						firstExpected = e;
					}
					++differing;
				}
				++e;
			}
			++a_stats.buildParityChecks;
			if (decalSlots)
				logger::info("[DCLF] BuildDraws decal parity {}: {} slots, {} culled, {} differ", decalDiffering ? "MISMATCH" : "OK", decalSlots, decalCulled, decalDiffering);
			if (decalDiffering)
				++a_stats.buildParityMismatches;
			if (!differing && !missing) {
				logger::info("[DCLF] BuildDraws parity OK: {} of {} sequences match the CPU templates ({} rejected by the culling)", count, expected.size(),
					expected.size() - count);
			} else {
				++a_stats.buildParityMismatches;
				logger::warn("[DCLF] BuildDraws parity MISMATCH: GPU wrote {}, CPU templated {} ({} counted culled); {} differ, {} have no template{}", count,
					expected.size(), culled, differing, missing,
					first != SIZE_MAX ? fmt::format(" (first: object {}, pipeline {} vs {}, index count {} vs {})", built[first].objectIndex, built[first].pipelineIndex,
											expected[firstExpected].pipelineIndex, built[first].indexCount, expected[firstExpected].indexCount) :
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
		// The templates cover every candidate; the gate drops the ones the engine culled before BuildDraws
		// writes a sequence for them, and it is a pure per-object flag test, so the expectation can apply it
		// exactly. Frustum culling cannot be predicted here, which is why parity and culling are separate
		// switches. inputs is parallel to sequences.
		readback.expected.clear();
		readback.expected.reserve(sequences.size());
		for (std::uint32_t group = 0; group < kDecalGroups; ++group)
			readback.expectedDecals[group] = decalTemplates[group];
		{
			// Cull-only inputs carry no sequence, so the two run at different rates and the drawable ones
			// have to be counted off rather than indexed in step. Decals have their own slots and templates.
			std::size_t sequence = 0;
			for (const auto& input : inputs) {
				if (!(input.flags & kInputDrawable) || (input.flags & kObjectDecal))
					continue;
				if (sequence >= sequences.size())
					break;
				const auto& candidate = sequences[sequence++];
				if (RequireNativeVisible() && !(input.flags & kObjectNativeVisible))
					continue;
				readback.expected.push_back(candidate);
			}
		}
		readback.framesLeft = 3;
		if (readback.sequences && readback.count)
			parity = std::move(readback);
	}

	void IndirectDraws::ShowDebugView()
	{
		// The hybrid path imports the native targets too, so their presence does not mean the view is on: an
		// epoch here cost ~0.3 ms of render thread and three queue submissions a frame for nothing.
		if (failed || !DebugViewEnabled() || !impl->resources || !impl->resources->native[0])
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
	void IndirectDraws::PublishClaims() {}
	bool IndirectDraws::DrewLastFrame(const RE::BSGeometry*, std::uint32_t) const { return false; }
	void IndirectDraws::CaptureMainPass() {}
	void IndirectDraws::Execute() {}
	void IndirectDraws::CaptureDepthPass() {}
	void IndirectDraws::ExecuteColour() {}
	void IndirectDraws::ProbeTargets(const char*) {}
	void IndirectDraws::RunEpoch(RenderGraphRuntime::Segment) {}
	void IndirectDraws::ShowDebugView() {}
	bool IndirectDraws::ShadowsEnabled() { return false; }
	void IndirectDraws::BeginShadowFrame(const RE::NiPoint3&) {}
	void IndirectDraws::ExecuteShadowView(std::uint32_t, std::uint32_t) {}
	void IndirectDraws::ExecuteShadowFrame() {}
	void IndirectDraws::KickColourBuild() {}
	void IndirectDraws::KickZPrepassBuild() {}
	void IndirectDraws::KickShadowBuild() {}
	void IndirectDraws::EndFrame() {}
	void IndirectDraws::DrainAsync() {}
	std::string IndirectDraws::AsyncReport() { return {}; }
}

#endif
