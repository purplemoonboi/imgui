// dear imgui: Renderer Backend for NVRHI

#pragma once
#include "imgui.h" // IMGUI_IMPL_API
#ifndef IMGUI_DISABLE

#include <nvrhi/nvrhi.h>
#include "Foundation/Renderer/DescriptorManager.h"

using namespace Foundation;

// Initialization data, for ImGui_ImplNVRHI_Init()
struct ImGui_ImplNVRHI_InitInfo
{
    nvrhi::DeviceHandle Device;
    int NumFramesInFlight;
    nvrhi::Format RTFormat;
    nvrhi::Format DSFormat;
    void* UserData;

    ImGui_ImplNVRHI_InitInfo() {}
};

// Follow "Getting Started" link and check examples/ folder to learn about using backends!
IMGUI_IMPL_API bool ImGui_ImplNVRHI_Init(ImGui_ImplNVRHI_InitInfo* info);
IMGUI_IMPL_API void ImGui_ImplNVRHI_Shutdown();
IMGUI_IMPL_API void ImGui_ImplNVRHI_NewFrame();
IMGUI_IMPL_API void ImGui_ImplNVRHI_RenderDrawData(ImDrawData* draw_data, nvrhi::CommandListHandle commandList, nvrhi::FramebufferHandle framebuffer);

// Use if you want to reset your rendering device without losing Dear ImGui state.
IMGUI_IMPL_API bool ImGui_ImplNVRHI_CreateDeviceObjects();
IMGUI_IMPL_API void ImGui_ImplNVRHI_InvalidateDeviceObjects();

// (Advanced) Use e.g. if you need to precisely control the timing of texture updates (e.g. for staged rendering), by setting ImDrawData::Textures = NULL to handle this manually.
IMGUI_IMPL_API void ImGui_ImplNVRHI_UpdateTexture(ImTextureData* tex);

// [BETA] Selected render state data shared with callbacks.
// This is temporarily stored in GetPlatformIO().Renderer_RenderState during the ImGui_ImplDX12_RenderDrawData() call.
// (Please open an issue if you feel you need access to more data)
struct ImGui_ImplNVRHI_RenderState
{
    nvrhi::DeviceHandle Device;
    nvrhi::CommandListHandle CommandList;
};

#endif // #ifndef IMGUI_DISABLE
