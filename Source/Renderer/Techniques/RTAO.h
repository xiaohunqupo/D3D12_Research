#pragma once

#include "RHI/RHI.h"
#include "RenderGraph/RenderGraphDefinitions.h"

struct RenderView;
struct SceneTextures;

class RTAO
{
public:
	RTAO(GraphicsDevice* pDevice);

	RGTexture* Execute(RGGraph& graph, const RenderView* pView, RGTexture* pDepth, RGTexture* pVelocity);

private:
	Ref<Texture> m_pHistory;

	Ref<StateObject> m_pTraceRaysSO;
	Ref<PipelineState> m_pDenoisePSO;
	Ref<PipelineState> m_pBilateralBlurPSO;
};
