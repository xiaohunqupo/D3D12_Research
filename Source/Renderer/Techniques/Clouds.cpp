#include "stdafx.h"
#include "Clouds.h"
#include "RHI/Shader.h"
#include "RHI/PipelineState.h"
#include "RHI/RootSignature.h"
#include "RHI/Device.h"
#include "RHI/Texture.h"
#include "RHI/CommandContext.h"
#include "RenderGraph/RenderGraph.h"
#include "Renderer/RenderTypes.h"
#include "Renderer/Renderer.h"

Clouds::Clouds(GraphicsDevice* pDevice)
{
	const char* pCloudShapesShader = "CloudsShapes.hlsl";
	m_pCloudShapeNoisePSO		= pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, pCloudShapesShader, "CloudShapeNoiseCS");
	m_pCloudDetailNoisePSO		= pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, pCloudShapesShader, "CloudDetailNoiseCS");
	m_pCloudHeighDensityLUTPSO	= pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, pCloudShapesShader, "CloudHeightDensityCS");
	m_pCloudsPSO				= pDevice->CreateComputePipeline(GraphicsCommon::pCommonRS, "Clouds.hlsl", "CSMain");

	pDevice->GetShaderManager()->OnShaderEditedEvent().AddLambda([this](Shader*) { m_pShapeNoise = nullptr; });
}

RGTexture* Clouds::Render(RGGraph& graph, const RenderView* pView, RGTexture* pColorTarget, RGTexture* pDepth)
{
	struct CloudParameters
	{
		int32 NoiseSeed		= 0;
		float GlobalScale	= 0.001f;
		float GlobalDensity = 0.1f;

		float RaymarchStepSize = 15.0f;
		int32 LightMarchSteps  = 6;

		int32 ShapeNoiseFrequency  = 4;
		int32 ShapeNoiseResolution = 128;
		float ShapeNoiseScale	   = 0.3f;

		int32 DetailNoiseFrequency	= 3;
		int32 DetailNoiseResolution = 32;
		float DetailNoiseScale		= 3.0f;
		float DetailNoiseInfluence	= 0.4f;

		float WindAngle	   = 0;
		float WindSpeed	   = 0.03f;
		float CloudTopSkew = 10.0f;

		float	Coverage			  = 0.9f;
		float	CloudType			  = 0.5f;
		float	PlanetRadius		  = 60000;
		Vector2 AtmosphereHeightRange = Vector2(200.0f, 900.0f);
	};
	static CloudParameters parameters;

	bool isDirty = !m_pShapeNoise || !m_pDetailNoise || !m_pCloudHeightDensityLUT;

	if (ImGui::Begin("Settings"))
	{
		if (ImGui::CollapsingHeader("Clouds"))
		{
			isDirty |= ImGui::SliderInt("Seed", &parameters.NoiseSeed, 0, 100);
			isDirty |= ImGui::SliderInt("Shape Noise Frequency", &parameters.ShapeNoiseFrequency, 1, 10);
			isDirty |= ImGui::SliderInt("Shape Noise Resolution", &parameters.ShapeNoiseResolution, 32, 256);
			ImGui::SliderFloat("Shape Noise Scale", &parameters.ShapeNoiseScale, 0.1f, 5.0f);

			isDirty |= ImGui::SliderInt("Detail Noise Frequency", &parameters.DetailNoiseFrequency, 1, 10);
			isDirty |= ImGui::SliderInt("Detail Noise Resolution", &parameters.DetailNoiseResolution, 8, 64);
			ImGui::SliderFloat("Detail Noise Scale", &parameters.DetailNoiseScale, 2.0f, 12.0f);
			ImGui::SliderFloat("Detail Noise Influence", &parameters.DetailNoiseInfluence, 0.0f, 1.0f);

			ImGui::SliderFloat("Global Scale", &parameters.GlobalScale, 0.01f, 0.0005f);
			ImGui::SliderFloat("Global Density", &parameters.GlobalDensity, 0.0f, 1.0f);
			ImGui::SliderAngle("Wind Direction", &parameters.WindAngle);
			ImGui::SliderFloat("Wind Speed", &parameters.WindSpeed, 0, 1.0f);
			ImGui::SliderFloat("Cloud Top Skew", &parameters.CloudTopSkew, 0, 100.0f);

			ImGui::SliderFloat("Raymarch Step Size", &parameters.RaymarchStepSize, 1.0f, 40.0f);
			ImGui::SliderInt("Light Steps", &parameters.LightMarchSteps, 1, 20);
			ImGui::SliderFloat("Coverage", &parameters.Coverage, 0, 1);
			ImGui::SliderFloat("Cloud Type", &parameters.CloudType, 0, 1);

			ImGui::SliderFloat("Planet Size", &parameters.PlanetRadius, 100, 100000);
			ImGui::DragFloatRange2("Atmosphere Height", &parameters.AtmosphereHeightRange.x, &parameters.AtmosphereHeightRange.y, 1.0f, 10, 1000);
		}
	}
	ImGui::End();

	RGTexture* pNoiseTexture = RGUtils::CreatePersistent(graph, "Shape Noise",
		TextureDesc::Create3D(parameters.ShapeNoiseResolution, parameters.ShapeNoiseResolution, parameters.ShapeNoiseResolution, ResourceFormat::RGBA8_UNORM, 4), &m_pShapeNoise);
	RGTexture* pDetailNoiseTexture = RGUtils::CreatePersistent(graph, "Detail Noise",
		TextureDesc::Create3D(parameters.DetailNoiseResolution, parameters.DetailNoiseResolution, parameters.DetailNoiseResolution, ResourceFormat::RGBA8_UNORM, 4), &m_pDetailNoise);
	RGTexture* pCloudTypeLUT = RGUtils::CreatePersistent(graph, "Height Gradient",
		TextureDesc::Create2D(128, 128, ResourceFormat::R8_UNORM), &m_pCloudHeightDensityLUT);

	if (isDirty)
	{
		struct NoiseParams
		{
			uint32		  Frequency;
			float		  ResolutionInv;
			uint32		  Seed;
			RWTextureView OutputNoise;
			RWTextureView OutputHeightGradient;
		};

		for (uint32 i = 0; i < pNoiseTexture->GetDesc().Mips; ++i)
		{
			graph.AddPass("Compute Shape Noise", RGPassFlag::Compute)
				.Write(pNoiseTexture)
				.Bind([=](CommandContext& context, const RGResources& resources)
					{
						uint32 resolution = pNoiseTexture->GetDesc().Width >> i;

						context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
						context.SetPipelineState(m_pCloudShapeNoisePSO);

						NoiseParams constants{};
						constants.Seed			= parameters.NoiseSeed;
						constants.ResolutionInv = 1.0f / resolution;
						constants.Frequency		= parameters.ShapeNoiseFrequency;
						constants.OutputNoise	= resources.GetUAV(pNoiseTexture, i);
						context.BindRootSRV(BindingSlot::PerInstance, constants);

						context.Dispatch(ComputeUtils::GetNumThreadGroups(Vector3i(resolution), Vector3i(8)));
					});
		}
		for (uint32 i = 0; i < pDetailNoiseTexture->GetDesc().Mips; ++i)
		{
			graph.AddPass("Compute Detail Noise", RGPassFlag::Compute)
				.Write(pDetailNoiseTexture)
				.Bind([=](CommandContext& context, const RGResources& resources)
					{
						uint32 resolution = pDetailNoiseTexture->GetDesc().Width >> i;

						context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
						context.SetPipelineState(m_pCloudDetailNoisePSO);

						NoiseParams constants;
						constants.Seed			= parameters.NoiseSeed;
						constants.ResolutionInv = 1.0f / resolution;
						constants.Frequency		= parameters.DetailNoiseFrequency;
						constants.OutputNoise	= resources.GetUAV(pDetailNoiseTexture, i);
						context.BindRootSRV(BindingSlot::PerInstance, constants);

						context.Dispatch(ComputeUtils::GetNumThreadGroups(Vector3i(resolution), Vector3i(8)));
					});
		}

		graph.AddPass("Height Gradient", RGPassFlag::Compute)
			.Write(pCloudTypeLUT)
			.Bind([=](CommandContext& context, const RGResources& resources)
				{
					Texture* pTarget = resources.Get(pCloudTypeLUT);

					context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
					context.SetPipelineState(m_pCloudHeighDensityLUTPSO);

					NoiseParams constants;
					constants.ResolutionInv		   = 1.0f / pTarget->GetWidth();
					constants.OutputHeightGradient = pTarget->GetUAV();
					context.BindRootSRV(BindingSlot::PerInstance, constants);

					context.Dispatch(ComputeUtils::GetNumThreadGroups(Vector3i(pTarget->GetWidth()), Vector3i(8)));
				});
	}

	RGTexture* pIntermediateColor = graph.Create("Intermediate Color", pColorTarget->GetDesc());

	graph.AddPass("Clouds", RGPassFlag::Compute)
		.Read({ pNoiseTexture, pDetailNoiseTexture, pCloudTypeLUT, pColorTarget, pDepth })
		.Write(pIntermediateColor)
		.Bind([=](CommandContext& context, const RGResources& resources)
			{
				Texture* pTarget = resources.Get(pIntermediateColor);

				context.SetComputeRootSignature(GraphicsCommon::pCommonRS);
				context.SetPipelineState(m_pCloudsPSO);

				struct
				{
					float		  GlobalScale;
					float		  ShapeNoiseScale;
					float		  DetailNoiseScale;
					float		  Coverage;
					float		  GlobalDensity;
					float		  RayStepSize;
					uint32		  LightMarchSteps;
					float		  PlanetRadius;
					float		  AtmosphereHeightStart;
					float		  AtmosphereHeightEnd;
					float		  DetailNoiseInfluence;
					float		  CloudType;
					Vector3		  WindDirection;
					float		  WindSpeed;
					float		  TopSkew;
					TextureView	  SceneTexture;
					TextureView	  DepthTexture;
					TextureView	  CloudTypeDensityLUT;
					TextureView	  ShapeNoise;
					TextureView	  DetailNoise;
					RWTextureView Output;
				} constants;

				constants.GlobalScale			= parameters.GlobalScale;
				constants.ShapeNoiseScale		= parameters.ShapeNoiseScale;
				constants.DetailNoiseScale		= parameters.DetailNoiseScale;
				constants.Coverage				= parameters.Coverage;
				constants.GlobalDensity			= parameters.GlobalDensity;
				constants.RayStepSize			= parameters.RaymarchStepSize;
				constants.LightMarchSteps		= parameters.LightMarchSteps;
				constants.PlanetRadius			= parameters.PlanetRadius;
				constants.AtmosphereHeightStart = parameters.AtmosphereHeightRange.x;
				constants.AtmosphereHeightEnd	= parameters.AtmosphereHeightRange.y;
				constants.DetailNoiseInfluence	= parameters.DetailNoiseInfluence;
				constants.CloudType				= parameters.CloudType;
				constants.WindDirection			= Vector3(cos(parameters.WindAngle), 0, -sin(parameters.WindAngle));
				constants.WindSpeed				= parameters.WindSpeed;
				constants.TopSkew				= parameters.CloudTopSkew;
				constants.SceneTexture		  = resources.GetSRV(pColorTarget);
				constants.DepthTexture		  = resources.GetSRV(pDepth);
				constants.CloudTypeDensityLUT = resources.GetSRV(pCloudTypeLUT);
				constants.ShapeNoise		  = resources.GetSRV(pNoiseTexture);
				constants.DetailNoise		  = resources.GetSRV(pDetailNoiseTexture);
				constants.Output			  = pTarget->GetUAV();
				context.BindRootSRV(BindingSlot::PerInstance, constants);

				Renderer::BindViewUniforms(context, *pView);

				context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 16, pTarget->GetHeight(), 16));
			});

	return pIntermediateColor;
}
