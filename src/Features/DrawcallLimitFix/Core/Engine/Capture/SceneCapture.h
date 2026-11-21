#pragma once

// The scene's records: engine values the scene work reads, captured by hooks or frame requests and carried into the coordinator's mirror.

#include <array>
#include <d3d11.h>
#include <winrt/base.h>
#include <cstdint>
#include <memory>
#include <variant>
#include <vector>

#include "Features/DrawcallLimitFix/Api/EntryRanges.h"

struct ID3D11Buffer;

namespace DCLF::SceneCapture
{
	/**
	 * @brief Engine scene values as the scene work reads them. The scene work reads no engine memory. The hook that changed a value
	 * captures it on the writer's thread and the event applies it to the mirror in order. The mirror parity uses the same capture
	 * functions, so a field differing with no event names a missed writer.
	 * Only what holds still between writer events is a record. Per-frame values belong to FrameValues. Records hold no references.
	 */

	enum NodeKind : std::uint16_t
	{
		kKindGeometry = 1u << 0,
		kKindNode = 1u << 1,
		kKindSwitch = 1u << 2,
		kKindBillboard = 1u << 3,
		kKindOrdered = 1u << 4,
		kKindMultiBound = 1u << 5,
		kKindFadeNode = 1u << 6,
		kKindFaceGen = 1u << 7,
		kKindTree = 1u << 8,
		kKindCaptured = 1u << 9,  // a node capture contribution's exact RTTI, whose list is captured
		// With kKindBillboard, the face mode (userFlags & 7, +0x128) in bits 13-15.
		kKindBillboardModeShift = 13,
	};
	/** @brief A billboard node record's face mode. */
	inline constexpr std::uint32_t BillboardModeOf(std::uint16_t a_kind) { return (a_kind >> kKindBillboardModeShift) & 7u; }

	struct NodeRecord
	{
		enum Field : std::uint32_t
		{
			kParent = 1u << 0,
			kRtti = 1u << 1,
			kKind = 1u << 2,
			kHidden = 1u << 3,
			kFlags = 1u << 4,  // the still flag bits the scene work reads
			kUserData = 1u << 5,
			kControllers = 1u << 6,
			kBody = 1u << 7,  // a rigid body whose motion is not fixed
			kChildren = 1u << 8,
			kSwitch = 1u << 9,
			kFadeNear = 1u << 10,
			kFadeFar = 1u << 11,
			kFade109 = 1u << 12,
			kFadeType = 1u << 13,
			kTreeLodSwitch = 1u << 14,
			kName = 1u << 15,  // for logs only, not compared
			kExtra = 1u << 16,
			kFadeCurrent = 1u << 17,  // every frame while it fades
			kFadeLevel = 1u << 18,
			kFadeDoor = 1u << 19,
			kFadeAmount = 1u << 20,
			kCaptured = 1u << 21,  // a contributed node's list
			kFieldCount = 22,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "parent", "rtti", "kind", "hidden", "flags", "user data", "controllers",
			"body", "children", "switch", "fade near", "fade far", "fade +0x109", "fade type", "tree LOD switch", "name", "extra", "current fade", "LOD level",
			"screen door", "fade amount", "decals" };

		// Besides kHidden only a geometry's kMeshLOD (12, set by its loader) is recorded. Bits the culls and updates write each frame
		// (14, 15, 22, 26, 27, and 11, 12, 20 on a fade root) are not.
		static constexpr std::uint32_t RecordFlags(std::uint16_t a_kind) { return (a_kind & kKindGeometry) ? (1u << 12) : 0u; }

		const void* key = nullptr;
		const void* parent = nullptr;
		const void* rtti = nullptr;
		std::uint16_t kind = 0;
		std::uint32_t flags = 0;
		const void* userData = nullptr;
		std::uint8_t formType = 0;
		bool actor = false;
		/** @brief Whether the record's reference is an ActorCharacter. */
		bool IsActorCharacter() const { return userData && formType == static_cast<std::uint8_t>(RE::FormType::ActorCharacter); }
		bool controllers = false;
		bool body = false;
		// Children by engine slot, null slots kept: what OnVisible visits, in order.
		std::vector<const void*> children;
		std::int32_t switchIndex = -1;
		std::uint16_t switchFlags = 0;
		bool switchCurrent = false;
		const void* switchChild = nullptr;  // children[switchIndex]
		// What OnVisible visits in the children's place, in the contribution's order.
		std::vector<const void*> captured;
		// bsx is -1 for none. facts holds a bit per Api::ReferenceFactId.
		std::int32_t bsx = -1;
		bool emittance = false;
		std::uint32_t facts = ~0u;
		float currentFade = 1.0f;
		std::uint8_t fadeLevel = 0, fadeDoor = 0;
		float fadeAmount = 1.0f;
		float fadeNear = 0.0f, fadeFar = 0.0f;
		std::uint8_t fade109 = 0, fadeType = 0;
		const void* treeLodSwitch = nullptr;
		const char* name = nullptr;
		// Diagnostics, not compared.
		std::uint32_t capturedFrame = 0, updatedFrame = 0, thread = 0;

		// NextSequence at the capture's start. Not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const NodeRecord& a_other) const;
		/** @brief Copies a_fields of a_from into this record. */
		void Assign(const NodeRecord& a_from, std::uint32_t a_fields);
	};

	struct GeometryRecord
	{
		enum Field : std::uint32_t
		{
			kType = 1u << 0,
			kRenderer = 1u << 1,
			kSkin = 1u << 2,
			kDismember = 1u << 3,
			kProperty = 1u << 4,
			kAlpha = 1u << 5,
			kLayer = 1u << 6,  // the additional property, the alt index buffer and its count
			kExtra = 1u << 7,
			kMultiParams = 1u << 8,
			kRanges = 1u << 9,  // object LOD's segments
			kFieldCount = 10,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "type", "renderer", "skin", "dismember", "property", "alpha", "layer", "extra",
			"multi-index parameters", "segments" };

		struct Partition
		{
			const void* rendererData = nullptr;  // a geometry slot's key
			ID3D11Buffer* vertexBuffer = nullptr;
			ID3D11Buffer* indexBuffer = nullptr;
			std::array<winrt::com_ptr<ID3D11Buffer>, 2> buffersHeld;
			std::uint64_t vertexDesc = 0;
			std::uint16_t vertices = 0, triangles = 0;
			std::uint8_t lodByte = 0;
			bool operator==(const Partition&) const = default;
		};

		const void* key = nullptr;
		std::uint8_t type = 0;
		const void* rendererData = nullptr;
		ID3D11Buffer* vertexBuffer = nullptr;
		ID3D11Buffer* indexBuffer = nullptr;
		// Held before handing their addresses to the scene worker, because a dynamic shape can replace its renderer data while the worker
		// still resolves the previous capture.
		std::array<winrt::com_ptr<ID3D11Buffer>, 2> buffersHeld;
		std::uint64_t vertexDesc = 0;
		// The geometry's own VertexDesc (+0x148). A skinned shape's renderer data is another TriShape or none.
		std::uint64_t shapeVertexDesc = 0;
		std::uint32_t vertexCount = 0, triangleCount = 0;
		const void* skin = nullptr;
		const void* skinRtti = nullptr;
		const void* skinPartition = nullptr;
		const void* skinData = nullptr;
		std::uint32_t boneCount = 0;
		std::vector<Partition> partitions;
		std::vector<std::uint8_t> shown;  // a dismember skin's partitions' editorVisible
		const void* property = nullptr;
		const void* alpha = nullptr;
		const void* layerProperty = nullptr;
		const void* altIndexList = nullptr;  // its first qword is the buffer, the layer's slot key
		ID3D11Buffer* altIndexBuffer = nullptr;
		winrt::com_ptr<ID3D11Buffer> altIndexHeld;
		std::uint32_t altPrimCount = 0;
		static constexpr std::int64_t kNoExtra = -1, kWrongExtra = -2;
		std::int64_t extraValue = kNoExtra;
		// materialProjection (16), materialParams (4), materialScale, normalDampener.
		std::array<float, 22> multiParams{};
		// Compared by triangles, since object LOD's rebuild moves run starts but not triangles.
		std::vector<Api::EntryRange> ranges;
		/** @brief A particle geometry's inputs to PaletteItemClassOf. */
		struct Particles
		{
			bool system = false;  // not NiMeshParticleSystem or BSStripParticleSystem
			bool worldSpace = false;
			bool data = false;
			bool aligned = false;  // velocity-aligned or speed-stretched quads
			bool velocities = false;
			std::uint16_t capacity = 0;
			std::uint32_t subtextures = 0;
			bool operator==(const Particles&) const = default;
		};
		Particles particles;

		// NextSequence at the capture's start. Not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const GeometryRecord& a_other) const;
		void Assign(const GeometryRecord& a_from, std::uint32_t a_fields);
		/** @brief The layer the engine draws: the additional property, if it has an alt index buffer and count. */
		const void* Layer() const { return altIndexBuffer && altPrimCount ? layerProperty : nullptr; }
	};

	struct PropertyRecord
	{
		enum Field : std::uint32_t
		{
			kRtti = 1u << 0,
			kFlags = 1u << 1,
			kMaterial = 1u << 2,
			kMaterialAlpha = 1u << 3,
			kMaterialOther = 1u << 4,
			kFadeNode = 1u << 5,
			kEmissive = 1u << 6,
			kControllers = 1u << 7,
			kAlphaValue = 1u << 8,  // 1 for anything but Lighting
			kProjected = 1u << 9,
			kLandBlend = 1u << 10,
			kShadowPasses = 1u << 11,
			kFieldCount = 12,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "rtti", "flags", "material", "material alpha", "material other", "fade node",
			"emissive", "controllers", "alpha", "projected UV", "land blend", "shadow passes" };

		const void* key = nullptr;
		const void* rtti = nullptr;
		bool lighting = false;
		// A material port's kind, 0 for none.
		std::uint8_t port = 0;
		std::uint64_t flags = 0;
		const void* material = nullptr;
		float materialAlpha = 1.0f;
		std::uint32_t feature = 0;
		std::uint8_t contributorBits = 0;
		const void* diffuseView = nullptr;
		// Held because a texture swap may free the view before the scene work binds it. Not compared.
		winrt::com_ptr<ID3D11ShaderResourceView> diffuseHeld;
		const void* fadeNode = nullptr;
		const void* emissive = nullptr;
		bool controllers = false;
		float alpha = 1.0f;
		std::array<float, 8> projected{};  // a Lighting property's projected UV, or a material port's own values
		std::array<float, 4> landBlend{};
		bool shadowPasses = false;

		// NextSequence at the capture's start. Not compared.
		std::uint64_t sequence = 0;
		// Diagnostics, not compared.
		std::uint32_t capturedFrame = 0, thread = 0, writtenFrame = 0;
		std::uint8_t writer = 0;

		std::uint32_t Differ(const PropertyRecord& a_other) const;
		void Assign(const PropertyRecord& a_from, std::uint32_t a_fields);
	};

	struct AlphaRecord
	{
		enum Field : std::uint32_t
		{
			kFlags = 1u << 0,
			kThreshold = 1u << 1,
			kRtti = 1u << 2,
			kControllers = 1u << 3,
			kFieldCount = 4,
		};
		static constexpr std::array<const char*, kFieldCount> kFieldNames{ "flags", "threshold", "rtti", "controllers" };

		const void* key = nullptr;
		std::uint16_t flags = 0;
		std::uint8_t threshold = 0;
		const void* rtti = nullptr;
		bool controllers = false;

		// NextSequence at the capture's start. Not compared.
		std::uint64_t sequence = 0;

		std::uint32_t Differ(const AlphaRecord& a_other) const;
		void Assign(const AlphaRecord& a_from, std::uint32_t a_fields);
	};

	/** @brief What one capture took: nodes, geometries and the properties they name. */
	struct Records
	{
		std::vector<NodeRecord> nodes;
		std::vector<GeometryRecord> geometries;
		std::vector<PropertyRecord> properties;
		std::vector<AlphaRecord> alphas;
		// An update numbered after this may have written after the capture read the object.
		std::uint64_t sequence = 0;
		// An attach's capture holds every object it recorded, taken while the engine's attach holds them. The scene work's references
		// come only from these, never from a key. Released with the batch on the render thread.
		bool pinning = false;
		std::vector<RE::NiPointer<RE::NiRefObject>> pins;
		bool Empty() const { return nodes.empty(); }
	};

	/** @brief A geometry's records, from the mirror or a live capture. Null where none. */
	struct LeafView
	{
		const NodeRecord* node = nullptr;
		const NodeRecord* parent = nullptr;
		const GeometryRecord* geometry = nullptr;
		const PropertyRecord* property = nullptr;
		const PropertyRecord* layer = nullptr;
		const AlphaRecord* alpha = nullptr;
		const NodeRecord* fadeNode = nullptr;
		const NodeRecord* layerFadeNode = nullptr;
		explicit operator bool() const { return node && geometry; }
		/** @brief The fade node record of the leaf's property or layer. */
		const NodeRecord* FadeNodeOf(const PropertyRecord* a_property) const { return a_property == property ? fadeNode : a_property == layer ? layerFadeNode : nullptr; }
		std::uint8_t Type() const { return geometry ? geometry->type : 0; }
		bool Skinned() const { return geometry && geometry->skin; }
		// NiAlphaProperty::GetAlphaBlending, GetAlphaTesting.
		bool AlphaBlending() const { return alpha && (alpha->flags & 1u); }
		bool AlphaTesting() const { return alpha && (alpha->flags & (1u << 9)); }
		/** @brief The alpha's flags and threshold in one word, 0 without one. */
		std::uint32_t AlphaState() const { return alpha ? (static_cast<std::uint32_t>(alpha->flags) << 8) | alpha->threshold : 0u; }
		/** @brief a_property if a BSLightingShaderProperty, else null. */
		static const PropertyRecord* Lighting(const PropertyRecord* a_property) { return a_property && a_property->lighting ? a_property : nullptr; }
		/** @brief a_property if a material port's, else null. */
		static const PropertyRecord* Port(const PropertyRecord* a_property) { return a_property && a_property->port ? a_property : nullptr; }
	};

	/** @brief A geometry's records captured live and held, for a reader on the render thread. */
	struct LiveLeaf
	{
		explicit LiveLeaf(const RE::BSGeometry& a_geometry);
		LiveLeaf(const LiveLeaf&) = delete;
		LiveLeaf& operator=(const LiveLeaf&) = delete;
		Records records;
		LeafView view;
	};

	/** @brief A hook's values after the write, applied in push order to the mirror's record when one exists. */
	struct Update
	{
		std::uint32_t fields = 0;
		// The dismember setter knows only the skin, so an update may name a geometry by its skin alone.
		std::variant<NodeRecord, PropertyRecord, AlphaRecord, GeometryRecord> record;
		// A property or alpha swap carries the whole leaf, applied as a capture.
		std::shared_ptr<const Records> leaf;
		// NextSequence after the write. An update numbered before a capture's start is in it.
		std::uint64_t sequence = 0;
		// A hidden store's site index, else ~0u.
		std::uint32_t hiddenSite = ~0u;
	};

	/**
	 * @brief Orders captures and updates. A loader-thread capture can take milliseconds while another writer pushes first. The mirror
	 * applies a capture, then the updates numbered after its start, and skips updates numbered before it.
	 */
	std::uint64_t NextSequence();

	NodeRecord CaptureNode(const RE::NiAVObject& a_object);
	/** @brief Only a_fields of a node, for a hook's update. A fade field needs a BSFadeNode. */
	NodeRecord CaptureNodeFields(const RE::NiAVObject& a_object, std::uint32_t a_fields);
	GeometryRecord CaptureGeometry(const RE::BSGeometry& a_geometry);
	PropertyRecord CaptureProperty(const RE::BSShaderProperty& a_property);
	AlphaRecord CaptureAlpha(const RE::NiAlphaProperty& a_alpha);
	/** @brief The geometry's records into a_out. With a_pins its properties are held there. */
	void CaptureLeaf(const RE::BSGeometry& a_geometry, Records& a_out, std::vector<RE::NiPointer<RE::NiRefObject>>* a_pins = nullptr);

	/** @brief Whether a_object reaches Main::WorldRootNode by its parent chain. */
	bool InWorld(const RE::NiAVObject* a_object);
	/**
	 * @brief The parentless roots InWorld also counts as in the world. Written on the render thread and read lock-free by the hooks
	 * inside a SceneTracker::SetRead.
	 */
	void SetDrawnRoots(std::vector<const void*> a_roots);
	/** @brief An attach's capture of a_root's subtree and ancestors. Null when a_root is not in the world yet. */
	std::unique_ptr<Records> CaptureAttached(const RE::NiAVObject& a_root);

	/** @brief Capture counters since the last call. */
	struct Counters
	{
		std::uint64_t attaches = 0, records = 0, ns = 0, mainThread = 0, mainThreadNs = 0, outOfWorld = 0;
	};
	Counters TakeCounters();
	void NoteCapture(std::uint64_t a_records, std::uint64_t a_ns);
	void NoteOutOfWorld();
	/** @brief The render thread, recorded at the frame's start, for the counters. */
	void SetMainThread(std::uint32_t a_thread);

	/**
	 * @brief The skin-partition LOD row a pass draws with (NiSkinPartition::Unk_25, AE 140d43a10): LODMode.index + singleLevel * 4.
	 * A kMeshLOD geometry uses its fade node's LOD level. Everything else uses level 3.
	 */
	std::uint32_t LodRowOf(const RE::BSGeometry& a_geometry, const RE::BSShaderProperty* a_property);
	/** @brief LodRowOf from the records. */
	std::uint32_t LodRowOf(const LeafView& a_leaf, const PropertyRecord* a_property);
	std::uint32_t LodRowOf(const RE::BSRenderPass& a_pass);
}
