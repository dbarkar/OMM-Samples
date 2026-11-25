/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#include "OmmHelper.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#    include <io.h>
#    include <share.h>
#else
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace ommhelper {
void OpacityMicroMapsHelper::Initialize(nri::Device* device, bool disableMaskedGeometryBuild) {
    m_Device = device;
    if (m_Device) {
        uint32_t nriResult = (uint32_t)nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::CoreInterface), (nri::CoreInterface*)&NRI);
        nriResult |= (uint32_t)nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::HelperInterface), (nri::HelperInterface*)&NRI);
        nriResult |= (uint32_t)nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::RayTracingInterface), (nri::RayTracingInterface*)&NRI);

        ommMessageInterface log;
        log.userArg = nullptr;
        log.messageCallback = [](ommMessageSeverity severity, const char* message, void* /* userArg*/) {
            const char* severityStr = "";
            switch (severity) {
                case ommMessageSeverity_Info:
                    severityStr = "INFO";
                    break;
                case ommMessageSeverity_PerfWarning:
                    severityStr = "WARNING";
                    break;
                case ommMessageSeverity_Error:
                    severityStr = "ERROR";
                    break;
                case ommMessageSeverity_Fatal:
                    severityStr = "FATAL";
                    break;
                default:
                    severityStr = "UNKNOWN";
                    break;
            }
            printf("[OMM][%s]: %s\n", severityStr, message);
        };
        ommBakerCreationDesc desc = ommBakerCreationDescDefault();
        desc.messageInterface = log;
        desc.type = ommBakerType_CPU;
        if (ommCreateBaker(&desc, &m_OmmCpuBaker) != ommResult_SUCCESS) {
            printf("[FAIL]: ommCreateOpacityMicromapBaker\n");
            std::abort();
        }

        nri::GraphicsAPI gapi = NRI.GetDeviceDesc(*m_Device).graphicsAPI;
        if (gapi != nri::GraphicsAPI::D3D12 && gapi != nri::GraphicsAPI::VK) {
            printf("[FAIL]: Unsupported Graphics API\n");
            std::abort();
        }

        m_GpuBakerIntegration.Initialize(*m_Device);

        m_DisableGeometryBuild = disableMaskedGeometryBuild;
        if (m_DisableGeometryBuild)
            return; // diable geometry

        if (gapi == nri::GraphicsAPI::D3D12) {
            nriResult |= (uint32_t)nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::WrapperD3D12Interface), (nri::WrapperD3D12Interface*)&NRI);
            InitializeD3D12();
        } else if (NRI.GetDeviceDesc(*m_Device).graphicsAPI == nri::GraphicsAPI::VK) {
            nriResult |= (uint32_t)nri::nriGetInterface(*m_Device, NRI_INTERFACE(nri::WrapperVKInterface), (nri::WrapperVKInterface*)&NRI);
            InitializeVK();
        }
    }
}

void OpacityMicroMapsHelper::Destroy() {
    m_GpuBakerIntegration.Destroy();
    ommDestroyBaker(m_OmmCpuBaker);
    ReleaseGeometryMemory();

    if(m_Device5)
        m_Device5->Release();
}

#pragma region[ Utils ]

inline ommCpuTextureFormat GetOmmBakerTextureFormat(nri::Format format) {
    switch (format) {
        case nri::Format::R32_SFLOAT:
            return ommCpuTextureFormat_FP32;
        case nri::Format::R8_UNORM:
            return ommCpuTextureFormat_UNORM8;
        default:
            printf("[FAIL] Unknown texture format passed to Cpu Baker!\n");
            std::abort();
    }
}

inline ommIndexFormat GetOmmBakerIndexFormat(nri::Format format) {
    switch (format) {
        case nri::Format::R8_UINT:
            return ommIndexFormat_UINT_8;
        case nri::Format::R16_UINT:
            return ommIndexFormat_UINT_16;
        case nri::Format::R32_UINT:
            return ommIndexFormat_UINT_32;
        default:
            printf("[FAIL] Unknown index format passed to Cpu Baker!\n");
            std::abort();
    }
}

inline ommTexCoordFormat GetOmmBakerUvFormat(nri::Format format) {
    switch (format) {
        case nri::Format::RG16_SFLOAT:
            return ommTexCoordFormat_UV16_FLOAT;
        case nri::Format::RG32_SFLOAT:
            return ommTexCoordFormat_UV32_FLOAT;
        case nri::Format::RG16_UNORM:
            return ommTexCoordFormat_UV16_UNORM;
        default:
            printf("[FAIL] Unknown UV format passed to Cpu Baker!\n");
            std::abort();
    }
}

inline ommFormat GetOmmFormat(OmmFormats format) {
    switch (format) {
        case OmmFormats::OC1_2_STATE:
            return ommFormat_OC1_2_State;
        case OmmFormats::OC1_4_STATE:
            return ommFormat_OC1_4_State;
        default:
            printf("[FAIL] Unknown OMM format passed to Cpu Baker!\n");
            std::abort();
    }
}

inline nri::Format GetNriIndexFormat(ommIndexFormat format) {
    switch (format) {
        case ommIndexFormat_UINT_8:
            return nri::Format::R8_UINT;
        case ommIndexFormat_UINT_16:
            return nri::Format::R16_UINT;
        case ommIndexFormat_UINT_32:
            return nri::Format::R32_UINT;
        default:
            printf("[FAIL] Unknown Index format returned from Cpu Baker!\n");
            std::abort();
    }
}

inline ommTextureAddressMode GetOmmAddressingMode(nri::AddressMode mode) {
    switch (mode) {
        case nri::AddressMode::REPEAT:
            return ommTextureAddressMode_Wrap;
        case nri::AddressMode::MIRRORED_REPEAT:
            return ommTextureAddressMode_Mirror;
        case nri::AddressMode::CLAMP_TO_EDGE:
            return ommTextureAddressMode_Clamp;
        case nri::AddressMode::CLAMP_TO_BORDER:
            return ommTextureAddressMode_Border;
        default:
            printf("[FAIL] Ivalid AddressMode passed to Cpu Baker!\n");
            std::abort();
    }
}

void OpacityMicroMapsHelper::ConvertUsageCountsToApiFormat(uint8_t* outFormattedBuffer, size_t& outSize, const uint8_t* bakerOutputBuffer, size_t bakerOutputBufferSize) {
    size_t stride = 0;
    if (NRI.GetDeviceDesc(*m_Device).graphicsAPI == nri::GraphicsAPI::D3D12) {
        stride = sizeof(D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY);

        size_t countsNum = bakerOutputBufferSize / sizeof(ommCpuOpacityMicromapUsageCount);
        outSize = countsNum * stride;

        if (!outFormattedBuffer)
            return;

        std::vector<D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY> sanitizedUsageCounts;
        ommCpuOpacityMicromapUsageCount* ommData = (ommCpuOpacityMicromapUsageCount*)bakerOutputBuffer;
        for (size_t i = 0; i < countsNum; ++i) {
            D3D12_RAYTRACING_OPACITY_MICROMAP_HISTOGRAM_ENTRY usageDesc = {ommData[i].count, ommData[i].subdivisionLevel, (D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT)ommData[i].format};
            sanitizedUsageCounts.push_back(usageDesc);
        }
        memcpy(outFormattedBuffer, sanitizedUsageCounts.data(), outSize);
    } else {
        stride = sizeof(VkMicromapUsageEXT);

        size_t countsNum = bakerOutputBufferSize / sizeof(ommCpuOpacityMicromapUsageCount);
        outSize = countsNum * stride;

        if (!outFormattedBuffer)
            return;

        std::vector<VkMicromapUsageEXT> sanitizedUsageCounts;
        ommCpuOpacityMicromapUsageCount* ommData = (ommCpuOpacityMicromapUsageCount*)bakerOutputBuffer;
        for (size_t i = 0; i < countsNum; ++i) {
            VkMicromapUsageEXT usageDesc = {ommData[i].count, ommData[i].subdivisionLevel, (uint32_t)ommData[i].format};
            sanitizedUsageCounts.push_back(usageDesc);
        }
        memcpy(outFormattedBuffer, sanitizedUsageCounts.data(), outSize);
    }
}

void OpacityMicroMapsHelper::DestroyMaskedGeometry(nri::AccelerationStructure* blas, nri::Buffer* ommArray) {
    if (NRI.GetDeviceDesc(*m_Device).graphicsAPI == nri::GraphicsAPI::D3D12) {
        if (ommArray)
            NRI.DestroyBuffer(ommArray);
    } else {
        if (ommArray)
            DestroyOmmArrayVK(ommArray);
        if (blas)
            DestroyBlasVK(blas);
    }

    if (blas)
        NRI.DestroyAccelerationStructure(blas);
}

void OpacityMicroMapsHelper::ReleaseGeometryMemory() {
    ReleaseMemoryD3D12();
    for (nri::Buffer* buffer : m_VkBuffers)
        NRI.DestroyBuffer(buffer);
    m_VkBuffers.clear();
    m_CurrentHeapOffset = 0;

    if (m_ScratchBuffer)
        NRI.DestroyBuffer(m_ScratchBuffer);
    m_ScratchBuffer = nullptr;
    m_ScratchAddress = 0;
}

#pragma endregion

#pragma region[ CPU baking ]

static ommCpuBakeFlags GetCpuBakeFlags(CpuBakerFlags cpuBakerFlags) {
    uint32_t result = 0;
    result |= cpuBakerFlags.enableInternalThreads ? uint32_t(ommCpuBakeFlags_EnableInternalThreads) : 0;
    result |= !cpuBakerFlags.enableSpecialIndices ? uint32_t(ommCpuBakeFlags_DisableSpecialIndices) : 0;
    result |= !cpuBakerFlags.enableDuplicateDetection ? uint32_t(ommCpuBakeFlags_DisableDuplicateDetection) : 0;
    result |= cpuBakerFlags.enableNearDuplicateDetection ? uint32_t(ommCpuBakeFlags_EnableNearDuplicateDetection) : 0;
    result |= cpuBakerFlags.force32bitIndices ? uint32_t(ommCpuBakeFlags_Force32BitIndices) : 0;
    result |= cpuBakerFlags.allow8bitIndices ? uint32_t(ommCpuBakeFlags_Allow8BitIndices) : 0;
    return ommCpuBakeFlags(result);
}

void OpacityMicroMapsHelper::BakeOpacityMicroMapsCpu(OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& desc) {
    for (size_t i = 0; i < count; ++i) {
        OmmBakeGeometryDesc& instance = *queue[i];

        InputTexture& inTexture = instance.texture;
        ommCpuTextureMipDesc texuteMipDescs[OMM_MAX_MIP_NUM] = {};
        for (uint32_t mip = 0; mip < inTexture.mipNum; ++mip) {
            ommCpuTextureMipDesc& texuteMipDesc = texuteMipDescs[mip];
            texuteMipDesc = ommCpuTextureMipDescDefault();
            MipDesc& inMipDesc = inTexture.mips[mip];
            texuteMipDesc.width = inMipDesc.width;
            texuteMipDesc.height = inMipDesc.height;
            texuteMipDesc.textureData = inMipDesc.nriTextureOrPtr.ptr;
        }

        ommCpuTextureDesc textureDesc = ommCpuTextureDescDefault();
        textureDesc.mipCount = inTexture.mipNum;
        textureDesc.mips = texuteMipDescs;
        textureDesc.format = GetOmmBakerTextureFormat(inTexture.format);
        textureDesc.alphaCutoff = instance.alphaCutoff;

        ommCpuTexture vmTex = 0;
        if (ommCpuCreateTexture(m_OmmCpuBaker, &textureDesc, &vmTex) != ommResult_SUCCESS) {
            printf("[FAIL]: ommCpuCreateTexture\n");
            std::abort();
        }

        ommCpuBakeInputDesc bakeDesc = ommCpuBakeInputDescDefault();
        bakeDesc.texture = vmTex;
        bakeDesc.alphaMode = ommAlphaMode(instance.alphaMode);
        bakeDesc.runtimeSamplerDesc.addressingMode = GetOmmAddressingMode(inTexture.addressingMode);
        bakeDesc.runtimeSamplerDesc.filter = ommTextureFilterMode(desc.filter);
        bakeDesc.maxSubdivisionLevel = (uint8_t)desc.subdivisionLevel;
        bakeDesc.alphaCutoff = instance.alphaCutoff;
        bakeDesc.dynamicSubdivisionScale = desc.dynamicSubdivisionScale;

        InputBuffer& inIndices = instance.indices;
        bakeDesc.indexFormat = GetOmmBakerIndexFormat(inIndices.format);
        bakeDesc.indexBuffer = (uint8_t*)inIndices.nriBufferOrPtr.ptr;
        bakeDesc.indexCount = (uint32_t)inIndices.numElements;

        InputBuffer& inUvs = instance.uvs;
        bakeDesc.texCoords = (uint8_t*)inUvs.nriBufferOrPtr.ptr;
        bakeDesc.texCoordFormat = GetOmmBakerUvFormat(inUvs.format);

        bakeDesc.bakeFlags = GetCpuBakeFlags(desc.cpuFlags);
        bakeDesc.format = GetOmmFormat(desc.format);

        ommCpuBakeResult bakeResult;
        ommResult res = ommCpuBake(m_OmmCpuBaker, &bakeDesc, &bakeResult);

        if (res == ommResult_WORKLOAD_TOO_BIG) {
            printf("[WARNING]: ommCpuBakeOpacityMicromap - Workload size is too big.\n");
            return;
        }

        if (res != ommResult_SUCCESS) {
            printf("[FAIL]: ommCpuBakeVisibilityMap\n");
            std::abort();
        }

        const ommCpuBakeResultDesc* resDesc = nullptr;
        res = ommCpuGetBakeResultDesc(bakeResult, &resDesc);

        if (res != ommResult_SUCCESS) {
            printf("[FAIL]: ommCpuGetBakeResultDesc\n");
            std::abort();
        }

        if (resDesc->arrayData) {
            instance.outData[(uint32_t)OmmDataLayout::ArrayData].resize(resDesc->arrayDataSize);
            memcpy(instance.outData[(uint32_t)OmmDataLayout::ArrayData].data(), resDesc->arrayData, resDesc->arrayDataSize);

            size_t ommDescArraySize = resDesc->descArrayCount * sizeof(ommCpuOpacityMicromapDesc);
            instance.outData[(uint32_t)OmmDataLayout::DescArray].resize(ommDescArraySize);
            memcpy(instance.outData[(uint32_t)OmmDataLayout::DescArray].data(), resDesc->descArray, ommDescArraySize);

            size_t ommDescArrayHistogramSize = resDesc->descArrayHistogramCount * sizeof(ommCpuOpacityMicromapDesc);
            instance.outData[(uint32_t)OmmDataLayout::DescArrayHistogram].resize(ommDescArrayHistogramSize);
            memcpy(instance.outData[(uint32_t)OmmDataLayout::DescArrayHistogram].data(), resDesc->descArrayHistogram, ommDescArrayHistogramSize);
            instance.outDescArrayHistogramCount = resDesc->descArrayHistogramCount;

            size_t ommIndexHistogramSize = resDesc->indexHistogramCount * sizeof(ommCpuOpacityMicromapDesc);
            instance.outData[(uint32_t)OmmDataLayout::IndexHistogram].resize(ommIndexHistogramSize);
            memcpy(instance.outData[(uint32_t)OmmDataLayout::IndexHistogram].data(), resDesc->indexHistogram, ommIndexHistogramSize);
            instance.outIndexHistogramCount = resDesc->indexHistogramCount;

            size_t stride = resDesc->indexFormat == ommIndexFormat_UINT_8 ? sizeof(uint8_t) : resDesc->indexFormat == ommIndexFormat_UINT_16 ? sizeof(uint16_t) : sizeof(uint32_t);
            size_t indexDataSize = resDesc->indexCount * stride;
            instance.outOmmIndexFormat = GetNriIndexFormat(resDesc->indexFormat);
            instance.outOmmIndexStride = (uint32_t)stride;
            instance.outData[(uint32_t)OmmDataLayout::Indices].resize(indexDataSize);
            memcpy(instance.outData[(uint32_t)OmmDataLayout::Indices].data(), resDesc->indexBuffer, indexDataSize);
        }
        ommCpuDestroyTexture(m_OmmCpuBaker, vmTex);
        ommCpuDestroyBakeResult(bakeResult);
    }
}

#pragma endregion

#pragma region[ GPU Baking ]

inline BakerBakeFlags GetGpuBakeFlags(const OmmBakeDesc& bakeDesc, OmmGpuBakerPass pass) {
    uint32_t result = 0;
    const GpuBakerFlags& flags = bakeDesc.gpuFlags;
    result |= uint32_t(pass);
    result |= !flags.enableSpecialIndices ? uint32_t(BakerBakeFlags::DisableSpecialIndices) : 0;
    result |= !flags.enableTexCoordDeduplication ? uint32_t(BakerBakeFlags::DisableTexCoordDeduplication) : 0;
    result |= flags.enablePostBuildInfo ? uint32_t(BakerBakeFlags::EnablePostBuildInfo) : 0;
    result |= bakeDesc.enableDebugMode ? uint32_t(BakerBakeFlags::EnableNsightDebugMode) : 0;
    result |= flags.force32bitIndices ? uint32_t(BakerBakeFlags::Force32BitIndices) : 0;
    result |= flags.computeOnlyWorkload ? uint32_t(BakerBakeFlags::ComputeOnly) : 0;
    result |= flags.allow8bitIndices ? uint32_t(BakerBakeFlags::Allow8BitIndices) : 0;
    return BakerBakeFlags(result);
}

inline void FillGpuBakerInputBufferDesc(BufferResource& bakerDesc, const InputBuffer& inDesc) {
    bakerDesc.buffer = (nri::Buffer*)inDesc.nriBufferOrPtr.buffer;
    bakerDesc.format = inDesc.format;
    bakerDesc.state = nri::AccessBits::SHADER_RESOURCE;
    bakerDesc.size = inDesc.bufferSize - inDesc.offset;
    bakerDesc.offset = inDesc.offset;
    bakerDesc.numElements = inDesc.numElements;
    bakerDesc.stride = inDesc.stride;
    bakerDesc.offsetInStruct = inDesc.offsetInStruct;
}

inline void FillGpuBakerResourceBufferDesc(BufferResource& bakerDesc, const GpuBakerBuffer& inDesc) {
    bakerDesc.buffer = inDesc.buffer;
    bakerDesc.offset = inDesc.offset;
    bakerDesc.size = inDesc.dataSize;
    bakerDesc.state = nri::AccessBits::NONE;
}

inline void FillInputGeometryDesc(const OmmBakeGeometryDesc& desc, InputGeometryDesc& geometryDesc, const OmmBakeDesc& bakeDesc, OmmGpuBakerPass pass) {
    BakerInputs& inputs = geometryDesc.inputs;
    FillGpuBakerInputBufferDesc(inputs.inIndexBuffer, desc.indices);
    FillGpuBakerInputBufferDesc(inputs.inUvBuffer, desc.uvs);

    const InputTexture& texture = desc.texture;
    inputs.inTexture.texture = texture.mips[0].nriTextureOrPtr.texture;
    inputs.inTexture.state = nri::AccessBits::SHADER_RESOURCE;
    inputs.inTexture.layout = nri::Layout::SHADER_RESOURCE;
    inputs.inTexture.format = texture.format;
    inputs.inTexture.width = texture.mips[0].width;
    inputs.inTexture.height = texture.mips[0].height;
    inputs.inTexture.mipOffset = texture.mipOffset;
    inputs.inTexture.alphaChannelId = texture.alphaChannelId;

    for (size_t i = 0; i < OMM_MAX_TRANSIENT_POOL_BUFFERS; ++i)
        FillGpuBakerResourceBufferDesc(inputs.inTransientPool[i], desc.transientBuffers[i]);

    BakerOutputs& outputs = geometryDesc.outputs;
    FillGpuBakerResourceBufferDesc(outputs.outArrayData, desc.gpuBuffers[(uint32_t)OmmDataLayout::ArrayData]);
    FillGpuBakerResourceBufferDesc(outputs.outDescArray, desc.gpuBuffers[(uint32_t)OmmDataLayout::DescArray]);
    FillGpuBakerResourceBufferDesc(outputs.outIndexBuffer, desc.gpuBuffers[(uint32_t)OmmDataLayout::Indices]);
    FillGpuBakerResourceBufferDesc(outputs.outArrayHistogram, desc.gpuBuffers[(uint32_t)OmmDataLayout::DescArrayHistogram]);
    FillGpuBakerResourceBufferDesc(outputs.outIndexHistogram, desc.gpuBuffers[(uint32_t)OmmDataLayout::IndexHistogram]);
    FillGpuBakerResourceBufferDesc(outputs.outPostBuildInfo, desc.gpuBuffers[(uint32_t)OmmDataLayout::GpuPostBuildInfo]);

    BakerSettings& settings = geometryDesc.settings;
    settings.alphaCutoff = desc.alphaCutoff;
    settings.borderAlpha = desc.borderAlpha;
    settings.alphaMode = BakerAlphaMode(desc.alphaMode);

    settings.globalOMMFormat = bakeDesc.format == OmmFormats::OC1_2_STATE ? BakerOmmFormat::OC1_2_State : BakerOmmFormat::OC1_4_State;
    settings.maxSubdivisionLevel = bakeDesc.subdivisionLevel;

    settings.samplerAddressingMode = desc.texture.addressingMode;
    settings.samplerFilterMode = bakeDesc.filter == OmmBakeFilter::Linear ? nri::Filter::LINEAR : nri::Filter::NEAREST;
    settings.maxScratchMemorySize = BakerScratchMemoryBudget::MB_512;

    settings.dynamicSubdivisionScale = bakeDesc.dynamicSubdivisionScale;
    settings.bakeFlags = GetGpuBakeFlags(bakeDesc, pass);
}

void OpacityMicroMapsHelper::GetGpuBakerPrebuildInfo(OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& bakeDesc) {
    for (size_t i = 0; i < count; ++i) {
        InputGeometryDesc gpuBakerDesc = {};
        FillInputGeometryDesc(*queue[i], gpuBakerDesc, bakeDesc, OmmGpuBakerPass::Combined);
        m_GpuBakerIntegration.GetPrebuildInfo(&gpuBakerDesc, 1);

        OmmBakeGeometryDesc::GpuBakerPrebuildInfo& prebuildInfo = queue[i]->gpuBakerPreBuildInfo;
        PrebuildInfo& ommBakerPrebuildInfo = gpuBakerDesc.outputs.prebuildInfo;

        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::ArrayData] = ommBakerPrebuildInfo.arrayDataSize;
        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::DescArray] = ommBakerPrebuildInfo.descArraySize;
        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::Indices] = ommBakerPrebuildInfo.indexBufferSize;
        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::DescArrayHistogram] = ommBakerPrebuildInfo.ommDescArrayHistogramSize;
        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::IndexHistogram] = ommBakerPrebuildInfo.ommIndexHistogramSize;
        prebuildInfo.dataSizes[(uint32_t)OmmDataLayout::GpuPostBuildInfo] = ommBakerPrebuildInfo.postBuildInfoSize;

        memcpy(prebuildInfo.transientBufferSizes, ommBakerPrebuildInfo.transientBufferSizes, sizeof(uint64_t) * OMM_MAX_TRANSIENT_POOL_BUFFERS);

        queue[i]->outOmmIndexFormat = ommBakerPrebuildInfo.indexFormat;
        queue[i]->outOmmIndexStride = nri::nriGetFormatProps(ommBakerPrebuildInfo.indexFormat)->stride;
        queue[i]->outDescArrayHistogramCount = uint32_t(ommBakerPrebuildInfo.ommDescArrayHistogramSize / (uint64_t)sizeof(ommCpuOpacityMicromapUsageCount));
        queue[i]->outIndexHistogramCount = uint32_t(ommBakerPrebuildInfo.ommIndexHistogramSize / (uint64_t)sizeof(ommCpuOpacityMicromapUsageCount));
    }
}

void OpacityMicroMapsHelper::BakeOpacityMicroMapsGpu(nri::CommandBuffer* commandBuffer, OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& bakeDesc, OmmGpuBakerPass pass) {
    std::vector<InputGeometryDesc> gpuBakerDescs(count);
    for (size_t i = 0; i < count; ++i)
        FillInputGeometryDesc(*queue[i], gpuBakerDescs[i], bakeDesc, pass);

    m_GpuBakerIntegration.Bake(*commandBuffer, gpuBakerDescs.data(), (uint32_t)gpuBakerDescs.size());
}

void OpacityMicroMapsHelper::GpuPostBakeCleanUp() {
    m_GpuBakerIntegration.ReleaseTemporalResources();
}

#pragma endregion

#pragma region[ Geometry Builder ]

void OpacityMicroMapsHelper::GetBlasPrebuildInfo(MaskedGeometryBuildDesc** queue, const size_t count) {
    if (m_DisableGeometryBuild)
        return;

    if (NRI.GetDeviceDesc(*m_Device).graphicsAPI == nri::GraphicsAPI::D3D12)
        GetPreBuildInfoD3D12(queue, count);
    else
        GetPreBuildInfoVK(queue, count);
}

nri::Buffer* OpacityMicroMapsHelper::CreateBuildBuffer(uint64_t size, nri::BufferUsageBits usage) {
    nri::Buffer* buffer = nullptr;
    const nri::BufferDesc desc = {size, 0, usage};
    if (NRI.CreateCommittedBuffer(*m_Device, nri::MemoryLocation::DEVICE, 0.0f, desc, buffer) != nri::Result::SUCCESS) {
        fprintf(stderr, "[FAIL] Cannot allocate OMM build buffer (%llu bytes)\n", (unsigned long long)size);
        std::abort();
    }
    return buffer;
}

void OpacityMicroMapsHelper::BuildMaskedGeometry(MaskedGeometryBuildDesc** queue, const size_t count, nri::CommandBuffer* commandBuffer) {
    if (m_DisableGeometryBuild || count == 0)
        return;

    GetBlasPrebuildInfo(queue, count);
    uint64_t scratchSize = 0;
    for (size_t i = 0; i < count; ++i)
        scratchSize = std::max(scratchSize, queue[i]->prebuildInfo.maxScratchDataSize);

    if (scratchSize > (m_ScratchBuffer ? NRI.GetBufferDesc(*m_ScratchBuffer).size : 0)) {
        if (m_ScratchBuffer)
            NRI.DestroyBuffer(m_ScratchBuffer);
        m_ScratchBuffer = CreateBuildBuffer(scratchSize, nri::BufferUsageBits::SCRATCH);
        m_ScratchAddress = NRI.GetBufferDeviceAddress(*m_ScratchBuffer);
    }

    if (NRI.GetDeviceDesc(*m_Device).graphicsAPI == nri::GraphicsAPI::D3D12)
        BuildMaskedGeometryD3D12(queue, count, commandBuffer);
    else
        BuildMaskedGeometryVK(queue, count, commandBuffer);
}

#pragma endregion

#pragma region[ OMM Caching ]

uint64_t OmmCaching::CalculateSateHash(const OmmBakeDesc& bakeDesc) {
    struct CommonState { // leave only those parameters of OmmBakeDesc that contribute to state uniqueness
        uint32_t subdivisionLevel;
        uint32_t mipBias;
        uint32_t filter;
        uint32_t format;
        uint32_t type;
        float dynamicSubdivisionScale;

        void InitCommon(const OmmBakeDesc& bakeDesc) {
            subdivisionLevel = bakeDesc.subdivisionLevel;
            mipBias = bakeDesc.mipBias;
            dynamicSubdivisionScale = bakeDesc.dynamicSubdivisionScale;
            filter = (uint32_t)bakeDesc.filter;
            format = (uint32_t)bakeDesc.format;
            type = (uint32_t)bakeDesc.type;
        }
    };

    struct GpuState : public CommonState {
        GpuBakerFlags gpuFlags;

        void Init(const OmmBakeDesc& bakeDesc) {
            InitCommon(bakeDesc);
            gpuFlags = bakeDesc.gpuFlags;
        }
    };

    struct CpuState : public CommonState {
        CpuBakerFlags cpuFlags;
        uint32_t mipCount;

        void Init(const OmmBakeDesc& bakeDesc) {
            InitCommon(bakeDesc);
            cpuFlags = bakeDesc.cpuFlags;
            mipCount = bakeDesc.mipCount;
        }
    };

    GpuState gpuState = {};
    CpuState cpuState = {};
    memset(&gpuState, 0, sizeof(GpuState));
    memset(&cpuState, 0, sizeof(CpuState));
    gpuState.Init(bakeDesc);
    cpuState.Init(bakeDesc);

    const uint8_t* p = (bakeDesc.type == OmmBakerType::GPU) ? (uint8_t*)&gpuState : (uint8_t*)&cpuState;
    size_t len = (bakeDesc.type == OmmBakerType::GPU) ? sizeof(GpuState) : sizeof(CpuState);

    constexpr uint64_t fnvPrime = 1099511628211ull;
    uint64_t result = 14695981039346656037ull;
    while (len--)
        result = (result ^ (*p++)) * fnvPrime;
    if (bakeDesc.type == OmmBakerType::GPU && bakeDesc.mipBias != 0) {
        // Older GPU bakes used the base mip dimensions with the selected mip view.
        constexpr uint8_t gpuMipBakeVersion = 1;
        result = (result ^ gpuMipBakeVersion) * fnvPrime;
    }
    return result;
}

bool OmmCaching::MaskHeader::Fits(uint64_t available) const {
    if (blobSize > available)
        return false;

    uint64_t remaining = blobSize;
    for (uint64_t size : sizes) {
        if (size == 0 || size > remaining)
            return false;
        remaining -= size;
    }
    return remaining == 0;
}

#ifdef _WIN32
static constexpr auto SeekCacheFile = _fseeki64;
static constexpr auto TellCacheFile = _ftelli64;
#else
static constexpr auto SeekCacheFile = fseeko;
static constexpr auto TellCacheFile = ftello;
#endif

static FILE* OpenCacheFile(const char* filename, bool writable) {
    const char* mode = writable ? "a+b" : "rb";
#ifdef _WIN32
    return _fsopen(filename, mode, _SH_DENYWR);
#else
    FILE* file = fopen(filename, mode);
    if (file && flock(fileno(file), (writable ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0) {
        fclose(file);
        return nullptr;
    }
    return file;
#endif
}

static bool TrimCacheFile(FILE* file, uint64_t size) {
#ifdef _WIN32
    return _chsize_s(_fileno(file), size) == 0;
#else
    return ftruncate(fileno(file), size) == 0;
#endif
}

void OmmCaching::Open(const std::string& filename, const OmmBakeDesc& bakeDesc) {
    m_Filename = filename;
    m_StateHash = CalculateSateHash(bakeDesc);
    m_ValidSize = 0;
    m_MaskOffsets.clear();

    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(filename).parent_path(), error);
    m_File.reset();
    m_File.reset(OpenCacheFile(filename.c_str(), true));
    m_Writable = (bool)m_File;
    if (!m_File)
        m_File.reset(OpenCacheFile(filename.c_str(), false));
    FILE* file = m_File.get();
    if (!file)
        return;
    if (SeekCacheFile(file, 0, SEEK_END) != 0) {
        m_Writable = false;
        return;
    }
    const auto end = TellCacheFile(file);
    if (end < 0) {
        m_Writable = false;
        return;
    }

    const uint64_t fileSize = (uint64_t)end;
    while (fileSize - m_ValidSize >= sizeof(MaskHeader)) {
        MaskHeader header = {};
        if (SeekCacheFile(file, m_ValidSize, SEEK_SET) != 0 || fread(&header, sizeof(header), 1, file) != 1) {
            m_Writable = false;
            return;
        }
        if (!header.Fits(fileSize - m_ValidSize - sizeof(header)))
            break;

        if (header.stateHash == m_StateHash)
            m_MaskOffsets.emplace(header.instanceHash, m_ValidSize);
        m_ValidSize += sizeof(header) + header.blobSize;
    }

    if (m_ValidSize != fileSize && m_Writable && !TrimCacheFile(file, m_ValidSize)) {
        m_Writable = false;
        printf("[OMM] Unable to trim incomplete cache record in '%s'.\n", filename.c_str());
    }
}

bool OmmCaching::HasMask(uint64_t hash) const {
    return m_MaskOffsets.count(hash) != 0;
}

bool OmmCaching::ReadMask(uint64_t hash, OmmBakeGeometryDesc& geometry, nri::Format& indexFormat) const {
    const auto it = m_MaskOffsets.find(hash);
    if (it == m_MaskOffsets.end())
        return false;

    FILE* file = m_File.get();
    const uint64_t offset = it->second;
    MaskHeader header = {};
    if (SeekCacheFile(file, offset, SEEK_SET) != 0 || fread(&header, sizeof(header), 1, file) != 1)
        return false;
    if (header.stateHash != m_StateHash || header.instanceHash != hash || !header.Fits(m_ValidSize - offset - sizeof(header)))
        return false;

    for (uint32_t i = 0; i < (uint32_t)OmmDataLayout::CpuMaxNum; ++i) {
        geometry.outData[i].resize(header.sizes[i]);
        if (fread(geometry.outData[i].data(), 1, header.sizes[i], file) != header.sizes[i]) {
            for (auto& data : geometry.outData)
                data.clear();
            return false;
        }
    }

    indexFormat = (nri::Format)header.ommIndexFormat;
    return true;
}

void OmmCaching::SaveMask(uint64_t hash, const OmmBakeGeometryDesc& geometry) {
    if (!m_Writable || HasMask(hash))
        return;

    MaskHeader header = {};
    header.stateHash = m_StateHash;
    header.instanceHash = hash;
    header.ommIndexFormat = (uint16_t)geometry.outOmmIndexFormat;
    for (uint32_t i = 0; i < (uint32_t)OmmDataLayout::CpuMaxNum; ++i) {
        if (geometry.outData[i].empty())
            return;
        header.sizes[i] = geometry.outData[i].size();
        header.blobSize += header.sizes[i];
    }

    FILE* file = m_File.get();
    if (SeekCacheFile(file, 0, SEEK_END) != 0 || TellCacheFile(file) != (int64_t)m_ValidSize) {
        m_Writable = false;
        printf("[OMM] Unable to append to cache '%s'.\n", m_Filename.c_str());
        return;
    }

    bool written = fwrite(&header, sizeof(header), 1, file) == 1;
    for (uint32_t i = 0; written && i < (uint32_t)OmmDataLayout::CpuMaxNum; ++i)
        written = fwrite(geometry.outData[i].data(), 1, header.sizes[i], file) == header.sizes[i];
    if (!written || fflush(file) != 0) {
        m_Writable = false;
        printf("[OMM] Unable to finish writing cache record in '%s'.\n", m_Filename.c_str());
        return;
    }

    m_MaskOffsets.emplace(hash, m_ValidSize);
    m_ValidSize += sizeof(header) + header.blobSize;
}

#pragma endregion
} // namespace ommhelper