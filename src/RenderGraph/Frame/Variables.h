#pragma once

#include "Features/Upscaling/PresentationConfig.h"
#include "RenderGraph/Engine/Targets/TargetRef.h"
#include "Utils/Setting.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

/**
* @brief The frame-wide variables (frame revision plan, step 5b).
*
* A variable is a typed, named input of a frame revision. A write that changes its value is an event. It wakes the revision
* lane, and the next frame revision carries the new value. The frame reads the value of the revision it adopted last, never the
* newest written one.
*
* The variables are the rows of one table (kTable). Set and Raise work from any thread and return whether the value changed.
* Writing the same value is no event, so writers may write every frame. Each event logs one line
* "[ORG] variable <name>: <old> -> <new> (<writer>)", rate-limited per variable.
*
* Adopted is for the render thread. It returns the value of the last adopted revision, or the initial value before the first
* adoption. OnAdopted runs listeners at the adoptions that change a value. CS::Setting::Adopted is the same for a setting, from
* any thread.
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

	/** @brief A 3D texture's width, height and depth in texels. */
	struct Extent3D
	{
		std::uint32_t width = 0;
		std::uint32_t height = 0;
		std::uint32_t depth = 0;
		friend bool operator==(const Extent3D&, const Extent3D&) = default;
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

	/** @brief An engine shadow map array: kSHADOWMAPS (EngineShadowMap) or kSHADOWMAPS_ESRAM (ShadowCascades). */
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

	/**
	* @brief The engine's ImageSpace shape: the loops its effects run and the water's textures (frame revision plan, 6.7).
	* ImageSpace raises it at events: at Setup, at each Begin until the engine has made its ImageSpace, when a hook first sees an
	* effect object, and when the hook that runs a part of the shape sees it change (ImageSpace.cpp, RaiseShape).
	*/
	struct ImageSpaceShape
	{
		std::uint32_t luminanceLevels = 0;   // the HDR effect's luminance chain, from kHDR_DOWNSAMPLE0's size and two engine flags
		std::uint32_t saoLevels = 0;         // RenderSSAO's level count
		std::uint32_t displacementSize = 0;  // kWATER_DISPLACEMENT's width, 0 before the engine made it
		std::uint32_t flowCopySize = 0;      // the width of the flow map the flow copies write, 0 for none
		std::uint64_t flowMap = 0;           // the water's flow map texture, 0 for none
		std::uint32_t effects = 0;           // the covered effects the engine has made, 0 before its ImageSpace manager exists
		std::uint32_t states = 0;            // the engine blend states the steps use that it has made
		std::uint32_t heightmapSteps = 0;    // the most wading heightmap steps one water update runs, from the engine timer
		friend bool operator==(const ImageSpaceShape&, const ImageSpaceShape&) = default;
	};

	/** @brief What the presenter's surface takes: the swap chain's format, and whether it presents HDR10. */
	struct SurfaceFormat
	{
		std::uint32_t format = 0;  // DXGI_FORMAT, 0 before the presenter committed one
		bool hdr10 = false;
		friend bool operator==(const SurfaceFormat&, const SurfaceFormat&) = default;
	};

	/** @brief The index of a variable with no row in kTable, such as one derived from a setting (Frame/Declare.h). */
	inline constexpr std::uint32_t kNoTableIndex = UINT32_MAX;

	/**
	* @brief A frame-wide variable, as declarations name their dependencies (Frame/Declare.h) and readers and writers name it.
	* A row of kTable has its index there. A derived variable has only its name.
	*/
	template <class T>
	struct Var
	{
		std::string_view name;
		std::uint32_t index = kNoTableIndex;
		bool sizes = true;  // false: constants only, no texture or buffer may be sized by it (decision 6a)
	};

	namespace Detail
	{
		/** @brief One row of kTable: a variable's type, name and traits. Every variable starts at T{}. */
		template <class T>
		struct Row
		{
			using Type = T;
			std::string_view name;
			bool sizes = true;
		};
	}

	/**
	* @brief The frame-wide variables, one row each. A variable's index is its row. The variable set declares them in this order
	* (Variables.cpp). Readers and writers name them with the functions below.
	*/
	inline constexpr std::tuple kTable{
		Detail::Row<Extent2D>{ "DisplaySize" },
		// Decision 6a: render size and ratios only reach constants and viewports.
		Detail::Row<Extent2D>{ "RenderSize", false },
		Detail::Row<Ratios>{ "DynamicRatios", false },
		Detail::Row<Upscaler>{ "UpscaleMethod" },
		Detail::Row<bool>{ "HdrEnabled" },
		Detail::Row<TargetIdentity>{ "HdrOutputTarget" },
		Detail::Row<ShadowMapShape>{ "EngineShadowMap" },
		Detail::Row<ShadowMapShape>{ "ShadowCascades" },
		Detail::Row<CubemapShape>{ "ReflectionCubemap" },
		Detail::Row<HeightMapIdentity>{ "HeightMap" },
		Detail::Row<SurfaceFormat>{ "PresenterFormat" },
		Detail::Row<UpscalerOptions>{ "UpscalerOptions" },
		Detail::Row<ImageSpaceShape>{ "EngineImageSpace" },
		Detail::Row<FrameGen::PresentationConfig>{ "PresentationConfig" },
		Detail::Row<bool>{ "Interior" },
		// Sizes nothing: a reload changes pipelines only.
		Detail::Row<std::uint64_t>{ "ShaderGeneration", false },
		// Every engine target's recorded shape. The engine target set writes it at each set; the targets' declarations depend on it.
		Detail::Row<::RenderGraph::Engine::Targets::ShapeTable>{ "EngineTargetShapes" },
		Detail::Row<Extent3D>{ "VolumetricLightingSize" },
	};

	inline constexpr std::size_t kTableSize = std::tuple_size_v<std::remove_cvref_t<decltype(kTable)>>;

	namespace Detail
	{
		template <std::size_t I>
		using RowType = typename std::remove_cvref_t<decltype(std::get<I>(kTable))>::Type;

		/** @brief The row named a_name as a Var<T>. Not compiled for a name with no row or a row of another type. */
		template <class T, std::size_t... I>
		consteval Var<T> TableVar(std::string_view a_name, std::index_sequence<I...>)
		{
			Var<T> found{ a_name };
			bool typed = false;
			const auto match = [&](std::uint32_t a_index, std::string_view a_row, bool a_sizes, bool a_typed) {
				if (a_row != a_name)
					return;
				found.index = a_index;
				found.sizes = a_sizes;
				typed = a_typed;
			};
			(match(static_cast<std::uint32_t>(I), std::get<I>(kTable).name, std::get<I>(kTable).sizes, std::is_same_v<T, RowType<I>>), ...);
			if (found.index == kNoTableIndex || !typed)
				throw "no row of kTable has this name and type";
			return found;
		}

		template <class T>
		consteval Var<T> TableVar(std::string_view a_name)
		{
			return TableVar<T>(a_name, std::make_index_sequence<kTableSize>{});
		}
	}

	/** @brief The display size: the swap chain's back buffer and the engine's full-size targets. */
	constexpr Var<Extent2D> DisplaySize() { return Detail::TableVar<Extent2D>("DisplaySize"); }
	/** @brief The render size before upscaling. Constants only. */
	constexpr Var<Extent2D> RenderSize() { return Detail::TableVar<Extent2D>("RenderSize"); }
	/** @brief The dynamic resolution ratios. Constants only. */
	constexpr Var<Ratios> DynamicRatios() { return Detail::TableVar<Ratios>("DynamicRatios"); }
	/** @brief The upscaling method in use. */
	constexpr Var<Upscaler> UpscaleMethod() { return Detail::TableVar<Upscaler>("UpscaleMethod"); }
	/** @brief Whether the frame is rendered for an HDR display. */
	constexpr Var<bool> HdrEnabled() { return Detail::TableVar<bool>("HdrEnabled"); }
	/** @brief The texture the post chain writes in HDR, or none. */
	constexpr Var<TargetIdentity> HdrOutputTarget() { return Detail::TableVar<TargetIdentity>("HdrOutputTarget"); }
	/** @brief The engine's shadow map array. */
	constexpr Var<ShadowMapShape> EngineShadowMap() { return Detail::TableVar<ShadowMapShape>("EngineShadowMap"); }
	/** @brief The engine's cascade shadow maps (kSHADOWMAPS_ESRAM), which Volumetric Shadows reads. */
	constexpr Var<ShadowMapShape> ShadowCascades() { return Detail::TableVar<ShadowMapShape>("ShadowCascades"); }
	/** @brief The engine's reflection cubemap. */
	constexpr Var<CubemapShape> ReflectionCubemap() { return Detail::TableVar<CubemapShape>("ReflectionCubemap"); }
	/** @brief Terrain Shadows' heightmap. */
	constexpr Var<HeightMapIdentity> HeightMap() { return Detail::TableVar<HeightMapIdentity>("HeightMap"); }
	/** @brief The presenter's committed surface format. Frame generation's hudless colour takes it in SDR. */
	constexpr Var<SurfaceFormat> PresenterFormat() { return Detail::TableVar<SurfaceFormat>("PresenterFormat"); }
	/** @brief What the upscaler SDK context is made for. Named ...Var, since the struct has the plain name. */
	constexpr Var<UpscalerOptions> UpscalerOptionsVar() { return Detail::TableVar<UpscalerOptions>("UpscalerOptions"); }
	/** @brief The engine's ImageSpace shape. */
	constexpr Var<ImageSpaceShape> EngineImageSpace() { return Detail::TableVar<ImageSpaceShape>("EngineImageSpace"); }
	/**
	* @brief What the swap chain presents with. A revision that changes it is adopted only once the swap chain has it (the
	* presenter's input producer, FrameGen::PresentationRevision).
	*/
	constexpr Var<FrameGen::PresentationConfig> PresentationConfig() { return Detail::TableVar<FrameGen::PresentationConfig>("PresentationConfig"); }
	/** @brief Whether the player is in an interior cell. The frame driver's engine sampler writes it at each Begin. */
	constexpr Var<bool> Interior() { return Detail::TableVar<bool>("Interior"); }
	/**
	* @brief The pipeline registry generation of a shader reload's new versions. The revision that carries a new value is adopted
	* once every graph is built with them, and each drawing fragment source's fragment is recorded on its rebuilt graphs
	* (RenderGraph/PipelineVersions.h). Until then frames bind the old versions. Constants only.
	*/
	constexpr Var<std::uint64_t> ShaderGeneration() { return Detail::TableVar<std::uint64_t>("ShaderGeneration"); }
	/**
	* @brief Every engine target's shape as its create function got it (Engine/Targets/EngineTargetSet.h). Written at each target
	* set. A change makes the changed targets' next versions, and the set points the engine's field handles at them at adoption.
	*/
	constexpr Var<::RenderGraph::Engine::Targets::ShapeTable> EngineTargetShapes() { return Detail::TableVar<::RenderGraph::Engine::Targets::ShapeTable>("EngineTargetShapes"); }
	/**
	* @brief The engine's volumetric lighting volume size, from the VL descriptor's +0x30 to +0x38. The frame driver's engine
	* sampler writes it at each Begin. The engine sets it from the INI's quality when its ImageSpace is made, after CS's setup.
	* Later only a quality change sets it: the console's volumetric lighting command, or CS's feature while it is on.
	*/
	constexpr Var<Extent3D> VolumetricLightingSize() { return Detail::TableVar<Extent3D>("VolumetricLightingSize"); }

	/** @brief Any thread. Writes a_value to a row of kTable. Returns whether it changed, which is an event. */
	template <class T>
	bool Set(Var<T> a_variable, const T& a_value, std::string_view a_writer);

	/** @brief Any thread. One event on a serial row of kTable: its value goes up by one. A client's own serial is a CS::Setting. */
	void Raise(Var<std::uint64_t> a_variable, std::string_view a_writer);

	/** @brief Any thread. The newest written value, which no revision may carry yet. For writers that change one field. */
	template <class T>
	T Written(Var<T> a_variable);

	/** @brief Render thread. The value of the last adopted revision, or T{} before the first adoption. */
	template <class T>
	T Adopted(Var<T> a_variable);

	/**
	* @brief Render thread. The dynamic-resolution part of the adopted display size, in pixels, as Util::ConvertToDynamic gives it
	* from the engine's live ratios. A per-frame value for a commit's constants, viewports and dispatch sizes. Never put it in a key.
	*/
	float2 DynamicArea(bool a_ignoreLock = false);

	/**
	* @brief Any thread. a_listener runs on the render thread inside the adoption point of every revision that changes the adopted
	* value of a_variable, with the value before and the value now. A variable may have many listeners. They run in the order they
	* were added, once the revision's variables are the frame's and before its versions are. Work outside the frame that a change
	* needs goes in an input producer instead (Revision::RegisterInputProducer).
	*/
	template <class T>
	void OnAdopted(Var<T> a_variable, std::function<void(const T& a_before, const T& a_now)> a_listener);

	namespace Detail
	{
		/** @brief The type-erased OnAdopted of a CS::Setting: a_listener takes the value's bytes before and now. */
		void OnSettingAdopted(CS::SettingVariableId a_id, std::size_t a_size, std::function<void(const void* a_before, const void* a_now)> a_listener);
	}

	/**
	* @brief Any thread. a_listener runs as OnAdopted's does, at every adoption that changes a_setting's adopted value
	* (CS::Setting::Adopted). The setting must be bound.
	*/
	template <class T>
	void OnAdopted(const CS::Setting<T>& a_setting, std::function<void(const T& a_before, const T& a_now)> a_listener)
	{
		static_assert(sizeof(T) <= CS::kAdoptedSettingCapacity, "a frame variable carries settings of 32 bytes or fewer");
		Detail::OnSettingAdopted(a_setting.Id(), sizeof(T), [listener = std::move(a_listener)](const void* a_before, const void* a_now) {
			T before{}, now{};
			std::memcpy(&before, a_before, sizeof(T));
			std::memcpy(&now, a_now, sizeof(T));
			listener(before, now);
		});
	}

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
struct std::formatter<RenderGraph::Frame::Vars::Extent3D> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::Extent3D& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "{}x{}x{}", a_value.width, a_value.height, a_value.depth);
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
struct std::formatter<RenderGraph::Frame::Vars::ImageSpaceShape> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Frame::Vars::ImageSpaceShape& a_value, std::format_context& a_context) const
	{
		return std::format_to(a_context.out(), "luminance {} SAO {} displacement {} flow copies {} flow map {:#x} effects {} states {}", a_value.luminanceLevels,
			a_value.saoLevels, a_value.displacementSize, a_value.flowCopySize, a_value.flowMap, a_value.effects, a_value.states);
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

template <>
struct std::formatter<RenderGraph::Engine::Targets::ShapeTable> : std::formatter<std::string_view>
{
	auto format(const RenderGraph::Engine::Targets::ShapeTable& a_value, std::format_context& a_context) const
	{
		std::uint32_t known = 0;
		for (const auto& shape : a_value.colours)
			known += shape.width ? 1u : 0u;
		for (const auto& shape : a_value.depths)
			known += shape.width ? 1u : 0u;
		for (const auto& shape : a_value.cubes)
			known += shape.width ? 1u : 0u;
		for (const auto& shape : a_value.volumes)
			known += shape.width ? 1u : 0u;
		const auto& main = a_value.colours[1];  // RE::RENDER_TARGET::kMAIN
		return std::format_to(a_context.out(), "{} targets, kMAIN {}x{}", known, main.width, main.height);
	}
};
