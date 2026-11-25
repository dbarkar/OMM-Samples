/*
Copyright (c) 2022, NVIDIA CORPORATION. All rights reserved.

NVIDIA CORPORATION and its licensors retain all intellectual property
and proprietary rights in and to this software, related documentation
and any modifications thereto. Any use, reproduction, disclosure or
distribution of this software and related documentation without an express
license agreement from NVIDIA CORPORATION is strictly prohibited.
*/

#pragma once

#include <dxgi1_6.h>
#include "build/native/include/d3d12.h"

#include <vulkan/vulkan.h>
#include <array>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "NRI.h"

#include "Extensions/NRIDeviceCreation.h"
#include "Extensions/NRIHelper.h"
#include "Extensions/NRIRayTracing.h"
#include "Extensions/NRIWrapperD3D12.h"
#include "Extensions/NRIWrapperVK.h"

#define OMM_SUPPORTS_CPP17 (1)
#include "omm.h"

#include "OmmBakerIntegration.h"

namespace ommhelper {
enum class OmmFormats {
    OC1_2_STATE,
    OC1_4_STATE,
    Count
};

enum class OmmBakeFilter {
    Nearest = (uint32_t)ommTextureFilterMode_Nearest,
    Linear = (uint32_t)ommTextureFilterMode_Linear,
    Count,
};

enum class OmmBakerType {
    GPU,
    CPU,
    Count
};

struct CpuBakerFlags {
    bool enableInternalThreads = true;
    bool enableSpecialIndices = true;
    bool enableDuplicateDetection = true;
    bool enableNearDuplicateDetection = false;
    bool force32bitIndices = false;
    bool allow8bitIndices = false;
};

struct GpuBakerFlags {
    bool enablePostBuildInfo = true;
    bool enableSpecialIndices = true;
    bool enableTexCoordDeduplication = true;
    bool force32bitIndices = false;
    bool computeOnlyWorkload = true;
    bool allow8bitIndices = false;
};

struct OmmBakeDesc {
    uint32_t subdivisionLevel = 9; // 4^N
    uint32_t mipBias = 0;
    uint32_t mipCount = 1;
    uint32_t buildFrameId = 0;
    float dynamicSubdivisionScale = 1.0f;
    OmmBakeFilter filter = OmmBakeFilter::Linear;
    OmmFormats format = OmmFormats::OC1_4_STATE;
    OmmBakerType type = OmmBakerType::GPU;
    CpuBakerFlags cpuFlags;
    GpuBakerFlags gpuFlags;
    bool enableDebugMode = false;
    bool enableCache = false;
};

enum class OmmGpuBakerPass {
    Setup = ommGpuBakeFlags_PerformSetup,
    Bake = ommGpuBakeFlags_PerformBake,
    Combined = Setup | Bake,
};

enum class OmmAlphaMode {
    Test = (uint32_t)ommAlphaMode_Test,
    Blend = (uint32_t)ommAlphaMode_Blend,
    MaxNum = (uint32_t)ommAlphaMode_MAX_NUM,
};

enum class OmmDataLayout {
    ArrayData,
    DescArray,
    Indices,
    DescArrayHistogram,
    IndexHistogram,
    GpuPostBuildInfo,
    MaxNum,
    BlasBuildGpuBuffersNum = DescArrayHistogram,
    CpuMaxNum = GpuPostBuildInfo,
    GpuOutputNum = MaxNum,
};

struct GpuBakerBuffer {
    nri::Buffer* buffer;
    uint64_t bufferSize; // total buffer size
    uint64_t dataSize;
    uint64_t offset;
};

struct InputBuffer {
    union NriBufferOrPtr {
        nri::Buffer* buffer;
        void* ptr;
    } nriBufferOrPtr;

    uint64_t bufferSize; // total buffer size;
    uint64_t offset;
    uint64_t numElements;
    uint64_t stride;
    uint64_t offsetInStruct;
    nri::Format format;
};

#define OMM_MAX_MIP_NUM 16

struct MipDesc {
    union NriTextureOrPtr {
        nri::Texture* texture;
        void* ptr;
    } nriTextureOrPtr;

    uint32_t width;
    uint32_t height;
    uint32_t rowPitch;
};

struct InputTexture {
    MipDesc mips[OMM_MAX_MIP_NUM];

    uint32_t mipOffset;
    uint32_t mipNum;

    uint32_t alphaChannelId;
    nri::Format format;
    nri::AddressMode addressingMode;
};

struct OmmBakeGeometryDesc {
    InputBuffer indices;
    InputBuffer uvs;
    InputTexture texture;

    GpuBakerBuffer gpuBuffers[uint32_t(OmmDataLayout::GpuOutputNum)];
    GpuBakerBuffer transientBuffers[OMM_MAX_TRANSIENT_POOL_BUFFERS];
    GpuBakerBuffer readBackBuffers[uint32_t(OmmDataLayout::GpuOutputNum)];

    std::vector<uint8_t> outData[uint32_t(OmmDataLayout::MaxNum)]; // cpu baker outputs/gpu baker readback for caching

    struct GpuBakerPrebuildInfo {
        uint64_t dataSizes[(uint32_t)OmmDataLayout::GpuOutputNum];
        uint64_t transientBufferSizes[OMM_MAX_TRANSIENT_POOL_BUFFERS];
    } gpuBakerPreBuildInfo;

    float alphaCutoff;
    float borderAlpha;

    uint32_t outIndexHistogramCount;
    uint32_t outDescArrayHistogramCount;
    uint32_t outOmmIndexStride;
    nri::Format outOmmIndexFormat;
    OmmAlphaMode alphaMode;
};

struct MaskedGeometryBuildDesc {
    struct Inputs {
        InputBuffer indices;
        InputBuffer vertices;
        void* descArrayHistogram;
        void* indexHistogram;

        GpuBakerBuffer buffers[uint32_t(OmmDataLayout::BlasBuildGpuBuffersNum)];

        uint64_t ommIndexStride;
        uint32_t descArrayHistogramNum;
        uint32_t indexHistogramNum;
        nri::Format ommIndexFormat;
    } inputs;

    struct PrebuildInfo {
        uint64_t ommArraySize;
        uint64_t blasSize;
        uint64_t maxScratchDataSize;
    } prebuildInfo;

    struct Outputs {
        nri::AccelerationStructure* blas;
        nri::Buffer* ommArray;
    } outputs;
};

struct OmmCaching {
    void Open(const std::string& filename, const OmmBakeDesc& bakeDesc);
    bool HasMask(uint64_t hash) const;
    bool ReadMask(uint64_t hash, OmmBakeGeometryDesc& geometry, nri::Format& indexFormat) const;
    void SaveMask(uint64_t hash, const OmmBakeGeometryDesc& geometry);

private:
    struct MaskHeader {
        uint64_t instanceHash;
        uint64_t stateHash;
        uint64_t sizes[(uint32_t)OmmDataLayout::CpuMaxNum];
        uint64_t blobSize;
        uint16_t ommIndexFormat;

        bool Fits(uint64_t available) const;
    };

    static uint64_t CalculateSateHash(const OmmBakeDesc& bakeDesc);
    std::unique_ptr<FILE, decltype(&fclose)> m_File{nullptr, &fclose};
    std::string m_Filename;
    bool m_Writable = false;
    uint64_t m_StateHash = 0;
    uint64_t m_ValidSize = 0;
    std::map<uint64_t, uint64_t> m_MaskOffsets;
};

class OpacityMicroMapsHelper {
public:
    void Initialize(nri::Device* device, bool disableMaskedGeometryBuild);

    void GetGpuBakerPrebuildInfo(OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& desc);
    void BakeOpacityMicroMapsGpu(nri::CommandBuffer* commandBuffer, OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& bakeDesc, OmmGpuBakerPass pass);
    void GpuPostBakeCleanUp();

    void BakeOpacityMicroMapsCpu(OmmBakeGeometryDesc** queue, const size_t count, const OmmBakeDesc& desc);
    void ConvertUsageCountsToApiFormat(uint8_t* outFormattedBuffer, size_t& outSize, const uint8_t* bakerOutputBuffer, size_t bakerOutputBufferSize);

    void GetBlasPrebuildInfo(MaskedGeometryBuildDesc** queue, const size_t count);
    void BuildMaskedGeometry(MaskedGeometryBuildDesc** queue, const size_t count, nri::CommandBuffer* commandBuffer);
    void DestroyMaskedGeometry(nri::AccelerationStructure* blas, nri::Buffer* ommArray);
    void ReleaseGeometryMemory();

    void Destroy();

private:
    // D3D12:
    void InitializeD3D12();
    void GetPreBuildInfoD3D12(MaskedGeometryBuildDesc** queue, const size_t count);
    void BindResourceToMemoryD3D12(ID3D12Resource*& resource, size_t size);
    void AllocateMemoryD3D12(uint64_t size);
    void ReleaseMemoryD3D12();
    void BuildMaskedGeometryD3D12(MaskedGeometryBuildDesc** queue, const size_t count, nri::CommandBuffer* commandBuffer);
    void BuildOmmArrayD3D12(MaskedGeometryBuildDesc& desc, nri::CommandBuffer* commandBuffer);
    void BuildBlasD3D12(MaskedGeometryBuildDesc& desc, nri::CommandBuffer* commandBuffer);
    ID3D12GraphicsCommandList4* GetD3D12GraphicsCommandList4(nri::CommandBuffer* commandBuffer);

    // VK:
    void InitializeVK();
    VkBuffer GetGeometryBufferVK(uint64_t size);
    void GetPreBuildInfoVK(MaskedGeometryBuildDesc** queue, const size_t count);
    void BindOmmToMemoryVK(VkMicromapEXT& ommArray, size_t size);
    void BindBlasToMemoryVK(VkAccelerationStructureKHR& blas, size_t size);
    void BuildMaskedGeometryVK(MaskedGeometryBuildDesc** queue, const size_t count, nri::CommandBuffer* commandBuffer);
    void BuildOmmArrayVK(MaskedGeometryBuildDesc& desc, nri::CommandBuffer* commandBuffer);
    void BuildBlasVK(MaskedGeometryBuildDesc& desc, nri::CommandBuffer* commandBuffer);
    void DestroyOmmArrayVK(nri::Buffer* ommArray);
    void DestroyBlasVK(nri::AccelerationStructure* blas);
    VkDevice GetVkDevice();

private:
    nri::Buffer* CreateBuildBuffer(uint64_t size, nri::BufferUsageBits usage);

    // internal memory for masked geometry
    // TODO: when micromaps are supported in NRI, move memory managment to the sample main part
    const uint64_t m_DefaultHeapSize = 100 * 1024 * 1024;
    uint64_t m_CurrentHeapOffset = 0;
    nri::Buffer* m_ScratchBuffer = nullptr;
    uint64_t m_ScratchAddress = 0;

    // D3D12:
    std::vector<ID3D12Heap*> m_D3D12GeometryHeaps;
    ID3D12Device5* m_Device5 = nullptr;

    // VK:
    std::vector<nri::Buffer*> m_VkBuffers;

    // common
    struct NriInterface
        : public nri::CoreInterface,
          public nri::RayTracingInterface,
          public nri::HelperInterface,
          public nri::WrapperD3D12Interface,
          public nri::WrapperVKInterface {};

    NriInterface NRI = {};

    OmmBakerGpuIntegration m_GpuBakerIntegration;
    ommBaker m_OmmCpuBaker = 0;
    nri::Device* m_Device;
    bool m_DisableGeometryBuild = false;
};
} // namespace ommhelper
