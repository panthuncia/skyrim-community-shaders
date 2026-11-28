#pragma once

// Parts of the frame-value pool that its stats line shares. Included by TextureTransformsPool.cpp and the report only.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"
#include "Features/DrawcallLimitFix/Integrations/LinearLighting/LinearLighting.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace DCLF::Features::TextureTransforms
{
	// The game half at a ticket's frame start: its materials are held by the publication the render installed, released no earlier than
	// the next capture. a_space is Linear Lighting's working space this frame, null while it is off.
	inline void Read(const TextureTransformList::Entry& a_entry, std::size_t a_buffer, const Integrations::LinearLighting::WorkingSpace* a_space, float (&a_values)[4])
	{
		const auto* material = a_entry.material;
		if (const auto* components = a_entry.components) {
			const auto* bytes = reinterpret_cast<const std::byte*>(material);
			for (std::size_t c = 0; c < 4; ++c)
				std::memcpy(&a_values[c], bytes + components[c].source, sizeof(float));
			// Linear Lighting converts the colour before the scale, as its upload hook does.
			if (a_space && a_entry.workingColour)
				Integrations::LinearLighting::ToWorking(*a_space, a_values);
			for (std::size_t c = 0; c < 4; ++c) {
				if (const auto& component = components[c]; component.scale != Api::MaterialFrameComponent::kNoScale) {
					float scale;
					std::memcpy(&scale, bytes + component.scale, sizeof(scale));
					a_values[c] = scale * a_values[c];
				}
			}
			return;
		}
		a_values[0] = material->texCoordOffset[a_buffer].x;
		a_values[1] = material->texCoordOffset[a_buffer].y;
		a_values[2] = material->texCoordScale[a_buffer].x;
		a_values[3] = material->texCoordScale[a_buffer].y;
	}

	// The line's counts every frame, at the end of the producer's sample.
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused, std::size_t a_variables, std::uint64_t a_variablesChanged);
}
