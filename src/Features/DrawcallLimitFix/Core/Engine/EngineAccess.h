#pragma once

// Raw access to the engine for the hooks and the engine state DCLF reads without CommonLib declarations.
// Every address is an Offset of Address Library ids, resolved to the running runtime's module offset.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <memory>

namespace DCLF::Engine
{
	/** @brief The game versions DCLF's addresses are checked against. Any other gets no address at all. */
	enum class Runtime
	{
		Unsupported,
		Se1597,   // SE 1.5.97
		Ae1170,   // AE 1.6.1170
		Ae17104,  // AE 1.7.104
	};

	inline Runtime Running()
	{
		static const Runtime running = [] {
			const auto version = REL::Module::get().version();
			if (version == SKSE::RUNTIME_SSE_1_5_97)
				return Runtime::Se1597;
			if (version == SKSE::RUNTIME_SSE_1_6_1170)
				return Runtime::Ae1170;
			if (version == REL::Version(1, 7, 104, 0))
				return Runtime::Ae17104;
			return Runtime::Unsupported;
		}();
		return running;
	}

	inline bool Supported() { return Running() != Runtime::Unsupported; }

	/**
	 * @brief An Address Library id, plus a place `at` bytes into the function it names. Built at compile time only. It rejects ids at or
	 * above 2^20 and places 0x4000 or more in, which catches a module offset pair written where an id belongs.
	 */
	struct Id
	{
		std::uint64_t id = 0;
		std::uint32_t at = 0;

		constexpr Id() = default;
		consteval Id(std::uint64_t a_id, std::uint32_t a_at = 0) :
			id(a_id), at(a_at)
		{
			if (a_id >= (1u << 20) || a_at >= 0x4000)
				throw "Engine::Id: not an Address Library id and a place inside its function";
		}
	};

	namespace detail
	{
		/** @brief An id's module offset, looked up once, because CommonLib's lookup is a binary search and some offsets are read per object. */
		inline std::uintptr_t Resolve(std::uint64_t a_id)
		{
			constexpr std::uint64_t kCached = 1u << 20;  // above every id SE (847307) and AE 1.7.104 (565758) have
			static const auto cache = std::make_unique<std::atomic<std::uint32_t>[]>(kCached);
			if (a_id >= kCached)
				return REL::ID(a_id).offset();
			auto& slot = cache[a_id];
			std::uint32_t offset = slot.load(std::memory_order_relaxed);
			if (!offset) {
				offset = static_cast<std::uint32_t>(REL::ID(a_id).offset());
				slot.store(offset, std::memory_order_relaxed);
			}
			return offset;
		}
	}

	/**
	 * @brief An engine address on each supported runtime: AE, SE, and AE 1.7.104 where it differs. An empty id means not found for
	 * that runtime, and whatever uses it must not install there.
	 */
	struct Offset
	{
		Id ae{};  // AE 1.6.1170, and 1.7.104 unless ae17 is given
		Id se{};    // SE 1.5.97
		Id ae17{};  // AE 1.7.104, where it differs from ae

		[[nodiscard]] std::uintptr_t Get() const
		{
			const Id* id = nullptr;
			switch (Running()) {
			case Runtime::Se1597:
				id = &se;
				break;
			case Runtime::Ae1170:
				id = &ae;
				break;
			case Runtime::Ae17104:
				id = ae17.id ? &ae17 : &ae;
				break;
			default:
				return 0;
			}
			return id->id ? detail::Resolve(id->id) + id->at : 0;
		}
		[[nodiscard]] bool Known() const { return Get() != 0; }
		operator std::uintptr_t() const { return Get(); }
	};

	/** @brief Whether the instruction at a_address is a CALL rel32 to a_target, as reverse engineered. */
	inline bool CallsTo(std::uintptr_t a_address, std::uintptr_t a_target)
	{
		const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
		if (bytes[0] != 0xE8)
			return false;
		std::int32_t displacement = 0;
		std::memcpy(&displacement, bytes + 1, sizeof(displacement));
		return a_address + 5 + static_cast<std::intptr_t>(displacement) == a_target;
	}

	/** @brief A module global at a_offset. */
	template <class T>
	T& Global(std::uintptr_t a_offset)
	{
		return *reinterpret_cast<T*>(REL::Module::get().base() + a_offset);
	}

	/** @brief The field of type T at a_offset into a_base. */
	template <class T>
	T& At(const void* a_base, std::size_t a_offset)
	{
		return *reinterpret_cast<T*>(const_cast<std::byte*>(static_cast<const std::byte*>(a_base)) + a_offset);
	}

	/** @brief The performance counter, for the hooks' timings. */
	inline std::int64_t Now()
	{
		LARGE_INTEGER value{};
		QueryPerformanceCounter(&value);
		return value.QuadPart;
	}

	/** @brief Render thread. BSGraphics::SetDirtyStates(false): applies the engine's pending render state as its next draw would. */
	inline void ApplyPendingState()
	{
		static REL::Relocation<void (*)(bool)> setDirtyStates{ REL::RelocationID(75580, 77386) };
		setDirtyStates(false);
	}

	/**
	 * @brief The engine's "no fade node", a parentless BSFadeNode (0x14332a2a0) alive for the whole process. Every shader property's
	 * fadeNode starts pointing at it, and the engine's readers treat it as null. Terrain, object LOD and non-fade-root references keep it.
	 */
	inline constexpr Offset kEmptyFadeNode{ { 414561 }, { 527632 } };  // NiPointer<BSFadeNode>
	inline const void* EmptyFadeNode()
	{
		return Global<const void*>(kEmptyFadeNode);
	}

	// Engine addresses DCLF reads or calls. The comments' addresses are AE 1.6.1170's.
	namespace Address
	{
		inline constexpr Offset kDepthAccumulator{ { 415008 }, { 528063 } };  // BSShaderAccumulator*: the main camera's depth pass (render mode 0xC)
		inline constexpr Offset kMainAccumulator{ { 415009 }, { 528064 } };  // BSShaderAccumulator*: the main camera's (render mode 0)
		inline constexpr Offset kFirstPersonAccumulator{ { 415011 }, { 528066 } };  // BSShaderAccumulator*: the first-person view's
		inline constexpr Offset kSceneListCount{ { 415016 }, { 528071 } };  // std::uint32_t: the scene lists
		inline constexpr Offset kSceneLists{ { 415017 }, { 528072 } };  // BSTArray<NiPointer<NiAVObject>>*: the scene lists
		inline constexpr Offset kExtraList{ { 415020 }, { 528075 } };  // BSTArray<NiPointer<NiAVObject>>: culled by the first list job only
		inline constexpr Offset kListCullJobs{ { 414944 }, { 527999 } };  // JobList*: _gJobList_SceneListAccumCulling (null: the list culls run inline)
		inline constexpr Offset kListProcesses{ { 415022 }, { 528077 } };  // BSGeometryListCullingProcess**: one per scene list
		inline constexpr Offset kDrawWorldEye{ { 415027 }, { 528082 } };  // Main::DrawWorld's camera position (float[3])
		inline constexpr Offset kWorldSceneGraph{ { 415032 }, { 528087 } };  // the world scene graph; its camera at +0x128
		inline constexpr Offset kSunOff{ { 415040 }, { 528095 } };  // bool: the sun does not draw (no full-frustum cull, no cascades)
		inline constexpr Offset kListJobsFinish{ { 69353 }, { 67992 } };  // the scene list jobs' finish (the job list's wait)
		inline constexpr Offset kFadeTypeDivisors{ { 390867 }, { 513143 } };  // float per LOD type (+0x153 & 0xF): the LOD factor's divisor
		inline constexpr Offset kFadeLodUpdates{ { 390865 }, { 513141 } };  // byte: the fade LOD updates are on
		inline constexpr Offset kFadesOn{ { 390866 }, { 513142 } };  // byte: BSFadeNode::OnVisible updates fades
		inline constexpr Offset kFadeOutThreshold{ { 390880 }, { 513156 } };  // float: a faded-in node fades out below this fade value
		inline constexpr Offset kFadeDistanceMult{ { 390884 }, { 513160 } };  // float: multiplies the node's near and far distances
		inline constexpr Offset kFadeSpecialA{ { 414551 }, { 527623 } };  // float: with kFadeSpecialB, OnVisible's LOD-type-6 branch (the metric override)
		inline constexpr Offset kFadeSpecialB{ { 186426 }, { 228611 } };  // float: -1.0, a constant (also the float tree LOD refills pack)
		inline constexpr Offset kFadeDefaultScale{ { 470010 }, { 232846 } };  // float: the fade scale when the type's divisor is not positive
		inline constexpr Offset kMetricScale{ { 252903 }, { 302862 } };  // float: the fade metric scale
		inline constexpr Offset kScreenDoorFades{ { 391102 }, { 513330 } };  // byte: screen-door fades are on
		inline constexpr Offset kNoSunShadowDir{ { 390962 }, { 513222 } };  // byte: GetRenderPasses gives no pass ShadowDir
		inline constexpr Offset kTreeOnVisible{ { 105604 }, { 98953 } };  // BSTreeNode::OnVisible (the height test, BSLeafAnimNode's, the LOD fix-up)
		inline constexpr Offset kFadeUpdate{ { 105486 }, { 98849 } };  // FUN_14147a160(node, fadeAmount, camera): BSFadeNode's fade state machine
		inline constexpr Offset kLodLevelUpdate{ { 105487 }, { 98850 } };  // FUN_14147a430(node, level): the leaf and tree LOD transition (+0x152)
		inline constexpr Offset kShadowSceneNode{ { 390951 }, { 513211 } };  // ShadowSceneNode*: the main one (BSShaderManager::State's first; its +0x304 the wind magnitude)
		inline constexpr Offset kFocusTargetCount{ { 414625 }, { 527703 } };  // the focus targets' count
		inline constexpr Offset kFocusTargets{ { 414623 }, { 527701 } };      // 0x14332a488: the focus targets, 16 bytes each ({float distance; NiAVObject* node})
		inline constexpr Offset kFocusShadows{ { 390932 }, { 513201 } };      // byte, 0x142032fd0: the focus shadows setting
		inline constexpr Offset kShadowLightCount{ { 415036 }, { 528091 } };  // std::uint32_t, 0x14338c904: gLastFrameEnabledShadowCasterLightsCount2
		inline constexpr Offset kShadowInsideMask{ { 415038 }, { 528093 } };  // std::uint32_t, 0x14338c90c: the shadow lights whose sphere holds the camera
		inline constexpr Offset kLodFadeCutoff{ { 415264 }, { 528316 } };     // float, 0x1433dcfac: the shadow lights' UpdateCamera lodFade cutoff
		inline constexpr Offset kTlsIndex{ { 415542 }, { 528600 } };          // std::uint32_t, 0x1435f0878: the executable's TLS index
		inline constexpr Offset kTransformToMatrix{ { 106462 }, { 99814 } };  // FUN_1414aaf10: an NiTransform as the shaders' row-major matrix
		inline constexpr Offset kLightingShader{ { 415094 }, { 528150 } };  // BSLightingShader*: the instance its constructor writes
		inline constexpr Offset kImageSpaceManager{ { 414660 }, { 527731 } };  // ImageSpaceManager*; the HDR multiplier at +0xe0
		inline constexpr Offset kHighDetailRange{ { 390958 }, { 513218 } };  // float[4]: the loaded grid's centre and half extents (FUN_141480ab0)
		inline constexpr Offset kHighDetailMargin{ { 470029 }, { 249190 } };  // float: taken off the high detail range's z and w (15)
		inline constexpr Offset kProjectedTiling{ { 391644 }, { 513700 } };  // float: ProjectedUV's first tiling (FUN_1414e00c0)
		inline constexpr Offset kProjectedTiling2{ { 391647 }, { 513702 } };  // float: its second tiling
		inline constexpr Offset kProjectedNormals{ { 391635 }, { 513694 } };  // byte: the projected-normals switch
		inline constexpr Offset kSwitchSetRevision{ { 70598 }, { 69234 } };  // FUN_140d29990(revisions, index, revID): a switch child's revision marked current
		inline constexpr Offset kMaterialRelease{ { 107720 }, { 100935 } };  // FUN_1414f7a40(manager, material): a material reference dropped (AE ID 107720)
		inline constexpr Offset kMaterialManager{ { 403555 }, { 517047 } };  // the material database (AE ID 403555)
		inline constexpr Offset kFloatControllerTargets{ { 415317 }, { 528371 } };  // std::uint32_t per type: the float controller's material float index
		inline constexpr Offset kColorControllerTargets{ { 415344 }, { 528398 } };  // std::uint32_t per type: the colour controller's material byte offset

		// The feature's hook sites, each checked with CallsTo before it is patched.
		inline constexpr Offset kGeometryGroupDraw{ { 107630, 0xfd }, {} };     // a geometry group's pass list draw (FUN_1414f19d0; SE: the loop is RenderPassImmediately<1>'s)
		inline constexpr Offset kRenderPassImmediately{ { 107644 }, { 100854 } };  // what it calls: BSBatchRenderer::RenderPassImmediately
		inline constexpr Offset kUpdateEntryCall{ { 36564, 0x3e }, { 35565, 0x1e } };         // Main::Update's first call
		inline constexpr Offset kStagesReset{ { 69510 }, { 68163 } };             // what it calls
		inline constexpr Offset kCellAnimationsCall{ { 39067, 0xa2 }, { 38111, 0xaa } };      // the cell animations job's update call (FUN_1406d29d0, FUN_140640300)
		inline constexpr Offset kCellAnimationsUpdate{ { 13291 }, { 13151 } };    // what it calls: TES's animated references' update (TES, time)
		inline constexpr Offset kAnimatedObjectsCall{ { 39100, 0x32 }, { 38143, 0x32 } };     // the "Animated cell object update" job's update call (FUN_1406d3770, FUN_140640fc0)
		inline constexpr Offset kAnimatedObjectsUpdate{ { 36998 }, { 36022 } };   // what it calls: the animated objects' updates, queued on its job group
		inline constexpr Offset kAnimatedObjectsWaitCall{ { 39100, 0x52 }, { 38143, 0x52 } };  // the job's wait for its group (after the group's start)
		inline constexpr Offset kAnimatedObjectsWait{ { 69353 }, { 67992 } };     // what it calls: the job group's wait
		inline constexpr Offset kUpdateSkinInstance{ { 77461 }, { 75655 } };      // FUN_140e4ff90(skin, world): a skin instance's palette update
		inline constexpr Offset kFrameCounter{ { 411489 }, { 525008 } };          // std::uint32_t gFrameCounter: Renderer::End (0x140e4482b) increments it
		inline constexpr Offset kParabolaRadius{ { 391879 }, { 513818 } };          // float: the paraboloid shadow map's radius (ParabolaParam.x's inverse)
		inline constexpr Offset kParabolaSide{ { 391880 }, { 513819 } };            // float: ParabolaParam.y
		inline constexpr Offset kDepthBiasEnabled{ { 390939 }, { 513197 } };        // byte: ToggleDepthBias (read by FUN_1414b3bb0)
		inline constexpr Offset kCameraDepthRange{ { 388951 }, { 524904 } };        // float[2]: the camera state's depth range (FUN_140e57e10)
		inline constexpr Offset kMatrixMultiply{ { 109815 }, { 102357 } };          // the engine's 4x4 multiply (out, a, b)
	}

	/**
	 * @brief Sets the thread's allocation context id (+0x768 in its TLS block) for a scope and restores it after, as the engine does around
	 * some allocations and releases, so its memory accounting treats them as its own.
	 */
	class ScopedMemoryContext
	{
	public:
		explicit ScopedMemoryContext(std::uint32_t a_context)
		{
			constexpr std::size_t kContextSlot = 0x768;
			const auto* blocks = reinterpret_cast<const std::uintptr_t*>(__readgsqword(0x58));
			slot = reinterpret_cast<std::uint32_t*>(blocks[Global<std::uint32_t>(Address::kTlsIndex)] + kContextSlot);
			saved = *slot;
			*slot = a_context;
		}
		~ScopedMemoryContext() { *slot = saved; }
		ScopedMemoryContext(const ScopedMemoryContext&) = delete;
		ScopedMemoryContext& operator=(const ScopedMemoryContext&) = delete;

	private:
		std::uint32_t* slot = nullptr;
		std::uint32_t saved = 0;
	};

	// NiAVObject::flags and the bits DCLF reads and writes.
	constexpr std::size_t kObjectFlags = 0xF4;
	constexpr std::uint32_t kFlagHidden = 1u << 0;        // kHidden (app-culled)
	constexpr std::uint32_t kFlagAccumulated = 1u << 26;  // kAccumulated: passed the last cull that reached it

	/**
	 * @brief A node's flags, read atomically because engine threads store other bits of the same dword. Any thread. a_node must be alive.
	 */
	inline std::uint32_t FlagsOf(const void* a_node)
	{
		return std::atomic_ref<std::uint32_t>(At<std::uint32_t>(a_node, kObjectFlags)).load(std::memory_order_relaxed);
	}
	/** @brief Whether a node is app-culled. */
	inline bool IsHidden(const void* a_node) { return (FlagsOf(a_node) & kFlagHidden) != 0; }

	// NiAVObject's OnVisible vtable slot, which tells the fade node classes apart.
	constexpr std::size_t kOnVisibleSlot = 0x34;

	/** @brief A BSTreeNode's LOD switch (+0x180), or null for a node whose OnVisible is not BSTreeNode's. */
	inline const RE::NiAVObject* TreeLodSwitch(const RE::NiAVObject& a_node)
	{
		const auto onVisible = (*reinterpret_cast<const std::uintptr_t* const*>(&a_node))[kOnVisibleSlot];
		if (onVisible != REL::Module::get().base() + Address::kTreeOnVisible)
			return nullptr;
		return At<const RE::NiAVObject*>(&a_node, 0x180);
	}

	/** @brief A ShadowSceneNode's object root, its child 3 and the cells' parent, or null. The tree must be held. */
	inline RE::NiAVObject* ObjectRoot(const RE::NiNode& a_sceneNode)
	{
		const auto& children = a_sceneNode.GetChildren();
		return children.free_idx() > 3 ? children[3].get() : nullptr;
	}

	/** @brief An object root's visible cell nodes from child a_first. Children 0 and 1 are whole entries. */
	template <class Visit>
	void ForEachCell(const RE::NiAVObject* a_objectRoot, std::uint16_t a_first, Visit&& a_visit)
	{
		auto* root = a_objectRoot ? const_cast<RE::NiAVObject*>(a_objectRoot)->AsNode() : nullptr;
		if (!root || IsHidden(root))
			return;
		const auto& cells = root->GetChildren();
		for (std::uint16_t i = a_first; i < cells.free_idx(); ++i)
			if (auto* cell = cells[i] ? cells[i]->AsNode() : nullptr; cell && !IsHidden(cell))
				a_visit(*cell);
	}

	/** @brief A cell node's visible category nodes. */
	template <class Visit>
	void ForEachCategory(RE::NiNode& a_cell, Visit&& a_visit)
	{
		const auto& categories = a_cell.GetChildren();
		for (std::uint16_t j = 0; j < categories.free_idx(); ++j)
			if (auto* category = categories[j] ? categories[j]->AsNode() : nullptr; category && !IsHidden(category))
				a_visit(j, *category);
	}

	/** @brief The loaded cells TES draws: the interior cell, else every grid cell, empty slots passed as null. */
	template <class Visit>
	void ForEachLoadedCell(const RE::TES& a_tes, Visit&& a_visit)
	{
		if (a_tes.interiorCell) {
			a_visit(a_tes.interiorCell);
		} else if (const auto* grid = a_tes.gridCells) {
			const std::uint32_t count = grid->length * grid->length;
			for (std::uint32_t i = 0; i < count; ++i)
				a_visit(grid->cells[i]);
		}
	}

	/** @brief Whether an NiObject's exact class is named a_name. */
	inline bool RttiIs(const void* a_object, const char* a_name)
	{
		const auto* rtti = static_cast<const RE::NiObject*>(a_object)->GetRTTI();
		return rtti && rtti->name && std::strcmp(rtti->name, a_name) == 0;
	}

	/** @brief Whether an object's exact class has vtable a_vtable. */
	inline bool VtableIs(const void* a_object, std::uintptr_t a_vtable)
	{
		return a_object && *static_cast<const std::uintptr_t*>(a_object) == a_vtable;
	}

	/** @brief BSLightingShaderMaterial and its 13 subclasses' vtables, in a fixed order. */
	inline const std::array<REL::VariantID, 14>& LightingMaterialVtables()
	{
		static const std::array<REL::VariantID, 14> vtables{ RE::VTABLE_BSLightingShaderMaterial[0], RE::VTABLE_BSLightingShaderMaterialBase[0],
			RE::VTABLE_BSLightingShaderMaterialEnvmap[0], RE::VTABLE_BSLightingShaderMaterialEye[0], RE::VTABLE_BSLightingShaderMaterialGlowmap[0],
			RE::VTABLE_BSLightingShaderMaterialParallax[0], RE::VTABLE_BSLightingShaderMaterialParallaxOcc[0], RE::VTABLE_BSLightingShaderMaterialFacegen[0],
			RE::VTABLE_BSLightingShaderMaterialFacegenTint[0], RE::VTABLE_BSLightingShaderMaterialHairTint[0], RE::VTABLE_BSLightingShaderMaterialLandscape[0],
			RE::VTABLE_BSLightingShaderMaterialLODLandscape[0], RE::VTABLE_BSLightingShaderMaterialSnow[0], RE::VTABLE_BSLightingShaderMaterialMultiLayerParallax[0] };
		return vtables;
	}

	// BSShaderProperty's light data, holding the mask of the shadow lights the geometry is registered with.
	constexpr std::size_t kPropertyLightData = 0x70;
	constexpr std::size_t kLightDataActiveMask = 0x1C;

	/**
	 * @brief An NiSwitchNode's own fields at their AE offsets. CommonLib declares them after NiNode, whose size in a multi-runtime
	 * build is VR's, so its members read the wrong memory.
	 */
	struct SwitchState
	{
		std::uint16_t flags = 0;  // bit 0: the update pass updates only the selected child
		std::int32_t index = -1;
		std::uint32_t revID = 0;
		const std::uint32_t* childRevID = nullptr;
		std::uint16_t childRevCapacity = 0;
	};
	/**
	 * @brief The child a switch selects, bounded by capacity since Gamebryo arrays are indexed by slot. It is current when
	 * childRevID[index] == revID. Otherwise NiSwitchNode::OnVisible updates it before culling.
	 */
	struct SwitchSelection
	{
		RE::NiAVObject* child = nullptr;
		std::uint16_t index = 0;
		bool current = false;
	};

	inline SwitchState ReadSwitch(const RE::NiSwitchNode& a_switch)
	{
		const auto* base = reinterpret_cast<const std::byte*>(&a_switch);
		SwitchState state;
		state.flags = *reinterpret_cast<const std::uint16_t*>(base + 0x128);
		state.index = *reinterpret_cast<const std::int32_t*>(base + 0x12C);
		state.revID = *reinterpret_cast<const std::uint32_t*>(base + 0x134);
		// childRevID is an NiTPrimitiveArray at +0x138, data at +0x8 and capacity at +0x10.
		state.childRevID = *reinterpret_cast<const std::uint32_t* const*>(base + 0x140);
		state.childRevCapacity = *reinterpret_cast<const std::uint16_t*>(base + 0x148);
		return state;
	}

	inline SwitchSelection SelectionOf(const RE::NiSwitchNode& a_switch, const SwitchState& a_state)
	{
		const auto& children = a_switch.GetChildren();
		if (a_state.index < 0 || static_cast<std::uint32_t>(a_state.index) >= children.capacity() || !a_state.childRevID ||
			static_cast<std::uint32_t>(a_state.index) >= a_state.childRevCapacity)
			return {};
		const auto index = static_cast<std::uint16_t>(a_state.index);
		return { children[index].get(), index, a_state.childRevID[index] == a_state.revID };
	}

	inline bool SwitchSelects(const RE::NiSwitchNode& a_switch, const RE::NiAVObject* a_child)
	{
		// NiSwitchNode::OnVisible (AE 140d29700) brings a newly selected child up to date in the cull, after this walk read its transforms.
		// So leave it native for that frame.
		const auto selection = SelectionOf(a_switch, ReadSwitch(a_switch));
		return selection.child && selection.child == a_child && selection.current;
	}
}
