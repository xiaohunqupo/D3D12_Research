#pragma once

#include "RHI/RHI.h"
#include "RenderGraph/RenderGraphDefinitions.h"

struct CaptureTextureContext
{
	// State
	float				RangeMin = 0.0f;
	float				RangeMax = 1.0f;
	bool				VisibleChannels[4] = { true, true, true, true };
	int					CubeFaceIndex = 0;
	int					MipLevel = 0;
	float				Slice = 0.0f;
	bool				IntAsID = false;

	// Private
	String				SourceName;
	TextureDesc			SourceDesc;
	float				Scale = 1.0f;
	bool				XRay = false;
	Vector2u			HoveredPixel;

	// Resources
	Ref<Texture>		pTextureTarget;
	Ref<Buffer>			pReadbackBuffer;
	Ref<Buffer>			pPickBuffer;
	uint32				ReadbackIndex = 0;
	Vector4u			PickingData;
};


class CaptureTextureSystem
{
public:
	CaptureTextureSystem(GraphicsDevice* pDevice);
	void Capture(RGGraph& graph, CaptureTextureContext& captureContext, RGTexture* pTexture);
	void RenderUI(CaptureTextureContext& captureContext, const ImVec2& viewportOrigin, const ImVec2& viewportSize);

private:
	Ref<PipelineState> m_pVisualizePSO;
};
