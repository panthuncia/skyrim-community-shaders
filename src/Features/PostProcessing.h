#pragma once

#include "PostProcessing/PostProcessFeature.h"

namespace RenderGraph::Engine
{
	struct TonemapTargets;
}

#include "PostProcessing/BokehResources.h"
#include "PostProcessing/Border.h"
#include "PostProcessing/CODBloom.h"
#include "PostProcessing/Camera.h"
#include "PostProcessing/CinematicCamera.h"
#include "PostProcessing/ColorGrading.h"
#include "PostProcessing/Composite.h"
#include "PostProcessing/DoF.h"
#include "PostProcessing/HistogramAutoExposure.h"
#include "PostProcessing/LUT.h"
#include "PostProcessing/LensFlare.h"
#include "PostProcessing/LocalExposure.h"
#include "PostProcessing/MotionBlur.h"
#include "PostProcessing/PhysicalGlare.h"
#include "PostProcessing/PhysicalLens.h"
#include "PostProcessing/Vignette.h"
#include "Snapshot/FrameSnapshot.h"

#include <chrono>
#include <map>
#include <optional>
#include <tuple>

struct PostProcessing : Feature
{
	static PostProcessing* GetSingleton()
	{
		static PostProcessing singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Post Processing"; }
	virtual inline std::string GetDisplayName() override { return T("feature.post_processing.name", "Post Processing"); }
	virtual inline std::string GetShortName() override { return "PostProcessing"; }
	virtual inline std::string_view GetShaderDefineName() override { return "POSTPROCESS"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::ImageSpace;
	};
	virtual std::string_view GetCategory() const override { return FeatureCategories::kPostProcessing; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			T("feature.post_processing.description", "Post Processing provides advanced image effects and enhancements to improve the visual quality of the game."),
			{ T("feature.post_processing.key_feature_1", "Customizable post-processing effects"),
				T("feature.post_processing.key_feature_2", "Supports various presets for different visual styles"),
				T("feature.post_processing.key_feature_3", "Improves overall image quality and immersion"),
				T("feature.post_processing.key_feature_4", "Includes features like bloom, depth of field, and color grading") }
		};
	}

	virtual void DrawSettings() override;
	virtual bool HasDebugSection() const override { return true; }
	virtual void DrawDebugSection() override;

	/** @brief The effect whose settings page is open when it has debug views, or null (the debug tabs then show Post Processing only). */
	PostProcessFeature* GetOpenEffectWithDebug() const;

	/** @brief Draws the game's current image space values as cards (the Post Processing debug tab). */
	void DrawImageSpaceDebug();
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void ApplyContextualDefaults(json& a_defaults) const override;

	/**
	 * @brief Whether Post Processing wants to replace the vanilla tonemap this frame.
	 */
	bool WantsTonemapOwnership() const;

	/**
	 * @brief Whether the sub-feature pipeline runs this frame: Post Processing mode and owner of the tonemap.
	 *
	 * Otherwise PreProcess draws no sub-features, so every other entry point must bail out too.
	 */
	bool IsPipelineRunning() const;

	/**
	 * @brief Whether Color Grading wants Pumbo AutoHDR this frame (SDR tonemapper + HDR Display).
	 */
	bool WantsAutoHDR() const;

	/// Settings loaded before the pipeline exists.
	json pendingSettings = {};

	void ProcessSettings(json& o_json);

	/// Sets each sub-feature's on/off state to the shipped default. Their values default to their
	/// Settings initializers, so this and RestoreDefaultSettings need no defaults file.
	void ApplyDefaultEnabledStates();

	/** @brief Creates every sub-feature at its shipped defaults, without GPU resources. */
	void BuildPipeline();

	enum class FeaturePipelineIndex : size_t
	{
		DoF,
		Vignette,
		LocalExposure,
		AutoExposure,
		MotionBlur,
		PhysicalGlare,
		CODBloom,
		LensFlare,
		PhysicalLens,
		Composite,
		ColorGrading,
		LUT,
		Camera,
		Border,
		COUNT
	};

	void UpdateVanillaEffects(bool active);

	/** @brief Binds a sub-feature's on/off to its frame variable, "PostProcessing.<slot>.enabled". At each new instance. */
	static void BindPipelineSettings(PostProcessFeature& a_feature, FeaturePipelineIndex a_index);

	/** @brief Creates the sub-feature at a pipeline slot, at its defaults and without GPU resources. */
	static std::shared_ptr<PostProcessFeature> MakePipelineFeature(FeaturePipelineIndex idx);

	/** @brief Runs the sub-feature's SetupResources once per instance. */
	void EnsureResources(PostProcessFeature& feature);

	/**
	 * @brief Frees inactive sub-features, so disabled effects hold no GPU memory. An inactive sub-feature with resources is
	 * replaced by a fresh instance carrying its settings and enabled state. Active ones are set up where the D3D11 path draws
	 * them (DrawFeature), so none are made while the render graph runs the chain. Call after the auto-enabled update.
	 */
	void SyncPipelineResources();

	/** @brief Updates the auto-enabled sub-features, then SyncPipelineResources: every draw of the pipeline starts with it. */
	void UpdatePipeline();

	/**
	 * @brief The sub-features the tonemap chain (PreProcess) draws this frame, in draw order: the pipeline's order, those drawn
	 * after Color Grading last. Empty while bypassed. Call after UpdatePipeline.
	 */
	std::vector<FeaturePipelineIndex> ChainOrder() const;

	/// shared_ptr, not unique_ptr: see PostProcessFeature's weak_ptr callback contract.
	std::array<std::shared_ptr<PostProcessFeature>, static_cast<size_t>(FeaturePipelineIndex::COUNT)> pipeline;

	BokehResources bokehResources;

	CinematicCamera::Controller cinematicCamera;

	/**
	* @brief What the post chain reads of the game, captured once per game update: the ImageSpace data, the Cinematic Camera's
	* state, and the focus targets DoF and Physical Lens resolve.
	*/
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::PostProcessing;

		RE::ImageSpaceData imageSpace{};  // the ImageSpace manager's data, with the override or current base data as its base

		bool cameraValid = false;                       // the Cinematic Camera published a state
		CinematicCamera::PhysicalCameraState camera{};  // valid while cameraValid

		/** @brief The focus target, by the DoF console-selection preference: console pick, TDM lock, dialogue speaker. */
		struct Target
		{
			bool found = false;      // a reference was selected this update
			bool projected = false;  // it is on screen: focus at coord
			float2 coord = { 0.5f, 0.5f };
			float distance = 0.0f;  // game units from the camera
			bool operator==(const Target&) const = default;
		};
		CinematicCamera::FocusResolver::TargetFocusResult cameraTarget{};  // the Cinematic Camera's Target mode (holds a lost target)
		Target dofTarget{};                                                // DoF's own target focus, while it is on
		bool consoleRefSet = false;                                        // console selection was allowed: consoleRef is current
		uint consoleRef = 0;                                               // the console pick's form id, for the DoF settings readout
		bool raceMenu = false;                                             // RaceMenu is open: focus on the player
		Target player{};

		bool operator==(const Block& a_other) const
		{
			return std::memcmp(&imageSpace, &a_other.imageSpace, sizeof(imageSpace)) == 0 && cameraValid == a_other.cameraValid &&
			       camera == a_other.camera && cameraTarget == a_other.cameraTarget && dofTarget == a_other.dofTarget &&
			       consoleRefSet == a_other.consoleRefSet && consoleRef == a_other.consoleRef && raceMenu == a_other.raceMenu &&
			       player == a_other.player;
		}
	};

	/**
	* @brief Fills the block. Runs the Cinematic Camera's update here too: its FOV write is game state. Registered in PostPostLoad.
	* Game thread.
	*/
	static void Capture(Snapshot::Builder& a_builder);

	/** @brief The adopted snapshot's block, or null before the first or while Post Processing captured none. */
	static const Block* FrameBlock();

	/** @brief The Cinematic Camera's state in the adopted snapshot, or null while it publishes none. */
	[[nodiscard]] static const CinematicCamera::PhysicalCameraState* GetActivePhysicalCameraState()
	{
		const Block* block = FrameBlock();
		return block && block->cameraValid ? &block->camera : nullptr;
	}

	[[nodiscard]] CinematicCamera::Controller& GetCinematicCamera() { return cinematicCamera; }

	template <typename T>
	T* GetPipelineFeature(FeaturePipelineIndex idx)
	{
		return static_cast<T*>(pipeline[static_cast<size_t>(idx)].get());
	}

	virtual void ClearShaderCache() override;

	virtual void SetupResources() override;
	virtual void Reset() override;

	virtual void PostPostLoad() override;
	virtual void Prepass() override;

	/**
	 * @brief At the tonemap hook, before the Tonemap anchor runs: lets the render graph take the chain this frame
	 * (ORGPostProcessing, in graph mode). PreProcess then draws nothing.
	 */
	void BeforeTonemapAnchor(const RenderGraph::Engine::TonemapTargets& a_targets);

	/** @brief Whether the scene is in kMAIN_COPY at the tonemap (its input, or refraction routed it there), else in kMAIN. */
	bool ReadsMainCopy(RE::RENDER_TARGET a_input) const;

	/// Returns the untouched scene target when ISHDR must tonemap it directly.
	std::optional<RE::RENDER_TARGET> PreProcess(RE::RENDER_TARGET a_input);

	/**
	 * @brief Whether a Post Processing bloom replaces the vanilla bloom this frame, so the HDR effect's bloom blur draws nothing (its
	 * Render hook, and the native post chain, skip it outside the UI's 3D scenes).
	 */
	static bool SkipsVanillaBloom();
	void DrawBeforeUpscaling();
	void DrawFeature(PostProcessFeature& feature, PostProcessFeature::TextureInfo& lastTexColor);

	/// Render-thread scratch for the serial color chain. The returned target
	/// differs from input, which must be the only chain texture still needed.
	Texture2D* GetColorTarget(ID3D11Texture2D* input, DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);

	enum class Gamut : uint
	{
		Rec709,
		ACEScg,
		Rec2020
	};
	struct alignas(16) CopyCB
	{
		Gamut inputGamut = Gamut::Rec709;
		Gamut outputGamut = Gamut::Rec709;
		float gamma = 1.f;
		float scale = 1.f;
	};

	/**
	 * @brief The conversion PreProcess applies when it copies the chain's output into the main target.
	 * @param a_colorGraded Whether Color Grading drew this frame: its output is in the display's gamut.
	 * @param a_exposureResolved Whether Composite removed the scene pre-exposure. When false the copy divides it out.
	 */
	CopyCB OutputConversion(bool a_colorGraded, bool a_exposureResolved = true) const;

	/////////////////////////////////////////////////////////////////////////////////

	bool bypass = false;
	bool isrefraction = false;

	struct ImageSpaceManager
	{
		RE::ImageSpaceData gameISData;
	};

	std::unique_ptr<ImageSpaceManager> imageSpaceManager = std::make_unique<ImageSpaceManager>();

	/// Format-conversion copy pass (fullscreen triangle PS draw).
	winrt::com_ptr<ID3D11PixelShader> copyPS = nullptr;
	std::unique_ptr<ConstantBuffer> copyCB;

	/// Shared fullscreen-triangle vertex shader for every raster pass in the
	/// pipeline (compiled from PostProcessing/fullscreen.hlsli).
	winrt::com_ptr<ID3D11VertexShader> fullscreenVS = nullptr;

	/**
	 * @brief Vertex shader every rasterized sub-feature draws with.
	 * @return The shared fullscreen-triangle VS, or null until SetupResources
	 *         succeeds; raster passes must skip when null.
	 */
	ID3D11VertexShader* GetFullscreenVS() const { return fullscreenVS.get(); }

	/////////////////////////////////////////////////////////////////////////////////

	// Tonemap-time entry is driven by PostProcessingExtensions::Main_HDRTonemapBlendCinematic_Render
	// (Hooks.cpp), which arbitrates between this feature and Effects11. Only the refraction
	// hook remains here, and it just flags which buffer the scene currently lives in.

	struct BSImagespaceShaderRefraction_SetupTechnique
	{
		static void thunk(RE::BSShader* a_shader, RE::BSShaderMaterial* a_material)
		{
			globals::features::postProcessing.isrefraction = true;
			func(a_shader, a_material);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	struct RenderTargetCopy
	{
		ID3D11Texture2D* target;
		PostProcessFeature::TextureInfo source;
		Texture2D* scratch;
		CopyCB conversion;
	};

	std::optional<RenderTargetCopy> PrepareRenderTargetCopy(
		ID3D11Texture2D* target, PostProcessFeature::TextureInfo source, const CopyCB& conversion);
	void CopyToRenderTarget(const RenderTargetCopy& copy);
	bool BeginLinearProcessing(PostProcessFeature::TextureInfo& texture);
	Texture2D* GetColorTarget(ID3D11Texture2D* input, UINT width, UINT height, DXGI_FORMAT format);

	D3D11_TEXTURE2D_DESC sceneColorDesc{};
	D3D11_TEXTURE2D_DESC mainColorDesc{};
	std::vector<std::unique_ptr<Texture2D>> colorTargets;
	std::map<std::tuple<UINT, UINT, DXGI_FORMAT, UINT>, std::chrono::steady_clock::time_point> colorTargetRetry;
};
