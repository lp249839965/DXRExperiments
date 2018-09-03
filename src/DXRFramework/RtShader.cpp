#include "stdafx.h"
#include "RtShader.h"
#include "nv_helpers_dx12/RootSignatureGenerator.h"

namespace DXRFramework
{
    RtShader::SharedPtr RtShader::create(RtContext::SharedPtr context, RtShaderType shaderType, const std::string &entryPoint, uint32_t maxPayloadSize, uint32_t maxAttributesSize)
    {
        return SharedPtr(new RtShader(context, shaderType, entryPoint, maxPayloadSize, maxAttributesSize));
    }

    RtShader::RtShader(RtContext::SharedPtr context, RtShaderType shaderType, const std::string &entryPoint, uint32_t maxPayloadSize, uint32_t maxAttributesSize)
        : mFallbackDevice(context->getFallbackDevice()), mShaderType(shaderType), mEntryPoint(entryPoint), mMaxPayloadSize(maxPayloadSize), mMaxAttributesSize(maxAttributesSize)
    {
        // Reflection
        // TODO:

        mLocalRootSignature = TempCreateLocalRootSignature();
    }

    RtShader::~RtShader() = default;

    ID3D12RootSignature *RtShader::TempCreateLocalRootSignature()
    {
        nv_helpers_dx12::RootSignatureGenerator rootSigGenerator;
        return rootSigGenerator.Generate(mFallbackDevice, true /* local root signature */);
    }
}
