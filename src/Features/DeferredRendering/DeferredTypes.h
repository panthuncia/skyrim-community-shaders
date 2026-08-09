#pragma once

namespace CS::Deferred
{
	inline constexpr std::uint32_t kAbiVersion = 1;
	inline constexpr std::uint32_t kMaxLights = 1024;
	using ContextIndex = std::uint16_t;
	inline constexpr ContextIndex kInvalidContext = 0xFFFF;

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
		Legacy = 0,
		StandardOpaque = 1,
		AlphaTestedOpaque = 2,
	};

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
	static_assert(sizeof(LightingContext) == 112);
	static_assert(offsetof(LightingContext, roomIndex) == 96);
	static_assert(offsetof(LightingContext, abiVersion) == 108);

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
