// DrawPipelines::Impl and helpers shared by the pipeline translation units. Nothing here runs per draw.

#pragma once

// volk must precede every Vulkan header in a translation unit.
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <rhi_interop_vulkan.h>

#include "Features/DrawcallLimitFix/Core/Gpu/BuildExecutor.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelines.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DrawPipelinesRhi.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/EngineStates.h"
#include "Features/DrawcallLimitFix/Core/Extensions/DrawFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/MaterialPorts.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Materials.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ProgramFamilies.h"
#include "Features/DrawcallLimitFix/Core/Extensions/Views.h"
#include "RenderGraph/Async/EventQueue.h"
#include "RenderGraph/Async/LatestSlot.h"
#include "Features/DrawcallLimitFix/Core/Scene/SceneWake.h"
#include "Features/DrawcallLimitFix/Core/Settings/Switches.h"

#include "RenderGraph/RenderGraphRuntime.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/DxilStages.h"
#include "Features/DrawcallLimitFix/Core/Gpu/Pipelines/SpirvReflection.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingConstants.h"
#include "Features/DrawcallLimitFix/Core/Shading/EngineShaders.h"
#include "Features/DrawcallLimitFix/Core/Shading/LightingDescriptors.h"
#include "Features/DrawcallLimitFix/Core/Scene/Model/Lookups.h"
#include "Features/DrawcallLimitFix/Core/Shading/VertexInput.h"
#include "ShaderCache.h"

#include <Tracy/Tracy.hpp>

#include <OpenRenderGraph/GraphHost.h>
#include <ORGModuleServices/PipelineRegistry.h>
#include <rhi_helpers.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include "Features/DrawcallLimitFix/Diagnostics/Observation.h"
#include "Features/DrawcallLimitFix/Diagnostics/ParityCounter.h"
#include "Features/DrawcallLimitFix/Diagnostics/Policy.h"

namespace DCLF
{
	// Inline so the names stay unqualified, as in the old single unit's anonymous namespace, without sharing DCLF's linkage names.
	inline namespace DrawPipelinesDetail
	{
		/**
		 * @brief Whether a fragment program defeats early depth testing by discarding or writing depth, sample mask or memory.
		 * Such shadow pipelines go in the discarding class, drawn after the others.
		 */
		inline bool FragmentDefersDepth(std::span<const std::byte> a_spirv)
		{
			if (DxilStages::IsDxil(a_spirv))
				return DxilStages::Fragment(a_spirv).defersDepth;
			const std::size_t words = a_spirv.size() / 4;
			if (words < 5)
				return true;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_spirv.data() + i * 4, 4);
				return w;
			};
			constexpr std::uint32_t kOpDecorate = 71, kOpMemberDecorate = 72, kOpImageWrite = 99, kOpAtomicFirst = 227, kOpAtomicLast = 242,
									kOpKill = 252, kOpTerminateInvocation = 4416, kOpDemoteToHelperInvocation = 5380, kDecorationBuiltIn = 11,
									kBuiltInSampleMask = 20, kBuiltInFragDepth = 22;
			auto depthOrMask = [](std::uint32_t a_builtIn) { return a_builtIn == kBuiltInFragDepth || a_builtIn == kBuiltInSampleMask; };
			for (std::size_t i = 5; i < words;) {
				const std::uint32_t first = word(i);
				const std::uint32_t count = first >> 16, op = first & 0xFFFF;
				if (count == 0 || i + count > words)
					return true;
				if (op == kOpKill || op == kOpTerminateInvocation || op == kOpDemoteToHelperInvocation || op == kOpImageWrite ||
					(op >= kOpAtomicFirst && op <= kOpAtomicLast))
					return true;
				if (op == kOpDecorate && count >= 4 && word(i + 2) == kDecorationBuiltIn && depthOrMask(word(i + 3)))
					return true;
				if (op == kOpMemberDecorate && count >= 5 && word(i + 3) == kDecorationBuiltIn && depthOrMask(word(i + 4)))
					return true;
				i += count;
			}
			return false;
		}

		/** @brief Which of FragDepth and SampleMask a fragment program writes, for its state class. */
		inline void FragmentExports(std::span<const std::byte> a_spirv, bool& a_depth, bool& a_sampleMask)
		{
			a_depth = a_sampleMask = false;
			if (DxilStages::IsDxil(a_spirv)) {
				const auto facts = DxilStages::Fragment(a_spirv);
				a_depth = facts.exportsDepth;
				a_sampleMask = facts.exportsSampleMask;
				return;
			}
			const std::size_t words = a_spirv.size() / 4;
			auto word = [&](std::size_t i) {
				std::uint32_t w;
				std::memcpy(&w, a_spirv.data() + i * 4, 4);
				return w;
			};
			constexpr std::uint32_t kOpDecorate = 71, kOpMemberDecorate = 72, kDecorationBuiltIn = 11, kBuiltInSampleMask = 20, kBuiltInFragDepth = 22;
			for (std::size_t i = 5; i < words;) {
				const std::uint32_t first = word(i);
				const std::uint32_t count = first >> 16, op = first & 0xFFFF;
				if (count == 0 || i + count > words)
					return;
				std::uint32_t builtIn = ~0u;
				if (op == kOpDecorate && count >= 4 && word(i + 2) == kDecorationBuiltIn)
					builtIn = word(i + 3);
				else if (op == kOpMemberDecorate && count >= 5 && word(i + 3) == kDecorationBuiltIn)
					builtIn = word(i + 4);
				a_depth = a_depth || builtIn == kBuiltInFragDepth;
				a_sampleMask = a_sampleMask || builtIn == kBuiltInSampleMask;
				i += count;
			}
		}
		inline constexpr std::uint32_t kMaxLoggedFailures = 8;

		// Register classes as shifted by the SPIR-V builds, set 0.
		inline constexpr std::uint32_t kRecordAddressBinding = kDrawPushBinding;
		// The vertex stage may declare t121 to t127. t125 is pixel-only but inside the run.
		inline constexpr std::uint32_t kObjectBufferBinding = kBindingShiftT + kObjectBufferRegister;
		inline constexpr std::uint32_t kVertexTextureBinding = kBindingShiftT + kShadingBufferRegister;
		inline constexpr std::uint32_t kVertexTextureCount = kObjectBufferRegister - kShadingBufferRegister + 1;
		inline constexpr std::uint32_t kDescriptorHeapBindings = 1000000;  // DXC's ResourceDescriptorHeap bindings

		inline bool InRange(std::uint32_t a_binding, std::uint32_t a_first, std::uint32_t a_count)
		{
			return a_binding >= a_first && a_binding < a_first + a_count;
		}

		inline const char* KindName(SpirvReflection::BindingKind a_kind)
		{
			switch (a_kind) {
			case SpirvReflection::BindingKind::ConstantBuffer:
				return "constant buffer";
			case SpirvReflection::BindingKind::Texture:
				return "texture";
			case SpirvReflection::BindingKind::Sampler:
				return "sampler";
			case SpirvReflection::BindingKind::StorageBuffer:
				return "storage buffer";
			default:
				return "resource";
			}
		}

		// Every binding a stage declares must be one the layout maps for that stage.
		inline void CheckBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				bool mapped = binding.set == 0;
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					// The two push blocks, the draw's (b190) and the pass's (b191).
					mapped = mapped && (InRange(binding.binding, kBindingShiftB, kConstantBufferRegisters) || binding.binding == kRecordAddressBinding ||
										   binding.binding == kFramePushBinding);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					// The pixel stage maps the whole t range, the vertex stage only the object-buffer run.
					mapped = mapped && (a_pixel ? InRange(binding.binding, kBindingShiftT, kTextureRegisters) : InRange(binding.binding, kVertexTextureBinding, kVertexTextureCount));
					break;
				case Kind::Sampler:
					mapped = mapped && a_pixel && InRange(binding.binding, kBindingShiftS, kSamplerRegisters);
					break;
				default:
					mapped = false;
					break;
				}
				if (!mapped)
					throw std::runtime_error(fmt::format("{} {} at set {} binding {} is outside the DCLF layout", a_pixel ? "pixel" : "vertex", KindName(binding.kind), binding.set,
						binding.binding));
			}
		}

		// A pulled stage reads its rows itself, so it may declare none of a row's bindings. The Z-prepass layout leaves them out.
		inline void CheckPulledBindings(const SpirvReflection& a_module, bool a_pixel)
		{
			CheckBindings(a_module, a_pixel);
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				const std::uint32_t b = binding.binding - kBindingShiftB, t = binding.binding - kBindingShiftT;
				const bool row = (binding.kind == Kind::ConstantBuffer && (b <= 2 || b == 4)) ||
				                 ((binding.kind == Kind::Texture || binding.kind == Kind::StorageBuffer) && InRange(binding.binding, kBindingShiftT, kTextureRegisters) &&
									 (t < kPixelTextureSlots || Extensions::FeatureTextureSlotOf(t) >= 0)) ||
				                 binding.kind == Kind::Sampler;
				if (row)
					throw std::runtime_error(fmt::format("pulled {} {} at binding {} is a row's", a_pixel ? "pixel" : "vertex", KindName(binding.kind), binding.binding));
			}
		}

		/**
		 * @brief Identity of a Z-prepass pipeline. Slots with equal keys share a depth pipeline, bucket and draw call.
		 * There is no pixel stage unless it defers the depth test for the alpha test.
		 */
		struct ZPipelineKey
		{
			std::uint64_t vertex = 0, pixel = 0;  // wyhash of the modules, pixel 0 without a pixel stage
			std::uint32_t vertexBytes = 0, pixelBytes = 0;
			std::uint32_t cull = 0, depthWrite = 0, depthFunc = 0;
			std::uint32_t depthBias = 0, depthBiasClamp = 0, slopeScaledDepthBias = 0;  // the floats' bits
			// 1 for terrain group 1, whose bucket has a near-block twin. It holds no other slot.
			std::uint32_t terrainNear = 0, padding = 0;

			bool operator==(const ZPipelineKey&) const = default;
		};
		static_assert(std::has_unique_object_representations_v<ZPipelineKey>);
		struct ZPipelineKeyHash
		{
			using is_avalanching = void;
			std::uint64_t operator()(const ZPipelineKey& a_key) const noexcept { return ankerl::unordered_dense::detail::wyhash::hash(&a_key, sizeof(a_key)); }
		};

		inline void AddUsage(const SpirvReflection& a_module, bool a_pixel, RegisterUsage& a_usage)
		{
			using Kind = SpirvReflection::BindingKind;
			for (const auto& binding : a_module.bindings) {
				if (binding.binding >= kDescriptorHeapBindings)
					continue;
				if (binding.binding == kRecordAddressBinding || binding.binding == kFramePushBinding)
					continue;  // push data, not a DrawBindings entry
				switch (binding.kind) {
				case Kind::ConstantBuffer:
					(a_pixel ? a_usage.pixelConstants : a_usage.vertexConstants) |= 1u << (binding.binding - kBindingShiftB);
					break;
				case Kind::Texture:
				case Kind::StorageBuffer:
					{
						const std::uint32_t t = binding.binding - kBindingShiftT;
						a_usage.textures[t / 64] |= 1ull << (t % 64);
					}
					break;
				case Kind::Sampler:
					a_usage.samplers |= 1u << (binding.binding - kBindingShiftS);
					break;
				default:
					break;
				}
			}
		}

		// Named as ShaderCache's GetVariableIndices matches them.
		inline constexpr std::string_view kLightingVSNames[] = { "World", "PreviousWorld", "EyePosition", "LandBlendParams", "TreeParams", "WindTimers", "TextureProj",
			"IndexScale", "WorldMapOverlayParameters", "LeftEyeCenter", "RightEyeCenter", "TexcoordOffset", "HighDetailRange", "FogParam", "FogNearColor", "FogFarColor",
			"Bones" };
		inline constexpr std::string_view kLightingPSNames[] = { "NumLightNumShadowLight", "PointLightPosition", "PointLightColor", "DirLightDirection", "DirLightColor",
			"DirectionalAmbient", "AmbientSpecularTintAndFresnelPower", "MaterialData", "EmitColor", "AlphaTestRef", "ShadowLightMaskSelect", "VPOSOffset",
			"ProjectedUVParams", "ProjectedUVParams2", "ProjectedUVParams3", "SplitDistance", "SSRParams", "WorldMapOverlayParametersPS", "AmbientColor", "FogColor",
			"ColourOutputClamp", "EnvmapData", "ParallaxOccData", "TintColor", "LODTexParams", "SpecularColor", "SparkleParams", "MultiLayerParallaxData",
			"LightingEffectParams", "IBLParams", "LandscapeTexture1to4IsSnow", "LandscapeTexture5to6IsSnow", "LandscapeTexture1to4IsSpecPower",
			"LandscapeTexture5to6IsSpecPower", "SnowRimLightParameters", "CharacterLightParams", "PBRFlags", "PBRParams1", "LandscapeTexture2PBRParams",
			"LandscapeTexture3PBRParams", "LandscapeTexture4PBRParams", "LandscapeTexture5PBRParams", "LandscapeTexture6PBRParams", "PBRParams2",
			"LandscapeTexture1GlintParameters", "LandscapeTexture2GlintParameters", "LandscapeTexture3GlintParameters", "LandscapeTexture4GlintParameters",
			"LandscapeTexture5GlintParameters", "LandscapeTexture6GlintParameters", "MaterialObjectRGBScale" };
		static_assert(std::size(kLightingVSNames) == kLightingVSVariables && std::size(kLightingPSNames) == kLightingPSVariables);

		inline std::span<const std::string_view> LightingNames(bool a_pixel)
		{
			return a_pixel ? std::span<const std::string_view>(kLightingPSNames) : std::span<const std::string_view>(kLightingVSNames);
		}

		// No DCLF pipeline has vertex input, which an execution set would otherwise have to share. Every vertex stage pulls its attributes,
		// so a key's vertex layout must hold the inputs its vertex stage needs.
		inline void CheckVertexAttributes(std::uint32_t a_attributes, std::uint64_t a_vertexLayout)
		{
			// Some skinned eye meshes omit TEXCOORD2. The native input assembler supplies zero, and DCLFPullFloat does the same.
			constexpr std::uint32_t optionalEyeParameter = 1u << 8;
			if (const std::uint32_t missing = a_attributes & ~optionalEyeParameter & ~VertexLayoutAttributes(a_vertexLayout))
				throw std::runtime_error(fmt::format("vertex attributes {:03X} (BSGraphics::Vertex::Attribute bits) are not in the geometry's layout {:016X}", missing,
					a_vertexLayout));
		}
		inline rhi::LayoutBindingRange LayoutRange(std::uint32_t a_binding, std::uint32_t a_count, rhi::ShaderStage a_stage, rhi::LayoutRangeSource a_source, std::size_t a_offset,
			bool a_samplers = false)
		{
			rhi::LayoutBindingRange range{};
			range.set = 0;
			range.binding = a_binding;
			range.count = a_count;
			range.visibility = a_stage;
			range.source = a_source;
			range.recordOffset = static_cast<std::uint32_t>(a_offset);
			range.samplers = a_samplers;
			return range;
		}
		// A range whose record or pushed address is at a_word of push range a_root.
		inline rhi::LayoutBindingRange RangeFrom(rhi::LayoutBindingRange a_range, std::uint32_t a_root, std::uint32_t a_word)
		{
			a_range.addressRootIndex = a_root;
			a_range.addressOffset32 = a_word;
			return a_range;
		}
		inline rhi::LayoutBindingRange PushedRange(std::uint32_t a_binding, rhi::ShaderStage a_stage, std::uint32_t a_root, std::uint32_t a_word)
		{
			return RangeFrom(LayoutRange(a_binding, 1, a_stage, rhi::LayoutRangeSource::PushAddress, 0), a_root, a_word);
		}
		inline rhi::LayoutBindingRange FromMaterialRow(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 0, kDrawPushMaterialRow); }
		inline rhi::LayoutBindingRange FromFrameRecord(rhi::LayoutBindingRange a_range) { return RangeFrom(a_range, 1, kFramePushRecord); }

		std::vector<rhi::LayoutBindingRange> MainLayoutRanges(bool a_pulled);
		std::vector<rhi::LayoutBindingRange> ShadowLayoutRanges();

		struct KeyDifference
		{
			std::uint32_t bits = 0;
			std::string fields;
			void Field(const char* a_name, std::uint64_t a_from, std::uint64_t a_to)
			{
				if (a_from == a_to)
					return;
				bits += static_cast<std::uint32_t>(std::popcount(a_from ^ a_to));
				fields += fmt::format("{}{} {:X} -> {:X} (bits {:X})", fields.empty() ? "" : ", ", a_name, a_from, a_to, a_from ^ a_to);
			}
		};

		/**
		 * @brief Everything of one variant's pipeline but its shader modules. An execution set's pipelines must agree on all of it
		 * (VK_EXT_device_generated_commands), so this keys a state class.
		 */
		struct PipelineStateDesc
		{
			rhi::RasterState raster{};
			rhi::DepthStencilState depth{};
			rhi::BlendState blend{};
			rhi::RenderTargets targets{};
			rhi::Format depthFormat = rhi::Format::Unknown;
			rhi::PrimitiveTopology topology = rhi::PrimitiveTopology::TriangleList;
			rhi::PipelineFlags flags = rhi::PipelineFlags_None;
			std::uint32_t outputLocations = 0;
			bool hasPixel = false, exportsDepth = false, exportsSampleMask = false, defersDepth = false;
		};
		/** @brief A state class's key, both variants' state, since a set index selects the same entry in both variants' sets. */
		struct StateClassKey
		{
			std::array<PipelineStateDesc, kVariantCount> variants{};
		};

		/**
		 * @brief Each field of a_state as SameState compares them. Blend attachments are expanded as the Vulkan backend does
		 * (rhi_vulkan.cpp), and floats compare by bits.
		 */
		template <class Field>
		void StateFields(const PipelineStateDesc& a_state, Field&& a_field)
		{
			const auto& r = a_state.raster;
			a_field("fill", static_cast<std::uint64_t>(r.fill));
			a_field("cull", static_cast<std::uint64_t>(r.cull));
			a_field("frontCCW", r.frontCCW);
			a_field("depthBias", std::bit_cast<std::uint32_t>(r.depthBias));
			a_field("depthBiasClamp", std::bit_cast<std::uint32_t>(r.depthBiasClamp));
			a_field("slopeScaledDepthBias", std::bit_cast<std::uint32_t>(r.slopeScaledDepthBias));
			a_field("conservative", r.conservative);
			a_field("depthEnable", a_state.depth.depthEnable);
			a_field("depthWrite", a_state.depth.depthWrite);
			a_field("depthFunc", static_cast<std::uint64_t>(a_state.depth.depthFunc));
			const auto& b = a_state.blend;
			a_field("alphaToCoverage", b.alphaToCoverage);
			a_field("independentBlend", b.independentBlend);
			const std::uint32_t attachments = std::min<std::uint32_t>(std::max(a_state.targets.count, b.numAttachments), 8);
			a_field("blend attachments", attachments);
			for (std::uint32_t i = 0; i < attachments; ++i) {
				const auto& a = b.attachments[std::min(i, b.numAttachments ? b.numAttachments - 1 : 0u)];
				a_field("blend enable", (std::uint64_t(i) << 32) | (a.enable ? 1u : 0u));
				a_field("blend colour", (std::uint64_t(i) << 32) | (std::uint64_t(a.srcColor) << 16) | (std::uint64_t(a.dstColor) << 8) | std::uint64_t(a.colorOp));
				a_field("blend alpha", (std::uint64_t(i) << 32) | (std::uint64_t(a.srcAlpha) << 16) | (std::uint64_t(a.dstAlpha) << 8) | std::uint64_t(a.alphaOp));
				a_field("write mask", (std::uint64_t(i) << 32) | static_cast<std::uint64_t>(a.writeMask));
			}
			a_field("targets", a_state.targets.count);
			for (std::uint32_t i = 0; i < a_state.targets.count && i < 8; ++i)
				a_field("target format", (std::uint64_t(i) << 32) | static_cast<std::uint64_t>(a_state.targets.formats[i]));
			a_field("depth format", static_cast<std::uint64_t>(a_state.depthFormat));
			a_field("topology", static_cast<std::uint64_t>(a_state.topology));
			a_field("flags", static_cast<std::uint64_t>(a_state.flags));
			a_field("output locations", a_state.outputLocations);
			a_field("pixel stage", a_state.hasPixel);
			a_field("FragDepth", a_state.exportsDepth);
			a_field("SampleMask", a_state.exportsSampleMask);
			a_field("defers depth", a_state.defersDepth);
		}

		/** @brief Whether two keys are one state class. a_fields gets the differing fields, for the set class parity. */
		inline bool SameState(const StateClassKey& a_left, const StateClassKey& a_right, KeyDifference* a_fields = nullptr)
		{
			bool same = true;
			for (std::uint32_t v = 0; v < kVariantCount; ++v) {
				std::vector<std::pair<const char*, std::uint64_t>> left, right;
				StateFields(a_left.variants[v], [&](const char* a_name, std::uint64_t a_value) { left.emplace_back(a_name, a_value); });
				StateFields(a_right.variants[v], [&](const char* a_name, std::uint64_t a_value) { right.emplace_back(a_name, a_value); });
				if (left == right)
					continue;
				same = false;
				if (!a_fields)
					break;
				const std::size_t common = std::min(left.size(), right.size());
				for (std::size_t f = 0; f < common; ++f)
					if (left[f] != right[f])
						a_fields->Field(fmt::format("{}: {}", v == kDepthVariant ? "depth" : "colour", left[f].first).c_str(), left[f].second, right[f].second);
				if (left.size() != right.size())
					a_fields->Field(v == kDepthVariant ? "depth: fields" : "colour: fields", left.size(), right.size());
			}
			return same;
		}
		struct StateClassHash
		{
			using is_avalanching = void;
			std::uint64_t operator()(const StateClassKey& a_key) const noexcept
			{
				std::uint64_t hash = 0xcbf29ce484222325ull;
				for (const auto& variant : a_key.variants)
					StateFields(variant, [&](const char*, std::uint64_t a_value) { hash = ankerl::unordered_dense::detail::wyhash::mix(hash ^ a_value, 0x9E3779B97F4A7C15ull); });
				return hash;
			}
		};
		struct StateClassEqual
		{
			bool operator()(const StateClassKey& a_left, const StateClassKey& a_right) const { return SameState(a_left, a_right); }
		};
	}

	struct DrawPipelines::Impl
	{
		struct Entry
		{
			std::uint32_t index = kNotReady;  // handed out once a published set version holds the pipeline
			std::uint32_t slot = kNotReady;
			bool failed = false;
		};
		/** @brief A RequestLighting key and the slot that asked. */
		struct LightingRequest
		{
			PipelineKey key{};
			std::uint32_t slot = ~0u;
		};
		/** @brief A RequestShadow key, with the casters' key and occlusion view for the warnings. */
		struct ShadowRequest
		{
			ShadowPipelineKey viewKey{};
			ShadowPipelineKey casterKey{};
			std::uint32_t occlusion = ~0u;
		};

		/**
		 * @brief One version of the state classes' execution sets and signatures. New pipelines are written only into a version nothing
		 * references, at indices it never held, then published. Recordings keep the version they used, so a set is never written while
		 * in use (VUID-VkGeneratedCommandsInfoEXT-preprocessSize-11071, VUID-VkWriteIndirectExecutionSetPipelineEXT-index-11029).
		 */
		struct SetVersion
		{
			/** @brief A state class's sets, one per variant with the same index in both, and their signatures. */
			struct ClassSets
			{
				std::array<rhi::IndirectPipelineSetPtr, kVariantCount> sets;
				std::array<rhi::CommandSignaturePtr, kVariantCount> signatures;
				std::array<std::vector<rhi::PipelineHandle>, kVariantCount> written;
				std::array<std::uint32_t, kVariantCount> capacity{};
				std::uint32_t applied = 0;
			};
			std::vector<ClassSets> classes;
			// By set index: class << kSetClassShift | index in the class.
			std::vector<std::uint32_t> classOf;
			// Held while a recording holds the version.
			std::vector<org::services::PipelinePayload> pipelines;
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			std::uint32_t applied = 0;
		};
		// The shadow views' pipelines, versioned the same way.
		struct ShadowSetVersion
		{
			std::vector<org::services::PipelinePayload> pipelines;
			std::vector<std::uint64_t> layouts;
			std::vector<std::uint8_t> discards;
			std::uint32_t applied = 0;
		};
		// As many versions as are held at once. A new one is made when none is free, so a new pipeline never waits for a recording.
		// ORG's recording reuse can hold a version while a recording may be resubmitted.
		template <class Version>
		struct Versions
		{
			std::vector<std::shared_ptr<Version>> owned;
			std::atomic<std::shared_ptr<Version>> published;
			// Previous generations' versions, dropped once nothing holds them.
			std::vector<std::shared_ptr<Version>> retired;
			std::uint32_t handedOut = 0;  // pipelines whose Entry::index is set
			bool failureLogged = false;

			void Retire()
			{
				for (auto& version : owned)
					if (version)
						retired.push_back(std::move(version));
				owned.clear();
				published.store(nullptr);
				handedOut = 0;
			}

			enum class PublishResult
			{
				Current,
				Published,
				Waiting,  // every other version is still held, so admissions wait
				Failed,
			};

			/** @brief Brings an unreferenced version up to a_admitted and publishes it. */
			template <class Apply>
			PublishResult Publish(std::uint32_t a_admitted, Apply&& a_apply)
			{
				std::erase_if(retired, [](const std::shared_ptr<Version>& a_version) { return a_version.use_count() == 1; });
				const auto current = published.load();
				if (current && current->applied == a_admitted)
					return PublishResult::Current;
				auto free = std::ranges::find_if(owned, [&](const std::shared_ptr<Version>& a_version) {
					return !a_version || (a_version != current && a_version.use_count() == 1);
				});
				if (free == owned.end())
					free = owned.insert(owned.end(), nullptr);
				{
					auto& version = *free;
					// Every other owner has let go, so whatever a recording did with it happened before this.
					std::atomic_thread_fence(std::memory_order_acquire);
					if (!version)
						version = std::make_shared<Version>();
					if (!a_apply(*version)) {
						// Start over, since what it holds may be partial.
						version.reset();
						return PublishResult::Failed;
					}
					version->applied = a_admitted;
					published.store(version);
					return PublishResult::Published;
				}
			}
		};

		rhi::Device device{};
		rhi::PipelineLayoutPtr layout;
		rhi::PipelineLayoutPtr shadowLayout;
		rhi::PipelineLayoutPtr zLayout;
		rhi::CommandSignaturePtr zDrawSignature;
		// Draw families' signatures. Their pipelines belong to the lane.
		std::array<rhi::CommandSignaturePtr, kDrawFamilyLanes> familySignatures;
		// A 20-byte record, an instance word then DrawInstanced's arguments. D3D12 reads both, Vulkan only the draw.
		rhi::CommandSignaturePtr familyInstanceWordSignature;
		/** @brief a_family's draw signature, or null if it could not be made. Valid after InstallFeatures. */
		const rhi::CommandSignaturePtr& FamilySignature(Api::DrawFamily a_family) const
		{
			const bool instanceWord = Extensions::DrawFamilyOf(a_family).contribution.program.signature == Api::DrawFamilySignature::InstanceWordDrawInstanced;
			return instanceWord ? familyInstanceWordSignature : familySignatures[static_cast<std::size_t>(a_family)];
		}
		struct FamilyBuilt
		{
			rhi::PipelinePtr depth;
			std::array<rhi::PipelinePtr, 2> colours;  // the second is null with one colour program
		};
		// Render thread. Each is asked of the lane only once.
		std::array<bool, kDrawFamilyLanes> familyAsked{};
		bool refractionAsked = false;
		ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash> forwardAsked;
		rhi::PipelineLayoutHandle ShadowLayout() const { return shadowLayout->GetHandle(); }
		// The refraction's plain indirect draw. On D3D12 the instance word comes first.
		rhi::CommandSignaturePtr shadowDrawSignature;
		rhi::CommandSignaturePtr shadowViewDrawSignature;
		bool supported = false;
		bool d3d12 = false;  // layouts of root constants alone, plain draws only
		bool deviceGenerated = false;
		bool gpuDriven = false;
		bool attempted = false;
		/** @brief A build's result: the pipeline, or why it failed. */
		struct Artifact
		{
			org::services::PipelinePayload payload;
			std::string error;
			explicit operator bool() const noexcept { return payload != nullptr; }
		};
		/** @brief Builds a pipeline once on DCLF's build pool. a_done gets the result on the thread that built it. Nothing waits for it. */
		template <class Done>
		static void RequestBuild(std::function<org::services::PipelinePayload()> a_build, Done a_done)
		{
			buildsInFlight.fetch_add(1, std::memory_order_relaxed);
			org::services::BuildPipeline(BuildSubmitter(), std::move(a_build), [a_done = std::move(a_done)](org::services::LiveBuild a_built) {
				buildsInFlight.fetch_sub(1, std::memory_order_relaxed);
				a_done(Artifact{ std::move(a_built.payload), std::move(a_built.error) });
			});
		}
		/** @brief Builds RequestBuild started that have not finished, so the load hold waits for them. Any thread. */
		static inline std::atomic<std::uint32_t> buildsInFlight{ 0 };
		/** @brief A pipeline's hot reload registration: how to rebuild it and the program it is built from now. */
		struct Live
		{
			org::services::LivePipelineDesc desc;
			std::uint64_t fingerprint = 0;
			std::uint64_t builtFor = 0;  // its program's sources generation
		};
		/** @brief A program kind's hot reload registration. A rebuild runs on DCLF's build pool only when the program changed or is forced. */
		template <class P, class RebuildProgram, class BuildPayload>
		static std::shared_ptr<const Live> MakeLive(std::string a_id, std::string a_source, const P& a_program, RebuildProgram a_rebuildProgram, BuildPayload a_build)
		{
			auto live = std::make_shared<Live>();
			live->desc.id = std::move(a_id);
			live->desc.sources = { std::move(a_source) };
			live->desc.rebuild = [a_rebuildProgram, a_build](const org::services::LiveRebuildRequest& a_request, org::services::LiveRebuildDone a_done) {
				a_rebuildProgram([a_request, a_build, a_done = std::move(a_done)](std::shared_ptr<const P> a_rebuilt, std::string a_error) {
					if (!a_rebuilt)
						return a_done({ .error = std::move(a_error) });
					const std::uint64_t fingerprint = a_rebuilt->fingerprint;
					if (fingerprint == a_request.fingerprint && !a_request.force)
						return a_done({ .fingerprint = fingerprint });
					// The program's last compile may have completed on any thread.
					auto task = [a_rebuilt, a_build, a_done, fingerprint] {
						try {
							a_done({ .payload = a_build(*a_rebuilt), .fingerprint = fingerprint });
						} catch (const std::exception& e) {
							a_done({ .error = e.what() });
						}
					};
					if (!BuildSubmitter()(task))
						task();
				});
			};
			live->fingerprint = a_program.fingerprint;
			live->builtFor = a_program.sourcesGeneration;
			return live;
		}
		/** @brief The lane. A newly admitted pipeline's registry slot, or null without a registry. */
		static std::shared_ptr<org::services::LivePipelineSlot> RegisterLive(const std::shared_ptr<const Live>& a_live, const org::services::PipelinePayload& a_payload)
		{
			auto* registry = RenderGraphRuntime::PipelineRegistry();
			if (!a_live || !registry)
				return {};
			return registry->Register(a_live->desc, a_payload, a_live->fingerprint, a_live->builtFor);
		}

		// Build completions. Each callback pushes its artifact and wakes the lane, which drains them.
		// A completion from before a target or format change is dropped by its generation.
		struct MainDone
		{
			PipelineKey key;
			std::uint32_t generation = 0;
			Artifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct ShadowDone
		{
			ShadowPipelineKey key;
			std::uint32_t generation = 0;
			Artifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct FamilyDone
		{
			std::uint32_t generation = 0;
			Artifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct ForwardDone
		{
			ForwardPipelineKey key;
			std::uint32_t generation = 0;
			Artifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct RefractionDone
		{
			std::uint32_t generation = 0;
			DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // the refraction normals format it was built for
			Artifact artifact;
			std::shared_ptr<const Live> live;
		};
		struct Completions
		{
			RenderGraph::Frame::EventQueue<MainDone, 1024> main;
			RenderGraph::Frame::EventQueue<ShadowDone, 1024> shadow;
			std::array<RenderGraph::Frame::EventQueue<FamilyDone, 16>, kDrawFamilyLanes> family;
			RenderGraph::Frame::EventQueue<ForwardDone, 256> forward;
			RenderGraph::Frame::EventQueue<RefractionDone, 16> refraction;
			// Requests to the lane, and the lane's engine-state requests back to the render thread.
			RenderGraph::Frame::EventQueue<LightingRequest, 1024> requests;
			RenderGraph::Frame::EventQueue<ShadowRequest, 1024> shadowRequests;
			RenderGraph::Frame::EventQueue<ForwardPipelineKey, 256> forwardRequests;
			std::array<std::atomic<bool>, kDrawFamilyLanes> familyRequest{};
			std::atomic<bool> refractionRequest{ false };
			RenderGraph::Frame::EventQueue<std::uint32_t, 64> stateRequests;
		};
		// Never freed, because a build may complete while the process tears down.
		Completions* completions = new Completions;

		// Builds run on DCLF's preparation pool. A shader reload's start and its swap wake the lane.
		Impl()
		{
			if (auto* registry = RenderGraphRuntime::PipelineRegistry())
				registry->AddListener([] { WakePipelineLane(); });
		}

		// Render thread. The lane reads the posted copy.
		struct ShadowRasterState
		{
			float depthBias = 0.0f;
			float depthBiasClamp = 0.0f;
			float slopeScaledDepthBias = 0.0f;
			rhi::CullMode cull = rhi::CullMode::Back;
			bool frontCCW = false;
			// D3D11's DepthClipEnable. Off clamps depth to the viewport's range instead of clipping.
			bool depthClip = true;

			bool operator==(const ShadowRasterState&) const = default;
		};
		std::vector<ShadowRasterState> shadowRasterStates;

		/** @brief Render thread. The engine's FrontCounterClockwise, read once. Every entry and every CS cascade copy has the same winding. */
		std::optional<bool> EngineFrontCCW()
		{
			if (!engineFrontCCW) {
				if (auto* state = EngineRasterStates()[0][1][0][0]) {
					D3D11_RASTERIZER_DESC desc{};
					state->GetDesc(&desc);
					engineFrontCCW = desc.FrontCounterClockwise != FALSE;
					logger::info("[DCLF] engine rasterizer winding: front face {}", *engineFrontCCW ? "counter-clockwise" : "clockwise");
				}
			}
			return engineFrontCCW;
		}
		std::optional<bool> engineFrontCCW;
		std::uint32_t loggedShadowRasterFailures = 0;
		struct Built
		{
			std::array<rhi::PipelinePtr, kVariantCount> pipelines;
			// Per variant. The Z-prepass pixel stage reads far less than the colour one.
			std::array<RegisterUsage, kVariantCount> usage;
			bool discards = false;
			// Shared by equal Z keys.
			rhi::PipelinePtr pulledDepth;
			// Null when the pulled stages would not build. That range then draws device-generated.
			rhi::PipelinePtr pulledColour;
			rhi::PipelinePtr pulledDecalDepth;
			// A deferred family key's plain pipeline per deferred range, null where it does not draw. Its family's build makes them.
			std::array<rhi::PipelinePtr, Api::kDeferredRanges> pulledDeferred;
			ZPipelineKey zKey;
			std::shared_ptr<const ConstantTables> tables;
			// Not the pulled depth pipeline's state, which plain draws bind.
			StateClassKey stateClass;
		};

		/** @brief The engine's state behind a key's RasterStateBits, read once and kept in RHI terms so builds never touch D3D11. */
		struct EngineState
		{
			float depthBias = 0.0f;
			float depthBiasClamp = 0.0f;
			float slopeScaledDepthBias = 0.0f;
			rhi::BlendState blend{};
			bool valid = false;  // false if a state object was missing or the RHI cannot express it
			// An opaque key takes only the engine's blend write masks. Depth and bias stay the opaque pass's.
			bool blendOnly = false;
		};
		// As the engine draws them, alpha-tested writes every channel (mode 10). Plain opaque leaves RT0's alpha alone (mode 1),
		// so a shader alpha below 1 never reaches the G-buffer.
		static constexpr std::uint32_t kOpaqueWriteMode = 1, kAlphaTestedWriteMode = 10;
		ankerl::unordered_dense::map<std::uint32_t, EngineState> engineStates;
		std::uint32_t loggedStateFailures = 0;

		static bool ToRHI(D3D11_BLEND a_factor, rhi::BlendFactor& a_out);
		static bool ToRHI(D3D11_BLEND_OP a_op, rhi::BlendOp& a_out);

		EngineState ReadEngineState(std::uint32_t a_stateBits, std::string& a_error);

		/** @brief What the builds need that only the render thread can read. Posted whole, latest wins. */
		struct FrameInput
		{
			TargetFormats targets;
			// Every state asked for, valid or not.
			std::shared_ptr<const ankerl::unordered_dense::map<std::uint32_t, EngineState>> states;
			std::optional<bool> frontCCW;
			RE::BSShader* lighting = nullptr;
			std::array<const void*, Extensions::kProgramFamilies> familySource{};
			RE::BSShader* utility = nullptr;
			DXGI_FORMAT shadowFormat = DXGI_FORMAT_UNKNOWN;
			std::shared_ptr<const std::vector<ShadowRasterState>> shadowStates;
			std::array<RE::BSShader*, kDrawFamilyLanes> drawFamilySource{};
			std::array<ForwardTargets, Extensions::kEngineViews> viewTargets{};
			RE::BSShader* refractionUtility = nullptr;
			DXGI_FORMAT refractionFormat = DXGI_FORMAT_UNKNOWN;
			std::uint8_t refractionWriteMask = 0xF;
		};
		::RenderGraph::Frame::LatestSlot<FrameInput> inputSlot;
		FrameInput renderInput;
		std::vector<std::uint32_t> pendingStateBits;  // asked for by the lane but not readable until the deferred pass
		::RenderGraph::Frame::LatestSlot<PipelineCatalog> catalogSlot;

		/** @brief Render thread. Posts the current frame inputs and wakes the lane. */
		void PostInput(const DrawPipelines& a_owner)
		{
			renderInput.targets = a_owner.targets;
			inputSlot.Post(std::make_unique<FrameInput>(renderInput));
			WakePipelineLane();
		}

		struct FamilyCounters
		{
			std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 }, setPublishes{ 0 }, setWaits{ 0 }, zPipelines{ 0 }, zDepthOnly{ 0 };
			void Zero()
			{
				for (auto* counter : { &requested, &ready, &failed, &zPipelines, &zDepthOnly })
					counter->store(0, std::memory_order_relaxed);
			}
		};
		/** @brief A forward pipeline: built, queued, or failed for good this generation. */
		struct ForwardEntry
		{
			org::services::PipelinePayload built;
			std::shared_ptr<org::services::LivePipelineSlot> slot;
			bool requested = false;
			bool failed = false;
		};
		/**
		 * @brief One keyed family of the lane's pipelines. A change of what they are built for starts a new generation.
		 * Queued builds still finish, and their completions are dropped.
		 */
		template <class Key, class Hash, class E, class Asked>
		struct PipelineFamily
		{
			std::uint32_t generation = 0;
			ankerl::unordered_dense::map<Key, E, Hash> entries;
			std::uint32_t failedEntries = 0;
			Asked asked;
			// Keys waiting on their program are retried when it finishes. Keys waiting on an input are retried when it changes.
			ankerl::unordered_dense::map<std::uint64_t, std::vector<Key>> programWaiters;
			std::vector<Key> blocked;
			std::uint32_t loggedFailures = 0;
			FamilyCounters counters;

			/** @brief At capacity, counting queued builds. */
			bool Full() const { return entries.size() - failedEntries >= kMaxPipelines; }
			/** @brief a_key's program can never be made, so its objects stay native. */
			void FailForGood(const Key& a_key)
			{
				entries.try_emplace(a_key).first->second.failed = true;
				++failedEntries;
			}
			template <class Request>
			void RetryProgram(std::uint64_t a_program, Request&& a_request)
			{
				const auto waiting = programWaiters.find(a_program);
				if (waiting == programWaiters.end())
					return;
				const auto keys = std::move(waiting->second);
				programWaiters.erase(waiting);
				for (const auto& key : keys)
					a_request(key);
			}
			template <class Request>
			void RetryBlocked(Request&& a_request)
			{
				for (const auto& key : std::exchange(blocked, {}))
					a_request(key);
			}
			void StartOver()
			{
				entries.clear();
				failedEntries = 0;
				++generation;
				counters.Zero();
				// Every key is requested again once the new inputs are in.
				programWaiters.clear();
				blocked.clear();
				for (const auto& request : asked) {
					if constexpr (requires { request.first; })
						blocked.push_back(request.first);
					else
						blocked.push_back(request);
				}
			}
		};

		/** @brief The lane's state. Only its passes touch it, one at a time on the coordinator, except the report's counters. */
		struct Lane : PipelineFamily<PipelineKey, PipelineKeyHash, Entry, ankerl::unordered_dense::map<PipelineKey, std::uint32_t, PipelineKeyHash>>
		{
			std::shared_ptr<const FrameInput> input;
			ankerl::unordered_dense::set<std::uint32_t> statesAsked;
			ShaderPrograms::Finished finishedPrograms;
			Versions<SetVersion> versions;
			std::vector<org::services::PipelinePayload> setPipelines;  // admitted, not necessarily published
			std::vector<std::shared_ptr<org::services::LivePipelineSlot>> setSlots;
			std::uint64_t registryGeneration = 0;
			std::vector<std::uint32_t> zGroupOf, zGroupFirst;
			ankerl::unordered_dense::map<ZPipelineKey, std::uint32_t, ZPipelineKeyHash> zGroups;
			std::vector<std::array<RegisterUsage, kVariantCount>> usage;
			std::vector<std::shared_ptr<const ConstantTables>> constantTables;
			// State classes in admission order, each pipeline's class word, and each class's members.
			std::vector<std::uint32_t> classOf;
			std::vector<std::vector<std::uint32_t>> classMembers;
			ankerl::unordered_dense::map<StateClassKey, std::uint32_t, StateClassHash, StateClassEqual> classIndex;
			std::atomic<std::uint32_t> stateClasses{ 0 }, largestClass{ 0 };
			Diagnostics::ParityCounter classParity;
			std::atomic<std::uint64_t> classParityChecks{ 0 }, classParityMismatches{ 0 };
			std::uint64_t revision = 0;
			bool dirty = false;  // publish the catalog at the end of the pass
			struct ProgramFamilyCounters
			{
				std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
			};
			std::array<ProgramFamilyCounters, Extensions::kProgramFamilies> programFamilyCounters;
			/** @brief The shadow views' set, kept like the main set but built for the shadow map format. */
			struct Shadow : PipelineFamily<ShadowPipelineKey, ShadowPipelineKeyHash, Entry, ankerl::unordered_dense::map<ShadowPipelineKey, ShadowRequest, ShadowPipelineKeyHash>>
			{
				Versions<ShadowSetVersion> versions;
				std::vector<org::services::PipelinePayload> setPipelines;
				std::vector<std::shared_ptr<org::services::LivePipelineSlot>> setSlots;
				std::vector<std::uint64_t> layouts;
				std::vector<std::uint8_t> discards;
			};
			Shadow shadow;
			/** @brief A draw family's two pipelines, rebuilt with each new main set. */
			struct FamilyLane
			{
				bool asked = false;  // built from here on, for every generation
				bool requested = false;
				bool failed = false;
				bool programFailed = false;  // for good
				org::services::PipelinePayload built;
				std::shared_ptr<org::services::LivePipelineSlot> slot;
				// Kept for the process, because a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
			};
			std::array<FamilyLane, kDrawFamilyLanes> families;
			/** @brief The refraction pipeline, rebuilt with each new main set, normals format or write mask. */
			struct Refraction
			{
				bool asked = false;
				bool requested = false;
				bool failed = false;
				bool programFailed = false;  // for good
				DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
				org::services::PipelinePayload built;
				std::shared_ptr<org::services::LivePipelineSlot> slot;
				// Kept for the process, because a recording may still draw with them.
				std::vector<org::services::PipelinePayload> retired;
				struct Counters
				{
					std::atomic<std::uint32_t> requested{ 0 }, ready{ 0 }, failed{ 0 };
				};
				Counters counters;
				/** @brief Retires the build, keeping it for the process, and keeps the program's verdict. */
				void Reset();
			};
			Refraction refraction;
			/** @brief The forward pipelines. Recordings bind each by handle with no set, so every build is kept for the process. */
			struct Forward : PipelineFamily<ForwardPipelineKey, ForwardPipelineKeyHash, ForwardEntry, ankerl::unordered_dense::set<ForwardPipelineKey, ForwardPipelineKeyHash>>
			{
				std::vector<org::services::PipelinePayload> retired;  // kept for the process
			};
			Forward forward;
		};
		Lane lane;

		/** @brief Publishes a family's admitted pipelines and hands out the published indices. */
		template <class Family, class Version, class Publish, class OnReady>
		typename Versions<Version>::PublishResult PublishAdmitted(Family& a_family, Versions<Version>& a_versions, std::uint32_t a_admitted, const char* a_what, Publish&& a_publish, OnReady&& a_onReady)
		{
			using Result = typename Versions<Version>::PublishResult;
			const auto result = a_versions.Publish(a_admitted, a_publish);
			if (result == Result::Published)
				a_family.counters.setPublishes.fetch_add(1, std::memory_order_relaxed);
			if (result == Result::Waiting)
				a_family.counters.setWaits.fetch_add(1, std::memory_order_relaxed);
			if ((result == Result::Current || result == Result::Published) && a_versions.handedOut != a_admitted) {
				a_versions.handedOut = a_admitted;
				for (auto& [key, entry] : a_family.entries)
					if (entry.index == kNotReady && entry.slot != kNotReady) {
						entry.index = entry.slot;
						a_family.counters.ready.fetch_add(1, std::memory_order_relaxed);
						a_onReady(key);
					}
				lane.dirty = true;
			} else if (result == Result::Failed && !a_versions.failureLogged) {
				a_versions.failureLogged = true;
				logger::warn("[DCLF] Could not publish the {} ({} pipelines admitted)", a_what, a_admitted);
			}
			return result;
		}

		static void DrainLaneThunk();
		void DrainLane();
		/** @brief Requests a key's program, then its pipeline, or parks it until what it lacks arrives. */
		void TryRequest(const PipelineKey& a_key);
		/** @brief Warns about an on-demand build, with its nearest requested relative. */
		void LogOnDemandBuild(const PipelineKey& a_key, Api::ProgramFamily a_family, std::uint32_t a_slot) const;
		/** @brief Starts the forward pipelines over, keeping the built ones for the process. */
		void ResetForward();
		/** @brief After a shader reload moved the sources, request every parked or failed key again. */
		void RetryAfterSourcesMoved();
		/**
		 * @brief Once a reload swapped pipeline slots, take the new payloads wherever the lane keeps them. Changed sets get new versions
		 * and the catalog is published again. This is the one reload step for every kind.
		 */
		void AdoptLivePipelines();
		/** @brief a_index's Z-prepass group and state class, in admission order. */
		void IndexMainBuilt(std::uint32_t a_index, const Built& a_built);
		/** @brief Rebuilds the main set's groups and classes from its pipelines, in index order. */
		void ReindexMainSet();
		/** @brief Admits completed builds, publishes a set version and hands out their indices. */
		void AdmitLane();
		void TryRequestShadow(const ShadowPipelineKey& a_key);
		void AdmitShadow();
		void TryRequestFamily(Api::DrawFamily a_family);
		void TryRequestFamilies();
		void TakeFamilyRequests();
		/** @brief Retries draw family keys that waited on the finished programs. */
		void RetryFamilyPrograms(const ShaderPrograms::Finished& a_finished);
		void TryRequestForward(const ForwardPipelineKey& a_key);
		void TryRequestRefraction();
		/** @brief The forward targets changed, so every forward key is built again. */
		void RecreateForward(const ForwardTargets& a_from, const ForwardTargets& a_to);
		void AdmitFamiliesAndForward();
		void PublishCatalog();
		std::string NearestShadowKey(const ShadowPipelineKey& a_key) const;
		/** @brief For warnings. The requested key with the fewest differing bits, and what differs. */
		std::string NearestKey(const PipelineKey& a_key) const;
		static std::string DescribeKey(const PipelineKey& a_key)
		{
			const auto family = KeyFamily(a_key.rasterFlags);
			return fmt::format("VS {:08X} PS {:08X} pass {:08X} ({}), raster {:X}, vertex layout {:016X}", a_key.vertexDescriptor, a_key.pixelDescriptor,
				a_key.passDescriptor,
				family != Api::ProgramFamily::Count ? std::string_view(Extensions::ProgramFamilyOf(family).keyName) : LightingTechniqueName((a_key.passDescriptor >> 24) & 0x3f),
				a_key.rasterFlags, a_key.vertexLayout);
		}

		bool Initialize();

		/** @brief Initialize's command signatures. False when a required one could not be made. */
		bool CreateDrawSignatures();

		/** @brief A triangle-list pipeline description with no vertex input. Create throws on failure. */
		struct PipelineDesc
		{
			PipelineDesc(rhi::PipelineLayoutHandle a_layout, std::span<const std::byte> a_vertex, std::span<const std::byte> a_pixel, DXGI_FORMAT a_depthFormat) :
				layout{ a_layout }, vertex{ rhi::ShaderStage::Vertex, { a_vertex.data(), static_cast<std::uint32_t>(a_vertex.size()) }, "main" },
				pixel{ rhi::ShaderStage::Pixel, { a_pixel.data(), static_cast<std::uint32_t>(a_pixel.size()) }, "main" }, hasPixel(!a_pixel.empty()),
				depthFormat{ rhi::helpers::ToRHI(a_depthFormat) }
			{
				blend.bs.numAttachments = 0;
			}
			rhi::SubobjLayout layout;
			rhi::SubobjShader vertex, pixel;
			bool hasPixel;
			rhi::SubobjRaster raster{};
			rhi::SubobjDepth depth{};
			rhi::SubobjBlend blend{};
			rhi::SubobjRTVs targets{};
			rhi::SubobjDSV depthFormat;
			rhi::SubobjInputLayout input{};  // always empty, the vertex stages pull their attributes
			rhi::SubobjFlags flags{};

			/** @brief Adds a colour target, blended per its index's blend attachment. */
			void Colour(DXGI_FORMAT a_format)
			{
				targets.rt.formats[targets.rt.count++] = rhi::helpers::ToRHI(a_format);
				blend.bs.numAttachments = targets.rt.count;
			}
			void Colours(const TargetFormats& a_targets)
			{
				for (std::uint32_t i = 0; i < a_targets.colorCount; ++i)
					Colour(a_targets.colors[i]);
			}
			/** @brief The state class part: everything Create passes but the modules, plus the pixel module's fragment interface. */
			PipelineStateDesc State(const SpirvReflection& a_pixel) const
			{
				PipelineStateDesc state;
				state.raster = raster.rs;
				state.depth = depth.ds;
				state.blend = blend.bs;
				state.targets = targets.rt;
				state.depthFormat = depthFormat.dsv;
				state.topology = rhi::PrimitiveTopology::TriangleList;
				state.flags = flags.flags;
				state.hasPixel = hasPixel;
				if (hasPixel) {
					const std::span<const std::byte> module(static_cast<const std::byte*>(pixel.bytecode.data), pixel.bytecode.size);
					state.outputLocations = a_pixel.outputLocations;
					FragmentExports(module, state.exportsDepth, state.exportsSampleMask);
					state.defersDepth = FragmentDefersDepth(module);
				}
				return state;
			}
			void Create(rhi::Device a_device, rhi::PipelinePtr& a_out, std::string_view a_what) const
			{
				const rhi::SubobjPrimitiveTopology topology{ rhi::PrimitiveTopology::TriangleList };
				const rhi::PipelineStreamItem items[] = {
					rhi::Make(layout), rhi::Make(vertex), rhi::Make(raster), rhi::Make(depth), rhi::Make(blend), rhi::Make(targets), rhi::Make(depthFormat),
					rhi::Make(topology), rhi::Make(input), rhi::Make(flags), rhi::Make(pixel),
				};
				// DXVK_PIPELINE_STATS logs statistics under this name.
				const std::string label = fmt::format("DCLF {}", a_what);
				rhi::vulkan::set_pipeline_statistics_label(label.c_str());
				const auto result = a_device.CreatePipeline(items, static_cast<std::uint32_t>(std::size(items)) - (hasPixel ? 0u : 1u), a_out);
				rhi::vulkan::set_pipeline_statistics_label(nullptr);
				// DXVK_SHADER_DUMP_PATH: dump the pixel stage's SPIR-V beside DXVK's dumps.
				if (static const char* dump = std::getenv("DXVK_SHADER_DUMP_PATH"); dump && *dump && hasPixel && pixel.bytecode.data) {
					std::string file = label;
					for (auto& c : file)
						if (!std::isalnum(static_cast<unsigned char>(c)))
							c = '_';
					std::ofstream(std::filesystem::path(dump) / (file + ".fs.spv"), std::ios::binary)
						.write(static_cast<const char*>(pixel.bytecode.data), static_cast<std::streamsize>(pixel.bytecode.size));
				}
				if (result != rhi::Result::Ok)
					throw std::runtime_error(fmt::format("CreatePipeline ({}) failed ({})", a_what, static_cast<int>(result)));
			}
		};

		/** @brief A SPIR-V module's hash. */
		static std::uint64_t ModuleHash(const std::vector<std::byte>& a_module) { return ankerl::unordered_dense::detail::wyhash::hash(a_module.data(), a_module.size()); }

		/** @brief Parses SPIR-V modules, or throws. */
		template <std::size_t N>
		static std::array<SpirvReflection, N> Parse(const std::array<const std::vector<std::byte>*, N>& a_modules)
		{
			std::array<SpirvReflection, N> out;
			for (std::size_t i = 0; i < N; ++i)
				if (!out[i].Parse(*a_modules[i]))
					throw std::runtime_error("not SPIR-V");
			return out;
		}

		/** @brief A draw family's depth pipeline and one colour pipeline per colour program. */
		static org::services::PipelinePayload BuildDrawFamily(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::DrawProgram* a_program,
			std::uint8_t a_colours, TargetFormats a_targets, rhi::BlendState a_blend, bool a_frontCCW, std::string_view a_label);

		/**
		 * @brief The refraction pipeline. FUN_1414b4860 binds kREFRACTION_NORMALS alone as RT0 with depth LESS_EQUAL and no writes.
		 * No culling, because the pixel stage discards one-sided back faces itself.
		 */
		static org::services::PipelinePayload BuildRefraction(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, const ShaderPrograms::ShadowProgram* a_program,
			DXGI_FORMAT a_format, DXGI_FORMAT a_depthFormat, std::uint8_t a_writeMask, bool a_frontCCW);

		/** @brief A forward pipeline. A second target for first-person motion vectors is written unblended, since those members are opaque. */
		static org::services::PipelinePayload BuildForward(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const ShaderPrograms::ForwardProgram* a_program,
			ForwardTargets a_targets, rhi::CullMode a_cull, bool a_frontCCW);

		/**
		 * @brief A forward or decal key's pipelines. An attachment the pixel stage does not output keeps its contents, as DXVK builds the
		 * engine's pipeline. Its Z key is never shared.
		 */
		static org::services::PipelinePayload BuildForwardOrDecal(const ProgramFamilySlot& a_slot, std::string_view a_keyName, rhi::Device a_device,
			rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key, const ShaderPrograms::Program* a_program, TargetFormats a_targets,
			EngineState a_state, bool a_frontCCW);
		/** @brief A forward key's plain colour pipeline. If the pulled stages will not build it stays null and the key's other pipelines stand. */
		static void BuildPulledColour(rhi::Device a_device, rhi::PipelineLayoutHandle a_zLayout, const std::vector<std::byte>& a_vertex, const std::vector<std::byte>& a_pixel,
			const PipelineDesc& a_colour, std::string_view a_name, Built& a_built, rhi::PipelinePtr Built::* a_into = &Built::pulledColour);

		static org::services::PipelinePayload Build(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, rhi::PipelineLayoutHandle a_zLayout, PipelineKey a_key,
			const ShaderPrograms::Program* a_program, TargetFormats a_targets, EngineState a_state, bool a_frontCCW);

		/** @brief One shadow pipeline, depth only with LESS and writes. A two-sided caster draws without culling, as the engine's Utility pass does. */
		static org::services::PipelinePayload BuildShadow(rhi::Device a_device, rhi::PipelineLayoutHandle a_layout, ShadowPipelineKey a_key,
			const ShaderPrograms::ShadowProgram* a_program, DXGI_FORMAT a_depthFormat, ShadowRasterState a_state);

		bool CreateSignature(SetVersion::ClassSets& a_class, std::uint32_t a_variant);
		/** @brief a_index's place in its state class, making the class when it is the first. */
		void AdmitToClass(const Built& a_built, std::uint32_t a_index);
		/** @brief Brings every class's sets of a_version up to its members below a_admitted. */
		bool ApplyClasses(SetVersion& a_version, std::uint32_t a_admitted);
		/**
		 * @brief CS_DCLF_PARITY. Checks every class of a just-published version against its members.
		 * A member whose state differs from its class's first pipeline would draw with the wrong fixed-function state.
		 */
		void CheckClasses(const SetVersion& a_version);
		/** @brief CheckClasses for one class. */
		template <class Check>
		static void CheckClassMembers(const SetVersion& a_version, std::uint32_t a_class, const std::vector<std::uint32_t>& a_members,
			std::vector<std::uint8_t>& a_seen, Check&& a_check);

		/** @brief Writes [a_applied, a_admitted) into a_set, creating it and its signatures if absent. */
		template <class GetPipeline, class CreateSignatures>
		bool ApplyToSet(rhi::IndirectPipelineSetPtr& a_set, std::uint32_t a_applied, std::uint32_t a_admitted, std::uint32_t a_capacity, const char* a_name, GetPipeline&& a_pipeline,
			CreateSignatures&& a_createSignatures)
		{
			std::uint32_t from = a_applied;
			if (!a_set) {
				if (device.CreateIndirectPipelineSet(rhi::IndirectPipelineSetDesc{ a_pipeline(0), a_capacity }, a_set) != rhi::Result::Ok)
					return false;
				a_set->SetName(a_name);
				if (!a_createSignatures())
					return false;
				from = 1;
			}
			if (from >= a_admitted)
				return true;
			std::vector<rhi::PipelineHandle> pipelines;
			pipelines.reserve(a_admitted - from);
			for (std::uint32_t i = from; i < a_admitted; ++i)
				pipelines.push_back(a_pipeline(i));
			return device.UpdateIndirectPipelineSet(a_set->GetHandle(), from, { pipelines.data(), static_cast<std::uint32_t>(pipelines.size()) }) ==
			       rhi::Result::Ok;
		}
	};
}
