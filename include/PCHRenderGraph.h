#pragma once

// Added to the precompiled header after PCH.h when the render graph is built (cmake/BuildSpeed.cmake).
// The heavy, stable submodule headers that Drawcall Limit Fix and the render graph include almost
// everywhere: parsing them once per build instead of once per file is most of their compile time.
//
// Rules: only headers that change rarely (an edit here rebuilds every file), and none that include
// Vulkan's headers. volk must come first in a file that uses it and it hides Vulkan's exported
// functions, which a few files call (Features/Upscaling). So no rhi_interop_vulkan.h or volk.h here.

#include <rhi.h>
#include <rhi_helpers.h>

#include <OpenRenderGraph/GraphHost.h>
#include <Render/LatchBlock.h>
#include <Render/RenderGraph/RenderGraph.h>
#include <RenderPasses/Base/TypedRenderGraphPass.h>
#include <Resources/Buffers/Buffer.h>
#include <Resources/ExternalTextureResource.h>

#include <ORGModuleServices/GrowableBufferSet.h>
