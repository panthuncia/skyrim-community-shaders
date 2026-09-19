#include "CaptureParity.h"

#include <cstring>

#include "Deferred.h"
#include "SceneStore.h"
#include "State.h"

namespace DCLF
{
	namespace
	{
		constexpr std::size_t kMaxSamples = 8;
		constexpr std::uint32_t kPerMaterial = 1;
		constexpr std::uint32_t kPerGeometry = 2;

		// Lighting variable indices (ShaderConstants::LightingVS / LightingPS).
		constexpr std::uint32_t kVSWorld = 0;
		constexpr std::uint32_t kVSPreviousWorld = 1;
		constexpr std::uint32_t kVSLeftEyeCenter = 9;  // first PerMaterial VS variable
		constexpr std::uint32_t kPSNumLights = 0;
		constexpr std::uint32_t kPSPointLightPosition = 1;
		constexpr std::uint32_t kPSPointLightColor = 2;
		constexpr std::uint32_t kPSDirLightDirection = 3;  // first PerGeometry PS variable
		constexpr std::uint32_t kPSMaterialData = 7;
		constexpr std::uint32_t kPSEmitColor = 8;
		constexpr std::uint32_t kPSShadowLightMaskSelect = 10;
		constexpr std::uint32_t kPSSSRParams = 16;
		constexpr std::uint32_t kPSLODTexParams = 24;  // first PerMaterial PS variable

		// Per-object light assignment, which the Light Limit Fix shaders never read.
		constexpr std::uint64_t kPSLightAssignment = (1ull << kPSNumLights) | (1ull << kPSPointLightPosition) | (1ull << kPSPointLightColor) |
		                                             (1ull << kPSShadowLightMaskSelect);

		std::string Describe(const RE::BSGeometry* a_geometry)
		{
			const char* name = a_geometry->name.c_str();
			auto* ref = a_geometry->GetUserData();
			for (auto* node = a_geometry->parent; !ref && node; node = node->parent)
				ref = node->GetUserData();
			return fmt::format("'{}' ref {:08X}", name ? name : "", ref ? ref->GetFormID() : 0u);
		}

		bool SameTransform(const float (&a_record)[12], const RE::NiTransform& a_transform)
		{
			const auto& r = a_transform.rotate.entry;
			const float s = a_transform.scale;
			for (int row = 0; row < 3; ++row) {
				if (a_record[row * 4 + 0] != r[row][0] * s || a_record[row * 4 + 1] != r[row][1] * s || a_record[row * 4 + 2] != r[row][2] * s)
					return false;
			}
			return a_record[3] == a_transform.translate.x && a_record[7] == a_transform.translate.y && a_record[11] == a_transform.translate.z;
		}

		// World matrix as the PerGeometry constant holds it: translation relative to the camera (posAdjust).
		void StoreRelative(float* a_out, const float (&a_world)[12], const RE::NiPoint3& a_posAdjust)
		{
			std::memcpy(a_out, a_world, sizeof(a_world));
			a_out[3] = a_world[3] - a_posAdjust.x;
			a_out[7] = a_world[7] - a_posAdjust.y;
			a_out[11] = a_world[11] - a_posAdjust.z;
		}
	}

	bool CaptureParity::Enabled()
	{
		static const bool enabled = [] {
			char buf[4] = {};
			return GetEnvironmentVariableA("CS_DCLF_CAPTURE_PARITY", buf, sizeof(buf)) && buf[0] == '1';
		}();
		return enabled;
	}

	CaptureParity& CaptureParity::Get()
	{
		static CaptureParity parity;
		return parity;
	}

	void CaptureParity::NoteMismatch(std::string a_message)
	{
		if (samples.size() < kMaxSamples)
			samples.push_back(std::move(a_message));
	}

	void CaptureParity::OnMap(ID3D11Resource* a_resource, void* a_data)
	{
		if (ConstantEvaluator::Evaluating())
			return;
		// Only the current shaders' PerMaterial and PerGeometry buffers are of interest.
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		for (std::uint32_t level : { kPerMaterial, kPerGeometry }) {
			if (vs && reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[level].buffer) == a_resource) {
				Slot(0, level).buffer = a_resource;
				Slot(0, level).mapped = a_data;
			}
			if (ps && reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[level].buffer) == a_resource) {
				Slot(1, level).buffer = a_resource;
				Slot(1, level).mapped = a_data;
			}
		}
	}

	void CaptureParity::Snapshot(ConstantSnapshot& a_snapshot, ID3D11Resource* a_resource)
	{
		if (a_snapshot.buffer != a_resource || !a_snapshot.mapped)
			return;
		D3D11_BUFFER_DESC desc{};
		static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&desc);
		a_snapshot.size = std::min<std::uint32_t>(desc.ByteWidth, static_cast<std::uint32_t>(a_snapshot.bytes.size()));
		std::memcpy(a_snapshot.bytes.data(), a_snapshot.mapped, a_snapshot.size);
		a_snapshot.mapped = nullptr;
		a_snapshot.valid = true;
	}

	void CaptureParity::OnUnmap(ID3D11Resource* a_resource)
	{
		for (auto& stage : snapshots) {
			for (auto& snapshot : stage)
				Snapshot(snapshot, a_resource);
		}
	}

	bool CaptureParity::CompareBlock(const RE::BSGeometry* a_geometry, const char* a_what, const ConstantBlock& a_expected, const StageLayout& a_layout,
		const std::int8_t* a_nativeTable, std::size_t a_nativeTableSize, const ConstantSnapshot& a_native, ID3D11Resource* a_boundBuffer,
		std::uint32_t a_firstVariable, std::uint64_t a_skip)
	{
		if (!a_native.valid || a_boundBuffer != a_native.buffer)
			return true;

		bool ok = true;
		for (std::uint32_t i = 0; i < a_layout.count && i < a_nativeTableSize; ++i) {
			if ((a_skip >> i) & 1)
				continue;
			// Offset 0 is also what reflection leaves for variables a permutation lacks, so it only
			// counts for the group's first variable.
			const std::uint32_t nativeOffset = static_cast<std::uint8_t>(a_nativeTable[i]);
			if (nativeOffset == 0 && i != a_firstVariable)
				continue;
			const std::uint32_t ourOffset = a_layout.offset[i];
			bool differs = false;
			std::uint32_t firstComponent = 0;
			for (std::uint32_t c = 0; c < a_layout.size[i]; ++c) {
				// Only components the engine writes; the rest of a discard-mapped buffer is undefined.
				if (!a_expected.Written(ourOffset + c) || (nativeOffset + c + 1) * 4 > a_native.size)
					continue;
				if (std::memcmp(&a_native.bytes[(nativeOffset + c) * 4], &a_expected.floats[ourOffset + c], 4) != 0) {
					if (!differs)
						firstComponent = c;
					differs = true;
				}
			}
			if (differs) {
				ok = false;
				float native = 0;
				std::memcpy(&native, &a_native.bytes[(nativeOffset + firstComponent) * 4], 4);
				NoteMismatch(fmt::format("{} {} variable {} component {}: DCLF {}, native {}", Describe(a_geometry), a_what, i, firstComponent,
					a_expected.floats[ourOffset + firstComponent], native));
			}
		}
		return ok;
	}

	bool CaptureParity::CompareMaterial(const RE::BSGeometry* a_geometry, std::uint32_t a_materialIndex)
	{
		const auto& record = SceneStore::Get().GetTables().materials[a_materialIndex];
		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerMaterial", record.vs, LightingVSLayout(), vs->constantTable.data(),
				vs->constantTable.size(), Slot(0, kPerMaterial), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerMaterial].buffer), kVSLeftEyeCenter, 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerMaterial", record.ps, LightingPSLayout(), ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerMaterial),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerMaterial].buffer), kPSLODTexParams, 0);

		// Textures and address modes the material binds.
		const auto& state = globals::game::shadowState->GetRuntimeData();
		for (std::uint32_t slot = 0; slot < kPixelTextureSlots; ++slot) {
			if (!((record.textureWritten >> slot) & 1))
				continue;
			const auto* native = reinterpret_cast<const ID3D11ShaderResourceView*>(state.PSTexture[slot]);
			const auto nativeMode = static_cast<std::uint32_t>(state.PSTextureAddressMode[slot].underlying());
			if (native != record.textures[slot] || nativeMode != record.addressModes[slot]) {
				ok = false;
				NoteMismatch(fmt::format("{} texture slot {}: DCLF {} mode {}, native {} mode {}", Describe(a_geometry), slot,
					fmt::ptr(record.textures[slot]), record.addressModes[slot], fmt::ptr(native), nativeMode));
			}
		}
		return ok;
	}

	bool CaptureParity::CompareGeometry(const RE::BSGeometry* a_geometry, std::uint32_t a_objectIndex, std::uint32_t a_renderFlags)
	{
		const auto& tables = SceneStore::Get().GetTables();
		const auto& object = tables.objects[a_objectIndex];
		if (!tables.geometryConstantsValid[object.pipelineIndex])
			return true;

		// Expected values: the per-frame block for this pass descriptor with the per-object values on top.
		GeometryConstants expected = tables.geometryConstants[object.pipelineIndex];
		auto& state = globals::game::shadowState->GetRuntimeData();
		const auto& vsLayout = LightingVSLayout();
		const auto& psLayout = LightingPSLayout();
		StoreRelative(&expected.vs.floats[vsLayout.offset[kVSWorld]], object.world, state.posAdjust.getEye());
		StoreRelative(&expected.vs.floats[vsLayout.offset[kVSPreviousWorld]], (a_renderFlags & 0x10) ? object.world : object.previousWorld,
			state.previousPosAdjust.getEye());
		const auto& shading = tables.shading[a_objectIndex];
		std::memcpy(&expected.ps.floats[psLayout.offset[kPSMaterialData]], shading.materialData, sizeof(shading.materialData));
		std::memcpy(&expected.ps.floats[psLayout.offset[kPSEmitColor]], shading.emitColor, sizeof(shading.emitColor));
		expected.ps.floats[psLayout.offset[kPSSSRParams] + 3] = shading.ssrSpecular;

		auto* vs = *globals::game::currentVertexShader;
		auto* ps = *globals::game::currentPixelShader;
		bool ok = true;
		if (vs)
			ok &= CompareBlock(a_geometry, "VS PerGeometry", expected.vs, vsLayout, vs->constantTable.data(),
				vs->constantTable.size(), Slot(0, kPerGeometry), reinterpret_cast<ID3D11Resource*>(vs->constantBuffers[kPerGeometry].buffer), kVSWorld, 0);
		if (ps)
			ok &= CompareBlock(a_geometry, "PS PerGeometry", expected.ps, psLayout, ps->constantTable.data(), ps->constantTable.size(), Slot(1, kPerGeometry),
				reinterpret_cast<ID3D11Resource*>(ps->constantBuffers[kPerGeometry].buffer), kPSDirLightDirection, kPSLightAssignment);
		return ok;
	}

	namespace
	{
		struct DrawIndexedHook
		{
			static void thunk(ID3D11DeviceContext* a_this, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
			{
				CaptureParity::Get().OnDrawIndexed(a_this, a_indexCount, a_startIndex, a_baseVertex);
				func(a_this, a_indexCount, a_startIndex, a_baseVertex);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void CaptureParity::InstallDrawHook()
	{
		if (drawHookInstalled || !globals::d3d::context)
			return;
		stl::detour_vfunc<12, DrawIndexedHook>(globals::d3d::context);
		drawHookInstalled = true;
	}

	void CaptureParity::OnDrawIndexed(ID3D11DeviceContext* a_context, UINT a_indexCount, UINT a_startIndex, INT a_baseVertex)
	{
		const std::int32_t index = pendingObject;
		pendingObject = -1;
		if (index < 0 || ConstantEvaluator::Evaluating())
			return;

		const auto& tables = SceneStore::Get().GetTables();
		if (static_cast<std::size_t>(index) >= tables.objects.size())
			return;
		const auto& object = tables.objects[index];
		const auto& geometryRecord = tables.geometries[object.geometryIndex];
		const auto* geometry = tables.objectGeometry[index];
		++drawsChecked;

		// Draw arguments and input assembler bindings.
		ID3D11Buffer* indexBuffer = nullptr;
		DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
		UINT indexOffset = 0;
		a_context->IAGetIndexBuffer(&indexBuffer, &indexFormat, &indexOffset);
		ID3D11Buffer* vertexBuffer = nullptr;
		UINT stride = 0, vertexOffset = 0;
		a_context->IAGetVertexBuffers(0, 1, &vertexBuffer, &stride, &vertexOffset);
		const bool argsOk = a_indexCount == geometryRecord.indexCount && a_startIndex == geometryRecord.firstIndex && a_baseVertex == 0;
		const bool buffersOk = indexBuffer == geometryRecord.indexBuffer && indexFormat == DXGI_FORMAT_R16_UINT && indexOffset == 0 &&
		                       vertexBuffer == geometryRecord.vertexBuffer && stride == geometryRecord.vertexStride && vertexOffset == 0;
		if (!argsOk || !buffersOk) {
			++drawMismatches;
			NoteMismatch(fmt::format("{} draw: DCLF {} indices from {} (IB {} VB {} stride {}), native {} from {} base {} (IB {} fmt {} +{}, VB {} stride {} +{})",
				Describe(geometry), geometryRecord.indexCount, geometryRecord.firstIndex, fmt::ptr(geometryRecord.indexBuffer), fmt::ptr(geometryRecord.vertexBuffer),
				geometryRecord.vertexStride, a_indexCount, a_startIndex, a_baseVertex, fmt::ptr(indexBuffer), static_cast<int>(indexFormat), indexOffset,
				fmt::ptr(vertexBuffer), stride, vertexOffset));
		}
		if (indexBuffer)
			indexBuffer->Release();
		if (vertexBuffer)
			vertexBuffer->Release();

		// Render state, learned per property-derived key.
		auto& data = const_cast<RE::BSGeometry*>(geometry)->GetGeometryRuntimeData();
		const auto* property = data.shaderProperty.get();
		const auto* alpha = data.alphaProperty.get();
		RenderStateKey key{};
		key.twoSided = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
		key.zTest = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferTest);
		key.zWrite = property && property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kZBufferWrite);
		key.alphaTest = alpha && alpha->GetAlphaTesting();
		key.alphaThreshold = alpha ? alpha->alphaThreshold : 0;
		const auto& state = globals::game::shadowState->GetRuntimeData();
		RenderStateValue value{ state.rasterStateCullMode, static_cast<std::uint32_t>(state.depthStencilDepthMode), state.depthStencilStencilMode,
			state.alphaBlendMode, state.rasterStateDepthBiasMode, state.alphaTestEnabled, state.alphaTestRef };
		auto& seen = renderStates[key];
		if (std::find(seen.begin(), seen.end(), value) == seen.end())
			seen.push_back(value);
	}

	void CaptureParity::OnNativeLightingDraw(const RE::BSRenderPass* a_pass, std::uint32_t a_renderFlags)
	{
		pendingObject = -1;
		if (ConstantEvaluator::Evaluating() || !globals::deferred->deferredPass || !a_pass || !a_pass->geometry)
			return;
		InstallDrawHook();
		++nativeDraws;
		renderFlagsSeen.insert(a_renderFlags);

		auto& store = SceneStore::Get();
		auto* geometry = a_pass->geometry;
		const std::int32_t index = store.FindObject(geometry);

		if (index < 0) {
			if (store.IsTracked(geometry)) {
				// Tracked, drawn natively, but DCLF left it out this frame. Hidden or Fading here would
				// mean the per-frame checks disagree with the game.
				const Ineligible reason = store.Classify(geometry);
				if (reason == Ineligible::Hidden || reason == Ineligible::Fading) {
					++mismatchedDraws;
					NoteMismatch(fmt::format("{} drawn natively but excluded as {}", Describe(geometry), kIneligibleNames[static_cast<std::size_t>(reason)]));
				}
				++notInTables;
			} else if (SceneStore::ClassifyStatic(*geometry, nullptr) == Ineligible::None && store.IsUnderDrawnCategory(geometry)) {
				++untrackedEligible;
				NoteMismatch(fmt::format("{} eligible but not tracked", Describe(geometry)));
			}
			return;
		}

		++checkedDraws;
		const auto& tables = store.GetTables();
		const auto& object = tables.objects[index];
		const auto& key = tables.pipelines[object.pipelineIndex];
		const auto* state = globals::state;

		bool mismatch = false;
		if (key.vertexDescriptor != state->modifiedVertexDescriptor || key.pixelDescriptor != state->modifiedPixelDescriptor) {
			mismatch = true;
			NoteMismatch(fmt::format("{} descriptors: DCLF VS {:08X} PS {:08X}, native VS {:08X} PS {:08X} (pass {:08X}, flags {:016X})",
				Describe(geometry), key.vertexDescriptor, key.pixelDescriptor, state->modifiedVertexDescriptor, state->modifiedPixelDescriptor,
				a_pass->passEnum, a_pass->shaderProperty ? a_pass->shaderProperty->flags.underlying() : 0ull));
		}
		if (!SameTransform(object.world, geometry->world)) {
			mismatch = true;
			NoteMismatch(fmt::format("{} world transform changed after the tables were built", Describe(geometry)));
		}
		if (!mismatch && !CompareMaterial(geometry, object.materialIndex)) {
			mismatch = true;
			++materialMismatches;
		}
		if (!mismatch && !CompareGeometry(geometry, static_cast<std::uint32_t>(index), a_renderFlags)) {
			mismatch = true;
			++geometryMismatches;
		}
		if (mismatch)
			++mismatchedDraws;
		pendingObject = index;
	}

	void CaptureParity::Report(std::uint32_t a_frame, std::uint32_t a_interval)
	{
		if (a_interval == 0 || (a_frame % a_interval) != 0)
			return;

		const auto& stats = SceneStore::Get().GetStats();
		const bool ok = mismatchedDraws == 0 && untrackedEligible == 0 && drawMismatches == 0;
		std::string flags;
		for (auto value : renderFlagsSeen)
			flags += fmt::format(" {:X}", value);
		logger::info("[DCLF] capture parity {}: {} native main-pass lighting draws, {} checked against the tables, {} mismatched ({} material, {} per-geometry), {} untracked eligible, {} tracked but excluded; tables hold {} objects / {} geometries / {} pipelines / {} materials from {} tracked; render flags seen:{}",
			ok ? "OK" : "MISMATCH", nativeDraws, checkedDraws, mismatchedDraws, materialMismatches, geometryMismatches, untrackedEligible, notInTables,
			stats.objects, stats.geometries, stats.pipelines, stats.materials, stats.tracked, flags);
		logger::info("[DCLF] draw parity {}: {} draws checked, {} with different arguments or bound buffers", drawMismatches == 0 ? "OK" : "MISMATCH",
			drawsChecked, drawMismatches);
		for (const auto& [key, values] : renderStates) {
			std::string text;
			for (const auto& v : values)
				text += fmt::format(" [cull {} depth {} stencil {} blend {} bias {} alphaTest {} ref {}]", v.cull, v.depth, v.stencil, v.blend, v.depthBias,
					v.alphaTestEnabled, v.alphaTestRef);
			logger::info("[DCLF] render state{} twoSided {} zTest {} zWrite {} alphaTest {} threshold {} ->{}", values.size() > 1 ? " (VARIES)" : "",
				key.twoSided, key.zTest, key.zWrite, key.alphaTest, key.alphaThreshold, text);
		}
		for (const auto& sample : samples)
			logger::info("[DCLF]   {}", sample);

		nativeDraws = checkedDraws = mismatchedDraws = untrackedEligible = notInTables = materialMismatches = geometryMismatches = 0;
		drawMismatches = drawsChecked = 0;
		renderStates.clear();
		renderFlagsSeen.clear();
		samples.clear();
	}
}
