// dear imgui: Renderer Backend for NVRHI
#include "imgui.h"
#ifndef IMGUI_DISABLE
#include "imgui_impl_nvrhi.h"

#include "Foundation/Core/Window.h"
#include "Foundation/Core/Application/Application.h"
#include "Foundation/Renderer/GraphicsCore.h"
#include "Foundation/Renderer/GpuBuffer.h"
#include "Foundation/Renderer/SwapChain.h"

// Clang/GCC warnings with -Weverything
#if defined(__clang__)
#pragma clang diagnostic ignored "-Wold-style-cast"  // warning: use of old-style cast                            // yes, they are more terse.
#pragma clang diagnostic ignored "-Wsign-conversion" // warning: implicit conversion changes signedness
#endif

#endif // #ifndef IMGUI_DISABLE

struct ImGui_ImplNVRHI_RenderBuffers;

struct ImGui_ImplNVRHI_Texture
{
    nvrhi::TextureHandle Texture;

    // DX12 had a descriptor handle here: we could?
    Foundation::Graphics::DescriptorHandle Handle;

    ImGui_ImplNVRHI_Texture() {}
};

struct ImGui_ImplNVRHI_Data
{
    ImGui_ImplNVRHI_InitInfo InitInfo;
    ImGui_ImplNVRHI_RenderBuffers* pFrameResources;

    nvrhi::DeviceHandle Device;
    nvrhi::CommandListHandle CommandList;
    nvrhi::GraphicsPipelineHandle Pipeline;
    nvrhi::Format RTFormat;
    nvrhi::Format DSFormat;
    nvrhi::BindingLayoutHandle BindingLayout;
    nvrhi::SamplerHandle FontSampler;

    std::unordered_map<nvrhi::ITexture*, nvrhi::BindingSetHandle> ResourceCache = {};

    uint64_t FenceLastSignaledValue;
    uint32_t NumFramesInFlight;

    bool TearingSupport;
    bool LegacySingleDescriptorUsed;

    UINT frameIndex;

    ImGui_ImplNVRHI_Data() {}

    nvrhi::BindingSetHandle GetBindingSet(nvrhi::ITexture* texture)
    {
        if (ResourceCache.contains(texture))
        {
            return ResourceCache.at(texture);
        }

        nvrhi::BindingSetDesc desc;

        desc.addItem(nvrhi::BindingSetItem::PushConstants(0, 2 * sizeof(float)));
        desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, texture));
        desc.addItem(nvrhi::BindingSetItem::Sampler(0, FontSampler));

        auto set = Device->createBindingSet(desc, BindingLayout);

        ResourceCache[texture] = set;

        return set;
    }
};

// Backend data stored in io.BackendRendererUserData to allow support for multiple Dear ImGui contexts
// It is STRONGLY preferred that you use docking branch with multi-viewports (== single Dear ImGui context + multiple windows) instead of multiple Dear ImGui contexts.
static ImGui_ImplNVRHI_Data* ImGui_ImplNVRHI_GetBackendData()
{
    return ImGui::GetCurrentContext() ? (ImGui_ImplNVRHI_Data*)ImGui::GetIO().BackendRendererUserData : nullptr;
}

// Buffers used during the rendering of a frame
struct ImGui_ImplNVRHI_RenderBuffers
{
    Graphics::IndexBuffer16 IndexBuffer;
    Graphics::VertexBuffer<ImDrawVert> VertexBuffer;
    int IndexBufferSize;
    int VertexBufferSize;
};

// Buffers used for secondary viewports created by the multi-viewports systems
struct ImGui_ImplNVRHI_FrameContext
{
    uint64_t FenceValue;
    Graphics::FrameBuffer RenderTarget;
};

// Helper structure we store in the void* RendererUserData field of each ImGuiViewport to easily retrieve our backend data.
// Main viewport created by application will only use the Resources field.
// Secondary viewports created by this backend will use all the fields (including Window fields),
struct ImGui_ImplNVRHI_ViewportData
{
    // Window
    // ID3D12CommandQueue* CommandQueue;
    nvrhi::CommandListHandle CommandList;

    Graphics::SwapChain* SwapChain;

    UINT NumFramesInFlight;
    ImGui_ImplNVRHI_FrameContext* FrameCtx;

    // Render buffers
    UINT FrameIndex;
    ImGui_ImplNVRHI_RenderBuffers* FrameRenderBuffers;

    ImGui_ImplNVRHI_ViewportData(UINT num_frames_in_flight)
    {
        CommandList = nullptr;
        SwapChain = nullptr;
        NumFramesInFlight = num_frames_in_flight;
        FrameCtx = new ImGui_ImplNVRHI_FrameContext[NumFramesInFlight];
        FrameIndex = 0;
        FrameRenderBuffers = new ImGui_ImplNVRHI_RenderBuffers[NumFramesInFlight];

        for (UINT i = 0; i < NumFramesInFlight; ++i)
        {
            FrameCtx[i].FenceValue = 0;
            FrameCtx[i].RenderTarget = Foundation::Graphics::FrameBuffer(); // Is empty by default

            // Create buffers with a default size (they will later be grown as needed)
            FrameRenderBuffers[i].VertexBufferSize = 5000;
            FrameRenderBuffers[i].IndexBufferSize = 10000;
        }
    }

    ~ImGui_ImplNVRHI_ViewportData()
    {
        IM_ASSERT(CommandList == nullptr);
        IM_ASSERT(SwapChain == nullptr);

        for (UINT i = 0; i < NumFramesInFlight; ++i)
        {
            IM_ASSERT(FrameCtx[i].RenderTarget.GetFramebufferTexture() == nullptr);
            IM_ASSERT(FrameRenderBuffers[i].IndexBuffer.GetBufferHandle() == nullptr && FrameRenderBuffers[i].VertexBuffer.GetBufferHandle() == nullptr);
        }

        delete[] FrameCtx;
        FrameCtx = nullptr;
        delete[] FrameRenderBuffers;
        FrameRenderBuffers = nullptr;
    }
};

struct VERTEX_CONSTANT_BUFFER_NVRHI
{
    float mvp[4][4];
};

// Forward Declarations
static void ImGui_ImplNVRHI_InitMultiViewportSupport();
static void ImGui_ImplNVRHI_ShutdownMultiViewportSupport();

// Functions
static void ImGui_ImplNVRHI_SetupRenderState(ImDrawData* draw_data, nvrhi::CommandListHandle command_list, ImGui_ImplNVRHI_RenderBuffers* fr, nvrhi::GraphicsState& graphicsState)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();

    // Setup viewport
    nvrhi::ViewportState vpState = nvrhi::ViewportState();
    nvrhi::Viewport vp = nvrhi::Viewport();

    vp.maxX = draw_data->DisplaySize.x * draw_data->FramebufferScale.x;
    vp.maxY = draw_data->DisplaySize.y * draw_data->FramebufferScale.y;
    vp.minX = vp.minY = 0.0f;

    vpState.addViewport(vp);

    graphicsState.setViewport(vpState);

    // Bind shader and vertex buffers
    unsigned int stride = sizeof(ImDrawVert);
    unsigned int offset = 0;

    nvrhi::VertexBufferBinding vbBinding = nvrhi::VertexBufferBinding();
    vbBinding.setSlot(0);
    vbBinding.setOffset(0);
    vbBinding.setBuffer(fr->VertexBuffer.GetBufferHandle());

    nvrhi::IndexBufferBinding ibBinding = nvrhi::IndexBufferBinding();
    ibBinding.setOffset(0);
    ibBinding.setFormat(sizeof(ImDrawIdx) == 2 ? nvrhi::Format::R16_UINT : nvrhi::Format::R32_UINT);
    ibBinding.setBuffer(fr->IndexBuffer.GetBufferHandle());

    graphicsState.addVertexBuffer(vbBinding);
    graphicsState.setIndexBuffer(ibBinding);

    graphicsState.setPipeline(bd->Pipeline);

    // Setup blend factor
    const float blend_factor[4] = {0.f, 0.f, 0.f, 0.f};
    nvrhi::Color blend_col = nvrhi::Color(blend_factor[0], blend_factor[1], blend_factor[2], blend_factor[3]);
    graphicsState.setBlendColor(blend_col);
}

static inline void SafeRelease(nvrhi::BufferHandle& handle)
{
    if (handle)
    {
        handle->Release();
    }
    handle = nullptr;
}

static inline void SafeRelease(nvrhi::TextureHandle& handle)
{
    if (handle)
    {
        handle->Release();
    }
    handle = nullptr;
}

// Render function
void ImGui_ImplNVRHI_RenderDrawData(ImDrawData* draw_data, nvrhi::CommandListHandle commandList, nvrhi::FramebufferHandle framebuffer)
{
    // Avoid rendering when minimized
    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)
    {
        return;
    }

    // Catch up with texture updates. Most of the times, the list will have 1 element with an OK status, aka nothing to do.
    // (This almost always points to ImGui::GetPlatformIO().Textures[] but is part of ImDrawData to allow overriding or disabling texture updates).
    if (draw_data->Textures != nullptr)
    {
        for (ImTextureData* tex : *draw_data->Textures)
        {
            if (tex->Status != ImTextureStatus_OK)
            {
                ImGui_ImplNVRHI_UpdateTexture(tex);
            }
        }
    }

    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)draw_data->OwnerViewport->RendererUserData;
    vd->FrameIndex++;

    ImGui_ImplNVRHI_RenderBuffers* fr = &vd->FrameRenderBuffers[vd->FrameIndex % bd->NumFramesInFlight];

    // Create and grow vertex/index buffers if needed
    if (fr->VertexBuffer.GetBufferHandle() == nullptr || fr->VertexBufferSize < draw_data->TotalVtxCount)
    {
        SafeRelease(fr->VertexBuffer.GetBufferHandle());

        fr->VertexBufferSize = draw_data->TotalVtxCount + 5000;

        fr->VertexBuffer = Foundation::Graphics::VertexBuffer<ImDrawVert>("ImGui - Frame Resource VB", fr->VertexBufferSize / sizeof(ImDrawVert), true);

        if (!fr->VertexBuffer.GetBufferHandle())
        {
            Foundation::FCORE_ERROR("Failed to create a new ImGui frame resource vertex buffer.");
            return;
        }
    }

    if (fr->IndexBuffer.GetBufferHandle() == nullptr || fr->IndexBufferSize < draw_data->TotalIdxCount)
    {
        SafeRelease(fr->IndexBuffer.GetBufferHandle());

        fr->IndexBufferSize = draw_data->TotalIdxCount + 10000;

        fr->IndexBuffer = Foundation::Graphics::IndexBuffer16("ImGui - Frame Resource IB", static_cast<uint32_t>(fr->IndexBufferSize) / sizeof(ImDrawIdx), true);

        if (!fr->IndexBuffer.GetBufferHandle())
        {
            Foundation::FCORE_ERROR("Failed to create a new ImGui frame resource index buffer.");
            return;
        }
    }

    // Upload vertex/index data into a single contiguous GPU buffer
    void *vtx_resource, *idx_resource;

    uint64_t vbOffset = 0u;
    uint64_t ibOffset = 0u;

    vtx_resource = bd->Device->mapBuffer(fr->VertexBuffer.GetBufferHandle(), nvrhi::CpuAccessMode::Write);
    idx_resource = bd->Device->mapBuffer(fr->IndexBuffer.GetBufferHandle(), nvrhi::CpuAccessMode::Write);

    ImDrawVert* vtx_dst = (ImDrawVert*)vtx_resource;
    ImDrawIdx* idx_dst = (ImDrawIdx*)idx_resource;

    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        memcpy(vtx_dst, draw_list->VtxBuffer.Data, draw_list->VtxBuffer.Size * sizeof(ImDrawVert));
        memcpy(idx_dst, draw_list->IdxBuffer.Data, draw_list->IdxBuffer.Size * sizeof(ImDrawIdx));
        vtx_dst += draw_list->VtxBuffer.Size;
        idx_dst += draw_list->IdxBuffer.Size;
    }

    bd->Device->unmapBuffer(fr->VertexBuffer.GetBufferHandle());
    bd->Device->unmapBuffer(fr->IndexBuffer.GetBufferHandle());

    nvrhi::GraphicsState graphicsState;

    graphicsState.setFramebuffer(framebuffer);

    // Setup desired DX state
    ImGui_ImplNVRHI_SetupRenderState(draw_data, commandList, fr, graphicsState);

    graphicsState.viewport.scissorRects.resize(1);

    // Setup render state structure (for callbacks and custom texture bindings)
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    ImGui_ImplNVRHI_RenderState render_state;
    render_state.Device = bd->Device;
    render_state.CommandList = commandList;
    platform_io.Renderer_RenderState = &render_state;

    // Setup orthographic projection matrix into our constant buffer
    // Our visible imgui space lies from draw_data->DisplayPos (top left) to draw_data->DisplayPos+data_data->DisplaySize (bottom right).
    VERTEX_CONSTANT_BUFFER_NVRHI vertex_constant_buffer;
    {
        float L = draw_data->DisplayPos.x;
        float R = draw_data->DisplayPos.x + draw_data->DisplaySize.x;
        float T = draw_data->DisplayPos.y;
        float B = draw_data->DisplayPos.y + draw_data->DisplaySize.y;
        float mvp[4][4] = {
            {2.0f / (R - L), 0.0f, 0.0f, 0.0f},
            {0.0f, 2.0f / (T - B), 0.0f, 0.0f},
            {0.0f, 0.0f, 0.5f, 0.0f},
            {(R + L) / (L - R), (T + B) / (B - T), 0.5f, 1.0f},
        };
        memcpy(&vertex_constant_buffer.mvp, mvp, sizeof(mvp));
    }

    // Render command lists
    // (Because we merged all buffers into a single one, we maintain our own offset into them)
    int global_vtx_offset = 0;
    int global_idx_offset = 0;
    ImVec2 clip_off = draw_data->DisplayPos;
    ImVec2 clip_scale = draw_data->FramebufferScale;
    for (const ImDrawList* draw_list : draw_data->CmdLists)
    {
        for (int cmd_i = 0; cmd_i < draw_list->CmdBuffer.Size; cmd_i++)
        {
            const ImDrawCmd* pcmd = &draw_list->CmdBuffer[cmd_i];
            if (pcmd->UserCallback != nullptr)
            {
                // User callback, registered via ImDrawList::AddCallback()
                // (ImDrawCallback_ResetRenderState is a special callback value used by the user to request the renderer to reset render state.)
                if (pcmd->UserCallback == ImDrawCallback_ResetRenderState)
                {
                    ImGui_ImplNVRHI_SetupRenderState(draw_data, commandList, fr, graphicsState);
                }
                else
                {
                    pcmd->UserCallback(draw_list, pcmd);
                }
            }
            else
            {
                // Project scissor/clipping rectangles into framebuffer space
                ImVec2 clip_min((pcmd->ClipRect.x - clip_off.x) * clip_scale.x, (pcmd->ClipRect.y - clip_off.y) * clip_scale.y);
                ImVec2 clip_max((pcmd->ClipRect.z - clip_off.x) * clip_scale.x, (pcmd->ClipRect.w - clip_off.y) * clip_scale.y);

                if (clip_max.x <= clip_min.x || clip_max.y <= clip_min.y)
                {
                    continue;
                }

                graphicsState.viewport.scissorRects[0] = nvrhi::Rect((LONG)clip_min.x, (LONG)clip_max.x, (LONG)clip_min.y, (LONG)clip_max.y);

                graphicsState.bindings = {bd->GetBindingSet((nvrhi::ITexture*)pcmd->GetTexID())};

                commandList->setGraphicsState(graphicsState);

                commandList->setPushConstants(&vertex_constant_buffer, sizeof(vertex_constant_buffer));

                nvrhi::DrawArguments drawArgs = nvrhi::DrawArguments();
                drawArgs.setVertexCount(pcmd->ElemCount); // In NVRHI, vert count acts as index count when we invoke draw instanced
                drawArgs.setStartIndexLocation(pcmd->IdxOffset + global_idx_offset);
                drawArgs.setStartVertexLocation(pcmd->VtxOffset + global_vtx_offset);
                drawArgs.setInstanceCount(1);
                drawArgs.setStartInstanceLocation(0);

                commandList->drawIndexed(drawArgs);
            }
        }
        global_idx_offset += draw_list->IdxBuffer.Size;
        global_vtx_offset += draw_list->VtxBuffer.Size;
    }
    platform_io.Renderer_RenderState = nullptr;
}

static void ImGui_ImplNVRHI_DestroyTexture(ImTextureData* tex)
{
    if (ImGui_ImplNVRHI_Texture* backend_tex = (ImGui_ImplNVRHI_Texture*)tex->BackendUserData)
    {
        ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
        SafeRelease(backend_tex->Texture);
        IM_DELETE(backend_tex);

        // Clear identifiers and mark as destroyed (in order to allow e.g. calling InvalidateDeviceObjects while running)
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
    }
    tex->SetStatus(ImTextureStatus_Destroyed);
}

void ImGui_ImplNVRHI_UpdateTexture(ImTextureData* tex)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();

    if (tex->Status == ImTextureStatus_WantCreate)
    {
        // Create and upload new texture to graphics system
        IM_ASSERT(tex->TexID == ImTextureID_Invalid && tex->BackendUserData == nullptr);
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);
        ImGui_ImplNVRHI_Texture* backend_tex = IM_NEW(ImGui_ImplNVRHI_Texture)();
        SafeRelease(backend_tex->Texture);

        nvrhi::TextureDesc desc = nvrhi::TextureDesc();
        desc.setDebugName("ImGui Texture");
        desc.setWidth(tex->Width);
        desc.setHeight(tex->Height);
        desc.setFormat(nvrhi::Format::RGBA8_UNORM);
        desc.enableAutomaticStateTracking(nvrhi::ResourceStates::ShaderResource);
        backend_tex->Texture = bd->Device->createTexture(desc);

        // Store identifiers
        tex->SetTexID((ImTextureID)backend_tex->Texture.Get());
        tex->BackendUserData = backend_tex;
    }

    if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates)
    {
        ImGui_ImplNVRHI_Texture* backend_tex = (ImGui_ImplNVRHI_Texture*)tex->BackendUserData;
        IM_ASSERT(tex->Format == ImTextureFormat_RGBA32);

        const int upload_x = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.x;
        const int upload_y = (tex->Status == ImTextureStatus_WantCreate) ? 0 : tex->UpdateRect.y;
        const int upload_w = (tex->Status == ImTextureStatus_WantCreate) ? tex->Width : tex->UpdateRect.w;
        const int upload_h = (tex->Status == ImTextureStatus_WantCreate) ? tex->Height : tex->UpdateRect.h;

        auto D3D12_DATA_PITCH_ALIGNMENT = 256;
        UINT upload_pitch_src = upload_w * tex->BytesPerPixel;

        // Create a staging texture
        nvrhi::TextureDesc stageTexDesc = nvrhi::TextureDesc();
        stageTexDesc.setFormat(nvrhi::Format::RGBA8_UNORM);
        stageTexDesc.setWidth(upload_w);
        stageTexDesc.setHeight(upload_h);
        stageTexDesc.setDepth(1);
        stageTexDesc.setSampleCount(1);
        stageTexDesc.setSampleQuality(0);
        stageTexDesc.setMipLevels(1);
        stageTexDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::CopySource);

        nvrhi::StagingTextureHandle stagingTexHandle = bd->Device->createStagingTexture(stageTexDesc, nvrhi::CpuAccessMode::Write);

        // Resolve source tile
        nvrhi::TextureSlice srcTile = nvrhi::TextureSlice().resolve(stageTexDesc);

        // Map CPU memory to the GPU buffer
        size_t upload_pitch_dst = 0;
        void* mapped = bd->Device->mapStagingTexture(stagingTexHandle, srcTile, nvrhi::CpuAccessMode::Write, &upload_pitch_dst);

        // Copy to staging buffer
        for (int y = 0; y < upload_h; y++)
        {
            memcpy((void*)((uintptr_t)mapped + y * upload_pitch_dst), tex->GetPixelsAt(upload_x, upload_y + y), upload_pitch_src);
        }

        bd->Device->unmapStagingTexture(stagingTexHandle);

        nvrhi::TextureSlice dstTile;
        dstTile.x = upload_x;
        dstTile.y = upload_y;
        dstTile.width = upload_w;
        dstTile.height = upload_h;

        // Note: we're using the backend command list here

        // Open command list
        bd->CommandList->open();

        // Manually track resource states.
        // bd->CommandList->beginTrackingTextureState(backend_tex->Texture, nvrhi::TextureSubresourceSet(), nvrhi::ResourceStates::CopyDest);

        // Copy staging texture into destination tile.
        bd->CommandList->copyTexture(backend_tex->Texture, dstTile, stagingTexHandle, srcTile);

        // bd->CommandList->beginTrackingTextureState(backend_tex->Texture, nvrhi::TextureSubresourceSet(), nvrhi::ResourceStates::ShaderResource);

        // Close command list
        bd->CommandList->close();

        // Execute command list and wait for the graphics queue to finish processing
        // texture upload.
        uint64_t instance = bd->Device->executeCommandList(bd->CommandList);

        bd->Device->queueWaitForCommandList(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics, instance);

        tex->SetStatus(ImTextureStatus_OK);
    }

    if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames >= (int)bd->NumFramesInFlight)
    {
        ImGui_ImplNVRHI_DestroyTexture(tex);
    }
}

bool ImGui_ImplNVRHI_CreateDeviceObjects()
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    if (!bd || !bd->Device)
    {
        return false;
    }

    if (bd->Pipeline)
    {
        ImGui_ImplNVRHI_InvalidateDeviceObjects();
    }

    // Create and define the graphics pipeline
    {
        nvrhi::GraphicsPipelineDesc pipelineDesc = nvrhi::GraphicsPipelineDesc();
        pipelineDesc.setPrimType(nvrhi::PrimitiveType::TriangleList);

        // load imgui shaders
        {
            auto shaderManager = Application::GetInstance()->GetRenderer()->GetShaderManager();

            nvrhi::ShaderHandle vs = shaderManager.LoadAndAddPreCompiled("imgui_vs.cso");
            nvrhi::ShaderHandle ps = shaderManager.LoadAndAddPreCompiled("imgui_ps.cso");

            pipelineDesc.setVertexShader(vs);
            pipelineDesc.setPixelShader(ps);

            // create input layout
            {
                struct ImGuiVertex
                {
                    float mPos[2];
                    float mCol[4];
                    float mUV[2];
                };

                nvrhi::VertexAttributeDesc inputLayoutDesc[3];
                inputLayoutDesc[0].setName("POSITION");
                inputLayoutDesc[0].setFormat(nvrhi::Format::RG32_FLOAT);
                inputLayoutDesc[0].setOffset(offsetof(ImGuiVertex, mPos));
                inputLayoutDesc[0].setElementStride(sizeof(ImGuiVertex));

                inputLayoutDesc[1].setName("COLOR");
                inputLayoutDesc[1].setFormat(nvrhi::Format::RGBA32_FLOAT);
                inputLayoutDesc[1].setOffset(offsetof(ImGuiVertex, mCol));
                inputLayoutDesc[1].setElementStride(sizeof(ImGuiVertex));

                inputLayoutDesc[2].setName("TEXCOORD");
                inputLayoutDesc[2].setFormat(nvrhi::Format::RG32_FLOAT);
                inputLayoutDesc[2].setOffset(offsetof(ImGuiVertex, mUV));
                inputLayoutDesc[2].setElementStride(sizeof(ImGuiVertex));

                nvrhi::InputLayoutHandle layoutHandle = bd->Device->createInputLayout(inputLayoutDesc, 3, vs.Get());

                pipelineDesc.setInputLayout(layoutHandle);
            }
        }

        // define render state
        {
            nvrhi::RenderState renderState = nvrhi::RenderState();

            // define blend state
            {
                nvrhi::BlendState blendState = nvrhi::BlendState();
                nvrhi::BlendState::RenderTarget renderTarget = nvrhi::BlendState::RenderTarget();
                renderTarget.setBlendEnable(true);
                renderTarget.setSrcBlend(nvrhi::BlendFactor::SrcAlpha);
                renderTarget.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha);
                renderTarget.setBlendOp(nvrhi::BlendOp::Add);
                renderTarget.setSrcBlendAlpha(nvrhi::BlendFactor::One);
                renderTarget.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha);
                renderTarget.setBlendOpAlpha(nvrhi::BlendOp::Add);
                renderTarget.setColorWriteMask(nvrhi::ColorMask::All);
                blendState.setRenderTarget(0, renderTarget);

                renderState.setBlendState(blendState);
            }

            // define depth state
            {
                nvrhi::DepthStencilState depthStencilState = nvrhi::DepthStencilState();
                depthStencilState.setDepthTestEnable(false);
                depthStencilState.setDepthFunc(nvrhi::ComparisonFunc::Always);
                depthStencilState.setStencilEnable(false);

                // define stencil
                {
                    nvrhi::DepthStencilState::StencilOpDesc stencilOpDesc = nvrhi::DepthStencilState::StencilOpDesc();
                    stencilOpDesc.setDepthFailOp(nvrhi::StencilOp::Keep);
                    stencilOpDesc.setStencilFunc(nvrhi::ComparisonFunc::Always);
                    stencilOpDesc.setFailOp(nvrhi::StencilOp::Keep);
                    stencilOpDesc.setPassOp(nvrhi::StencilOp::Keep);

                    depthStencilState.setFrontFaceStencil(stencilOpDesc);
                    depthStencilState.setBackFaceStencil(stencilOpDesc);
                }

                renderState.setDepthStencilState(depthStencilState);
            }

            // define raster state
            {
                nvrhi::RasterState rasterState = nvrhi::RasterState();
                rasterState.setFillMode(nvrhi::RasterFillMode::Solid);
                rasterState.setCullMode(nvrhi::RasterCullMode::None);
                rasterState.setFrontCounterClockwise(false);
                rasterState.setDepthBias(0);
                rasterState.setDepthBiasClamp(0.0f);
                rasterState.setDepthClipEnable(true);
                rasterState.setMultisampleEnable(false);
                rasterState.setAntialiasedLineEnable(false);
                rasterState.setForcedSampleCount(0);
                rasterState.setConservativeRasterEnable(false);

                renderState.setRasterState(rasterState);
            }

            pipelineDesc.setRenderState(renderState);
        }

        // define binding layout
        {
            Graphics::RenderResourceManager& bindingManager = Application::GetInstance()->GetRenderer()->GetBindingManager();

            nvrhi::BindingLayoutDesc layoutDesc;
            layoutDesc.visibility = nvrhi::ShaderType::All;
            layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(VERTEX_CONSTANT_BUFFER_NVRHI)));
            layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
            layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));

            bd->BindingLayout = bindingManager.EmplaceResourceLayout("imgui_layout", layoutDesc);

            pipelineDesc.addBindingLayout(bd->BindingLayout);
        }

        nvrhi::FramebufferInfo fbInfo = nvrhi::FramebufferInfo();
        fbInfo.addColorFormat(nvrhi::Format::RGBA8_UNORM);

        nvrhi::SamplerDesc desc = nvrhi::SamplerDesc();
        desc.setAllAddressModes(nvrhi::SamplerAddressMode::Wrap);
        desc.setAllFilters(true);

        bd->FontSampler = bd->Device->createSampler(desc);

        bd->Pipeline = bd->Device->createGraphicsPipeline(pipelineDesc, fbInfo);
    }

    return true;
}

static void ImGui_ImplNVRHI_DestroyRenderBuffers(ImGui_ImplNVRHI_RenderBuffers* render_buffers)
{
    SafeRelease(render_buffers->IndexBuffer.GetBufferHandle());
    SafeRelease(render_buffers->VertexBuffer.GetBufferHandle());
    render_buffers->IndexBufferSize = render_buffers->VertexBufferSize = 0;
}

void ImGui_ImplNVRHI_InvalidateDeviceObjects()
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    if (!bd || !bd->Device)
    {
        return;
    }

    bd->Pipeline.Reset();

    // Destroy all textures
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures)
    {
        if (tex->RefCount == 1)
        {
            ImGui_ImplNVRHI_DestroyTexture(tex);
        }
    }
}

IMGUI_IMPL_API bool ImGui_ImplNVRHI_Init(ImGui_ImplNVRHI_InitInfo* init_info)
{
    ImGuiIO& io = ImGui::GetIO();
    IMGUI_CHECKVERSION();
    IM_ASSERT(io.BackendRendererUserData == nullptr && "Already initialized a renderer backend!");

    // Setup backend capabilities flags
    ImGui_ImplNVRHI_Data* bd = IM_NEW(ImGui_ImplNVRHI_Data)();
    bd->InitInfo = *init_info; // Deep copy
    init_info = &bd->InitInfo;

    bd->Device = init_info->Device;
    bd->CommandList = bd->Device->createCommandList();

    bd->RTFormat = init_info->RTFormat;
    bd->DSFormat = init_info->DSFormat;

    bd->NumFramesInFlight = init_info->NumFramesInFlight;
    bd->TearingSupport = false;

    io.BackendRendererUserData = (void*)bd;
    io.BackendRendererName = "imgui_impl_nvrhi";
    io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui_ImplNVRHI_InitMultiViewportSupport();
    }

    // Create a dummy ImGui_ImplNVRHI_ViewportData holder for the main viewport,
    // Since this is created and managed by the application, we will only use the ->Resources[] fields.
    ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    main_viewport->RendererUserData = IM_NEW(ImGui_ImplNVRHI_ViewportData)(bd->NumFramesInFlight);

    return true;
}

void ImGui_ImplNVRHI_Shutdown()
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    IM_ASSERT(bd != nullptr && "No renderer backend to shutdown, or already shutdown?");
    ImGuiIO& io = ImGui::GetIO();
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();

    // Manually delete main viewport render resources in-case we haven't initialized for viewports
    ImGuiViewport* main_viewport = ImGui::GetMainViewport();
    if (ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)main_viewport->RendererUserData)
    {
        // We could just call ImGui_ImplDX12_DestroyWindow(main_viewport) as a convenience but that would be misleading since we only use data->Resources[]
        for (UINT i = 0; i < bd->NumFramesInFlight; i++)
        {
            ImGui_ImplNVRHI_DestroyRenderBuffers(&vd->FrameRenderBuffers[i]);
        }
        IM_DELETE(vd);
        main_viewport->RendererUserData = nullptr;
    }

    ImGui_ImplNVRHI_ShutdownMultiViewportSupport();
    ImGui_ImplNVRHI_InvalidateDeviceObjects();

    io.BackendRendererName = nullptr;
    io.BackendRendererUserData = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasViewports);
    platform_io.ClearRendererHandlers();
    IM_DELETE(bd);
}

void ImGui_ImplNVRHI_NewFrame()
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    IM_ASSERT(bd != nullptr && "Context or backend not initialized! Did you call ImGui_ImplNVRHI_Init()?");

    if (!bd->Pipeline)
    {
        if (!ImGui_ImplNVRHI_CreateDeviceObjects())
        {
            IM_ASSERT(0 && "ImGui_ImplNVRHI_CreateDeviceObjects() failed!");
        }
    }
}

//--------------------------------------------------------------------------------------------------------
// MULTI-VIEWPORT / PLATFORM INTERFACE SUPPORT
// This is an _advanced_ and _optional_ feature, allowing the backend to create and handle multiple viewports simultaneously.
// If you are new to dear imgui or creating a new binding for dear imgui, it is recommended that you completely ignore this section first..
//--------------------------------------------------------------------------------------------------------

static void ImGui_ImplNVRHI_CreateWindow(ImGuiViewport* viewport)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_ViewportData* vd = IM_NEW(ImGui_ImplNVRHI_ViewportData)(bd->NumFramesInFlight);
    viewport->RendererUserData = vd;

    // Use shared command queue from init info
    vd->FrameIndex = 0;

    // Create command list
    vd->CommandList = bd->Device->createCommandList();

    // Create a new swap chain for this viewport
    Graphics::SwapChainDesc desc;
    desc.mWidth = viewport->Size.x;
    desc.mHeight = viewport->Size.y;
    desc.mFormat = bd->RTFormat;
    desc.mNativeWindowHandle = viewport->PlatformHandle;

    vd->SwapChain = Graphics::SwapChain::Create("ImGui Swap Chain", desc);

    for (UINT i = 0; i < bd->NumFramesInFlight; i++)
    {
        ImGui_ImplNVRHI_DestroyRenderBuffers(&vd->FrameRenderBuffers[i]);
    }
}

static void ImGui_WaitForPendingOperations(ImGui_ImplNVRHI_ViewportData* vd)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();

    bd->Device->queueWaitForCommandList(nvrhi::CommandQueue::Graphics, nvrhi::CommandQueue::Graphics, bd->FenceLastSignaledValue);
}

static ImGui_ImplNVRHI_FrameContext* ImGui_WaitForNextFrameContext(ImGui_ImplNVRHI_ViewportData* vd)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_FrameContext* frame_context = &vd->FrameCtx[vd->FrameIndex % vd->NumFramesInFlight];

    vd->SwapChain->Prepare();

    return frame_context;
}

static void ImGui_ImplNVRHI_DestroyWindow(ImGuiViewport* viewport)
{
    // The main viewport (owned by the application) will always have RendererUserData == 0 since we didn't create the data for it.
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    if (ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)viewport->RendererUserData)
    {
        ImGui_WaitForPendingOperations(vd);

        // Reset command list
        vd->CommandList.Reset();

        // Destroy swap chain
        if (vd->SwapChain)
        {
            delete vd->SwapChain;
            vd->SwapChain = nullptr;
        }

        for (UINT i = 0; i < bd->NumFramesInFlight; i++)
        {
            SafeRelease(vd->FrameCtx[i].RenderTarget.GetFramebufferTexture());
            ImGui_ImplNVRHI_DestroyRenderBuffers(&vd->FrameRenderBuffers[i]);
        }
        IM_DELETE(vd);
    }
    viewport->RendererUserData = nullptr;
}

static void ImGui_ImplNVRHI_SetWindowSize(ImGuiViewport* viewport, ImVec2 size)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)viewport->RendererUserData;

    ImGui_WaitForPendingOperations(vd);

    for (UINT i = 0; i < bd->NumFramesInFlight; i++)
    {
        SafeRelease(vd->FrameCtx[i].RenderTarget.GetFramebufferTexture());
    }

    if (vd->SwapChain)
    {
        vd->SwapChain->ResizeSwapChain(static_cast<uint32_t>(size.x), static_cast<uint32_t>(size.y));
    }
}

static void ImGui_ImplNVRHI_RenderWindow(ImGuiViewport* viewport, void*)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)viewport->RendererUserData;

    ImGui_ImplNVRHI_FrameContext* frame_context = ImGui_WaitForNextFrameContext(vd);
    UINT back_buffer_idx = vd->SwapChain->GetCurrentBackBufferIndex();

    const ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.0f);

    if (!(viewport->Flags & ImGuiViewportFlags_NoRendererClear))
    {
        vd->FrameCtx[back_buffer_idx].RenderTarget.ClearFramebuffer(vd->CommandList);
    }

    // Open the viewport's command list and begin recording render commands
    vd->CommandList->open();

    ImGui_ImplNVRHI_RenderDrawData(viewport->DrawData, vd->CommandList, vd->FrameCtx[back_buffer_idx].RenderTarget.GetBufferHandle());

    // Close the viewport's command list
    vd->CommandList->close();

    // Execute viewports command list and store the last submitted fence value
    bd->FenceLastSignaledValue = bd->Device->executeCommandList(vd->CommandList);

    // Update this frame context's fence value
    frame_context->FenceValue = bd->FenceLastSignaledValue;
}

static void ImGui_ImplNVRHI_SwapBuffers(ImGuiViewport* viewport, void*)
{
    ImGui_ImplNVRHI_Data* bd = ImGui_ImplNVRHI_GetBackendData();
    ImGui_ImplNVRHI_ViewportData* vd = (ImGui_ImplNVRHI_ViewportData*)viewport->RendererUserData;

    vd->SwapChain->Present();
    vd->FrameIndex++;
}

void ImGui_ImplNVRHI_InitMultiViewportSupport()
{
    ImGuiPlatformIO& platform_io = ImGui::GetPlatformIO();
    platform_io.Renderer_CreateWindow = ImGui_ImplNVRHI_CreateWindow;
    platform_io.Renderer_DestroyWindow = ImGui_ImplNVRHI_DestroyWindow;
    platform_io.Renderer_SetWindowSize = ImGui_ImplNVRHI_SetWindowSize;
    platform_io.Renderer_RenderWindow = ImGui_ImplNVRHI_RenderWindow;
    platform_io.Renderer_SwapBuffers = ImGui_ImplNVRHI_SwapBuffers;
}

void ImGui_ImplNVRHI_ShutdownMultiViewportSupport()
{
    ImGui::DestroyPlatformWindows();
}

//-----------------------------------------------------------------------------
