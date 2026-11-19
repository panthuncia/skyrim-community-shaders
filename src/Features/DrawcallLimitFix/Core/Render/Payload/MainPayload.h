#pragma once

// The main payload a build makes for a main epoch. DCLF's pool writes it and the render thread's commit reads it.

#include "Features/DrawcallLimitFix/Core/Extensions/PayloadProducts.h"
#include "Features/DrawcallLimitFix/Core/Render/Payload/MainRows.h"

#include <span>

namespace DCLF::Draws
{
	/**
	 * @brief Emits the rows a table at a_contentEpoch lacks, as they are. Rows hold no addresses, so any backing takes the same bytes.
	 * Rows past a_capacity wait for the table to grow. Returns the rows sent.
	 */
	template <class Row, class Emit>
	std::size_t EmitMainRows(const KeptView<Row>& a_rows, std::uint64_t a_contentEpoch, std::uint64_t a_capacity, Emit&& a_emit)
	{
		return a_rows.Emit(a_contentEpoch, a_emit, static_cast<std::size_t>(a_capacity)) / sizeof(Row);
	}
	/**
	 * @brief A material row is journalled in four 256-byte parts.
	 * Engine controllers move an animated material every frame, and only the part that moved is resent.
	 */
	inline constexpr std::size_t kMaterialRowParts = 4;
	static_assert(kMaterialRowBytes % kMaterialRowParts == 0 && kMaterialRowVS % (kMaterialRowBytes / kMaterialRowParts) == 0 &&
				  kMaterialRowPS % (kMaterialRowBytes / kMaterialRowParts) == 0 && kMaterialRowHeader % (kMaterialRowBytes / kMaterialRowParts) == 0);

	/** @brief EmitMainRows for rows journalled by part. Adjacent parts go as one span. Returns the parts sent. */
	template <class Row, class Emit>
	std::size_t EmitMainRowParts(const KeptView<Row>& a_rows, std::size_t a_parts, std::uint64_t a_contentEpoch, std::uint64_t a_capacity, Emit&& a_emit)
	{
		if (!a_rows.elements)
			return 0;
		const auto& rows = *a_rows.elements;
		const std::size_t partBytes = sizeof(Row) / a_parts;
		const std::uint64_t capacity = a_capacity * a_parts;
		const std::byte* pending = nullptr;
		std::size_t pendingBytes = 0, pendingOffset = 0, sent = 0;
		const auto flush = [&] {
			if (pendingBytes)
				a_emit(static_cast<const void*>(pending), pendingBytes, pendingOffset);
			pendingBytes = 0;
		};
		a_rows.changes.ForEachRun(a_contentEpoch, rows.size() * a_parts, [&](std::uint64_t a_first, std::uint64_t a_count) {
			if (a_first >= capacity)
				return;
			a_count = std::min(a_count, capacity - a_first);
			for (std::uint64_t part = a_first; part < a_first + a_count;) {
				const std::uint64_t r = part / a_parts;
				const std::uint64_t partsEnd = std::min(a_first + a_count, (r + 1) * a_parts);
				const std::byte* data = reinterpret_cast<const std::byte*>(&rows[static_cast<std::size_t>(r)]) + (part % a_parts) * partBytes;
				const std::size_t bytes = static_cast<std::size_t>(partsEnd - part) * partBytes, offset = static_cast<std::size_t>(part * partBytes);
				if (pendingBytes && pending + pendingBytes == data && pendingOffset + pendingBytes == offset) {
					pendingBytes += bytes;
				} else {
					flush();
					pending = data;
					pendingBytes = bytes;
					pendingOffset = offset;
				}
				sent += static_cast<std::size_t>(partsEnd - part);
				part = partsEnd;
			}
		});
		flush();
		return sent;
	}
	/** @brief An input's rows. The pipeline slot is in the high 16 bits and the material slot in the low 16.
	 * A large list passes 4096 pipeline slots, so 16 bits are needed. */
	constexpr std::uint32_t kRowPipelineShift = 16;
	constexpr std::uint32_t kRowMaterialMask = (1u << kRowPipelineShift) - 1;
	constexpr std::uint32_t RowsOf(std::uint32_t a_pipeline, std::uint32_t a_material) { return (a_pipeline << kRowPipelineShift) | a_material; }

	struct MainInputs
	{
		std::uint32_t frameNumber = 0;
		bool depthOnly = false;
		std::uint32_t vsFrameMask = 0, psFrameMask = 0;  // the frame slots the commit supplies
		std::array<std::uint32_t, kDecalGroups> decalCount{};
		ResourceAddresses addresses{};
		std::uint32_t lookupGeneration = 0, tablesGeneration = 0;
		// The oldest content epoch any backing of the payload's pools holds. The journals keep changes back to it. ~0 means none.
		std::uint64_t inputsOldestContentEpoch = 0;
		TablesContentEpochs tablesOldestContentEpochs;
		std::uint64_t materialRowsOldestContentEpoch = 0, pipelineRowsOldestContentEpoch = 0;
	};

	/**
	 * @brief What one main build makes for both segments. Immutable once Finish hands it over.
	 * Pooled by the build cache, which reuses a body no payload holds.
	 */
	struct MainBody
	{
		std::vector<std::shared_ptr<const void>> bindingOwners;  // exact roots used by this payload, including clean kept rows
		std::vector<DrawSequence> sequences;  // CPU templates of BuildDraws' output
		std::array<std::vector<DrawSequence>, kDecalGroups> decalTemplates;  // by group and slot
		std::vector<DrawInput> inputList;
		GeometryDrawsOut geometryDraws;
		// The vertex stream rows. This points into the publication's tables, which the draws holding this payload keep alive.
		std::span<const Api::VertexStreamRow> streamRows;
		KeptView<MaterialRow> materialRows;
		KeptView<PipelineRow> pipelineRows;
		Extensions::PayloadProducts products;
		std::shared_ptr<const StreamViews> streams;
		// What this build did with each object. Set parity uses kObjectStateAbsent to explain mismatches.
		std::vector<std::uint8_t> objectState;
		std::array<std::uint32_t, kDecalGroups> decalCount{};
		// Merged into IndirectDraws::Stats by the commit.
		std::array<std::uint32_t, static_cast<std::size_t>(IndirectDraws::Skip::Count)> skipped{};
		std::array<std::uint32_t, 4> missingTextures{};
		std::uint32_t missingNext = 0;
		std::uint32_t missingVertexConstants = 0, missingPixelConstants = 0;
		std::uint32_t decalsDrawn = 0, shortBuffers = 0, deferredTextures = 0;
		// The first draw past its buffers, logged by the commit with the geometry's name.
		struct ShortBuffer
		{
			std::uint32_t object = ~0u;
			std::uint64_t vertexNeeded = 0, indexNeeded = 0;
		} shortBuffer;
		std::uint32_t residentDraws = 0, residentPairs = 0, residentUndrawable = 0;

		void Reset()
		{
			residentDraws = residentPairs = residentUndrawable = 0;
			bindingOwners.clear();
			sequences.clear();
			inputList.clear();
			geometryDraws.Reset();
			streams.reset();
			streamRows = {};
			objectState.clear();
			materialRows.Reset();
			pipelineRows.Reset();
			for (auto& product : products)
				product.reset();
			for (auto& templates : decalTemplates)
				templates.clear();
			decalCount = {};
			skipped = {};
			missingTextures = {};
			missingNext = 0;
			missingVertexConstants = missingPixelConstants = 0;
			decalsDrawn = shortBuffers = deferredTextures = 0;
			shortBuffer = {};
		}
	};
	inline const std::shared_ptr<const MainBody>& EmptyMainBody()
	{
		static const std::shared_ptr<const MainBody> kEmpty = std::make_shared<MainBody>();
		return kEmpty;
	}

	/** @brief One segment's payload. Both segments share the build's body. */
	struct MainPayload
	{
		MainInputs inputs;
		std::shared_ptr<const MainBody> body = EmptyMainBody();  // never null
		// Frame textures from t16 up, which only the commit can resolve. The depth segment resolves none.
		std::array<std::uint64_t, 2> frameRegisters{};
		// The culling ViewProj with the eye folded in, used by set parity to test live bounds.
		std::array<float, 16> cullViewProj{};
		bool culled = false;
		// Only the primary segment carries these. The other's are zero.
		std::uint64_t materialRowsWritten = 0, pipelineRowsWritten = 0;
		std::uint32_t rowTableConflicts = 0;  // pairs whose pipeline's constant tables are not the ones its material row was packed with
		std::array<double, 7> partMs{};
		// The resident region's inputs, which lead the payload's scene list. The body's inputList follows them.
		KeptView<DrawInput> resident;
		std::uint32_t residentResyncs = 0;
		std::uint32_t residentResyncReasons = 0;  // bit k: kResyncReasons' k
		std::uint32_t residentDecalRetakes = 0;   // builds whose decal groups' counts moved (no resync: UpdateRegionEntries)

		void Reset()
		{
			body = EmptyMainBody();
			resident.Reset();
			residentResyncs = residentResyncReasons = residentDecalRetakes = 0;
			frameRegisters = {};
			materialRowsWritten = pipelineRowsWritten = 0;
			rowTableConflicts = 0;
			partMs = {};
		}
	};
}
