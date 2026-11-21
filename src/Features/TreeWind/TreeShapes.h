#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * @brief Finds the DynDOLOD tree LOD shapes and builds their per-vertex bend data.
 *
 * Object LOD merges many trees into one shape with no per-tree origin, so the bend data comes from
 * the CPU copy of the vertices: trunk cards give each tree's base and top, and each crown vertex
 * takes the nearest trunk card of its LOD block. Billboard-only shapes bend each card or piece
 * from its own foot.
 *
 * Tree-mod support covers 3D LOD built differently from DynDOLOD's own models (Happy Little Trees,
 * Nature of the Wild Lands, Ulvenwald): trunk cards mixed with 3D trunks and stumps in one shape,
 * and crowns without rim or soft lighting.
 *
 * The classification and the build read plain values, not engine objects, so Drawcall Limit Fix can run
 * them from its own records and vertex copies. BendWeights is the engine draws' cache of D3D11 buffers.
 */
namespace TreeShapes
{
	enum class Kind
	{
		None,
		Crown,
		Trunk
	};

	/** @brief What the classification reads of a shape and its Lighting property. */
	struct ShapeFacts
	{
		std::uint64_t propertyFlags = 0;  // BSShaderProperty::EShaderPropertyFlag bits
		bool subIndex = false;            // a BSSubIndexTriShape: DynDOLOD's merged object LOD
		bool triShape = false;            // a BSTriShape or one of its subclasses
		bool treeName = false;            // named 'treepassthru'
		std::uint32_t vertexCount = 0, triangleCount = 0;
		float worldScale = 1.0f;
	};

	/** @brief Classifies a shape by its shader flags, geometry type, shape name and layout. */
	Kind Classify(const ShapeFacts& a_shape, bool a_treeModSupport);

	/** @brief Classifies a Lighting or Utility pass. */
	Kind Classify(const RE::BSRenderPass* a_pass, bool a_treeModSupport);

	/** @brief DynDOLOD names the tree LOD shape of an object LOD block 'treepassthru', whatever billboard type made it. */
	bool IsTreeShapeName(const char* a_name);

	/** @brief A block's key from its world translate, which every shape of one LOD block shares. */
	std::uint64_t BlockKey(float a_x, float a_y);

	struct TrunkCard
	{
		float x, y, base, top;
		float random = 0.0f;  // timing offset and flutter choice, shared by a tree's crossed cards
		float scale = 1.0f;   // sway scale from the tree height
	};

	/** @brief The trunk cards of one LOD block, which its crowns follow. */
	struct Block
	{
		std::vector<TrunkCard> cards;
		std::unordered_map<int64_t, std::vector<int>> grid;  // cell -> card indices
		std::unordered_set<const void*> trunkShapes;
		uint32_t generation = 0;  // bumped when cards are added, so tree-mod crowns built before rebuild
	};

	/** @brief A shape's CPU copy: float3 positions at offset 0 of each vertex, and its 16-bit triangle list if it has one. */
	struct ShapeData
	{
		const std::uint8_t* positions = nullptr;
		std::uint32_t stride = 0;
		std::uint32_t vertexCount = 0;
		const std::uint16_t* indices = nullptr;
		std::uint32_t triangleCount = 0;
		float worldScale = 1.0f;
		bool treeName = false;
	};

	enum class Built
	{
		Data,     // the output holds four halves per vertex
		Still,    // nothing in the shape sways
		Waiting,  // a crown whose block has no trunk cards yet
	};

	/**
	 * @brief Builds a shape's bend data: per vertex, bend weight, tree random, tree sway scale and distance from the trunk, as halves.
	 * A trunk adds its cards to a_block once per a_shapeKey, so a block's trunks must be built before its crowns.
	 */
	Built BuildBendData(Block& a_block, const void* a_shapeKey, const ShapeData& a_shape, Kind a_kind, bool a_treeModSupport, std::vector<std::uint16_t>& a_out);

	class BendWeights
	{
	public:
		/**
		 * @brief Returns the bend data for a shape, built on first sight.
		 * @return nullptr when the shape has nothing to sway or its block's trunks have not been drawn yet.
		 */
		ID3D11ShaderResourceView* Get(RE::BSGeometry* a_geometry, Kind a_kind, uint a_frame, bool a_treeModSupport);

		/** @brief Frees the bend data of shapes not drawn for a while, and the blocks left without shapes. */
		void Evict(uint a_frame);

		/** @brief Frees all bend data, so it is rebuilt with new settings. */
		void Clear();

		size_t Count() const { return entries.size(); }

	private:
		struct Entry
		{
			winrt::com_ptr<ID3D11Buffer> buffer;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;  // nullptr for a shape checked once and left still
			uint32_t vertexCount = 0;
			float firstVertex[3]{};
			uint lastFrame = 0;
			uint64_t blockKey = 0;
			uint32_t generation = 0;
			bool crown = false;
		};

		std::unordered_map<uint64_t, Block> blocks;      // key = the block's world translate
		std::unordered_map<const void*, Entry> entries;  // key = the shape's vertex buffer
	};
}
