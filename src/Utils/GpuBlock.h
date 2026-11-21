#pragma once

// A feature's GPU settings block, filled from its CPU settings through a field map.
// A CS::Setting field is read at its adopted value, so the constants follow the same revision as the frame's shape.
// A plain field is read as it is: it does not shape the frame, so its live value is the one the frame uses.
// No ORG or feature includes here. Feature headers may include this file.

#include "Utils/Setting.h"

#include <type_traits>

namespace CS
{
	/** @brief How a mapped field is converted on its way to the GPU block. */
	enum class GpuConvert
	{
		Copy,           // the value as it is
		SRGBToWorking,  // a colour: the block's colour hook converts its RGB from sRGB to the working space
	};

	/** @brief One entry of a GpuBlock map: the settings member a_Source goes to the GPU member a_Target. */
	template <auto a_Source, auto a_Target, GpuConvert a_Convert = GpuConvert::Copy>
	struct GpuField
	{};

	/** @brief A colour entry: copied, then converted from sRGB to the working space by the block's colour hook. */
	template <auto a_Source, auto a_Target>
	using GpuColour = GpuField<a_Source, a_Target, GpuConvert::SRGBToWorking>;

	namespace Detail
	{
		template <class T>
		struct IsSetting : std::false_type
		{};

		template <class T>
		struct IsSetting<Setting<T>> : std::true_type
		{};

		/** @brief A setting's adopted value, or a plain field's value. */
		template <class T>
		decltype(auto) GpuSource(const T& a_value)
		{
			if constexpr (IsSetting<T>::value)
				return a_value.Adopted();
			else
				return (a_value);
		}
	}

	/**
	 * @brief The GPU block Gpu, built from a feature's settings by the map Fields (GpuField entries).
	 *
	 * Fields not in the map keep Gpu's default. A colour hook takes a float pointer to the colour's first channel and converts
	 * its RGB in place (LinearLighting::SRGBToWorking). Any thread, as Setting::Adopted is.
	 */
	template <class Gpu, class... Fields>
	struct GpuBlock
	{
		/** @brief Fills a block from a_settings. a_colour converts each colour entry in place. */
		template <class Settings, class ColourHook>
		static Gpu Fill(const Settings& a_settings, ColourHook&& a_colour)
		{
			Gpu block{};
			(Apply(Fields{}, a_settings, block, a_colour), ...);
			return block;
		}

		/** @brief Fills a block from a_settings, for a map with no colour entries. */
		template <class Settings>
		static Gpu Fill(const Settings& a_settings)
		{
			static_assert(!(IsColour(Fields{}) || ...), "a map with colour entries needs a colour hook");
			return Fill(a_settings, [](float*) {});
		}

	private:
		template <auto a_Source, auto a_Target, GpuConvert a_Convert>
		static constexpr bool IsColour(GpuField<a_Source, a_Target, a_Convert>)
		{
			return a_Convert == GpuConvert::SRGBToWorking;
		}

		template <auto a_Source, auto a_Target, GpuConvert a_Convert, class Settings, class ColourHook>
		static void Apply(GpuField<a_Source, a_Target, a_Convert>, const Settings& a_settings, Gpu& a_block, ColourHook& a_colour)
		{
			auto& target = a_block.*a_Target;
			using Target = std::remove_cvref_t<decltype(target)>;
			target = static_cast<Target>(Detail::GpuSource(a_settings.*a_Source));
			if constexpr (a_Convert == GpuConvert::SRGBToWorking)
				a_colour(&target.x);
		}
	};
}
