// © 2024 NVIDIA Corporation

#include "Include/Shared.hlsli"
#include "Include/RaytracingShared.hlsli"

[numthreads( LINEAR_BLOCK_SIZE, 1, 1 )]
void main( uint threadIndex : SV_DispatchThreadID )
{
    SharcParameters sharcParams = GetSharcParameters( );

    SharcResolveParameters sharcResolveParameters;
    sharcResolveParameters.cameraPositionPrev = gCameraGlobalPosPrev.xyz;
    sharcResolveParameters.accumulationFrameNum = gMaxAccumulatedFrameNum;
    sharcResolveParameters.responsiveFrameNum = gMaxAccumulatedFrameNum;
    sharcResolveParameters.staleFrameNumMax = SHARC_STALE_FRAME_NUM_MIN;
    sharcResolveParameters.frameIndex = gFrameIndex;

    SharcResolveEntry( threadIndex, sharcParams, sharcResolveParameters );
}
