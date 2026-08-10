#include "DeferredRendering.h"

#include "Deferred.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

#define I18N_KEY_PREFIX "feature.deferred_rendering."

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	DeferredRendering::Settings,
	visualizeDeferredCoverage,
	classifyVisibleMaterials)

void DeferredRendering::DrawSettings()
{
	if (ImGui::Checkbox(T(TKEY("visualize_deferred_coverage"), "Visualize Deferred Coverage"),
			&settings.visualizeDeferredCoverage)) {
		// Diagnostic state is expected to survive closing/restarting while testing;
		// do not require the easy-to-miss global Save button.
		globals::state->Save();
	}
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted(T(TKEY("visualize_deferred_coverage_tooltip"),
			"Draws deferred pixels in a bold evaluator-family color after the Community Shaders menu is closed. Legacy and unsupported materials retain their normal color."));
	if (settings.visualizeDeferredCoverage) {
		ImGui::Indent();
		const auto generic = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::Generic);
		const auto grass = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::Grass);
		const auto distantTree = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::DistantTree);
		const auto foliage = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::FoliageSpecial);
		const auto truePbr = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::TruePBR);
		const auto truePbrSubsurface = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::TruePBRSubsurfaceFuzz);
		const auto truePbrCoat = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::TruePBRCoat);
		const auto truePbrGlint = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::TruePBRGlint);
		const auto truePbrTerrain = CS::Deferred::GetEvaluatorColor(CS::Deferred::Evaluator::TruePBRTerrain);
		ImGui::TextColored({ generic.red, generic.green, generic.blue, 1.0f }, "Generic");
		ImGui::SameLine();
		ImGui::TextColored({ grass.red, grass.green, grass.blue, 1.0f }, "Grass");
		ImGui::SameLine();
		ImGui::TextColored({ distantTree.red, distantTree.green, distantTree.blue, 1.0f }, "Distant Tree");
		ImGui::SameLine();
		ImGui::TextColored({ foliage.red, foliage.green, foliage.blue, 1.0f }, "Special Foliage");
		ImGui::TextColored({ truePbr.red, truePbr.green, truePbr.blue, 1.0f }, "TruePBR Core/Wetness");
		ImGui::SameLine();
		ImGui::TextColored({ truePbrSubsurface.red, truePbrSubsurface.green, truePbrSubsurface.blue, 1.0f }, "TruePBR SSS/Fuzz");
		ImGui::SameLine();
		ImGui::TextColored({ truePbrCoat.red, truePbrCoat.green, truePbrCoat.blue, 1.0f }, "TruePBR Coat");
		ImGui::SameLine();
		ImGui::TextColored({ truePbrGlint.red, truePbrGlint.green, truePbrGlint.blue, 1.0f }, "TruePBR Glint");
		ImGui::SameLine();
		ImGui::TextColored({ truePbrTerrain.red, truePbrTerrain.green, truePbrTerrain.blue, 1.0f }, "TruePBR Terrain");
		ImGui::TextDisabled("Parallax, advanced terrain, and LOD blend are prepared but disabled pending direct material-texture sharing");
		ImGui::TextDisabled("HairMarschner, skin, hair, and eyes use compatibility rendering");
		ImGui::Unindent();
	}
	if (ImGui::Checkbox(T(TKEY("classify_visible_materials"), "Classify Visible Materials"),
			&settings.classifyVisibleMaterials))
		globals::state->Save();
	if (auto tooltip = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Aggregates the intended material class and actual deferred coverage on the GPU, then asynchronously reads back a small histogram. This includes compatibility-rendered pixels.");
	if (settings.classifyVisibleMaterials) {
		const auto classification = GetMaterialClassification();
		ImGui::Text("Last completed sample: %llu   Evaluator mask: 0x%08X",
			static_cast<unsigned long long>(classification.serial), classification.enabledEvaluatorMask);
		ImGui::Text("Depth-covered pixels: %u   Missing material identity: %u (%.1f%%)",
			classification.depthCoveredPixels, classification.unclassifiedDepthPixels,
			classification.depthCoveredPixels ? 100.0f * classification.unclassifiedDepthPixels /
				classification.depthCoveredPixels : 0.0f);
		if (ImGui::BeginTable("DeferredMaterialClassification", 4,
			ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("Material variant");
			ImGui::TableSetupColumn("Visible pixels");
			ImGui::TableSetupColumn("Deferred pixels");
			ImGui::TableSetupColumn("Coverage");
			ImGui::TableHeadersRow();
			for (std::uint32_t id = 0; id < classification.visible.size(); ++id) {
				const auto visible = classification.visible[id];
				if (!visible) continue;
				const auto deferred = classification.deferred[id];
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Text("%s (%u)", CS::Deferred::GetMaterialClassName(
					static_cast<CS::Deferred::MaterialClass>(id)).data(), id);
				ImGui::TableSetColumnIndex(1); ImGui::Text("%u", visible);
				ImGui::TableSetColumnIndex(2); ImGui::Text("%u", deferred);
				ImGui::TableSetColumnIndex(3); ImGui::Text("%.1f%%", 100.0f * deferred / visible);
			}
			ImGui::EndTable();
		}
	}
}

void DeferredRendering::UpdateMaterialClassification(const std::uint32_t* visible,
	const std::uint32_t* deferred, std::uint32_t evaluatorMask,
	std::uint32_t depthCovered, std::uint32_t unclassifiedDepth)
{
	if (!visible || !deferred) return;
	std::scoped_lock lock(classificationMutex);
	std::copy_n(visible, materialClassification.visible.size(), materialClassification.visible.begin());
	std::copy_n(deferred, materialClassification.deferred.size(), materialClassification.deferred.begin());
	materialClassification.enabledEvaluatorMask = evaluatorMask;
	materialClassification.depthCoveredPixels = depthCovered;
	materialClassification.unclassifiedDepthPixels = unclassifiedDepth;
	++materialClassification.serial;
	if ((materialClassification.serial % 1800u) == 1u) {
		logger::info("[DeferredMaterialClassification] depthCovered={}, unclassifiedDepth={}, missingIdentity={:.2f}%",
			depthCovered, unclassifiedDepth, depthCovered ? 100.0 * unclassifiedDepth / depthCovered : 0.0);
		for (std::uint32_t id = 0; id < materialClassification.visible.size(); ++id) {
			const auto total = materialClassification.visible[id];
			const auto promoted = materialClassification.deferred[id];
			if (total > promoted)
				logger::info("[DeferredMaterialClassification] class={}({}), visible={}, deferred={}, compatibility={}",
					CS::Deferred::GetMaterialClassName(static_cast<CS::Deferred::MaterialClass>(id)),
					id, total, promoted, total - promoted);
		}
		for (std::size_t index = 0; index < kSelectionClassCount; ++index) {
			const auto total = shaderSelections[index].exchange(0, std::memory_order_relaxed);
			const auto inside = shaderSelectionsInsideDeferred[index].exchange(0, std::memory_order_relaxed);
			const auto flagged = shaderSelectionsWithDeferredPermutation[index].exchange(0, std::memory_order_relaxed);
			const auto inWorld = shaderSelectionsInWorld[index].exchange(0, std::memory_order_relaxed);
			const auto outsideEpoch = shaderSelectionsInWorldOutsideDeferred[index].exchange(0, std::memory_order_relaxed);
			const auto reflections = shaderSelectionsInReflections[index].exchange(0, std::memory_order_relaxed);
			const auto targetBinds = targetApplications[index].exchange(0, std::memory_order_relaxed);
			const auto identityBinds = targetApplicationsWithIdentity[index].exchange(0, std::memory_order_relaxed);
			if (!total) continue;
			static constexpr std::array shaderNames{
				"None", "Grass", "Sky", "Water", "BloodSplatter", "ImageSpace",
				"Lighting", "Effect", "Utility", "DistantTree", "Particle"
			};
			const auto label = index < 64 ? std::format("LightingTechnique{}", index) :
				std::format("{}Shader", shaderNames[index - 64]);
			logger::info("[DeferredShaderSelection] class={}, selections={}, inWorld={}, insideDeferred={}, "
				"inWorldOutsideEpoch={}, reflections={}, deferredPermutation={}, targetApplications={}, identityTargetBound={}",
				label, total, inWorld, inside, outsideEpoch, reflections, flagged, targetBinds, identityBinds);
		}
	}
}

DeferredRendering::MaterialClassification DeferredRendering::GetMaterialClassification() const
{
	std::scoped_lock lock(classificationMutex);
	return materialClassification;
}

void DeferredRendering::RecordShaderSelection(RE::BSShader::Type type,
	std::uint32_t vertexDescriptor, std::uint32_t pixelDescriptor, bool insideDeferred,
	bool inWorld, bool activeReflections) noexcept
{
	if (!IsMaterialClassificationEnabled()) return;
	std::size_t classification = 64 + std::min<std::size_t>(static_cast<std::size_t>(type),
		static_cast<std::size_t>(RE::BSShader::Type::Total) - 1u);
	std::uint32_t deferredBit = 0;
	if (type == RE::BSShader::Type::Lighting) {
		classification = std::min<std::size_t>((vertexDescriptor >> 24u) & 0x3Fu, 63u);
		deferredBit = static_cast<std::uint32_t>(SIE::ShaderCache::LightingShaderFlags::Deferred);
	} else if (type == RE::BSShader::Type::DistantTree) {
		classification = 64 + static_cast<std::size_t>(RE::BSShader::Type::DistantTree);
		deferredBit = static_cast<std::uint32_t>(SIE::ShaderCache::DistantTreeShaderFlags::Deferred);
	}
	shaderSelections[classification].fetch_add(1, std::memory_order_relaxed);
	if (insideDeferred)
		shaderSelectionsInsideDeferred[classification].fetch_add(1, std::memory_order_relaxed);
	if (inWorld)
		shaderSelectionsInWorld[classification].fetch_add(1, std::memory_order_relaxed);
	if (inWorld && !insideDeferred)
		shaderSelectionsInWorldOutsideDeferred[classification].fetch_add(1, std::memory_order_relaxed);
	if (activeReflections)
		shaderSelectionsInReflections[classification].fetch_add(1, std::memory_order_relaxed);
	if (deferredBit && (pixelDescriptor & deferredBit) != 0)
		shaderSelectionsWithDeferredPermutation[classification].fetch_add(1, std::memory_order_relaxed);
}

void DeferredRendering::RecordAppliedRenderTargets(bool isCompute) noexcept
{
	if (isCompute || !IsMaterialClassificationEnabled() || !globals::state->currentShader)
		return;

	const auto type = globals::state->currentShader->shaderType.get();
	std::size_t classification = 64 + std::min<std::size_t>(static_cast<std::size_t>(type),
		static_cast<std::size_t>(RE::BSShader::Type::Total) - 1u);
	if (type == RE::BSShader::Type::Lighting)
		classification = std::min<std::size_t>((globals::state->currentVertexDescriptor >> 24u) & 0x3Fu, 63u);

	targetApplications[classification].fetch_add(1, std::memory_order_relaxed);
	ID3D11RenderTargetView* targets[8]{};
	globals::d3d::context->OMGetRenderTargets(8, targets, nullptr);
	const auto* identity = globals::game::renderer->GetRuntimeData().renderTargets[MASKS2].RTV;
	bool identityBound = false;
	for (auto*& target : targets) {
		identityBound |= target == identity;
		if (target)
			target->Release();
	}
	if (identityBound)
		targetApplicationsWithIdentity[classification].fetch_add(1, std::memory_order_relaxed);
}

void DeferredRendering::LoadSettings(json& json)
{
	settings = json;
	runtimeEnabled.store(true, std::memory_order_release);
}

void DeferredRendering::SaveSettings(json& json)
{
	json = settings;
}

void DeferredRendering::RestoreDefaultSettings()
{
	settings = {};
}

bool DeferredRendering::ToggleAtBootSetting()
{
	const bool enabled = Feature::ToggleAtBootSetting();
	runtimeEnabled.store(enabled, std::memory_order_release);
	if (!enabled) {
		std::unique_lock lock(snapshotMutex);
		deferredLights.clear();
		deferredContexts.clear();
		deferredPBRMaterials.clear();
		drawContexts.clear();
		drawPBRMaterials.clear();
		finalizedFrame.reset();
	}
	logger::info("[DeferredRendering] Runtime execution {} immediately", enabled ? "enabled" : "disabled");
	return enabled;
}

#undef I18N_KEY_PREFIX

void DeferredRendering::BeginFrame(
	std::span<const LightData> lights,
	const std::uint32_t (&clusterDimensions)[3],
	float cameraNearPlane,
	float cameraFarPlane)
{
	if (!IsRuntimeEnabled())
		return;
	std::unique_lock lock(snapshotMutex);
	deferredLights.assign(lights.begin(), lights.end());
	deferredContexts.clear();
	deferredPBRMaterials.clear();
	// Non-BSLightingShader G-buffer producers (distant trees first, then grass)
	// need a stable valid identity even when their evaluator consumes only
	// resolved per-pixel inputs. Keep slot zero ABI-valid for that purpose.
	LightingContext frameContext{};
	if (globals::state && globals::state->sharedDataCpu) {
		const auto& shared = *globals::state->sharedDataCpu;
		frameContext.directionalLightDirection = shared.DirLightDirection;
		frameContext.directionalLightColor = shared.DirLightColor;
		// Convert CS's first-order SH representation into the affine rows used by
		// the deferred lighting-context ABI: dot(row, float4(normal, 1)).
		constexpr float l0 = 0.28209479177387814f;
		constexpr float l1 = 0.4886025119029199f;
		auto ambientRow = [&](const float4& sh) {
			return float4{ -l1 * sh.w, -l1 * sh.y, l1 * sh.z, l0 * sh.x };
		};
		const auto rowR = ambientRow(shared.AmbientSHR);
		const auto rowG = ambientRow(shared.AmbientSHG);
		const auto rowB = ambientRow(shared.AmbientSHB);
		frameContext.directionalAmbient[0] = { rowR.x, rowR.y, rowR.z, rowR.w };
		frameContext.directionalAmbient[1] = { rowG.x, rowG.y, rowG.z, rowG.w };
		frameContext.directionalAmbient[2] = { rowB.x, rowB.y, rowB.z, rowB.w };
		frameContext.featureFlags = shared.InInterior ? 0u : kWorld;
		frameContext.featureFlags |= kLightingUniformsValid;
	}
	deferredContexts.emplace_back(frameContext);
	drawContexts.clear();
	drawPBRMaterials.clear();
	std::ranges::copy(clusterDimensions, clusterSize);
	nearPlane = cameraNearPlane;
	farPlane = cameraFarPlane;
	cameraView = globals::game::frameBufferCached.GetCameraView();
	cameraViewInverse = globals::game::frameBufferCached.GetCameraViewInverse();
	projectionInverse = globals::game::frameBufferCached.GetCameraProjInverse();
	const auto linearLighting = globals::features::linearLighting.GetCommonBufferData();
	lightingTransform.enableLinearLighting = linearLighting.enableLinearLighting;
	lightingTransform.isDirectionalLightLinear = linearLighting.isDirLightLinear;
	lightingTransform.directionalLightScale = linearLighting.dirLightMult;
	lightingTransform.lightGamma = linearLighting.lightGamma;
	lightingTransform.directionalLightMultiplier = linearLighting.directionalLightMult;
	lightingTransform.pointLightMultiplier = linearLighting.pointLightMult;
	lightingTransform.vanillaNormalization = linearLighting.enableLinearLighting ? (1.0f / DirectX::XM_PI) : 1.0f;
	lightingTransform.ambientGamma = linearLighting.ambientGamma;
	lightingTransform.ambientMultiplier = linearLighting.ambientMult;
	grassLighting = globals::features::grassLighting.settings;
	ibl = globals::features::ibl.GetCommonBufferData();
	const bool inWorld = !globals::state->isMapMenuOpen && !Util::IsInterior();
	skylightingEnabled = globals::features::skylighting.loaded && inWorld;
	skylighting = globals::features::skylighting.GetCommonBufferData(skylightingEnabled);
	renderWidth = globals::game::graphicsState ? globals::game::graphicsState->screenWidth : 0;
	renderHeight = globals::game::graphicsState ? globals::game::graphicsState->screenHeight : 0;

	if (!lights.empty()) {
		static std::once_flag proofLogged;
		std::call_once(proofLogged, [&] {
			logger::info("[DeferredRendering] Captured {} active Skyrim lights for the DX12 graph snapshot", lights.size());
		});
	}
}

DeferredRendering::ContextIndex DeferredRendering::AssignContext(const RE::BSRenderPass* renderPass, const LightingContext& context)
{
	if (!IsRuntimeEnabled())
		return INVALID_CONTEXT;
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

CS::Deferred::PBRMaterialIndex DeferredRendering::AssignPBRMaterial(
	const RE::BSRenderPass* renderPass, const CS::Deferred::PBRMaterialRecord& material)
{
	if (!IsRuntimeEnabled() || !renderPass || !renderPass->geometry)
		return CS::Deferred::kInvalidPBRMaterial;
	std::unique_lock lock(snapshotMutex);
	const auto index = InternPBRMaterial(material);
	drawPBRMaterials.insert_or_assign(renderPass, index);
	return index;
}

CS::Deferred::PBRMaterialIndex DeferredRendering::InternPBRMaterial(
	const CS::Deferred::PBRMaterialRecord& material)
{
	const auto it = std::ranges::find(deferredPBRMaterials, material);
	if (it != deferredPBRMaterials.end())
		return static_cast<CS::Deferred::PBRMaterialIndex>(
			std::distance(deferredPBRMaterials.begin(), it));
	if (deferredPBRMaterials.size() >= CS::Deferred::kMaxPBRMaterials)
		return CS::Deferred::kInvalidPBRMaterial;
	deferredPBRMaterials.push_back(material);
	return static_cast<CS::Deferred::PBRMaterialIndex>(deferredPBRMaterials.size() - 1u);
}

void DeferredRendering::FinalizeFrame()
{
	if (!IsRuntimeEnabled())
		return;
	std::unique_lock lock(snapshotMutex);
	auto result = std::make_shared<FrameSnapshot>();
	result->lights = deferredLights;
	result->contexts = deferredContexts;
	result->pbrMaterials = deferredPBRMaterials;
	result->cameraView = cameraView;
	result->cameraViewInverse = cameraViewInverse;
	result->projectionInverse = projectionInverse;
	result->lightingTransform = lightingTransform;
	result->grassLighting = grassLighting;
	result->ibl = ibl;
	result->skylighting = skylighting;
	result->skylightingEnabled = skylightingEnabled;
	result->renderWidth = renderWidth;
	result->renderHeight = renderHeight;
	std::copy(clusterSize, clusterSize + 3, result->clusterSize);
	result->nearPlane = nearPlane;
	result->farPlane = farPlane;
	finalizedFrame = std::move(result);
}

std::shared_ptr<const DeferredRendering::FrameSnapshot> DeferredRendering::GetFrameSnapshot() const
{
	std::shared_lock lock(snapshotMutex);
	return finalizedFrame;
}

void DeferredRendering::RetainSubmittedFrame(std::shared_ptr<const FrameSnapshot> frame, std::uint64_t completionValue)
{
	if (!frame || completionValue == 0)
		return;
	std::unique_lock lock(snapshotMutex);
	submittedFrames.emplace_back(completionValue, std::move(frame));
}

void DeferredRendering::RetireFrames(std::uint64_t completedValue)
{
	std::unique_lock lock(snapshotMutex);
	while (!submittedFrames.empty() && submittedFrames.front().first <= completedValue)
		submittedFrames.pop_front();
}

DeferredRendering::ContextIndex DeferredRendering::GetContext(const RE::BSRenderPass* renderPass) const
{
	std::shared_lock lock(snapshotMutex);
	const auto it = drawContexts.find(renderPass);
	return it == drawContexts.end() ? INVALID_CONTEXT : it->second;
}

CS::Deferred::PBRMaterialIndex DeferredRendering::GetPBRMaterial(
	const RE::BSRenderPass* renderPass) const
{
	std::shared_lock lock(snapshotMutex);
	const auto it = drawPBRMaterials.find(renderPass);
	return it == drawPBRMaterials.end() ? CS::Deferred::kInvalidPBRMaterial : it->second;
}
