#include "PassCapture.h"

#include "Switches.h"

#include <span>

namespace DCLF
{
	namespace
	{
		// BSShaderProperty::EShaderPropertyFlag bits the engine's batch-group classifier reads.
		constexpr std::uint64_t kFlagList4 = 1ull << 54;  // -> list 4 on its own
		constexpr std::uint64_t kFlagList2 = 1ull << 36;  // -> adds 2
		constexpr std::uint16_t kAlphaTestingFlag = 1u << 9;
	}

	PassCapture& PassCapture::Get()
	{
		static PassCapture capture;
		return capture;
	}

	std::uint32_t PassCapture::SubPassOf(const RE::BSGeometry* a_geometry, std::uint64_t a_propertyFlags)
	{
		if (a_propertyFlags & kFlagList4)
			return 4;
		std::uint32_t subPass = (a_propertyFlags & kFlagList2) ? 2u : 0u;
		if (a_geometry) {
			const auto* alpha = a_geometry->GetGeometryRuntimeData().alphaProperty.get();
			if (alpha && (alpha->alphaFlags & kAlphaTestingFlag))
				subPass |= 1u;
		}
		return subPass;
	}

	void PassCapture::Record(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass, std::uint32_t a_technique)
	{
		if (!a_pass || !a_pass->geometry || !a_pass->shader || a_pass->shader->shaderType.get() != RE::BSShader::Type::Lighting)
			return;

		// How many threads register, for the record: the engine classifier takes a mutex and the scene
		// lists are built on a job list, so more than one is expected.
		const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
		if (lastThread.exchange(thread, std::memory_order_relaxed) != thread)
			threadCount.fetch_add(1, std::memory_order_relaxed);

		const auto slot = cursor.fetch_add(1, std::memory_order_relaxed);
		if (slot >= kCapacity) {
			overflow.fetch_add(1, std::memory_order_relaxed);
			return;
		}
		auto* property = a_pass->geometry->GetGeometryRuntimeData().shaderProperty.get();
		entries[slot] = Entry{ a_pass->geometry, a_pass, a_batch, a_technique,
			SubPassOf(a_pass->geometry, property ? property->flags.underlying() : 0ull), a_pass->passEnum };
	}

	std::span<const PassCapture::Entry> PassCapture::Drain()
	{
		const auto count = std::min(cursor.exchange(0, std::memory_order_relaxed), kCapacity);
		stats.captured = static_cast<std::uint32_t>(count);
		stats.overflowed = overflow.exchange(0, std::memory_order_relaxed);
		stats.threads = threadCount.exchange(0, std::memory_order_relaxed);
		stats.withheld = withheld.exchange(0, std::memory_order_relaxed);
		return { entries.data(), count };
	}

	bool PassCapture::WithholdingEnabled()
	{
		static const bool enabled = SwitchValue("CS_DCLF_OWNERSHIP") == "static";
		return enabled;
	}

	void PassCapture::PublishClaims(std::shared_ptr<const ClaimSet> a_claims)
	{
		std::atomic_store(&claims, std::move(a_claims));
	}

	void PassCapture::SetMainBatchRenderers(std::shared_ptr<const ankerl::unordered_dense::set<const RE::BSBatchRenderer*>> a_renderers)
	{
		std::atomic_store(&mainRenderers, std::move(a_renderers));
	}

	bool PassCapture::Withhold(const RE::BSBatchRenderer* a_batch, const RE::BSRenderPass* a_pass)
	{
		if (!WithholdingEnabled() || !a_pass || !a_pass->geometry)
			return false;
		// Only the main camera's renderers: the shadow cameras must keep drawing these objects, and DCLF
		// does not own their passes.
		const auto renderers = std::atomic_load(&mainRenderers);
		if (!renderers || !renderers->contains(a_batch))
			return false;
		const auto owned = std::atomic_load(&claims);
		if (!owned || !owned->contains(a_pass->geometry))
			return false;
		withheld.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	struct PassCapture::Hook
	{
		static void thunk(RE::BSBatchRenderer* a_this, RE::BSRenderPass* a_pass, std::uint32_t a_techniqueID)
		{
			auto& capture = PassCapture::Get();
			capture.Record(a_this, a_pass, a_techniqueID);
			// Withholding is the whole of static ownership: the pass is built, lit and shadowed exactly as
			// before - only the batch renderer never receives it, so the native loop has nothing to draw
			// and DCLF owns the object outright. Everything the tables need was taken by Record above.
			if (capture.Withhold(a_this, a_pass))
				return;
			func(a_this, a_pass, a_techniqueID);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void PassCapture::Install()
	{
		if (installed)
			return;
		stl::write_vfunc<0x2, Hook>(RE::VTABLE_BSBatchRenderer[0]);
		installed = true;
		logger::info("[DCLF] pass capture installed on BSBatchRenderer::RegisterPass");
	}
}
