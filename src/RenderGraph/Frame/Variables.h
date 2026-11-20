#pragma once

#include "Features/Upscaling/PresentationConfig.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <string_view>

/**
* @brief The frame-wide variables (frame revision plan, step 5b).
*
* A variable is a typed, named input of a frame revision. A write that changes its value is an event. It wakes the revision
* lane, and the next frame revision carries the new value. The frame reads the value of the revision it adopted last, never the
* newest written one.
*
* Setters work from any thread and return whether the value changed. Writing the same value is no event, so writers may write
* every frame. Each event logs one line "[ORG] variable <name>: <old> -> <new> (<writer>)", rate-limited per variable.
*
* Getters named Adopted... are for the render thread. They return the value of the last adopted revision, or the initial value
* before the first adoption.
*
* Without the render graph there are no revisions. HdrEnabled and PresentationConfig read back the newest written value, the
* others their initial value. This header includes nothing of ORG or OMS, since features include it.
*/
namespace RenderGraph::Frame::Vars
{
	/** @brief A width and a height in pixels. */
	struct Extent2D
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		friend bool operator==(const Extent2D&, const Extent2D&) = default;
	};

	/** @brief The dynamic resolution ratios, render size over display size per axis. */
	struct Ratios
	{
		float x = 1.0f;
		float y = 1.0f;
		friend bool operator==(const Ratios&, const Ratios&) = default;
	};

	/** @brief The upscaling method in use. Same values as Upscaling::UpscaleMethod. */
	enum class Upscaler : std::uint32_t
	{
		None,
		TAA,
		FSR,
		DLSS,
		XeSS,
	};

	/** @brief Which texture a target is, and its shape. A new texture is a new id even at the same shape. */
	struct TargetIdentity
	{
		std::uint64_t id = 0;  // the texture's address, 0 for none
		Extent2D size{};
		std::uint32_t format = 0;  // DXGI_FORMAT
		friend bool operator==(const TargetIdentity&, const TargetIdentity&) = default;
	};

	/** @brief The engine's shadow map array (kSHADOWMAPS). */
	struct ShadowMapShape
	{
		Extent2D size{};
		std::uint32_t format = 0;  // DXGI_FORMAT
		std::uint32_t slices = 0;
		friend bool operator==(const ShadowMapShape&, const ShadowMapShape&) = default;
	};

	/** @brief The engine's reflection cubemap (kREFLECTIONS). */
	struct CubemapShape
	{
		std::uint32_t size = 0;  // the edge of a face
		friend bool operator==(const CubemapShape&, const CubemapShape&) = default;
	};

	/** @brief What an upscaler SDK context is made for (plan 11, 4.3). */
	struct UpscalerOptions
	{
		Upscaler method = Upscaler::None;
		std::uint32_t qualityMode = 0;
		Extent2D output{};
		bool hdr = false;
		friend bool operator==(const UpscalerOptions&, const UpscalerOptions&) = default;
	};

	/** @brief Terrain Shadows' heightmap: which one is installed, and its size. A new heightmap is a new id even at the same size. */
	struct HeightMapIdentity
	{
		std::uint64_t id = 0;  // 0 for none
		Extent2D size{};
		friend bool operator==(const HeightMapIdentity&, const HeightMapIdentity&) = default;
	};

	/** @brief What the presenter's surface takes: the swap chain's format, and whether it presents HDR10. */
	struct SurfaceFormat
	{
		std::uint32_t format = 0;  // DXGI_FORMAT, 0 before the presenter committed one
		bool hdr10 = false;
		friend bool operator==(const SurfaceFormat&, const SurfaceFormat&) = default;
	};

	/** @brief A frame-wide variable by name, as declarations name their dependencies (Frame/Declare.h). */
	template <class T>
	struct Var
	{
		std::string_view name;
	};

	/** @brief The frame-wide variables a declaration may depend on or read. */
	constexpr Var<Extent2D> DisplaySize() { return { "DisplaySize" }; }
	constexpr Var<Extent2D> RenderSize() { return { "RenderSize" }; }
	constexpr Var<Ratios> DynamicRatios() { return { "DynamicRatios" }; }
	constexpr Var<Upscaler> UpscaleMethod() { return { "UpscaleMethod" }; }
	constexpr Var<bool> HdrEnabled() { return { "HdrEnabled" }; }
	constexpr Var<TargetIdentity> HdrOutputTarget() { return { "HdrOutputTarget" }; }
	constexpr Var<ShadowMapShape> EngineShadowMap() { return { "EngineShadowMap" }; }
	constexpr Var<CubemapShape> ReflectionCubemap() { return { "ReflectionCubemap" }; }
	constexpr Var<HeightMapIdentity> HeightMap() { return { "HeightMap" }; }
	constexpr Var<SurfaceFormat> PresenterFormat() { return { "PresenterFormat" }; }

	/** @brief Any thread. The display size: the swap chain's back buffer and the engine's full-size targets. */
	bool SetDisplaySize(Extent2D a_value, std::string_view a_writer);
	/** @brief Any thread. The render size before upscaling. Constants only: no texture may be sized by it. */
	bool SetRenderSize(Extent2D a_value, std::string_view a_writer);
	/** @brief Any thread. The dynamic resolution ratios. Constants only: no texture may be sized by them. */
	bool SetDynamicRatios(Ratios a_value, std::string_view a_writer);
	/** @brief Any thread. The upscaling method in use. */
	bool SetUpscaleMethod(Upscaler a_value, std::string_view a_writer);
	/** @brief Any thread. Whether the frame is rendered for an HDR display. */
	bool SetHdrEnabled(bool a_value, std::string_view a_writer);
	/** @brief Any thread. The texture the post chain writes in HDR, or none. */
	bool SetHdrOutputTarget(TargetIdentity a_value, std::string_view a_writer);
	/** @brief Any thread. The engine's shadow map array. */
	bool SetEngineShadowMap(ShadowMapShape a_value, std::string_view a_writer);
	/** @brief Any thread. The engine's reflection cubemap. */
	bool SetReflectionCubemap(CubemapShape a_value, std::string_view a_writer);
	/** @brief Any thread. Terrain Shadows' heightmap. */
	bool SetHeightMap(HeightMapIdentity a_value, std::string_view a_writer);
	/** @brief Any thread. The presenter's committed surface format. Frame generation's hudless colour takes it in SDR. */
	bool SetPresenterFormat(SurfaceFormat a_value, std::string_view a_writer);
	/** @brief Any thread. What the upscaler SDK is configured with. */
	bool SetUpscalerOptions(UpscalerOptions a_value, std::string_view a_writer);
	/**
	* @brief Any thread. What the swap chain presents with. A revision that changes it is adopted only once the swap chain has it
	* (the presenter's input fragment, FrameGen::PresentationRevision).
	*/
	bool SetPresentationConfig(const FrameGen::PresentationConfig& a_value, std::string_view a_writer);
	/** @brief Any thread. The newest written presentation, which no revision may carry yet. For writers that change one field. */
	FrameGen::PresentationConfig WrittenPresentationConfig();

	/** @brief Render thread. The adopted display size. */
	Extent2D AdoptedDisplaySize();
	/** @brief Render thread. The adopted render size. */
	Extent2D AdoptedRenderSize();
	/** @brief Render thread. The adopted dynamic resolution ratios. */
	Ratios AdoptedDynamicRatios();
	/** @brief Render thread. The adopted upscaling method. */
	Upscaler AdoptedUpscaleMethod();
	/** @brief Render thread. Whether the adopted revision renders for an HDR display. */
	bool AdoptedHdrEnabled();
	/** @brief Render thread. The adopted HDR output target. */
	TargetIdentity AdoptedHdrOutputTarget();
	/** @brief Render thread. The adopted engine shadow map shape. */
	ShadowMapShape AdoptedEngineShadowMap();
	/** @brief Render thread. The adopted reflection cubemap shape. */
	CubemapShape AdoptedReflectionCubemap();
	/** @brief Render thread. The adopted Terrain Shadows heightmap. */
	HeightMapIdentity AdoptedHeightMap();
	/** @brief Render thread. The adopted presenter surface format. */
	SurfaceFormat AdoptedPresenterFormat();
	/** @brief Render thread. The adopted upscaler options. */
	UpscalerOptions AdoptedUpscalerOptions();
	/** @brief Render thread. The presentation of the adopted revision: the swap chain had it when the revision was adopted. */
	FrameGen::PresentationConfig AdoptedPresentationConfig();

	/**
	* @brief Any thread. One event on the feature's settings variable "FeatureSettings.<feature>", a serial. The variable is
	* declared on first use. a_writer names who wrote the settings.
	*/
	void RaiseFeatureSettings(std::string_view a_feature, std::string_view a_writer);

	/** @brief Any thread. How many events RaiseFeatureSettings raised for the feature this session. */
	std::uint64_t FeatureSettingsEvents(std::string_view a_feature);

	/**
	* @brief Any thread. One event on the variable of a CS::Setting, "Setting.<name>", declared on first use. It holds the
	* setting's value bytes (a_bytes, a_size), so a revision carries the value. Settings are told apart by id, so two settings with
	* one name get two variables. Matches CS::SettingRegistry::RaiseHook.
	*/
	void RaiseSetting(std::uint32_t a_settingId, std::string_view a_name, const void* a_bytes, std::size_t a_size);

	/** @brief Logs the "[ORG] variables:" line, events per variable in the window and the session, and resets the window. */
	void LogStats();
}

template <>
struct std::formatter<RenderGraph::Frame::Vars::Extent2D> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::Extent2D& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "{}x{}", a_value.width, a_value.height);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::Ratios> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::Ratios& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "({:.6g}, {:.6g})", a_value.x, a_value.y);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::Upscaler> : std::formatter<std::string_view>
{
	auto format(RenderGraph::Frame::Vars::Upscaler a_value, std::format_context& a_context) const
	{
		using enum RenderGraph::Frame::Vars::Upscaler;
		std::string_view name = "unknown";
		switch (a_value) {
		case None:
			name = "None";
			break;
		case TAA:
			name = "TAA";
			break;
		case FSR:
			name = "FSR";
			break;
		case DLSS:
			name = "DLSS";
			break;
		case XeSS:
			name = "XeSS";
			break;
		}
		return std::format_to(a_context.out(), "{}", name);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::TargetIdentity> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::TargetIdentity& a_value, std::format_context& a_context) const
	{
		if (!a_value.id)
			return std::format_to(a_context.out(), "none");
		return std::format_to(a_context.out(), "{:#x} {} format {}", a_value.id, a_value.size, a_value.format);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::ShadowMapShape> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::ShadowMapShape& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "{} format {} slices {}", a_value.size, a_value.format, a_value.slices);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::CubemapShape> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::CubemapShape& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "{}", a_value.size);
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::UpscalerOptions> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::UpscalerOptions& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "{} quality {} output {}{}", a_value.method, a_value.qualityMode, a_value.output, a_value.hdr ? " HDR" : "");
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::SurfaceFormat> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::SurfaceFormat& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "format {}{}", a_value.format, a_value.hdr10 ? " HDR10" : "");
	}
};

template <>
struct std::formatter<RenderGraph::Frame::Vars::HeightMapIdentity> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::HeightMapIdentity& a_value, std::format_context& a_context) const
	{
		if (!a_value.id)
			return std::format_to(a_context.out(), "none");
		return std::format_to(a_context.out(), "#{} {}", a_value.id, a_value.size);
	}
};
