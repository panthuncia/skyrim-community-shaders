#pragma once

// Views drawn by epochs of their own, the reflection's faces and first person, and their frame state.
// Captured and committed on the render thread. Their shapes come from the revision producer. Features include this header.

// Main/Resources.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Core/Render/Main/Resources.h"
#include "RenderGraph/BufferRef.h"
#include "Features/DrawcallLimitFix/Api/Passes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Render/Main/FrameLayout.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Latched.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/Versioned.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/GpuLayouts.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/Helpers.h"
#include "Features/DrawcallLimitFix/Core/Render/Revision/Epochs.h"
#include "Features/DrawcallLimitFix/Core/Render/Shadow/ShadowCameras.h"

#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/RenderGraphRuntime.h"
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>
#include <Resources/PixelBuffer.h>
#include <rhi.h>

#include <d3d11.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
}

namespace DCLF::Draws
{
	class FrameBlockViews;  // Resources/FrameSlots.h

	/*
	 * View epochs cull the frame's scene list, frustum only, with each face's own camera, and draw the buckets into the view's targets.
	 * The water reflection draws six cube faces per update. The shape always has all six, so recordings hold across updates and
	 * a face the update skips just culls nothing.
	 * First person draws a depth epoch into the main depth and a colour epoch into kMAIN and the motion vectors, both against one
	 * shape and with depth range [0, f1stPersonFarDepthRange].
	 */
	constexpr std::uint32_t kMaxViewFaces = 6;
	// A face's VS_PerFrame (b12, VS and PS) as the engine writes it for the view's camera: at most a D3D11 block's worth.
	constexpr std::uint32_t kViewBlockBytes = 1024;
	static_assert(kViewBlockBytes % 256 == 0);  // pushed per face, 16-byte aligned for the descriptor-buffer model's pointer loads

	/** @brief What tells the views apart. */
	struct ViewTraits
	{
		ViewKind kind;
		std::uint32_t faces;
		bool trees;  // draw families drawn per face, such as tree LOD
		bool forwardPart;  // a forward range, sorted by the depth epoch and drawn after the buckets by the colour epoch
		bool depthPass;  // depth epoch draws depth pipelines into the main depth, colour epoch then the colour ones
		std::uint32_t viewBits;
		std::array<std::uint32_t, 2> epochs;  // depth or only epoch, then colour, kNoEpoch for none
		RenderGraphRuntime::Segment segments[2];
		const char* name;    // for the log
		const char* prefix;  // prefix for its graph resources, passes and extension
		const char* blocks;  // its blocks' resource name
		const char* target;  // its target's resource name
		struct Labels
		{
			const char *map, *tables, *latches, *blocks, *trees;
		} labels;
		bool frustumCull;  // false draws every member untested
	};
	const ViewTraits& TraitsOf(ViewKind a_kind);

	/** @brief Whether a view uses its own frame constants instead of the main resources'. Views without a depth pass do. */
	inline bool OwnsFrameConstants(const ViewTraits& a_traits) { return !a_traits.depthPass; }

	/** @brief A view's latch block layout per run. The revision producer grows frameBytes to what the frame values take. */
	struct ViewLatchLayout
	{
		std::uint32_t faces = 1;
		std::uint32_t slots = 0;    // pipeline slots the map holds a word for
		std::uint32_t buckets = 0;  // entries a face's table holds
		std::uint32_t faceBytes = kViewBlockBytes;
		std::uint32_t frameBytes = 0;  // none when drawing with the main frame constants
		std::uint32_t classMapWords = 0;  // none without a forward part
		static ViewLatchLayout Initial(const ViewTraits& a_traits) { return { a_traits.faces, 64, 4, a_traits.trees ? Extensions::DrawFamilyFaceBytes() : kViewBlockBytes }; }
		std::uint32_t MapOffset() const { return faces * static_cast<std::uint32_t>(sizeof(BuildDrawsLatch)); }
		std::uint32_t TableOffset(std::uint32_t a_face) const { return MapOffset() + slots * 4 + a_face * buckets * 8; }
		// [set indices, 0, 0, 0] then a word per set index, 16-byte aligned because the table ends on 8.
		std::uint32_t ClassMapOffset() const { return (TableOffset(faces) + 15u) & ~15u; }
		std::uint32_t FaceOffset(std::uint32_t a_face) const
		{
			return ((ClassMapOffset() + (kClassHeaderWords + classMapWords) * 4 + 255u) & ~255u) + a_face * faceBytes;
		}
		std::uint32_t FrameOffset() const { return (FaceOffset(faces) + 255u) & ~255u; }
		std::uint32_t Bytes() const { return frameBytes ? FrameOffset() + frameBytes : FaceOffset(faces); }
		bool operator==(const ViewLatchLayout&) const = default;
	};

	/** @brief What a view's epoch passes record against. Per-frame values live in its latch. */
	struct ViewFrame
	{
		ViewKind kind = ViewKind::Reflection;
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};  // the version holds these
		std::shared_ptr<const org::LatchBlock> latch;
		std::uint32_t width = 0, height = 0;  // a face's render area
		float depthRange = 1.0f;              // the viewport's maximum depth
		std::uint32_t sequenceDraws = 0;  // a face's range of the sequences
		// The pipeline slots that share a pipeline pair, and their range of a face's sequences.
		struct Bucket
		{
			rhi::PipelineHandle depth{}, colour{};
			std::uint32_t first = 0, capacity = 0;
			bool operator==(const Bucket& o) const
			{
				return SameHandle(depth, o.depth) && SameHandle(colour, o.colour) && first == o.first && capacity == o.capacity;
			}
		};
		std::vector<Bucket> buckets;
		// Forward range per state class. Staging slots first, the sorted half after. A class without forward slots has capacity 0 and no call.
		std::vector<SetClassRanges::Range> forwardRanges;
		std::uint32_t forwardEntry = 0;
		// Per face, the frame push data with VS and PS b12 pointing at the face's block.
		std::vector<std::array<std::uint32_t, kFramePushWords>> push;
		// Per draw family, its per-face forward pipeline and signature, and the cull's groups.
		struct Faces
		{
			rhi::PipelineHandle pipeline{};
			rhi::CommandSignatureHandle signature{};
			std::uint32_t groups = 0;
			bool operator==(const Faces& o) const { return SameHandle(pipeline, o.pipeline) && SameHandle(signature, o.signature) && groups == o.groups; }
		};
		std::array<Faces, Extensions::kDrawFamilies> faces;
		// Latched copies. Unwritten copies are zeroed apart from the faces' values.
		static constexpr std::size_t kFrameList = 2;
		std::array<LatchedList, 3> latched;
		// Not compared, because the latch block and the buckets imply them.
		ViewLatchLayout latchLayout;
		std::shared_ptr<const std::vector<std::uint32_t>> map;

		bool SameShape(const ViewFrame& o) const
		{
			return latch == o.latch && latched == o.latched && SameHandle(resourceHeap, o.resourceHeap) && SameHandle(samplerHeap, o.samplerHeap) &&
			       SameIndirect(indirect, o.indirect) && width == o.width && height == o.height && depthRange == o.depthRange &&
			       sequenceDraws == o.sequenceDraws && buckets == o.buckets && push == o.push && forwardRanges == o.forwardRanges &&
			       forwardEntry == o.forwardEntry && faces == o.faces;
		}
	};

	/** @brief A view's graph resources. The revision producer grows its buffers to hold the view's plan. */
	struct ViewResources
	{
		const ViewTraits* traits = nullptr;
		std::shared_ptr<Resources> main;
		Versioned sequences;
		std::shared_ptr<org::Buffer> count;
		std::array<Versioned, kMaxViewFaces> bucketCounts;
		std::shared_ptr<org::Buffer> blocks;
		std::uint64_t blocksAddress = 0;
		// Laid out like the main frame slots and written by its commit from this frame's sources. Null for first person, which uses the main resources'.
		std::shared_ptr<org::Buffer> frameConstants;
		std::uint64_t frameConstantsAddress = 0;
		RenderGraph::BufferRef frameConstantsRef;
		// A table of the pass-wide blocks per face, each face's VS and PS b12 its own camera block (FrameBlockViews::Create).
		std::shared_ptr<const FrameBlockViews> frameBlocks;
		bool sharedLightZeroed = false;
		// Copies of the main pass's frame buffers, filled by the commit from CPU mirrors. Replaced whole because the revision producer reads them.
		std::atomic<std::shared_ptr<const std::vector<FrameBuffer>>> frameBuffers;
		// Reflection: DCLF's depth, cleared per face. First person: kPOST_ZPREPASS_COPY, which FUN_1414cb6d0 binds as its pass's depth.
		std::shared_ptr<org::PixelBuffer> depth;
		FrameAnchors::Texture target;
		ID3D11Texture2D* targetTexture = nullptr;
		std::uint32_t width = 0, height = 0;
		// Per family and face, a row and a visible list for the family's units.
		std::array<std::array<std::shared_ptr<org::Buffer>, kMaxViewFaces>, Extensions::kDrawFamilies> faceRows;
		std::array<std::array<Versioned, kMaxViewFaces>, Extensions::kDrawFamilies> faceLists;
		std::array<std::array<RenderGraph::BufferRef, kMaxViewFaces>, Extensions::kDrawFamilies> faceRowsRef{};
		// Passes are redeclared only at the view's own import, never mid-frame.
		bool rebuildPending = false;
		std::shared_ptr<const ComputeProgram> buildDraws;
		rhi::CommandSignaturePtr dispatchSignature;
	};

	struct BuildState;
	struct ViewBuild;
	struct SceneBuffers;

	/** @brief Makes view a_kind's graph extension. Null when the view is not made. */
	std::unique_ptr<org::RenderGraph::IRenderGraphExtension> MakeViewExtension(std::shared_ptr<const BuildState> a_build, ViewKind a_kind);
	/** @brief The view epochs' passes. */
	std::span<const Api::PassDef> ViewPassDefs();
	/** @brief At graph build, whether a view's passes can be added, in its own epochs or hosted by a main epoch. */
	bool ViewPassesReady(const ViewBuild& a_view, const Resources& a_main);
	/** @brief The targets a view's latched copies write. Declared by its own epoch or by the main epoch hosting it. */
	std::vector<LatchedTarget> ViewLatchedTargets(const ViewResources& a_view, const SceneBuffers& a_scene);

	/** @brief A view's buckets, each slot's bucket, and the forward range after them. */
	struct ViewPlan
	{
		std::vector<std::uint32_t> map;
		std::vector<ViewFrame::Bucket> buckets;
		std::uint32_t draws = 0;  // per face, all buckets and the forward range
		std::vector<SetClassRanges::Range> forwardRanges;
	};
	/** @brief A pipeline slot's Z-prepass group, or kNoBucket. */
	std::uint32_t ZGroupOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot);
	/** @brief A pipeline slot's state class word, or kNoSetClass. */
	std::uint32_t SetClassOf(const Lookups* a_lookups, const IndirectState& a_indirect, std::uint32_t a_slot);
	void PlanViewBuckets(const ViewTraits& a_traits, const MainLayout& a_main, const Lookups* a_lookups, const IndirectState& a_indirect,
		std::span<const rhi::PipelineHandle> a_slotPipelines, std::span<const std::uint8_t> a_forwardSlots, ViewPlan& a_out);

	/** @brief A view's latch, its layout and the zeros its latched copies take. */
	struct ViewLatches
	{
		std::shared_ptr<const org::LatchBlock> latch, zeros;
		ViewLatchLayout layout;
	};

	/** @brief What a view's shape is made from. */
	struct ViewShapeInputs
	{
		rhi::DescriptorHeapHandle resourceHeap{}, samplerHeap{};
		IndirectState indirect{};
		ViewPlan plan;
		std::uint32_t sequenceDraws = 0;  // at least plan.draws
		const ViewLatches* latches = nullptr;
		std::uint64_t frameConstantsAddress = 0;
		RenderGraph::BufferRef frameConstantsRef;
		std::shared_ptr<const FrameBlockViews> frameBlocks;  // the frame constants' block tables
		std::vector<LatchedCopy> frameCopies;
		std::uint32_t width = 0, height = 0;
		float depthRange = 1.0f;
		// Per draw family, its forward pipeline, signature and list units. An invalid pipeline means none.
		struct Faces
		{
			rhi::PipelineHandle pipeline{};
			rhi::CommandSignatureHandle signature{};
			std::uint32_t units = 0;
		};
		std::array<Faces, Extensions::kDrawFamilies> faces;
	};
	std::shared_ptr<ViewFrame> MakeViewShape(const ViewResources& a_resources, const ViewShapeInputs& a_in);

	/**
	 * @brief The view's forward key for a pipeline slot, false for a slot it does not draw. Faces cull front faces, first person
	 * culls back faces, unless two-sided.
	 */
	bool ViewForwardKey(ViewKind a_kind, const SceneTables& a_tables, std::size_t a_slot, ForwardPipelineKey& a_key);
	/** @brief Per pipeline slot, its forward pipeline for a_targets. None while a_on is false. */
	void ViewPipelinesOf(ViewKind a_kind, const PipelineCatalog* a_catalog, const SceneTables& a_tables, const ForwardTargets& a_targets, bool a_on,
		std::vector<rhi::PipelineHandle>& a_slots);
	/** @brief First person's forward range. Marks the slots with a RasterForward key, excluding refraction. None while a_on is false. */
	void ViewForwardSlotsOf(const SceneTables& a_tables, bool a_on, std::vector<std::uint8_t>& a_slots);

	struct PreviousCamera
	{
		std::array<float, 16> viewProjUnjittered{};
		RE::NiPoint3 eye;
	};
	/**
	 * @brief Builds VS_PerFrame (b12) for a camera as the engine's SetCameraData does (Snapshot::FrameCamera::WriteCamera), with the
	 * render frame's jitter when a_jitter. Without a_previous, c16 and c41 are zero, as for a reflection face.
	 */
	void PerFrameBlock(const ShadowCameras::Camera& a_camera, std::span<const std::byte> a_frame, bool a_jitter, const PreviousCamera* a_previous, std::byte* a_out);
	/** @brief The renderer's last VS_PerFrame upload, from the mirror or from Community Shaders' copy until the mirror sees a write. */
	std::span<const std::byte> FrameBlockNow();

	/** @brief A trees view commit's rows per draw family, empty for a family without faces. */
	using ViewFamilyRows = std::array<std::span<const std::byte>, Extensions::kDrawFamilies>;

	/** @brief A face's values for a view's commit. */
	struct ViewFace
	{
		bool drawn = false;
		std::span<const std::byte> block;  // written when not empty
		std::array<float, 16> viewProj{};
		RE::NiPoint3 eye;
	};

	/** @brief What a view's revision shapes read from the frame. */
	struct ViewInputs
	{
		std::shared_ptr<ViewResources> resources;
		ForwardTargets targets;
		std::uint32_t width = 0, height = 0;
		float depthRange = 1.0f;
		bool operator==(const ViewInputs&) const = default;
	};
}

namespace DCLF
{
	/**
	 * @brief A view's frame state, render thread. The scene lane reads only LaneTargets and forwardCompatible, never `targets`.
	 * `covered` is decided at frame start, before the engine draws the view.
	 */
	struct ViewEpochState
	{
		ForwardTargets targets;
		std::atomic<std::uint64_t> laneColours{ 0 }, laneDepth{ 0 };  // colour in the low 32 bits, colour2 in the high
		void PublishTargets()
		{
			const std::uint64_t colours = std::uint64_t(static_cast<std::uint32_t>(targets.colour)) | (std::uint64_t(static_cast<std::uint32_t>(targets.colour2)) << 32);
			if (laneColours.load(std::memory_order_relaxed) != colours)
				laneColours.store(colours, std::memory_order_release);
			if (laneDepth.load(std::memory_order_relaxed) != static_cast<std::uint64_t>(targets.depth))
				laneDepth.store(static_cast<std::uint64_t>(targets.depth), std::memory_order_release);
		}
		ForwardTargets LaneTargets() const
		{
			const std::uint64_t colours = laneColours.load(std::memory_order_acquire);
			ForwardTargets out;
			out.colour = static_cast<DXGI_FORMAT>(colours & 0xFFFFFFFFu);
			out.colour2 = static_cast<DXGI_FORMAT>(colours >> 32);
			out.depth = static_cast<DXGI_FORMAT>(laneDepth.load(std::memory_order_acquire));
			return out;
		}
		std::vector<rhi::PipelineHandle> slotPipelines;
		std::shared_ptr<Draws::ViewResources> resources;
		bool setupFailed = false;
		// Until the first coverage decision, coveredInitially stands in.
		bool covered = false;
		bool coverageDecided = false;
		// The engine's viewport depth range for the view, a revision input. 0 until the view's work sets one.
		float depthRange = 0.0f;
		// Whether the main forward pipelines can draw into the view's forward part, because its depth format matches the main pass's.
		std::atomic<bool> forwardCompatible{ false };
		// Per draw family of a trees view, its forward pipeline and row buffers, sized once.
		struct FamilyFaces
		{
			rhi::PipelineHandle pipeline{};
			bool ready = false;
			std::vector<std::byte> row, faceRow, faceHeader;
		};
		std::array<FamilyFaces, Extensions::kDrawFamilies> faces;
		// Per family, whether the faces' draws are DCLF's this frame.
		std::array<bool, Extensions::kDrawFamilies> facesOwned{};
		// Frame textures (t16 up) as its last commit resolved them. Each binding is kept while its register names the same view.
		std::array<std::uint32_t, kTextureRegisters> frameTextures{};
		std::array<ID3D11ShaderResourceView*, kTextureRegisters> frameTextureViews{};
		std::array<GpuTextures::Binding, kTextureRegisters> frameTextureBindings{};
		std::uint32_t frameTextureGeneration = ~0u;
		std::array<std::uint64_t, 2> nullTextures{};
	};
}
