#pragma once

namespace CS::Deferred
{
	inline constexpr std::uint32_t kAbiVersion = 7;
	inline constexpr std::uint32_t kMaxLights = 1024;
	using ContextIndex = std::uint16_t;
	inline constexpr ContextIndex kInvalidContext = 0xFFFF;
	// Context zero is reserved for frame-global/non-BSLightingShader producers.
	// Draw-specific interned contexts begin at one.
	inline constexpr ContextIndex kFrameGlobalContext = 0;
	using PBRMaterialIndex = std::uint16_t;
	inline constexpr PBRMaterialIndex kInvalidPBRMaterial = 0xFFFF;
	// Keep the persistent material table bounded. The index ABI remains 16-bit,
	// leaving room to grow this without changing the packed G-buffer identity.
	inline constexpr std::uint32_t kMaxPBRMaterials = 4096;

	enum class PBRPayloadProfile : std::uint8_t
	{
		Core = 0,
		SubsurfaceFuzz = 1,
		Coat = 2,
		Glint = 3,
		Parallax = 4,
		TerrainAdvanced = 5,
		LodBlend = 6,
	};

	struct PackedPBRIdentity
	{
		std::uint32_t surface{};
		std::uint32_t materialProfileAux = kInvalidPBRMaterial;
		std::uint32_t payload0{};
		std::uint32_t payload1{};
	};
	static_assert(sizeof(PackedPBRIdentity) == 16);
	static_assert(offsetof(PackedPBRIdentity, materialProfileAux) == 4);

	constexpr std::uint32_t PackPBRMaterialProfileAux(PBRMaterialIndex material,
		PBRPayloadProfile profile, std::uint8_t auxiliary) noexcept
	{
		return std::uint32_t(material) |
			(std::uint32_t(profile) << 16u) | (std::uint32_t(auxiliary) << 24u);
	}
	static_assert(PackPBRMaterialProfileAux(kInvalidPBRMaterial,
		PBRPayloadProfile::Core, 0) == 0x0000FFFFu);

	inline constexpr std::uint32_t kInvalidTextureDescriptor = 0xFFFFFFFFu;
	struct alignas(16) PBRLandscapeLayerRecord
	{
		std::uint32_t baseColorTexture = kInvalidTextureDescriptor;
		std::uint32_t normalTexture = kInvalidTextureDescriptor;
		std::uint32_t rmaosTexture = kInvalidTextureDescriptor;
		std::uint32_t displacementTexture = kInvalidTextureDescriptor;
		// xyz: roughness, displacement, specular; w: active/PBR layer.
		float4 materialParameters{};
		float4 glintParameters{};
	};
	static_assert(sizeof(PBRLandscapeLayerRecord) == 48);

	// Fence-retired, immutable material record ABI. Descriptor fields remain
	// invalid for raster-resolved profiles. Texture-dependent evaluators remain
	// disabled until a future direct-share/native-D3D12 texture registry can
	// populate every descriptor they require; material textures are never mirrored.
	struct alignas(16) PBRMaterialRecord
	{
		std::uint32_t abiVersion = kAbiVersion;
		std::uint32_t flags{};
		std::uint32_t samplerPolicy{};
		std::uint32_t generation{};
		std::uint32_t objectTextures[8]{
			kInvalidTextureDescriptor, kInvalidTextureDescriptor,
			kInvalidTextureDescriptor, kInvalidTextureDescriptor,
			kInvalidTextureDescriptor, kInvalidTextureDescriptor,
			kInvalidTextureDescriptor, kInvalidTextureDescriptor };
		float4 materialParameters[6]{};
		PBRLandscapeLayerRecord landscapeLayers[6]{};

		bool operator==(const PBRMaterialRecord& other) const noexcept
		{
			return std::memcmp(this, std::addressof(other), sizeof(*this)) == 0;
		}
	};
	STATIC_ASSERT_ALIGNAS_16(PBRMaterialRecord);
	static_assert(sizeof(PBRMaterialRecord) == 432);
	static_assert(offsetof(PBRMaterialRecord, landscapeLayers) == 144);

	enum class LightFlags : std::uint32_t
	{
		PortalStrict = 1u << 0,
		Shadow = 1u << 1,
		Simple = 1u << 2,
		Initialised = 1u << 8,
		Disabled = 1u << 9,
		InverseSquare = 1u << 10,
		Linear = 1u << 11,
	};

	struct Position
	{
		float3 data{};
		std::uint32_t padding{};
	};

	struct alignas(16) Light
	{
		float3 color{};
		float fade = 1.0f;
		float radius{};
		float invRadius{};
		float fadeZone{};
		float sizeBias{};
		Position positionWS{};
		uint128_t roomFlags = std::uint32_t(0);
		stl::enumeration<LightFlags> lightFlags{};
		std::uint32_t shadowMaskIndex = 0;
		std::uint32_t padding[2]{};
	};
	STATIC_ASSERT_ALIGNAS_16(Light);
	static_assert(sizeof(Light) == 80);

	enum class MaterialClass : std::uint8_t
	{
	#define CS_DEFERRED_MATERIAL(name, value, cppEvaluator, hlslEvaluator) name = value,
	#include "../../../package/Shaders/DeferredRendering/DeferredMaterialRegistry.def"
	#undef CS_DEFERRED_MATERIAL
		Legacy = CSRegLegacy,
		StandardOpaque = CSRegStandardOpaque,
		AlphaTestedOpaque = CSRegAlphaTestedOpaque,
		StandardSpecular = CSRegStandardSpecular,
		AlphaTestedSpecular = CSRegAlphaTestedSpecular,
		Foliage = CSRegFoliage,
		Terrain = CSRegTerrain,
		TerrainSpecular = CSRegTerrainSpecular,
		LodObject = CSRegLodObject,
		FoliageSpecular = CSRegFoliageSpecular,
		TruePBR = CSRegTruePbr,
		TruePBRTerrain = CSRegTruePbrTerrain,
		TruePBRSubsurfaceFuzz = CSRegTruePbrSubsurfaceFuzz,
		TruePBRCoat = CSRegTruePbrCoat,
		TruePBRGlint = CSRegTruePbrGlint,
		TruePBRParallax = CSRegTruePbrParallax,
		TruePBRTerrainAdvanced = CSRegTruePbrTerrainAdvanced,
		TruePBRLodBlend = CSRegTruePbrLodBlend,
		Grass = CSRegGrass,
		DistantTree = CSRegDistantTree,
		FoliageSpecial = CSRegFoliageSpecial,
		LodLand = CSRegLodLand,
		Skin = CSRegSkin,
		Hair = CSRegHair,
		EyeEnvmap = CSRegEyeEnvmap,
	};

	// Stable evaluator-family ABI. MaterialClass describes resolved surface
	// variants; Evaluator selects the compute kernel that consumes those data.
	enum class Evaluator : std::uint8_t
	{
	#define CS_DEFERRED_EVALUATOR(cppName, hlslName, value, red, green, blue) cppName = value,
	#include "../../../package/Shaders/DeferredRendering/DeferredMaterialRegistry.def"
	#undef CS_DEFERRED_EVALUATOR
		Compatibility = CSRegCompatibility,
		Generic = CSRegGeneric,
		TruePBR = CSRegTruePbr,
		Grass = CSRegGrass,
		DistantTree = CSRegDistantTree,
		Skin = CSRegSkin,
		Hair = CSRegHair,
		EyeEnvmap = CSRegEyeEnvmap,
		FoliageSpecial = CSRegFoliageSpecial,
		TruePBRSubsurfaceFuzz = CSRegTruePbrSubsurfaceFuzz,
		TruePBRCoat = CSRegTruePbrCoat,
		TruePBRGlint = CSRegTruePbrGlint,
		TruePBRParallax = CSRegTruePbrParallax,
		TruePBRTerrain = CSRegTruePbrTerrain,
		TruePBRTerrainAdvanced = CSRegTruePbrTerrainAdvanced,
		TruePBRLodBlend = CSRegTruePbrLodBlend,
		Count = 16,
	};
	inline constexpr std::uint32_t kEvaluatorCount = static_cast<std::uint32_t>(Evaluator::Count);
	static_assert(kEvaluatorCount <= 256);

	constexpr Evaluator GetEvaluator(MaterialClass materialClass) noexcept
	{
		switch (materialClass) {
	#define CS_DEFERRED_MATERIAL(name, value, cppEvaluator, hlslEvaluator) \
		case MaterialClass::name: return Evaluator::cppEvaluator;
	#include "../../../package/Shaders/DeferredRendering/DeferredMaterialRegistry.def"
	#undef CS_DEFERRED_MATERIAL
		default:
			return Evaluator::Compatibility;
		}
	}

	constexpr std::string_view GetMaterialClassName(MaterialClass materialClass) noexcept
	{
		switch (materialClass) {
	#define CS_DEFERRED_MATERIAL(name, value, cppEvaluator, hlslEvaluator) \
		case MaterialClass::name: return std::string_view(#name).substr(5);
	#include "../../../package/Shaders/DeferredRendering/DeferredMaterialRegistry.def"
	#undef CS_DEFERRED_MATERIAL
		default: return "Unknown";
		}
	}

	constexpr std::uint32_t EvaluatorBit(Evaluator evaluator) noexcept
	{
		return 1u << static_cast<std::uint32_t>(evaluator);
	}

	constexpr bool IsEvaluatorEnabled(std::uint32_t mask, Evaluator evaluator) noexcept
	{
		return evaluator != Evaluator::Compatibility && (mask & EvaluatorBit(evaluator)) != 0;
	}

	struct EvaluatorColor
	{
		float red;
		float green;
		float blue;
	};

	constexpr EvaluatorColor GetEvaluatorColor(Evaluator evaluator) noexcept
	{
		switch (evaluator) {
	#define CS_DEFERRED_EVALUATOR(cppName, hlslName, value, red, green, blue) \
		case Evaluator::cppName: return { red##f, green##f, blue##f };
	#include "../../../package/Shaders/DeferredRendering/DeferredMaterialRegistry.def"
	#undef CS_DEFERRED_EVALUATOR
		default: return {};
		}
	}

	static_assert(GetEvaluator(MaterialClass::FoliageSpecial) == Evaluator::FoliageSpecial);
	static_assert(GetEvaluator(MaterialClass::DistantTree) == Evaluator::DistantTree);
	static_assert(GetEvaluator(MaterialClass::Grass) == Evaluator::Grass);
	static_assert(GetEvaluator(MaterialClass::LodObject) == Evaluator::Generic);
	static_assert(GetEvaluator(MaterialClass::LodLand) == Evaluator::Generic);
	static_assert(GetEvaluator(MaterialClass::FoliageSpecular) == Evaluator::Generic);
	static_assert(GetEvaluator(MaterialClass::TruePBR) == Evaluator::TruePBR);
	static_assert(GetEvaluator(MaterialClass::TruePBRSubsurfaceFuzz) == Evaluator::TruePBRSubsurfaceFuzz);
	static_assert(GetEvaluator(MaterialClass::TruePBRCoat) == Evaluator::TruePBRCoat);
	static_assert(GetEvaluator(MaterialClass::TruePBRGlint) == Evaluator::TruePBRGlint);
	static_assert(GetEvaluator(MaterialClass::TruePBRTerrain) == Evaluator::TruePBRTerrain);
	static_assert(IsEvaluatorEnabled(EvaluatorBit(Evaluator::Generic), Evaluator::Generic));

	enum SurfaceFlags : std::uint8_t
	{
		kSurfaceNone = 0,
		kSurfaceReceivesDirectional = 1u << 0,
		kSurfaceReceivesLocal = 1u << 1,
		kSurfaceHasVertexAO = 1u << 2,
	};
	inline constexpr std::uint8_t kSurfaceFlagMask = 0x07;
	inline constexpr std::uint8_t kVertexAOShift = 3;
	inline constexpr std::uint8_t kVertexAOMax = 31;

	inline constexpr std::uint32_t PackSurface(
		ContextIndex context,
		MaterialClass materialClass,
		std::uint8_t flags) noexcept
	{
		return std::uint32_t(context) |
			(std::uint32_t(materialClass) << 16) |
			(std::uint32_t(flags) << 24);
	}
	inline constexpr ContextIndex UnpackContext(std::uint32_t packed) noexcept
	{
		return static_cast<ContextIndex>(packed & 0xFFFFu);
	}
	inline constexpr MaterialClass UnpackMaterialClass(std::uint32_t packed) noexcept
	{
		return static_cast<MaterialClass>((packed >> 16) & 0xFFu);
	}
	inline constexpr std::uint8_t UnpackSurfaceFlags(std::uint32_t packed) noexcept
	{
		return static_cast<std::uint8_t>((packed >> 24) & kSurfaceFlagMask);
	}

	inline constexpr std::uint32_t kLegacyPackedSurface =
		PackSurface(kInvalidContext, MaterialClass::Legacy, kSurfaceNone) |
		(std::uint32_t(kVertexAOMax) << (24 + kVertexAOShift));
	static_assert(UnpackContext(kLegacyPackedSurface) == kInvalidContext);
	static_assert(UnpackMaterialClass(kLegacyPackedSurface) == MaterialClass::Legacy);
	static_assert(UnpackSurfaceFlags(PackSurface(42, MaterialClass::StandardOpaque,
		kSurfaceReceivesDirectional | kSurfaceReceivesLocal)) ==
		(kSurfaceReceivesDirectional | kSurfaceReceivesLocal));

	struct alignas(16) LightingContext
	{
		float4 directionalLightDirection{};
		float4 directionalLightColor{};
		float4 directionalAmbient[3]{};
		float4 ambientSpecularTintAndFresnelPower{};
		float4 emissiveColor{};
		// xyz: the geometry shader's SpecularColor; w: vanilla shininess.
		// Glossiness remains a resolved per-pixel G-buffer value.
		float4 specularColorAndShininess{};
		// x: soft-light rolloff; y: rim exponent. These are draw-uniform inputs
		// to the existing CS foliage lobes.
		float4 lightingEffectParams{};
		std::int32_t roomIndex = -1;
		std::uint32_t shadowLightMembershipMask = 0;
		std::uint32_t featureFlags = 0;
		std::uint32_t abiVersion = kAbiVersion;

		bool operator==(const LightingContext& other) const noexcept
		{
			return std::memcmp(this, std::addressof(other), sizeof(*this)) == 0;
		}
	};
	STATIC_ASSERT_ALIGNAS_16(LightingContext);
	static_assert(sizeof(LightingContext) == 160);
	static_assert(offsetof(LightingContext, roomIndex) == 144);
	static_assert(offsetof(LightingContext, abiVersion) == 156);

	struct LightingTransform
	{
		std::uint32_t enableLinearLighting{};
		std::uint32_t isDirectionalLightLinear{};
		float directionalLightScale = 1.0f;
		float lightGamma = 1.0f;
		float directionalLightMultiplier = 1.0f;
		float pointLightMultiplier = 1.0f;
		float vanillaNormalization = 1.0f;
		float ambientGamma = 1.0f;
		float ambientMultiplier = 1.0f;
		float padding[3]{};
	};
	static_assert(sizeof(LightingTransform) == 48);

	struct alignas(16) Cluster
	{
		float4 minPoint{};
		float4 maxPoint{};
		std::uint32_t numLights{};
		std::uint32_t firstPage = 0xFFFFFFFF;
		std::uint32_t padding[2]{};
	};
	static_assert(sizeof(Cluster) == 48);
	static_assert(offsetof(Cluster, firstPage) == 36);

	struct LightPage
	{
		std::uint32_t nextPage = 0xFFFFFFFF;
		std::uint32_t numLights{};
		std::uint32_t lightIndices[12]{};
	};
	static_assert(sizeof(LightPage) == 56);
	static_assert(offsetof(LightPage, lightIndices) == 8);
}
