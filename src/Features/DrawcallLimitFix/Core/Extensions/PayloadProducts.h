#pragma once

// Core's side of Api/Payload.h: what a product's build reads, the products a payload carries, and the calls that build, carry and install them.

#include "Features/DrawcallLimitFix/Api/Payload.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace DCLF
{
	struct SceneTables;
	namespace Draws
	{
		struct MainRows;
	}
}

namespace DCLF::Api
{
	/**
	 * @brief What a product's build reads on the snapshot builder, read-only, and what moved since its previous build.
	 * Rewrite only what moved, and return the previous product when nothing did.
	 */
	class PayloadProductBuild
	{
	public:
		const Draws::MainRows& rows;   // as this build left them
		const SceneTables& tables;     // the publication's tables the build read
		// Material slots that may have moved since previous was built, unique and in any order. Every slot when there is no previous.
		std::span<const std::uint32_t> dirtyMaterials;
		bool everyMaterial = true;
		const std::shared_ptr<const void>* previous = nullptr;  // null or empty when there is none
		bool check = false;  // builder parity: also build the product whole and compare
	};
}

namespace DCLF::Extensions
{
	/** @brief A payload's products by registration. A slot may be null. */
	using PayloadProducts = std::vector<std::shared_ptr<const void>>;

	/** @brief Snapshot builder. Builds every product into a_out from its previous product. */
	void BuildPayloadProducts(const Api::PayloadProductBuild& a_build, const PayloadProducts& a_previous, PayloadProducts& a_out);
	/** @brief Snapshot builder. Each product's carried, with the main payload's. */
	void CarryPayloadProducts(const PayloadProducts& a_main);
	/** @brief Render thread. Each product's install, with a_installed's, or null for no payload. */
	void InstallPayloadProducts(const PayloadProducts* a_installed);
}
