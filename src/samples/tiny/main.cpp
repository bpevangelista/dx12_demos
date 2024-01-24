#include "../../fastdx/fastdx.h"
#include <filesystem>
#include <fstream>

const int32_t kFrameCount = 3;
const float kClearColor[4] = { 0.0f, 0.2f, 0.4f, 1.0f };
const DXGI_FORMAT kFrameFormat = DXGI_FORMAT_R10G10B10A2_UNORM;

fastdx::D3D12DeviceWrapperPtr device;
fastdx::ID3D12CommandQueuePtr commandQueue;
fastdx::ID3D12CommandAllocatorPtr commandAllocators[kFrameCount];
fastdx::ID3D12GraphicsCommandListPtr commandList;
fastdx::IDXGISwapChainPtr swapChain;
fastdx::ID3D12DescriptorHeapPtr swapChainRtvHeap;
fastdx::ID3D12PipelineStatePtr pipelineState;
fastdx::ID3D12RootSignaturePtr pipelineRootSignature;
std::vector<fastdx::ID3D12ResourcePtr> renderTargets;
std::vector<uint8_t> vertexShader, pixelShader;

int32_t frameIndex = 0;
HANDLE fenceEvent;
fastdx::ID3D12FencePtr swapFence;
uint64_t swapFenceCounter = 0, swapFenceWaitValue[kFrameCount] = {};

HRESULT readShader(LPCWSTR filePath, std::vector<uint8_t>& outShaderData) {
    WCHAR modulePathBuffer[1024];
    GetModuleFileName(nullptr, modulePathBuffer, _countof(modulePathBuffer));
    auto fullFilePath = std::filesystem::path(modulePathBuffer).parent_path() / filePath;

    std::ifstream file(fullFilePath, std::ios::binary);
    if (file) {
        std::uintmax_t fileSize = std::filesystem::file_size(fullFilePath);
        outShaderData.resize(fileSize);
        file.read(reinterpret_cast<char*>(outShaderData.data()), fileSize);
    }
    return file? S_OK : E_FAIL;
}

void initializeD3d(HWND hwnd) {
    // Create a device and queue to dispatch command lists
    device = fastdx::createDevice(D3D_FEATURE_LEVEL_12_2);
    commandQueue = device->createCommandQueue(D3D12_COMMAND_LIST_TYPE_DIRECT);

    // Create a triple frame buffer swap chain for window
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = fastdx::defaultSwapChainDesc(hwnd);
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Format = kFrameFormat;
    swapChain = device->createSwapChainForHwnd(commandQueue, swapChainDesc, hwnd);

    // Create a heap of descriptors, then them fill with swap chain render targets desc
    swapChainRtvHeap = device->createHeapDescriptor(8, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    renderTargets = device->createRenderTargetViews(swapChain, swapChainRtvHeap);

    // Create one command allocator per frame buffer
    for (int32_t i = 0; i < kFrameCount; ++i) {
        commandAllocators[i] = device->createCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT);
    }

    // Single command list will reuse all allocators
    commandList = device->createCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[0]);
    commandList->Close();

    // Fence to wait for a completed frame to reuse
    swapFence = device->createFence(swapFenceCounter++, D3D12_FENCE_FLAG_NONE);
    fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    // Read VS and PS
    readShader(L"simple_vs.cso", vertexShader);
    readShader(L"simple_ps.cso", pixelShader);
    
    // Create a root signature for shaders
    pipelineRootSignature = device->createRootSignature(0, vertexShader.data(), vertexShader.size());

    // Create a pipeline state
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipelineDesc = fastdx::defaultGraphicsPipelineDesc(kFrameFormat);
    pipelineDesc.pRootSignature = pipelineRootSignature.get();
    pipelineDesc.VS = { vertexShader.data(), vertexShader.size() };
    pipelineDesc.PS = { pixelShader.data(), pixelShader.size() };
    pipelineState = device->createGraphicsPipelineState(pipelineDesc);
}

void draw() {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = swapChainRtvHeap->GetCPUDescriptorHandleForHeapStart();
    size_t heapDescriptorSize = device->d3dDevice()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtvHandle.ptr += frameIndex * heapDescriptorSize;

    D3D12_RESOURCE_BARRIER transitionBarrier = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION, D3D12_RESOURCE_BARRIER_FLAG_NONE };
    transitionBarrier.Transition.pResource = renderTargets[frameIndex].get();
    transitionBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    // Get and reset allocator for current frame, then point command list to it
    auto commandAllocator = commandAllocators[frameIndex];
    commandAllocator->Reset();
    commandList->Reset(commandAllocator.get(), nullptr);
    {
        // Present->RenderTarget barrier
        transitionBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        transitionBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        commandList->ResourceBarrier(1, &transitionBarrier);

        commandList->SetPipelineState(pipelineState.get());
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);
        commandList->ClearRenderTargetView(rtvHandle, kClearColor, 0, nullptr);

        // RenderTarget->Present barrier
        transitionBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        transitionBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        commandList->ResourceBarrier(1, &transitionBarrier);
    }
    commandList->Close();

    // Dispatch command list and present
    ID3D12CommandList* commandLists[] = { commandList.get() };
    commandQueue->ExecuteCommandLists(_countof(commandLists), commandLists);
    swapChain->Present(1, 0);

    // Queue always signal increasing counter values
    commandQueue->Signal(swapFence.get(), swapFenceCounter);
    swapFenceWaitValue[frameIndex] = swapFenceCounter++;

    // Wait if next frame not ready
    int32_t nextFrameIndex = swapChain->GetCurrentBackBufferIndex();
    if (swapFence->GetCompletedValue() < swapFenceWaitValue[nextFrameIndex]) {
        swapFence->SetEventOnCompletion(swapFenceWaitValue[nextFrameIndex], fenceEvent);
        WaitForSingleObjectEx(fenceEvent, INFINITE, FALSE);
    }
    frameIndex = nextFrameIndex;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    fastdx::WindowProperties prop;
    HWND hwnd = fastdx::createWindow(prop);
    initializeD3d(hwnd);

    return fastdx::runMainLoop(nullptr, draw);
}
