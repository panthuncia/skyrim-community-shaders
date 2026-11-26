#pragma once

#include "Buffer.h"
#include "GrassShared/GrassFrame.h"
#include "Snapshot/FrameSnapshot.h"
#include "Utils/Setting.h"

struct GrassCollision : Feature
{
public:
	/** @brief Binds the settings that shape the frame. */
	GrassCollision();

	virtual inline std::string GetName() override { return "Grass Collision"; }
	virtual std::string GetDisplayName() override { return T("feature.grass_collision.name", "Grass Collision"); }
	virtual inline std::string GetShortName() override { return "GrassCollision"; }
	virtual inline std::string_view GetShaderDefineName() override { return "GRASS_COLLISION"; }
	virtual std::string_view GetCategory() const override { return FeatureCategories::kGrass; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { T("feature.grass_collision.description", "Enables dynamic grass interactions where grass bends and moves in response to actors walking through it, creating more immersive environmental reactions."),
			{ T("feature.grass_collision.key_feature_1", "Real-time grass deformation from actor movement"),
				T("feature.grass_collision.key_feature_2", "Collision detection for up to 256 simultaneous interactions"),
				T("feature.grass_collision.key_feature_3", "Dynamic tracking of actor positions for grass response"),
				T("feature.grass_collision.key_feature_4", "Performance-optimized collision calculation"),
				T("feature.grass_collision.key_feature_5", "Seamless integration with existing grass rendering") } };
	};

	/** @brief Returns true only for Grass shader type. */
	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	/** @brief Dispatches the collision update compute shader to write actor collision data into the collision texture. */
	void UpdateCollisionTexture();

	struct Settings
	{
		CS::Setting<bool> EnableGrassCollision = true;
		bool TrackRagdolls = 1;
		bool EnableBlur = 1;
	};

	struct alignas(16) BoundingBoxPacked
	{
		float2 MinExtent = { 0, 0 };
		float2 MaxExtent = { 0, 0 };
		uint IndexStart = 0;
		uint IndexEnd = 0;
		float2 pad0 = { 0, 0 };

		bool operator==(const BoundingBoxPacked&) const = default;
	};
	STATIC_ASSERT_ALIGNAS_16(BoundingBoxPacked);

	struct alignas(16) PerFrame
	{
		float2 PosOffset;              // cell origin in camera model space
		DirectX::XMUINT2 ArrayOrigin;  // xy: array origin (clipmap wrapping)

		DirectX::XMINT2 ValidMargin;
		float TimeDelta;
		uint BoundingBoxCount;

		float CameraHeightDelta;
		float3 pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	Settings settings;

	ConstantBuffer* perFrame = nullptr;
	// The constants of the latest Update, published to GrassShared on frames DCLF owns grass.
	PerFrame lastPerFrame{};

	eastl::unique_ptr<Buffer> collisionBoundingBoxes = nullptr;
	eastl::unique_ptr<Buffer> collisionInstances = nullptr;

	/**
	* @brief The frame's colliders, gathered on the game thread from the actors near the main camera. Shapes are relative to the
	* snapshot's main camera position, the eye the frame's update uses.
	*/
	struct Block
	{
		static constexpr Snapshot::BlockId kId = Snapshot::BlockId::GrassCollision;
		std::vector<BoundingBoxPacked> boxes;  // at most MAX_BOUNDING_BOXES
		std::vector<float4> collisions;        // the shapes the boxes index: centre (camera-relative) and radius

		bool operator==(const Block&) const = default;
	};

	/** @brief Gathers the colliders into the block. Registered with Snapshot::RegisterCapture in PostPostLoad. Game thread. */
	static void Capture(Snapshot::Builder& a_builder);

	/** @brief The adopted snapshot's block, or an empty one. */
	static const Block& FrameBlock();

	/** @brief Releases the cached collision update compute shader so it can be recompiled. */
	virtual void ClearShaderCache() override;

	/** @brief Returns the collision update compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetCollisionUpdateCS();
	ID3D11ComputeShader* collisionUpdateCS;

	Texture2D* collisionTexture = nullptr;

	/**
	 * @brief Creates the per-frame constant buffer, then the collision update's resources: the render graph's (CS_ORG_GRASS_COLLISION=1,
	 * ORGGrassCollision), else the D3D11 collision texture and structured buffers.
	 */
	virtual void SetupResources() override;

	/**
	 * @brief On the render graph's path: this frame's per-frame values and colliders, staged for the EarlyPrepass anchor that runs right
	 * after, the constant buffer the engine's grass reads updated, and the state published for DCLF's grass.
	 */
	virtual void EarlyPrepass() override;

	/** @brief Draws the ImGui settings UI for grass collision options. */
	virtual void DrawSettings() override;
	/**
	 * @brief Uploads queued collision data to GPU buffers and dispatches the collision texture update.
	 *
	 * Called once per frame: from the grass shader setup geometry hook (a_eye the renderer's, the main view's there), or stood down
	 * (a_eye the frame's, from GrassShared).
	 */
	void Update(const RE::NiPoint3& a_eye);

	/** @brief The frame's per-frame values for a_eye and a_block; advances the clipmap origin and eye height kept from the last call. */
	PerFrame NextPerFrame(const RE::NiPoint3& a_eye, const Block& a_block);

	/** @brief The collision texture's view the grass reads: the render graph's export while it runs the update, else the D3D11 texture's. */
	ID3D11ShaderResourceView* CollisionSRV() const;

	/** @brief Publishes the collision texture and lastPerFrame for DCLF's grass draws (GrassShared). */
	void PublishCollision() const;

	/** @brief Binds the per-frame constants (VS b5) and the collision texture (VS t100) for the engine's grass draws. */
	void BindCollision() const;

	/** @brief Logs the colliders of a_block every 120 frames when CS_GRASS_COLLISION_STATS=1. */
	static void LogColliders(const char* a_path, const Block& a_block);

	/** @brief The D3D11 path's collision texture and structured buffers: at setup, or when the render graph's path fails. */
	void CreateD3D11Resources();

	float2 prevCellID = { 0, 0 };
	RE::NiPoint3 prevEyePos{};
	bool hasPrevEye = false;
	std::uint64_t steppedSnapshot = 0;  // the snapshot the collision decay last stepped for: a repeat does not step it

	/** @brief GrassShared stood-down callback, on frames DCLF owns grass and no grass SetupGeometry runs: updates the
	    collision texture and publishes it with its constants for DCLF's grass draws. */
	static void StoodDownUpdate(const GrassShared::StoodDownFrame& a_frame);

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Installs the BSGrassShader hook after all plugins have loaded, and registers the capture and the stood-down update. */
	virtual void PostPostLoad() override;

	struct Hooks
	{
		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);
			logger::info("[GRASS COLLISION] Installed hooks");
		}
	};
};
