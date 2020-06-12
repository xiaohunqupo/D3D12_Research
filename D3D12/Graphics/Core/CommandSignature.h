#pragma once

class CommandSignature
{
public:
	void Finalize(const char* pName, ID3D12Device* pDevice);

	void SetRootSignature(ID3D12RootSignature* pRootSignature) { m_pRootSignature = pRootSignature; }
	void AddDispatch();
	void AddDraw();
	void AddDrawIndexed();

	ID3D12CommandSignature* GetCommandSignature() const { return m_pCommandSignature.Get(); }

private:
	ComPtr<ID3D12CommandSignature> m_pCommandSignature;
	ID3D12RootSignature* m_pRootSignature = nullptr;
	uint32 m_Stride = 0;
	std::vector<D3D12_INDIRECT_ARGUMENT_DESC> m_ArgumentDesc;
};
