#include "RenderGraph/Frame/EngineTargetUse.h"

#include "RenderGraph/Engine/Native/Native.h"

#include <array>
#include <atomic>

namespace RenderGraph::Frame::EngineTargetUse
{
	namespace
	{
		using T = RE::RENDER_TARGET;

		/** @brief A render target native mode keeps without memory, and why nothing uses it. */
		struct Colour
		{
			std::uint32_t target;
			bool dropClears;    // the engine still clears it; the clear is declared dropped
			const char* users;  // who would use it, and what they use instead
		};

		// The engine targets nothing in native mode uses. Each line names what replaced the engine's use. Every target not listed
		// here is backed: the stages and DCLF declare it, or an old native path still reaches it ("State of the native frame").
		constexpr std::array kColours{
			Colour{ T::kSAO, false, "none: the image-space port draws SAO into ImageSpace::SAO (Frame::Resources)" },
			Colour{ T::kSAO_RAWAO, false, "none: the raw AO history is ImageSpace::SAORawAO0/1 (Frame::Resources)" },
			Colour{ T::kSAO_RAWAO_PREVIOUS, false, "none: the raw AO history is ImageSpace::SAORawAO0/1 (Frame::Resources)" },
			Colour{ T::kSAO_TEMP_BLUR, false, "none: the SAO blur draws into ImageSpace::SAOBlurTemp" },
			Colour{ T::kTEMPORAL_AA_ACCUMULATION_1, true, "the engine's clear at a TAA reset only (dropped); TAA's history is ImageSpace::TAAHistory0/1 (Frame::Resources)" },
			Colour{ T::kTEMPORAL_AA_ACCUMULATION_2, true, "the engine's clear at a TAA reset only (dropped); TAA's history is ImageSpace::TAAHistory0/1 (Frame::Resources)" },
			Colour{ T::kTEMPORAL_AA_UI_ACCUMULATION_1, true, "the engine's clear at a TAA reset only (dropped); TAA's UI form (a menu scene's) has no CS pass (NoPass)" },
			Colour{ T::kTEMPORAL_AA_UI_ACCUMULATION_2, true, "the engine's clear at a TAA reset only (dropped); TAA's UI form (a menu scene's) has no CS pass (NoPass)" },
			Colour{ T::kIMAGESPACE_TEMP_COPY, false, "none: the post chain works in place, with ImageSpace::HdrScratch for its copies" },
			Colour{ T::kIMAGESPACE_TEMP_COPY2, false, "none: the post chain works in place, with ImageSpace::LdrScratch for its copies" },
			Colour{ T::kGETHIT_BUFFER, false, "none: the get-hit step draws into ImageSpace::GetHitA/B" },
			Colour{ T::kGETHIT_BLURSWAP, false, "none: the get-hit step draws into ImageSpace::GetHitSwap" },
			Colour{ T::kLDR_BLURSWAP, false, "none: depth of field draws into ImageSpace::DofBlurSwap" },
			Colour{ T::kLDR_DOWNSAMPLE0, false, "none: depth of field draws into ImageSpace::DofDownsample" },
			Colour{ T::kNORMAL_TAAMASK_SSRMASK_DOWNSAMPLED, true, "the engine's frame clear only (dropped); SSR reads ImageSpace::SSRMaskDownsampled" },
		};

		/** @brief An engine 3D target native mode keeps without memory, by the name the engine gives it. */
		struct Volume
		{
			std::string_view label;
			const char* users;
		};

		constexpr std::array kVolumes{
			Volume{ "VOLUMETRIC_LIGHTING_ACCUMULATION", "none: the volumetric lighting port draws VolumetricLighting::Volume" },
			Volume{ "VOLUMETRIC_LIGHTING_ACCUMULATION_COPY", "none: the volumetric lighting port draws VolumetricLighting::Volume" },
		};

		bool Native() { return Engine::Native::Requested(); }

		const Colour* FindColour(std::uint32_t a_target)
		{
			for (const auto& entry : kColours)
				if (entry.target == a_target)
					return &entry;
			return nullptr;
		}

		const Volume* FindVolume(std::string_view a_label)
		{
			for (const auto& entry : kVolumes)
				if (entry.label == a_label)
					return &entry;
			return nullptr;
		}
	}

	bool ColourUnbacked(std::uint32_t a_target)
	{
		return Native() && FindColour(a_target);
	}

	bool ColourClearsDropped(std::uint32_t a_target)
	{
		const auto* entry = Native() ? FindColour(a_target) : nullptr;
		return entry && entry->dropClears;
	}

	bool VolumeUnbacked(std::string_view a_label)
	{
		return Native() && FindVolume(a_label);
	}

	std::string_view ColourReason(std::uint32_t a_target)
	{
		const auto* entry = Native() ? FindColour(a_target) : nullptr;
		return entry ? entry->users : "";
	}

	std::string_view VolumeReason(std::string_view a_label)
	{
		const auto* entry = Native() ? FindVolume(a_label) : nullptr;
		return entry ? entry->users : "";
	}

	void LogOnce()
	{
		static std::atomic_flag logged;
		if (!Native() || logged.test_and_set())
			return;
		std::string list;
		for (const auto& entry : kColours)
			list += fmt::format("{}{}", list.empty() ? "" : ", ", magic_enum::enum_name(static_cast<T>(entry.target)));
		for (const auto& entry : kVolumes)
			list += fmt::format(", 3D {}", entry.label);
		logger::info("[ORG native] engine targets without memory (Frame/EngineTargetUse.cpp; any GPU use is a recipe mismatch): {}", list);
	}
}
