#pragma once

// Per-object helpers shared by the builds on DCLF's pool and the commits on the render thread.

namespace DCLF::Draws
{
	/** @brief Visits the geometry slots an object draws, in BuildDrawsCS's order. A skin follows nextPartition for each partition in its mask. */
	template <class F>
	void ForEachDrawnGeometry(const SceneTables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw);

	/** @brief What the buffer versions a_sealed names can hold. The payload holds all its records, so those parts are unbounded. */
	inline SceneFit SceneFitOf(const NamedVersions& a_sealed, const SceneBuffers& a_scene, std::uint64_t a_objectBuffers)
	{
		SceneFit fit;
		fit.objects = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_objectBuffers, UINT32_MAX));
		Extensions::FitSceneResources(a_sealed, a_scene.sceneResources, fit);
		return fit;
	}

	/**
	 * @brief Whether a_object's rows fit in the scene's buffers.
	 * An object past them waits for growth, so no draw or culling test reads past a buffer.
	 */
	inline bool ObjectFits(const SceneTables& a_tables, std::uint32_t a_object, const SceneFit& a_fit)
	{
		if (a_object >= a_fit.objects)
			return false;
		const auto families = a_tables.families.All();
		for (std::size_t f = 0; f < families.size(); ++f)
			if (!families[f].RowFits(a_object, a_fit.familyRows[f]))
				return false;
		if (a_object < a_tables.streamRow.size() && a_tables.streamRow[a_object] != kNoStreamRow) {
			const std::uint32_t row = a_tables.streamRow[a_object];
			if (row < a_tables.streamRows.size() && std::uint64_t(a_tables.streamRows[row].first) + a_tables.streamRows[row].count > a_fit.streamVertices)
				return false;
		}
		return true;
	}

	template <class F>
	void ForEachDrawnGeometry(const SceneTables& a_tables, std::uint32_t a_firstSlot, std::uint32_t a_partitions, F&& a_draw)
	{
		if (a_partitions == kNoPartitions)
			return;
		std::uint32_t slot = a_firstSlot;
		if (a_partitions & kPartitionChain) {
			for (std::uint32_t i = 0; i < (a_partitions & kPartitionChainCount) && slot < a_tables.geometries.size(); ++i) {
				a_draw(slot);
				slot = a_tables.geometries[slot].nextPartition;
			}
			return;
		}
		for (std::uint32_t i = 0; i < kMaxSkinPartitions && slot < a_tables.geometries.size(); ++i) {
			if (a_partitions == 0 || ((a_partitions >> i) & 1))
				a_draw(slot);
			if ((a_partitions >> (i + 1)) == 0)
				break;
			slot = a_tables.geometries[slot].nextPartition;
		}
	}

	inline std::uint32_t PartitionsOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		return a_object < a_tables.skinPartitions.size() ? a_tables.skinPartitions[a_object] : 0u;
	}

	/** @brief A LOD skin's partitions per LOD level, 0 for any other object. */
	inline std::uint32_t LodPartitionsOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		return a_object < a_tables.skinLodPartitions.size() ? a_tables.skinLodPartitions[a_object] : 0u;
	}

	/** @brief Each object's root slot. Empty without a producer. Fetch once per build or commit, not per object. */
	inline std::span<const std::uint32_t> ObjectRootsOf(const SceneTables& a_tables)
	{
		const auto* family = RootFamilyOf(a_tables);
		return family ? std::span<const std::uint32_t>(family->objectRow) : std::span<const std::uint32_t>();
	}

	/** @brief An object's fade root slot, ~0u for none. A shadow caster is dropped while a stood-in root fades. */
	inline std::uint32_t FadeRootOf(const SceneTables& a_tables, std::uint32_t a_object)
	{
		const auto* family = RootFamilyOf(a_tables);
		return family ? family->RowOf(a_object) : ~0u;
	}

	/** @brief Sets a main input's fade row. The slot and distance change only with its membership and bindings. */
	inline void SetFadeRow(DrawInput& a_input, std::span<const std::uint32_t> a_objectRoots, const SceneTables& a_tables, std::size_t a_object)
	{
		if (a_object < a_objectRoots.size())
			a_input.fadeRoot = a_objectRoots[a_object];
		a_input.lodPartitions = LodPartitionsOf(a_tables, static_cast<std::uint32_t>(a_object));
		if (!(a_input.flags & (kObjectFadeTest | kObjectHeightTest)) || a_object >= a_tables.fadeDistance.size() || a_object >= a_tables.hasFadeNode.size())
			return;
		if (!a_tables.hasFadeNode[a_object])
			return;  // no fade node: nothing to measure
		a_input.fadeDistance = a_tables.fadeDistance[a_object];
	}

	// Sets a draw template's geometry. The second stream repeats the first unless the draw has its own, such as face positions.
	inline void SetSequenceGeometry(DrawSequence& a_sequence, const GeometryBuffers& a_geometry, bool a_ownStream = false)
	{
		if (!a_ownStream) {
			a_sequence.streamBufferRef = a_geometry.vertexRef;
			a_sequence.streamBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
			a_sequence.streamStride = a_geometry.vertexStride;
		}
		a_sequence.vertexBufferRef = a_geometry.vertexRef;
		a_sequence.vertexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.vertexBytes, UINT32_MAX));
		a_sequence.vertexStride = a_geometry.vertexStride;
		a_sequence.indexBufferAddress = a_geometry.indexAddress;
		a_sequence.indexBufferSize = static_cast<std::uint32_t>(std::min<std::uint64_t>(a_geometry.indexBytes, UINT32_MAX));
		a_sequence.indexCount = a_geometry.indexCount;
		a_sequence.firstIndex = a_geometry.firstIndex;
	}

	inline GeometryDraw PackGeometryDraw(const SceneTables& a_tables, std::uint32_t a_slot, std::size_t a_count)
	{
		const auto& geometry = a_tables.geometries[a_slot];
		return { geometry.vertexRef, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.vertexBytes, UINT32_MAX)), geometry.vertexStride,
			geometry.indexAddress, static_cast<std::uint32_t>(std::min<std::uint64_t>(geometry.indexBytes, UINT32_MAX)), geometry.indexCount, geometry.firstIndex,
			geometry.nextPartition < a_count ? geometry.nextPartition : kNoPartition };
	}

	/** @brief Updates the geometry slots' draws from the store's log, or packs them all without a store. */
	void UpdateGeometryDraws(GeometryStore* a_store, std::uint64_t a_oldestContentEpoch, const SceneTables& a_tables, std::uint32_t a_generation,
		std::uint32_t a_frame, GeometryDrawsOut& a_out);

	/** @brief Emits the geometry slots a buffer at a_contentEpoch lacks, then the stream rows. Returns the bytes sent. */
	template <class Emit>
	std::size_t EmitGeometryDraws(const GeometryDrawsOut& a_out, std::uint64_t a_contentEpoch, Emit&& a_emit, std::size_t a_rows = ~std::size_t(0))
	{
		// Rows past the buffer wait for the growth requirement.
		std::size_t bytes = a_out.slots.Emit(a_contentEpoch, a_emit, a_rows);
		if (!a_out.streamRows.empty() && a_out.SlotCount() < a_rows) {
			const std::size_t rows = std::min(a_out.streamRows.size(), a_rows - a_out.SlotCount());
			a_emit(a_out.streamRows.data(), rows * sizeof(GeometryDraw), a_out.SlotCount() * sizeof(GeometryDraw));
			bytes += rows * sizeof(GeometryDraw);
		}
		return bytes;
	}

	// ---- The vertex stream, used for face positions. Each row gets a GeometryDraw after the geometry slots,
	// whose vertex buffer view is the row's range of the stream buffer.

	// BSGraphics::VertexDesc: the position attribute's flag in the second stream (bit 54 + attribute 0).
	constexpr std::uint64_t kPositionInSecondStream = 1ull << 54;

	// Object a_object's stream row GeometryDraw, or ~0u when there is none. Indices follow the geometry slots.
	inline std::uint32_t StreamGeometry(const SceneTables& a_tables, std::size_t a_object, RenderGraph::BufferRef a_vertices)
	{
		if (!a_vertices || a_object >= a_tables.streamRow.size() || a_tables.streamRow[a_object] == kNoStreamRow)
			return ~0u;
		return static_cast<std::uint32_t>(a_tables.geometries.size() + a_tables.streamRow[a_object]);
	}

	inline bool IsStreamObject(const SceneTables& a_tables, std::size_t a_object)
	{
		return a_object < a_tables.streamRow.size() && a_tables.streamRow[a_object] != kNoStreamRow;
	}

	// Call after PackGeometryDraws. Appends the stream rows in streamRows order.
	inline void AppendStreamRows(const SceneTables& a_tables, RenderGraph::BufferRef a_vertices, GeometryDrawsOut& a_geometries)
	{
		if (!a_vertices)
			return;
		auto& a_out = a_geometries.streamRows;
		for (const auto& row : a_tables.streamRows) {
			GeometryDraw draw{};
			if (row.object == Api::VertexStreamRow::kNoObject) {
				draw.nextPartition = kNoPartition;
				a_out.push_back(draw);
				continue;
			}
			draw.vertexBufferRef = a_vertices + std::uint64_t(row.first) * kStreamVertexBytes;
			draw.vertexBufferSize = row.count * kStreamVertexBytes;
			draw.vertexStride = kStreamVertexBytes;
			draw.nextPartition = kNoPartition;
			a_out.push_back(draw);
		}
	}

	inline void SetSequenceStream(DrawSequence& a_sequence, const SceneTables& a_tables, std::size_t a_object, RenderGraph::BufferRef a_vertices)
	{
		const auto& row = a_tables.streamRows[a_tables.streamRow[a_object]];
		a_sequence.streamBufferRef = a_vertices + std::uint64_t(row.first) * kStreamVertexBytes;
		a_sequence.streamBufferSize = row.count * kStreamVertexBytes;
		a_sequence.streamStride = kStreamVertexBytes;
	}
}
