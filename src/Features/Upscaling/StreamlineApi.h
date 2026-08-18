#pragma once

#include <Windows.h>
#include <filesystem>
#include <vulkan/vulkan.h>

// Streamline's SDK headers expose the dynamically resolved function types.
#define NV_WINDOWS
#pragma warning(push)
#pragma warning(disable: 4471 5103)
#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_device_wrappers.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_fsr.h>
#include <sl_fsr_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_xess.h>
#pragma warning(pop)

/** Owns the mapped Streamline interposer and every dynamically resolved SDK entry point. */
class StreamlineApi
{
public:
	bool Load(const std::filesystem::path& a_path);
	void Unload();
	[[nodiscard]] bool IsLoaded() const { return interposer != nullptr; }

	template <typename T>
	bool Resolve(T*& a_fn, const char* a_name)
	{
		a_fn = reinterpret_cast<T*>(GetProcAddress(interposer, a_name));
		return a_fn != nullptr;
	}

	HMODULE interposer = nullptr;
	PFun_slInit* slInit = nullptr;
	PFun_slIsFeatureSupported* slIsFeatureSupported = nullptr;
	PFun_slGetNewFrameToken* slGetNewFrameToken = nullptr;
	PFun_slSetTagForFrame* slSetTagForFrame = nullptr;
	PFun_slSetConstants* slSetConstants = nullptr;
	PFun_slEvaluateFeature* slEvaluateFeature = nullptr;
	PFun_slGetFeatureFunction* slGetFeatureFunction = nullptr;
	PFun_slSetFeatureLoaded* slSetFeatureLoaded = nullptr;
	PFun_slIsFeatureLoaded* slIsFeatureLoaded = nullptr;
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings = nullptr;
	PFun_slDLSSSetOptions* slDLSSSetOptions = nullptr;
	PFun_slReflexGetState* slReflexGetState = nullptr;
	PFun_slReflexSetOptions* slReflexSetOptions = nullptr;
	PFun_slReflexSleep* slReflexSleep = nullptr;
	sl::Result (*slReflexSetExternalPacing)(bool) = nullptr;
	PFun_slPCLSetMarker* slPCLSetMarker = nullptr;
	PFun_slDLSSGSetOptions* slDLSSGSetOptions = nullptr;
	PFun_slDLSSGGetState* slDLSSGGetState = nullptr;
	PFun_slFSRSetOptions* slFSRSetOptions = nullptr;
	PFun_slFSRFrameGenerationSetOptions* slFSRFrameGenerationSetOptions = nullptr;
	PFun_slFSRGetFrameGenState* slFSRGetFrameGenState = nullptr;
	PFun_slFSRFrameGenerationDiscardPreparedFrame* slFSRFrameGenerationDiscardPreparedFrame = nullptr;
	PFun_slFSRFrameGenerationOwnsSwapchain* slFSRFrameGenerationOwnsSwapchain = nullptr;
	PFun_slFSRFrameGenerationCompleteSwapchainTeardown* slFSRFrameGenerationCompleteSwapchainTeardown = nullptr;
	PFun_slXeSSSetOptions* slXeSSSetOptions = nullptr;
};
