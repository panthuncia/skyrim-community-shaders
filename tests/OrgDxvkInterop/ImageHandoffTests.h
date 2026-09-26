#pragma once
#include <d3d11.h>
#include "RenderGraph/DxvkOrgInterop.h"

namespace org::tests {
int ImageHandoff(ID3D11Device* device, ID3D11DeviceContext* context,
    const DxvkOrgInteropDeviceInfo& native, const DxvkOrgInteropResourceInterface& interop,
    PFN_dxvkEnqueueResourceHandoff enqueue);
}
