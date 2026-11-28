#pragma once
#include "OverlayFeature.h"
#include "Snapshot/FrameSnapshot.h"
#include "UnifiedWater/Flowmap.h"
#include "UnifiedWater/WaterCache.h"

#include <atomic>

/** @brief Replaces distant water tiles with LOD0 water to eliminate water LOD mismatch. */
struct UnifiedWater : OverlayFeature
{
	virtual inline std::string GetName() override { return "Unified Water"; }
	virtual std::string GetDisplayName() override { return T("feature.unified_water.name", "Unified Water"); }
	/** @brief Returns the short identifier used for file paths and logging. */
	virtual inline std::string GetShortName() override { return "UnifiedWater"; }
	virtual inline std::string_view GetShaderDefineName() override { return "UNIFIED_WATER"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kWater; }
	/** @brief Returns a summary description and list of key features for the UI. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.unified_water.description", "Unified Water provides a comprehensive fix to water LOD mismatch by replacing distant water tiles with LOD0 (Close Water)."),
			{ T("feature.unified_water.key_feature_1", "Unifies distant and close water appearance, streamlining all lighting visuals."),
				T("feature.unified_water.key_feature_2", "Completely and fundamentally resolves water LOD mismatch issues."),
				T("feature.unified_water.key_feature_3", "Provides background systems for water geometry rendering, allowing more advanced water effects."),
				T("feature.unified_water.key_feature_4", "Improves vanilla performance by using efficient water meshes for distant water.") } };
	};

	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	/** @brief The flowmap values BSWaterShader_SetupGeometry rewrites a water draw's constants from. */
	struct FlowRewrite
	{
		bool active = false;  // the hook rewrites: an exterior worldspace is active and the flowmap exists
		int32_t width = 0;
		int32_t height = 0;
		int32_t offsetX = 0;
		int32_t offsetY = 0;
	};

	/** @brief What the SetupGeometry hook would apply to a water draw now. DCLF's water port computes the same values from it. */
	FlowRewrite GetFlowRewrite() const;

	/** @brief The LOD water root (BGSTerrainManager's "WaterLOD" node) the tiles hang under, or null. */
	RE::NiNode* GetWaterLODRoot() const;

	/** @brief Hook that overrides water shader material parameters during water initialization. */
	struct TESWaterSystem_InitializeWater_SetWaterShaderMaterialParams
	{
		static void thunk(RE::TESWaterForm* form, RE::BSWaterShaderMaterial* material);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that modifies CRC32 computation for water shader materials to account for unified water. */
	struct BSWaterShaderMaterial_ComputeCRC32
	{
		static int32_t thunk(RE::BSWaterShaderMaterial* material, uint32_t srcHash);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that intercepts terrain block attachment to inject unified water meshes. */
	struct BGSTerrainBlock_Attach
	{
		static void thunk(RE::BGSTerrainBlock* block);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that intercepts terrain block detachment to clean up unified water meshes. */
	struct BGSTerrainBlock_Detach
	{
		static void thunk(RE::BGSTerrainBlock* block);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that controls water mesh sub-visibility for terrain LOD nodes. */
	struct BGSTerrainNode_UpdateWaterMeshSubVisibility
	{
		static void thunk(const RE::BGSTerrainNode* node, RE::BSMultiBoundNode* waterParent);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that handles worldspace transitions to update water cache and flowmap state. */
	struct TES_SetWorldSpace
	{
		static void thunk(RE::TES* tes, RE::TESWorldSpace* worldSpace, bool isExterior);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that handles sky cell destruction during worldspace cleanup. */
	struct TES_DestroySkyCell
	{
		static void thunk(RE::TES* tes);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that sets up per-geometry water shader data during rendering. */
	struct BSWaterShader_SetupGeometry
	{
		static void thunk(RE::BSShader* waterShader, RE::BSRenderPass* pass, uint32_t renderFlags);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Hook that updates displacement mesh position for water surface animation. */
	struct TESWaterSystem_UpdateDisplacementMeshPosition
	{
		static void thunk(RE::TESWaterSystem* waterSystem);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	/** @brief Event handler that tracks menu open/close state for water rendering decisions. */
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		/** @brief Processes menu open/close events to track map menu state. */
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override;

		/** @brief Registers the event handler with the game's UI event source. */
		static bool Register();
	};

	virtual bool HasDebugSection() const override { return true; }

	/** @brief Draws the Debug section (flowmap and water cache regeneration). */
	virtual void DrawDebugSection() override;

	/** @brief Draws the debug overlay showing water cache build progress. */
	virtual void DrawOverlay() override;
	/** @brief Returns whether the overlay should be displayed. */
	virtual bool IsOverlayVisible() const override;

	/** @brief Handles post-data-load initialization including flowmap and cache setup. */
	virtual void DataLoaded() override;

	virtual bool IsCore() const override { return true; }

	/** @brief Installs engine hooks for water mesh replacement and worldspace handling, and registers the capture. */
	virtual void PostPostLoad() override;

	/** @brief What the frame's water draws read of the world. */
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::UnifiedWater;
		bool exteriorActive = false;  // IsExteriorWorldspaceActive at the capture

		bool operator==(const Block&) const = default;
	};

	/** @brief Fills the block and updates the water LOD's cull flag (game-side). Registered in PostPostLoad. Game thread. */
	static void Capture(Snapshot::Builder& a_builder);

private:
	RE::NiPointer<RE::BSTriShape> waterMesh;
	Flowmap* flowmap = nullptr;
	WaterCache* waterCache = nullptr;

	RE::NiNode** gWaterLOD = nullptr;
	RE::NiPointer<RE::NiSourceTexture>* gFlowMapSourceTex = nullptr;
	int32_t* gFlowMapSize = nullptr;
	float4* gDisplacementCellTexCoordOffset = nullptr;
	RE::NiPoint2* gDisplacementMeshPos = nullptr;
	RE::NiPoint2* gDisplacementMeshFlowCellOffset = nullptr;

	std::atomic_bool exteriorWorldspaceActive{ false };
	std::atomic_bool mapMenuOpen{ false };

	void SetFlowmapTex() const;
	/** @brief Returns whether the water mesh and cache built in DataLoaded are available; hooks installed in PostPostLoad run before it and survive its failure paths. */
	bool IsWaterDataReady() const;
	bool IsExteriorWorldspaceActive() const;
	/**
	 * @brief The adopted snapshot's IsExteriorWorldspaceActive, for the render half's water hooks. The game thread, while the native
	 * render thread runs, reads the game's own state instead: the adopted snapshot is the render thread's.
	 */
	static bool FrameExteriorActive();
	void UpdateWaterLODCull() const;
	static bool LoadOrderChanged();
};
