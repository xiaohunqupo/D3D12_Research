#include "Common.hlsli"
#include "Random.hlsli"
#include "Lighting.hlsli"
#include "RayTracing/DDGICommon.hlsli"
#include "Noise.hlsli"
#include "GBuffer.hlsli"

Texture2D<uint4> tGBuffer 					: register(t0);
Texture2D<float> tDepth 					: register(t1);
Texture2D tPreviousSceneColor 				: register(t2);
Texture3D<float4> tFog 						: register(t3);
StructuredBuffer<uint> tLightGrid 			: register(t4);
Texture2D<float> tAO						: register(t5);

RWTexture2D<float4> uOutput 				: register(u0);

float3 DoLight(float3 specularColor, float R, float3 diffuseColor, float3 N, float3 V, float3 worldPos, float2 pixel, float linearDepth, float dither)
{
	uint2 tileIndex = uint2(floor(pixel / TILED_LIGHTING_TILE_SIZE));
	uint tileIndex1D = tileIndex.x + DivideAndRoundUp(cView.ViewportDimensions.x, TILED_LIGHTING_TILE_SIZE) * tileIndex.y;
	uint lightGridOffset = tileIndex1D * TILED_LIGHTING_NUM_BUCKETS;

	float3 lighting = 0.0f;
	for(uint bucketIndex = 0; bucketIndex < TILED_LIGHTING_NUM_BUCKETS; ++bucketIndex)
	{
		uint bucket = tLightGrid[lightGridOffset + bucketIndex];
		while(bucket)
		{
			uint bitIndex = firstbitlow(bucket);
			bucket ^= 1u << bitIndex;

			uint lightIndex = bitIndex + bucketIndex * 32;
			Light light = GetLight(lightIndex);
			lighting += DoLight(light, specularColor, diffuseColor, R, N, V, worldPos, linearDepth, dither);
		}
	}
	return lighting;
}


[numthreads(8, 8, 1)]
void ShadeCS(uint3 threadId : SV_DispatchThreadID)
{
	uint2 texel = threadId.xy;
	if(any(texel >= cView.ViewportDimensions))
		return;

	float2 uv = TexelToUV(texel, cView.ViewportDimensionsInv);
	float depth = tDepth[texel];
	float3 viewPos = ViewPositionFromDepth(uv, depth, cView.ClipToView);
	float3 worldPos = mul(float4(viewPos, 1), cView.ViewToWorld).xyz;
	float linearDepth = viewPos.z;

	MaterialProperties surface = (MaterialProperties)0;
	GBufferData gbuffer = LoadGBuffer(tGBuffer[texel]);
	surface.BaseColor = gbuffer.BaseColor;
	surface.Specular = gbuffer.Specular;
	surface.Normal = gbuffer.Normal;
	surface.Roughness = gbuffer.Roughness;
	surface.Metalness = gbuffer.Metalness;
	surface.Emissive = gbuffer.Emissive;

	float ambientOcclusion = tAO.SampleLevel(sLinearClamp, uv, 0);
	float dither = InterleavedGradientNoise(texel);

	BrdfData brdfData = GetBrdfData(surface);

	float3 V = normalize(cView.ViewLocation - worldPos);
	float ssrWeight = 0;
	float3 ssr = ScreenSpaceReflections(worldPos, surface.Normal, V, brdfData.Roughness, tDepth, tPreviousSceneColor, dither, ssrWeight);

	float3 lighting = 0;
	lighting += DoLight(brdfData.Specular, brdfData.Roughness, brdfData.Diffuse, surface.Normal, V, worldPos, texel, linearDepth, dither);
	lighting += ambientOcclusion * Diffuse_Lambert(brdfData.Diffuse) * SampleDDGIIrradiance(worldPos, surface.Normal, -V);
	lighting += ssr;
	lighting += surface.Emissive;

	float fogSlice = sqrt((linearDepth - cView.FarZ) / (cView.NearZ - cView.FarZ));
	float4 scatteringTransmittance = tFog.SampleLevel(sLinearClamp, float3(uv, fogSlice), 0);
	lighting = lighting * scatteringTransmittance.w + scatteringTransmittance.rgb;

	uOutput[texel] = float4(lighting, surface.Opacity);
}
