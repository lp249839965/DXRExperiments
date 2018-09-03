#pragma once

namespace DXRFramework
{
    class RtBindings;
    class RtState;

    class RtContext
    {
    public:
        using SharedPtr = std::shared_ptr<RtContext>;

        static SharedPtr create(ID3D12Device *device, ID3D12GraphicsCommandList *commandList);
        ~RtContext();

        ID3D12Device *getDevice() const { return mDevice; }
        ID3D12GraphicsCommandList *getCommandList() const { return mCommandList; }
        ID3D12RaytracingFallbackDevice *getFallbackDevice() const { return mFallbackDevice.Get(); }
        ID3D12RaytracingFallbackCommandList *getFallbackCommandList() const { return mFallbackCommandList.Get(); }

        void raytrace(std::shared_ptr<RtBindings> bindings, std::shared_ptr<RtState> state, uint32_t width, uint32_t height);

        void bindDescriptorHeap();
        D3D12_GPU_DESCRIPTOR_HANDLE getDescriptorGPUHandle(UINT heapIndex);

        // Create a wrapped pointer for the Fallback Layer path.
        UINT allocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE* cpuDescriptor, UINT descriptorIndexToUse = UINT_MAX);

        // Allocate a descriptor and return its index. 
        // If the passed descriptorIndexToUse is valid, it will be used instead of allocating a new one.
        WRAPPED_GPU_POINTER createFallbackWrappedPointer(ID3D12Resource* resource, UINT bufferNumElements);

    private:
        RtContext(ID3D12Device *device, ID3D12GraphicsCommandList *commandList);

        ID3D12Device *mDevice;
        ID3D12GraphicsCommandList *mCommandList;

        ComPtr<ID3D12RaytracingFallbackDevice> mFallbackDevice;
        ComPtr<ID3D12RaytracingFallbackCommandList> mFallbackCommandList;
        
        // RT global descriptor heap
        ComPtr<ID3D12DescriptorHeap> mDescriptorHeap;
        UINT mDescriptorsAllocated;
        UINT mDescriptorSize;

        void createDescriptorHeap();
    };
}