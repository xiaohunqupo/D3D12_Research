#include "Common.hlsli"
#include "HZB.hlsli"
#include "D3D12.hlsli"
#include "VisibilityBuffer.hlsli"

#ifndef OCCLUSION_FIRST_PASS
#define OCCLUSION_FIRST_PASS 1
#endif

uint DivideAndRoundUp(uint x, uint y)
{
	return (x + y - 1) / y;
}

struct VisibleCluster
{
    uint InstanceID;
    uint ClusterIndex;
};

bool IsClusterVisible(VisibleCluster cluster)
{
	InstanceData instance = GetInstance(cluster.InstanceID);
	MeshData mesh = GetMesh(instance.ID);
	MeshletBounds bounds = BufferLoad<MeshletBounds>(mesh.BufferIndex, cluster.ClusterIndex, mesh.MeshletBoundsOffset);

	float4x4 world = instance.LocalToWorld;
	float4 center = mul(float4(bounds.Center, 1), world);
	float3 radius3 = abs(mul(bounds.Radius.xxx, (float3x3)world));
	float radius = Max3(radius3);
	float3 coneAxis = normalize(mul(bounds.ConeAxis, (float3x3)world));

	FrustumCullData cullData = FrustumCull(center.xyz, radius3, cView.ViewProjectionPrev);
	if(!cullData.IsVisible)
	{
		return false;
	}

#if 0
	if(!HZBCull(cullData, tHZB))
	{
		return false;
	}
#endif

	float3 viewLocation = cView.ViewLocation;
	if(dot(viewLocation - center.xyz, coneAxis) >= bounds.ConeCutoff * length(center.xyz - viewLocation) + radius)
	{
		return false;
	}
	return true;
}

bool IsInstanceVisible(InstanceData instance)
{
	FrustumCullData cullData = FrustumCull(instance.BoundsOrigin, instance.BoundsExtents, cView.ViewProjection);
    return cullData.IsVisible;
}

RWStructuredBuffer<VisibleCluster> uClustersToProcess : register(u0);
RWBuffer<uint> uCounter_ClustersToProcess : register(u1);

RWStructuredBuffer<uint> uCulledInstances : register(u2);
RWBuffer<uint> uCounter_CulledInstances : register(u3);

RWStructuredBuffer<VisibleCluster> uCulledClusters : register(u4);
RWBuffer<uint> uCounter_CulledClusters : register(u5);

RWStructuredBuffer<D3D12_DISPATCH_ARGUMENTS> uDispatchArguments : register(u0);

StructuredBuffer<VisibleCluster> tClustersToProcess : register(t0);
Buffer<uint> tCounter_ClustersToProcess : register(t1);
StructuredBuffer<uint> tInstancesToProcess : register(t2);

[numthreads(64, 1, 1)]
void CullInstancesCS(uint threadID : SV_DispatchThreadID)
{
#if OCCLUSION_FIRST_PASS
    uint numInstances = cView.NumInstances;
#else
    uint numInstances = uCounter_CulledInstances[0];
#endif

    if(threadID >= numInstances)
    {
        return;
    }

#if OCCLUSION_FIRST_PASS
    InstanceData instance = GetInstance(threadID);
#else
    InstanceData instance = GetInstance(tInstancesToProcess[threadID]);
#endif

    MeshData mesh = GetMesh(instance.ID);
    bool visible = IsInstanceVisible(instance);
    if(visible)
    {
        uint elementOffset;
        InterlockedAdd(uCounter_ClustersToProcess[0], mesh.MeshletCount, elementOffset);
        for(uint i = 0; i < mesh.MeshletCount; ++i)
        {
            VisibleCluster cluster;
            cluster.InstanceID = instance.ID;
            cluster.ClusterIndex = i;
            uClustersToProcess[elementOffset + i] = cluster;
        }
    }

#if OCCLUSION_FIRST_PASS
    else
    {
        uint elementOffset = 0;
        InterlockedAdd(uCounter_CulledInstances[0], 1, elementOffset);
        uCulledInstances[elementOffset] = instance.ID;
    }
#endif
}

[numthreads(1, 1, 1)]
void BuildMeshShaderIndirectArgs(uint threadID : SV_DispatchThreadID)
{
    uint numMeshlets = tCounter_ClustersToProcess[0];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCountX = DivideAndRoundUp(numMeshlets, 32);
    args.ThreadGroupCountY = 1;
    args.ThreadGroupCountZ = 1;
    uDispatchArguments[0] = args;
}

[numthreads(1, 1, 1)]
void BuildInstanceCullIndirectArgs(uint threadID : SV_DispatchThreadID)
{
    uint numInstances = uCounter_CulledInstances[0];
    D3D12_DISPATCH_ARGUMENTS args;
    args.ThreadGroupCountX = DivideAndRoundUp(numInstances, 64);
    args.ThreadGroupCountY = 1;
    args.ThreadGroupCountZ = 1;
    uDispatchArguments[0] = args;
}

#define NUM_AS_THREADS 32

struct PayloadData
{
	uint InstanceIndices[NUM_AS_THREADS];
	uint MeshletIndices[NUM_AS_THREADS];
};

groupshared PayloadData gsPayload;

#if __SHADER_TARGET_STAGE == __SHADER_STAGE_AMPLIFICATION
[numthreads(32, 1, 1)]
void CullAndDrawMeshletsAS(uint threadID : SV_DispatchThreadID)
{
	bool visible = false;

	if(threadID < tCounter_ClustersToProcess[0])
	{
		VisibleCluster cluster = tClustersToProcess[threadID];
		visible = IsClusterVisible(cluster);

		if(visible)
		{
			uint index = WavePrefixCountBits(visible);
			gsPayload.InstanceIndices[index] = cluster.InstanceID;
			gsPayload.MeshletIndices[index] = cluster.ClusterIndex;
		}
#if OCCLUSION_FIRST_PASS
		else
		{
			uint elementOffset;
			InterlockedAdd(uCounter_CulledClusters[0], 1, elementOffset);
			uCulledClusters[elementOffset] = cluster;
		}
#endif
	}

	uint visibleCount = WaveActiveCountBits(visible);
	DispatchMesh(visibleCount, 1, 1, gsPayload);
}
#endif

struct PrimitiveAttribute
{
	uint PrimitiveID : SV_PrimitiveID;
	uint MeshletID : MESHLET_ID;
	uint InstanceID : INSTANCE_ID;
};

struct VertexAttribute
{
	float4 Position : SV_Position;
	float2 UV : TEXCOORD;
};

VertexAttribute FetchVertexAttributes(MeshData mesh, float4x4 world, uint vertexId)
{
	VertexAttribute result;
	float3 Position = BufferLoad<float3>(mesh.BufferIndex, vertexId, mesh.PositionsOffset);
	float3 positionWS = mul(float4(Position, 1.0f), world).xyz;
	result.Position = mul(float4(positionWS, 1.0f), cView.ViewProjection);
	result.UV = UnpackHalf2(BufferLoad<uint>(mesh.BufferIndex, vertexId, mesh.UVsOffset));
	return result;
}

#define NUM_MESHLET_THREADS 32

[outputtopology("triangle")]
[numthreads(NUM_MESHLET_THREADS, 1, 1)]
void MSMain(
	in uint groupThreadID : SV_GroupIndex,
	in payload PayloadData payload,
	in uint groupID : SV_GroupID,
	out vertices VertexAttribute verts[MESHLET_MAX_VERTICES],
	out indices uint3 triangles[MESHLET_MAX_TRIANGLES],
	out primitives PrimitiveAttribute primitives[MESHLET_MAX_TRIANGLES])
{
	uint instanceID = payload.InstanceIndices[groupID];
	uint meshletIndex = payload.MeshletIndices[groupID];

	InstanceData instance = GetInstance(instanceID);
	MeshData mesh = GetMesh(instance.MeshIndex);
	Meshlet meshlet = BufferLoad<Meshlet>(mesh.BufferIndex, meshletIndex, mesh.MeshletOffset);

	SetMeshOutputCounts(meshlet.VertexCount, meshlet.TriangleCount);

	for(uint i = groupThreadID; i < meshlet.VertexCount; i += NUM_MESHLET_THREADS)
	{
		uint vertexId = BufferLoad<uint>(mesh.BufferIndex, i + meshlet.VertexOffset, mesh.MeshletVertexOffset);
		VertexAttribute result = FetchVertexAttributes(mesh, instance.LocalToWorld, vertexId);
		verts[i] = result;
	}

	for(uint i = groupThreadID; i < meshlet.TriangleCount; i += NUM_MESHLET_THREADS)
	{
		MeshletTriangle tri = BufferLoad<MeshletTriangle>(mesh.BufferIndex, i + meshlet.TriangleOffset, mesh.MeshletTriangleOffset);
		triangles[i] = uint3(tri.V0, tri.V1, tri.V2);

		PrimitiveAttribute pri;
		pri.PrimitiveID = i;
		pri.MeshletID = meshletIndex;
		pri.InstanceID = instanceID;
		primitives[i] = pri;
	}
}

VisBufferData PSMain(
    VertexAttribute vertexData,
    PrimitiveAttribute primitiveData) : SV_TARGET0
{
	VisBufferData Data;
	Data.ObjectID = primitiveData.InstanceID;
	Data.PrimitiveID = primitiveData.PrimitiveID;
	Data.MeshletID = primitiveData.MeshletID;
	return Data;
}
