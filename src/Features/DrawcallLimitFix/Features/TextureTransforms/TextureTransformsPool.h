#pragma once

// Parts of the frame-value pool that its diagnostics share. Included by TextureTransformsPool.cpp and the parity only.

#include "Features/DrawcallLimitFix/Features/TextureTransforms/TextureTransforms.h"
#include "Features/DrawcallLimitFix/Api/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineReadWindow.h"
#include "Features/DrawcallLimitFix/Core/Extensions/FrameValuePools.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace DCLF::Features::TextureTransforms
{
	// Read only while this producer's Kick is the newest. Its materials are held by the publication its frame installed, released no earlier than the next frame's Present.
	inline bool Readable(const Api::FrameValuePoolProducer& a_producer, EngineReadWindow::Lease& a_lease) { return a_lease && a_producer.Newest(); }

	inline void Read(const TextureTransformList::Entry& a_entry, std::size_t a_buffer, float (&a_values)[4])
	{
		const auto* material = a_entry.material;
		if (const auto* components = a_entry.components) {
			const auto* bytes = reinterpret_cast<const std::byte*>(material);
			for (std::size_t c = 0; c < 4; ++c) {
				const auto& component = components[c];
				std::memcpy(&a_values[c], bytes + component.source, sizeof(float));
				if (component.scale != Api::MaterialFrameComponent::kNoScale) {
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

	// The parity on parity frames and the line's counts every frame, at the end of the producer's sample.
	void CheckPoolParity(const Api::FrameValuePoolProducer& a_producer, const TextureTransformList* a_list, std::size_t a_count, std::size_t a_buffer,
		const std::vector<TextureTransformRow>& a_out, const std::vector<const RE::BSShaderMaterial*>& a_materials);
	void CountPoolLine(std::size_t a_count, std::uint64_t a_changed, std::uint64_t a_refused, std::size_t a_variables, std::uint64_t a_variablesChanged);
}
