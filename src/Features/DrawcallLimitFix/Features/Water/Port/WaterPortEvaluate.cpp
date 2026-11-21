// The water port's pure functions: BSWaterShader's SetupTechnique, SetupMaterial and SetupGeometry, and GetRenderPasses, as
// Ghidra reads them on SE 1.5.97 and AE 1.6.1170 (the plan's "Phase 0 findings" has the details). Both runtimes compute the same.
// Each value is computed in the engine's operation order, so the results match the engine's bit for bit.
// CS compiles with /fp:fast, which may reorder float math, so this file asks for precise semantics.

#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"

#include "Features/LinearLighting.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Game.h"

#include <DirectXMath.h>

#include <algorithm>
#include <bit>
#include <cstring>

#pragma float_control(precise, on, push)
#pragma fp_contract(off)

namespace DCLF::Features::Water::Port
{
	namespace
	{
		using namespace DirectX;

		// BSWaterShaderMaterial fields SetupMaterial reads.
		constexpr std::size_t kShallowColor = 0x60;  // rgb and sun sparkle power
		constexpr std::size_t kDeepColor = 0x70;
		constexpr std::size_t kReflectionColor = 0x80;
		constexpr std::size_t kVarAmounts = 0x90;  // sun specular power, reflection amount, alpha, refraction magnitude
		constexpr std::size_t kBlendRadiusZ = 0xA8;
		constexpr std::size_t kLightScale = 0xAC;  // light passes' colour scale
		constexpr std::size_t kSpecularPower = 0xC8;
		constexpr std::size_t kSpecularRadius = 0xCC;  // FresnelRI.w is max(it * 0.1, 1)
		constexpr std::size_t kWaterParams = 0xE0;     // noise falloff, reflection magnitude, sun sparkle magnitude, +0xEC
		constexpr std::size_t kDepthProperties = 0xF0;
		constexpr std::size_t kNormalScroll = 0x100;  // NormalsScroll0 at +0x100, NormalsScroll1 at +0x110
		constexpr std::size_t kUvScale = 0x120;
		constexpr std::size_t kAmplitude = 0x130;
		constexpr std::size_t kBlendRadiusXY = 0x150;  // +0x150 and flowmap scale +0x154
		constexpr std::size_t kFogDistance = 0x158;    // above water fog distance far and +0x15C
		constexpr std::size_t kFogAmount = 0x160;
		constexpr std::size_t kFresnelAmount = 0x170;

		// The constants the engine's code holds (SE 0x14152fa30, 0x1418654a0 and 0x14152d040; the same values on AE).
		constexpr float kSpecularRadiusScale = std::bit_cast<float>(0x3DCCCCCDu);  // 0.1
		// TextureProj's texture space: x and y halved and offset by a half, as rows.
		const XMFLOAT4X4 kTextureSpace{ 0.5f, 0.0f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 1.0f };

		float ReadFloat(MaterialBytes a_bytes, std::size_t a_offset)
		{
			float value = 0.0f;
			std::memcpy(&value, a_bytes.data() + a_offset, sizeof(value));
			return value;
		}

		Row Copy(MaterialBytes a_bytes, std::size_t a_offset, std::uint8_t a_written = 0xF)
		{
			Row row;
			std::memcpy(row.v.data(), a_bytes.data() + a_offset, sizeof(row.v));
			row.written = a_written;
			return row;
		}

		Row Make(float a_x, float a_y, float a_z, float a_w, std::uint8_t a_written = 0xF) { return { { a_x, a_y, a_z, a_w }, a_written }; }

		void Set(Row& a_row, std::uint32_t a_component, float a_value)
		{
			a_row.v[a_component] = a_value;
			a_row.written |= static_cast<std::uint8_t>(1u << a_component);
		}

		/** @brief FUN_1412c3440 (SE) / FUN_1414aaf10 (AE): the object's matrix in rows, scaled rotation and translation less a_posAdjust. */
		XMFLOAT4X4 ObjectMatrix(const RE::NiTransform& a_world, const std::array<float, 3>& a_posAdjust)
		{
			const auto& r = a_world.rotate.entry;
			const float s = a_world.scale;
			XMFLOAT4X4 m;
			m._11 = s * r[0][0];
			m._12 = s * r[1][0];
			m._13 = s * r[2][0];
			m._14 = 0.0f;
			m._21 = s * r[0][1];
			m._22 = s * r[1][1];
			m._23 = s * r[2][1];
			m._24 = 0.0f;
			m._31 = s * r[0][2];
			m._32 = s * r[1][2];
			m._33 = s * r[2][2];
			m._34 = 0.0f;
			m._41 = a_world.translate.x - a_posAdjust[0];
			m._42 = a_world.translate.y - a_posAdjust[1];
			m._43 = a_world.translate.z - a_posAdjust[2];
			m._44 = 1.0f;
			return m;
		}

		std::array<Row, 4> Rows(const XMFLOAT4X4& a_matrix)
		{
			std::array<Row, 4> rows;
			for (std::uint32_t i = 0; i < 4; ++i)
				rows[i] = Make(a_matrix.m[i][0], a_matrix.m[i][1], a_matrix.m[i][2], a_matrix.m[i][3]);
			return rows;
		}

		XMFLOAT4X4 Store(FXMMATRIX a_matrix)
		{
			XMFLOAT4X4 out;
			XMStoreFloat4x4(&out, a_matrix);
			return out;
		}

		XMFLOAT4X4 LoadMatrix(const std::array<float, 16>& a_values)
		{
			XMFLOAT4X4 out;
			std::memcpy(&out, a_values.data(), sizeof(out));
			return out;
		}

		/** @brief Linear Lighting's colour conversion, as its hooks call it. Only while it is active. */
		RE::NiColor ToWorking(const RE::NiColor& a_color) { return globals::features::linearLighting.SRGBToWorking(a_color); }
	}

	Placement PlacementOf(VsVariable a_variable)
	{
		using enum VsVariable;
		switch (a_variable) {
		case WorldViewProj:
			return { 2, 8, 4 };
		case World:
			return { 2, 0, 4 };
		case PreviousWorld:
			return { 2, 4, 4 };
		case QPosAdjust:
			return { 0, 0, 1 };
		case ObjectUV:
			return { 2, 12, 1 };
		case NormalsScroll0:
			return { 1, 3, 1 };
		case NormalsScroll1:
			return { 1, 4, 1 };
		case NormalsScale:
			return { 1, 5, 1 };
		case VSFogParam:
			return { 1, 0, 1 };
		case VSFogNearColor:
			return { 1, 1, 1 };
		case VSFogFarColor:
			return { 1, 2, 1 };
		case CellTexCoordOffset:
			return { 2, 13, 1 };
		default:
			return { 0, 0, 0, false };
		}
	}

	Placement PlacementOf(PsVariable a_variable)
	{
		using enum PsVariable;
		switch (a_variable) {
		case TextureProj:
			return { 2, 0, 4 };
		case ShallowColor:
			return { 1, 0, 1 };
		case DeepColor:
			return { 1, 1, 1 };
		case ReflectionColor:
			return { 1, 2, 1 };
		case FresnelRI:
			return { 1, 3, 1 };
		case BlendRadius:
			return { 1, 4, 1 };
		case PosAdjust:
			return { 0, 1, 1 };
		case ReflectPlane:
			return { 2, 4, 1 };
		case CameraDataWater:
			return { 0, 2, 1 };
		case ProjData:
			return { 2, 5, 1 };
		case VarAmounts:
			return { 1, 5, 1 };
		case FogParam:
			return { 1, 10, 1 };
		case FogNearColor:
			return { 1, 8, 1 };
		case FogFarColor:
			return { 1, 9, 1 };
		case SunDir:
			return { 0, 3, 1 };
		case SunColor:
			return { 0, 4, 1 };
		case NumLights:
			return { 2, 0, 1, false };
		case LightPos:
			return { 2, 6, 8 };
		case LightColor:
			return { 2, 14, 8 };
		case WaterParams:
			return { 1, 7, 1 };
		case DepthControl:
			return { 1, 11, 1 };
		case SSRParams:
			return { 1, 12, 1 };
		case SSRParams2:
			return { 1, 13, 1 };
		case NormalsAmplitude:
			return { 1, 6, 1 };
		case VPOSOffset:
			return { 0, 0, 1 };
		default:
			return { 0, 0, 0, false };
		}
	}

	std::string_view VsName(VsVariable a_variable)
	{
		constexpr std::string_view kNames[] = { "WorldViewProj", "World", "PreviousWorld", "QPosAdjust", "ObjectUV", "NormalsScroll0", "NormalsScroll1",
			"NormalsScale", "VSFogParam", "VSFogNearColor", "VSFogFarColor", "CellTexCoordOffset" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(VsVariable::Count));
		return kNames[static_cast<std::size_t>(a_variable)];
	}

	std::string_view PsName(PsVariable a_variable)
	{
		constexpr std::string_view kNames[] = { "TextureProj", "ShallowColor", "DeepColor", "ReflectionColor", "FresnelRI", "BlendRadius", "PosAdjust",
			"ReflectPlane", "CameraDataWater", "ProjData", "VarAmounts", "FogParam", "FogNearColor", "FogFarColor", "SunDir", "SunColor", "NumLights",
			"LightPos", "LightColor", "WaterParams", "DepthControl", "SSRParams", "SSRParams2", "NormalsAmplitude", "VPOSOffset" };
		static_assert(std::size(kNames) == static_cast<std::size_t>(PsVariable::Count));
		return kNames[static_cast<std::size_t>(a_variable)];
	}

	ConstantTables CanonicalTables()
	{
		ConstantTables tables;
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(VsVariable::Count); ++i) {
			const auto place = PlacementOf(static_cast<VsVariable>(i));
			tables.vs[i] = place.present ? static_cast<std::int8_t>(place.row * 4) : 0;
		}
		for (std::uint32_t i = 0; i < static_cast<std::uint32_t>(PsVariable::Count); ++i) {
			const auto place = PlacementOf(static_cast<PsVariable>(i));
			tables.ps[i] = place.present ? static_cast<std::int8_t>(place.row * 4) : 0;
		}
		return tables;
	}

	std::uint32_t RawDescriptor(std::uint32_t a_passEnum)
	{
		switch (a_passEnum) {
		case 0x5C00006B:
			return 0x5800;
		case 0x5C00006C:
			return 0x5801;
		case 0x5C00006D:
		case 0x5C00006F:
			return 0x5000;
		case 0x5C00006E:
		case 0x5C000070:
			return 0x5001;
		default:
			return a_passEnum - kSurfacePassBase;
		}
	}

	MaterialBytes BytesOf(const RE::BSWaterShaderMaterial& a_material)
	{
		static_assert(sizeof(RE::BSWaterShaderMaterial) == kMaterialBytes);
		return MaterialBytes(reinterpret_cast<const std::byte*>(&a_material), kMaterialBytes);
	}

	TechniqueRows EvaluateTechnique(std::uint32_t a_descriptor, const FrameInputs& a_frame)
	{
		TechniqueRows rows;
		// The engine also tests descriptor & ~0x80, which is never zero with the Depth bit set.
		if (a_descriptor & Flag::kDepth)
			rows.vposOffset = Make(1.0f / static_cast<float>(a_frame.renderWidth), 1.0f / static_cast<float>(a_frame.renderHeight), 0.0f, 0.0f);
		rows.posAdjust = Make(a_frame.posAdjust[0], a_frame.posAdjust[1], a_frame.posAdjust[2], a_frame.invFrameBufferRange);
		rows.qPosAdjust = rows.posAdjust;
		const float farPlane = a_frame.cameraFar, nearPlane = a_frame.cameraNear;
		rows.cameraData = Make(farPlane, nearPlane, farPlane - nearPlane, farPlane * nearPlane);
		rows.sunDir = Make(a_frame.sunDir[0], a_frame.sunDir[1], a_frame.sunDir[2], a_frame.sunDir[3]);
		rows.sunColor = Make(a_frame.sunColor[0], a_frame.sunColor[1], a_frame.sunColor[2], a_frame.sunColor[3]);
		return rows;
	}

	MaterialRow EvaluateMaterial(MaterialBytes a_material, const FrameInputs& a_frame)
	{
		MaterialRow out;
		auto& vs = out.vs;
		auto& ps = out.ps;
		// Without a fog property both colours are the default and the distances zero (VSFogParam.x is then 0 * inf, a NaN, as in the engine).
		const float fogNear = a_frame.hasFog ? a_frame.fogNear : 0.0f;
		const float fogFar = a_frame.hasFog ? a_frame.fogFar : 0.0f;
		const float power = a_frame.hasFog ? a_frame.fogPower : 0.0f;
		const float clamp = a_frame.hasFog ? a_frame.fogClamp : 0.0f;
		const float inverseRange = 1.0f / (fogFar - fogNear);

		// VS b1.
		vs[3] = Copy(a_material, kNormalScroll);
		vs[4] = Copy(a_material, kNormalScroll + 16);
		vs[5] = Copy(a_material, kUvScale);
		vs[0] = Make(inverseRange * fogNear, inverseRange, ReadFloat(a_material, kFogDistance), ReadFloat(a_material, kFogDistance + 4));
		vs[1] = Make(a_frame.fogNearColor[0], a_frame.fogNearColor[1], a_frame.fogNearColor[2], ReadFloat(a_material, kFogAmount));
		vs[2] = Make(a_frame.fogFarColor[0], a_frame.fogFarColor[1], a_frame.fogFarColor[2], clamp);
		vs[5].v[3] = power;

		// PS b1, in SetupMaterial's order.
		ps[12] = Make(a_frame.ssrEnabled ? a_frame.ssrIntensity : 0.0f, a_frame.ssrBlur, 1.0f / static_cast<float>(a_frame.renderWidth),
			1.0f / static_cast<float>(a_frame.renderHeight));
		ps[13] = Make(a_frame.ssrPerturbation, 0.0f, 0.0f, 0.0f);
		const auto& light = a_frame.waterLighting;
		auto lit = [&](std::size_t a_offset) {
			return Make(ReadFloat(a_material, a_offset) * light[0], ReadFloat(a_material, a_offset + 4) * light[1],
				ReadFloat(a_material, a_offset + 8) * light[2], ReadFloat(a_material, a_offset + 12) * light[3]);
		};
		ps[0] = lit(kShallowColor);
		ps[1] = lit(kDeepColor);
		ps[2] = lit(kReflectionColor);
		ps[2].v[3] = a_frame.timer;
		float specularRadius = ReadFloat(a_material, kSpecularRadius) * kSpecularRadiusScale;
		if (specularRadius < 1.0f)
			specularRadius = 1.0f;
		ps[3] = Make(ReadFloat(a_material, kFresnelAmount), power, ReadFloat(a_material, kSpecularPower), specularRadius);
		// BlendRadius.w is never written.
		ps[4] = Make(ReadFloat(a_material, kBlendRadiusXY), ReadFloat(a_material, kBlendRadiusXY + 4), ReadFloat(a_material, kBlendRadiusZ), 0.0f, 0x7);
		ps[5] = Copy(a_material, kVarAmounts);
		ps[7] = Copy(a_material, kWaterParams);
		ps[7].v[3] = 1.0f / a_frame.invFrameBufferRange;
		ps[10] = vs[0];
		ps[8] = vs[1];
		ps[9] = vs[2];
		ps[11] = Copy(a_material, kDepthProperties);
		ps[6] = Copy(a_material, kAmplitude);

		// Linear Lighting's WaterMaterialUpload: working-space rgb times the working-space water lighting. w stays the engine's.
		if (a_frame.linearLighting) {
			const auto lighting = ToWorking(RE::NiColor{ light[0], light[1], light[2] });
			auto rewrite = [&](Row& a_row, std::size_t a_offset) {
				const auto color = ToWorking(RE::NiColor{ ReadFloat(a_material, a_offset), ReadFloat(a_material, a_offset + 4), ReadFloat(a_material, a_offset + 8) }) * lighting;
				a_row.v[0] = color.red;
				a_row.v[1] = color.green;
				a_row.v[2] = color.blue;
			};
			rewrite(ps[0], kShallowColor);
			rewrite(ps[1], kDeepColor);
			rewrite(ps[2], kReflectionColor);
		}
		return out;
	}

	ObjectRow EvaluateObject(const ObjectInputs& a_object, std::uint32_t a_descriptor, const FrameInputs& a_frame)
	{
		ObjectRow out;
		out.stencilBit = a_object.stencilBit;
		out.cellX = a_object.cellX;
		out.cellY = a_object.cellY;
		out.flowX = a_object.flowX;
		out.flowY = a_object.flowY;

		// Unified Water's SetupGeometry hook rewrites the flowmap globals and the property's cell fields before the engine reads them.
		std::int32_t flowmapSize = a_frame.flowmapSize;
		std::array<float, 2> meshFlowCellOffset = a_frame.meshFlowCellOffset;
		const auto& uw = a_frame.unifiedWater;
		if (uw.active) {
			flowmapSize = uw.width;
			meshFlowCellOffset[0] = static_cast<float>(uw.height);
			meshFlowCellOffset[1] = 1.0f - a_object.localScale;
			std::int32_t x = 0, y = 0;
			Util::WorldToCell(a_object.world.translate, x, y);
			out.flowX = x + uw.offsetX;
			out.flowY = y + uw.offsetY + uw.width - uw.height;
			out.cellX = x;
			out.cellY = y;
		}

		// ObjectUV: 1 in x for property bit 8, the flowmap's size and offset with kEnableFlowmap, else 0 in x. Only those are written.
		if (a_object.waterFlags & 0x100)
			Set(out.objectUV, 0, 1.0f);
		else if (a_object.waterFlags & 0x8000) {
			Set(out.objectUV, 0, static_cast<float>(flowmapSize));
			Set(out.objectUV, 1, meshFlowCellOffset[0]);
			Set(out.objectUV, 2, 1.0f - meshFlowCellOffset[1]);
		} else
			Set(out.objectUV, 0, 0.0f);

		// CellTexCoordOffset, with the Flowmap bit only. The displacement mesh (Wading) reads the frame's global instead.
		if (a_descriptor & Flag::kFlowmap) {
			if (a_descriptor & Flag::kWading)
				out.cellTexCoordOffset = Make(a_frame.displacementOffset[0], a_frame.displacementOffset[1], a_frame.displacementOffset[2], a_frame.displacementOffset[3]);
			else
				out.cellTexCoordOffset = Make(static_cast<float>(out.flowX), static_cast<float>((flowmapSize - out.flowY) - 1), static_cast<float>(out.cellX),
					static_cast<float>(-out.cellY));
		}
		return out;
	}

	GeometryRows EvaluateGeometry(const ObjectInputs& a_object, std::uint32_t a_descriptor, const FrameInputs& a_frame)
	{
		GeometryRows out;
		const auto object = ObjectMatrix(a_object.world, a_frame.posAdjust);
		const auto viewProj = LoadMatrix(a_frame.viewProj);
		// D3DXMatrixMultiplyTranspose and D3DXMatrixTranspose, which CS replaces with DirectXMath's (D3DX9MathUpgrade.cpp).
		const auto worldViewProj = Store(XMMatrixMultiplyTranspose(XMLoadFloat4x4(&object), XMLoadFloat4x4(&viewProj)));
		out.worldViewProj = Rows(worldViewProj);
		out.world = Rows(Store(XMMatrixTranspose(XMLoadFloat4x4(&object))));
		// PreviousWorld is the current world with the previous position adjust, not the object's previous world.
		const auto previous = ObjectMatrix(a_object.world, a_frame.previousPosAdjust);
		out.previousWorld = Rows(Store(XMMatrixTranspose(XMLoadFloat4x4(&previous))));
		if (LightCountOf(a_descriptor) == 0) {
			// FUN_1412f83b0: the texture space times the transposed world-view-projection, stored as it is (its columns are rows).
			out.textureProj = Rows(Store(XMMatrixMultiply(XMLoadFloat4x4(&kTextureSpace), XMLoadFloat4x4(&worldViewProj))));
		}
		const auto& p = a_object.plane;
		const auto& eye = a_frame.eyePosition;
		out.reflectPlane = Make(p[0], p[1], p[2], (p[1] * eye[1] + eye[0] * p[0] + p[2] * eye[2]) - p[3]);
		out.projData = Make(a_frame.proj00, a_frame.proj11, 0.0f, 0.0f, 0x3);
		return out;
	}

	PassLights EvaluateLights(const RE::BSRenderPass& a_pass, MaterialBytes a_material, std::uint32_t a_descriptor, const FrameInputs& a_frame)
	{
		PassLights out;
		const std::uint32_t count = LightCountOf(a_descriptor);
		if (!count || !a_pass.sceneLights)
			return out;
		out.any = true;
		out.numLights = static_cast<float>(count);
		const float lightScale = ReadFloat(a_material, kLightScale);
		for (std::uint32_t i = 0; i < count; ++i) {
			const RE::BSLight* bsLight = a_pass.sceneLights[i + 1];
			const RE::NiLight* light = bsLight ? bsLight->light.get() : nullptr;
			if (!bsLight || !bsLight->affectWater || !light) {
				out.lightPos[i] = Make(0.0f, 0.0f, 0.0f, 0.0f);
				const auto& c = a_frame.defaultLightColor;
				out.lightColor[i] = Make(c[0], c[1], c[2], c[3]);
				continue;
			}
			const auto& data = light->GetLightRuntimeData();
			out.lightPos[i] = Make(light->world.translate.x, light->world.translate.y, light->world.translate.z, data.radius.x);
			const float fade = data.fade * bsLight->lodDimmer;
			out.lightColor[i] = Make(data.diffuse.red * fade * lightScale, data.diffuse.green * fade * lightScale, data.diffuse.blue * fade * lightScale, 0.0f);
			// Linear Lighting's WaterGeometryUpload rewrites rgb for lights the pass holds.
			if (a_frame.linearLighting && count < 8 && i + 1 < a_pass.numLights) {
				const auto color = globals::features::linearLighting.LightColorToWorking(light) * (data.fade * bsLight->lodDimmer * lightScale);
				out.lightColor[i].v[0] = color.red;
				out.lightColor[i].v[1] = color.green;
				out.lightColor[i].v[2] = color.blue;
			}
		}
		return out;
	}

	namespace
	{
		/** @brief Writes variables into the six groups through a constant table, in the engine's order, so aliased writes land as they do. */
		class Writer
		{
		public:
			Writer(Constants& a_out, const ConstantTables& a_tables) :
				out(a_out), tables(a_tables) {}

			void Vs(VsVariable a_variable, std::span<const Row> a_rows) { Put(PlacementOf(a_variable).group, true, tables.vs[static_cast<std::size_t>(a_variable)], a_rows); }
			void Ps(PsVariable a_variable, std::span<const Row> a_rows) { Put(PlacementOf(a_variable).group, false, tables.ps[static_cast<std::size_t>(a_variable)], a_rows); }
			void Vs(VsVariable a_variable, const Row& a_row) { Vs(a_variable, std::span<const Row>(&a_row, 1)); }
			void Ps(PsVariable a_variable, const Row& a_row) { Ps(a_variable, std::span<const Row>(&a_row, 1)); }

		private:
			std::span<Row> Group(std::uint8_t a_group, bool a_vertex)
			{
				switch (a_group) {
				case 0:
					return a_vertex ? std::span<Row>(out.vsTechnique) : std::span<Row>(out.psTechnique);
				case 1:
					return a_vertex ? std::span<Row>(out.vsMaterial) : std::span<Row>(out.psMaterial);
				default:
					return a_vertex ? std::span<Row>(out.vsGeometry) : std::span<Row>(out.psGeometry);
				}
			}

			void Put(std::uint8_t a_group, bool a_vertex, std::int8_t a_offset, std::span<const Row> a_rows)
			{
				auto group = Group(a_group, a_vertex);
				const auto first = static_cast<std::uint32_t>(static_cast<std::uint8_t>(a_offset));
				for (std::size_t r = 0; r < a_rows.size(); ++r)
					for (std::uint32_t c = 0; c < 4; ++c) {
						if (!(a_rows[r].written & (1u << c)))
							continue;
						const std::uint32_t at = first + static_cast<std::uint32_t>(r) * 4 + c;
						if (at / 4 >= group.size())
							continue;  // past the rows the latch takes
						auto& row = group[at / 4];
						row.v[at % 4] = a_rows[r].v[c];
						row.written |= static_cast<std::uint8_t>(1u << (at % 4));
					}
			}

			Constants& out;
			const ConstantTables& tables;
		};
	}

	Constants Assemble(const TechniqueRows& a_technique, const MaterialRow& a_material, const ObjectRow& a_object, const GeometryRows& a_geometry,
		const PassLights& a_lights, const ConstantTables& a_tables)
	{
		Constants out;
		Writer w(out, a_tables);
		// SetupTechnique.
		w.Ps(PsVariable::VPOSOffset, a_technique.vposOffset);
		w.Ps(PsVariable::PosAdjust, a_technique.posAdjust);
		w.Vs(VsVariable::QPosAdjust, a_technique.qPosAdjust);
		w.Ps(PsVariable::CameraDataWater, a_technique.cameraData);
		w.Ps(PsVariable::SunDir, a_technique.sunDir);
		w.Ps(PsVariable::SunColor, a_technique.sunColor);
		// SetupMaterial. NormalsScale.w, the far colours' w and FresnelRI.y come last in the engine but write their own floats.
		const auto& vs = a_material.vs;
		const auto& ps = a_material.ps;
		w.Vs(VsVariable::NormalsScroll0, vs[3]);
		w.Vs(VsVariable::NormalsScroll1, vs[4]);
		w.Vs(VsVariable::NormalsScale, vs[5]);
		w.Vs(VsVariable::VSFogParam, vs[0]);
		w.Vs(VsVariable::VSFogNearColor, vs[1]);
		w.Vs(VsVariable::VSFogFarColor, vs[2]);
		w.Ps(PsVariable::SSRParams, ps[12]);
		w.Ps(PsVariable::SSRParams2, ps[13]);
		w.Ps(PsVariable::ShallowColor, ps[0]);
		w.Ps(PsVariable::DeepColor, ps[1]);
		w.Ps(PsVariable::ReflectionColor, ps[2]);
		w.Ps(PsVariable::FresnelRI, ps[3]);
		w.Ps(PsVariable::BlendRadius, ps[4]);
		w.Ps(PsVariable::VarAmounts, ps[5]);
		w.Ps(PsVariable::WaterParams, ps[7]);
		w.Ps(PsVariable::FogParam, ps[10]);
		w.Ps(PsVariable::FogNearColor, ps[8]);
		w.Ps(PsVariable::FogFarColor, ps[9]);
		w.Ps(PsVariable::DepthControl, ps[11]);
		w.Ps(PsVariable::NormalsAmplitude, ps[6]);
		// SetupGeometry.
		w.Vs(VsVariable::WorldViewProj, a_geometry.worldViewProj);
		w.Vs(VsVariable::World, a_geometry.world);
		w.Vs(VsVariable::PreviousWorld, a_geometry.previousWorld);
		w.Ps(PsVariable::TextureProj, a_geometry.textureProj);
		w.Vs(VsVariable::ObjectUV, a_object.objectUV);
		w.Ps(PsVariable::ReflectPlane, a_geometry.reflectPlane);
		w.Ps(PsVariable::ProjData, a_geometry.projData);
		if (a_lights.any) {
			w.Ps(PsVariable::NumLights, Make(a_lights.numLights, 0.0f, 0.0f, 0.0f, 0x1));
			const auto count = static_cast<std::size_t>(a_lights.numLights);
			w.Ps(PsVariable::LightPos, std::span<const Row>(a_lights.lightPos.data(), count));
			w.Ps(PsVariable::LightColor, std::span<const Row>(a_lights.lightColor.data(), count));
		}
		w.Vs(VsVariable::CellTexCoordOffset, a_object.cellTexCoordOffset);
		return out;
	}

	RenderPasses DeriveRenderPasses(const ObjectInputs& a_object, std::uint32_t a_renderMode, std::uint32_t a_lightCount, const FrameInputs& a_frame)
	{
		RenderPasses out;
		// Render modes 12-17 take no water passes, nor does a disabled water system.
		if (!a_frame.waterEnabled || (a_renderMode - 0xC) < 4 || (a_renderMode - 0x10) < 2) {
			out.none = true;
			return out;
		}
		if (a_frame.localMapPasses) {
			out.localMap = true;
			return out;
		}
		// GetRenderPasses makes kUnderwater follow the camera, clearing the property's passes when it changes.
		std::uint32_t flags = a_object.waterFlags;
		flags = a_frame.cameraUnderwater ? (flags | 0x20) : (flags & ~0x20u);
		out.waterFlags = flags;
		const bool underwater = (flags & 0x20) != 0;
		const bool reflections = (flags & 0x40) != 0;
		const bool depth = (flags & 0x4) != 0;
		const bool lod = (flags & 0x2) != 0;
		const bool wading = (flags & 0x1) != 0;  // also the hint 13 surface and the 6F/70 stencil pass
		const bool alphaDepth = (flags & 0x200) != 0;
		const bool vc = (flags & 0x100) != 0 || alphaDepth;
		const bool cubemap = (flags & 0x4000) != 0;
		const bool flowmap = (flags & 0x8000) != 0;
		const bool blendNormals = (flags & 0x10000) != 0;
		const bool hasUv = (a_object.vertexDescLow & 0xF00) != 0;
		const bool hasColors = ((a_object.vertexDescLow >> 0x16) & 0x3C) != 0;

		std::uint32_t d = vc ? Flag::kVc : 0;
		if (wading || hasUv)
			d |= Flag::kNormalTexCoord;
		if (reflections)
			d |= Flag::kReflections;
		if (flags & 0x80)
			d |= Flag::kRefractions;
		if (depth)
			d |= Flag::kDepth;
		if (a_frame.interior)
			d |= Flag::kInterior;
		if (wading)
			d |= Flag::kWading;
		if (alphaDepth)
			d |= Flag::kVertexAlphaDepth;
		if (cubemap)
			d |= Flag::kCubemap;
		if (flowmap)
			d |= blendNormals ? (Flag::kFlowmap | Flag::kBlendNormals) : Flag::kFlowmap;
		std::uint32_t technique = 0;
		if (underwater) {
			d = reflections ? (cubemap ? 0x10Eu : 0xEu) : 0xAu;
			if (wading)
				d |= Flag::kWading;
			if (flowmap)
				d |= blendNormals ? (Flag::kFlowmap | Flag::kBlendNormals) : Flag::kFlowmap;
			technique = kTechniqueUnderwater;
		}
		if (lod) {
			d = reflections ? (cubemap ? 0x104u : 0x4u) : 0u;
			technique = kTechniqueLod;
		}
		out.surfaceDescriptor = technique << 11 | d;
		out.surface = { kSurfacePassBase + out.surfaceDescriptor, static_cast<std::uint8_t>(wading ? 13 : 0), true };
		// The stencil pass is cached on the property (+0xC0) the first time, so a later flag change keeps the first id.
		if (wading)
			out.stencil = { hasColors ? 0x5C000070u : 0x5C00006Fu, 0, true };
		else
			out.stencil = { hasColors ? 0x5C00006Eu : 0x5C00006Du, 0, true };
		if (!underwater && a_lightCount > 1) {
			const std::uint32_t lights = std::min(a_lightCount - 1, 7u);
			std::uint32_t l = (vc ? Flag::kVc : 0) | Flag::kNormalTexCoord | lights << 11;
			if (wading)
				l |= Flag::kWading;
			out.lights = { kSurfacePassBase + l, 0, true };
		}
		std::uint32_t vertex = out.surfaceDescriptor, pixel = out.surfaceDescriptor;
		if (globals::state)
			globals::state->ModifyShaderLookup(RE::BSShader::Type::Water, vertex, pixel);
		out.lookupDescriptor = vertex;
		return out;
	}

	BindingRewrites EvaluateBindingRewrites(const FrameInputs& a_frame)
	{
		return { .physicalSkySampler = a_frame.physicalSkySampler, .nullReflectionCube = a_frame.cubemapFixInterior && !a_frame.mapMenuOpen };
	}
}

#pragma float_control(pop)
