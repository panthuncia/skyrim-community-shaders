#pragma once

#include "RenderGraph/FrameAnchors.h"

#include <winrt/base.h>

#include <cstdint>

struct ID3D11ShaderResourceView;

namespace RenderGraph
{
	/**
	* @brief A depth view a pass reads, in the form a client's key can hold: the engine depth target it is, or the view itself.
	* Find it at Prepass with Of() and observe it. Import it in the key with Resolve(), inside the adoption point, as imports must be.
	* Render thread.
	*/
	struct DepthViewSource
	{
		std::uint32_t engineDepth = UINT32_MAX;                // the engine depth target, UINT32_MAX for none
		winrt::com_ptr<ID3D11ShaderResourceView> other;      // a view that is no engine target

		explicit operator bool() const { return engineDepth != UINT32_MAX || other; }
		bool operator==(const DepthViewSource& a_other) const { return engineDepth == a_other.engineDepth && other.get() == a_other.other.get(); }

		/**
		* @brief What a_view is. A field handle is identified by the target set. The view may also be DXVK's own object, which the set
		* cannot identify, so it is compared with the engine's depth targets by what they wrap. Empty for null.
		*/
		static DepthViewSource Of(ID3D11ShaderResourceView* a_view);

		/** @brief The graph texture: the engine target's slot, which never changes, or the view imported as a_id. Null when empty. */
		FrameAnchors::Texture Resolve(const char* a_id) const;
	};
}
