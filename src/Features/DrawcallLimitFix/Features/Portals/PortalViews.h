#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <ankerl/unordered_dense.h>

#include "Features/DrawcallLimitFix/Diagnostics/Reports/Schedule.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalContext.h"
#include "Features/DrawcallLimitFix/Features/Portals/PortalProgramModel.h"

namespace RE
{
	class NiAVObject;
	class NiNode;
}

namespace DCLF
{
	/**
	 * @brief A port of the engine's portal and occluder walk for the main camera (FUN_1401a43c0, Traverse FUN_1414a41e0).
	 * Builds one compound-frustum program per visible room and one for unbound space, using DCLF's own copy of the shape scratch.
	 * It writes no engine object and reads nothing the engine's culls wrote. Render thread, right after Main::Update's walk.
	 */
	class PortalViews
	{
	public:
		using PlaneSet = PortalModel::PlaneSet;
		using Op = PortalModel::Op;
		using Program = PortalModel::Program;

		struct Frame
		{
			std::uint32_t sceneFrame = ~0u;
			const void* graph = nullptr;  // the ShadowSceneNode's portal graph (+0x228); null: nothing built
			bool valid = false;
			bool visibleUnbound = false;           // a walk reached unbound space
			Program unbound;                       // the portal-graph entry's own (+0x68): unbound space
			std::vector<const void*> roomKeys;     // the visibility map's rooms, as inserted
			std::vector<Program> rooms;            // by roomKeys
			std::vector<const void*> accumulated;  // rooms the walk popped (multiBoundRoomAccumList), in order
			const void* cameraRoom = nullptr;      // first room holding the camera's near-plane centre
		};

		static PortalViews& Get();

		/** @brief Builds this frame's programs. Render thread, after Main::Update's walk. */
		void Build();
		const Frame& Current() const { return frame; }
		/** @brief Whether this frame's walk reached unbound space. A frame with no graph keeps the last answer, as the engine does. */
		bool VisibleUnbound() const { return visibleUnbound.load(std::memory_order_acquire); }
		/** @brief a_room's program this frame, or null when the room was not reached. */
		const Program* RoomProgram(const void* a_room) const;
		/** @brief Encodes this frame's programs as FadeStateCS and BuildDraws read them (PortalPrograms.h). */
		void Encode(std::vector<std::uint32_t>& a_words) const;
		/** @brief This frame's Encode, made at Build so the GPU copy and the CPU checks read the same words. */
		const std::vector<std::uint32_t>& Encoded() const { return encoded; }
		/** @brief Owned main-view context, independent of mutable walk scratch. Render thread. */
		Portals::ContextHold MainContext() const { return mainContext; }
		/**
		 * @brief PortalPrograms.hlsli's PortalTest on the CPU. Returns 1 in view, 0 culled, -1 no program.
		 * a_nodeFlags bits: always-draw 11, preprocessed 12, preprocess-hidden 20.
		 */
		static int Visible(std::span<const std::uint32_t> a_words, std::uint32_t a_program, const float a_centre[3], float a_radius, std::uint32_t a_nodeFlags,
			std::string& a_why);
		/** @brief The program a node is culled through: 1 + its nearest room's index, 0 for unbound space. Render thread. */
		std::uint32_t ProgramOf(const RE::NiAVObject* a_node) const;
		/** @brief A walked room's program (1 + its room index), else kPortalNoProgram. Render thread. */
		std::uint32_t ProgramOfRoom(const void* a_room) const;
		/** @brief Changes whenever ProgramOf's answers may have changed. */
		std::uint64_t ProgramsVersion() const { return structure.load(std::memory_order_relaxed) * 0x9E3779B97F4A7C15ull ^ graphVersion; }
		/** @brief Notes that a room's, portal shared node's or multibound node's child changed. Any thread. */
		void NoteStructure(const RE::NiNode* a_parent);

		/**
		 * @brief The keys Main::IsRoomVisible (0x140649f20) answers for the main view: walked rooms and held portal shared nodes.
		 * Immutable once published.
		 */
		struct VisibleSet
		{
			/** @brief Per graph: its rooms, and each portal's shared node with the portal's two rooms. */
			struct GraphKeys
			{
				const void* graph = nullptr;
				std::uint64_t signature = 0;  // the room and portal counts it was built from
				ankerl::unordered_dense::set<const void*> rooms;
				ankerl::unordered_dense::map<const void*, std::array<const void*, 2>> shared;  // a null room: unbound space
			};
			struct Keys
			{
				ankerl::unordered_dense::set<const void*> rooms;
				ankerl::unordered_dense::set<const void*> shared;
			};
			enum Kind : std::uint8_t
			{
				kNoKey,  // a null key: true
				kRoom,
				kShared,
				kOther,  // not a room or shared node (or no graph): true, as the engine's
			};
			struct Answer
			{
				bool visible = true;
				Kind kind = kNoKey;
			};

			std::uint64_t generation = 0;
			std::uint32_t sceneFrame = ~0u;
			std::shared_ptr<const GraphKeys> graph;  // null: no walk this frame
			std::uint32_t graphAge = 0;              // publishes since the graph last changed
			Keys current;
			Keys previous;  // the previous set's current (for observers' frame alignment)

			/** @brief Main::IsRoomVisible on keys alone. a_previous reads the previous walk's keys. */
			Answer Visible(const void* a_key, bool a_previous = false) const;
		};

		/**
		 * @brief A lock-free hold on the newest VisibleSet, because MSVC's atomic shared_ptr takes a lock.
		 * Any thread. Not freed while held. Null before the first publish.
		 */
		class VisibleSetHold
		{
		public:
			VisibleSetHold();
			~VisibleSetHold();
			VisibleSetHold(const VisibleSetHold&) = delete;
			VisibleSetHold& operator=(const VisibleSetHold&) = delete;
			const VisibleSet* get() const { return set; }
			const VisibleSet* operator->() const { return set; }
			explicit operator bool() const { return set != nullptr; }

		private:
			const VisibleSet* set = nullptr;
		};
		/** @brief From now on Build publishes a VisibleSet every frame. */
		void PublishVisibleSets() { publishVisible.store(true, std::memory_order_release); }
		/** @brief The newest set's generation. An older hold raced a publish. */
		std::uint64_t VisibleGeneration() const { return visibleGeneration.load(std::memory_order_acquire); }

		/**
		 * @brief A shadow light's rooms as its portal-graph entry holds them (port of FUN_1414a2530 / FUN_1414a6400).
		 * The cases are in dclf-architecture.md, "PortalViews::WalkLight". An unwalked light keeps its entry.
		 */
		struct LightRooms
		{
			bool visibleUnbound = true;      // +0x130
			std::vector<const void*> rooms;  // multiBoundRoomAccumList (+0x18)
			std::uint32_t walk = 0;          // how the last call left it: LightWalk
			Portals::ContextHold context;
		};
		enum LightWalk : std::uint32_t
		{
			kLightUntouched,  // not walked this frame (hidden, faded, on an object node): the engine keeps its last entry
			kLightBounds,
			kLightWalked,
			kLightNoGraph,    // no portal graph with rooms: nothing for a strict light's walk to reach, nor to gate
		};
		/** @brief Walks a_light's entry as FUN_1414a2530 would this frame. Render thread, after Build. */
		void WalkLight(const void* a_light, LightRooms& a_rooms);
		/** @brief Keeps the last walked light, so its edge flags become the shapes'. The engine walks only kept lights. */
		void CommitLightWalk();
		/** @brief FUN_140e14300: the light and the world camera both see unbound space, or share a room. */
		bool SharesRoom(const LightRooms& a_light) const;

	private:
		PortalViews() = default;

		struct Vec3
		{
			float x = 0.0f, y = 0.0f, z = 0.0f;
		};
		using Quad = std::array<Vec3, 4>;
		using Flags = std::array<std::uint8_t, 4>;
		/** @brief The engine's scratch on an occlusion box while its state is 1. */
		struct BoxScratch
		{
			PlaneSet set[2];
			std::array<std::uint32_t, 8> hull{};
			std::uint32_t hullCount = 0;
			std::array<std::int32_t, 6> face{ -1, -1, -1, -1, -1, -1 };
		};
		struct Camera
		{
			const std::byte* camera = nullptr;  // the NiCamera
			Vec3 eye;
			PlaneSet view;      // NiCullingProcess::SetFrustum's planes
			PlaneSet relative;  // FUN_140e16f70's camera-relative copy (box refinement)
		};

		void Walk(const void* a_graph, const void* a_startRoom, bool a_isFirst);
		void BuildRoom(const void* a_room, std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, const void* a_startRoom);
		void BuildUnbound(std::uint32_t a_parent, bool a_parentUnbound, const void* a_portal, const void* a_graph);
		Program& ProgramAt(std::uint32_t a_index, bool a_unbound) { return a_unbound ? frame.unbound : frame.rooms[a_index]; }
		std::int32_t FindRoom(const void* a_room) const;
		std::uint32_t AddRoom(const void* a_room);

		void Reset(Program& a_program) const;
		static void Emit(Program& a_program, std::uint32_t a_word) { PortalModel::Emit(a_program, a_word); }
		static void EmitTest(Program& a_program, std::uint32_t a_type, std::uint32_t a_set) { PortalModel::EmitTest(a_program, a_type, a_set); }
		static void Finalize(Program& a_program) { PortalModel::Finalize(a_program); }
		static void ReopenAsOr(Program& a_program) { PortalModel::ReopenAsOr(a_program); }
		static void CompactEmptyGroups(Program& a_program) { PortalModel::CompactEmptyGroups(a_program); }
		static void Merge(Program& a_destination, const Program& a_source) { PortalModel::Merge(a_destination, a_source); }
		void CopyMasked(Program& a_out, const Program& a_parent, const void* a_portal);
		void AddPortal(Program& a_program, const void* a_portal);
		void AddOccluderTop(Program& a_program, const void* a_occluder, bool& a_needOpen);
		void AddOccluderRoom(Program& a_program, const void* a_occluder, const Program* a_parent);
		bool AddBox(Program& a_program, const void* a_box);
		void AddPlane(Program& a_program, const void* a_plane);
		static Quad Corners(const void* a_quad);
		static PlaneSet& Grow(Program& a_program, std::uint32_t a_index) { return PortalModel::Grow(a_program, a_index); }
		PlaneSet PortalPlanes(const void* a_quad, const Vec3& a_eye);
		bool TestQuadProgram(const Program& a_program, const void* a_quad);
		bool TestShapeProgram(const Program& a_program, const void* a_shape) const;
		static bool QuadVsSet(const PlaneSet& a_set, const Quad& a_quad, Flags& a_flags);
		static bool AllCornersInside(const Quad& a_quad, const PlaneSet& a_set);
		bool PortalVsSetEdges(const void* a_portal, const PlaneSet& a_set);
		bool WithinFrustumDistFirst(const void* a_quad, const PlaneSet& a_planes, const Vec3& a_eye);
		bool PlaneJudged(const void* a_plane);
		bool BoxInView(const void* a_box) const;
		bool BoxSilhouette(const void* a_box, BoxScratch& a_scratch);
		bool BoxRefine(const void* a_box, BoxScratch& a_scratch) const;

		/** @brief Encodes the words at the end of every Build, and publishes the VisibleSet once enabled. */
		void AfterBuild();
		void PublishVisible();

		Frame frame;
		Camera camera;
		// DCLF's copies of the engine's shape scratch. States are 0 untested, 1 accepted, 2 rejected, and FUN_1401a4a00 resets them
		// before each walk. Edge flags persist across frames.
		ankerl::unordered_dense::map<const void*, Flags> edgeFlags;
		ankerl::unordered_dense::map<const void*, std::uint32_t> states;
		ankerl::unordered_dense::map<const void*, BoxScratch> boxes;
		ankerl::unordered_dense::set<const void*> crossed;                   // portals keyed this frame
		Program temp;                                                        // the engine's temp frustum (0x14332a710)
		const void* edgeGraph = nullptr;                                     // the graph edgeFlags are for
		ankerl::unordered_dense::map<const void*, std::uint32_t> roomIndex;  // room -> index (ProgramOf)
		std::uint64_t graphVersion = 0;                                      // bumped with every new graph
		std::atomic<std::uint64_t> structure{ 0 };                           // NoteStructure
		std::vector<std::uint32_t> encoded;                                  // Encoded
		Portals::ContextHold mainContext;
		std::uint64_t contextGeneration = 0;
		std::atomic<bool> visibleUnbound{ true };  // VisibleUnbound (the engine entry's initial value)

		// Replaced VisibleSets are freed at a publish that sees no reader. Readers count themselves before loading.
		std::atomic<bool> publishVisible{ false };
		std::atomic<const VisibleSet*> visibleSet{ nullptr };
		std::atomic<std::uint32_t> visibleReaders{ 0 };
		std::atomic<std::uint64_t> visibleGeneration{ 0 };
		std::shared_ptr<const VisibleSet> visibleOwned;
		std::vector<std::shared_ptr<const VisibleSet>> visibleRetired;
		std::shared_ptr<const VisibleSet::GraphKeys> graphKeys;  // reused while the graph's counts hold
		Reports::Window visibleReports;                          // RoomVisibility's line every 300 sets

		/** @brief BSParabolicCullingProcess's view test (TestBaseVisibility3, 0x14151a050) for a light's walk. */
		struct Parabolic
		{
			Vec3 centre;
			float radius = 0.0f;
			std::array<float, 4> plane{};  // through the light, along its camera's forward (FUN_141519d20)
			bool bothSides = false;        // +0x30200 set
		};
		static bool ParabolicVisible(const Parabolic& a_parabolic, const void* a_portal);
		const Parabolic* parabolic = nullptr;  // set during a light's walk
		Frame lightFrame;
		ankerl::unordered_dense::set<const void*> lightCrossed;
		ankerl::unordered_dense::map<const void*, Flags> lightEdgeFlags;  // the last light walk's copy of edgeFlags
		bool lightFlagsPending = false;
		std::array<std::byte, 0x188> lightCamera{};
	};
}
