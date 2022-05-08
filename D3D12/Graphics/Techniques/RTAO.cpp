#include "stdafx.h"
#include "RTAO.h"
#include "Graphics/RHI/RootSignature.h"
#include "Graphics/RHI/Graphics.h"
#include "Graphics/RHI/CommandContext.h"
#include "Graphics/RHI/Texture.h"
#include "Graphics/RHI/ShaderBindingTable.h"
#include "Graphics/RHI/StateObject.h"
#include "Graphics/RHI/PipelineState.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/SceneView.h"

RTAO::RTAO(GraphicsDevice* pDevice)
	: m_pDevice(pDevice)
{
	if (pDevice->GetCapabilities().SupportsRaytracing())
	{
		m_pCommonRS = new RootSignature(pDevice);
		m_pCommonRS->AddRootConstants(0, 4);
		m_pCommonRS->AddConstantBufferView(100);
		m_pCommonRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1);
		m_pCommonRS->AddDescriptorTableSimple(0, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4);
		m_pCommonRS->Finalize("Global");

		StateObjectInitializer stateDesc;
		stateDesc.AddLibrary("RayTracing/RTAOTraceRays.hlsl");
		stateDesc.AddLibrary("RayTracing/SharedRaytracingLib.hlsl", { "OcclusionMS" });
		stateDesc.Name = "RT AO";
		stateDesc.MaxPayloadSize = sizeof(float);
		stateDesc.pGlobalRootSignature = m_pCommonRS;
		stateDesc.RayGenShader = "RayGen";
		stateDesc.AddMissShader("OcclusionMS");
		m_pTraceRaysSO = pDevice->CreateStateObject(stateDesc);

		m_pDenoisePSO = pDevice->CreateComputePipeline(m_pCommonRS, "RayTracing/RTAODenoise.hlsl", "DenoiseCS");
		m_pBilateralBlurPSO = m_pDevice->CreateComputePipeline(m_pCommonRS, "SSAOBlur.hlsl", "CSMain");
	}
}

void RTAO::Execute(RGGraph& graph, const SceneView& view, SceneTextures& sceneTextures)
{
	TextureDesc aoDesc = sceneTextures.AmbientOcclusion->DescTexture;

	RGTexture* rayTraceTarget = graph.CreateTexture("AO Target 0", aoDesc);
	RGTexture* denoiseTarget = graph.CreateTexture("AO Target 1", aoDesc);

	if (!m_pHistory || m_pHistory->GetDesc() != aoDesc)
	{
		m_pHistory = m_pDevice->CreateTexture(aoDesc, "AO History");
	}
	RGTexture* aoHistory = graph.ImportTexture("AO History", m_pHistory);

	static float g_AoPower = 1.0f;
	static float g_AoRadius = 2.0f;
	static int32 g_AoSamples = 1;

	if (ImGui::Begin("Parameters"))
	{
		if (ImGui::CollapsingHeader("Ambient Occlusion"))
		{
			ImGui::SliderFloat("Power", &g_AoPower, 0, 1);
			ImGui::SliderFloat("Radius", &g_AoRadius, 0.1f, 4.0f);
			ImGui::SliderInt("Samples", &g_AoSamples, 1, 64);
		}
	}
	ImGui::End();

	RG_GRAPH_SCOPE("RTAO", graph);

	graph.AddPass("Trace Rays", RGPassFlag::Compute)
		.Read(sceneTextures.Depth)
		.Write(rayTraceTarget)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pRayTraceTarget = rayTraceTarget->Get();

				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pTraceRaysSO);

				struct
				{
					float Power;
					float Radius;
					uint32 Samples;
				} parameters{};

				parameters.Power = g_AoPower;
				parameters.Radius = g_AoRadius;
				parameters.Samples = g_AoSamples;

				ShaderBindingTable bindingTable(m_pTraceRaysSO);
				bindingTable.BindRayGenShader("RayGen");
				bindingTable.BindMissShader("OcclusionMS", {});

				context.SetRootConstants(0, parameters);
				context.SetRootCBV(1, Renderer::GetViewUniforms(view, pRayTraceTarget));
				context.BindResources(2, pRayTraceTarget->GetUAV());
				context.BindResources(3, {
					sceneTextures.Depth->Get()->GetSRV(),
					});

				context.DispatchRays(bindingTable, pRayTraceTarget->GetWidth(), pRayTraceTarget->GetHeight());
			});

	graph.AddPass("Denoise", RGPassFlag::Compute)
		.Read({ rayTraceTarget, sceneTextures.Velocity, sceneTextures.Depth, aoHistory })
		.Write(denoiseTarget)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pDenoiseTarget = denoiseTarget->Get();

				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pDenoisePSO);

				//context.SetRootCBV(0, parameters);
				context.SetRootCBV(1, Renderer::GetViewUniforms(view, pDenoiseTarget));
				context.BindResources(2, pDenoiseTarget->GetUAV());
				context.BindResources(3, {
					sceneTextures.Depth->Get()->GetSRV(),
					aoHistory->Get()->GetSRV(),
					rayTraceTarget->Get()->GetSRV(),
					sceneTextures.Velocity->Get()->GetSRV(),
					});
				context.Dispatch(ComputeUtils::GetNumThreadGroups(pDenoiseTarget->GetWidth(), 8, pDenoiseTarget->GetHeight(), 8));
			});

	graph.AddCopyPass("Store AO History", denoiseTarget, aoHistory);

	graph.AddPass("Blur AO - Horizontal", RGPassFlag::Compute)
		.Read({ denoiseTarget, sceneTextures.Depth })
		.Write(rayTraceTarget)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pTarget = rayTraceTarget->Get();

				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pBilateralBlurPSO);

				struct
				{
					Vector2 DimensionsInv;
					uint32 Horizontal;
				} shaderParameters;

				shaderParameters.Horizontal = 1;
				shaderParameters.DimensionsInv = Vector2(1.0f / pTarget->GetWidth(), 1.0f / pTarget->GetHeight());

				context.SetRootConstants(0, shaderParameters);
				context.SetRootCBV(1, Renderer::GetViewUniforms(view, pTarget));
				context.BindResources(2, pTarget->GetUAV());
				context.BindResources(3, {
					sceneTextures.Depth->Get()->GetSRV(),
					denoiseTarget->Get()->GetSRV()
					});

				context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 256, pTarget->GetHeight(), 1));
			});

	graph.AddPass("Blur AO - Horizontal", RGPassFlag::Compute)
		.Read({ rayTraceTarget, sceneTextures.Depth })
		.Write(sceneTextures.AmbientOcclusion)
		.Bind([=](CommandContext& context, const RGPassResources& resources)
			{
				Texture* pTarget = sceneTextures.AmbientOcclusion->Get();

				context.SetComputeRootSignature(m_pCommonRS);
				context.SetPipelineState(m_pBilateralBlurPSO);

				struct
				{
					Vector2 DimensionsInv;
					uint32 Horizontal;
				} shaderParameters;

				shaderParameters.DimensionsInv = Vector2(1.0f / pTarget->GetWidth(), 1.0f / pTarget->GetHeight());
				shaderParameters.Horizontal = 0;

				context.SetRootConstants(0, shaderParameters);
				context.BindResources(2, pTarget->GetUAV());
				context.BindResources(3, {
					sceneTextures.Depth->Get()->GetSRV(),
					rayTraceTarget->Get()->GetSRV()
					});

				context.Dispatch(ComputeUtils::GetNumThreadGroups(pTarget->GetWidth(), 1, pTarget->GetHeight(), 256));
			});
}
