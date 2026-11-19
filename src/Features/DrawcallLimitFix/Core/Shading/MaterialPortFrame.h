#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d11.h>
#include <winrt/base.h>

#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialHookFrame.h"
#include "Features/DrawcallLimitFix/Core/Shading/MaterialPortVanilla.h"

namespace DCLF::MaterialPort
{
	/**
	 * @brief A witness of everything the signature-sourced frame components read, and the gates their evaluation passes.
	 * Two frames with equal witnesses give every signature the same frame components, so signatures are only re-probed when it moves.
	 * Values that alternate every frame, the transform buffer and the character light's target, are left out on purpose.
	 */
	struct FrameComponentsWitness
	{
		// 13 vanilla words, 5 hook words, the contributors' active bits and their witness words.
		std::array<std::uint32_t, 19 + Extensions::kMaterialContributorWitnessWords> words{};
		bool operator==(const FrameComponentsWitness&) const = default;
	};

	/**
	 * @brief References on every view the frame's sources hand an evaluation, taken where the render thread samples them.
	 * Records name those views as bare pointers, so whatever keeps the record keeps this. Shared between frames while unchanged.
	 */
	struct FrameViewHold
	{
		std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> views;
		std::vector<std::shared_ptr<const void>> contributors;  // the samples' held tables
	};

	/** @brief The per-frame sources SetupMaterial reads, sampled on the render thread at the frame's start. */
	struct MaterialFrame
	{
		VanillaFrame vanilla;
		HookFrame hook;
		Extensions::MaterialContributorSamples contributors;
		FrameComponentsWitness components;
		std::shared_ptr<const FrameViewHold> held;  // the references on the views above
	};

	/** @brief Pure. a_frame's witness. */
	FrameComponentsWitness WitnessOf(const VanillaFrame& a_vanilla, const HookFrame& a_hook, const Extensions::MaterialContributorSamples& a_contributors);
}
