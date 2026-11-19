#pragma once

// An immutable value a feature builds with every main payload, carried with the snapshot and installed with it on the render thread.

#include <memory>
#include <string_view>

namespace DCLF::Api
{
	// Defined in Core/Extensions/PayloadProducts.h.
	class PayloadProductBuild;

	/**
	 * @brief A payload product.
	 *
	 * - build: snapshot builder, at the end of each main build. The result is immutable and may be null.
	 *   It may hold engine pointers only while the payload's publication holds them. If nothing moved, return the previous result.
	 * - install: render thread, when the frame adopts a new payload. The product stays valid until the next install.
	 */
	struct PayloadProduct
	{
		std::string_view name;
		std::shared_ptr<const void> (*build)(const PayloadProductBuild& a_build) = nullptr;
		void (*carried)(const std::shared_ptr<const void>& a_product) = nullptr;
		void (*install)(std::shared_ptr<const void> a_product) = nullptr;
	};
}
