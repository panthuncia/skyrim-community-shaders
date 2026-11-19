// CS_DCLF_LAND_TRACE: logs each loaded land quad's engine state when first seen and when it changes.
// This tells a terrain drawing the wrong textures apart from one whose engine state really changed. Main thread.

#include "Features/DrawcallLimitFix/Features/Landscape/Landscape.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Diagnostics/Hooks.h"

#include <string>

namespace DCLF::Features::Landscape
{
	namespace
	{
		std::uint32_t frame = 0;

		ankerl::unordered_dense::map<std::uint64_t, std::string> lastSeen;

		std::string TextureName(const RE::NiSourceTexture* a_texture)
		{
			if (!a_texture)
				return "-";
			const char* name = a_texture->name.c_str();
			return fmt::format("{}:{}", fmt::ptr(a_texture), name && *name ? name : "?");
		}

		std::string DescribeQuad(RE::NiNode* a_node)
		{
			if (!a_node)
				return "no node";
			std::string text = fmt::format("node {}{}", fmt::ptr(a_node), Engine::IsHidden(a_node) ? " HIDDEN" : "");
			const auto& children = a_node->GetChildren();
			auto* geometry = !children.free_idx() || !children[0] ? nullptr : children[0]->AsGeometry();
			if (!geometry)
				return text + " no geometry";
			const auto& runtime = geometry->GetGeometryRuntimeData();
			text += fmt::format(" geometry {}{} renderer {}", fmt::ptr(geometry), Engine::IsHidden(geometry) ? " HIDDEN" : "", fmt::ptr(runtime.rendererData));
			const auto* property = runtime.shaderProperty.get();
			if (!property)
				return text + " no property";
			const auto* material = static_cast<const RE::BSLightingShaderMaterialLandscape*>(property->material);
			text += fmt::format(" property {} flags {:016X} material {}", fmt::ptr(property), property->flags.underlying(), fmt::ptr(material));
			if (!material || *reinterpret_cast<const std::uintptr_t*>(material) != RE::VTABLE_BSLightingShaderMaterialLandscape[0].address())
				return text + " (not a landscape material)";
			text += fmt::format(" hash {:08X} layers {} base {}", material->hashKey, material->numLandscapeTextures, TextureName(material->diffuseTexture.get()));
			for (std::uint32_t i = 0; i < 5 && i < material->numLandscapeTextures; ++i)
				text += fmt::format(" L{} {}", i, TextureName(material->landscapeDiffuseTexture[i].get()));
			return text;
		}

		void TraceLand()
		{
			++frame;
			auto* tes = RE::TES::GetSingleton();
			if (!tes)
				return;
			Engine::ForEachLoadedCell(*tes, [&](RE::TESObjectCELL* a_cell) {
				auto* land = a_cell ? a_cell->GetRuntimeData().cellLand : nullptr;
				auto* loaded = land ? land->loadedData : nullptr;
				if (!loaded)
					return;
				for (std::uint32_t quad = 0; quad < 4; ++quad) {
					const std::uint64_t key = reinterpret_cast<std::uintptr_t>(land) * 4 + quad;
					std::string now = DescribeQuad(loaded->mesh[quad]);
					auto& seen = lastSeen[key];
					if (seen == now)
						continue;
					logger::info("[DCLF] land trace [frame {}] cell {:08X} ({}, {}) land {:08X} quad {}: {}{}", frame, a_cell->GetFormID(),
						a_cell->GetCoordinates() ? a_cell->GetCoordinates()->cellX : 0, a_cell->GetCoordinates() ? a_cell->GetCoordinates()->cellY : 0,
						land->GetFormID(), quad, seen.empty() ? "" : "CHANGED ", now);
					seen = std::move(now);
				}
			});
		}
	}

	void SubscribeLandTrace()
	{
		Diagnostics::Subscribe<Diagnostics::FrameBegin>([](const Diagnostics::FrameBegin&) { TraceLand(); });
	}
}
