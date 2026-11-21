// Mirrors each BGSDecalNode's decal array. Edits push a node update on the editing thread. The capture
// re-records the array on the capturing thread, inside the engine-read window.

#include "Features/DrawcallLimitFix/Features/Decals/Decals.h"

#include "Features/DrawcallLimitFix/Core/Engine/Capture/SceneWriter.h"
#include "Features/DrawcallLimitFix/Core/Extensions/NodeCaptures.h"
#include "RenderGraph/Fatal.h"

namespace DCLF::Features::Decals
{
	namespace
	{
		// Covers append (FUN_1401fdfb0) and the erase paths (FUN_1401fdc80, FUN_1401fdcb0, FUN_1401fdd50, FUN_1401fde40).
		// Runs after the call, so the node already holds its new decals.
		void OnDecalArrayEdit(const SWF_EventHeader* a_event, std::uintptr_t*, void*)
		{
			Extensions::PushNodeUpdate(SceneWriter::Arg<RE::NiAVObject*>(a_event, 0), Api::kNodeFieldCaptured);
		}

		// Array order. OnVisible culls these last to first instead of the node's children.
		void CaptureDecals(const void* a_node, std::vector<const void*>& a_out)
		{
			for (const auto& decal : static_cast<const RE::BGSDecalNode*>(a_node)->GetRuntimeData().decals)
				a_out.push_back(decal ? decal->Get3D() : nullptr);
		}
	}

	const void* DecalNodeRtti()
	{
		static const REL::Relocation<const RE::NiRTTI*> decalNode{ RE::BGSDecalNode::Ni_RTTI };
		return decalNode.get();
	}

	void InstallDecalHooks()
	{
		if (!SceneWriter::SubscribeCall(SWF_HOOK_DECAL_ARRAY_EDIT, &OnDecalArrayEdit, SWF_PHASE_POST))
			::RenderGraph::Fatal("Drawcall Limit Fix", "the scene writer framework refused the decal array edits after reporting them installed");
	}

	void RegisterDecalCapture(Api::InstallContext& a_context)
	{
		a_context.RegisterNodeCapture({ .name = "decal node", .rtti = &DecalNodeRtti, .capture = &CaptureDecals });
	}
}
