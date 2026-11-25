#pragma once

// Small helpers used across the DCLF draws: formats, handles, buffers, registers.
// Features include this header directly, so it must stay self-contained.

// volk must come before every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>
#include "Features/DrawcallLimitFix/Core/Gpu/GpuTextures.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/ComputeProgram.h"
#include "RenderGraph/Switches.h"

#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <rhi.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdint>
#include <memory>

namespace DCLF::Draws
{
	// Live override of CS_DCLF_ASYNC_COMPUTE from the test harness. -1 none, 0 graphics, 1 compute.
	inline std::atomic<int> asyncComputeOverride{ -1 };
	// The least a sun caster's bound may project to, in texels, or 0 for off. It is data only, so it can change live for A/B tests.
	inline std::atomic<float> shadowMinTexels{ 0.0f };
	// These passes default to the graphics queue. They take about 0.03 ms, but cross-queue waits cost the graphics queue about
	// 0.13 ms a frame (native mode). In guest mode FSR frame generation's interpolation shares DXVK's compute queue, and a graph
	// batch waiting there deadlocks its present (render-graph.md, "Not with FSR frame generation"). CS_DCLF_ASYNC_COMPUTE=1
	// forces the compute queue.
	inline org::QueueKind AsyncComputeQueue()
	{
		const int forced = asyncComputeOverride.load(std::memory_order_relaxed);
		const auto& value = SwitchValue(Switch::AsyncCompute);
		const bool compute = forced >= 0 ? forced != 0 : !value.empty() && value != "0";
		return compute ? org::QueueKind::Compute : org::QueueKind::Graphics;
	}

	// The format that reads a depth resource's depth aspect in a shader, as DXGI names it.
	inline rhi::Format DepthReadFormat(rhi::Format a_format)
	{
		switch (a_format) {
		case rhi::Format::D24_UNorm_S8_UInt:
		case rhi::Format::R24G8_Typeless:
			return rhi::Format::R24_UNorm_X8_Typeless;
		case rhi::Format::D32_Float_S8X24_UInt:
		case rhi::Format::R32G8X24_Typeless:
			return rhi::Format::R32_Float_X8X24_Typeless;
		case rhi::Format::D32_Float:
			return rhi::Format::R32_Float;
		case rhi::Format::D16_UNorm:
			return rhi::Format::R16_UNorm;
		default:
			return a_format;
		}
	}

	// A power of two that only grows, so the recording settles while preprocess memory stays near what the frame draws.
	// Never past a_limit, even when the buffer shrinks under the current count.
	inline std::uint32_t GrowCapacity(std::uint32_t a_current, std::uint32_t a_needed, std::uint32_t a_limit)
	{
		if (a_needed <= a_current)
			return std::min(a_current, a_limit);
		return std::min(a_limit, std::max(256u, std::bit_ceil(a_needed)));
	}

	template <class H>
	bool SameHandle(const H& a, const H& b)
	{
		return a.index == b.index && a.generation == b.generation;
	}

	inline bool SameIndirect(const IndirectState& a, const IndirectState& b)
	{
		if (a.valid != b.valid || !SameHandle(a.layout, b.layout) || a.classes.size() != b.classes.size())
			return false;
		// Each state class's sets, signatures and pipeline counts. The class map is the commit's.
		for (std::size_t c = 0; c < a.classes.size(); ++c) {
			const auto &left = a.classes[c], &right = b.classes[c];
			if (left.pipelines != right.pipelines)
				return false;
			for (std::size_t v = 0; v < left.sets.size(); ++v)
				if (!SameHandle(left.sets[v], right.sets[v]) || !SameHandle(left.signatures[v], right.signatures[v]))
					return false;
		}
		return SameHandle(a.zLayout, b.zLayout) && SameHandle(a.zDrawSignature, b.zDrawSignature) && SameHandle(a.refraction, b.refraction) &&
		       a.refractionFormat == b.refractionFormat;
	}

	// The dispatch signature every BuildDraws pass records with. One Dispatch argument, read from a latch.
	inline rhi::CommandSignaturePtr CreateDispatchSignature(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout)
	{
		rhi::IndirectArg args[] = { { .kind = rhi::IndirectArgKind::Dispatch } };
		rhi::CommandSignaturePtr signature;
		if (a_device.CreateCommandSignature(rhi::CommandSignatureDesc{ rhi::Span<rhi::IndirectArg>(args, 1), sizeof(std::uint32_t) * 3 }, a_layout, signature) != rhi::Result::Ok)
			return {};
		return signature;
	}

	inline std::shared_ptr<org::Buffer> CreateWords(std::uint64_t a_words, bool a_unorderedAccess, const char* a_name)
	{
		auto buffer = org::Buffer::CreateUnmaterializedStructuredBuffer(static_cast<std::uint32_t>(a_words), sizeof(std::uint32_t), a_unorderedAccess);
		buffer->SetName(a_name);
		buffer->Materialize();
		return buffer;
	}

	// The native draw rebinds only Extensions::PerDrawMask's constant buffers per object. Everything else in the main pass is per frame.

	constexpr std::uint32_t kStrictLightDataBytes = 1216;  // LightLimitFix.hlsli StrictLightData (15 lights)
	constexpr std::uint32_t kCharacterLightMaterialRegister = 11;  // the engine's register for the character light's noise
	constexpr std::uint32_t kInvalidIndex = GpuTextures::kInvalid;
}
