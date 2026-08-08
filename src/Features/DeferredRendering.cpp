#include "DeferredRendering.h"

void DeferredRendering::BeginFrame(
	std::span<const LightData> lights,
	const std::uint32_t (&clusterDimensions)[3],
	float cameraNearPlane,
	float cameraFarPlane)
{
	std::unique_lock lock(snapshotMutex);
	deferredLights.assign(lights.begin(), lights.end());
	deferredContexts.clear();
	drawContexts.clear();
	std::ranges::copy(clusterDimensions, clusterSize);
	nearPlane = cameraNearPlane;
	farPlane = cameraFarPlane;

	if (!lights.empty()) {
		static std::once_flag proofLogged;
		std::call_once(proofLogged, [&] {
			logger::info("[DeferredRendering] Captured {} active Skyrim lights for the DX12 graph snapshot", lights.size());
		});
	}
}

DeferredRendering::ContextIndex DeferredRendering::AssignContext(const RE::BSRenderPass* renderPass, const LightingContext& context)
{
	if (!renderPass || !renderPass->geometry)
		return INVALID_CONTEXT;

	std::unique_lock lock(snapshotMutex);
	const auto index = InternContext(context);
	drawContexts.insert_or_assign(renderPass, index);
	static std::once_flag proofLogged;
	std::call_once(proofLogged, [&] {
		logger::info("[DeferredRendering] Assigned Skyrim geometry to lighting context {} (room={}, shadowMask=0x{:08X})",
			index, context.roomIndex, context.shadowLightMembershipMask);
	});
	return index;
}

DeferredRendering::ContextIndex DeferredRendering::InternContext(const LightingContext& context)
{
	// Caller holds snapshotMutex exclusively.
	const auto it = std::ranges::find(deferredContexts, context);
	if (it != deferredContexts.end())
		return static_cast<ContextIndex>(std::distance(deferredContexts.begin(), it));
	if (deferredContexts.size() >= INVALID_CONTEXT)
		return INVALID_CONTEXT;
	deferredContexts.push_back(context);
	return static_cast<ContextIndex>(deferredContexts.size() - 1);
}

DeferredRendering::FrameSnapshot DeferredRendering::GetFrameSnapshot() const
{
	std::shared_lock lock(snapshotMutex);
	FrameSnapshot result;
	result.lights = deferredLights;
	result.contexts = deferredContexts;
	std::copy(clusterSize, clusterSize + 3, result.clusterSize);
	result.nearPlane = nearPlane;
	result.farPlane = farPlane;
	return result;
}

DeferredRendering::ContextIndex DeferredRendering::GetContext(const RE::BSRenderPass* renderPass) const
{
	std::shared_lock lock(snapshotMutex);
	const auto it = drawContexts.find(renderPass);
	return it == drawContexts.end() ? INVALID_CONTEXT : it->second;
}
