#include <rhi_interop_vulkan.h>
#include "ImageHandoffTests.h"
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <cstdio>

namespace org::tests {
using Microsoft::WRL::ComPtr;
#define CHECK_IMAGE(expr) do { if (!(expr)) { std::fprintf(stderr, "Image handoff failed at %d: %s\n", __LINE__, #expr); return 1; } } while (false)

int ImageHandoff(ID3D11Device* device, ID3D11DeviceContext* context,
    const DxvkOrgInteropDeviceInfo& native, const DxvkOrgInteropResourceInterface& interop,
    PFN_dxvkEnqueueResourceHandoff enqueue) {
    CHECK_IMAGE(enqueue);
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = 3;
    desc.ArraySize = 2;
    desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture, readback;
    CHECK_IMAGE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)));
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK_IMAGE(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &readback)));
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Format = desc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 1;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 1;
    srvDesc.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11ShaderResourceView> srv;
    CHECK_IMAGE(SUCCEEDED(device->CreateShaderResourceView(texture.Get(), &srvDesc, &srv)));
    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
    rtvDesc.Format = desc.Format;
    rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
    rtvDesc.Texture2DArray.MipSlice = 1;
    rtvDesc.Texture2DArray.FirstArraySlice = 1;
    rtvDesc.Texture2DArray.ArraySize = 1;
    ComPtr<ID3D11RenderTargetView> rtv;
    CHECK_IMAGE(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), &rtvDesc, &rtv)));
    DxvkOrgInteropRegistration image{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
    CHECK_IMAGE(SUCCEEDED(interop.registerResource(interop.context, srv.Get(), &image)));
    CHECK_IMAGE(image.resource.image.layout == VK_IMAGE_LAYOUT_GENERAL);

    D3D11_BUFFER_DESC bufferDesc{};
    bufferDesc.ByteWidth = 8 * 8 * sizeof(float);
    bufferDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    bufferDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bufferDesc.StructureByteStride = sizeof(float);
    ComPtr<ID3D11Buffer> buffer, bufferReadback;
    CHECK_IMAGE(SUCCEEDED(device->CreateBuffer(&bufferDesc, nullptr, &buffer)));
    bufferDesc.BindFlags = bufferDesc.MiscFlags = bufferDesc.StructureByteStride = 0;
    bufferDesc.Usage = D3D11_USAGE_STAGING;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK_IMAGE(SUCCEEDED(device->CreateBuffer(&bufferDesc, nullptr, &bufferReadback)));
    DxvkOrgInteropRegistration output{DXVK_ORG_RESOURCE_REGISTRATION_VERSION};
    CHECK_IMAGE(SUCCEEDED(interop.registerResource(interop.context, buffer.Get(), &output)));
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK_IMAGE(SUCCEEDED(device->CreateUnorderedAccessView(buffer.Get(), nullptr, &uav)));
    const char shader[] = "Texture2DArray<float> image : register(t0); RWStructuredBuffer<float> output : register(u0); "
        "[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) { output[id.y * 8 + id.x] = image.Load(int4(id.xy, 0, 0)); }";
    ComPtr<ID3DBlob> bytecode, errors;
    ComPtr<ID3D11ComputeShader> consumer;
    CHECK_IMAGE(SUCCEEDED(D3DCompile(shader, sizeof(shader) - 1, "ImageConsumer", nullptr, nullptr,
        "main", "cs_5_0", 0, 0, &bytecode, &errors)));
    CHECK_IMAGE(SUCCEEDED(device->CreateComputeShader(bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &consumer)));

    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = native.graphicsQueueFamily;
    CHECK_IMAGE(vkCreateCommandPool(native.device, &poolInfo, nullptr, &pool) == VK_SUCCESS);
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    CHECK_IMAGE(vkAllocateCommandBuffers(native.device, &allocation, &commands) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT;
    CHECK_IMAGE(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS);
    VkBufferImageCopy copy{};
    copy.bufferOffset = output.resource.buffer.offset;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 1, 1, 1};
    copy.imageExtent = {8, 8, 1};
    vkCmdCopyImageToBuffer(commands, image.resource.image.image, VK_IMAGE_LAYOUT_GENERAL,
        output.resource.buffer.buffer, 1, &copy);
    VkImageMemoryBarrier2 internal{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    internal.image = image.resource.image.image;
    internal.subresourceRange = image.resource.image.subresourceRange;
    internal.oldLayout = internal.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    internal.srcQueueFamilyIndex = internal.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    internal.srcStageMask = internal.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    // WAR needs execution ordering only; the epoch owns this internal barrier.
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &internal;
    vkCmdPipelineBarrier2(commands, &dependency);
    VkClearColorValue clear{};
    clear.float32[0] = 11;
    vkCmdClearColorImage(commands, image.resource.image.image, VK_IMAGE_LAYOUT_GENERAL,
        &clear, 1, &internal.subresourceRange);
    CHECK_IMAGE(vkEndCommandBuffer(commands) == VK_SUCCESS);

    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    command.commandBuffer = commands;
    VkSubmitInfo2 submits[2] = {{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}, {VK_STRUCTURE_TYPE_SUBMIT_INFO_2}};
    for (auto& submit : submits) {
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
    }
    DxvkOrgInteropBufferAccess bufferUse{output.leaseToken, 0, bufferDesc.ByteWidth,
        VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    DxvkOrgInteropImageAccess imageUses[] = {
        {image.leaseToken, internal.subresourceRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL},
        {image.leaseToken, internal.subresourceRange, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_GENERAL}};
    DxvkOrgInteropResourceHandoff handoff{};
    handoff.version = DXVK_ORG_RESOURCE_HANDOFF_VERSION;
    handoff.submission.version = DXVK_ORG_RESOURCE_INTERFACE_VERSION;
    handoff.submission.batch.version = DXVK_ORG_INTEROP_VERSION;
    handoff.submission.batch.submitCount = 1;
    handoff.submission.batch.submits = submits;
    DxvkOrgInteropEpochAccesses epochs[2]{};
    for (auto& epoch : epochs) {
        epoch.complete = VK_TRUE;
        epoch.bufferCount = 1;
        epoch.buffers = &bufferUse;
        epoch.imageCount = 2;
        epoch.images = imageUses;
    }
    handoff.epochCount = 1;
    handoff.epochs = epochs;
    // A view token must not admit another slice, and rejection changes no state.
    imageUses[0].range.baseArrayLayer = 0;
    CHECK_IMAGE(FAILED(enqueue(interop.context, &handoff)));
    imageUses[0].range.baseArrayLayer = 1;
    epochs[0].complete = VK_FALSE;
    CHECK_IMAGE(FAILED(enqueue(interop.context, &handoff)));
    epochs[0].complete = VK_TRUE;

    for (unsigned frame = 0; frame < 8; ++frame) {
        const float input[] = {float(frame + 3), 0, 0, 0};
        // Deliberately leave this clear deferred at the external boundary.
        context->ClearRenderTargetView(rtv.Get(), input);
        handoff.epochCount = handoff.submission.batch.submitCount = (frame & 1) ? 2 : 1;
        CHECK_IMAGE(SUCCEEDED(enqueue(interop.context, &handoff)));
        context->CopyResource(bufferReadback.Get(), buffer.Get());
        context->CopyResource(readback.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        CHECK_IMAGE(SUCCEEDED(context->Map(bufferReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        bool correct = true;
        const float expectedInput = (frame & 1) ? 11.0f : input[0];
        for (unsigned i = 0; i < 64; ++i) correct &= static_cast<const float*>(mapped.pData)[i] == expectedInput;
        context->Unmap(bufferReadback.Get(), 0);
        CHECK_IMAGE(correct);
        CHECK_IMAGE(SUCCEEDED(context->Map(readback.Get(), D3D11CalcSubresource(1, 1, 3), D3D11_MAP_READ, 0, &mapped)));
        for (unsigned y = 0; y < 8; ++y) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
            for (unsigned x = 0; x < 8; ++x) correct &= row[x] == 11;
        }
        context->Unmap(readback.Get(), D3D11CalcSubresource(1, 1, 3));
        CHECK_IMAGE(correct);
        ID3D11ShaderResourceView* source = srv.Get();
        ID3D11UnorderedAccessView* destination = uav.Get();
        context->CSSetShader(consumer.Get(), nullptr, 0);
        context->CSSetShaderResources(0, 1, &source);
        context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
        context->Dispatch(1, 1, 1);
        context->Dispatch(1, 1, 1); // repeat without dirtying descriptors
        source = nullptr;
        destination = nullptr;
        context->CSSetShaderResources(0, 1, &source);
        context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
        context->CopyResource(bufferReadback.Get(), buffer.Get());
        CHECK_IMAGE(SUCCEEDED(context->Map(bufferReadback.Get(), 0, D3D11_MAP_READ, 0, &mapped)));
        for (unsigned i = 0; i < 64; ++i) correct &= static_cast<const float*>(mapped.pData)[i] == 11;
        context->Unmap(bufferReadback.Get(), 0);
        CHECK_IMAGE(correct);
    }
    // The native readbacks above retire every use of this reusable command buffer.
    vkDestroyCommandPool(native.device, pool, nullptr);
    CHECK_IMAGE(SUCCEEDED(interop.unregisterResource(interop.context, image.leaseToken)));
    CHECK_IMAGE(SUCCEEDED(interop.unregisterResource(interop.context, output.leaseToken)));
    return 0;
}
#undef CHECK_IMAGE
}
