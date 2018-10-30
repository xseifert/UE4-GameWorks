// Copyright (c) 2015, NVIDIA CORPORATION. All rights reserved.

#if WITH_GFSDK_VXGI

#include "D3D12RHIPrivate.h"
#include "D3D12NvRHI.h"
#include "AllowWindowsPlatformTypes.h"
#include <D3D12.h>
#include <nvapi.h>
#include "HideWindowsPlatformTypes.h"

#include "PipelineStateCache.h"
#include "MemoryWriter.h"

namespace NVRHI
{
	class FConstantBuffer : public IConstantBuffer
	{
	public:
		FRendererInterfaceD3D12* Parent;
		unsigned long RefCount;

		ConstantBufferDesc Desc;
		FRHIUniformBufferLayout Layout;
		FUniformBufferRHIRef UniformBufferRHI;

		FConstantBuffer(FRendererInterfaceD3D12* InParent)
			: Parent(InParent)
			, RefCount(1)
			, Layout(FRHIUniformBufferLayout::Zero)
		{ }

		unsigned long AddRef() override { return ++RefCount; }
		unsigned long Release() override { unsigned long result = --RefCount; if (result == 0) Parent->destroyConstantBuffer(this); return result; }
		virtual ~FConstantBuffer() { }
	};

	class FTexture : public ITexture
	{
	public:
		FRendererInterfaceD3D12* Parent;
		unsigned long RefCount;

		TextureDesc Desc;
		FTextureRHIRef TextureRHI;
		std::map<std::pair<Format::Enum, uint32>, FShaderResourceViewRHIRef> ShaderResourceViews;
		std::map<std::pair<Format::Enum, uint32>, FUnorderedAccessViewRHIRef> UnorderedAccessViews;

		FTexture(FRendererInterfaceD3D12* InParent)
			: Parent(InParent)
			, RefCount(1)
		{ }

		unsigned long AddRef() override { return ++RefCount; }
		unsigned long Release() override { unsigned long result = --RefCount; if (result == 0) Parent->destroyTexture(this); return result; }
		const TextureDesc& GetDesc() const override { return Desc; }
		virtual ~FTexture() { }
	};

	class FBuffer : public IBuffer
	{
	public:
		FRendererInterfaceD3D12* Parent;
		unsigned long RefCount;

		BufferDesc Desc;
		uint32 Usage;
		uint32 EffectiveStride;
		FStructuredBufferRHIRef BufferRHI;
		FShaderResourceViewRHIRef ShaderResourceView;
		FUnorderedAccessViewRHIRef UnorderedAccessView;

		FBuffer(FRendererInterfaceD3D12* InParent)
			: Parent(InParent)
			, RefCount(1)
			, Usage(0)
			, EffectiveStride(0)
		{ }

		unsigned long AddRef() override { return ++RefCount; }
		unsigned long Release() override { unsigned long result = --RefCount; if (result == 0) Parent->destroyBuffer(this); return result; }
		const BufferDesc& GetDesc() const override { return Desc; }
		virtual ~FBuffer() { }
	};

	class FSampler : public ISampler
	{
	public:
		FRendererInterfaceD3D12* Parent;
		unsigned long RefCount;

		FSamplerStateRHIRef SamplerRHI;

		FSampler(FRendererInterfaceD3D12* InParent)
			: Parent(InParent)
			, RefCount(1)
		{ }

		unsigned long AddRef() override { return ++RefCount; }
		unsigned long Release() override { unsigned long result = --RefCount; if (result == 0) Parent->destroySampler(this); return result; }
		virtual ~FSampler() { }

		static FRHISamplerState* Unwrap(SamplerHandle s) { if (s) return static_cast<FSampler*>(s)->SamplerRHI; else return nullptr; }
	};

	class FShader : public IShader
	{
	public:
		FRendererInterfaceD3D12* Parent;
		unsigned long RefCount;

		TRefCountPtr<FRHIShader> ShaderRHI;

		FShader(FRendererInterfaceD3D12* InParent)
			: Parent(InParent)
			, RefCount(1)
		{ }

		unsigned long AddRef() override { return ++RefCount; }
		unsigned long Release() override { unsigned long result = --RefCount; if (result == 0) Parent->destroyShader(this); return result; }
		virtual ~FShader() { }

		static FRHIShader* Unwrap(ShaderHandle s) { if (s) return static_cast<FShader*>(s)->ShaderRHI; else return nullptr; }
	};

	struct FormatMapping
	{
		Format::Enum abstractFormat;
		EPixelFormat unrealFormat;
		DXGI_FORMAT resourceFormat;
		DXGI_FORMAT srvFormat;
		DXGI_FORMAT rtvFormat;
		uint32 bytesPerPixel;
		bool isDepthStencil;
	};

	// Format mapping table. The rows must be in the exactly same order as Format enum members are defined.
	const FormatMapping FormatMappings[] = {
		{ Format::UNKNOWN,              PF_Unknown,           DXGI_FORMAT_UNKNOWN,                DXGI_FORMAT_UNKNOWN,                DXGI_FORMAT_UNKNOWN,                0, false },
		{ Format::R8_UINT,              PF_R8_UINT,           DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_R8_UINT,                DXGI_FORMAT_R8_UINT,                1, false },
		{ Format::R8_UNORM,             PF_L8,                DXGI_FORMAT_R8_TYPELESS,            DXGI_FORMAT_R8_UNORM,               DXGI_FORMAT_R8_UNORM,               1, false },
		{ Format::RG8_UINT,             PF_R8G8,              DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R8G8_UINT,              DXGI_FORMAT_R8G8_UINT,              2, false },
		{ Format::RG8_UNORM,            PF_R8G8,              DXGI_FORMAT_R8G8_TYPELESS,          DXGI_FORMAT_R8G8_UNORM,             DXGI_FORMAT_R8G8_UNORM,             2, false },
		{ Format::R16_UINT,             PF_R16_UINT,          DXGI_FORMAT_R16_TYPELESS,           DXGI_FORMAT_R16_UINT,               DXGI_FORMAT_R16_UINT,               2, false },
		{ Format::R16_UNORM,            PF_R16_UINT,          DXGI_FORMAT_R16_TYPELESS,           DXGI_FORMAT_R16_UNORM,              DXGI_FORMAT_R16_UNORM,              2, false },
		{ Format::R16_FLOAT,            PF_R16F,              DXGI_FORMAT_R16_TYPELESS,           DXGI_FORMAT_R16_FLOAT,              DXGI_FORMAT_R16_FLOAT,              2, false },
		{ Format::RGBA8_UNORM,          PF_R8G8B8A8,          DXGI_FORMAT_R8G8B8A8_TYPELESS,      DXGI_FORMAT_R8G8B8A8_UNORM,         DXGI_FORMAT_R8G8B8A8_UNORM,         4, false },
		{ Format::RGBA8_SNORM,          PF_R8G8B8A8,          DXGI_FORMAT_R8G8B8A8_TYPELESS,      DXGI_FORMAT_R8G8B8A8_SNORM,         DXGI_FORMAT_R8G8B8A8_SNORM,         4, false },
		{ Format::BGRA8_UNORM,          PF_B8G8R8A8,          DXGI_FORMAT_B8G8R8A8_TYPELESS,      DXGI_FORMAT_B8G8R8A8_UNORM,         DXGI_FORMAT_B8G8R8A8_UNORM,         4, false },
		{ Format::SRGBA8_UNORM,         PF_R8G8B8A8,          DXGI_FORMAT_R8G8B8A8_TYPELESS,      DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,    4, false },
		{ Format::SBGRA8_UNORM,         PF_B8G8R8A8,          DXGI_FORMAT_B8G8R8A8_TYPELESS,      DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,    4, false },
		{ Format::R10G10B10A2_UNORM,    PF_A2B10G10R10,       DXGI_FORMAT_R10G10B10A2_TYPELESS,   DXGI_FORMAT_R10G10B10A2_UNORM,      DXGI_FORMAT_R10G10B10A2_UNORM,      4, false },
		{ Format::R11G11B10_FLOAT,      PF_FloatR11G11B10,    DXGI_FORMAT_R11G11B10_FLOAT,        DXGI_FORMAT_R11G11B10_FLOAT,        DXGI_FORMAT_R11G11B10_FLOAT,        4, false },
		{ Format::RG16_UINT,            PF_G16R16,            DXGI_FORMAT_R16G16_TYPELESS,        DXGI_FORMAT_R16G16_UINT,            DXGI_FORMAT_R16G16_UINT,            4, false },
		{ Format::RG16_FLOAT,           PF_G16R16F,           DXGI_FORMAT_R16G16_TYPELESS,        DXGI_FORMAT_R16G16_FLOAT,           DXGI_FORMAT_R16G16_FLOAT,           4, false },
		{ Format::R32_UINT,             PF_R32_UINT,          DXGI_FORMAT_R32_TYPELESS,           DXGI_FORMAT_R32_UINT,               DXGI_FORMAT_R32_UINT,               4, false },
		{ Format::R32_FLOAT,            PF_R32_FLOAT,         DXGI_FORMAT_R32_TYPELESS,           DXGI_FORMAT_R32_FLOAT,              DXGI_FORMAT_R32_FLOAT,              4, false },
		{ Format::RGBA16_FLOAT,         PF_FloatRGBA,         DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R16G16B16A16_FLOAT,     DXGI_FORMAT_R16G16B16A16_FLOAT,     8, false },
		{ Format::RGBA16_UNORM,         PF_Unknown,           DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R16G16B16A16_UNORM,     DXGI_FORMAT_R16G16B16A16_UNORM,     8, false },
		{ Format::RGBA16_SNORM,         PF_Unknown,           DXGI_FORMAT_R16G16B16A16_TYPELESS,  DXGI_FORMAT_R16G16B16A16_SNORM,     DXGI_FORMAT_R16G16B16A16_SNORM,     8, false },
		{ Format::RG32_UINT,            PF_Unknown,           DXGI_FORMAT_R32G32_TYPELESS,        DXGI_FORMAT_R32G32_UINT,            DXGI_FORMAT_R32G32_UINT,            8, false },
		{ Format::RG32_FLOAT,           PF_G32R32F,           DXGI_FORMAT_R32G32_TYPELESS,        DXGI_FORMAT_R32G32_FLOAT,           DXGI_FORMAT_R32G32_FLOAT,           8, false },
		{ Format::RGB32_UINT,           PF_Unknown,           DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_UINT,         DXGI_FORMAT_R32G32B32_UINT,         12, false },
		{ Format::RGB32_FLOAT,          PF_FloatRGB,          DXGI_FORMAT_R32G32B32_TYPELESS,     DXGI_FORMAT_R32G32B32_FLOAT,        DXGI_FORMAT_R32G32B32_FLOAT,        12, false },
		{ Format::RGBA32_UINT,          PF_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_UINT,      DXGI_FORMAT_R32G32B32A32_UINT,      16, false },
		{ Format::RGBA32_FLOAT,         PF_A32B32G32R32F,     DXGI_FORMAT_R32G32B32A32_TYPELESS,  DXGI_FORMAT_R32G32B32A32_FLOAT,     DXGI_FORMAT_R32G32B32A32_FLOAT,     16, false },
		{ Format::D16,                  PF_ShadowDepth,       DXGI_FORMAT_R16_TYPELESS,           DXGI_FORMAT_R16_UNORM,              DXGI_FORMAT_D16_UNORM,              2, true },
		{ Format::D24S8,                PF_DepthStencil,      DXGI_FORMAT_R24G8_TYPELESS,         DXGI_FORMAT_R24_UNORM_X8_TYPELESS,  DXGI_FORMAT_D24_UNORM_S8_UINT,      4, true },
		{ Format::X24G8_UINT,           PF_DepthStencil,      DXGI_FORMAT_R24G8_TYPELESS,         DXGI_FORMAT_X24_TYPELESS_G8_UINT,   DXGI_FORMAT_D24_UNORM_S8_UINT,      4, true },
		{ Format::D32,                  PF_R32_FLOAT,         DXGI_FORMAT_R32_TYPELESS,           DXGI_FORMAT_R32_FLOAT,              DXGI_FORMAT_D32_FLOAT,              4, true },
		{ Format::BC1,                  PF_DXT1,              DXGI_FORMAT_BC1_TYPELESS,           DXGI_FORMAT_BC1_UNORM,              DXGI_FORMAT_BC1_UNORM,              0, false },
		{ Format::BC2,                  PF_DXT3,              DXGI_FORMAT_BC2_TYPELESS,           DXGI_FORMAT_BC2_UNORM,              DXGI_FORMAT_BC2_UNORM,              0, false },
		{ Format::BC3,                  PF_DXT5,              DXGI_FORMAT_BC3_TYPELESS,           DXGI_FORMAT_BC3_UNORM,              DXGI_FORMAT_BC3_UNORM,              0, false },
		{ Format::BC4,                  PF_BC4,               DXGI_FORMAT_BC4_TYPELESS,           DXGI_FORMAT_BC4_UNORM,              DXGI_FORMAT_BC4_UNORM,              0, false },
		{ Format::BC5,                  PF_BC5,               DXGI_FORMAT_BC5_TYPELESS,           DXGI_FORMAT_BC5_UNORM,              DXGI_FORMAT_BC5_UNORM,              0, false },
		{ Format::BC6H,                 PF_BC6H,              DXGI_FORMAT_BC6H_UF16,              DXGI_FORMAT_BC6H_UF16,              DXGI_FORMAT_BC6H_UF16,              0, false },
		{ Format::BC7,                  PF_BC7,               DXGI_FORMAT_BC7_TYPELESS,           DXGI_FORMAT_BC7_UNORM,              DXGI_FORMAT_BC7_UNORM,              0, false },
	};

	static const FormatMapping& GetFormatMapping(Format::Enum abstractFormat)
	{
		const FormatMapping& mapping = FormatMappings[abstractFormat];
		check(mapping.abstractFormat == abstractFormat);
		return mapping;
	}

	static bool GetSSE42Support()
	{
		int cpui[4];
		__cpuidex(cpui, 1, 0);
		return !!(cpui[2] & 0x100000);
	}

	static const bool CpuSupportsSSE42 = GetSSE42Support();

	class CrcHash
	{
	private:
		uint32 crc;
	public:
		CrcHash()
			: crc(0)
		{
		}

		uint32 Get()
		{
			return crc;
		}

		template<size_t size> __forceinline void AddBytesSSE42(void* p)
		{
			static_assert(size % 4 == 0, "Size of hashable types must be multiple of 4");

			uint32* data = (uint32*)p;

			const size_t numIterations = size / sizeof(uint32);
			for (size_t i = 0; i < numIterations; i++)
			{
				crc = _mm_crc32_u32(crc, data[i]);
			}
		}

		__forceinline void AddBytes(char* p, uint32 size)
		{
			crc = FCrc::MemCrc32(p, size, crc);
		}

		template<typename T> void Add(const T& value)
		{
			if (CpuSupportsSSE42)
				AddBytesSSE42<sizeof(value)>((void*)&value);
			else
				AddBytes((char*)&value, sizeof(value));
		}
	};

	FRendererInterfaceD3D12::FRendererInterfaceD3D12(FD3D12Device* Device)
		: m_Device(Device)
		, m_TreatErrorsAsFatal(true)
		, m_RHICmdList(nullptr)
		, m_RHIThreadId(0)
	{
	}

	void FRendererInterfaceD3D12::setTreatErrorsAsFatal(bool v)
	{
		m_TreatErrorsAsFatal = v;
	}

	void FRendererInterfaceD3D12::signalError(const char* file, int line, const char* errorDesc)
	{
		if (m_TreatErrorsAsFatal)
		{
			UE_LOG(LogD3D12RHI, Fatal, TEXT("VXGI Error: %s (%s, %i)"), ANSI_TO_TCHAR(errorDesc), ANSI_TO_TCHAR(file), line);
		}
		else
		{
			UE_LOG(LogD3D12RHI, Error, TEXT("VXGI Error: %s (%s, %i)"), ANSI_TO_TCHAR(errorDesc), ANSI_TO_TCHAR(file), line);
		}
	}

	class FTextureInitData : public FResourceBulkDataInterface
	{
	public:
		const void* Data;
		uint32 Size;
		bool Disposed;
		FTextureInitData() : Disposed(false), Data(0), Size(0) {}
		virtual const void* GetResourceBulkData() const { return Data; }
		virtual uint32 GetResourceBulkDataSize() const { return Size; }
		virtual void Discard() { Disposed = true; }
	};


	TextureHandle FRendererInterfaceD3D12::createTexture(const TextureDesc& d, const void* data)
	{
		FTexture* texture = new FTexture(this);
		texture->Desc = d;

		const FormatMapping& mapping = GetFormatMapping(d.format);

		uint32 flags = TexCreate_None;

		flags |= TexCreate_ShaderResource;
		if (d.isRenderTarget)
			flags |= mapping.isDepthStencil ? TexCreate_DepthStencilTargetable : TexCreate_RenderTargetable;
		if (d.isUAV)
			flags |= TexCreate_UAV;
		if (d.disableGPUsSync)
			flags |= TexCreate_AFRManual;

		FTextureInitData InitData;
		InitData.Data = data;
		InitData.Size = d.width * d.height * mapping.bytesPerPixel * FMath::Max(1u, d.depthOrArraySize);

		FRHIResourceCreateInfo CreateInfo;
		if (data)
		{
			CreateInfo.BulkData = &InitData;
		}

		if (d.useClearValue)
		{
			if (mapping.isDepthStencil)
				CreateInfo.ClearValueBinding = FClearValueBinding(d.clearValue.r, uint32(d.clearValue.g));
			else
				CreateInfo.ClearValueBinding = FClearValueBinding(FLinearColor(d.clearValue.r, d.clearValue.g, d.clearValue.b, d.clearValue.a));
		}

		if (d.depthOrArraySize == 0)
		{
			texture->TextureRHI = GDynamicRHI->RHICreateTexture2D(d.width, d.height, mapping.unrealFormat, d.mipLevels, d.sampleCount, flags, CreateInfo);
			check(texture->TextureRHI.IsValid());
		}
		else if (d.isArray)
		{
			check(d.sampleCount == 1);
			texture->TextureRHI = GDynamicRHI->RHICreateTexture2DArray(d.width, d.height, d.depthOrArraySize, mapping.unrealFormat, d.mipLevels, flags, CreateInfo);
			check(texture->TextureRHI.IsValid());
		}
		else if (d.isCubeMap)
		{
			check(d.sampleCount == 1);
			check(d.width == d.height);
			check(d.depthOrArraySize == 6);
			texture->TextureRHI = GDynamicRHI->RHICreateTextureCube(d.width, mapping.unrealFormat, d.mipLevels, flags, CreateInfo);
			check(texture->TextureRHI.IsValid());
		}
		else
		{
			check(d.sampleCount == 1);
			texture->TextureRHI = GDynamicRHI->RHICreateTexture3D(d.width, d.height, d.depthOrArraySize, mapping.unrealFormat, d.mipLevels, flags, CreateInfo);
			check(texture->TextureRHI.IsValid());
		}

		if (d.debugName)
		{
			GDynamicRHI->RHIBindDebugLabelName(texture->TextureRHI, ANSI_TO_TCHAR(d.debugName));
		}

		return texture;
	}

	const TextureDesc& FRendererInterfaceD3D12::describeTexture(TextureHandle t)
	{
		return static_cast<FTexture*>(t)->Desc;
	}

	void FRendererInterfaceD3D12::clearTextureFloat(TextureHandle _t, const Color& clearColor)
	{
		checkCommandList();

		FTexture* t = static_cast<FTexture*>(_t);

		if (!t->Desc.isUAV)
			// TODO: add clear of non-UAV textures
			return;

		uint32 Color[4] = { clearColor.r, clearColor.g, clearColor.b, clearColor.a };

		for (uint32 mipLevel = 0; mipLevel < t->Desc.mipLevels; mipLevel++)
		{
			auto UAV = getTextureUAV(t, mipLevel, Format::UNKNOWN);
			m_RHICmdList->ClearTinyUAV(UAV, Color);
		}
	}

	void FRendererInterfaceD3D12::clearTextureUInt(TextureHandle t, uint32 clearColor)
	{
		clearTextureFloat(t, Color(float(clearColor)));
	}

	void FRendererInterfaceD3D12::writeTexture(TextureHandle t, uint32 subresource, const void* data, uint32 rowPitch, uint32 depthPitch)
	{
		checkNoEntry();
	}

	bool FRendererInterfaceD3D12::readTexture(TextureHandle t, void* data, size_t rowPitch)
	{
		checkNoEntry();
		return false;
	}

	void FRendererInterfaceD3D12::destroyTexture(TextureHandle t)
	{
		if (!t) return;
		delete t;
	}

	void FRendererInterfaceD3D12::resolveTexture(TextureHandle dst, TextureHandle src, Format::Enum format, uint32_t dstSubres, uint32_t srcSubres)
	{
		checkNoEntry();
	}

	void* FRendererInterfaceD3D12::handoffTexture(TextureHandle t)
	{
		checkNoEntry();
		return nullptr;
	}

	TextureHandle FRendererInterfaceD3D12::getHandleForTexture(void* resource, Format::Enum formatOverride)
	{
		checkNoEntry();
		return nullptr;
	}

	class FBufferInitData : public FResourceArrayInterface
	{
	public:
		const void* Data;
		uint32 Size;
		FBufferInitData() : Data(nullptr), Size(0) { }
		virtual const void* GetResourceData() const { return Data; }
		virtual uint32 GetResourceDataSize() const { return Size; }
		virtual void Discard() { }
		virtual bool IsStatic() const { return true; }
		virtual bool GetAllowCPUAccess() const { return false; }
		virtual void SetAllowCPUAccess(bool bInNeedsCPUAccess) { }
	};

	BufferHandle FRendererInterfaceD3D12::createBuffer(const BufferDesc& d, const void* data)
	{
		FBuffer* buffer = new FBuffer(this);
		buffer->Desc = d;
		buffer->EffectiveStride = (d.structStride == 0) ? 4 : d.structStride;

		buffer->Usage = BUF_ShaderResource;

		if (d.canHaveUAVs)
			buffer->Usage |= BUF_UnorderedAccess;
		else
			buffer->Usage |= BUF_Dynamic;

		// Force non-structured buffers to have BUF_DrawIndirect flag because otherwise UE will create them as structured.
		if (d.isDrawIndirectArgs || d.structStride == 0)
			buffer->Usage |= BUF_DrawIndirect;

		FBufferInitData InitData;
		InitData.Data = data;
		InitData.Size = d.byteSize;

		FRHIResourceCreateInfo CreateInfo;
		if (data)
		{
			CreateInfo.ResourceArray = &InitData;
		}

		if (data || !(buffer->Usage & BUF_Dynamic))
		{
			buffer->BufferRHI = GDynamicRHI->RHICreateStructuredBuffer(buffer->EffectiveStride, d.byteSize, buffer->Usage, CreateInfo);
		}
		else
			buffer->BufferRHI = nullptr;

		return buffer;
	}

	void FRendererInterfaceD3D12::writeBuffer(BufferHandle _b, const void* data, size_t dataSize)
	{
		FBuffer* b = static_cast<FBuffer*>(_b);

		check(data);
		check(dataSize == b->Desc.byteSize);
		check(b->Usage & BUF_Dynamic);

		FBufferInitData InitData;
		InitData.Data = data;
		InitData.Size = dataSize;

		FRHIResourceCreateInfo CreateInfo;
		CreateInfo.ResourceArray = &InitData;

		b->BufferRHI.SafeRelease();
		b->UnorderedAccessView.SafeRelease();
		b->ShaderResourceView.SafeRelease();

		b->BufferRHI = GDynamicRHI->RHICreateStructuredBuffer(b->EffectiveStride, dataSize, b->Usage, CreateInfo);
	}

	void FRendererInterfaceD3D12::clearBufferUInt(BufferHandle b, uint32 clearValue)
	{
		checkCommandList();

		FRHIUnorderedAccessView* UAV = getBufferUAV(b, Format::UNKNOWN);
		uint32 ClearValues[4] = { clearValue, clearValue, clearValue, clearValue };
		m_RHICmdList->ClearTinyUAV(UAV, ClearValues);
	}

	void FRendererInterfaceD3D12::copyToBuffer(BufferHandle dest, uint32 destOffsetBytes, BufferHandle src, uint32 srcOffsetBytes, size_t dataSizeBytes)
	{
		checkCommandList();

		m_RHICmdList->CopyStructuredBufferData(static_cast<FBuffer*>(dest)->BufferRHI, destOffsetBytes, static_cast<FBuffer*>(src)->BufferRHI, srcOffsetBytes, dataSizeBytes);
	}

	void FRendererInterfaceD3D12::destroyBuffer(BufferHandle b)
	{
		if (!b) return;
		delete b;
	}

	void FRendererInterfaceD3D12::readBuffer(BufferHandle b, void* data, size_t* dataSize) { }

	ConstantBufferHandle FRendererInterfaceD3D12::createConstantBuffer(const ConstantBufferDesc& d, const void* data)
	{
		FConstantBuffer* pCB = new FConstantBuffer(this);
		pCB->Desc = d;
		pCB->Layout.ConstantBufferSize = d.byteSize;

		if (data)
			writeConstantBuffer(pCB, data, d.byteSize);

		return pCB;
	}

	void FRendererInterfaceD3D12::writeConstantBuffer(ConstantBufferHandle _b, const void* data, size_t dataSize)
	{
		FConstantBuffer* b = static_cast<FConstantBuffer*>(_b);

		check(dataSize == b->Desc.byteSize);

		b->UniformBufferRHI.SafeRelease();
		b->UniformBufferRHI = RHICreateUniformBuffer(data, b->Layout, UniformBuffer_SingleFrame);
	}

	void FRendererInterfaceD3D12::destroyConstantBuffer(ConstantBufferHandle b)
	{
		if (!b) return;
		static_cast<FConstantBuffer*>(b)->UniformBufferRHI.SafeRelease();
		delete b;
	}

	ShaderHandle FRendererInterfaceD3D12::createShader(const ShaderDesc& d, const void* binary, const size_t binarySize)
	{
		uint32 NumCBs = 0, NumSRVs = 0, NumSamplers = 0, NumUAVs = 0;

		for (uint32_t word = 0; word < ARRAYSIZE(d.metadata.slotsSRV); word++)
		{
			if (d.metadata.slotsSRV[word])
			{
				for (uint32_t bit = 0; bit < 32; bit++)
				{
					uint32_t i = (word << 5) | bit;

					if (d.metadata.slotsSRV[word] & (1 << bit))
					{
						NumSRVs = FMath::Max(NumSRVs, i + 1);
					}
				}
			}
		}

		for (uint32_t word = 0; word < ARRAYSIZE(d.metadata.slotsSampler); word++)
		{
			if (d.metadata.slotsSampler[word])
			{
				for (uint32_t bit = 0; bit < 32; bit++)
				{
					uint32_t i = (word << 5) | bit;

					if (d.metadata.slotsSampler[word] & (1 << bit))
					{
						NumSamplers = FMath::Max(NumSamplers, i + 1);
					}
				}
			}
		}

		for (uint32_t i = 0; i < ARRAYSIZE(d.metadata.constantBufferSizes); i++)
		{
			if (d.metadata.constantBufferSizes[i])
			{
				NumCBs = FMath::Max(NumCBs, i + 1);
			}
		}

		if (d.metadata.slotsUAV)
		{
			for (uint32_t i = 0; i < 32; i++)
			{
				if (d.metadata.slotsUAV & (1 << i))
				{
					NumUAVs = FMath::Max(NumUAVs, i + 1);
				}
			}
		}

		FD3D12ShaderResourceTable ShaderResourceTable;

		FShaderCode Code;
		TArray<uint8>& CodeArray = Code.GetWriteAccess();

		FMemoryWriter Ar(CodeArray, true, true);
		Ar << ShaderResourceTable;
		int64 Offset = Ar.Tell();

		CodeArray.AddZeroed(binarySize);
		FMemory::Memcpy(CodeArray.GetData() + Offset, binary, binarySize);

		FShaderCodePackedResourceCounts ResourceCounts;
		ResourceCounts.NumCBs = NumCBs;
		ResourceCounts.NumSamplers = NumSamplers;
		ResourceCounts.NumSRVs = NumSRVs;
		ResourceCounts.NumUAVs = NumUAVs;
		ResourceCounts.bGlobalUniformBufferUsed = false;
		Code.AddOptionalData(ResourceCounts);
		Code.FinalizeShaderCode();

		TRefCountPtr<FRHIShader> Result;

		check(d.numCustomSemantics == 0); // Custom semantics are not supported by this implementation

		TArray<NVAPI_D3D12_PSO_EXTENSION_DESC*> Extensions;

		if ((d.fastGSFlags & FastGeometryShaderFlags::FORCE_FAST_GS) || d.numCustomSemantics || d.pCoordinateSwizzling)
		{
			NVAPI_D3D12_PSO_GEOMETRY_SHADER_DESC* pExtn = new NVAPI_D3D12_PSO_GEOMETRY_SHADER_DESC();
			memset(pExtn, 0, sizeof(*pExtn));
			pExtn->baseVersion = NV_PSO_EXTENSION_DESC_VER;
			pExtn->psoExtension = NV_PSO_GEOMETRY_SHADER_EXTENSION;
			pExtn->version = NV_GEOMETRY_SHADER_PSO_EXTENSION_DESC_VER;

			pExtn->NumCustomSemantics = d.numCustomSemantics;
			pExtn->pCustomSemantics = d.numCustomSemantics ? d.pCustomSemantics : nullptr;
			pExtn->UseCoordinateSwizzle = d.pCoordinateSwizzling != nullptr;
			pExtn->pCoordinateSwizzling = d.pCoordinateSwizzling != nullptr ? d.pCoordinateSwizzling : nullptr;
			pExtn->ForceFastGS = (d.fastGSFlags & FastGeometryShaderFlags::FORCE_FAST_GS) != 0;
			pExtn->UseViewportMask = (d.fastGSFlags & FastGeometryShaderFlags::USE_VIEWPORT_MASK) != 0;
			pExtn->OffsetRtIndexByVpIndex = (d.fastGSFlags & FastGeometryShaderFlags::OFFSET_RT_INDEX_BY_VP_INDEX) != 0;
			pExtn->DontUseViewportOrder = (d.fastGSFlags & FastGeometryShaderFlags::STRICT_API_ORDER) != 0;
			pExtn->UseSpecificShaderExt = d.useSpecificShaderExt;
			pExtn->UseAttributeSkipMask = false;

			Extensions.Add(pExtn);
		}

		if (d.hlslExtensionsUAV >= 0)
		{
			NVAPI_D3D12_PSO_SET_SHADER_EXTENSION_SLOT_DESC* pExtn = new NVAPI_D3D12_PSO_SET_SHADER_EXTENSION_SLOT_DESC();
			memset(pExtn, 0, sizeof(*pExtn));
			pExtn->baseVersion = NV_PSO_EXTENSION_DESC_VER;
			pExtn->psoExtension = NV_PSO_SET_SHADER_EXTNENSION_SLOT_AND_SPACE;
			pExtn->version = NV_SET_SHADER_EXTENSION_SLOT_DESC_VER;

			pExtn->uavSlot = d.hlslExtensionsUAV;
			pExtn->registerSpace = 0;

			Extensions.Add(pExtn);
		}

		if (Extensions.Num() > 0)
		{
			GDynamicRHI->RHISetExtensionsForNextShader((const void* const*)&Extensions[0], Extensions.Num());
		}

		switch (d.shaderType)
		{
		case ShaderType::SHADER_VERTEX:
			Result = GDynamicRHI->RHICreateVertexShader(CodeArray);
			break;
		case ShaderType::SHADER_HULL:
			Result = GDynamicRHI->RHICreateHullShader(CodeArray);
			break;
		case ShaderType::SHADER_DOMAIN:
			Result = GDynamicRHI->RHICreateDomainShader(CodeArray);
			break;
		case ShaderType::SHADER_GEOMETRY:
			Result = GDynamicRHI->RHICreateGeometryShader(CodeArray);
			break;
		case ShaderType::SHADER_PIXEL:
			Result = GDynamicRHI->RHICreatePixelShader(CodeArray);
			break;
		case ShaderType::SHADER_COMPUTE:
			Result = GDynamicRHI->RHICreateComputeShader(CodeArray);
			break;
		}

		if (Extensions.Num() > 0)
		{
			GDynamicRHI->RHISetExtensionsForNextShader(nullptr, 0);
		}

		check(Result.IsValid());

		FShader* Shader = new FShader(this);
		Shader->ShaderRHI = Result;
		return Shader;
	}

	void FRendererInterfaceD3D12::setPixelShaderResourceAttributes(NVRHI::ShaderHandle PixelShader, const TArray<uint8>& ShaderResourceTable, bool bUsesGlobalCB)
	{
		FD3D12PixelShader* PixelShaderRHI = (FD3D12PixelShader*)static_cast<FShader*>(PixelShader)->ShaderRHI.GetReference();

		//Overwrite PixelShader->ShaderResourceTable
		FMemoryReader Ar(ShaderResourceTable, true);
		Ar << PixelShaderRHI->ShaderResourceTable;

		PixelShaderRHI->ResourceCounts.bGlobalUniformBufferUsed = bUsesGlobalCB;
	}

	ShaderHandle FRendererInterfaceD3D12::createShaderFromAPIInterface(ShaderType::Enum shaderType, const void* apiInterface)
	{
		checkNoEntry();
		return nullptr;
	}

	void FRendererInterfaceD3D12::destroyShader(ShaderHandle _s)
	{
		if (!_s) return;
		FShader* s = static_cast<FShader*>(_s);
		delete s;
	}

	FRHIShader* FRendererInterfaceD3D12::getRHIShader(ShaderHandle shader)
	{
		return FShader::Unwrap(shader);
	}

	static ESamplerAddressMode convertSamplerAddressMode(SamplerDesc::WrapMode mode)
	{
		switch (mode)
		{
		case SamplerDesc::WRAP_MODE_CLAMP: return AM_Clamp;
		case SamplerDesc::WRAP_MODE_WRAP: return AM_Wrap;
		case SamplerDesc::WRAP_MODE_BORDER: return AM_Border;
		}

		return AM_Wrap;
	}

	SamplerHandle FRendererInterfaceD3D12::createSampler(const SamplerDesc& d)
	{
		FSamplerStateInitializerRHI Desc;

		if (d.minFilter || d.magFilter)
			if (d.mipFilter)
				Desc.Filter = d.anisotropy > 1 ? SF_AnisotropicLinear : SF_Trilinear;
			else
				Desc.Filter = d.anisotropy > 1 ? SF_AnisotropicLinear : SF_Bilinear;
		else
			Desc.Filter = SF_Point;

		Desc.AddressU = convertSamplerAddressMode(d.wrapMode[0]);
		Desc.AddressV = convertSamplerAddressMode(d.wrapMode[1]);
		Desc.AddressW = convertSamplerAddressMode(d.wrapMode[2]);

		Desc.MipBias = d.mipBias;
		Desc.MaxAnisotropy = d.anisotropy;
		Desc.BorderColor = FColor(
			uint8(d.borderColor.r * 255.f),
			uint8(d.borderColor.g * 255.f),
			uint8(d.borderColor.b * 255.f),
			uint8(d.borderColor.a * 255.f)).DWColor();

		Desc.SamplerComparisonFunction = d.shadowCompare ? SCF_Less : SCF_Never;
		Desc.MinMipLevel = 0;
		Desc.MaxMipLevel = FLT_MAX;

		FSampler* Sampler = new FSampler(this);
		Sampler->SamplerRHI = GDynamicRHI->RHICreateSamplerState(Desc);
		return Sampler;
	}

	void FRendererInterfaceD3D12::destroySampler(SamplerHandle _s)
	{
		if (!_s) return;
		FSampler* s = static_cast<FSampler*>(_s);
		delete s;
	}

	InputLayoutHandle FRendererInterfaceD3D12::createInputLayout(const VertexAttributeDesc* d, uint32 attributeCount, const void* vertexShaderBinary, const size_t binarySize)
	{
		checkNoEntry();
		return nullptr;
	}

	void FRendererInterfaceD3D12::destroyInputLayout(InputLayoutHandle i) { }

	PerformanceQueryHandle FRendererInterfaceD3D12::createPerformanceQuery(const char* name)
	{
		checkNoEntry();
		return nullptr;
	}

	void FRendererInterfaceD3D12::destroyPerformanceQuery(PerformanceQueryHandle query) { }
	void FRendererInterfaceD3D12::beginPerformanceQuery(PerformanceQueryHandle query, bool onlyAnnotation) { }
	void FRendererInterfaceD3D12::endPerformanceQuery(PerformanceQueryHandle query) { }

	float FRendererInterfaceD3D12::getPerformanceQueryTimeMS(PerformanceQueryHandle query)
	{
		return 0.f;
	}

	GraphicsAPI::Enum FRendererInterfaceD3D12::getGraphicsAPI()
	{
		return GraphicsAPI::D3D12;
	}

	void* FRendererInterfaceD3D12::getAPISpecificInterface(APISpecificInterface::Enum interfaceType)
	{
		if (interfaceType == APISpecificInterface::D3D12DEVICE)
			return GDynamicRHI->RHIGetNativeDevice();

		return nullptr;
	}

	bool FRendererInterfaceD3D12::isOpenGLExtensionSupported(const char* name)
	{
		return false;
	}

	void* FRendererInterfaceD3D12::getOpenGLProcAddress(const char* procname)
	{
		return nullptr;
	}

	static void convertPrimTypeAndCount(PrimitiveType::Enum primType, uint32 vertexCount, uint32& unrealPrimType, uint32& primCount)
	{
		unrealPrimType = PT_TriangleList;
		primCount = 0;

		switch (primType)
		{
		case PrimitiveType::POINT_LIST:
			unrealPrimType = PT_PointList;
			primCount = vertexCount;
			break;

		case PrimitiveType::TRIANGLE_STRIP:
			unrealPrimType = PT_TriangleStrip;
			primCount = vertexCount - 2;
			break;

		case PrimitiveType::TRIANGLE_LIST:
			unrealPrimType = PT_TriangleList;
			primCount = vertexCount / 3;
			break;

		case PrimitiveType::PATCH_1_CONTROL_POINT:
			unrealPrimType = PT_1_ControlPointPatchList;
			primCount = vertexCount;
			break;

		case PrimitiveType::PATCH_3_CONTROL_POINT:
			unrealPrimType = PT_3_ControlPointPatchList;
			primCount = vertexCount / 3;
			break;

		default: check(0); // unknown prim type
		}
	}

	void FRendererInterfaceD3D12::draw(const DrawCallState& state, const DrawArguments* args, uint32 numDrawCalls)
	{
		checkCommandList();

		applyState(state, nullptr, PT_TriangleList);
		applyResources(state);

		for (uint32 n = 0; n < numDrawCalls; n++)
		{
			uint32 PrimitiveType = 0;
			uint32 PrimitiveCount = 0;

			convertPrimTypeAndCount(state.primType, args[n].vertexCount, PrimitiveType, PrimitiveCount);

			m_RHICmdList->DrawPrimitive(PrimitiveType, args[n].startVertexLocation, PrimitiveCount, args[n].instanceCount);
		}

		unapplyResources(state);
	}

	void FRendererInterfaceD3D12::drawIndexed(const DrawCallState& state, const DrawArguments* args, uint32 numDrawCalls)
	{
		checkNoEntry();
	}

	void FRendererInterfaceD3D12::drawIndirect(const DrawCallState& state, BufferHandle indirectParams, uint32 offsetBytes)
	{
		checkCommandList();

		applyState(state, nullptr, PT_TriangleList);
		applyResources(state);

		uint32 PrimitiveType = 0;
		uint32 PrimitiveCount = 0;

		convertPrimTypeAndCount(state.primType, 0, PrimitiveType, PrimitiveCount);

		m_RHICmdList->DrawIndirect(PrimitiveType, static_cast<FBuffer*>(indirectParams)->BufferRHI, offsetBytes);

		unapplyResources(state);
	}

	void FRendererInterfaceD3D12::dispatch(const DispatchState& state, uint32 groupsX, uint32 groupsY, uint32 groupsZ)
	{
		checkCommandList();

		applyState(state);

		m_RHICmdList->DispatchComputeShader(groupsX, groupsY, groupsZ);

		unapplyState(state);
	}

	void FRendererInterfaceD3D12::dispatchIndirect(const DispatchState& state, BufferHandle indirectParams, uint32 offsetBytes)
	{
		checkCommandList();

		check(indirectParams);

		applyState(state);

		m_RHICmdList->DispatchIndirectComputeShaderStructured(static_cast<FBuffer*>(indirectParams)->BufferRHI, offsetBytes);

		unapplyState(state);
	}

	void FRendererInterfaceD3D12::setModifiedWMode(bool enabled, uint32_t numViewports, const float* pA, const float* pB)
	{
		checkNoEntry();
	}

	void FRendererInterfaceD3D12::setSinglePassStereoMode(bool enabled, uint32_t renderTargetIndexOffset, bool independentViewportMask)
	{
		checkNoEntry();
	}

	uint32 FRendererInterfaceD3D12::getNumberOfAFRGroups()
	{
		return 1;
	}

	uint32 FRendererInterfaceD3D12::getAFRGroupOfCurrentFrame(uint32 numAFRGroups)
	{
		return 0;
	}

	void FRendererInterfaceD3D12::setEnableUavBarriers(bool enableBarriers, const TextureHandle* textures, size_t numTextures, const BufferHandle* buffers, size_t numBuffers)
	{
		checkCommandList();

		enum { MAX_ELEMENTS = 16 };
		TArray<FTextureRHIParamRef, TFixedAllocator<MAX_ELEMENTS>> Textures;
		TArray<FStructuredBufferRHIParamRef, TFixedAllocator<MAX_ELEMENTS>> Buffers;

		check(numTextures < MAX_ELEMENTS);
		for (uint32 TextureIndex = 0; TextureIndex < uint32(numTextures); TextureIndex++)
		{
			Textures.Add(static_cast<FTexture*>(textures[TextureIndex])->TextureRHI);
		}

		check(numBuffers < MAX_ELEMENTS);
		for (uint32 BufferIndex = 0; BufferIndex < uint32(numBuffers); BufferIndex++)
		{
			Buffers.Add(static_cast<FBuffer*>(buffers[BufferIndex])->BufferRHI);
		}

		FTextureRHIParamRef* pTextures = nullptr;
		if (numTextures) pTextures = &Textures[0];

		FStructuredBufferRHIParamRef* pBuffers = nullptr;
		if (numBuffers) pBuffers = &Buffers[0];

		m_RHICmdList->SetEnableUAVBarriers(enableBarriers, pTextures, uint32(numTextures), pBuffers, uint32(numBuffers));
	}

	TextureHandle FRendererInterfaceD3D12::getTextureFromRHI(FRHITexture* TextureRHI)
	{
		if (!TextureRHI) return nullptr;

		ID3D12Resource* pResource = (ID3D12Resource*)TextureRHI->GetNativeResource();

		FTexture* texture = m_UnmanagedTextures[pResource];
		if (texture) return texture;

		D3D12_RESOURCE_DESC desc = pResource->GetDesc();

		texture = new FTexture(this);
		texture->TextureRHI = TextureRHI;
		texture->Desc.width = uint32(desc.Width);
		texture->Desc.height = desc.Height;
		texture->Desc.depthOrArraySize = (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D) ? 0 : desc.DepthOrArraySize;
		texture->Desc.mipLevels = desc.MipLevels;
		texture->Desc.sampleCount = desc.SampleDesc.Count;
		texture->Desc.sampleQuality = desc.SampleDesc.Quality;
		texture->Desc.isArray = desc.DepthOrArraySize > 1 && desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		texture->Desc.isRenderTarget = (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
		texture->Desc.isUAV = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0;

		for (auto& mapping : FormatMappings)
			if (mapping.resourceFormat == desc.Format || mapping.srvFormat == desc.Format || mapping.rtvFormat == desc.Format)
			{
				texture->Desc.format = mapping.abstractFormat;
				break;
			}

		check(desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
		check(texture->Desc.format != Format::UNKNOWN);

		m_UnmanagedTextures[pResource] = texture;
		return texture;
	}

	FRHITexture* FRendererInterfaceD3D12::getRHITexture(TextureHandle texture)
	{
		if (!texture) return nullptr;

		return static_cast<FTexture*>(texture)->TextureRHI;
	}

	void FRendererInterfaceD3D12::releaseUnmanagedTextures()
	{
		for (auto it : m_UnmanagedTextures)
		{
			delete it.second;
		}
		m_UnmanagedTextures.clear();
	}

	FRHIShaderResourceView* FRendererInterfaceD3D12::getTextureSRV(TextureHandle _t, uint32 mipLevel, Format::Enum format)
	{
		FTexture* t = static_cast<FTexture*>(_t);

		if (format == Format::UNKNOWN)
			format = t->Desc.format;

		auto key = std::make_pair(format, mipLevel);
		auto found = t->ShaderResourceViews.find(key);
		if (found != t->ShaderResourceViews.end())
			return found->second;

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};

		SRVDesc.Format = GetFormatMapping(format).srvFormat;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		uint32 firstMip = mipLevel >= t->Desc.mipLevels ? 0 : mipLevel;
		uint32 mipLevels = mipLevel >= t->Desc.mipLevels ? t->Desc.mipLevels : 1;
		uint32 planeSlice = 0;

		if (SRVDesc.Format == DXGI_FORMAT_X24_TYPELESS_G8_UINT)
			planeSlice = 1;

		if (t->Desc.isArray || t->Desc.isCubeMap)
		{
			if (t->Desc.sampleCount > 1)
			{
				SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
				SRVDesc.Texture2DMSArray.ArraySize = t->Desc.depthOrArraySize;
			}
			else if (t->Desc.isCubeMap)
			{
				if (t->Desc.depthOrArraySize > 6)
				{
					SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
					SRVDesc.TextureCubeArray.NumCubes = t->Desc.depthOrArraySize / 6;
					SRVDesc.TextureCubeArray.MostDetailedMip = firstMip;
					SRVDesc.TextureCubeArray.MipLevels = mipLevels;
				}
				else
				{
					SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
					SRVDesc.TextureCube.MostDetailedMip = firstMip;
					SRVDesc.TextureCube.MipLevels = mipLevels;
				}
			}
			else
			{
				SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
				SRVDesc.Texture2DArray.ArraySize = t->Desc.depthOrArraySize;
				SRVDesc.Texture2DArray.MostDetailedMip = firstMip;
				SRVDesc.Texture2DArray.MipLevels = mipLevels;
				SRVDesc.Texture2DArray.PlaneSlice = planeSlice;
			}
		}
		else if (t->Desc.depthOrArraySize > 0)
		{
			SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
			SRVDesc.Texture3D.MostDetailedMip = firstMip;
			SRVDesc.Texture3D.MipLevels = mipLevels;
		}
		else
		{
			if (t->Desc.sampleCount > 1)
			{
				SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
			}
			else
			{
				SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				SRVDesc.Texture2D.MostDetailedMip = firstMip;
				SRVDesc.Texture2D.MipLevels = mipLevels;
				SRVDesc.Texture2D.PlaneSlice = planeSlice;
			}
		}

		FD3D12TextureBase* Texture = GetD3D12TextureFromRHITexture(t->TextureRHI);
		FRHIShaderResourceView* View = new FD3D12ShaderResourceView(m_Device, SRVDesc, Texture->ResourceLocation);
		t->ShaderResourceViews[key] = View;
		return View;
	}

	FRHIUnorderedAccessView* FRendererInterfaceD3D12::getTextureUAV(TextureHandle _t, uint32 mipLevel, Format::Enum format)
	{
		FTexture* t = static_cast<FTexture*>(_t);

		if (format == Format::UNKNOWN)
			format = t->Desc.format;

		auto key = std::make_pair(format, mipLevel);
		auto found = t->UnorderedAccessViews.find(key);
		if (found != t->UnorderedAccessViews.end())
			return found->second;

		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};

		const FormatMapping& mapping = GetFormatMapping(format);
		UAVDesc.Format = mapping.srvFormat;

		if (UAVDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
			UAVDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

		if (t->Desc.isArray || t->Desc.isCubeMap)
		{
			UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
			UAVDesc.Texture2DArray.ArraySize = t->Desc.depthOrArraySize;
			UAVDesc.Texture2DArray.MipSlice = mipLevel;
		}
		else if (t->Desc.depthOrArraySize > 0)
		{
			UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
			UAVDesc.Texture3D.WSize = t->Desc.depthOrArraySize;
			UAVDesc.Texture3D.MipSlice = mipLevel;
		}
		else
		{
			UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			UAVDesc.Texture2D.MipSlice = mipLevel;
		}

		FD3D12TextureBase* Texture = GetD3D12TextureFromRHITexture(t->TextureRHI);
		FRHIUnorderedAccessView* View = new FD3D12UnorderedAccessView(m_Device, UAVDesc, Texture->ResourceLocation); 
		t->UnorderedAccessViews[key] = View;
		return View;
	}

	FRHIShaderResourceView* FRendererInterfaceD3D12::getBufferSRV(BufferHandle _b, Format::Enum format)
	{
		FBuffer* b = static_cast<FBuffer*>(_b);

		if (b->ShaderResourceView)
			return b->ShaderResourceView;

		if (!b->BufferRHI)
			return nullptr;

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};

		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

		uint32 EffectiveStride = 0;

		if (b->Desc.structStride != 0)
		{
			EffectiveStride = b->Desc.structStride;
			SRVDesc.Format = DXGI_FORMAT_UNKNOWN;
			SRVDesc.Buffer.StructureByteStride = b->Desc.structStride;
		}
		else
		{
			const FormatMapping& mapping = GetFormatMapping(format == Format::UNKNOWN ? Format::R32_UINT : format);

			EffectiveStride = mapping.bytesPerPixel;
			SRVDesc.Format = mapping.srvFormat;
		}

		FD3D12StructuredBuffer*  StructuredBuffer = FD3D12DynamicRHI::ResourceCast(b->BufferRHI.GetReference());
		SRVDesc.Buffer.FirstElement = StructuredBuffer->ResourceLocation.GetOffsetFromBaseOfResource() / EffectiveStride;
		SRVDesc.Buffer.NumElements = StructuredBuffer->ResourceLocation.GetSize() / EffectiveStride;

		FRHIShaderResourceView* View = new FD3D12ShaderResourceView(m_Device, SRVDesc, StructuredBuffer->ResourceLocation, EffectiveStride);
		b->ShaderResourceView = View;
		return View;
	}

	FRHIUnorderedAccessView* FRendererInterfaceD3D12::getBufferUAV(BufferHandle _b, Format::Enum format)
	{
		FBuffer* b = static_cast<FBuffer*>(_b);

		if (b->UnorderedAccessView)
			return b->UnorderedAccessView;

		D3D12_UNORDERED_ACCESS_VIEW_DESC UAVDesc = {};

		UAVDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;

		uint32 EffectiveStride = 0;

		if (b->Desc.structStride != 0)
		{
			EffectiveStride = b->Desc.structStride;
			UAVDesc.Format = DXGI_FORMAT_UNKNOWN;
			UAVDesc.Buffer.StructureByteStride = b->Desc.structStride;
		}
		else
		{
			const FormatMapping& mapping = GetFormatMapping(format == Format::UNKNOWN ? Format::R32_UINT : format);

			EffectiveStride = mapping.bytesPerPixel;
			UAVDesc.Format = mapping.srvFormat;
		}

		FD3D12StructuredBuffer*  StructuredBuffer = FD3D12DynamicRHI::ResourceCast(b->BufferRHI.GetReference());
		UAVDesc.Buffer.FirstElement = StructuredBuffer->ResourceLocation.GetOffsetFromBaseOfResource() / EffectiveStride;
		UAVDesc.Buffer.NumElements = StructuredBuffer->ResourceLocation.GetSize() / EffectiveStride;

		FRHIUnorderedAccessView* View = new FD3D12UnorderedAccessView(m_Device, UAVDesc, StructuredBuffer->ResourceLocation);
		b->UnorderedAccessView = View;
		return View;
	}

	FRasterizerStateRHIParamRef FRendererInterfaceD3D12::getRasterizerState(const RasterState& rasterState)
	{
		CrcHash hasher;
		hasher.Add(rasterState);
		uint32 hash = hasher.Get();

		auto it = m_RasterizerStates.find(hash);
		if (it != m_RasterizerStates.end())
			return it->second;

		D3D12_RASTERIZER_DESC RasterizerDesc = {};

		switch (rasterState.fillMode)
		{
		case RasterState::FILL_SOLID:
			RasterizerDesc.FillMode = D3D12_FILL_MODE_SOLID;
			break;
		case RasterState::FILL_LINE:
			RasterizerDesc.FillMode = D3D12_FILL_MODE_WIREFRAME;
			break;
		}

		switch (rasterState.cullMode)
		{
		case RasterState::CULL_BACK:
			RasterizerDesc.CullMode = D3D12_CULL_MODE_BACK;
			break;
		case RasterState::CULL_FRONT:
			RasterizerDesc.CullMode = D3D12_CULL_MODE_FRONT;
			break;
		case RasterState::CULL_NONE:
			RasterizerDesc.CullMode = D3D12_CULL_MODE_NONE;
			break;
		}

		RasterizerDesc.FrontCounterClockwise = rasterState.frontCounterClockwise;
		RasterizerDesc.DepthBias = rasterState.depthBias;
		RasterizerDesc.DepthBiasClamp = rasterState.depthBiasClamp;
		RasterizerDesc.SlopeScaledDepthBias = rasterState.slopeScaledDepthBias;
		RasterizerDesc.DepthClipEnable = rasterState.depthClipEnable;
		RasterizerDesc.MultisampleEnable = rasterState.multisampleEnable;
		RasterizerDesc.AntialiasedLineEnable = rasterState.antialiasedLineEnable;
		RasterizerDesc.ConservativeRaster = rasterState.conservativeRasterEnable ? D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
		RasterizerDesc.ForcedSampleCount = rasterState.forcedSampleCount;

		FD3D12RasterizerState* RasterizerState = new FD3D12RasterizerState();
		RasterizerState->Desc = RasterizerDesc;

		m_RasterizerStates[hash] = RasterizerState;
		return RasterizerState;
	}

	static D3D12_STENCIL_OP convertStencilOp(DepthStencilState::StencilOp value)
	{
		switch (value)
		{
		case DepthStencilState::STENCIL_OP_KEEP:
			return D3D12_STENCIL_OP_KEEP;
		case DepthStencilState::STENCIL_OP_ZERO:
			return D3D12_STENCIL_OP_ZERO;
		case DepthStencilState::STENCIL_OP_REPLACE:
			return D3D12_STENCIL_OP_REPLACE;
		case DepthStencilState::STENCIL_OP_INCR_SAT:
			return D3D12_STENCIL_OP_INCR_SAT;
		case DepthStencilState::STENCIL_OP_DECR_SAT:
			return D3D12_STENCIL_OP_DECR_SAT;
		case DepthStencilState::STENCIL_OP_INVERT:
			return D3D12_STENCIL_OP_INVERT;
		case DepthStencilState::STENCIL_OP_INCR:
			return D3D12_STENCIL_OP_INCR;
		case DepthStencilState::STENCIL_OP_DECR:
			return D3D12_STENCIL_OP_DECR;
		default:
			return D3D12_STENCIL_OP_KEEP;
		}
	}

	static D3D12_COMPARISON_FUNC convertComparisonFunc(DepthStencilState::ComparisonFunc value)
	{
		switch (value)
		{
		case DepthStencilState::COMPARISON_NEVER:
			return D3D12_COMPARISON_FUNC_NEVER;
		case DepthStencilState::COMPARISON_LESS:
			return D3D12_COMPARISON_FUNC_LESS;
		case DepthStencilState::COMPARISON_EQUAL:
			return D3D12_COMPARISON_FUNC_EQUAL;
		case DepthStencilState::COMPARISON_LESS_EQUAL:
			return D3D12_COMPARISON_FUNC_LESS_EQUAL;
		case DepthStencilState::COMPARISON_GREATER:
			return D3D12_COMPARISON_FUNC_GREATER;
		case DepthStencilState::COMPARISON_NOT_EQUAL:
			return D3D12_COMPARISON_FUNC_NOT_EQUAL;
		case DepthStencilState::COMPARISON_GREATER_EQUAL:
			return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
		case DepthStencilState::COMPARISON_ALWAYS:
			return D3D12_COMPARISON_FUNC_ALWAYS;
		default:
			return D3D12_COMPARISON_FUNC_NEVER;
		}
	}

	FDepthStencilStateRHIParamRef FRendererInterfaceD3D12::getDepthStencilState(const DepthStencilState& depthStencilState, bool depthTargetPresent)
	{
		CrcHash hasher;
		hasher.Add(depthStencilState);
		uint32 hash = hasher.Get();

		auto it = m_DepthStencilStates.find(hash);
		if (it != m_DepthStencilStates.end())
			return it->second;

		D3D12_DEPTH_STENCIL_DESC DepthStencilDesc = {};

		DepthStencilDesc.DepthEnable = depthStencilState.depthEnable;
		DepthStencilDesc.DepthWriteMask = depthStencilState.depthWriteMask == DepthStencilState::DEPTH_WRITE_MASK_ALL ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		DepthStencilDesc.DepthFunc = convertComparisonFunc(depthStencilState.depthFunc);
		DepthStencilDesc.StencilEnable = depthStencilState.stencilEnable;
		DepthStencilDesc.StencilReadMask = (UINT8)depthStencilState.stencilReadMask;
		DepthStencilDesc.StencilWriteMask = (UINT8)depthStencilState.stencilWriteMask;
		DepthStencilDesc.FrontFace.StencilFailOp = convertStencilOp(depthStencilState.frontFace.stencilFailOp);
		DepthStencilDesc.FrontFace.StencilDepthFailOp = convertStencilOp(depthStencilState.frontFace.stencilDepthFailOp);
		DepthStencilDesc.FrontFace.StencilPassOp = convertStencilOp(depthStencilState.frontFace.stencilPassOp);
		DepthStencilDesc.FrontFace.StencilFunc = convertComparisonFunc(depthStencilState.frontFace.stencilFunc);
		DepthStencilDesc.BackFace.StencilFailOp = convertStencilOp(depthStencilState.backFace.stencilFailOp);
		DepthStencilDesc.BackFace.StencilDepthFailOp = convertStencilOp(depthStencilState.backFace.stencilDepthFailOp);
		DepthStencilDesc.BackFace.StencilPassOp = convertStencilOp(depthStencilState.backFace.stencilPassOp);
		DepthStencilDesc.BackFace.StencilFunc = convertComparisonFunc(depthStencilState.backFace.stencilFunc);

		if ((depthStencilState.depthEnable || depthStencilState.stencilEnable) && !depthTargetPresent)
		{
			DepthStencilDesc.DepthEnable = false;
			DepthStencilDesc.StencilEnable = false;
		}

		FD3D12DepthStencilState* DepthStencilState = new FD3D12DepthStencilState();
		DepthStencilState->Desc = DepthStencilDesc;

		m_DepthStencilStates[hash] = DepthStencilState;
		return DepthStencilState;
	}

	static D3D12_BLEND convertBlendValue(BlendState::BlendValue value)
	{
		switch (value)
		{
		case BlendState::BLEND_ZERO:
			return D3D12_BLEND_ZERO;
		case BlendState::BLEND_ONE:
			return D3D12_BLEND_ONE;
		case BlendState::BLEND_SRC_COLOR:
			return D3D12_BLEND_SRC_COLOR;
		case BlendState::BLEND_INV_SRC_COLOR:
			return D3D12_BLEND_INV_SRC_COLOR;
		case BlendState::BLEND_SRC_ALPHA:
			return D3D12_BLEND_SRC_ALPHA;
		case BlendState::BLEND_INV_SRC_ALPHA:
			return D3D12_BLEND_INV_SRC_ALPHA;
		case BlendState::BLEND_DEST_ALPHA:
			return D3D12_BLEND_DEST_ALPHA;
		case BlendState::BLEND_INV_DEST_ALPHA:
			return D3D12_BLEND_INV_DEST_ALPHA;
		case BlendState::BLEND_DEST_COLOR:
			return D3D12_BLEND_DEST_COLOR;
		case BlendState::BLEND_INV_DEST_COLOR:
			return D3D12_BLEND_INV_DEST_COLOR;
		case BlendState::BLEND_SRC_ALPHA_SAT:
			return D3D12_BLEND_SRC_ALPHA_SAT;
		case BlendState::BLEND_BLEND_FACTOR:
			return D3D12_BLEND_BLEND_FACTOR;
		case BlendState::BLEND_INV_BLEND_FACTOR:
			return D3D12_BLEND_INV_BLEND_FACTOR;
		case BlendState::BLEND_SRC1_COLOR:
			return D3D12_BLEND_SRC1_COLOR;
		case BlendState::BLEND_INV_SRC1_COLOR:
			return D3D12_BLEND_INV_SRC1_COLOR;
		case BlendState::BLEND_SRC1_ALPHA:
			return D3D12_BLEND_SRC1_ALPHA;
		case BlendState::BLEND_INV_SRC1_ALPHA:
			return D3D12_BLEND_INV_SRC1_ALPHA;
		default:
			return D3D12_BLEND_ZERO;
		}
	}

	static D3D12_BLEND_OP convertBlendOp(BlendState::BlendOp value)
	{
		switch (value)
		{
		case BlendState::BLEND_OP_ADD:
			return D3D12_BLEND_OP_ADD;
		case BlendState::BLEND_OP_SUBTRACT:
			return D3D12_BLEND_OP_SUBTRACT;
		case BlendState::BLEND_OP_REV_SUBTRACT:
			return D3D12_BLEND_OP_REV_SUBTRACT;
		case BlendState::BLEND_OP_MIN:
			return D3D12_BLEND_OP_MIN;
		case BlendState::BLEND_OP_MAX:
			return D3D12_BLEND_OP_MAX;
		default:
			return D3D12_BLEND_OP_ADD;
		}
	}

	FBlendStateRHIParamRef FRendererInterfaceD3D12::getBlendState(const BlendState& blendState)
	{
		CrcHash hasher;
		hasher.Add(blendState);
		uint32 hash = hasher.Get();

		auto it = m_BlendStates.find(hash);
		if (it != m_BlendStates.end())
			return it->second;

		D3D12_BLEND_DESC BlendDesc = {};

		BlendDesc.AlphaToCoverageEnable = blendState.alphaToCoverage;
		BlendDesc.IndependentBlendEnable = true;

		for (uint32_t i = 0; i < ARRAYSIZE(blendState.blendEnable); i++)
		{
			BlendDesc.RenderTarget[i].BlendEnable = blendState.blendEnable[i];
			BlendDesc.RenderTarget[i].SrcBlend = convertBlendValue(blendState.srcBlend[i]);
			BlendDesc.RenderTarget[i].DestBlend = convertBlendValue(blendState.destBlend[i]);
			BlendDesc.RenderTarget[i].BlendOp = convertBlendOp(blendState.blendOp[i]);
			BlendDesc.RenderTarget[i].SrcBlendAlpha = convertBlendValue(blendState.srcBlendAlpha[i]);
			BlendDesc.RenderTarget[i].DestBlendAlpha = convertBlendValue(blendState.destBlendAlpha[i]);
			BlendDesc.RenderTarget[i].BlendOpAlpha = convertBlendOp(blendState.blendOpAlpha[i]);
			BlendDesc.RenderTarget[i].RenderTargetWriteMask =
				(blendState.colorWriteEnable[i] & BlendState::COLOR_MASK_RED ? D3D12_COLOR_WRITE_ENABLE_RED : 0) |
				(blendState.colorWriteEnable[i] & BlendState::COLOR_MASK_GREEN ? D3D12_COLOR_WRITE_ENABLE_GREEN : 0) |
				(blendState.colorWriteEnable[i] & BlendState::COLOR_MASK_BLUE ? D3D12_COLOR_WRITE_ENABLE_BLUE : 0) |
				(blendState.colorWriteEnable[i] & BlendState::COLOR_MASK_ALPHA ? D3D12_COLOR_WRITE_ENABLE_ALPHA : 0);
		}

		FD3D12BlendState* BlendState = new FD3D12BlendState();
		BlendState->Desc = BlendDesc;

		m_BlendStates[hash] = BlendState;
		return BlendState;
	}

	template<typename ShaderType>
	void FRendererInterfaceD3D12::applyShaderState(PipelineStageBindings bindings)
	{
		checkCommandList();

		if (!bindings.shader)
			return;

		ShaderType shader = static_cast<ShaderType>(FShader::Unwrap(bindings.shader));

		for (uint32 n = 0; n < bindings.constantBufferBindingCount; n++)
		{
			const auto& binding = bindings.constantBuffers[n];
			FConstantBuffer* cbuffer = static_cast<FConstantBuffer*>(binding.buffer);
			check(cbuffer);
			check(cbuffer->UniformBufferRHI);
			m_RHICmdList->SetShaderUniformBuffer(shader, binding.slot, cbuffer->UniformBufferRHI);
		}

		for (uint32 n = 0; n < bindings.textureBindingCount; n++)
		{
			const auto& binding = bindings.textures[n];

			// UAVs are handled elsewhere
			if (!binding.isWritable)
			{
				m_RHICmdList->SetShaderResourceViewParameter(shader, binding.slot, getTextureSRV(binding.texture, binding.mipLevel, binding.format));
			}
		}

		for (uint32 n = 0; n < bindings.bufferBindingCount; n++)
		{
			const auto& binding = bindings.buffers[n];

			// UAVs are handled elsewhere
			if (!binding.isWritable)
			{
				m_RHICmdList->SetShaderResourceViewParameter(shader, binding.slot, getBufferSRV(binding.buffer, binding.format));
			}
		}

		for (uint32 n = 0; n < bindings.textureSamplerBindingCount; n++)
		{
			const auto& binding = bindings.textureSamplers[n];

			m_RHICmdList->SetShaderSampler(shader, binding.slot, FSampler::Unwrap(binding.sampler));
		}
	}

	template<typename ShaderType>
	void FRendererInterfaceD3D12::unapplyShaderState(PipelineStageBindings bindings)
	{
		checkCommandList();

		if (!bindings.shader)
			return;

		ShaderType shader = static_cast<ShaderType>(FShader::Unwrap(bindings.shader));

		for (uint32 n = 0; n < bindings.constantBufferBindingCount; n++)
		{
			const auto& binding = bindings.constantBuffers[n];
			m_RHICmdList->SetShaderUniformBuffer(shader, binding.slot, nullptr);
		}

		for (uint32 n = 0; n < bindings.textureBindingCount; n++)
		{
			const auto& binding = bindings.textures[n];

			// UAVs are handled elsewhere
			if (!binding.isWritable)
			{
				m_RHICmdList->SetShaderResourceViewParameter(shader, binding.slot, nullptr);
			}
		}

		for (uint32 n = 0; n < bindings.bufferBindingCount; n++)
		{
			const auto& binding = bindings.buffers[n];

			// UAVs are handled elsewhere
			if (!binding.isWritable)
			{
				m_RHICmdList->SetShaderResourceViewParameter(shader, binding.slot, nullptr);
			}
		}
	}

	void FRendererInterfaceD3D12::applyState(DrawCallState state, const FBoundShaderStateInput* BoundShaderStateInput, EPrimitiveType PrimitiveTypeOverride)
	{
		checkCommandList();

		check(state.inputLayout == nullptr);

		FGraphicsPipelineStateInitializer InitPSO;

		if (!BoundShaderStateInput)
		{
			InitPSO.BoundShaderState.VertexShaderRHI = static_cast<FVertexShaderRHIParamRef>(FShader::Unwrap(state.VS.shader));
			InitPSO.BoundShaderState.HullShaderRHI = static_cast<FHullShaderRHIParamRef>(FShader::Unwrap(state.HS.shader));
			InitPSO.BoundShaderState.DomainShaderRHI = static_cast<FDomainShaderRHIParamRef>(FShader::Unwrap(state.DS.shader));
			InitPSO.BoundShaderState.GeometryShaderRHI = static_cast<FGeometryShaderRHIParamRef>(FShader::Unwrap(state.GS.shader));
			InitPSO.BoundShaderState.PixelShaderRHI = static_cast<FPixelShaderRHIParamRef>(FShader::Unwrap(state.PS.shader));

			uint32 primType = 0;
			uint32 primCount = 0;
			convertPrimTypeAndCount(state.primType, 0, primType, primCount);
			InitPSO.PrimitiveType = EPrimitiveType(primType);
		}
		else
		{
			InitPSO.BoundShaderState = *BoundShaderStateInput;
			InitPSO.PrimitiveType = PrimitiveTypeOverride;
		}

		FRHIRenderTargetView RTVs[8] = {};
		FRHIDepthRenderTargetView DSV;
		FRHIDepthRenderTargetView* pDSV = nullptr;
		FUnorderedAccessViewRHIParamRef pUAVs[8] = {};

		for (uint32 RTVIndex = 0; RTVIndex < state.renderState.targetCount; RTVIndex++)
		{
			FTexture* Target = static_cast<FTexture*>(state.renderState.targets[RTVIndex]);

			RTVs[RTVIndex] = FRHIRenderTargetView(
				Target->TextureRHI,
				state.renderState.targetMipSlices[RTVIndex],
				state.renderState.targetIndicies[RTVIndex],
				ERenderTargetLoadAction::ELoad,
				ERenderTargetStoreAction::EStore
			);

			InitPSO.RenderTargetFormats[RTVIndex] = Target->TextureRHI->GetFormat();
			InitPSO.RenderTargetFlags[RTVIndex] = Target->TextureRHI->GetFlags();
			InitPSO.RenderTargetLoadActions[RTVIndex] = ERenderTargetLoadAction::ELoad;
			InitPSO.RenderTargetStoreActions[RTVIndex] = ERenderTargetStoreAction::EStore;

			if (InitPSO.NumSamples == 0)
				InitPSO.NumSamples = Target->TextureRHI->GetNumSamples();
		}

		InitPSO.RenderTargetsEnabled = state.renderState.targetCount;

		if (state.renderState.depthTarget)
		{
			check(state.renderState.depthIndex == 0);
			check(state.renderState.depthMipSlice == 0);

			FTexture* Target = static_cast<FTexture*>(state.renderState.depthTarget);

			DSV = FRHIDepthRenderTargetView(Target->TextureRHI, ERenderTargetLoadAction::ELoad, ERenderTargetStoreAction::EStore);
			pDSV = &DSV;

			InitPSO.DepthStencilTargetFormat = Target->TextureRHI->GetFormat();
			InitPSO.DepthStencilTargetFlag = Target->TextureRHI->GetFlags();
			InitPSO.DepthTargetLoadAction = ERenderTargetLoadAction::ELoad;
			InitPSO.DepthTargetStoreAction = ERenderTargetStoreAction::EStore;
			InitPSO.StencilTargetLoadAction = ERenderTargetLoadAction::ELoad;
			InitPSO.StencilTargetStoreAction = ERenderTargetStoreAction::EStore;

			if (InitPSO.NumSamples == 0)
				InitPSO.NumSamples = Target->TextureRHI->GetNumSamples();
		}

		InitPSO.RasterizerState = getRasterizerState(state.renderState.rasterState);
		InitPSO.DepthStencilState = getDepthStencilState(state.renderState.depthStencilState, state.renderState.depthTarget != nullptr);
		InitPSO.BlendState = getBlendState(state.renderState.blendState);

		uint32 NumUAVs = 0;

		for (uint32 n = 0; n < state.PS.textureBindingCount; n++)
		{
			const auto& binding = state.PS.textures[n];

			if (binding.isWritable)
			{
				check(binding.slot >= state.renderState.targetCount);
				check(binding.slot < 8);

				uint32 UAVIndex = binding.slot - state.renderState.targetCount;
				pUAVs[UAVIndex] = getTextureUAV(binding.texture, binding.mipLevel, binding.format);
				NumUAVs = FMath::Max(NumUAVs, UAVIndex + 1);
			}
		}

		for (uint32 n = 0; n < state.PS.bufferBindingCount; n++)
		{
			const auto& binding = state.PS.buffers[n];

			if (binding.isWritable)
			{
				check(binding.slot >= state.renderState.targetCount);
				check(binding.slot < 8);

				uint32 UAVIndex = binding.slot - state.renderState.targetCount;
				pUAVs[UAVIndex] = getBufferUAV(binding.buffer, binding.format);
				NumUAVs = FMath::Max(NumUAVs, UAVIndex + 1);
			}
		}

		FRHISetRenderTargetsInfo Info;

		for (uint32 RTVIndex = 0; RTVIndex < state.renderState.targetCount; RTVIndex++)
		{
			Info.ColorRenderTarget[RTVIndex] = RTVs[RTVIndex];
		}
		Info.NumColorRenderTargets = state.renderState.targetCount;
		Info.DepthStencilRenderTarget = DSV;

		for (uint32 UAVIndex = 0; UAVIndex < NumUAVs; UAVIndex++)
		{
			Info.UnorderedAccessView[UAVIndex] = pUAVs[UAVIndex];
		}
		Info.NumUAVs = NumUAVs;

		Info.bClearColor = state.renderState.clearColorTarget;
		Info.bClearDepth = state.renderState.clearDepthTarget;
		Info.bClearStencil = state.renderState.clearStencilTarget;

		m_RHICmdList->SetRenderTargetsAndClear(Info);

		SetGraphicsPipelineState(*m_RHICmdList, InitPSO);

		FLinearColor BlendFactors(state.renderState.blendState.blendFactor.r, state.renderState.blendState.blendFactor.g, state.renderState.blendState.blendFactor.b, state.renderState.blendState.blendFactor.a);
		m_RHICmdList->SetBlendFactor(BlendFactors);
		m_RHICmdList->SetStencilRef(state.renderState.depthStencilState.stencilRefValue);

		FViewportBounds Viewports[16];
		FScissorRect ScissorRects[16];

		for (uint32 vp = 0; vp < state.renderState.viewportCount; vp++)
		{
			Viewports[vp].TopLeftX = state.renderState.viewports[vp].minX;
			Viewports[vp].TopLeftY = state.renderState.viewports[vp].minY;
			Viewports[vp].Width = state.renderState.viewports[vp].maxX - state.renderState.viewports[vp].minX;
			Viewports[vp].Height = state.renderState.viewports[vp].maxY - state.renderState.viewports[vp].minY;
			Viewports[vp].MinDepth = state.renderState.viewports[vp].minZ;
			Viewports[vp].MaxDepth = state.renderState.viewports[vp].maxZ;

			if (state.renderState.rasterState.scissorEnable)
			{
				ScissorRects[vp].Left = state.renderState.scissorRects[vp].minX;
				ScissorRects[vp].Top = state.renderState.scissorRects[vp].minY;
				ScissorRects[vp].Right = state.renderState.scissorRects[vp].maxX;
				ScissorRects[vp].Bottom = state.renderState.scissorRects[vp].maxY;
			}
			else
			{
				ScissorRects[vp].Left = 0;
				ScissorRects[vp].Top = 0;
				ScissorRects[vp].Right = GetMax2DTextureDimension();
				ScissorRects[vp].Bottom = GetMax2DTextureDimension();
			}
		}

		m_RHICmdList->SetViewportsAndScissorRects(state.renderState.viewportCount, Viewports, ScissorRects);
	}

	void FRendererInterfaceD3D12::applyResources(DrawCallState state)
	{
		checkCommandList();

		applyShaderState<FVertexShaderRHIParamRef>(state.VS);
		applyShaderState<FHullShaderRHIParamRef>(state.HS);
		applyShaderState<FDomainShaderRHIParamRef>(state.DS);
		applyShaderState<FGeometryShaderRHIParamRef>(state.GS);
		applyShaderState<FPixelShaderRHIParamRef>(state.PS);
	}

	void FRendererInterfaceD3D12::unapplyResources(DrawCallState state)
	{
		checkCommandList();

		unapplyShaderState<FVertexShaderRHIParamRef>(state.VS);
		unapplyShaderState<FHullShaderRHIParamRef>(state.HS);
		unapplyShaderState<FDomainShaderRHIParamRef>(state.DS);
		unapplyShaderState<FGeometryShaderRHIParamRef>(state.GS);
		unapplyShaderState<FPixelShaderRHIParamRef>(state.PS);
	}

	void FRendererInterfaceD3D12::applyState(DispatchState state)
	{
		checkCommandList();

		FComputeShaderRHIParamRef ComputeShader = static_cast<FComputeShaderRHIParamRef>(FShader::Unwrap(state.shader));

		m_RHICmdList->SetComputeShader(ComputeShader);
		m_RHICmdList->SetRenderTargets(0, nullptr, nullptr, 0, nullptr);

		for (uint32 n = 0; n < state.textureBindingCount; n++)
		{
			const auto& binding = state.textures[n];

			if (binding.isWritable)
			{
				check(binding.slot < 8);

				m_RHICmdList->SetUAVParameter(ComputeShader, binding.slot, getTextureUAV(binding.texture, binding.mipLevel, binding.format));
			}
		}

		for (uint32 n = 0; n < state.bufferBindingCount; n++)
		{
			const auto& binding = state.buffers[n];

			if (binding.isWritable)
			{
				check(binding.slot < 8);

				m_RHICmdList->SetUAVParameter(ComputeShader, binding.slot, getBufferUAV(binding.buffer, binding.format));
			}
		}

		applyShaderState<FComputeShaderRHIParamRef>(state);
	}

	void FRendererInterfaceD3D12::unapplyState(DispatchState state)
	{
		checkCommandList();

		FComputeShaderRHIParamRef ComputeShader = static_cast<FComputeShaderRHIParamRef>(FShader::Unwrap(state.shader));

		for (uint32 n = 0; n < state.textureBindingCount; n++)
		{
			const auto& binding = state.textures[n];

			if (binding.isWritable)
			{
				m_RHICmdList->SetUAVParameter(ComputeShader, binding.slot, nullptr);
			}
		}

		for (uint32 n = 0; n < state.bufferBindingCount; n++)
		{
			const auto& binding = state.buffers[n];

			if (binding.isWritable)
			{
				check(binding.slot < 8);

				m_RHICmdList->SetUAVParameter(ComputeShader, binding.slot, nullptr);
			}
		}

		unapplyShaderState<FComputeShaderRHIParamRef>(state);
	}

	void FRendererInterfaceD3D12::setRHICommandList(FRHICommandList* RHICmdList)
	{
		m_RHICmdList = RHICmdList;
		m_RHIThreadId = RHICmdList ? FPlatformTLS::GetCurrentThreadId() : 0;
	}

	void FRendererInterfaceD3D12::checkCommandList()
	{
		check(m_RHICmdList);
		check(m_RHIThreadId == FPlatformTLS::GetCurrentThreadId());
	}

	void FRendererInterfaceD3D12::beginSection(const char* pSectionName)
	{
		checkCommandList();

		m_RHICmdList->PushEvent(ANSI_TO_TCHAR(pSectionName), FColor::Yellow);
	}

	void FRendererInterfaceD3D12::endSection()
	{
		checkCommandList();

		m_RHICmdList->PopEvent();
	}

	void FRendererInterfaceD3D12::beginRenderingPass()
	{ }

	void FRendererInterfaceD3D12::endRenderingPass()
	{ }
}

#endif
