#include "stdafx.h"
#include "Mesh.h"
#include "Graphics/Core/CommandContext.h"
#include "Graphics/Core/Graphics.h"
#include "Graphics/Core/Texture.h"
#include "Graphics/Core/GraphicsBuffer.h"
#include "Core/Paths.h"

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h"

bool Mesh::Load(const char* pFilePath, Graphics* pGraphics, CommandContext* pContext)
{
	Assimp::Importer importer;
	const aiScene* pScene = importer.ReadFile(pFilePath,
		aiProcess_Triangulate
		| aiProcess_ConvertToLeftHanded
		| aiProcess_CalcTangentSpace
		| aiProcess_GenUVCoords
	);

	uint32 vertexCount = 0;
	uint32 indexCount = 0;
	for (uint32 i = 0; i < pScene->mNumMeshes; ++i)
	{
		vertexCount += pScene->mMeshes[i]->mNumVertices;
		indexCount += pScene->mMeshes[i]->mNumFaces * 3;
	}

	struct Vertex
	{
		Vector3 Position;
		Vector2 TexCoord;
		Vector3 Normal;
		Vector3 Tangent;
		Vector3 Bitangent;
	};

	uint64 bufferSize = vertexCount * sizeof(Vertex) +
		indexCount * sizeof(uint32);

	m_pGeometryData = std::make_unique<Buffer>(pGraphics, "Mesh VertexBuffer");
	m_pGeometryData->Create(BufferDesc::CreateBuffer(bufferSize, BufferFlag::ShaderResource | BufferFlag::ByteAddress));

	pContext->InsertResourceBarrier(m_pGeometryData.get(), D3D12_RESOURCE_STATE_COPY_DEST);

	uint64 dataOffset = 0;
	auto CopyData = [this, &dataOffset, &pContext](void* pSource, uint64 size)
	{
		m_pGeometryData->SetData(pContext, pSource, size, dataOffset);
		dataOffset += size;
	};

	for (uint32 i = 0; i < pScene->mNumMeshes; ++i)
	{
		const aiMesh* pMesh = pScene->mMeshes[i];
		std::unique_ptr<SubMesh> pSubMesh = std::make_unique<SubMesh>();
		std::vector<Vertex> vertices(pMesh->mNumVertices);

		for (uint32 j = 0; j < pMesh->mNumVertices; ++j)
		{
			Vertex& vertex = vertices[j];
			vertex.Position = *reinterpret_cast<Vector3*>(&pMesh->mVertices[j]);
			if (pMesh->HasTextureCoords(0))
				vertex.TexCoord = *reinterpret_cast<Vector2*>(&pMesh->mTextureCoords[0][j]);
			vertex.Normal = *reinterpret_cast<Vector3*>(&pMesh->mNormals[j]);
			if (pMesh->HasTangentsAndBitangents())
			{
				vertex.Tangent = *reinterpret_cast<Vector3*>(&pMesh->mTangents[j]);
				vertex.Bitangent = *reinterpret_cast<Vector3*>(&pMesh->mBitangents[j]);
			}
		}

		std::vector<uint32> indices(pMesh->mNumFaces * 3);
		for (uint32 j = 0; j < pMesh->mNumFaces; ++j)
		{
			const aiFace& face = pMesh->mFaces[j];
			for (uint32 k = 0; k < 3; ++k)
			{
				check(face.mNumIndices == 3);
				indices[j * 3 + k] = face.mIndices[k];
			}
		}

		BoundingBox::CreateFromPoints(pSubMesh->m_Bounds, vertices.size(), (Vector3*)&vertices[0], sizeof(Vertex));
		pSubMesh->m_MaterialId = pMesh->mMaterialIndex;
		pSubMesh->m_VertexCount = (uint32)vertices.size();
		pSubMesh->m_VerticesLocation = m_pGeometryData->GetGpuHandle() + dataOffset;
		CopyData(vertices.data(), sizeof(Vertex) * vertices.size());

		pSubMesh->m_IndexCount = (uint32)indices.size();
		pSubMesh->m_IndicesLocation = m_pGeometryData->GetGpuHandle() + dataOffset;
		CopyData(indices.data(), sizeof(uint32) * indices.size());

		pSubMesh->m_Stride = sizeof(Vertex);
		pSubMesh->m_pParent = this;

		m_Meshes.push_back(std::move(pSubMesh));
	}

	pContext->InsertResourceBarrier(m_pGeometryData.get(), D3D12_RESOURCE_STATE_COMMON);
	pContext->FlushResourceBarriers();

	std::string dirPath = Paths::GetDirectoryPath(pFilePath);

	auto loadTexture = [this, pGraphics, pContext](const char* basePath, aiMaterial* pMaterial, aiTextureType type, bool srgb)
	{
		aiString path;
		aiReturn ret = pMaterial->GetTexture(type, 0, &path);
		bool success = ret == aiReturn_SUCCESS;
		std::string pathStr = path.C_Str();
		if (success)
		{
			StringHash pathHash = StringHash(pathStr.c_str());
			auto it = m_ExistingTextures.find(pathHash);
			if (it != m_ExistingTextures.end())
			{
				return it->second;
			}
			std::unique_ptr<Texture> pTex;
			pTex = std::make_unique<Texture>(pGraphics, pathStr.c_str());
			std::stringstream str;
			str << basePath << pathStr;
			success = pTex->Create(pContext, str.str().c_str(), srgb);
			if (success)
			{
				m_Textures.push_back(std::move(pTex));
				m_ExistingTextures[pathHash] = m_Textures.back().get();
				return m_Textures.back().get();
			}
		}
		return (Texture*)nullptr;
	};

	m_Materials.resize(pScene->mNumMaterials);
	for (uint32 i = 0; i < pScene->mNumMaterials; ++i)
	{
		Material& m = m_Materials[i];
		m.pDiffuseTexture = loadTexture(dirPath.c_str(), pScene->mMaterials[i], aiTextureType_DIFFUSE, true);
		m.pNormalTexture = loadTexture(dirPath.c_str(), pScene->mMaterials[i], aiTextureType_NORMALS, false);
		m.pRoughnessTexture = loadTexture(dirPath.c_str(), pScene->mMaterials[i], aiTextureType_SHININESS, false);
		m.pMetallicTexture = loadTexture(dirPath.c_str(), pScene->mMaterials[i], aiTextureType_AMBIENT, false);
		aiString p;
		m.IsTransparent = pScene->mMaterials[i]->GetTexture(aiTextureType_OPACITY, 0, &p) == aiReturn_SUCCESS;
	}

	if (pGraphics->SupportsRayTracing())
	{
		ID3D12GraphicsCommandList4* pCmd = pContext->GetRaytracingCommandList();

		//Bottom Level Acceleration Structure
		{
			std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
			for (size_t i = 0; i < GetMeshCount(); ++i)
			{
				const SubMesh* pSubMesh = GetMesh((int)i);
				D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
				geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
				geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
				geometryDesc.Triangles.IndexBuffer = pSubMesh->GetIndexBuffer().Location;
				geometryDesc.Triangles.IndexCount = pSubMesh->GetIndexBuffer().Elements;
				geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
				geometryDesc.Triangles.Transform3x4 = 0;
				geometryDesc.Triangles.VertexBuffer.StartAddress = pSubMesh->GetVertexBuffer().Location;
				geometryDesc.Triangles.VertexBuffer.StrideInBytes = pSubMesh->GetVertexBuffer().Stride;
				geometryDesc.Triangles.VertexCount = pSubMesh->GetVertexBuffer().Elements;
				geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
				geometries.push_back(geometryDesc);
			}

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS prebuildInfo{};
			prebuildInfo.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			prebuildInfo.Flags =
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE
				| D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
			prebuildInfo.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			prebuildInfo.NumDescs = (uint32)geometries.size();
			prebuildInfo.pGeometryDescs = geometries.data();

			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			pGraphics->GetRaytracingDevice()->GetRaytracingAccelerationStructurePrebuildInfo(&prebuildInfo, &info);

			m_pBLASScratch = std::make_unique<Buffer>(pGraphics, "BLAS Scratch Buffer");
			m_pBLASScratch->Create(BufferDesc::CreateByteAddress(Math::AlignUp<uint64>(info.ScratchDataSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT), BufferFlag::UnorderedAccess));

			m_pBLAS = std::make_unique<Buffer>(pGraphics, "BLAS");
			m_pBLAS->Create(BufferDesc::CreateAccelerationStructure(Math::AlignUp<uint64>(info.ResultDataMaxSizeInBytes, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)));

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC asDesc{};
			asDesc.Inputs = prebuildInfo;
			asDesc.DestAccelerationStructureData = m_pBLAS->GetGpuHandle();
			asDesc.ScratchAccelerationStructureData = m_pBLASScratch->GetGpuHandle();
			asDesc.SourceAccelerationStructureData = 0;

			pCmd->BuildRaytracingAccelerationStructure(&asDesc, 0, nullptr);
			pContext->InsertUavBarrier(m_pBLAS.get());
			pContext->FlushResourceBarriers();
		}
	}

	return true;
}

SubMesh::~SubMesh()
{

}

void SubMesh::Draw(CommandContext* pContext) const
{
	pContext->SetIndexBuffer(GetIndexBuffer());
	VertexBufferView vbView = GetVertexBuffer();
	pContext->SetVertexBuffers(&vbView, 1);
	pContext->DrawIndexed(m_IndexCount, 0, 0);
}

VertexBufferView SubMesh::GetVertexBuffer() const
{
	return VertexBufferView(m_VerticesLocation, m_VertexCount, m_Stride);
}

IndexBufferView SubMesh::GetIndexBuffer() const
{
	return IndexBufferView(m_IndicesLocation, m_IndexCount, false);
}

Buffer* SubMesh::GetSourceBuffer() const
{
	return m_pParent->GetData();
}
