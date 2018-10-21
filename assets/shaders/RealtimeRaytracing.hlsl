﻿#ifndef REALTIME_RAYTRACING_HLSL
#define REALTIME_RAYTRACING_HLSL

#define HLSL
#include "RaytracingHlslCompat.h"

#include "RaytracingUtils.hlsli"

#define RAY_MAX_T 1.0e+38f
#define RAY_EPSILON 0.0001

#define MAX_RADIANCE_RAY_DEPTH 1
#define MAX_SHADOW_RAY_DEPTH 2

////////////////////////////////////////////////////////////////////////////////
// Global root signature
////////////////////////////////////////////////////////////////////////////////

RWTexture2D<float4> gDirectLightingOutput : register(u0);
RWTexture2D<float4> gIndirectSpecularOutput : register(u1);
RaytracingAccelerationStructure SceneBVH : register(t0);
ConstantBuffer<PerFrameConstants> perFrameConstants : register(b0);

SamplerState defaultSampler : register(s0);

////////////////////////////////////////////////////////////////////////////////
// Hit-group local root signature
////////////////////////////////////////////////////////////////////////////////

// StructuredBuffer indexing is not supported in compute path of Fallback Layer,
// must use typed buffer or raw buffer in compute path.
#define USE_STRUCTURED_VERTEX_BUFFER 0
#if USE_STRUCTURED_VERTEX_BUFFER
StructuredBuffer<Vertex> vertexBuffer : register(t0, space1);
#else
Buffer<float3> vertexBuffer : register(t0, space1);
#endif

ByteAddressBuffer indexBuffer : register(t1, space1);

cbuffer MaterialConstants : register(b0, space1)
{
    MaterialParams materialParams;
}

////////////////////////////////////////////////////////////////////////////////
// Miss shader local root signature
////////////////////////////////////////////////////////////////////////////////

TextureCube envCubemap : register(t0, space2);

////////////////////////////////////////////////////////////////////////////////
// Ray-gen shader
////////////////////////////////////////////////////////////////////////////////

struct ShadingAOV
{
    float3 albedo;
    float roughness;
    float3 directLighting;
    float3 indirectSpecular;
};

struct RealtimePayload
{
    float3 color;
    float distance;
    ShadingAOV aov;
    uint depth;
};

[shader("raygeneration")] 
void RayGen() 
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.f - 1.f);
 
    RealtimePayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0.0;
    payload.depth = 0;

    float2 jitter = perFrameConstants.cameraParams.jitters * 2.0;

    RayDesc ray;
    ray.Origin = perFrameConstants.cameraParams.worldEyePos.xyz + float3(jitter.x, jitter.y, 0.0f);
    ray.Direction = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;
    ray.TMin = 0;
    ray.TMax = RAY_MAX_T;

    TraceRay(SceneBVH, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

    gDirectLightingOutput[launchIndex] = float4(max(payload.aov.directLighting, 0.0), 1.0f);
    gIndirectSpecularOutput[launchIndex] = float4(max(payload.aov.indirectSpecular, 0.0), 1.0f);
}

////////////////////////////////////////////////////////////////////////////////
// Hit groups
////////////////////////////////////////////////////////////////////////////////

void interpolateVertexAttributes(float2 bary, out float3 vertPosition, out float3 vertNormal)
{
    float3 barycentrics = float3(1.f - bary.x - bary.y, bary.x, bary.y);

    uint baseIndex = PrimitiveIndex() * 3;
    int address = baseIndex * 4;
    const uint3 indices = Load3x32BitIndices(address, indexBuffer);

    const uint strideInFloat3s = 2;
    const uint positionOffsetInFloat3s = 0;
    const uint normalOffsetInFloat3s = 1;

#if USE_STRUCTURED_VERTEX_BUFFER
    vertPosition = vertexBuffer[indices[0]].position * barycentrics.x +
                   vertexBuffer[indices[1]].position * barycentrics.y +
                   vertexBuffer[indices[2]].position * barycentrics.z;

    vertNormal = vertexBuffer[indices[0]].normal * barycentrics.x +
                 vertexBuffer[indices[1]].normal * barycentrics.y +
                 vertexBuffer[indices[2]].normal * barycentrics.z;
#else
    vertPosition = vertexBuffer[indices[0] * strideInFloat3s + positionOffsetInFloat3s] * barycentrics.x +
                   vertexBuffer[indices[1] * strideInFloat3s + positionOffsetInFloat3s] * barycentrics.y +
                   vertexBuffer[indices[2] * strideInFloat3s + positionOffsetInFloat3s] * barycentrics.z;
 
    vertNormal = vertexBuffer[indices[0] * strideInFloat3s + normalOffsetInFloat3s] * barycentrics.x +
                 vertexBuffer[indices[1] * strideInFloat3s + normalOffsetInFloat3s] * barycentrics.y +
                 vertexBuffer[indices[2] * strideInFloat3s + normalOffsetInFloat3s] * barycentrics.z;
#endif
}

float shootShadowRay(float3 orig, float3 dir, float minT, float maxT, uint currentDepth)
{
    if (currentDepth >= MAX_SHADOW_RAY_DEPTH) {
        return 1.0;
    }

    RayDesc ray = { orig, minT, dir, maxT };

    ShadowPayload payload = { 0.0 };

    TraceRay(SceneBVH, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xFF, 1, 0, 1, ray, payload);
    return payload.lightVisibility;
}

float3 shootSecondaryRay(float3 orig, float3 dir, float minT, uint currentDepth)
{
    if (currentDepth >= MAX_RADIANCE_RAY_DEPTH) {
        return float3(0.0, 0.0, 0.0);
    }

    RayDesc ray = { orig, minT, dir, RAY_MAX_T };

    RealtimePayload payload;
    payload.color = float3(0, 0, 0);
    payload.distance = 0.0;
    payload.depth = currentDepth + 1;

    TraceRay(SceneBVH, 0, 0xFF, 0, 0, 2, ray, payload);
    return payload.color;
}

float evaluateAO(float3 position, float3 normal)
{
    float visibility = 0.0f;
    const int aoRayCount = 4;

    uint2 pixIdx = DispatchRaysIndex().xy;
    uint2 numPix = DispatchRaysDimensions().xy;
    uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);

    for (int i = 0; i < aoRayCount; ++i) {
        float3 sampleDir;
        float NoL;
        float pdf;
        if (perFrameConstants.options.cosineHemisphereSampling) {
            sampleDir = getCosHemisphereSample(randSeed, normal);
            NoL = saturate(dot(normal, sampleDir));
            pdf = NoL / M_PI;
        } else {
            sampleDir = getUniformHemisphereSample(randSeed, normal);
            NoL = saturate(dot(normal, sampleDir));
            pdf = 1.0 / (2.0 * M_PI);
        }
        visibility += shootShadowRay(position, sampleDir, RAY_EPSILON, 10.0, 1) * NoL / pdf;
    }

    return visibility / float(aoRayCount);
}

float3 evaluateDirectionalLight(float3 position, float3 normal, uint currentDepth)
{
    float3 L = normalize(-perFrameConstants.directionalLight.forwardDir.xyz);
    float NoL = saturate(dot(normal, L));

    float visible = shootShadowRay(position, L, RAY_EPSILON, RAY_MAX_T, currentDepth);

    return perFrameConstants.directionalLight.color.rgb * perFrameConstants.directionalLight.color.a * NoL * visible;
}

float3 evaluatePointLight(float3 position, float3 normal, uint currentDepth)
{
    float3 lightPath = perFrameConstants.pointLight.worldPos.xyz - position;
    float lightDistance = length(lightPath);
    float3 L = normalize(lightPath);
    float NoL = saturate(dot(normal, L));

    float visible = shootShadowRay(position, L, RAY_EPSILON, lightDistance - RAY_EPSILON, currentDepth);

    float falloff = 1.0 / (2 * M_PI * lightDistance * lightDistance);
    return perFrameConstants.pointLight.color.rgb * perFrameConstants.pointLight.color.a * NoL * visible * falloff;
}

float3 evaluateIndirectDiffuse(float3 position, float3 normal, inout uint randSeed, uint currentDepth)
{
    float3 color = 0.0;
    const int rayCount = 1;

    for (int i = 0; i < rayCount; ++i) {
        if (perFrameConstants.options.cosineHemisphereSampling) {
            float3 sampleDir = getCosHemisphereSample(randSeed, normal);
            // float NoL = saturate(dot(normal, sampleDir));
            // float pdf = NoL / M_PI;
            // color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * NoL / pdf; 
            color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * M_PI; // term canceled
        } else {
            float3 sampleDir = getUniformHemisphereSample(randSeed, normal);
            float NoL = saturate(dot(normal, sampleDir));
            float pdf = 1.0 / (2.0 * M_PI); 
            color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * NoL / pdf;
        }
    }

    return color / float(rayCount);
}

float3 shadeAOV(float3 position, float3 normal, uint currentDepth, out ShadingAOV aov)
{
    // Set up random seeed
    uint2 pixIdx = DispatchRaysIndex().xy;
    uint2 numPix = DispatchRaysDimensions().xy;
    uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);

    // Calculate direct diffuse lighting
    float3 directContrib = 0.0;
    directContrib += evaluateDirectionalLight(position, normal, currentDepth);
    directContrib += evaluatePointLight(position, normal, currentDepth);

    // Accumulate indirect specular
    float fresnel = 0.0;
    float3 specularComponent = 0.0;
    if (materialParams.type == 1 || materialParams.type == 2) {
        if (materialParams.reflectivity > 0.001) {
            float exponent = exp((1.0 - materialParams.roughness) * 12.0);
            float pdf;
            float brdf;
            float3 mirrorDir = reflect(WorldRayDirection(), normal);
            float3 sampleDir = samplePhongLobe(randSeed, mirrorDir, exponent, pdf, brdf);
            float3 reflectionColor = shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth);
            specularComponent += reflectionColor * brdf / pdf;

            // Equivalent to: fresnel = FresnelReflectanceSchlick(-V, H, materialParams.specular);
            fresnel = FresnelReflectanceSchlick(WorldRayDirection(), normal, materialParams.specular);
        }
    }

    if (currentDepth == 0) {
        aov.albedo = materialParams.albedo;
        aov.roughness = materialParams.roughness;
        aov.directLighting = materialParams.albedo * directContrib / M_PI;
        aov.indirectSpecular = fresnel * materialParams.specular * materialParams.reflectivity * specularComponent;
    }

    return materialParams.albedo * directContrib / M_PI + fresnel * materialParams.specular * materialParams.reflectivity * specularComponent;
}

// Hit group 1

[shader("closesthit")] 
void PrimaryClosestHit(inout RealtimePayload payload, Attributes attrib) 
{
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    ShadingAOV shadingResult;
    float3 color = shadeAOV(HitWorldPosition(), normalize(vertNormal), payload.depth, shadingResult);

    payload.color = color;
    payload.distance = RayTCurrent();
    payload.aov = shadingResult;
}

// Hit group 2

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, Attributes attrib)
{
    // no-op
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, Attributes attrib)
{
    // no-op
}

////////////////////////////////////////////////////////////////////////////////
// Miss shader
////////////////////////////////////////////////////////////////////////////////

float3 sampleEnvironment()
{
    float4 envSample = envCubemap.SampleLevel(defaultSampler, WorldRayDirection().xyz, 0.0);
    return envSample.rgb;
}

[shader("miss")]
void PrimaryMiss(inout RealtimePayload payload : SV_RayPayload)
{
    payload.color = sampleEnvironment();
    payload.distance = -1.0;
    payload.aov.directLighting = payload.color;
    payload.aov.indirectSpecular = 0.0;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload : SV_RayPayload)
{
    payload.lightVisibility = 1.0;
}

[shader("miss")]
void SecondaryMiss(inout RealtimePayload payload : SV_RayPayload)
{
    payload.color = sampleEnvironment();
    payload.distance = -1.0;
    payload.aov.indirectSpecular = 0.0;
}

#endif // REALTIME_RAYTRACING_HLSL