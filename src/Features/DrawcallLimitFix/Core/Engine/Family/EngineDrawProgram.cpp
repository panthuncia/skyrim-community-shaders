// The engine draw families' programs (EngineDrawProgram.h).

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Engine/Family/EngineDrawProgram.h"

#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/ProgramCache.h"
#include "RenderGraph/RasterProgram.h"
#include "ShaderCache.h"

#include <format>

namespace DCLF::Family
{
	namespace Pipeline = ::RenderGraph::Engine::Pipeline;

	struct EngineDrawPrograms::Impl
	{
		std::string family;
		ProgramSource source;
		::RenderGraph::ProgramCache<RasterProgram, ProgramKey> cache;
		// Keys whose states have no ORG form: never queued.
		std::set<ProgramKey> refused;
		// Keys seen built (logged once each).
		std::set<ProgramKey> counted;

		Impl(std::string a_family, ProgramSource a_source) :
			family(std::move(a_family)), source(std::move(a_source)), cache(family) {}
	};

	EngineDrawPrograms::EngineDrawPrograms(std::string a_family, ProgramSource a_source) :
		impl(std::make_unique<Impl>(std::move(a_family), std::move(a_source))) {}

	EngineDrawPrograms::~EngineDrawPrograms() = default;

	std::shared_ptr<const RasterProgram> EngineDrawPrograms::Get(const ProgramKey& a_key, const RE::BSShader& a_shader,
		std::span<const std::pair<std::wstring, std::wstring>> a_variantDefines, ProgramState& a_state)
	{
		a_state = ProgramState::Failed;
		if (impl->refused.contains(a_key))
			return nullptr;
		switch (impl->cache.StatusOf(a_key)) {
		case ::RenderGraph::ProgramStatus::Failed:
			return nullptr;
		case ::RenderGraph::ProgramStatus::Building:
			a_state = ProgramState::Building;
			return nullptr;
		case ::RenderGraph::ProgramStatus::Ready:
			a_state = ProgramState::Ready;
			if (impl->counted.insert(a_key).second) {
				++built;
				logger::info("[DCLF] {}: built the program for descriptor {:#x} (variant {}, {} built)", impl->family, a_key.descriptor, a_key.variant, built);
			}
			return impl->cache.Get(a_key, {});
		default:
			break;
		}

		// The engine's state objects for the draw, as SetDirtyStates would bind them. The lookups answer on a tripwire handle too.
		const auto blend = Pipeline::FindBlend(static_cast<ID3D11BlendState*>(const_cast<void*>(a_key.blend)));
		const auto raster = Pipeline::FindRaster(static_cast<ID3D11RasterizerState*>(const_cast<void*>(a_key.raster)));
		const auto depthStencil = a_key.depthStencil ? Pipeline::FindDepthStencil(static_cast<ID3D11DepthStencilState*>(const_cast<void*>(a_key.depthStencil))) :
		                                               nullptr;
		const char* why = !blend                                     ? "no blend state" :
		                  !blend->error.empty()                      ? "its blend state has no ORG form" :
		                  blend->dualSource                          ? "its blend state reads a second source" :
		                  !raster                                    ? "no rasterizer state" :
		                  (a_key.depthStencil && !depthStencil)      ? "no depth-stencil state" :
		                  (!a_key.colourFormats[0] && !a_key.depthFormat) ? "no target" :
		                                                               nullptr;
		if (why) {
			impl->refused.insert(a_key);
			logger::warn("[DCLF] {}: the program for descriptor {:#x} cannot be built: {}{}; its draws stay the engine's", impl->family, a_key.descriptor, why,
				blend && !blend->error.empty() ? std::format(" ({})", blend->error) : std::string());
			return nullptr;
		}

		auto program = impl->cache.Get(a_key, [&] {
			RasterProgram::Desc desc{};
			desc.pixelSource = impl->source.source;
			desc.vertexSource = impl->source.source;
			desc.languageVersion = impl->source.languageVersion;
			desc.constantWords = impl->source.constantWords;
			// The descriptor's defines serve both stages; each stage's VSHADER or PSHADER is RasterProgram's (commonDefines).
			const auto stage = a_key.defineStage == DefineStage::Vertex ? SIE::ShaderClass::Vertex : SIE::ShaderClass::Pixel;
			for (const auto& [name, value] : SIE::ShaderCache::GetCompileDefines(a_shader, stage, a_key.descriptor)) {
				if (name == "VSHADER" || name == "PSHADER")
					continue;
				desc.defines.emplace_back(std::wstring(name.begin(), name.end()), std::wstring(value.begin(), value.end()));
			}
			desc.defines.insert(desc.defines.end(), a_variantDefines.begin(), a_variantDefines.end());
			for (std::size_t i = 0; i < a_key.colourFormats.size() && a_key.colourFormats[i]; ++i) {
				auto attachment = blend->rhi.attachments[blend->rhi.independentBlend ? i : 0];
				if ((a_key.maskedTargets >> i) & 1)
					attachment.writeMask = static_cast<rhi::ColorWriteEnable>(0);
				desc.targets.push_back({ .format = static_cast<rhi::Format>(a_key.colourFormats[i]), .blend = attachment });
			}
			desc.alphaToCoverage = blend->rhi.alphaToCoverage;
			if (depthStencil) {
				desc.depth = depthStencil->rhi;
				desc.depthFormat = static_cast<rhi::Format>(a_key.depthFormat);
			}
			desc.raster = raster->rhi;
			return desc;
		});
		a_state = program ? ProgramState::Ready : (impl->cache.StatusOf(a_key) == ::RenderGraph::ProgramStatus::Failed ? ProgramState::Failed : ProgramState::Building);
		return program;
	}
}
