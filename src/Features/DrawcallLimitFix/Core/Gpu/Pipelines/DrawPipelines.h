#pragma once

// DCLF's draw pipelines. The layout constants here (push words, row layouts, fixed texture registers) are shared with the shaders
// and must change together with them. Also the catalog the pipeline lane publishes, and DrawPipelines, which owns that lane.

#include <array>
#include <cstddef>
#include <cstdint>
#include <dxgiformat.h>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <bit>
#include <type_traits>
#include <utility>
#include <vector>

#include "Features/DrawcallLimitFix/Core/Shading/ConstantEvaluator.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Records.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/ShaderPrograms.h"
#include "Features/DrawcallLimitFix/Api/Views.h"
#include "RenderGraph/BufferRef.h"

namespace DCLF
{
	/** @brief Constant buffer registers b0-b13, separate per stage as in D3D11. */
	inline constexpr std::uint32_t kConstantBufferRegisters = 14;
	/**
	 * @brief Registers whose block is the same for every draw of a pass. Their addresses are pushed once per pass, saving an indirection.
	 * Push data order: the record address range (4 words, for alignment), then these addresses, VS registers ascending, then PS.
	 */
	inline constexpr std::uint32_t kFramePushVS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 11) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushPS = (1u << 3) | (1u << 5) | (1u << 6) | (1u << 9) | (1u << 10) | (1u << 12) | (1u << 13);
	inline constexpr std::uint32_t kFramePushBinding = 191;
	// The draw's push range. Per draw under device-generated commands, once per call for plain draws.
	inline constexpr std::uint32_t kDrawPushBinding = 190;
	// The frame record's address, its buffer view ref (Common/DCLFFrameRecord.hlsli), then the registers' addresses.
	inline constexpr std::uint32_t kFramePushRecord = 0;
	inline constexpr std::uint32_t kFramePushRecordRef = 2;
	inline constexpr std::uint32_t kFramePushRegisters = 4;
	inline constexpr std::uint32_t kFramePushWords = kFramePushRegisters + 2 * (std::popcount(kFramePushVS) + std::popcount(kFramePushPS));
	// Pass-wide blocks, in the frame push words' order. Read through uniform buffer descriptors rather than pushed addresses,
	// because address-sourced blocks cost the colour pixel stage about 40 registers on NVIDIA (tools/shader-compare).
	inline constexpr std::uint32_t kFrameBlocks = std::popcount(kFramePushVS) + std::popcount(kFramePushPS);
	// A draw's push words (DrawSequence bytes 4-99). These must match DCLFPushData's packoffsets in DCLFObjects.hlsli.
	// BasicRHI needs pushed addresses 8-byte aligned, hence the pad at word 13 and the even word count.
	inline constexpr std::uint32_t kDrawPushPipelineRow = 0, kDrawPushMaterialRow = 2, kDrawPushObject = 4, kDrawPushVertexBuffer = 5, kDrawPushStreamBuffer = 9,
								   kDrawPushTechniquePS = 14, kDrawPushGeometryVS = 16, kDrawPushGeometryPS = 18, kDrawPushPermutation = 20, kDrawPushMaterialPS = 22,
								   kDrawPushArgumentWords = 24, kDrawPushWords = 24;
	static_assert(kDrawPushWords % 2 == 0 && kDrawPushArgumentWords <= kDrawPushWords);
	// Pushed addresses must sit at even words for BasicRHI's 8-byte alignment.
	static_assert(kDrawPushPipelineRow % 2 == 0 && kDrawPushMaterialRow % 2 == 0 && kDrawPushTechniquePS % 2 == 0 && kDrawPushGeometryVS % 2 == 0 &&
				  kDrawPushGeometryPS % 2 == 0 && kDrawPushPermutation % 2 == 0 && kDrawPushMaterialPS % 2 == 0);
	// Push word w is sequence byte 4 + 4w, and the argument ends where the index buffer view starts.
	static_assert(offsetof(DrawSequence, pipelineRowAddress) == 4 + 4 * kDrawPushPipelineRow && offsetof(DrawSequence, materialRowAddress) == 4 + 4 * kDrawPushMaterialRow &&
				  offsetof(DrawSequence, objectIndex) == 4 + 4 * kDrawPushObject && offsetof(DrawSequence, vertexBufferRef) == 4 + 4 * kDrawPushVertexBuffer &&
				  offsetof(DrawSequence, streamBufferRef) == 4 + 4 * kDrawPushStreamBuffer &&
				  offsetof(DrawSequence, psTechniqueAddress) == 4 + 4 * kDrawPushTechniquePS && offsetof(DrawSequence, vsGeometryAddress) == 4 + 4 * kDrawPushGeometryVS &&
				  offsetof(DrawSequence, psGeometryAddress) == 4 + 4 * kDrawPushGeometryPS && offsetof(DrawSequence, permutationAddress) == 4 + 4 * kDrawPushPermutation &&
				  offsetof(DrawSequence, psMaterialAddress) == 4 + 4 * kDrawPushMaterialPS &&
				  offsetof(DrawSequence, indexBufferAddress) == 4 + 4 * kDrawPushArgumentWords);
	// D3D12 plain draws push the instance per draw, because D3D12's SV_InstanceID does not count the first instance (DCLFInstanceWord).
	inline constexpr std::uint32_t kDrawPushInstance = 23;
	static_assert(kDrawPushInstance < kDrawPushWords);
	// Z-prepass plain draw push word. A call draws every slot sharing its depth pipeline, so rows are read per draw through the sequence.
	inline constexpr std::uint32_t kZDrawPushSequences = 0;
	// Terrain Blending's depth offset (Utility.hlsl OFFSET_DEPTH), 1 for a terrain twin bucket's call. Added to clip z.
	inline constexpr std::uint32_t kZDrawPushOffsetDepth = 4;
	// A shadow view plain draw's push words.
	inline constexpr std::uint32_t kShadowDrawPushSequences = 0, kShadowDrawPushMaterialRows = 2, kShadowDrawPushVertexLayout = 4;
	// CPU-driven direct draws push the per-frame object word, which the resident sequence does not hold, plus a flag to use it.
	// The GPU path pushes 0 for both, so its stages read the sequence's word.
	inline constexpr std::uint32_t kDirectDrawPushObjectWord = 6, kDirectDrawPushFlags = 7;
	inline constexpr std::uint32_t kDirectFlagObjectWord = 1;
	// The sequence holds rows as offsets from bases pushed once per list, so resident sequences do not embed a payload's row versions.
	inline constexpr std::uint32_t kDirectFlagRelativeRows = 2;
	inline constexpr std::uint32_t kDirectDrawPushRowBases = 8;
	static_assert(kDirectDrawPushObjectWord > kZDrawPushOffsetDepth && kDirectDrawPushObjectWord >= kShadowDrawPushVertexLayout + 2 && kDirectDrawPushFlags < kDrawPushWords);
	static_assert(kDirectDrawPushRowBases > kDirectDrawPushFlags && kDirectDrawPushRowBases + 4 <= kDrawPushWords);
	// The frame's copy of the animated material rows, pushed once per list after the row bases. Matches BuildDrawsCS's MaterialRowAddress.
	inline constexpr std::uint32_t kDirectDrawPushAnimatedRows = kDirectDrawPushRowBases + 4;
	static_assert(kDirectDrawPushAnimatedRows + 2 <= kDrawPushWords);
	// The draw belongs to an indirect run. Its instance names its record in the frame slot's draw buffer.
	inline constexpr std::uint32_t kDirectFlagDrawRecords = 4;
	inline constexpr std::uint32_t kDirectDrawPushDrawRecords = kDirectDrawPushAnimatedRows + 2;
	static_assert(kDirectDrawPushDrawRecords + 2 <= kDrawPushWords && kDirectDrawPushDrawRecords % 2 == 0);
	/** @brief Where a DrawSequence's draw arguments start. The shadow views' plain indexed draws read them. */
	inline constexpr std::uint32_t kSequenceDrawOffset = 116;
	static_assert(offsetof(DrawSequence, indexCount) == kSequenceDrawOffset);
	// The shadow views' layout. A draw pushes its material row address and object word. The rest is pushed once per view.
	// One row table serves every view.
	inline constexpr std::uint32_t kShadowPushWords = 14;
	inline constexpr std::uint32_t kShadowPushFrameRecord = 0, kShadowPushFrameRecordRef = 2, kShadowPushViewBlock = 4, kShadowPushPerFrame = 6, kShadowPushZeros = 8,
								   kShadowPushSharedData = 10, kShadowPushFeatureData = 12;
	static_assert(kShadowPushFrameRecordRef == kFramePushRecordRef, "Common/DCLFFrameRecord.hlsli reads both at one word");
	inline constexpr std::uint32_t kPerFrameVertexRegister = 12;  // ViewProj at c8
	inline constexpr std::uint32_t kSharedDataRegister = 5;  // bound by Community Shaders
	inline constexpr std::uint32_t kFeatureDataRegister = 6;
	inline constexpr std::uint32_t kShadowRowDiffuseOffset = 16;

	// The main pass's rows. A draw names one material row and one pipeline row. Other registers come from the frame record.
	// Blocks sit at 256-byte offsets. The rows hold no addresses, so a version's bytes are the same in any backing.
	inline constexpr std::uint32_t kMaterialRowBytes = 1024;
	inline constexpr std::uint32_t kMaterialRowVS = 0, kMaterialRowPS = 256, kMaterialRowHeader = 768;
	inline constexpr std::uint32_t kMaterialRowVSBytes = 256, kMaterialRowPSBytes = 512;
	struct MaterialRowHeader
	{
		// Index among the version's animated rows, or ~0u for none. The pulled stages read these fields at fixed offsets.
		std::uint32_t animatedRow = ~0u;
		// Texture streaming's feedback slot and buffer view ref.
		std::uint32_t feedbackSlot = 0;
		RenderGraph::BufferRef feedbackRef;
		std::uint32_t textures[kPixelTextureSlots]{};
		std::uint32_t samplers[16]{};
		std::uint32_t features[Extensions::kFeatureTextureSlots]{};
	};
	static_assert(kMaterialRowHeader + sizeof(MaterialRowHeader) <= kMaterialRowBytes);
	// DCLFMaterialTextureIndex in DCLFObjects.hlsli reads textures from byte 16 and features 32 words after them.
	static_assert(offsetof(MaterialRowHeader, features) == 16 + 4 * 32);
	inline constexpr std::uint32_t kPipelineRowBytes = 2048;
	inline constexpr std::uint32_t kPipelineRowTechniqueVS = 0, kPipelineRowTechniquePS = 256, kPipelineRowGeometryVS = 512, kPipelineRowGeometryPS = 768,
								   kPipelineRowPermutation = 1280, kPipelineRowHeader = 1536;
	inline constexpr std::uint32_t kPipelineRowTechniqueBytes = 256, kPipelineRowGeometryVSBytes = 256, kPipelineRowGeometryPSBytes = 512,
								   kPipelineRowPermutationBytes = 256;
	struct PipelineRowHeader
	{
		// Zero. The block addresses live in the draw's sequence. The pulled stages read the words below at fixed offsets.
		std::uint64_t reserved[6]{};
		std::uint32_t shadowMask = 0;  // t14
		std::uint32_t shadowMaskSampler = 0;  // s14
		std::uint64_t vertexLayout = 0;  // decoded by a pulled vertex stage
	};
	static_assert(kPipelineRowHeader + sizeof(PipelineRowHeader) <= kPipelineRowBytes);
	static_assert(offsetof(PipelineRowHeader, shadowMask) == 48 && offsetof(PipelineRowHeader, vertexLayout) == 56);  // DCLFPipeline*Index
	// On Vulkan the shaders read blocks through PhysicalStorageBuffer pointers whose loads assume 16-byte alignment.
	// Rows and blocks are kept at 256 bytes, D3D11's constant buffer offset granularity.
	static_assert(kPipelineRowBytes % 256 == 0 && kMaterialRowBytes % 256 == 0);
	static_assert(kPipelineRowTechniqueVS % 256 == 0 && kPipelineRowTechniquePS % 256 == 0 && kPipelineRowGeometryVS % 256 == 0 &&
				  kPipelineRowGeometryPS % 256 == 0 && kPipelineRowPermutation % 256 == 0 && kMaterialRowVS % 256 == 0 && kMaterialRowPS % 256 == 0);
	/** @brief A sequence's block addresses from its rows' addresses, as BuildDrawsCS writes them. Used by the CPU templates. */
	inline void SetSequenceBlocks(DrawSequence& a_sequence)
	{
		a_sequence.psTechniqueAddress = a_sequence.pipelineRowAddress + kPipelineRowTechniquePS;
		a_sequence.vsGeometryAddress = a_sequence.pipelineRowAddress + kPipelineRowGeometryVS;
		a_sequence.psGeometryAddress = a_sequence.pipelineRowAddress + kPipelineRowGeometryPS;
		a_sequence.permutationAddress = a_sequence.pipelineRowAddress + kPipelineRowPermutation;
		a_sequence.psMaterialAddress = a_sequence.materialRowAddress + kMaterialRowPS;
	}
	/** @brief Pixel shader resource registers t0-t127. */
	inline constexpr std::uint32_t kTextureRegisters = 128;
	/** @brief Pixel shader sampler registers s0-s15. */
	inline constexpr std::uint32_t kSamplerRegisters = 16;
	/** @brief t127, the per-object record buffer. Top of the range because the engine binds nothing there. */
	inline constexpr std::uint32_t kObjectBufferRegister = kTextureRegisters - 1;
	/** @brief t126, the objects' extras rows, read by both stages. */
	inline constexpr std::uint32_t kExtrasBufferRegister = kTextureRegisters - 2;
	/**
	 * @brief t125, the character light's noise, which the engine binds at t11. It is a render target that alternates every frame,
	 * so the frame record supplies it and the material row binds null at t11.
	 */
	inline constexpr std::uint32_t kCharacterLightRegister = kTextureRegisters - 3;
	// t124 is a feature's frame binding.
	/** @brief t123, the objects' placement rows, both stages. */
	inline constexpr std::uint32_t kPlacementBufferRegister = kTextureRegisters - 5;
	/** @brief t122, the frame's bone palettes, vertex stage. */
	inline constexpr std::uint32_t kPaletteBufferRegister = kTextureRegisters - 6;
	/** @brief t121, the objects' shading rows by slot, both stages. The lowest vertex-stage register. */
	inline constexpr std::uint32_t kShadingBufferRegister = kTextureRegisters - 7;
	/** @brief t120 and t119, fade states and fade root rows, pixel stage. Unbound reads generation 0, no fade. */
	inline constexpr std::uint32_t kFadeStatesRegister = kTextureRegisters - 8;
	inline constexpr std::uint32_t kFadeRootsRegister = kTextureRegisters - 9;
	/**
	 * @brief t117, the fade roots' seeds, pixel stage. Used as a root's state until FadeStateCS first updates its generation.
	 * Unbound reads none.
	 */
	inline constexpr std::uint32_t kFadeSeedsRegister = kTextureRegisters - 11;
	/** @brief t116, the frame's per-object pixel rows, pixel stage. PixelRowsCS writes them first in the depth segment. */
	inline constexpr std::uint32_t kPixelObjectsRegister = kTextureRegisters - 12;

	/** @brief A buffer some device address points into. Stages turn it into a ref with DCLFRefFromBacking. */
	struct RecordBacking
	{
		RenderGraph::BufferRef ref;
		std::uint64_t address = 0;
		std::uint32_t bytes = 0;
		std::uint32_t pad[3]{};
	};
	static_assert(sizeof(RecordBacking) == 32);
	/** @brief Indices into DrawBindings::backings. Must match kDCLFBacking* in Common/DCLFFrameRecord.hlsli. */
	enum RecordBackingIndex : std::uint32_t
	{
		kBackingFrameConstants,
		kBackingPipelineRows,
		kBackingMaterialRows,
		kBackingAnimatedRows,
		kBackingShadowRows,
		kRecordBackings
	};

	struct DrawBindings
	{
		std::uint64_t vertexConstants[kConstantBufferRegisters];  // device addresses
		std::uint64_t pixelConstants[kConstantBufferRegisters];
		std::uint32_t textures[kTextureRegisters];  // descriptor heap indices
		std::uint32_t samplers[kSamplerRegisters];  // descriptor heap indices
		std::uint32_t frameBlocks[kFrameBlocks];  // uniform buffer descriptor heap indices
		std::uint32_t pad = 0;
		// The buffers device addresses point into. No layout reads them.
		RecordBacking backings[kRecordBackings];
	};
	static_assert(offsetof(DrawBindings, backings) == 864 && sizeof(DrawBindings) == 864 + 32 * kRecordBackings);

	/** @brief The registers a pipeline's shaders declare, which a draw's DrawBindings must supply. */
	struct RegisterUsage
	{
		std::uint32_t vertexConstants = 0;  // bit per register
		std::uint32_t pixelConstants = 0;
		std::array<std::uint64_t, kTextureRegisters / 64> textures{};
		std::uint32_t samplers = 0;

		bool UsesTexture(std::uint32_t a_register) const { return (textures[a_register / 64] >> (a_register % 64)) & 1; }
	};

	/**
	 * @brief Per Lighting variable, its float offset in its block, 0 where absent. Offset 0 counts only for a group's first variable.
	 * Built from SPIR-V reflection of DCLF's own modules.
	 */
	struct ConstantTables
	{
		std::vector<std::uint8_t> vs, ps;
	};

	struct Lookups;

	/** @brief Pipeline variants: the main colour pass with depth EQUAL, and DCLF's own Z-prepass with depth LESS and writes. */
	inline constexpr std::uint32_t kColorVariant = 0;
	inline constexpr std::uint32_t kDepthVariant = 1;
	inline constexpr std::uint32_t kVariantCount = 2;
	/**
	 * @brief A main pipeline's state class in the high bits and its index in that class's execution sets in the low bits.
	 * Pipelines in one execution set must share fixed-function state (VUID-vkUpdateIndirectExecutionSetPipelineEXT-None-11040).
	 */
	inline constexpr std::uint32_t kSetClassShift = 16, kSetClassIndexMask = 0xFFFFu, kNoSetClass = ~0u;
	/**
	 * @brief The refraction draw's Utility technique. The engine's technique also varies by Vc, Skinned and normal options, but the
	 * pulled build reads those per draw, so one pipeline draws every refraction object.
	 */
	inline constexpr std::uint32_t kRefractionTechnique = 0x20Au;
	/** @brief Shadow pipeline classes, drawn in this order. */
	inline constexpr std::uint32_t kShadowDepthOnly = 0;
	inline constexpr std::uint32_t kShadowDiscards = 1;
	inline constexpr std::uint32_t kShadowClasses = 2;

	/** @brief A forward view's target formats. Depth is the DSV format, not typeless. */
	struct ForwardTargets
	{
		DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;
		// Second colour attachment for first-person motion vectors (FUN_1414cb6d0 slot 1). Unknown means one attachment.
		DXGI_FORMAT colour2 = DXGI_FORMAT_UNKNOWN;

		bool operator==(const ForwardTargets&) const = default;
	};

	/**
	 * @brief A forward view's pipeline key. Culls front faces unless two-sided, because a cube face's projection mirrors the image
	 * and the engine culls front faces there.
	 */
	struct ForwardPipelineKey
	{
		// Core's bits. A view's own bits sit above them and name its targets and culling.
		static constexpr std::uint32_t kForwardTwoSided = 1, kForwardTreeLod = 2;
		std::uint32_t vertexDescriptor = 0, pixelDescriptor = 0;
		std::uint32_t flags = 0;

		bool operator==(const ForwardPipelineKey&) const = default;
	};
	static_assert(std::has_unique_object_representations_v<ForwardPipelineKey>);
	static_assert(DrawFamilyLaneSlotOf(Api::DrawFamily::TreeInstances).forwardFlags == ForwardPipelineKey::kForwardTreeLod,
		"the TreeInstances family's lane slot is not ForwardPipelineKey::kForwardTreeLod");
	static_assert(DrawFamilyLaneSlotOf(Api::DrawFamily::Grass).forwardFlags == 0, "the Grass family has no faces, so no forward key flags");
	struct ForwardPipelineKeyHash
	{
		using is_avalanching = void;
		std::uint64_t operator()(const ForwardPipelineKey& a_key) const noexcept { return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key)); }
	};

	/** @brief Render target and depth formats of the native main pass. */
	struct TargetFormats
	{
		std::array<DXGI_FORMAT, 8> colors{};
		std::uint32_t colorCount = 0;
		DXGI_FORMAT depth = DXGI_FORMAT_UNKNOWN;

		bool operator==(const TargetFormats&) const = default;
	};

	/**
	 * @brief The pipeline lane's results, immutable and published whole, latest wins.
	 * A set index is handed out only once a published set version holds its pipeline, so indices and set always agree.
	 */
	struct PipelineCatalog
	{
		static constexpr std::uint32_t kNone = ~0u;
		struct Entry
		{
			std::uint32_t setIndex = kNone;  // kNone while pending, and for good once failed
			bool failed = false;
			std::array<RegisterUsage, kVariantCount> usage{};
			std::shared_ptr<const ConstantTables> tables;
		};
		ankerl::unordered_dense::map<PipelineKey, Entry, PipelineKeyHash> entries;
		std::uint32_t generation = 0;  // a reset of the set starts a new one and every earlier index is stale
		std::uint64_t revision = 0;
		std::shared_ptr<const void> setVersion;  // holds every setIndex above

		struct ShadowEntry
		{
			std::uint32_t setIndex = kNone;  // kNone while pending, and for good once failed
			bool failed = false;
		};
		ankerl::unordered_dense::map<ShadowPipelineKey, ShadowEntry, ShadowPipelineKeyHash> shadowEntries;
		std::uint32_t shadowGeneration = 0;  // a reset of the shadow set starts a new one
		DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
		std::shared_ptr<const void> shadowSetVersion;

		// A failed program fails for good, and so does a failed build until a shader reload.
		struct FamilyEntry
		{
			std::shared_ptr<const void> pipelines;
			bool failed = false;
		};
		std::array<FamilyEntry, kDrawFamilyLanes> families{};
		struct RefractionEntry
		{
			std::shared_ptr<const void> pipeline;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
			bool failed = false;
		};
		RefractionEntry refraction;
		struct ForwardEntry
		{
			std::shared_ptr<const void> pipeline;  // an rhi::PipelinePtr
			bool program = false;
			bool failed = false;  // for good
		};
		ankerl::unordered_dense::map<ForwardPipelineKey, ForwardEntry, ForwardPipelineKeyHash> forwardEntries;
		std::array<ForwardTargets, static_cast<std::size_t>(Api::EngineView::Count)> viewTargets{};

		const Entry* Find(const PipelineKey& a_key) const
		{
			const auto it = entries.find(a_key);
			return it == entries.end() ? nullptr : &it->second;
		}
		const ShadowEntry* FindShadow(const ShadowPipelineKey& a_key) const
		{
			const auto it = shadowEntries.find(a_key);
			return it == shadowEntries.end() ? nullptr : &it->second;
		}
		const ForwardEntry* FindForward(const ForwardPipelineKey& a_key) const
		{
			const auto it = forwardEntries.find(a_key);
			return it == forwardEntries.end() ? nullptr : &it->second;
		}
	};

	/**
	 * @brief The graphics pipelines for DCLF's indirect draws and the execution sets the draws select them from.
	 *
	 * Main pipelines share one layout and are built asynchronously from a key and its SPIR-V program. Vertex stages pull their own
	 * attributes, so there is no vertex input. A program that needs a register or attribute the layout cannot supply fails, and its
	 * objects stay native. Pipelines with the same fixed-function state share an execution set per variant.
	 *
	 * The pipeline lane is the only owner. Render-thread state reaches it only as posted frame inputs, and the render thread only
	 * posts requests and reads its frame's catalog.
	 */
	class DrawPipelines
	{
	public:
		static constexpr std::uint32_t kNotReady = ~0u;
		static constexpr std::uint32_t kMaxPipelines = 4096;
		// The initial capacity of a state class's sets. They double past it, up to kMaxPipelines.
		static constexpr std::uint32_t kFirstClassSetPipelines = 32;

		struct Stats
		{
			std::uint32_t requested = 0;
			std::uint32_t ready = 0;
			std::uint32_t failed = 0;
			std::uint32_t zPipelines = 0, zDepthOnly = 0;
			std::uint32_t stateClasses = 0, largestClass = 0;
			std::uint64_t classParityChecks = 0, classParityMismatches = 0;
			std::uint32_t shadowRequested = 0;
			std::uint32_t shadowReady = 0;
			std::uint32_t shadowFailed = 0;
			// Set versions published, and admissions that waited because every version was still held.
			std::uint32_t setPublishes = 0, setWaits = 0, shadowSetPublishes = 0, shadowSetWaits = 0;
			// Frames that wanted a pipeline not yet in their catalog draw natively meanwhile.
			struct DrawFamilyCounters
			{
				const char* label = nullptr;
				std::uint32_t requested = 0, ready = 0, failed = 0, waits = 0;
			};
			std::array<DrawFamilyCounters, kDrawFamilyLanes> drawFamilies{};
			std::uint32_t forwardRequested = 0, forwardReady = 0, forwardFailed = 0;
			std::uint32_t forwardWaits = 0;
			struct Family
			{
				std::uint32_t requested = 0, ready = 0, failed = 0;
			};
			std::array<Family, Extensions::kProgramFamilies> families{};
		};

		static DrawPipelines& Get();

		bool Enabled() const;
		/** @brief Whether the device has device-generated commands with pipeline sets. Without them colour uses plain indirect draws only. */
		bool DeviceGenerated() const;
		/** @brief Whether the device can draw the GPU path at all, which needs counted indirect draws. Otherwise every cell is CPU-driven. */
		bool GpuDriven() const;
		/** @brief Whether the main set's pipelines are built indirect-bindable. */
		bool IndirectBindable() const;

		/**
		 * @brief Render thread, once at setup. The main pass's target formats, from the engine target set's declarations. They never
		 * change in a session. A second call with other formats is fatal.
		 */
		void InitTargetFormats(const TargetFormats& a_formats);
		bool HasTargetFormats() const { return targets.colorCount != 0; }
		const TargetFormats& Targets() const { return targets; }

		/** @brief Any thread. Asks the lane for a key's program and pipeline. The lane keeps every key. */
		void RequestLighting(const PipelineKey& a_key, std::uint32_t a_slot);
		/** @brief Scene lane only. The newest catalog since the last call, or null if none is newer. */
		std::shared_ptr<const PipelineCatalog> TakeCatalog();
		/** @brief Render thread, frame start. Holds the catalog the installed publication was resolved from, for the frame. */
		void HoldCatalog(std::shared_ptr<const PipelineCatalog> a_catalog) { heldCatalog = std::move(a_catalog); }
		/** @brief The frame's catalog, or null before the first. */
		const std::shared_ptr<const PipelineCatalog>& HeldCatalog() const { return heldCatalog; }

		/**
		 * @brief Any thread. Asks the lane for a shadow view key's pipeline. Shadow pipelines are a separate set, depth only, with the
		 * view's rasterizer state.
		 */
		void RequestShadow(const ShadowPipelineKey& a_viewKey, const ShadowPipelineKey& a_casterKey, std::uint32_t a_occlusion);
		/**
		 * @brief Render thread, once at setup. The shadow map DSV format, from the engine target set's declarations, and the Utility
		 * shader. A second call with another format is fatal.
		 */
		void InitShadowInputs(DXGI_FORMAT a_format, RE::BSShader& a_utility);

		/**
		 * @brief Render thread. An id for a shadow view's rasterizer state, or 0 if a pipeline cannot express it.
		 * Read it while the view is drawn, because ShadowmapCascadeRasterizerFix swaps in per-cascade copies only for that window.
		 */
		std::uint32_t ShadowRasterStateId(const D3D11_RASTERIZER_DESC& a_desc, std::uint32_t a_renderMode);

		/** @brief How many shadow rasterizer states are registered, ids 1 to this. */
		std::uint32_t ShadowRasterStateCount() const;

		/**
		 * @brief Render thread. Asks for a draw family's Z-prepass and colour pipelines, two-sided as the engine's.
		 * They arrive in the catalog once the family's inputs, targets, winding and write mode are in.
		 */
		void RequestFamily(Api::DrawFamily a_family);
		/** @brief Render thread. The engine shader a draw family's programs come from, posted on change. */
		void SetFamilyInputs(Api::DrawFamily a_family, RE::BSShader& a_shader);
		/**
		 * @brief Render thread. Asks for the refraction pipeline. No culling, because the pixel stage drops one-sided back faces.
		 * It arrives in the catalog once SetRefractionInputs has posted.
		 */
		void RequestRefraction();
		/** @brief Render thread. Refraction inputs, posted on change. */
		void SetRefractionInputs(RE::BSShader& a_utility, DXGI_FORMAT a_format, std::uint8_t a_writeMask);
		/** @brief Render thread. Asks for a forward pipeline. Repeats are dropped. */
		void RequestForward(const ForwardPipelineKey& a_key);
		/** @brief Render thread. A view's forward targets. A change rebuilds every forward pipeline. */
		void SetViewForwardTargets(Api::EngineView a_view, const ForwardTargets& a_targets);
		/** @brief Render thread. Counts a frame whose draw family waited for its pipelines. */
		void NoteFamilyWait(Api::DrawFamily a_family, bool a_waited) { stats.drawFamilies[static_cast<std::size_t>(a_family)].waits += a_waited ? 1u : 0u; }
		/** @brief Render thread. Counts a frame whose forward view waited for a pipeline. */
		void NoteForwardWait(bool a_waited) { stats.forwardWaits += a_waited ? 1u : 0u; }

		/**
		 * @brief Render thread. Captures the engine's raster and blend states the lane asked for, and posts what is new.
		 * Call inside the deferred pass. Community Shaders swaps in deferred blend variants only between StartDeferred and ResetBlendStates.
		 */
		void CaptureEngineStates();


		/** @brief Render thread. The shadow map format set at setup. */
		DXGI_FORMAT ShadowFormat() const;

		/** @brief Any thread. Counters for the report, read relaxed. */
		Stats GetStats() const;

		/** @brief Lighting constant variable names by index, per stage. */
		static std::span<const std::string_view> LightingVariableNames(bool a_pixel);

	private:
		DrawPipelines();
		~DrawPipelines();

		struct Impl;
		std::unique_ptr<Impl> impl;
		TargetFormats targets;
		std::shared_ptr<const PipelineCatalog> heldCatalog;
		Stats stats;

		friend struct IndirectState GetIndirectState(const PipelineCatalog&);
		friend bool IndirectStateValid(const PipelineCatalog&);
		friend bool ZGroupPipelineReady(const PipelineCatalog&, std::uint32_t);
		friend struct ForwardPipelineAccess;  // CatalogFamilyPipelines
		friend struct ShadowIndirectState GetShadowIndirectState(const PipelineCatalog&, DXGI_FORMAT);
	};

	/** @brief Render thread. The frame's catalog, which every pipeline lookup in the frame resolves from. */
	inline const PipelineCatalog* FrameCatalog()
	{
		return DrawPipelines::Get().HeldCatalog().get();
	}
}
