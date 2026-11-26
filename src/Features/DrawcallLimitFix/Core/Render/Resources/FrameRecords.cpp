// Writers for the frame record, the frame-wide rows and the frame buffers' copies. Render thread.
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Core/Render/Internal.h"
#include "Features/DrawcallLimitFix/Core/Render/Resources/FrameRecords.h"
#include "RenderGraph/BufferRefs.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"
#include "RenderGraph/Engine/Flush/Flush.h"
#include "Snapshot/EngineReads.h"

#include "Features/DrawcallLimitFix/Core/Extensions/Commits.h"
#include "Features/DrawcallLimitFix/Core/Extensions/LightVolumes.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Registry.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"

#include <algorithm>
#include <cstring>

namespace DCLF::Draws
{
	namespace
	{
		// The frame's point light table at PS b3 without Light Limit Fix. The light count, then a row per light.
		struct GpuPointLight
		{
			float positionRadius[4];
			float color[3];
			std::uint32_t flags;  // shadow mask channel + 1 under kPointLightShadowChannel, plus kPointLightAffectsLand
		};
		struct GpuPointLightTable
		{
			std::uint32_t counts[4];
			GpuPointLight lights[kPointLightTableLights];
		};
		static_assert(sizeof(GpuPointLight) == 32 && sizeof(GpuPointLightTable) == 16 + 32 * kPointLightTableLights);
		static_assert(sizeof(GpuPointLightTable) <= kSharedLightBlockBytes);
		constexpr std::uint32_t kPointLightShadowChannel = 0xFFu;
		constexpr std::uint32_t kPointLightAffectsLand = 0x100u;

		/** @brief Render thread. Builds this commit's point light table with the selectable shadow lights first.
		 * The shader counts a draw's first ShadowCount lights as shadow lights, so no plain row may come before a shadow row. */
		const GpuPointLightTable& SamplePointLightTable()
		{
			static GpuPointLightTable table{};
			static std::array<Api::PointLight, kPointLightTableLights> lights{};
			const std::uint32_t count = Extensions::SamplePointLights(lights.data(), kPointLightTableLights);
			const auto shadowChannel = [](const Api::PointLight& a_light) { return a_light.shadowMaskIndex < 4 ? a_light.shadowMaskIndex + 1 : 0u; };
			std::stable_partition(lights.begin(), lights.begin() + count, [&](const Api::PointLight& a_light) { return shadowChannel(a_light) != 0; });
			table = {};
			table.counts[0] = count;
			for (std::uint32_t i = 0; i < count; ++i) {
				const auto& light = lights[i];
				auto& row = table.lights[i];
				std::memcpy(row.positionRadius, light.position, sizeof(light.position));
				row.positionRadius[3] = light.radius;
				std::memcpy(row.color, light.color, sizeof(light.color));
				row.flags = (shadowChannel(light) & kPointLightShadowChannel) | (light.affectsLand ? kPointLightAffectsLand : 0u);
			}
			return table;
		}
	}

	void WriteRecordBuffers(DrawBindings& a_record, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted,
		const SceneBuffers& a_scene, std::uint32_t a_frame)
	{
		a_record.textures[kObjectBufferRegister] = a_parts[PayloadPart::Objects].srvIndex;
		a_record.textures[kExtrasBufferRegister] = a_parts[PayloadPart::Extras].srvIndex;
		a_record.textures[kPlacementBufferRegister] = a_values.placements;
		a_record.textures[kPaletteBufferRegister] = a_values.palettes;
		a_record.textures[kShadingBufferRegister] = a_values.shading;
		Extensions::FrameBindingValues bindings;
		Extensions::FrameBindingValuesOf(a_adopted, a_scene, a_frame, bindings, true);
		// Among them are the fade root states and rows every draw's alpha reads.
		Extensions::WriteFrameBindings(bindings, a_record.textures);
	}

	DrawBindings FrameRecordOf(std::uint64_t a_frameConstants, RenderGraph::BufferRef a_frameConstantsRef, const FrameBlockViews& a_blocks, std::uint32_t a_vsMask, std::uint32_t a_psMask, std::span<const std::uint32_t, kTextureRegisters> a_frameTextures,
		std::uint32_t a_nullIndex, const PayloadVersions& a_parts, const FrameValueIndices& a_values, const NamedVersions& a_adopted, const SceneBuffers& a_scene,
		std::uint32_t a_frame)
	{
		DrawBindings record{};
		for (std::uint32_t r = 0; r < kConstantBufferRegisters; ++r) {
			record.vertexConstants[r] = ((a_vsMask >> r) & 1) ? a_frameConstants + FrameSlotOffset(false, r) : 0;
			record.pixelConstants[r] = ((a_psMask >> r) & 1) ? a_frameConstants + FrameSlotOffset(true, r) : 0;
		}
		record.pixelConstants[3] = a_frameConstants + kSharedLightBlockOffset;
		record.pixelConstants[kFrameLightingRegister] = a_frameConstants + std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes;
		std::ranges::copy(a_blocks.indices, record.frameBlocks);
		for (std::uint32_t t = 0; t < kTextureRegisters; ++t)
			record.textures[t] = a_frameTextures[t] == kInvalidIndex ? a_nullIndex : a_frameTextures[t];
		WriteRecordBuffers(record, a_parts, a_values, a_adopted, a_scene, a_frame);
		WriteRecordBackings(record, a_frameConstantsRef, a_frameConstants, kFrameConstantBytes, a_parts);
		return record;
	}

	void WriteRecordBackings(DrawBindings& a_record, RenderGraph::BufferRef a_constantsRef, std::uint64_t a_constants, std::uint64_t a_constantsBytes,
		const PayloadVersions& a_parts)
	{
		auto backing = [&](RecordBackingIndex a_index, RenderGraph::BufferRef a_ref, std::uint64_t a_address, std::uint64_t a_bytes) {
			auto& entry = a_record.backings[a_index];
			entry.ref = a_ref;
			entry.address = a_ref ? a_address : 0;
			entry.bytes = a_ref ? static_cast<std::uint32_t>(std::min<std::uint64_t>(a_bytes, UINT32_MAX)) : 0u;
		};
		backing(kBackingFrameConstants, a_constantsRef, a_constants, a_constantsBytes);
		const auto& pipelineRows = a_parts[PayloadPart::PipelineRows];
		const auto& materialRows = a_parts[PayloadPart::MaterialRows];
		const auto& shadowRows = a_parts[PayloadPart::ShadowRows];
		backing(kBackingPipelineRows, pipelineRows.ref, pipelineRows.address, pipelineRows.units * kPipelineRowBytes);
		backing(kBackingMaterialRows, materialRows.ref, materialRows.address, materialRows.units * kMaterialRowBytes);
		const auto animatedRows = PassAnimatedRows();
		backing(kBackingAnimatedRows, animatedRows.ref, animatedRows.address, std::uint64_t(animatedRows.count) * kMaterialRowBytes);
		backing(kBackingShadowRows, shadowRows.ref, shadowRows.address, shadowRows.units * sizeof(ShadowMaterialRow));
	}

	void ZeroSharedLightOnce(CommitUploads& a_uploads, const std::shared_ptr<org::Buffer>& a_target, bool& a_zeroed)
	{
		if (a_zeroed)
			return;
		static const std::array<std::uint32_t, kStrictLightDataBytes / 4> zeroLight{};
		// The zero block, and Light Limit Fix's StrictLightData at PS b3.
		a_uploads(a_target, zeroLight.data(), sizeof(zeroLight), std::uint64_t(kFrameSlotSharedLight) * kFrameSlotBytes);
		if (!Extensions::PointLightsActive())
			a_uploads(a_target, zeroLight.data(), sizeof(zeroLight), kSharedLightBlockOffset);
		a_zeroed = true;
	}

	void LatchFrameRows(LatchedUploads& a_latched, const std::shared_ptr<org::Buffer>& a_target, std::uint32_t a_viewBits)
	{
		// Latched every commit, since latching only on change would change the frame's shape.
		const auto& frameCapture = Scene::FrameView::Get().GetFrameCapture();
		a_latched(a_target, frameCapture.lighting.data(), sizeof(frameCapture.lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		// The draws fade specular and envmap by distance.
		const LodFadeFrame lodFadeFrame = SampleLodFadeFrame();
		a_latched(a_target, &lodFadeFrame, sizeof(lodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		// The view's bits: a reflection face's draws are reflection draws (the shaders' InReflection), as CS's RenderCubemap hook marks them on D3D11.
		const std::uint32_t viewBits[4]{ a_viewBits, 0, 0, 0 };
		a_latched(a_target, viewBits, sizeof(viewBits), kViewBitsPixelFrameOffset);
		// The frame's fog goes to VS b13. The technique rows keep theirs.
		a_latched(a_target, frameCapture.fog.data(), sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		const ExtrasFrame extrasFrame = SampleExtrasFrame();
		a_latched(a_target, &extrasFrame, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		a_latched(a_target, extrasFrame.projectedGlobals, sizeof(extrasFrame.projectedGlobals), kExtrasPixelFrameOffset);
		// Frame constant rows in ascending order, as FrameRowWrites lays them out.
		for (const auto& row : Extensions::RegisteredFrameConstantRows()) {
			float value[4]{};
			row.sample(value);
			a_latched(a_target, value, sizeof(value), FrameConstantRowOffset(row.row));
		}
		// Without Light Limit Fix, the Lighting draws pick their point lights from this table.
		if (Extensions::PointLightsActive()) {
			const auto& table = SamplePointLightTable();
			a_latched(a_target, &table, sizeof(table), kSharedLightBlockOffset);
		}
	}

	void FrameRowWrites(void (*a_add)(void* a_context, std::size_t a_bytes, std::uint64_t a_offset), void* a_context)
	{
		a_add(a_context, sizeof(Scene::FrameView::FrameCapture::lighting), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes);
		a_add(a_context, sizeof(LodFadeFrame), std::uint64_t(kFrameSlotLighting) * kFrameSlotBytes + sizeof(FrameLighting));
		a_add(a_context, sizeof(std::uint32_t[4]), kViewBitsPixelFrameOffset);
		a_add(a_context, sizeof(FrameFog), FrameSlotOffset(false, kFrameFogRegister));
		a_add(a_context, kExtrasFrameVertexBytes, FrameSlotOffset(false, kFrameFogRegister) + sizeof(FrameFog));
		a_add(a_context, sizeof(ExtrasFrame::projectedGlobals), kExtrasPixelFrameOffset);
		for (const auto& row : Extensions::RegisteredFrameConstantRows())
			a_add(a_context, sizeof(float[4]), FrameConstantRowOffset(row.row));
		if (Extensions::PointLightsActive())
			a_add(a_context, sizeof(GpuPointLightTable), kSharedLightBlockOffset);
	}

	bool LatchFrameBuffer(LatchedUploads& a_latched, const FrameBuffer& a_frameBuffer, std::span<const std::byte> a_contents)
	{
		const std::size_t offset = std::size_t(a_frameBuffer.firstElement) * a_frameBuffer.stride;
		const std::size_t bytes = std::size_t(a_frameBuffer.elements) * a_frameBuffer.stride;
		if (a_contents.size() < offset + bytes) {
			static std::vector<std::byte> zeros;
			if (zeros.size() < bytes)
				zeros.resize(bytes);
			a_latched(a_frameBuffer.copy, zeros.data(), bytes, 0);
			return false;
		}
		a_latched(a_frameBuffer.copy, a_contents.data() + offset, bytes, 0);
		return true;
	}

	winrt::com_ptr<ID3D11Buffer> BoundPixelBuffer(std::uint32_t a_register)
	{
		CS_ENGINE_READ_MARK(BoundState, BoundState);
		winrt::com_ptr<ID3D11ShaderResourceView> view;
		globals::d3d::context->PSGetShaderResources(a_register, 1, view.put());
		if (!view)
			return nullptr;
		D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
		view->GetDesc(&desc);
		if (desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER && desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFEREX)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		view->GetResource(resource.put());
		return resource.try_as<ID3D11Buffer>();
	}

	std::span<const std::byte> SharedDataBlock()
	{
		// The constant mirror never sees CS's own block, so take the struct it uploaded.
		const auto* state = globals::state;
		return state ? std::as_bytes(std::span(&state->lastSharedData, 1)) : std::span<const std::byte>();
	}

	std::span<const std::byte> FeatureDataBlock()
	{
		const auto* state = globals::state;
		return state ? std::as_bytes(std::span(state->lastFeatureData)) : std::span<const std::byte>();
	}

	std::uint32_t HoldFrameTexture(ID3D11ShaderResourceView*& a_heldView, GpuTextures::Binding& a_held, ID3D11ShaderResourceView* a_view, std::uint32_t a_register,
		std::uint32_t a_nullIndex, std::vector<std::shared_ptr<const void>>& a_owners)
	{
		if (!a_view) {
			if (a_heldView) {
				a_heldView = nullptr;
				a_held = {};
			}
			return a_nullIndex;  // the shared lookup root already owns the null descriptor
		}
		// A superseded import holds its old storage and that storage's chunk for as long as it is held here.
		if (a_heldView != a_view || !a_held.owner || a_held.index == GpuTextures::kInvalid || GpuTextures::Superseded(a_held.owner.get())) {
			DCLF_ZONE("CS.DCLF.FrameRecords.HoldFrameTexture");
			a_heldView = a_view;
			a_held = GpuTextures::Get().ResolveBinding(a_view, GpuTextures::Tag(GpuTextures::Source::FrameSlot, a_register));
		}
		if (a_held.owner)
			a_owners.push_back(a_held.owner);
		return a_held.index;
	}
}
