#pragma once

// Address Library ids that only Fades uses. Addresses shared with Core or other features stay in Core/Engine/EngineAccess.h.

#include "Features/DrawcallLimitFix/Core/Engine/EngineAccess.h"

#include <cstddef>
#include <cstdint>

namespace DCLF::Features::Fades::Address
{
	inline constexpr DCLF::Engine::Offset kFadeStepDivisor{ { 390877 }, { 513153 } };  // float: the fade-in time
	inline constexpr DCLF::Engine::Offset kFadeStepMax{ { 390885 }, { 513161 } };  // float: the largest fade step
	inline constexpr DCLF::Engine::Offset kFadeFrameCounter{ { 390886 }, { 513162 } };  // std::int32_t: the fade code's frame counter
	inline constexpr DCLF::Engine::Offset kFadeStepTime{ { 390954 }, { 513214 } };  // float: the frame time the fades step by
	inline constexpr DCLF::Engine::Offset kFloatOne{ { 186482 }, { 228662 } };  // float 1.0
	inline constexpr DCLF::Engine::Offset kFadeOnVisible{ { 105485 }, { 98848 } };  // BSFadeNode::OnVisible
	inline constexpr DCLF::Engine::Offset kLeafOnVisible{ { 105588 }, { 98937 } };  // BSLeafAnimNode::OnVisible, which steps the leaf LOD and then calls BSFadeNode's
	inline constexpr DCLF::Engine::Offset kFadeDistance{ { 105506 }, { 98862 } };  // FUN_14147b110(node, camera, out), returns the distance and LOD level
	// BSFadeNode's fade and LOD globals.
	inline constexpr DCLF::Engine::Offset kFadeOutTime{ { 390878 }, { 513154 } };
	inline constexpr DCLF::Engine::Offset kFadeInAbove{ { 390879 }, { 513155 } };
	inline constexpr DCLF::Engine::Offset kFadeBlendTime{ { 390881 }, { 513157 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountTime{ { 414542 }, { 527615 } };
	// AE reads four separate threshold floats. SE's FUN_141290590 indexes two float arrays by node kind, 1 for a fade node and 4 for a tree.
	inline constexpr std::uint64_t kSeLodFadeFarArray = 527618, kSeLodFadeNearArray = 527624;  // 0x1431d0d30, 0x1431d0d60
	inline constexpr std::uint32_t kSeLodKindFade = 1, kSeLodKindTree = 4;
	inline constexpr DCLF::Engine::Offset kLodFadeFar{ { 414546 }, { kSeLodFadeFarArray, 4 * kSeLodKindFade } };
	inline constexpr DCLF::Engine::Offset kLodFadeNear{ { 414553 }, { kSeLodFadeNearArray, 4 * kSeLodKindFade } };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeFar{ { 414547 }, { kSeLodFadeFarArray, 4 * kSeLodKindTree } };
	inline constexpr DCLF::Engine::Offset kTreeLodFadeNear{ { 414554 }, { kSeLodFadeNearArray, 4 * kSeLodKindTree } };
	inline constexpr DCLF::Engine::Offset kLodFadeMinimum{ { 415263 }, { 528314 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapAbove{ { 196368 }, { 242717 } };
	inline constexpr DCLF::Engine::Offset kFadeAmountSnapOffset{ { 186632 }, { 228898 } };
	inline constexpr DCLF::Engine::Offset kFadeSnapRadiusLimit{ { 470092 }, { 230694 } };
	// The height test in BSTreeNode::OnVisible (AE 0x14147d3c0). The switch byte is a field on the culling process, the rest are globals.
	inline constexpr std::size_t kProcessTreeHeightTest = 0x121;  // byte on the culling process
	inline constexpr DCLF::Engine::Offset kTreeHeightTestOn{ { 390930 }, { 513192 } };  // byte
	inline constexpr DCLF::Engine::Offset kTreeHeightBase{ { 414568 }, { 527639 } };  // float
	inline constexpr DCLF::Engine::Offset kTreeHeightLimit{ { 390927 }, { 513190 } };  // float
}
