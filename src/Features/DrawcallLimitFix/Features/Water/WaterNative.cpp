// The water batches' draws computed from the engine's registered passes (WaterNative.h). Render thread.
// The engine functions this follows (AE 1.6.1170): RenderBatches 0x1414b4fe0, its technique walk 0x1414f37c0 and group step
// 0x1414f39c0, the pass draw 0x1414f3dc0, BSWaterShader::SetupTechnique 0x1414e0df0 and SetupGeometry 0x1414e1890 (which also
// binds the textures). The constants are the water port's (Port/WaterPort.h), which the parity observer matched bit for bit.

// volk must precede every Vulkan header in this translation unit.
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Features/Water/WaterNative.h"
#include "Features/DrawcallLimitFix/Diagnostics/Zones.h"
#include "Features/DrawcallLimitFix/Features/Water/Port/WaterPort.h"

#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Globals.h"
#include "RenderGraph/Engine/Pipeline/Descriptions.h"
#include "RenderGraph/Engine/Tripwire/Tripwire.h"
#include "RenderGraph/PublishedResources.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Logging.h"
#include "Utils/ShaderDescriptor.h"

#include <rhi_helpers.h>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace DCLF::Features::Water::Native
{
	namespace
	{
		namespace Pipeline = ::RenderGraph::Engine::Pipeline;
		using ::RenderGraph::Engine::Tripwire::Unwrap;

		namespace Address
		{
			// The engine globals SetupGeometry reads that the water port does not (AE, SE).
			inline constexpr Engine::Offset kWhiteTexture{ { 411503 }, { 525021 } };  // NiSourceTexture*: a normal map slot without a texture
			// std::int32_t: the render target the wading displacement reads at t2, kWATER_DISPLACEMENT or its swap. No SE id is known, so
			// on SE a wading pass is not computed and its batch stays the engine's.
			inline constexpr Engine::Offset kDisplacementTarget{ { 415235 }, {} };
		}

		template <class T>
		T Field(const void* a_object, std::size_t a_offset)
		{
			T value;
			std::memcpy(&value, static_cast<const std::byte*>(a_object) + a_offset, sizeof(T));
			return value;
		}

		// BSWaterShaderMaterial's texture fields (SetupGeometry): the static cube at +0x38, the normal maps at +0x40..+0x50, the flow
		// normals at +0x58, and the byte that picks the global cube.
		constexpr std::size_t kStaticCube = 0x38, kNormal1 = 0x40, kNormal2 = 0x48, kNormal3 = 0x50, kFlowNormals = 0x58, kGlobalCubeByte = 0x17D;
		// The wading mask passes (the Stencil technique of a wading property).
		constexpr std::uint32_t kWadingMaskFirst = 0x5C00006F, kWadingMaskLast = 0x5C000070;

		/** @brief The D3D11 view of a NiSourceTexture's renderer texture (+0x48, then +0x10), or null. */
		ID3D11ShaderResourceView* ViewOf(const void* a_texture)
		{
			if (!a_texture)
				return nullptr;
			const auto* renderer = Field<const std::byte*>(a_texture, 0x48);
			return renderer ? Unwrap(Field<ID3D11ShaderResourceView*>(renderer, 0x10)) : nullptr;
		}

		std::uint32_t FormatOf(ID3D11RenderTargetView* a_rtv)
		{
			if (!a_rtv)
				return 0;
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			a_rtv->GetDesc(&desc);
			return static_cast<std::uint32_t>(rhi::helpers::ToRHI(desc.Format));
		}

		/** @brief The batch renderer RenderBatches draws: the accumulator's, or the geometry group's own. */
		std::byte* BatchOf(const BatchInputs& a_inputs)
		{
			auto* batch = reinterpret_cast<std::byte*>(a_inputs.accumulator->GetRuntimeData()->batchRenderer);
			if (batch && a_inputs.group >= 0) {
				auto* group = Field<std::byte*>(batch, 0x70 + static_cast<std::size_t>(a_inputs.group) * 8);
				batch = group ? Field<std::byte*>(group, 0) : nullptr;
			}
			return batch;
		}

		/** @brief The batch renderer's pass group for a technique (its hash map at +0x48, the groups at +8), or null. */
		std::byte* PassGroupOf(std::byte* a_batch, std::uint32_t a_technique)
		{
			auto* buckets = Field<std::byte*>(a_batch, 0x48);
			if (!buckets)
				return nullptr;
			const auto capacity = Field<std::uint32_t>(a_batch, 0x2C);
			auto* sentinel = Field<std::byte*>(a_batch, 0x38);
			auto* entry = buckets + std::size_t((capacity - 1u) & a_technique) * 0x10;
			if (!Field<std::byte*>(entry, 8))
				return nullptr;
			for (;;) {
				if (Field<std::uint32_t>(entry, 0) == a_technique) {
					auto* groups = Field<std::byte*>(a_batch, 8);
					return groups ? groups + std::size_t(Field<std::uint32_t>(entry, 4)) * 0x30 : nullptr;
				}
				entry = Field<std::byte*>(entry, 8);
				if (!entry || entry == sentinel)
					return nullptr;
			}
		}

		/**
		 * @brief Calls a_visit(technique, passGroup) for each technique of the batch in [first, last] with a pass group, in the
		 * engine's order: the batch renderer's sorted technique list (its first node at +0x58, each node a technique and the next).
		 */
		template <class Visit>
		void ForEachTechnique(std::byte* a_batch, std::uint32_t a_first, std::uint32_t a_last, Visit&& a_visit)
		{
			const auto* node = a_batch + 0x58;
			if (!Field<const void*>(node, 8) && !Field<std::uint32_t>(node, 0))
				return;
			for (; node; node = Field<const std::byte*>(node, 8)) {
				const auto technique = Field<std::uint32_t>(node, 0);
				if (technique < a_first)
					continue;
				if (technique > a_last)
					break;
				if (auto* group = PassGroupOf(a_batch, technique))
					if (!a_visit(technique, group))
						return;
			}
		}

		/** @brief Whether every pass of a pass group is a BSWaterShader draw, so the group is the water's to compute and detach. */
		bool WaterGroup(std::byte* a_group)
		{
			bool any = false;
			for (std::uint32_t sub = 0; sub < 5; ++sub)
				for (auto* pass = Field<RE::BSRenderPass*>(a_group, sub * 8); pass; pass = pass->passGroupNext) {
					if (!pass->shader || pass->shader->shaderType.get() != RE::BSShader::Type::Water)
						return false;
					any = true;
				}
			return any;
		}

		Port::ConstantTables TablesOf(const RE::BSGraphics::VertexShader& a_vs, const RE::BSGraphics::PixelShader& a_ps)
		{
			Port::ConstantTables tables;
			std::memcpy(tables.vs.data(), a_vs.constantTable.data(), tables.vs.size());
			std::memcpy(tables.ps.data(), a_ps.constantTable.data(), tables.ps.size());
			return tables;
		}

		template <std::size_t N>
		void PutRows(const std::array<Port::Row, N>& a_rows, float (*a_out)[4])
		{
			for (std::size_t r = 0; r < N; ++r)
				for (std::uint32_t c = 0; c < 4; ++c)
					a_out[r][c] = (a_rows[r].written >> c) & 1u ? a_rows[r].v[c] : 0.0f;
		}

		/** @brief A draw's program and key, by what makes them. A frame's water uses a handful. The program cache never drops one. */
		struct ProgramInputs
		{
			const void* blend = nullptr;
			const void* depthStencil = nullptr;
			const void* raster = nullptr;
			const RE::BSShader* shader = nullptr;
			std::uint32_t descriptor = 0;
			std::array<std::uint32_t, 4> colourFormats{};
			std::uint32_t depthFormat = 0;
			bool operator==(const ProgramInputs&) const = default;
		};

		struct CachedProgram
		{
			ProgramInputs inputs;
			Family::ProgramKey key;
			std::shared_ptr<const void> program;
		};
		std::vector<CachedProgram> s_programs;  // render thread

		/** @brief The shadow state's modes a water draw depends on, as the engine leaves them pass by pass. */
		struct Modes
		{
			std::uint32_t depth = 0, stencil = 0, stencilRef = 0, fill = 0, cull = 0, depthBias = 0, scissor = 0;
			std::uint32_t blend = 0, alphaToCoverage = 0, write = 0, blendExtra = 0;

			static Modes Current()
			{
				const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				Modes m;
				m.depth = static_cast<std::uint32_t>(rss.depthStencilDepthMode);
				m.stencil = rss.depthStencilStencilMode;
				m.stencilRef = rss.stencilRef;
				m.fill = rss.rasterStateFillMode;
				m.cull = rss.rasterStateCullMode;
				m.depthBias = rss.rasterStateDepthBiasMode;
				m.scissor = rss.rasterStateScissorMode;
				m.blend = rss.alphaBlendMode;
				m.alphaToCoverage = rss.alphaBlendAlphaToCoverage;
				m.write = rss.alphaBlendWriteMode;
				m.blendExtra = rss.alphaBlendModeExtra;
				return m;
			}

			/**
			 * @brief SetupTechnique for a raw descriptor, after the group step: no culling, the light techniques blend additively, and the
			 * Stencil and Simple passes (0x5C00006B..70) write the mask's channels, the others kMAIN or kRAW_WATER.
			 */
			void Technique(std::uint32_t a_passEnum, std::uint32_t a_descriptor, std::uint32_t a_batchBlend)
			{
				cull = 0;
				const std::uint32_t technique = Port::TechniqueOf(a_descriptor);
				blend = technique >= 1 && technique <= 7 ? 2u : a_batchBlend;
				write = a_passEnum - 0x5C00006Bu < 6 ? 9u : 1u;
			}

			/** @brief SetupGeometry's depth and stencil for a pass of a property with water-type bit a_bit. */
			void Geometry(std::uint32_t a_passEnum, std::uint32_t a_descriptor, std::uint32_t a_bit)
			{
				const std::uint32_t technique = Port::TechniqueOf(a_descriptor);
				// Bit 1 is index 0; anything that is not one bit up to 0x80 takes the stencil off.
				const std::uint32_t index = std::has_single_bit(a_bit) && a_bit <= 0x80 ? static_cast<std::uint32_t>(std::countr_zero(a_bit)) : 0xFFu;
				stencil = 0;
				stencilRef = 0xFF;
				if (a_passEnum - kWadingMaskFirst < 2) {
					// The wading mask increments where a surface wrote the water type's bit: modes 0x20-0x26 for bits 2-0x80.
					depth = 1;
					depthBias = 1;
					if (index >= 1 && index <= 7)
						stencil = 0x1F + index;
				} else if (!(a_descriptor & Port::Flag::kWading)) {
					if (technique == Port::kTechniqueStencil) {
						// The mask writes the water type's bit: modes 2-9 for bits 1-0x80, the bit as reference.
						depth = 3;
						if (index <= 7) {
							stencil = 2 + index;
							stencilRef = a_bit;
						}
					} else {
						depth = 1;
						stencil = 0x1E;
						stencilRef = 1;
					}
				} else {
					// A wading surface draws where its mask incremented the bit: modes 0xE-0x14, reference the bit and 1.
					depth = 1;
					depthBias = 1;
					if (index >= 1 && index <= 7) {
						stencil = 0xD + index;
						stencilRef = a_bit | 1u;
					}
				}
			}
		};

		/** @brief What every pass of a batch reads alike, at the batch's call. */
		struct Frame
		{
			BatchInputs inputs;
			State* state = nullptr;
			Port::FrameInputs port;
			Port::BindingRewrites rewrites;
			Modes start;
			std::array<std::int32_t, 2> targets{ -1, -1 };  // the view's colour targets
			std::array<std::uint8_t, 2> colours{};          // their indices in kMaskTargets or kSurfaceTargets
			std::array<std::uint32_t, 4> colourFormats{};
			std::array<std::byte, kStrictLightBytes> strictLights{};
			bool llf = false;
			int (*roomIndex)(const RE::NiAVObject*) = nullptr;

			bool Read(const BatchInputs& a_inputs, std::string& a_why)
			{
				inputs = a_inputs;
				state = globals::state;
				auto* renderer = globals::game::renderer;
				if (!state || !renderer || !globals::shaderCache) {
					a_why = "no CS state";
					return false;
				}
				if (reinterpret_cast<const std::uint8_t*>(a_inputs.accumulator)[0x128]) {
					a_why = "a first-person accumulator";
					return false;
				}
				port = Port::CaptureFrame(a_inputs.accumulator);
				rewrites = Port::EvaluateBindingRewrites(port);
				start = Modes::Current();
				const auto& rss = RE::BSGraphics::RendererShadowState::GetSingleton()->GetRuntimeData();
				// The mask's targets are set just before its batch. The surface's RT0 is SetupTechnique's: kRAW_WATER for ISWaterBlend, else kMAIN.
				if (a_inputs.view == View::Mask) {
					targets = { static_cast<std::int32_t>(rss.renderTargets[0]), static_cast<std::int32_t>(rss.renderTargets[1]) };
				} else {
					if (port.renderMode == 0x12) {
						a_why = "the local map's surface";
						return false;
					}
					targets = { port.rawWater ? 88 : 1, -1 };
				}
				const auto& allowed = a_inputs.view == View::Mask ? kMaskTargets : kSurfaceTargets;
				const auto& engineTargets = renderer->GetRuntimeData().renderTargets;
				for (std::uint32_t i = 0; i < TargetsOf(a_inputs.view); ++i) {
					const auto found = std::ranges::find(allowed, targets[i]);
					if (targets[i] < 0 || found == allowed.end()) {
						a_why = std::format("colour target {} its view's client does not import", targets[i]);
						return false;
					}
					colours[i] = static_cast<std::uint8_t>(found - allowed.begin());
					colourFormats[i] = FormatOf(engineTargets[targets[i]].RTV);
					if (!colourFormats[i]) {
						a_why = "a colour target with no format";
						return false;
					}
				}
				// Light Limit Fix's strict lights for water: none, its room and the first-person flag (BSLightingShader_SetupGeometry_Before).
				auto& lightLimitFix = globals::features::lightLimitFix;
				llf = lightLimitFix.loaded;
				if (llf) {
					static_assert(sizeof(LightLimitFix::StrictLightDataCB) == kStrictLightBytes, "Light Limit Fix's PS b3");
					std::memcpy(strictLights.data(), &lightLimitFix.strictLightDataTemp, kStrictLightBytes);
				}
				return true;
			}

			void StrictLights(const RE::BSRenderPass& a_pass, std::byte* a_out) const
			{
				std::memcpy(a_out, strictLights.data(), kStrictLightBytes);
				if (!llf)
					return;
				auto data = reinterpret_cast<LightLimitFix::StrictLightDataCB*>(a_out);
				data->NumStrictLights = 0;
				data->ShadowBitMask = 0;
				data->FirstPerson = globals::features::lightLimitFix.localShadowFirstPersonPass ? 1u : 0u;
				data->RoomIndex = globals::features::lightLimitFix.GetRoomIndex(a_pass.geometry);
			}
		};

		/** @brief A slot's view and sampler modes, as SetupGeometry binds it. A null view reads zero. */
		struct Slot
		{
			ID3D11ShaderResourceView* view = nullptr;
			std::uint32_t address = 0, filter = 0;
		};

		/** @brief SetupGeometry's textures for a raw descriptor, then the cubemap fix's rewrite. Only the surface view samples any. */
		bool SlotsOf(const Frame& a_frame, std::uint32_t a_descriptor, const std::byte* a_material, std::array<Slot, kMaterialSlots>& a_slots, std::string& a_why)
		{
			a_slots = {};
			if (a_frame.inputs.view != View::Surface)
				return true;
			auto* renderer = globals::game::renderer;
			const auto& targets = renderer->GetRuntimeData().renderTargets;
			const auto bind = [&](std::uint32_t a_slot, ID3D11ShaderResourceView* a_view, std::uint32_t a_address, std::uint32_t a_filter) {
				a_slots[a_slot] = { Unwrap(a_view), a_address, a_filter };
			};
			const auto* white = Field<const void*>(reinterpret_cast<const void*>(REL::Offset(Address::kWhiteTexture).address()), 0);
			const auto normal = [&](std::uint32_t a_slot, std::size_t a_field) {
				const void* texture = Field<const void*>(a_material, a_field);
				bind(a_slot, ViewOf(texture ? texture : white), 3, 3);
			};
			ID3D11ShaderResourceView* reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS].SRV;
			if (a_descriptor & Port::Flag::kReflections) {
				bind(10, targets[RE::RENDER_TARGET::kSSR].SRV, 0, 1);
				bind(11, targets[RE::RENDER_TARGET::kSSR_RAW].SRV, 0, 1);
				// The material's static cube, or the global reflections cube when it has none or its byte picks the global one.
				const void* cube = Field<const void*>(a_material, kStaticCube);
				auto* view = !Field<std::uint8_t>(a_material, kGlobalCubeByte) && cube ? ViewOf(cube) : Unwrap(reflections);
				if (a_frame.rewrites.nullReflectionCube && view == Unwrap(reflections))
					view = nullptr;
				bind(3, view, 0, 1);
			}
			if (a_descriptor & Port::Flag::kRefractions)
				bind(1, targets[RE::RENDER_TARGET::kMAIN].SRVCopy, 0, 1);
			if (a_descriptor & Port::Flag::kWading) {
				if (!Address::kDisplacementTarget.Known()) {
					a_why = "a wading pass on a runtime with no address for the displacement target";
					return false;
				}
				const auto target = Field<std::int32_t>(reinterpret_cast<const void*>(REL::Offset(Address::kDisplacementTarget).address()), 0);
				if (target < 0 || target >= RE::RENDER_TARGET::kTOTAL) {
					a_why = std::format("a wading pass whose displacement target is {}", target);
					return false;
				}
				bind(2, targets[target].SRV, 3, 1);
			}
			if ((a_descriptor & (Port::Flag::kDepth | Port::Flag::kVertexAlphaDepth)) == Port::Flag::kDepth)
				bind(7, renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV, 0, 0);
			const bool flowmap = (a_descriptor & Port::Flag::kFlowmap) != 0;
			if (flowmap) {
				if (const auto offset = Port::Address::kFlowmapSource.Get()) {
					const auto* source = reinterpret_cast<const RE::NiPointer<RE::NiSourceTexture>*>(REL::Module::get().base() + offset)->get();
					if (source)
						bind(8, ViewOf(source), 3, 0);
				}
				normal(9, kFlowNormals);
				normal(4, kNormal1);
			}
			// A flowmap without BlendNormals binds only t4 for the surface, underwater and LOD techniques.
			const std::uint32_t technique = Port::TechniqueOf(a_descriptor);
			const bool onlyFirst = flowmap && !(a_descriptor & Port::Flag::kBlendNormals) && (technique == 0 || technique >= 8);
			if (!onlyFirst) {
				normal(4, kNormal1);
				normal(5, kNormal2);
				normal(6, kNormal3);
			}
			return true;
		}

		/** @brief One pass's draw, as SetupTechnique, SetupMaterial, SetupGeometry and CS's hooks on them would make it. */
		bool BuildPass(const Frame& a_frame, RE::BSRenderPass* a_pass, Modes& a_modes, Draw& a_draw, Family::Owners& a_owners, std::string& a_why)
		{
			auto* geometry = a_pass->geometry;
			const auto* property = a_pass->shaderProperty;
			if (!geometry || !property || property->GetRTTI() != globals::rtti::BSWaterShaderPropertyRTTI.get()) {
				a_why = "a pass that is not a BSWaterShaderProperty's";
				return false;
			}
			if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
				a_why = "a water pass that is not a TriShape";
				return false;
			}
			const auto& water = static_cast<const RE::BSWaterShaderProperty&>(*property);
			const auto* material = static_cast<const RE::BSWaterShaderMaterial*>(water.material);
			if (!material || material->GetType() != RE::BSShaderMaterial::Type::kWater) {
				a_why = "a water property with no water material";
				return false;
			}
			auto* state = a_frame.state;
			const auto& shader = *a_pass->shader;
			const std::uint32_t descriptor = Port::RawDescriptor(a_pass->passEnum);
			std::uint32_t vertexDescriptor = descriptor, pixelDescriptor = descriptor;
			state->ModifyShaderLookup(shader, vertexDescriptor, pixelDescriptor);
			const auto* vs = globals::shaderCache->GetVertexShader(shader, vertexDescriptor);
			const auto* ps = globals::shaderCache->GetPixelShader(shader, pixelDescriptor);
			if (!vs || !ps) {
				a_why = "a water shader CS has not compiled yet";
				return false;
			}

			a_modes.Technique(a_pass->passEnum, descriptor, a_frame.start.blend);
			a_modes.Geometry(a_pass->passEnum, descriptor, water.unk8C);
			auto* depthStencil = Pipeline::EngineDepthStencilState(a_modes.depth, a_modes.stencil);
			auto* blend = Pipeline::EngineBlendState(a_modes.blend, a_modes.alphaToCoverage, a_modes.write, a_modes.blendExtra);
			auto* raster = Pipeline::EngineRasterState(a_modes.fill, a_modes.cull, a_modes.depthBias, a_modes.scissor);
			if (!depthStencil) {
				a_why = "a depth-stencil state with no description";
				return false;
			}
			const ProgramInputs inputs{ .blend = blend, .depthStencil = depthStencil, .raster = raster, .shader = &shader, .descriptor = vertexDescriptor,
				.colourFormats = a_frame.colourFormats, .depthFormat = a_frame.inputs.depthFormat };
			if (const auto cached = std::ranges::find(s_programs, inputs, &CachedProgram::inputs); cached != s_programs.end()) {
				a_draw.key = cached->key;
				a_draw.program = cached->program;
			} else {
				// The vertex stage reads Wading, which State strips from the pixel descriptor, so the key is the vertex descriptor.
				a_draw.key.descriptor = vertexDescriptor;
				a_draw.key.defineStage = Family::DefineStage::Vertex;
				if (!Family::DescribeStates(a_draw.key, blend, depthStencil, raster)) {
					a_why = "a depth-stencil, blend or rasterizer state with no description";
					return false;
				}
				a_draw.key.colourFormats = a_frame.colourFormats;
				a_draw.key.depthFormat = a_frame.inputs.depthFormat;
				a_draw.program = WaterDraws::Program(a_draw.key, shader);
				if (!a_draw.program) {
					a_why = "its ORG program cannot be built";
					return false;
				}
				// Bounded: the states come from fixed engine tables, so only a changed target format makes new entries.
				if (s_programs.size() >= 256)
					s_programs.clear();
				s_programs.push_back({ inputs, a_draw.key, a_draw.program });
			}
			a_draw.colours = a_frame.colours;
			a_draw.stencilRef = a_modes.stencilRef;

			// The constants: the port's rows, written through the CS shaders' tables as the engine writes them.
			auto frame = a_frame.port;
			frame.linearLighting = globals::features::linearLighting.IsLinearLightingActive();
			const auto object = Port::CaptureObject(water, *geometry);
			const auto bytes = Port::BytesOf(*material);
			const auto rows = Port::Assemble(Port::EvaluateTechnique(descriptor, frame), Port::EvaluateMaterial(bytes, frame), Port::EvaluateObject(object, descriptor, frame),
				Port::EvaluateGeometry(object, descriptor, frame), Port::EvaluateLights(*a_pass, bytes, descriptor, frame), TablesOf(*vs, *ps));
			auto& c = a_draw.latch.constants;
			PutRows(rows.vsTechnique, c.vsTechnique);
			PutRows(rows.vsMaterial, c.vsMaterial);
			PutRows(rows.vsGeometry, c.vsGeometry);
			PutRows(rows.psTechnique, c.psTechnique);
			PutRows(rows.psMaterial, c.psMaterial);
			PutRows(rows.psGeometry, c.psGeometry);
			a_frame.StrictLights(*a_pass, a_draw.latch.strictLights);
			std::memcpy(a_draw.latch.directionalShadow, a_frame.inputs.directionalShadow, kDirectionalShadowBytes);
			// CS's permutation block, as its BeginTechnique hook sets it. The water shaders read the extra descriptor's InWorld bit.
			const auto& permutation = state->permutationData;
			c.descriptors[0] = descriptor;
			c.descriptors[1] = Util::ShaderDescriptor::UploadedPixelDescriptor(descriptor, pixelDescriptor, State::GetRuntimePixelBits(shader));
			c.descriptors[2] = permutation.ExtraShaderDescriptor;
			c.descriptors[3] = permutation.ExtraFeatureDescriptor;
			c.effectRadius = permutation.EffectRadius;
			c.physSky = a_frame.inputs.physSky;

			// The geometry: the TriShape's buffers and its whole index range, as the dispatcher draws it.
			const auto* shape = reinterpret_cast<const Family::TriShape*>(geometry->GetGeometryRuntimeData().rendererData);
			const auto triangles = static_cast<RE::BSTriShape*>(geometry)->GetTrishapeRuntimeData().triangleCount;
			Family::Geometry latched;
			std::string detail;
			if (const auto result = Family::LatchGeometry(shape, 0, triangles, vs->vertexDesc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, latched, a_owners, &detail);
				result != Family::GeometryResult::Ok) {
				a_why = std::format("geometry: {} {}", Family::ReasonOf(result), detail);
				return false;
			}
			c.vertexRef = latched.vertexRef;
			c.indexRef = latched.indexRef;
			c.vertexLayout[0] = static_cast<std::uint32_t>(latched.layout);
			c.vertexLayout[1] = static_cast<std::uint32_t>(latched.layout >> 32);
			c.vertexStride = latched.stride;
			c.firstIndex = latched.firstIndex;
			a_draw.indexCount = latched.indexCount;

			// The textures. An unbound slot reads zero, with the first sampler.
			std::array<Slot, kMaterialSlots> slots;
			if (!SlotsOf(a_frame, descriptor, reinterpret_cast<const std::byte*>(material), slots, a_why))
				return false;
			for (std::uint32_t t = 0; t < kMaterialSlots; ++t) {
				const auto& slot = slots[t];
				GpuTextures::ResolveFailure failure = GpuTextures::ResolveFailure::None;
				const auto result = Family::SlotTexture(slot.view, MaterialKind(t), GpuTextures::Tag(GpuTextures::Source::FrameSlot, t), c.textures[t], a_owners, &failure);
				if (result == Family::SlotResult::Published) {
					// An engine target a graph pass writes is read by published id, so the graph orders the write before the water.
					const auto id = PublishedResources::ExportedId(slot.view);
					if (const auto read = WaterDraws::ReadPublished(a_frame.inputs.view, id, c.textures[t]); read != Family::InputResult::Graph) {
						a_why = std::format("t{} holds '{}': {}", t, id, Family::ReasonOf(read));
						return false;
					}
				} else if (result != Family::SlotResult::Ok) {
					a_why = std::format("t{}: {}", t, GpuTextures::DescribeFailure(slot.view, t, failure));
					return false;
				}
				c.samplers[t] = Family::SlotSampler(slot.address, slot.filter, a_owners);
				if (c.samplers[t] == GpuTextures::kInvalid) {
					a_why = "its sampler modes have no graph sampler";
					return false;
				}
			}
			a_draw.viewport = Family::LatchViewport(a_modes.depthBias);
			return true;
		}
	}

	bool BuildBatch(const BatchInputs& a_inputs, std::vector<Draw>& a_draws, Family::Owners& a_owners, std::string& a_why)
	{
		DCLF_ZONE("CS.DCLF.Water.Native.Build");
		a_draws.clear();
		auto* batch = a_inputs.accumulator ? BatchOf(a_inputs) : nullptr;
		if (!batch) {
			a_why = "the accumulator has no batch renderer";
			return false;
		}
		Frame frame;
		if (!frame.Read(a_inputs, a_why))
			return false;
		Modes modes = frame.start;
		bool ok = true;
		ForEachTechnique(batch, a_inputs.first, a_inputs.last, [&](std::uint32_t, std::byte* a_group) {
			if (!WaterGroup(a_group))
				return true;
			for (std::uint32_t sub = 0; sub < 5 && ok; ++sub)
				for (auto* pass = Field<RE::BSRenderPass*>(a_group, sub * 8); pass && ok; pass = pass->passGroupNext)
					ok = BuildPass(frame, pass, modes, a_draws.emplace_back(), a_owners, a_why);
			return ok;
		});
		return ok;
	}

	Detached DetachBatch(const BatchInputs& a_inputs)
	{
		Detached detached;
		auto* batch = a_inputs.accumulator ? BatchOf(a_inputs) : nullptr;
		if (!batch)
			return detached;
		detached.restore = !Field<bool>(batch, 0x6C);
		ForEachTechnique(batch, a_inputs.first, a_inputs.last, [&](std::uint32_t, std::byte* a_group) {
			if (!WaterGroup(a_group))
				return true;
			Detached::Entry entry;
			entry.group = a_group;
			auto* heads = reinterpret_cast<RE::BSRenderPass**>(a_group);
			for (std::uint32_t sub = 0; sub < 5; ++sub)
				entry.heads[sub] = std::exchange(heads[sub], nullptr);
			entry.bits = std::exchange(*reinterpret_cast<std::uint32_t*>(a_group + 0x28), 0u);
			detached.entries.push_back(entry);
			return true;
		});
		return detached;
	}

	void Reattach(Detached& a_detached)
	{
		if (!a_detached.restore)
			return;
		for (const auto& entry : a_detached.entries) {
			auto* group = static_cast<std::byte*>(entry.group);
			auto* heads = reinterpret_cast<RE::BSRenderPass**>(group);
			for (std::uint32_t sub = 0; sub < 5; ++sub)
				heads[sub] = entry.heads[sub];
			*reinterpret_cast<std::uint32_t*>(group + 0x28) = entry.bits;
		}
		a_detached.entries.clear();
	}
}
