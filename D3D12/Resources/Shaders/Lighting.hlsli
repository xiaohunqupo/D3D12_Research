#include "Common.hlsli"
#include "ShadingModels.hlsli"
#include "CommonBindings.hlsli"

#define MAX_SHADOW_CASTERS 32
cbuffer LightData : register(b2)
{
	float4x4 cLightViewProjections[MAX_SHADOW_CASTERS];
	float4 cShadowMapOffsets[MAX_SHADOW_CASTERS];
	float4 cCascadeDepths;
	uint cNumCascades;
}

// Angle >= Umbra -> 0
// Angle < Penumbra -> 1
//Gradient between Umbra and Penumbra
float DirectionalAttenuation(float3 L, float3 direction, float cosUmbra, float cosPenumbra)
{
	float cosAngle = dot(-normalize(L), direction);
	float falloff = saturate((cosAngle - cosUmbra) / (cosPenumbra - cosUmbra));
	return falloff * falloff;
}

//Distance between rays is proportional to distance squared
//Extra windowing function to make light radius finite
//https://blog.selfshadow.com/publications/s2013-shading-course/karis/s2013_pbs_epic_notes_v2.pdf
float RadialAttenuation(float3 L, float range)
{
	float distSq = dot(L, L);
	float distanceAttenuation = 1 / (distSq + 1);
	float windowing = Square(saturate(1 - Square(distSq * Square(rcp(range)))));
	return distanceAttenuation * windowing;
}

float3 TangentSpaceNormalMapping(float3 sampledNormal, float3x3 TBN, float2 tex, bool invertY)
{
	sampledNormal.xy = sampledNormal.xy * 2.0f - 1.0f;

//#define NORMAL_BC5
#ifdef NORMAL_BC5
	sampledNormal.z = sqrt(saturate(1.0f - dot(sampledNormal.xy, sampledNormal.xy)));
#endif
	if(invertY)
	{
		sampledNormal.x = -sampledNormal.x;
	}
	sampledNormal = normalize(sampledNormal);
	return mul(sampledNormal, TBN);
}

float2 TransformShadowTexCoord(float2 texCoord, int shadowMapIndex)
{
	float2 shadowMapStart = cShadowMapOffsets[shadowMapIndex].xy;
	float2 normalizedShadowMapSize = cShadowMapOffsets[shadowMapIndex].zw;
	return shadowMapStart + float2(texCoord.x * normalizedShadowMapSize.x, texCoord.y * normalizedShadowMapSize.y); 
}

float DoShadow(float3 wPos, int shadowMapIndex, float invShadowSize)
{
	float4x4 lightViewProjection = cLightViewProjections[shadowMapIndex];
	float4 lightPos = mul(float4(wPos, 1), lightViewProjection);
	lightPos.xyz /= lightPos.w;
	lightPos.x = lightPos.x / 2.0f + 0.5f;
	lightPos.y = lightPos.y / -2.0f + 0.5f;

	float2 texCoord = lightPos.xy;

	Texture2D shadowTexture = tShadowMapTextures[NonUniformResourceIndex(shadowMapIndex)];
	
	const float Dilation = 2.0f;
    float d1 = Dilation * invShadowSize * 0.125f;
    float d2 = Dilation * invShadowSize * 0.875f;
    float d3 = Dilation * invShadowSize * 0.625f;
    float d4 = Dilation * invShadowSize * 0.375f;
    float result = (
        2.0f * shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord, lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2(-d2,  d1), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2(-d1, -d2), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2( d2, -d1), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2( d1,  d2), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2(-d4,  d3), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2(-d3, -d4), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2( d4, -d3), lightPos.z) +
        shadowTexture.SampleCmpLevelZero(sShadowMapSampler, texCoord + float2( d3,  d4), lightPos.z)
        ) / 10.0f;
    return result * result;
}

float GetAttenuation(Light light, float3 wPos)
{
	float attentuation = 1.0f;

	if(light.Type >= LIGHT_POINT)
	{
		float3 L = light.Position - wPos;
		attentuation *= RadialAttenuation(L, light.Range);
		if(light.Type >= LIGHT_SPOT)
		{
			attentuation *= DirectionalAttenuation(L, light.Direction, light.SpotlightAngles.y, light.SpotlightAngles.x);
		}
	}

	return attentuation;
}

float3 ApplyAmbientLight(float3 diffuse, float ao, float3 lightColor)
{
    return ao * diffuse * lightColor;
}

uint GetShadowIndex(Light light, float4 pos, float3 wPos)
{
	int shadowIndex = light.ShadowIndex;
	if(light.Type == LIGHT_DIRECTIONAL)
	{
		float4 splits = pos.w > cCascadeDepths;
		float4 cascades = cCascadeDepths > 0;
		int cascadeIndex = dot(splits, cascades);

	#define FADE_SHADOW_CASCADES 1
	#define FADE_THRESHOLD 0.1f
	#if FADE_SHADOW_CASCADES
			float nextSplit = cCascadeDepths[cascadeIndex];
			float splitRange = cascadeIndex == 0 ? nextSplit : nextSplit - cCascadeDepths[cascadeIndex - 1];
			float fadeFactor = (nextSplit - pos.w) / splitRange;
			if(fadeFactor <= FADE_THRESHOLD && cascadeIndex != cNumCascades - 1)
			{
				float lerpAmount = smoothstep(0.0f, FADE_THRESHOLD, fadeFactor);
				float dither = InterleavedGradientNoise(pos.xy);
				if(lerpAmount < dither)
				{
					cascadeIndex++;
				}
			}
	#endif

		shadowIndex += cascadeIndex;
	}
	else if(light.Type == LIGHT_POINT)
	{
		shadowIndex += GetCubeFaceIndex(wPos - light.Position);
	}
	return shadowIndex;
}

LightResult DoLight(Light light, float3 specularColor, float3 diffuseColor, float roughness, float4 pos, float3 wPos, float3 N, float3 V)
{
	LightResult result = (LightResult)0;

	float attenuation = GetAttenuation(light, wPos);
	if(attenuation <= 0)
	{
		return result;
	}

	float visibility = 1.0f;
	if(light.ShadowIndex >= 0)
	{
#define INLINE_RT_SHADOWS 0
#if INLINE_RT_SHADOWS
		RayDesc ray;
		ray.Origin = wPos + N * 0.01f;
		ray.Direction = light.Position - wPos;
		ray.TMin = 0.001;
		ray.TMax = 1;

		RayQuery<RAY_FLAG_NONE> q;

		q.TraceRayInline(
			tAccelerationStructure,
			RAY_FLAG_NONE,
			~0,
			ray);
		q.Proceed();

		if(q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		{
			visibility = 0;
		}
#else
		int shadowIndex = GetShadowIndex(light, pos, wPos);

#define VISUALIZE_CASCADES 0
#if VISUALIZE_CASCADES
		if(light.Type == LIGHT_DIRECTIONAL)
		{
			static float4 COLORS[4] = {
				float4(1,0,0,1),
				float4(0,1,0,1),
				float4(0,0,1,1),
				float4(1,0,1,1),
			};
			result.Diffuse += 0.4f * COLORS[shadowIndex - light.ShadowIndex].xyz;
			result.Specular = 0;
			return result;
		}
#endif

		visibility = DoShadow(wPos, shadowIndex, light.InvShadowSize);
		if(visibility <= 0)
		{
			return result;
		}
#endif
	}

	float3 L = normalize(light.Position - wPos);
	result = DefaultLitBxDF(specularColor, roughness, diffuseColor, N, V, L, attenuation);

	float4 color = light.GetColor();
	result.Diffuse *= color.rgb * light.Intensity * visibility;
	result.Specular *= color.rgb * light.Intensity * visibility;

	return result;
}

#define G_SCATTERING 0.0001f
float ComputeScattering(float LoV)
{
	float result = 1.0f - G_SCATTERING * G_SCATTERING;
	result /= (4.0f * PI * pow(1.0f + G_SCATTERING * G_SCATTERING - (2.0f * G_SCATTERING) * LoV, 1.5f));
	return result;
}

float3 ApplyVolumetricLighting(float3 cameraPos, float3 worldPos, float4 pos, float4x4 view, Light light, int samples, int frame)
{
	const float fogValue = 0.3f;
	float3 rayVector = cameraPos - worldPos;
	float3 rayStep = rayVector / samples;
	float3 accumFog = 0.0f.xxx;
	float3 currentPosition = worldPos;
		
	float ditherValue = InterleavedGradientNoise(pos.xy, frame);
	currentPosition += rayStep * ditherValue;

	for(int i = 0; i < samples; ++i)
	{
		float visibility = 1.0f;
		if(light.ShadowIndex >= 0)
		{
			int shadowMapIndex = GetShadowIndex(light, pos, currentPosition);
			float4x4 lightViewProjection = cLightViewProjections[shadowMapIndex];
			float4 lightPos = mul(float4(currentPosition, 1), lightViewProjection);
			lightPos.xyz /= lightPos.w;
			lightPos.x = lightPos.x / 2.0f + 0.5f;
			lightPos.y = lightPos.y / -2.0f + 0.5f;
			visibility = tShadowMapTextures[shadowMapIndex].SampleCmpLevelZero(sShadowMapSampler, lightPos.xy, lightPos.z);
		}
		accumFog += visibility * fogValue * ComputeScattering(dot(rayVector, light.Direction)).xxx * light.GetColor().rgb * light.Intensity;
		currentPosition += rayStep;
	}
	accumFog /= samples;
	return accumFog;
}