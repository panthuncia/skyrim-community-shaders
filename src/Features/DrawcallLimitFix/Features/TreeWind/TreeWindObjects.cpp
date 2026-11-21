// Tree wind's object contributions: technique 12 (TreeAnim), its record flag, the TreeParams and WindTimers
// SetupGeometry derives (used until TreeWindCS has an entry for the tree), and the engine globals that derivation reads.

// TreeWindScene.h first, because volk must precede every Vulkan header.
#include "Features/DrawcallLimitFix/Features/TreeWind/TreeWindScene.h"
#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"
#include "Features/DrawcallLimitFix/Core/Extensions/ObjectContributors.h"
#include "Features/DrawcallLimitFix/Core/Shading/FrameGlobals.h"

#include <algorithm>
#include <bit>

namespace DCLF::Features::TreeWind
{
	namespace
	{
		using Engine::Global;

		// The frame globals' floats: wind magnitude (ShadowSceneNode +0x304), fade band, timer scale.
		Api::FrameGlobalsSlot globalsSlot;  // install

		// WindTimers.y, the previous timer, is not compared, because the engine copies the current timer over it after every native draw.
		constexpr Api::ShadingVariable kBlockVariables[] = {
			{ .variable = kTreeParamsVariable, .firstWord = kBlockTreeParams, .floats = 4, .compared = 4 },
			{ .variable = kWindTimersVariable, .firstWord = kBlockWindTimers, .floats = 2, .compared = 1 },
		};
		enum WindGlobal : std::uint32_t
		{
			kWindMagnitude,
			kWindFadeStart,
			kWindFadeEnd,
			kWindTimerScale,
			kWindGlobals,
		};

		void CaptureWindGlobals(float* a_out)
		{
			if (const auto source = Global<std::uintptr_t>(Engine::Address::kShadowSceneNode))
				a_out[kWindMagnitude] = *reinterpret_cast<const float*>(source + 0x304);
			a_out[kWindFadeStart] = Global<float>(Address::kTreeWindFadeStart);
			a_out[kWindFadeEnd] = Global<float>(Address::kTreeWindFadeEnd);
			a_out[kWindTimerScale] = Global<float>(Address::kTreeWindTimerScale);
		}

		// BSFadeNode vtable slot 0x1f8/8: the downcast to BSTreeNode, null for anything that is not one.
		constexpr std::size_t kAsTreeNodeSlot = 0x1f8 / 8;

		const void* AsTreeNode(const RE::BSFadeNode* a_fadeNode)
		{
			// The engine's "no node" sentinel (0x14332a2a0 holds a pointer to it) counts as null.
			if (!a_fadeNode || a_fadeNode == Engine::EmptyFadeNode())
				return nullptr;
			const auto* vtable = *reinterpret_cast<const std::uintptr_t* const*>(a_fadeNode);
			using Fn = const void* (*)(const RE::BSFadeNode*);
			return reinterpret_cast<Fn>(vtable[kAsTreeNodeSlot])(a_fadeNode);
		}

		// The engine's own square root, reproduced bit for bit (engine notes: Func6, case 0xc).
		float FastSqrt(float a_value)
		{
			// The engine shifts as a SIGNED integer (`sar`): this matters for a negative pattern (a fern's uninitialised distance
			// field), where the engine's result clamps to the maximum amplitude.
			const auto bits = std::bit_cast<std::int32_t>(a_value);
			const float estimate = std::bit_cast<float>(static_cast<std::uint32_t>(0x5f3759df - (bits >> 1)));
			return (1.5f - a_value * 0.5f * estimate * estimate) * estimate * a_value;
		}

		float TreeNodeFloat(const void* a_node, std::size_t a_offset)
		{
			return *reinterpret_cast<const float*>(static_cast<const std::byte*>(a_node) + a_offset);
		}

		/**
		 * @brief One object's tree-animation constants as SetupGeometry computes them (Func6 at 1414dd040, case 0xc).
		 * Skips the previousWindTimer write, which the native draw still does. Doing it twice would advance trees twice a frame.
		 */
		void DeriveTreeAnim(const Api::ShadingDerivation& a_derivation)
		{
			float* const treeParams = a_derivation.block.data() + kBlockTreeParams;
			float* const windTimers = a_derivation.block.data() + kBlockWindTimers;
			const void* node = AsTreeNode(a_derivation.property.fadeNode);

			const auto& g = FrameGlobals::Current().features;
			const std::uint32_t at = globalsSlot.first;
			treeParams[0] = 0.0f;
			treeParams[1] = g[at + kWindMagnitude];

			// Amplitude falls off with distance. The engine takes the squared distance at +0x158 through the 0x5f3759df fast inverse
			// square root (one Newton step) times x, not sqrtf; std::sqrt differs for trees inside the fade band.
			const float distance = node ? FastSqrt(TreeNodeFloat(node, 0x158)) : 0.0f;
			const float maxAmplitude = node ? TreeNodeFloat(node, 0x15c) : 1.0f;
			const float fadeStart = g[at + kWindFadeStart];
			const float span = g[at + kWindFadeEnd] - fadeStart;
			float amplitude = (1.0f - (distance - fadeStart) / span) * maxAmplitude;
			amplitude = std::max(amplitude, 0.0f);
			amplitude = std::min(amplitude, maxAmplitude);
			treeParams[2] = amplitude;
			treeParams[3] = node ? TreeNodeFloat(node, 0x160) : 1.0f;

			const float scale = g[at + kWindTimerScale];
			windTimers[0] = node ? TreeNodeFloat(node, 0x164) * scale : 0.0f;
			windTimers[1] = node ? TreeNodeFloat(node, 0x168) * scale : 0.0f;
			// Diagnostics only, past kBlockWords (the row keeps xy): the amplitude's raw inputs, so a parity mismatch can tell node from distance.
			windTimers[2] = node ? TreeNodeFloat(node, 0x158) : -1.0f;
			windTimers[3] = node ? TreeNodeFloat(node, 0x15c) : -1.0f;
		}
	}

	void RegisterObjects(Api::InstallContext& a_context)
	{
		a_context.RegisterTechnique({ .technique = kTechniqueTreeAnim, .objectFlags = kObjectTreeAnim });
		a_context.RegisterShading({ .name = "tree-anim", .propertyFlags = static_cast<std::uint64_t>(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim),
			.derive = &DeriveTreeAnim, .objectFlags = kObjectTreeAnim, .blockWords = kBlockWords, .vsVariables = kBlockVariables });
		globalsSlot = a_context.RegisterFrameGlobals({ .name = "tree-wind", .floats = kWindGlobals, .capture = &CaptureWindGlobals });
	}
}
