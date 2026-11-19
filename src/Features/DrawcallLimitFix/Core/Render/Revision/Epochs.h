#pragma once

// A scene revision's epochs and their traits, and the views drawn by epochs of their own.
// Self-contained because features include it.

#include "Features/DrawcallLimitFix/Api/Epochs.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Api/Views.h"

#include "RenderGraph/RenderGraphRuntime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace DCLF::Draws
{
	// An epoch is its row index in the schedule, resolved once at the end of InstallFeatures.
	// Per-epoch arrays hold kMaxEpochs, and loops run to EpochCount().
	constexpr std::uint32_t kMaxEpochs = 12;
	constexpr std::uint32_t kNoEpoch = ~0u;

	/** @brief The views drawn by epochs of their own, such as the reflection faces and first person. */
	using ViewKind = Api::EngineView;
	constexpr std::size_t kViews = static_cast<std::size_t>(ViewKind::Count);
	constexpr std::size_t ViewIndex(ViewKind a_kind) { return static_cast<std::size_t>(a_kind); }

	constexpr std::size_t kDepthShape = static_cast<std::size_t>(Api::MainShape::Depth), kColourShape = static_cast<std::size_t>(Api::MainShape::Colour);
	// The shadow epoch has one shape, which holds both the shadow views and the occlusion maps.
	constexpr std::uint32_t kShadowShape = 0, kShadowShapes = 1;

	/** @brief What kind of shape an epoch records against. */
	enum class ShapeKind : std::uint8_t
	{
		Main,
		Shadow,
		View,
	};
	/**
	 * @brief An epoch's traits. sharesWith is always an earlier epoch. A hosted epoch has no recordings of its own.
	 * Its passes are recorded inside its host's recordings, and the host lists it in hostsMask.
	 */
	struct EpochTraits
	{
		RenderGraphRuntime::Segment segment{};
		std::string_view name;
		ShapeKind shape = ShapeKind::Main;
		std::uint32_t index = 0;
		std::uint32_t sharesWith = kNoEpoch;
		Api::ObjectClass gate = Api::ObjectClass::Count;
		bool claims = false;
		std::uint32_t hostedBy = kNoEpoch;
		std::uint32_t hostsMask = 0;
	};
	static_assert(kMaxEpochs <= 32, "EpochTraits::hostsMask (and a host commit's drawn bits) hold an epoch a bit");
	/** @brief The number of epochs. Zero before InstallFeatures. Any thread. */
	std::uint32_t EpochCount();
	/** @brief Epoch a_epoch's traits. Any thread, after InstallFeatures. */
	const EpochTraits& EpochTraitsOf(std::uint32_t a_epoch);
	/** @brief The epoch that records a shape itself rather than sharing it. Any thread, after InstallFeatures. */
	std::uint32_t EpochOfShape(ShapeKind a_shape, std::uint32_t a_index);
	/** @brief The epoch with id a_id, or kNoEpoch. This is a search, so look it up once. */
	std::uint32_t EpochById(std::string_view a_id);
	/** @brief A view's epochs: the one recording its shape, then the one sharing it, or kNoEpoch. */
	struct ViewEpochs
	{
		std::array<std::uint32_t, 2> epochs{ kNoEpoch, kNoEpoch };
		std::array<RenderGraphRuntime::Segment, 2> segments{};
	};
	/** @brief a_kind's epochs. Any thread, after InstallFeatures. */
	const ViewEpochs& ViewEpochsOf(ViewKind a_kind);
}
