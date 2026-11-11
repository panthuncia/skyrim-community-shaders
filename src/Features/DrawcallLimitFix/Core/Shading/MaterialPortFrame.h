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
	 * @brief What of a frame's sources the signature-sourced frame components read (MaterialSources::ApplyFrameComponents: PS 6,
	 * 24.z, 31.zw, 34 and 35, t11's writtenness and modes) and the gates their evaluation passes (the shader known, the technique
	 * remap, the contributors active and their witness words: Advanced Skin's keys), as the port uses them: two frames with equal
	 * witnesses give every signature's probe the same frame components (Coordinator::RefreshMaterialSignatures evaluates again only
	 * when it moves).
	 *
	 * Left out, as no signature application reads them: what alternates frame to frame, the texture-transform buffer (VS 11, the
	 * transform watch's: RefreshMaterialTransforms) and the character light's render target (its index within the array and every
	 * view: a record holds no t11 view); IBLParams (a record keeps its first evaluation's); the default textures (the material's
	 * own texture choices, no frame component).
	 */
	struct FrameComponentsWitness
	{
		// 13 vanilla, 5 hook, the contributors' active bits and their kMaterialContributorWitnessWords (WitnessOf).
		std::array<std::uint32_t, 19 + Extensions::kMaterialContributorWitnessWords> words{};
		bool operator==(const FrameComponentsWitness&) const = default;
	};

	/**
	 * @brief References on every view a frame's sources hand an evaluation (render targets, default textures, the contributors' held
	 * tables), taken by the render thread where it samples them. A record made from the frame names those views as bare pointers;
	 * whatever keeps the record (a material slot's pins) keeps this. Shared between frames while the views are unchanged.
	 */
	struct FrameViewHold
	{
		std::vector<winrt::com_ptr<ID3D11ShaderResourceView>> views;
		std::vector<std::shared_ptr<const void>> contributors;  // the samples' held tables (Advanced Skin's holds its views)
	};

	/** @brief The per-frame sources SetupMaterial reads (sampled by the render thread at the frame's start: FrameGlobals::material). */
	struct MaterialFrame
	{
		VanillaFrame vanilla;  // MaterialPortVanilla.h
		HookFrame hook;        // MaterialHookFrame.h
		Extensions::MaterialContributorSamples contributors;  // the material contributors' samples (Core/Extensions/Materials.h)
		FrameComponentsWitness components;  // of the three (SampleFrame)
		std::shared_ptr<const FrameViewHold> held;  // the references on the views above (SampleFrame)
	};

	/** @brief Pure: a_frame's witness (SampleFrame sets MaterialFrame::components from it). */
	FrameComponentsWitness WitnessOf(const VanillaFrame& a_vanilla, const HookFrame& a_hook, const Extensions::MaterialContributorSamples& a_contributors);
}
