#include "stdafx.h"
#include "DXRFrameworkApp.h"
#include "nv_helpers_dx12/DXRHelper.h"
#include "DirectXRaytracingHelper.h"
#include "ImGuiRendererDX.h"
#include "GameInput.h"

using namespace std;
using namespace DXRFramework;

namespace GameCore 
{ 
    extern HWND g_hWnd; 
}

DXRFrameworkApp::DXRFrameworkApp(UINT width, UINT height, std::wstring name) :
    DXSample(width, height, name),
    mBypassRaytracing(false)
{
    UpdateForSizeChange(width, height);
}

void DXRFrameworkApp::OnInit()
{
    m_deviceResources = std::make_unique<DX::DeviceResources>(
        DXGI_FORMAT_R16G16B16A16_FLOAT,//DXGI_FORMAT_R8G8B8A8_UNORM
        DXGI_FORMAT_UNKNOWN,
        FrameCount,
        D3D_FEATURE_LEVEL_12_0,
        // Sample shows handling of use cases with tearing support, which is OS dependent and has been supported since TH2.
        // Since the Fallback Layer requires Fall Creator's update (RS3), we don't need to handle non-tearing cases.
        DX::DeviceResources::c_RequireTearingSupport,
        m_adapterIDoverride
    );
    m_deviceResources->RegisterDeviceNotify(this);
    m_deviceResources->SetWindow(Win32Application::GetHwnd(), GetWidth(), GetHeight());
    m_deviceResources->InitializeDXGIAdapter();
    mNativeDxrSupported = IsDirectXRaytracingSupported(m_deviceResources->GetAdapter());
    ThrowIfFalse(EnableComputeRaytracingFallback(m_deviceResources->GetAdapter()));

    m_deviceResources->CreateDeviceResources();
    m_deviceResources->CreateWindowSizeDependentResources();

    GameInput::Initialize();

    // Initialize texture loader
    #if (_WIN32_WINNT >= 0x0A00 /*_WIN32_WINNT_WIN10*/)
        Microsoft::WRL::Wrappers::RoInitializeWrapper initialize(RO_INIT_MULTITHREADED);
        ThrowIfFailed(initialize, L"Cannot initialize WIC");
    #else
        #error Unsupported Windows version
    #endif

    // Setup camera states
    mCamera.reset(new Math::Camera());
    mCamera->SetAspectRatio(m_aspectRatio);
    mCamera->SetEyeAtUp(Math::Vector3(1.0, 1.2, 4.0), Math::Vector3(0.0, 0.5, 0.0), Math::Vector3(Math::kYUnitVector));
    mCamera->SetZRange(1.0f, 10000.0f);
    mCamController.reset(new GameCore::CameraController(*mCamera, mCamera->GetUpVec()));
    mCamController->EnableFirstPersonMouse(false);
 
    InitRaytracing();

    // Initialize UI renderer
    ui::RendererDX::Initialize(GameCore::g_hWnd, m_deviceResources->GetD3DDevice(), m_deviceResources->GetBackBufferFormat(), FrameCount, [&] () {
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        UINT heapOffset = mRtContext->allocateDescriptor(&cpuHandle);
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = mRtContext->getDescriptorGPUHandle(heapOffset);
        return std::make_pair(cpuHandle, gpuHandle);
    }); 
}

void DXRFrameworkApp::InitRaytracing()
{
    auto device = m_deviceResources->GetD3DDevice();
    auto commandList = m_deviceResources->GetCommandList();

    mRtContext = RtContext::create(device, commandList, false /* force compute */);
    mRaytracingPipeline = ProgressiveRaytracingPipeline::create(mRtContext);

    // Create scene
    mRtScene = RtScene::create();
    {
        auto identity = DirectX::XMMatrixIdentity();

        // working directory is "vc2015"
        mRtScene->addModel(RtModel::create(mRtContext, "..\\assets\\models\\pica\\Machines.fbx"), identity);
    }
    mRaytracingPipeline->setScene(mRtScene);

    // Configure raytracing pipeline
    {
        ProgressiveRaytracingPipeline::Material material1 = {};
        material1.params.albedo = XMFLOAT4(0.95f, 0.95f, 0.95f, 1.0f);
        material1.params.specular = XMFLOAT4(0.58f, 0.58f, 0.58f, 1.0f);
        material1.params.roughness = 0.08f;
        material1.params.reflectivity = 1.0f;
        material1.params.type = 1;

        mRaytracingPipeline->addMaterial(material1);
    }
    mRaytracingPipeline->setCamera(mCamera);
    mRaytracingPipeline->loadResources(m_deviceResources->GetCommandQueue(), FrameCount);
    mRaytracingPipeline->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());

    if (!mBypassRaytracing) {
        // Build acceleration structures
        commandList->Reset(m_deviceResources->GetCommandAllocator(), nullptr);
        mRaytracingPipeline->buildAccelerationStructures();
        m_deviceResources->ExecuteCommandList();
        m_deviceResources->WaitForGpu();
    }

    mDenoiser = DenoiseCompositor::create(mRtContext);
    mDenoiser->loadResources(m_deviceResources->GetCommandQueue(), FrameCount, mBypassRaytracing);
    mDenoiser->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
}

void DXRFrameworkApp::OnUpdate()
{
    DXSample::OnUpdate();

    // Begin recording UI draw list
    ui::RendererDX::NewFrame();

    float elapsedTime = static_cast<float>(mTimer.GetTotalSeconds());
    float deltaTime = static_cast<float>(mTimer.GetElapsedSeconds());

    GameInput::Update(deltaTime);
    mCamController->Update(deltaTime);

    ui::Checkbox("ProgressiveRaytracingPipeline", &mRaytracingPipeline->mActive);
    ui::Checkbox("DenosieCompositor", &mDenoiser->mActive);

    if (mRaytracingPipeline->mActive) {
        mRaytracingPipeline->userInterface();
        mRaytracingPipeline->update(elapsedTime, GetFrameCount(), m_deviceResources->GetPreviousFrameIndex(), m_deviceResources->GetCurrentFrameIndex(), GetWidth(), GetHeight());
    }

    if (mDenoiser->mActive) {
        mDenoiser->userInterface();
    }
}

void DXRFrameworkApp::OnRender()
{
    if (!m_deviceResources->IsWindowVisible()) return;

    // Reset command list
    m_deviceResources->Prepare();
    auto commandList = m_deviceResources->GetCommandList();
    auto currentFrame = m_deviceResources->GetCurrentFrameIndex();

    if (mBypassRaytracing || !mRaytracingPipeline->mActive) {
        auto rtvHandle = m_deviceResources->GetRenderTargetView();
        const float clearColor[] = { 0.3f, 0.2f, 0.1f, 1.0f };
        commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

        // Insert rasterizeration code here

        // Run denoiser with mock input textures
        if (mBypassRaytracing && mDenoiser->mActive) {
            mDenoiser->dispatch(commandList, D3D12_GPU_DESCRIPTOR_HANDLE{0}, currentFrame, GetWidth(), GetHeight());
            BlitToBackbuffer(mDenoiser->getOutputResource());
        }
    } else {
        mRaytracingPipeline->render(commandList, currentFrame, GetWidth(), GetHeight());

        auto outputResource = mRaytracingPipeline->getOutputResource();
        if (mDenoiser->mActive) {
            mRtContext->transitionResource(outputResource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            mDenoiser->dispatch(commandList, mRaytracingPipeline->getOutputSrvHandle(), currentFrame, GetWidth(), GetHeight());
            mRtContext->transitionResource(outputResource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            BlitToBackbuffer(mDenoiser->getOutputResource());
        } else {
            BlitToBackbuffer(outputResource);
        }
    }

    // Render UI
    {
        mRtContext->bindDescriptorHeap();

        auto rtvHandle = m_deviceResources->GetRenderTargetView();
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        ui::RendererDX::Render(commandList);
    }

    // Execute command list and insert fence
    m_deviceResources->Present(D3D12_RESOURCE_STATE_RENDER_TARGET);
}

void DXRFrameworkApp::OnKeyDown(UINT8 key)
{
    switch (key) {
    case 'F':
        mCamController->EnableFirstPersonMouse(!mCamController->IsFirstPersonMouseEnabled());
        break;
    }
}

void DXRFrameworkApp::OnDestroy()
{
    m_deviceResources->WaitForGpu();

    ui::RendererDX::Shutdown();
    GameInput::Shutdown();
}

void DXRFrameworkApp::OnSizeChanged(UINT width, UINT height, bool minimized)
{
    if (!m_deviceResources->WindowSizeChanged(width, height, minimized)) {
        return;
    }

    UpdateForSizeChange(width, height);

    mCamera->SetAspectRatio(m_aspectRatio);
    mRaytracingPipeline->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
    mDenoiser->createOutputResource(m_deviceResources->GetBackBufferFormat(), GetWidth(), GetHeight());
}

void DXRFrameworkApp::BlitToBackbuffer(ID3D12Resource *textureResource, D3D12_RESOURCE_STATES fromState, D3D12_RESOURCE_STATES toState)
{
    auto commandList= m_deviceResources->GetCommandList();
    auto renderTarget = m_deviceResources->GetRenderTarget();

    mRtContext->transitionResource(renderTarget, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
    mRtContext->transitionResource(textureResource, fromState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    commandList->CopyResource(renderTarget, textureResource);

    mRtContext->transitionResource(renderTarget, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
    mRtContext->transitionResource(textureResource, D3D12_RESOURCE_STATE_COPY_SOURCE, toState);
}

LRESULT DXRFrameworkApp::WindowProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ui::RendererDX::WindowProcHandler(hwnd, msg, wParam, lParam);
}
