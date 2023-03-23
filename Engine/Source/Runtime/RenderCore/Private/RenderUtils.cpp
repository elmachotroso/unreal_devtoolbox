// Copyright Epic Games, Inc. All Rights Reserved.

#include "RenderUtils.h"
#include "Containers/ResourceArray.h"
#include "Containers/DynamicRHIResourceArray.h"
#include "RenderResource.h"
#include "RHIStaticStates.h"
#include "RenderGraphUtils.h"
#include "PipelineStateCache.h"
#include "Misc/ConfigCacheIni.h"
#include "RenderGraphResourcePool.h"
#include "Misc/DataDrivenPlatformInfoRegistry.h"

#if WITH_EDITOR
#include "Misc/CoreMisc.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "RHIShaderFormatDefinitions.inl"
#endif

// This is a per-project master switch for Nanite, that influences the shader permutations compiled. Changing it will cause shaders to be recompiled.
int32 GNaniteProjectEnabled = 1;
FAutoConsoleVariableRef CVarAllowNanite(
	TEXT("r.Nanite.ProjectEnabled"),
	GNaniteProjectEnabled,
	TEXT("This setting allows you to disable Nanite on platforms that support it to reduce the number of shaders. It cannot be used to force Nanite on on unsupported platforms.\n"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe
);

FBufferWithRDG::FBufferWithRDG() = default;
FBufferWithRDG::FBufferWithRDG(const FBufferWithRDG & Other) = default;
FBufferWithRDG& FBufferWithRDG::operator=(const FBufferWithRDG & Other) = default;
FBufferWithRDG::~FBufferWithRDG() = default;

void FBufferWithRDG::ReleaseRHI()
{
	Buffer = nullptr;
	FRenderResource::ReleaseRHI();
}

const uint16 GCubeIndices[12*3] =
{
	0, 2, 3,
	0, 3, 1,
	4, 5, 7,
	4, 7, 6,
	0, 1, 5,
	0, 5, 4,
	2, 6, 7,
	2, 7, 3,
	0, 4, 6,
	0, 6, 2,
	1, 3, 7,
	1, 7, 5,
};

TGlobalResource<FCubeIndexBuffer> GCubeIndexBuffer;
TGlobalResource<FTwoTrianglesIndexBuffer> GTwoTrianglesIndexBuffer;
TGlobalResource<FScreenSpaceVertexBuffer> GScreenSpaceVertexBuffer;
TGlobalResource<FTileVertexDeclaration> GTileVertexDeclaration;

//
// FPackedNormal serializer
//
FArchive& operator<<(FArchive& Ar, FDeprecatedSerializedPackedNormal& N)
{
	Ar << N.Vector.Packed;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FPackedNormal& N)
{
	Ar << N.Vector.Packed;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FPackedRGBA16N& N)
{
	Ar << N.X;
	Ar << N.Y;
	Ar << N.Z;
	Ar << N.W;
	return Ar;
}

/**
 * Bulk data interface for providing a single black color used to initialize a
 * volume texture.
 */
class FBlackVolumeTextureResourceBulkDataInterface : public FResourceBulkDataInterface
{
public:

	/** Default constructor. */
	FBlackVolumeTextureResourceBulkDataInterface(uint8 Alpha)
		: Color(0, 0, 0, Alpha)
	{
	}

	/** Default constructor. */
	FBlackVolumeTextureResourceBulkDataInterface(FColor InColor)
		: Color(InColor)
	{
	}

	/**
	 * Returns a pointer to the bulk data.
	 */
	virtual const void* GetResourceBulkData() const override
	{
		return &Color;
	}

	/** 
	 * @return size of resource memory
	 */
	virtual uint32 GetResourceBulkDataSize() const override
	{
		return sizeof(Color);
	}

	/**
	 * Free memory after it has been used to initialize RHI resource 
	 */
	virtual void Discard() override
	{
	}

private:

	/** Storage for the color. */
	FColor Color;
};

//
// FWhiteTexture implementation
//

/**
 * A solid-colored 1x1 texture.
 */
template <int32 R, int32 G, int32 B, int32 A>
class FColoredTexture : public FTextureWithSRV
{
public:
	// FResource interface.
	virtual void InitRHI() override
	{
		// Create the texture RHI.  		
		FBlackVolumeTextureResourceBulkDataInterface BlackTextureBulkData(FColor(R, G, B, A));
		FRHIResourceCreateInfo CreateInfo(TEXT("ColoredTexture"), &BlackTextureBulkData);
		ETextureCreateFlags CreateFlags = TexCreate_ShaderResource;
		// BGRA typed UAV is unsupported per D3D spec, use RGBA here.
		FTexture2DRHIRef Texture2D = RHICreateTexture2D(1, 1, PF_R8G8B8A8, 1, 1, CreateFlags, CreateInfo);
		TextureRHI = Texture2D;

		// Create the sampler state RHI resource.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point,AM_Wrap,AM_Wrap,AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);

		// Create a view of the texture
		ShaderResourceViewRHI = RHICreateShaderResourceView(TextureRHI, 0u);
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/** Returns the height of the texture in pixels. */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}
};

class FEmptyVertexBuffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI() override
	{
		// Create the texture RHI.  		
		FRHIResourceCreateInfo CreateInfo(TEXT("EmptyVertexBuffer"));
		
		VertexBufferRHI = RHICreateVertexBuffer(16u, BUF_Static | BUF_ShaderResource | BUF_UnorderedAccess, CreateInfo);

		// Create a view of the buffer
		ShaderResourceViewRHI = RHICreateShaderResourceView(VertexBufferRHI, 4u, PF_R32_UINT);
		UnorderedAccessViewRHI = RHICreateUnorderedAccessView(VertexBufferRHI, PF_R32_UINT);
	}
};

class FBlackTextureWithSRV : public FColoredTexture<0, 0, 0, 255>
{
	virtual void InitRHI() override
	{
		FColoredTexture::InitRHI();
		FRHITextureReference::DefaultTexture = TextureRHI;
	}

	virtual void ReleaseRHI() override
	{
		FRHITextureReference::DefaultTexture.SafeRelease();
		FColoredTexture::ReleaseRHI();
	}
};

FTextureWithSRV* GWhiteTextureWithSRV = new TGlobalResource<FColoredTexture<255,255,255,255> >;
FTextureWithSRV* GBlackTextureWithSRV = new TGlobalResource<FBlackTextureWithSRV>();
FTextureWithSRV* GTransparentBlackTextureWithSRV = new TGlobalResource<FColoredTexture<0,0,0,0> >;
FTexture* GWhiteTexture = GWhiteTextureWithSRV;
FTexture* GBlackTexture = GBlackTextureWithSRV;
FTexture* GTransparentBlackTexture = GTransparentBlackTextureWithSRV;

FVertexBufferWithSRV* GEmptyVertexBufferWithUAV = new TGlobalResource<FEmptyVertexBuffer>;

class FWhiteVertexBuffer : public FVertexBufferWithSRV
{
public:
	virtual void InitRHI() override
	{
		// Create the texture RHI.  		
		FRHIResourceCreateInfo CreateInfo(TEXT("WhiteVertexBuffer"));

		VertexBufferRHI = RHICreateVertexBuffer(sizeof(FVector4f), BUF_Static | BUF_ShaderResource, CreateInfo);

		FVector4f* BufferData = (FVector4f*)RHILockBuffer(VertexBufferRHI, 0, sizeof(FVector4f), RLM_WriteOnly);
		*BufferData = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		RHIUnlockBuffer(VertexBufferRHI);

		// Create a view of the buffer
		ShaderResourceViewRHI = RHICreateShaderResourceView(VertexBufferRHI, sizeof(FVector4f), PF_A32B32G32R32F);
	}
};

FVertexBufferWithSRV* GWhiteVertexBufferWithSRV = new TGlobalResource<FWhiteVertexBuffer>;

class FWhiteVertexBufferWithRDG : public FBufferWithRDG
{
public:

	/**
	 * Initialize RHI resources.
	 */
	virtual void InitRHI() override
	{
		if (!Buffer.IsValid())
		{
			FRHICommandList* UnusedCmdList = new FRHICommandList(FRHIGPUMask::All());
			GetPooledFreeBuffer(*UnusedCmdList, FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), 1), Buffer, TEXT("WhiteVertexBufferWithRDG"));

			FVector4f* BufferData = (FVector4f*)RHILockBuffer(Buffer->GetRHI(), 0, sizeof(FVector4f), RLM_WriteOnly);
			*BufferData = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
			RHIUnlockBuffer(Buffer->GetRHI());
			delete UnusedCmdList;
			UnusedCmdList = nullptr;
		}
	}
};

FBufferWithRDG* GWhiteVertexBufferWithRDG = new TGlobalResource<FWhiteVertexBufferWithRDG>();

/**
 * A class representing a 1x1x1 black volume texture.
 */
template <EPixelFormat PixelFormat, uint8 Alpha>
class FBlackVolumeTexture : public FTexture
{
public:
	
	/**
	 * Initialize RHI resources.
	 */
	virtual void InitRHI() override
	{
		const TCHAR* Name = TEXT("BlackVolumeTexture");

		if (GSupportsTexture3D)
		{
			// Create the texture.
			FBlackVolumeTextureResourceBulkDataInterface BlackTextureBulkData(Alpha);
			FRHIResourceCreateInfo CreateInfo(TEXT("BlackVolumeTexture3D"), &BlackTextureBulkData);
			FTexture3DRHIRef Texture3D = RHICreateTexture3D(1,1,1,PixelFormat,1,TexCreate_ShaderResource,CreateInfo);
			TextureRHI = Texture3D;	
		}
		else
		{
			// Create a texture, even though it's not a volume texture
			FBlackVolumeTextureResourceBulkDataInterface BlackTextureBulkData(Alpha);
			FRHIResourceCreateInfo CreateInfo(TEXT("BlackVolumeTexture2D"), &BlackTextureBulkData);
			FTexture2DRHIRef Texture2D = RHICreateTexture2D(1, 1, PixelFormat, 1, 1, TexCreate_ShaderResource, CreateInfo);
			TextureRHI = Texture2D;
		}

		// Create the sampler state.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point, AM_Wrap, AM_Wrap, AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);
	}

	/**
	 * Return the size of the texture in the X dimension.
	 */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/**
	 * Return the size of the texture in the Y dimension.
	 */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}
};

/** Global black volume texture resource. */
FTexture* GBlackVolumeTexture = new TGlobalResource<FBlackVolumeTexture<PF_B8G8R8A8, 0>>();
FTexture* GBlackAlpha1VolumeTexture = new TGlobalResource<FBlackVolumeTexture<PF_B8G8R8A8, 255>>();

/** Global black volume texture resource. */
FTexture* GBlackUintVolumeTexture = new TGlobalResource<FBlackVolumeTexture<PF_R8G8B8A8_UINT, 0>>();

class FBlackArrayTexture : public FTexture
{
public:
	// FResource interface.
	virtual void InitRHI() override
	{
		// Create the texture RHI.
		FBlackVolumeTextureResourceBulkDataInterface BlackTextureBulkData(0);
		FRHIResourceCreateInfo CreateInfo(TEXT("BlackArrayTexture"), &BlackTextureBulkData);
		FTexture2DArrayRHIRef TextureArray = RHICreateTexture2DArray(1, 1, 1, PF_B8G8R8A8, 1, 1, TexCreate_ShaderResource, CreateInfo);
		TextureRHI = TextureArray;

		// Create the sampler state RHI resource.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point, AM_Wrap, AM_Wrap, AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/** Returns the height of the texture in pixels. */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}
};

FTexture* GBlackArrayTexture = new TGlobalResource<FBlackArrayTexture>;

//
// FMipColorTexture implementation
//

/**
 * A texture that has a different solid color in each mip-level
 */
class FMipColorTexture : public FTexture
{
public:
	enum
	{
		NumMips = 12
	};
	static const FColor MipColors[NumMips];

	// FResource interface.
	virtual void InitRHI() override
	{
		// Create the texture RHI.
		int32 TextureSize = 1 << (NumMips - 1);
		FRHIResourceCreateInfo CreateInfo(TEXT("FMipColorTexture"));
		FTexture2DRHIRef Texture2D = RHICreateTexture2D(TextureSize,TextureSize,PF_B8G8R8A8,NumMips,1,TexCreate_ShaderResource,CreateInfo);
		TextureRHI = Texture2D;

		// Write the contents of the texture.
		uint32 DestStride;
		int32 Size = TextureSize;
		for ( int32 MipIndex=0; MipIndex < NumMips; ++MipIndex )
		{
			FColor* DestBuffer = (FColor*)RHILockTexture2D(Texture2D, MipIndex, RLM_WriteOnly, DestStride, false);
			for ( int32 Y=0; Y < Size; ++Y )
			{
				for ( int32 X=0; X < Size; ++X )
				{
					DestBuffer[X] = MipColors[NumMips - 1 - MipIndex];
				}
				DestBuffer += DestStride / sizeof(FColor);
			}
			RHIUnlockTexture2D(Texture2D, MipIndex, false);
			Size >>= 1;
		}

		// Create the sampler state RHI resource.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point,AM_Wrap,AM_Wrap,AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		int32 TextureSize = 1 << (NumMips - 1);
		return TextureSize;
	}

	/** Returns the height of the texture in pixels. */
	// PVS-Studio notices that the implementation of GetSizeX is identical to this one
	// and warns us. In this case, it is intentional, so we disable the warning:
	virtual uint32 GetSizeY() const override //-V524
	{
		int32 TextureSize = 1 << (NumMips - 1);
		return TextureSize;
	}
};

const FColor FMipColorTexture::MipColors[NumMips] =
{
	FColor(  80,  80,  80, 0 ),		// Mip  0: 1x1			(dark grey)
	FColor( 200, 200, 200, 0 ),		// Mip  1: 2x2			(light grey)
	FColor( 200, 200,   0, 0 ),		// Mip  2: 4x4			(medium yellow)
	FColor( 255, 255,   0, 0 ),		// Mip  3: 8x8			(yellow)
	FColor( 160, 255,  40, 0 ),		// Mip  4: 16x16		(light green)
	FColor(   0, 255,   0, 0 ),		// Mip  5: 32x32		(green)
	FColor(   0, 255, 200, 0 ),		// Mip  6: 64x64		(cyan)
	FColor(   0, 170, 170, 0 ),		// Mip  7: 128x128		(light blue)
	FColor(  60,  60, 255, 0 ),		// Mip  8: 256x256		(dark blue)
	FColor( 255,   0, 255, 0 ),		// Mip  9: 512x512		(pink)
	FColor( 255,   0,   0, 0 ),		// Mip 10: 1024x1024	(red)
	FColor( 255, 130,   0, 0 ),		// Mip 11: 2048x2048	(orange)
};

RENDERCORE_API FTexture* GMipColorTexture = new FMipColorTexture;
RENDERCORE_API int32 GMipColorTextureMipLevels = FMipColorTexture::NumMips;

// 4: 8x8 cubemap resolution, shader needs to use the same value as preprocessing
RENDERCORE_API const uint32 GDiffuseConvolveMipLevel = 4;

/** A solid color cube texture. */
class FSolidColorTextureCube : public FTexture
{
public:
	FSolidColorTextureCube(const FColor& InColor)
		: bInitToZero(false)
		, PixelFormat(PF_B8G8R8A8)
		, ColorData(InColor.DWColor())
	{}

	FSolidColorTextureCube(EPixelFormat InPixelFormat)
		: bInitToZero(true)
		, PixelFormat(InPixelFormat)
		, ColorData(0)
	{}

	// FRenderResource interface.
	virtual void InitRHI() override
	{
		// Create the texture RHI.
		const TCHAR* Name = TEXT("SolidColorCube");

		FRHIResourceCreateInfo CreateInfo(Name);
		FTextureCubeRHIRef TextureCube = RHICreateTextureCube(1, (uint8)PixelFormat, 1, TexCreate_ShaderResource, CreateInfo);
		TextureRHI = TextureCube;

		// Write the contents of the texture.
		for (uint32 FaceIndex = 0; FaceIndex < 6; FaceIndex++)
		{
			uint32 DestStride;
			void* DestBuffer = RHILockTextureCubeFace(TextureCube, FaceIndex, 0, 0, RLM_WriteOnly, DestStride, false);
			if (bInitToZero)
			{
				FMemory::Memzero(DestBuffer, GPixelFormats[PixelFormat].BlockBytes);
			}
			else
			{
				FMemory::Memcpy(DestBuffer, &ColorData, sizeof(ColorData));
			}
			RHIUnlockTextureCubeFace(TextureCube, FaceIndex, 0, 0, false);
		}

		// Create the sampler state RHI resource.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point, AM_Wrap, AM_Wrap, AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/** Returns the height of the texture in pixels. */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}

private:
	const bool bInitToZero;
	const EPixelFormat PixelFormat;
	const uint32 ColorData;
};

/** A white cube texture. */
class FWhiteTextureCube : public FSolidColorTextureCube
{
public:
	FWhiteTextureCube() : FSolidColorTextureCube(FColor::White) {}
};
FTexture* GWhiteTextureCube = new TGlobalResource<FWhiteTextureCube>;

/** A black cube texture. */
class FBlackTextureCube : public FSolidColorTextureCube
{
public:
	FBlackTextureCube() : FSolidColorTextureCube(FColor::Black) {}
};
FTexture* GBlackTextureCube = new TGlobalResource<FBlackTextureCube>;

/** A black cube texture. */
class FBlackTextureDepthCube : public FSolidColorTextureCube
{
public:
	FBlackTextureDepthCube() : FSolidColorTextureCube(PF_ShadowDepth) {}
};
FTexture* GBlackTextureDepthCube = new TGlobalResource<FBlackTextureDepthCube>;

class FBlackCubeArrayTexture : public FTexture
{
public:
	// FResource interface.
	virtual void InitRHI() override
	{
		if (SupportsTextureCubeArray(GetFeatureLevel() ))
		{
			const TCHAR* Name = TEXT("BlackCubeArray");

			// Create the texture RHI.
			FRHIResourceCreateInfo CreateInfo(Name);
			FTextureCubeRHIRef TextureCubeArray = RHICreateTextureCubeArray(1,1,PF_B8G8R8A8,1,TexCreate_ShaderResource,CreateInfo);
			TextureRHI = TextureCubeArray;

			for(uint32 FaceIndex = 0;FaceIndex < 6;FaceIndex++)
			{
				uint32 DestStride;
				FColor* DestBuffer = (FColor*)RHILockTextureCubeFace(TextureCubeArray, FaceIndex, 0, 0, RLM_WriteOnly, DestStride, false);
				// Note: alpha is used by reflection environment to say how much of the foreground texture is visible, so 0 says it is completely invisible
				*DestBuffer = FColor(0, 0, 0, 0);
				RHIUnlockTextureCubeFace(TextureCubeArray, FaceIndex, 0, 0, false);
			}

			// Create the sampler state RHI resource.
			FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point,AM_Wrap,AM_Wrap,AM_Wrap);
			SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);
		}
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/** Returns the height of the texture in pixels. */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}
};
FTexture* GBlackCubeArrayTexture = new TGlobalResource<FBlackCubeArrayTexture>;

/**
 * A UINT 1x1 texture.
 */
template <EPixelFormat Format, uint32 R = 0, uint32 G = 0, uint32 B = 0, uint32 A = 0>
class FUintTexture : public FTextureWithSRV
{
public:
	// FResource interface.
	virtual void InitRHI() override
	{
		// Create the texture RHI.  		
		FRHIResourceCreateInfo CreateInfo(TEXT("UintTexture"));
		FTexture2DRHIRef Texture2D = RHICreateTexture2D(1, 1, Format, 1, 1, TexCreate_ShaderResource, CreateInfo);
		TextureRHI = Texture2D;

		// Write the contents of the texture.
		uint32 DestStride;
		void* DestBuffer = RHILockTexture2D(Texture2D, 0, RLM_WriteOnly, DestStride, false);
		WriteData(DestBuffer);
		RHIUnlockTexture2D(Texture2D, 0, false);

		// Create the sampler state RHI resource.
		FSamplerStateInitializerRHI SamplerStateInitializer(SF_Point, AM_Wrap, AM_Wrap, AM_Wrap);
		SamplerStateRHI = GetOrCreateSamplerState(SamplerStateInitializer);

		// Create a view of the texture
		ShaderResourceViewRHI = RHICreateShaderResourceView(TextureRHI, 0u);
	}

	/** Returns the width of the texture in pixels. */
	virtual uint32 GetSizeX() const override
	{
		return 1;
	}

	/** Returns the height of the texture in pixels. */
	virtual uint32 GetSizeY() const override
	{
		return 1;
	}

protected:
	static int32 GetNumChannels()
	{
		return GPixelFormats[Format].NumComponents;
	}

	static int32 GetBytesPerChannel()
	{
		return GPixelFormats[Format].BlockBytes / GPixelFormats[Format].NumComponents;
	}

	template<typename T>
	static void DoWriteData(T* DataPtr)
	{
		T Values[] = { R, G, B, A };
		for (int32 i = 0; i < GetNumChannels(); ++i)
		{
			DataPtr[i] = Values[i];
		}
	}

	static void WriteData(void* DataPtr)
	{
		switch (GetBytesPerChannel())
		{
		case 1: 
			DoWriteData((uint8*)DataPtr);
			return;
		case 2:
			DoWriteData((uint16*)DataPtr);
			return;
		case 4:
			DoWriteData((uint32*)DataPtr);
			return;
		}
		// Unsupported format
		check(0);
	}
};

FTexture* GBlackUintTexture = new TGlobalResource< FUintTexture<PF_R32G32B32A32_UINT> >;

/*
	3 XYZ packed in 4 bytes. (11:11:10 for X:Y:Z)
*/

/**
*	operator FVector - unpacked to -1 to 1
*/
FPackedPosition::operator FVector3f() const
{

	return FVector3f(Vector.X/1023.f, Vector.Y/1023.f, Vector.Z/511.f);
}

/**
* operator VectorRegister
*/
VectorRegister FPackedPosition::GetVectorRegister() const
{
	FVector3f UnpackedVect = *this;

	VectorRegister VectorToUnpack = VectorLoadFloat3_W0(&UnpackedVect);

	return VectorToUnpack;
}

/**
* Pack this vector(-1 to 1 for XYZ) to 4 bytes XYZ(11:11:10)
*/
void FPackedPosition::Set( const FVector3f& InVector )
{
	check (FMath::Abs<float>(InVector.X) <= 1.f && FMath::Abs<float>(InVector.Y) <= 1.f &&  FMath::Abs<float>(InVector.Z) <= 1.f);
	
#if !WITH_EDITORONLY_DATA
	// This should not happen in Console - this should happen during Cooking in PC
	check (false);
#else
	// Too confusing to use .5f - wanted to use the last bit!
	// Change to int for easier read
	Vector.X = FMath::Clamp<int32>(FMath::TruncToInt(InVector.X * 1023.0f),-1023,1023);
	Vector.Y = FMath::Clamp<int32>(FMath::TruncToInt(InVector.Y * 1023.0f),-1023,1023);
	Vector.Z = FMath::Clamp<int32>(FMath::TruncToInt(InVector.Z * 511.0f),-511,511);
#endif
}

// LWC_TODO: Perf pessimization
void FPackedPosition::Set(const FVector3d& InVector)
{
	Set(FVector3f(InVector));
}

/**
* operator << serialize
*/
FArchive& operator<<(FArchive& Ar,FPackedPosition& N)
{
	// Save N.Packed
	return Ar << N.Packed;
}

void CalcMipMapExtent3D( uint32 TextureSizeX, uint32 TextureSizeY, uint32 TextureSizeZ, EPixelFormat Format, uint32 MipIndex, uint32& OutXExtent, uint32& OutYExtent, uint32& OutZExtent )
{
	OutXExtent = FMath::Max<uint32>(TextureSizeX >> MipIndex, GPixelFormats[Format].BlockSizeX);
	OutYExtent = FMath::Max<uint32>(TextureSizeY >> MipIndex, GPixelFormats[Format].BlockSizeY);
	OutZExtent = FMath::Max<uint32>(TextureSizeZ >> MipIndex, GPixelFormats[Format].BlockSizeZ);
}

SIZE_T CalcTextureMipMapSize3D( uint32 TextureSizeX, uint32 TextureSizeY, uint32 TextureSizeZ, EPixelFormat Format, uint32 MipIndex )
{
	uint32 XExtent;
	uint32 YExtent;
	uint32 ZExtent;
	CalcMipMapExtent3D(TextureSizeX, TextureSizeY, TextureSizeZ, Format, MipIndex, XExtent, YExtent, ZExtent);

	// Offset MipExtent to round up result
	XExtent += GPixelFormats[Format].BlockSizeX - 1;
	YExtent += GPixelFormats[Format].BlockSizeY - 1;
	ZExtent += GPixelFormats[Format].BlockSizeZ - 1;

	const uint32 XPitch = (XExtent / GPixelFormats[Format].BlockSizeX) * GPixelFormats[Format].BlockBytes;
	const uint32 NumRows = YExtent / GPixelFormats[Format].BlockSizeY;
	const uint32 NumLayers = ZExtent / GPixelFormats[Format].BlockSizeZ;

	return static_cast<SIZE_T>(NumLayers) * NumRows * XPitch;
}

SIZE_T CalcTextureSize3D( uint32 SizeX, uint32 SizeY, uint32 SizeZ, EPixelFormat Format, uint32 MipCount )
{
	SIZE_T Size = 0;
	for ( uint32 MipIndex=0; MipIndex < MipCount; ++MipIndex )
	{
		Size += CalcTextureMipMapSize3D(SizeX,SizeY,SizeZ,Format,MipIndex);
	}
	return Size;
}

FIntPoint CalcMipMapExtent( uint32 TextureSizeX, uint32 TextureSizeY, EPixelFormat Format, uint32 MipIndex )
{
	return FIntPoint(FMath::Max<uint32>(TextureSizeX >> MipIndex, GPixelFormats[Format].BlockSizeX), FMath::Max<uint32>(TextureSizeY >> MipIndex, GPixelFormats[Format].BlockSizeY));
}

SIZE_T CalcTextureMipWidthInBlocks(uint32 TextureSizeX, EPixelFormat Format, uint32 MipIndex)
{
	const uint32 BlockSizeX = GPixelFormats[Format].BlockSizeX;
	if (BlockSizeX > 0)
	{
		const uint32 WidthInTexels = FMath::Max<uint32>(TextureSizeX >> MipIndex, 1);
		const uint32 WidthInBlocks = (WidthInTexels + BlockSizeX - 1) / BlockSizeX;
		return WidthInBlocks;
	}
	else
	{
		return 0;
	}
}

SIZE_T CalcTextureMipHeightInBlocks(uint32 TextureSizeY, EPixelFormat Format, uint32 MipIndex)
{
	const uint32 BlockSizeY = GPixelFormats[Format].BlockSizeY;
	if (BlockSizeY > 0)
	{
		const uint32 HeightInTexels = FMath::Max<uint32>(TextureSizeY >> MipIndex, 1);
		const uint32 HeightInBlocks = (HeightInTexels + BlockSizeY - 1) / BlockSizeY;
		return HeightInBlocks;
	}
	else
	{
		return 0;
	}
}

SIZE_T CalcTextureMipMapSize( uint32 TextureSizeX, uint32 TextureSizeY, EPixelFormat Format, uint32 MipIndex )
{
	const SIZE_T WidthInBlocks = CalcTextureMipWidthInBlocks(TextureSizeX, Format, MipIndex);
	const SIZE_T HeightInBlocks = CalcTextureMipHeightInBlocks(TextureSizeY, Format, MipIndex);
	return WidthInBlocks * HeightInBlocks * GPixelFormats[Format].BlockBytes;
}

SIZE_T CalcTextureSize( uint32 SizeX, uint32 SizeY, EPixelFormat Format, uint32 MipCount )
{
	SIZE_T Size = 0;
	for ( uint32 MipIndex=0; MipIndex < MipCount; ++MipIndex )
	{
		Size += CalcTextureMipMapSize(SizeX,SizeY,Format,MipIndex);
	}
	return Size;
}

void CopyTextureData2D(const void* Source,void* Dest,uint32 SizeY,EPixelFormat Format,uint32 SourceStride,uint32 DestStride)
{
	const uint32 BlockSizeY = GPixelFormats[Format].BlockSizeY;
	const uint32 NumBlocksY = (SizeY + BlockSizeY - 1) / BlockSizeY;

	// a DestStride of 0 means to use the SourceStride
	if(SourceStride == DestStride || DestStride == 0)
	{
		// If the source and destination have the same stride, copy the data in one block.
		if (ensure(Source))
		{
			FMemory::ParallelMemcpy(Dest,Source,NumBlocksY * SourceStride, EMemcpyCachePolicy::StoreUncached);
		}
		else
		{
			FMemory::Memzero(Dest,NumBlocksY * SourceStride);
		}
	}
	else
	{
		// If the source and destination have different strides, copy each row of blocks separately.
		const uint32 NumBytesPerRow = FMath::Min<uint32>(SourceStride, DestStride);
		for(uint32 BlockY = 0;BlockY < NumBlocksY;++BlockY)
		{
			if (ensure(Source))
			{
				FMemory::ParallelMemcpy(
					(uint8*)Dest   + DestStride   * BlockY,
					(uint8*)Source + SourceStride * BlockY,
					NumBytesPerRow,
					EMemcpyCachePolicy::StoreUncached
					);
			}
			else
			{
				FMemory::Memzero((uint8*)Dest + DestStride * BlockY, NumBytesPerRow);
			}
		}
	}
}

/** Helper functions for text output of texture properties... */
#ifndef CASE_ENUM_TO_TEXT
#define CASE_ENUM_TO_TEXT(txt) case txt: return TEXT(#txt);
#endif

#ifndef TEXT_TO_ENUM
#define TEXT_TO_ENUM(eVal, txt)		if (FCString::Stricmp(TEXT(#eVal), txt) == 0)	return eVal;
#endif

const TCHAR* GetPixelFormatString(EPixelFormat InPixelFormat)
{
	switch (InPixelFormat)
	{
		FOREACH_ENUM_EPIXELFORMAT(CASE_ENUM_TO_TEXT)
	default:
		return TEXT("PF_Unknown");
	}	
}

EPixelFormat GetPixelFormatFromString(const TCHAR* InPixelFormatStr)
{
#define TEXT_TO_PIXELFORMAT(f) TEXT_TO_ENUM(f, InPixelFormatStr);
	FOREACH_ENUM_EPIXELFORMAT(TEXT_TO_PIXELFORMAT)
#undef TEXT_TO_PIXELFORMAT
	return PF_Unknown;
}

EPixelFormatChannelFlags GetPixelFormatValidChannels(EPixelFormat InPixelFormat)
{
	static constexpr EPixelFormatChannelFlags PixelFormatToChannelFlags[] =
	{
		EPixelFormatChannelFlags::None,		// PF_Unknown,
		EPixelFormatChannelFlags::RGBA,		// PF_A32B32G32R32F
		EPixelFormatChannelFlags::RGBA,		// PF_B8G8R8A8
		EPixelFormatChannelFlags::G,		// PF_G8
		EPixelFormatChannelFlags::G,		// PF_G16
		EPixelFormatChannelFlags::RGB,		// PF_DXT1
		EPixelFormatChannelFlags::RGBA,		// PF_DXT3
		EPixelFormatChannelFlags::RGBA,		// PF_DXT5
		EPixelFormatChannelFlags::RG,		// PF_UYVY
		EPixelFormatChannelFlags::RGB,		// PF_FloatRGB
		EPixelFormatChannelFlags::RGBA,		// PF_FloatRGBA
		EPixelFormatChannelFlags::None,		// PF_DepthStencil
		EPixelFormatChannelFlags::None,		// PF_ShadowDepth
		EPixelFormatChannelFlags::R,		// PF_R32_FLOAT
		EPixelFormatChannelFlags::RG,		// PF_G16R16
		EPixelFormatChannelFlags::RG,		// PF_G16R16F
		EPixelFormatChannelFlags::RG,		// PF_G16R16F_FILTER
		EPixelFormatChannelFlags::RG,		// PF_G32R32F
		EPixelFormatChannelFlags::RGBA,		// PF_A2B10G10R10
		EPixelFormatChannelFlags::RGBA,		// PF_A16B16G16R16
		EPixelFormatChannelFlags::None,		// PF_D24
		EPixelFormatChannelFlags::R,		// PF_R16F
		EPixelFormatChannelFlags::R,		// PF_R16F_FILTER
		EPixelFormatChannelFlags::RG,		// PF_BC5
		EPixelFormatChannelFlags::RG,		// PF_V8U8
		EPixelFormatChannelFlags::A,		// PF_A1
		EPixelFormatChannelFlags::RGB,		// PF_FloatR11G11B10
		EPixelFormatChannelFlags::A,		// PF_A8
		EPixelFormatChannelFlags::R,		// PF_R32_UINT
		EPixelFormatChannelFlags::RGBA,		// PF_R32_SINT
		EPixelFormatChannelFlags::RGBA,		// PF_PVRTC2
		EPixelFormatChannelFlags::RGBA,		// PF_PVRTC4
		EPixelFormatChannelFlags::R,		// PF_R16_UINT
		EPixelFormatChannelFlags::R,		// PF_R16_SINT
		EPixelFormatChannelFlags::RGBA,		// PF_R16G16B16A16_UINT
		EPixelFormatChannelFlags::RGBA,		// PF_R16G16B16A16_SINT
		EPixelFormatChannelFlags::RGB,		// PF_R5G6B5_UNORM
		EPixelFormatChannelFlags::RGBA,		// PF_R8G8B8A8
		EPixelFormatChannelFlags::RGBA,		// PF_A8R8G8B8
		EPixelFormatChannelFlags::R,		// PF_BC4
		EPixelFormatChannelFlags::RG,		// PF_R8G8
		EPixelFormatChannelFlags::RGB,		// PF_ATC_RGB
		EPixelFormatChannelFlags::RGBA,		// PF_ATC_RGBA_E
		EPixelFormatChannelFlags::RGBA,		// PF_ATC_RGBA_I
		EPixelFormatChannelFlags::G,		// PF_X24_G8		
		EPixelFormatChannelFlags::RGB,		// PF_ETC1
		EPixelFormatChannelFlags::RGB,		// PF_ETC2_RGB
		EPixelFormatChannelFlags::RGBA,		// PF_ETC2_RGBA
		EPixelFormatChannelFlags::RGBA,		// PF_R32G32B32A32_UINT
		EPixelFormatChannelFlags::RG,		// PF_R16G16_UINT
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_4x4
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_6x6
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_8x8
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_10x10
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_12x12
		EPixelFormatChannelFlags::RGB,		// PF_BC6H
		EPixelFormatChannelFlags::RGBA,		// PF_BC7
		EPixelFormatChannelFlags::R,		// PF_R8_UINT
		EPixelFormatChannelFlags::None,		// PF_L8
		EPixelFormatChannelFlags::RGBA,		// PF_XGXR8
		EPixelFormatChannelFlags::RGBA,		// PF_R8G8B8A8_UINT
		EPixelFormatChannelFlags::RGBA,		// PF_R8G8B8A8_SNORM
		EPixelFormatChannelFlags::RGBA,		// PF_R16G16B16A16_UNORM
		EPixelFormatChannelFlags::RGBA,		// PF_R16G16B16A16_SNORM
		EPixelFormatChannelFlags::RGBA,		// PF_PLATFORM_HDR_0
		EPixelFormatChannelFlags::RGBA,		// PF_PLATFORM_HDR_1
		EPixelFormatChannelFlags::RGBA,		// PF_PLATFORM_HDR_2
		EPixelFormatChannelFlags::None,		// PF_NV12
		EPixelFormatChannelFlags::RG,		// PF_R32G32_UINT
		EPixelFormatChannelFlags::R,		// PF_ETC2_R11_EAC
		EPixelFormatChannelFlags::RG,		// PF_ETC2_RG11_EAC
		EPixelFormatChannelFlags::R,		// PF_R8
		EPixelFormatChannelFlags::RGBA,		// PF_B5G5R5A1_UNORM
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_4x4_HDR
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_6x6_HDR
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_8x8_HDR
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_10x10_HDR
		EPixelFormatChannelFlags::RGB,		// PF_ASTC_12x12_HDR
		EPixelFormatChannelFlags::RG,		// PF_G16R16_SNORM
		EPixelFormatChannelFlags::RG,		// PF_R8G8_UINT
		EPixelFormatChannelFlags::RGB,		// PF_R32G32B32_UINT
		EPixelFormatChannelFlags::RGB,		// PF_R32G32B32_SINT
		EPixelFormatChannelFlags::RGB,		// PF_R32G32B32F
		EPixelFormatChannelFlags::R,		// PF_R8_SINT
		EPixelFormatChannelFlags::R,		// PF_R64_UINT
	};
	static_assert(UE_ARRAY_COUNT(PixelFormatToChannelFlags) == (uint8)PF_MAX, "Missing pixel format");
	return (InPixelFormat < PF_MAX) ? PixelFormatToChannelFlags[(uint8)InPixelFormat] : EPixelFormatChannelFlags::None;
}

const TCHAR* GetCubeFaceName(ECubeFace Face)
{
	switch(Face)
	{
	case CubeFace_PosX:
		return TEXT("PosX");
	case CubeFace_NegX:
		return TEXT("NegX");
	case CubeFace_PosY:
		return TEXT("PosY");
	case CubeFace_NegY:
		return TEXT("NegY");
	case CubeFace_PosZ:
		return TEXT("PosZ");
	case CubeFace_NegZ:
		return TEXT("NegZ");
	default:
		return TEXT("");
	}
}

ECubeFace GetCubeFaceFromName(const FString& Name)
{
	// not fast but doesn't have to be
	if(Name.EndsWith(TEXT("PosX")))
	{
		return CubeFace_PosX;
	}
	else if(Name.EndsWith(TEXT("NegX")))
	{
		return CubeFace_NegX;
	}
	else if(Name.EndsWith(TEXT("PosY")))
	{
		return CubeFace_PosY;
	}
	else if(Name.EndsWith(TEXT("NegY")))
	{
		return CubeFace_NegY;
	}
	else if(Name.EndsWith(TEXT("PosZ")))
	{
		return CubeFace_PosZ;
	}
	else if(Name.EndsWith(TEXT("NegZ")))
	{
		return CubeFace_NegZ;
	}

	return CubeFace_MAX;
}

class FVector4VertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;
	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0, 0, VET_Float4, 0, sizeof(FVector4f)));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}
	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVector4VertexDeclaration> FVector4VertexDeclaration;

RENDERCORE_API FVertexDeclarationRHIRef& GetVertexDeclarationFVector4()
{
	return FVector4VertexDeclaration.VertexDeclarationRHI;
}

class FVector3VertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;
	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0, 0, VET_Float3, 0, sizeof(FVector3f)));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}
	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVector3VertexDeclaration> GVector3VertexDeclaration;

RENDERCORE_API FVertexDeclarationRHIRef& GetVertexDeclarationFVector3()
{
	return GVector3VertexDeclaration.VertexDeclarationRHI;
}

class FVector2VertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;
	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0, 0, VET_Float2, 0, sizeof(FVector2f)));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}
	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

TGlobalResource<FVector2VertexDeclaration> GVector2VertexDeclaration;

RENDERCORE_API FVertexDeclarationRHIRef& GetVertexDeclarationFVector2()
{
	return GVector2VertexDeclaration.VertexDeclarationRHI;
}

RENDERCORE_API bool PlatformSupportsSimpleForwardShading(const FStaticShaderPlatform Platform)
{
	static const auto SupportSimpleForwardShadingCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SupportSimpleForwardShading"));
	// Scalability feature only needed / used on PC
	return IsPCPlatform(Platform) && SupportSimpleForwardShadingCVar->GetValueOnAnyThread() != 0;
}

RENDERCORE_API bool IsSimpleForwardShadingEnabled(const FStaticShaderPlatform Platform)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SimpleForwardShading"));
	return CVar->GetValueOnAnyThread() != 0 && PlatformSupportsSimpleForwardShading(Platform);
}

RENDERCORE_API bool MobileSupportsGPUScene()
{
	// make it shader platform setting?
	static TConsoleVariableData<int32>* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.SupportGPUScene"));
	return (CVar && CVar->GetValueOnAnyThread() != 0) ? true : false;
}

RENDERCORE_API bool IsMobileDeferredShadingEnabled(const FStaticShaderPlatform Platform)
{
	static auto* MobileShadingPathCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.ShadingPath"));
	return MobileShadingPathCvar->GetValueOnAnyThread() == 1;
}

RENDERCORE_API bool MobileRequiresSceneDepthAux(const FStaticShaderPlatform Platform)
{
	static const auto CVarMobileHDR = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR"));
	const bool bMobileHDR = (CVarMobileHDR && CVarMobileHDR->GetValueOnAnyThread() != 0);

	// SceneDepth is used on most mobile platforms when forward shading is enabled and always on IOS.
	if (IsMetalMobilePlatform(Platform))
	{
		return true;
	}
	else if (IsMobileDeferredShadingEnabled(Platform) && IsAndroidOpenGLESPlatform(Platform))
	{
		return true;
	}
	else if (!IsMobileDeferredShadingEnabled(Platform) && bMobileHDR)
	{
		// SceneDepthAux disabled when MobileHDR=false for non-IOS
		return IsAndroidOpenGLESPlatform(Platform) || IsVulkanMobilePlatform(Platform) || IsSimulatedPlatform(Platform);
	}
	return false;
}

RENDERCORE_API bool SupportsTextureCubeArray(ERHIFeatureLevel::Type FeatureLevel)
{
	return FeatureLevel >= ERHIFeatureLevel::SM5 
		// mobile deferred requries ES3.2 feature set
		|| IsMobileDeferredShadingEnabled(GMaxRHIShaderPlatform);
}

RENDERCORE_API bool MaskedInEarlyPass(const FStaticShaderPlatform Platform)
{
	static IConsoleVariable* CVarMobileEarlyZPassOnlyMaterialMasking = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Mobile.EarlyZPassOnlyMaterialMasking"));
	static IConsoleVariable* CVarEarlyZPassOnlyMaterialMasking = IConsoleManager::Get().FindConsoleVariable(TEXT("r.EarlyZPassOnlyMaterialMasking"));
	if (IsMobilePlatform(Platform))
	{
		return (CVarMobileEarlyZPassOnlyMaterialMasking && CVarMobileEarlyZPassOnlyMaterialMasking->GetInt() != 0);
	}
	else
	{
		return (CVarEarlyZPassOnlyMaterialMasking && CVarEarlyZPassOnlyMaterialMasking->GetInt() != 0);
	}
}

RENDERCORE_API bool AllowPixelDepthOffset(const FStaticShaderPlatform Platform)
{
	if (IsMobilePlatform(Platform))
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.AllowPixelDepthOffset"));
		return CVar->GetValueOnAnyThread() != 0;
	}
	return true;
}

RENDERCORE_API bool AllowPerPixelShadingModels(const FStaticShaderPlatform Platform)
{
	if (IsMobilePlatform(Platform))
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.AllowPerPixelShadingModels"));
		return CVar->GetValueOnAnyThread() != 0;
	}
	return true;
}

RENDERCORE_API uint64 GMobileAmbientOcclusionPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GMobileAmbientOcclusionPlatformMask) * 8, "GMobileAmbientOcclusionPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API bool UseMobileAmbientOcclusion(const FStaticShaderPlatform Platform)
{
	return (IsMobilePlatform(Platform) && !!(GMobileAmbientOcclusionPlatformMask & (1ull << Platform)));
}

RENDERCORE_API bool IsMobileDistanceFieldEnabled(const FStaticShaderPlatform Platform)
{
	return IsMobilePlatform(Platform) && (FDataDrivenShaderPlatformInfo::GetSupportsMobileDistanceField(Platform)/* || IsD3DPlatform(Platform)*/) && IsUsingDistanceFields(Platform);
}

RENDERCORE_API bool MobileBasePassAlwaysUsesCSM(const FStaticShaderPlatform Platform)
{
	static TConsoleVariableData<int32>* CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.Shadow.CSMShaderCullingMethod"));
	return CVar && (CVar->GetValueOnAnyThread() & 0xF) == 5 && IsMobileDistanceFieldEnabled(Platform);
}

RENDERCORE_API bool SupportsGen4TAA(const FStaticShaderPlatform Platform)
{
	if (IsMobilePlatform(Platform))
	{
		static FShaderPlatformCachedIniValue<bool> MobileSupportsGen4TAAIniValue(TEXT("r.Mobile.SupportsGen4TAA"));
		return (MobileSupportsGen4TAAIniValue.Get(Platform) != 0);
	}

	return true;
}

RENDERCORE_API bool SupportsTSR(const FStaticShaderPlatform Platform)
{
	return FDataDrivenShaderPlatformInfo::GetSupportsGen5TemporalAA(Platform);
}

RENDERCORE_API int32 GUseForwardShading = 0;
static FAutoConsoleVariableRef CVarForwardShading(
	TEXT("r.ForwardShading"),
	GUseForwardShading,
	TEXT("Whether to use forward shading on desktop platforms - requires Shader Model 5 hardware.\n")
	TEXT("Forward shading has lower constant cost, but fewer features supported. 0:off, 1:on\n")
	TEXT("This rendering path is a work in progress with many unimplemented features, notably only a single reflection capture is applied per object and no translucency dynamic shadow receiving."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	); 

static TAutoConsoleVariable<int32> CVarGBufferDiffuseSampleOcclusion(
	TEXT("r.GBufferDiffuseSampleOcclusion"), 0,
	TEXT("Whether the gbuffer contain occlusion information for individual diffuse samples."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	); 

static TAutoConsoleVariable<int32> CVarDistanceFields(
	TEXT("r.DistanceFields"),
	1,
	TEXT("Enables distance fields rendering.\n") \
	TEXT(" 0: Disabled.\n") \
	TEXT(" 1: Enabled."),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
	); 


RENDERCORE_API uint64 GForwardShadingPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GForwardShadingPlatformMask) * 8, "GForwardShadingPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GDBufferPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GDBufferPlatformMask) * 8, "GDBufferPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GBasePassVelocityPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GBasePassVelocityPlatformMask) * 8, "GBasePassVelocityPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GVelocityEncodeDepthPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GVelocityEncodeDepthPlatformMask) * 8, "GVelocityEncodeDepthPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GSelectiveBasePassOutputsPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GSelectiveBasePassOutputsPlatformMask) * 8, "GSelectiveBasePassOutputsPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GDistanceFieldsPlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GDistanceFieldsPlatformMask) * 8, "GDistanceFieldsPlatformMask must be large enough to support all shader platforms");

RENDERCORE_API uint64 GSimpleSkyDiffusePlatformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GSimpleSkyDiffusePlatformMask) * 8, "GSimpleSkyDiffusePlatformMask must be large enough to support all shader platforms");

// Specifies whether ray tracing *can* be enabled on a particular platform.
// This takes into account whether RT is globally enabled for the project and specifically enabled on a target platform.
// Safe to use to make cook-time decisions, such as whether to compile ray tracing shaders.
RENDERCORE_API uint64 GRayTracingPlaformMask = 0;
static_assert(SP_NumPlatforms <= sizeof(GRayTracingPlaformMask) * 8, "GRayTracingPlaformMask must be large enough to support all shader platforms");

// Specifies whether ray tracing *is* enabled on the current running system (in current game or editor process).
// This takes into account additional factors, such as concrete current GPU/OS/Driver capability, user-set game graphics options, etc.
// Only safe to make run-time decisions, such as whether to build acceleration structures and render ray tracing effects.
// Value may be queried using IsRayTracingEnabled().
RENDERCORE_API bool GUseRayTracing = false;

RENDERCORE_API void RenderUtilsInit()
{
	checkf(GIsRHIInitialized, TEXT("RenderUtilsInit() may only be called once RHI is initialized."));

	if (GUseForwardShading)
	{
		GForwardShadingPlatformMask = ~0ull;
	}

	static IConsoleVariable* DBufferVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DBuffer"));
	if (DBufferVar && DBufferVar->GetInt())
	{
		GDBufferPlatformMask = ~0ull;
	}

	static IConsoleVariable* VelocityPassCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VelocityOutputPass"));
	if (VelocityPassCVar && VelocityPassCVar->GetInt() == 1)
	{
		GBasePassVelocityPlatformMask = ~0ull;
	}

	static IConsoleVariable* SelectiveBasePassOutputsCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SelectiveBasePassOutputs"));
	if (SelectiveBasePassOutputsCVar && SelectiveBasePassOutputsCVar->GetInt())
	{
		GSelectiveBasePassOutputsPlatformMask = ~0ull;
	}

	static IConsoleVariable* DistanceFieldsCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.DistanceFields")); 
	if (DistanceFieldsCVar && DistanceFieldsCVar->GetInt())
	{
		GDistanceFieldsPlatformMask = ~0ull;
	}

	static IConsoleVariable* RayTracingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing"));
	if (RayTracingCVar && RayTracingCVar->GetInt())
	{
		GRayTracingPlaformMask = ~0ull;
	}

	static IConsoleVariable* MobileAmbientOcclusionCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Mobile.AmbientOcclusion"));
	if (MobileAmbientOcclusionCVar && MobileAmbientOcclusionCVar->GetInt())
	{
		GMobileAmbientOcclusionPlatformMask = ~0ull;
	}

#if WITH_EDITOR
	ITargetPlatformManagerModule* TargetPlatformManager = GetTargetPlatformManager();
	if (TargetPlatformManager)
	{
		for (ITargetPlatform* TargetPlatform : TargetPlatformManager->GetTargetPlatforms())
		{
			TArray<FName> PlaformShaderFormats;
			TargetPlatform->GetAllPossibleShaderFormats(PlaformShaderFormats);

			for (FName Format : PlaformShaderFormats)
			{
				EShaderPlatform ShaderPlatform = ShaderFormatNameToShaderPlatform(Format);
				uint32 ShaderPlatformIndex = static_cast<uint32>(ShaderPlatform);

				uint64 Mask = 1ull << ShaderPlatformIndex;

				if (TargetPlatform->UsesForwardShading())
				{
					GForwardShadingPlatformMask |= Mask;
				}
				else
				{
					GForwardShadingPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesDBuffer() && !IsMobilePlatform(ShaderPlatform))
				{
					GDBufferPlatformMask |= Mask;
				}
				else
				{
					GDBufferPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesBasePassVelocity() && !IsMobilePlatform(ShaderPlatform))
				{
					GBasePassVelocityPlatformMask |= Mask;
				}
				else
				{
					GBasePassVelocityPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesSelectiveBasePassOutputs())
				{
					GSelectiveBasePassOutputsPlatformMask |= Mask;
				}
				else
				{
					GSelectiveBasePassOutputsPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesDistanceFields())
				{
					GDistanceFieldsPlatformMask |= Mask;
				}
				else
				{
					GDistanceFieldsPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesRayTracing())
				{
					GRayTracingPlaformMask |= Mask;
				}
				else
				{
					GRayTracingPlaformMask &= ~Mask;
				}

				if (TargetPlatform->ForcesSimpleSkyDiffuse())
				{
					GSimpleSkyDiffusePlatformMask |= Mask;
				}
				else
				{
					GSimpleSkyDiffusePlatformMask &= ~Mask;
				}

				if (TargetPlatform->VelocityEncodeDepth())
				{
					GVelocityEncodeDepthPlatformMask |= Mask;
				}
				else
				{
					GVelocityEncodeDepthPlatformMask &= ~Mask;
				}

				if (TargetPlatform->UsesMobileAmbientOcclusion())
				{
					GMobileAmbientOcclusionPlatformMask |= Mask;
				}
				else
				{
					GMobileAmbientOcclusionPlatformMask &= ~Mask;
				}
			}
		}
	}
#else

	if (IsMobilePlatform(GMaxRHIShaderPlatform))
	{
		GDBufferPlatformMask = 0;
		GBasePassVelocityPlatformMask = 0;
	}

	// Load runtime values from and *.ini file used by a current platform
	// Should be code shared between cook and game, but unfortunately can't be done before we untangle non data driven platforms
	const FString PlatformName(FPlatformProperties::IniPlatformName());
	const FDataDrivenPlatformInfo& PlatformInfo = FDataDrivenPlatformInfoRegistry::GetPlatformInfo(PlatformName);

	const FString CategoryName = PlatformInfo.TargetSettingsIniSectionName;
	if (!CategoryName.IsEmpty())
	{
		FConfigFile PlatformIniFile;
		if (FConfigCacheIni::LoadLocalIniFile(PlatformIniFile, TEXT("Engine"), /*bIsBaseIniName*/ true, *PlatformName))
		{
			bool bDistanceFields = false;
			if (PlatformIniFile.GetBool(*CategoryName, TEXT("bEnableDistanceFields"), bDistanceFields) && !bDistanceFields)
			{
				GDistanceFieldsPlatformMask = 0;
			}

			bool bRayTracing = false;
			if (PlatformIniFile.GetBool(*CategoryName, TEXT("bEnableRayTracing"), bRayTracing) && !bRayTracing)
			{
				GRayTracingPlaformMask = 0;
			}
		}
	}
#endif // WITH_EDITOR

	// Run-time ray tracing support depends on the following factors:
	// - Ray tracing must be enabled for the project
	// - Skin cache must be enabled for the project
	// - Current GPU, OS and driver must support ray tracing
	// - User is running the Editor *OR* running the game with ray tracing enabled in graphics options

	// When ray tracing is enabled, we must load additional shaders and build acceleration structures for meshes.
	// For this reason it is only possible to enable RT at startup and changing the state requires restart.
	// This is also the reason why IsRayTracingEnabled() lives in RenderCore module, as it controls creation of 
	// RT pipelines in ShaderPipelineCache.cpp.

	if (RayTracingCVar && RayTracingCVar->GetBool())
	{
		const bool bRayTracingAllowedOnCurrentPlatform = !!(GRayTracingPlaformMask & (1ull << GMaxRHIShaderPlatform));
		if (GRHISupportsRayTracing && bRayTracingAllowedOnCurrentPlatform)
		{
			if (GIsEditor)
			{
				// Ray tracing is enabled for the project and we are running on RT-capable machine,
				// therefore the core ray tracing features are also enabled, so that required shaders
				// are loaded, acceleration structures are built, etc.
				GUseRayTracing = true;

				UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is enabled for the editor. Reason: r.RayTracing=1."));
			}
			else
			{
				// If user preference exists in game settings file, the bRayTracingEnabled will be set based on its value.
				// Otherwise the current value is preserved.
				if (GConfig->GetBool(TEXT("RayTracing"), TEXT("r.RayTracing.EnableInGame"), GUseRayTracing, GGameUserSettingsIni))
				{
					UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is %s for the game. Reason: user setting r.RayTracing.EnableInGame=%d."),
						GUseRayTracing ? TEXT("enabled") : TEXT("disabled"),
						(int)GUseRayTracing);
				}
				else
				{
					GUseRayTracing = true;

					UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is enabled for the game. Reason: r.RayTracing=1, and r.RayTracing.EnableInGame is not present (default true)."));

					//GUseRayTracing = false;

					//UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is disabled for the game. Reason: r.RayTracing=1, and r.RayTracing.EnableInGame is not present (default false)."));
				}
			}

			// Sanity check: skin cache is *required* for ray tracing.
			// It can be dynamically enabled only when its shaders have been compiled.
			IConsoleVariable* SkinCacheCompileShadersCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SkinCache.CompileShaders"));
			if (GUseRayTracing && SkinCacheCompileShadersCVar->GetInt() <= 0)
			{
				GUseRayTracing = false;

				UE_LOG(LogRendererCore, Fatal, TEXT("Ray tracing requires skin cache to be enabled. Set r.SkinCache.CompileShaders=1."));
			}

		}
		else
		{
			if (!GRHISupportsRayTracing)
			{
				UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is disabled. Reason: not supported by current RHI."));
			}
			else
			{
				UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is disabled. Reason: disabled on current platform."));
			}
		}
	}
	else
	{
		UE_LOG(LogRendererCore, Log, TEXT("Ray tracing is disabled. Reason: r.RayTracing=0."));
	}
}

class FUnitCubeVertexBuffer : public FVertexBuffer
{
public:
	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{
		const int32 NumVerts = 8;
		TResourceArray<FVector4f, VERTEXBUFFER_ALIGNMENT> Verts;
		Verts.SetNumUninitialized(NumVerts);

		for (uint32 Z = 0; Z < 2; Z++)
		{
			for (uint32 Y = 0; Y < 2; Y++)
			{
				for (uint32 X = 0; X < 2; X++)
				{
					const FVector4f Vertex = FVector4f(
					  (X ? -1.f : 1.f),
					  (Y ? -1.f : 1.f),
					  (Z ? -1.f : 1.f),
					  1.f
					);

					Verts[GetCubeVertexIndex(X, Y, Z)] = Vertex;
				}
			}
		}

		uint32 Size = Verts.GetResourceDataSize();

		// Create vertex buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(TEXT("FUnitCubeVertexBuffer"), &Verts);
		VertexBufferRHI = RHICreateVertexBuffer(Size, BUF_Static, CreateInfo);
	}
};

class FUnitCubeIndexBuffer : public FIndexBuffer
{
public:
	/**
	* Initialize the RHI for this rendering resource
	*/
	void InitRHI() override
	{
		TResourceArray<uint16, INDEXBUFFER_ALIGNMENT> Indices;
		
		int32 NumIndices = UE_ARRAY_COUNT(GCubeIndices);
		Indices.AddUninitialized(NumIndices);
		FMemory::Memcpy(Indices.GetData(), GCubeIndices, NumIndices * sizeof(uint16));

		const uint32 Size = Indices.GetResourceDataSize();
		const uint32 Stride = sizeof(uint16);

		// Create index buffer. Fill buffer with initial data upon creation
		FRHIResourceCreateInfo CreateInfo(TEXT("FUnitCubeIndexBuffer"), &Indices);
		IndexBufferRHI = RHICreateIndexBuffer(Stride, Size, BUF_Static, CreateInfo);
	}
};

static TGlobalResource<FUnitCubeVertexBuffer> GUnitCubeVertexBuffer;
static TGlobalResource<FUnitCubeIndexBuffer> GUnitCubeIndexBuffer;

RENDERCORE_API FBufferRHIRef& GetUnitCubeVertexBuffer()
{
	return GUnitCubeVertexBuffer.VertexBufferRHI;
}

RENDERCORE_API FBufferRHIRef& GetUnitCubeIndexBuffer()
{
	return GUnitCubeIndexBuffer.IndexBufferRHI;
}

RENDERCORE_API void QuantizeSceneBufferSize(const FIntPoint& InBufferSize, FIntPoint& OutBufferSize)
{
	// Ensure sizes are dividable by the ideal group size for 2d tiles to make it more convenient.
	const uint32 DividableBy = 4;

	static_assert(DividableBy % 4 == 0, "A lot of graphic algorithms where previously assuming DividableBy == 4");

	const uint32 Mask = ~(DividableBy - 1);
	OutBufferSize.X = (InBufferSize.X + DividableBy - 1) & Mask;
	OutBufferSize.Y = (InBufferSize.Y + DividableBy - 1) & Mask;
}

RENDERCORE_API bool UseVirtualTexturing(const FStaticFeatureLevel InFeatureLevel, const ITargetPlatform* TargetPlatform)
{
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURE_STREAMING
	if (!FPlatformProperties::SupportsVirtualTextureStreaming())
	{
		return false;
	}

	// does the project has it enabled ?
	static const auto CVarVirtualTexture = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTextures"));
	check(CVarVirtualTexture);
	if (CVarVirtualTexture->GetValueOnAnyThread() == 0)
	{
		return false;
	}		

	// mobile needs an additional switch to enable VT		
	static const auto CVarMobileVirtualTexture = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.VirtualTextures"));
	if (InFeatureLevel == ERHIFeatureLevel::ES3_1 && CVarMobileVirtualTexture->GetValueOnAnyThread() == 0)
	{
		return false;
	}

	return true;
#else
	return false;
#endif
}

RENDERCORE_API bool UseVirtualTextureLightmap(const FStaticFeatureLevel InFeatureLevel, const ITargetPlatform* TargetPlatform)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTexturedLightmaps"));
	const bool bUseVirtualTextureLightmap = (CVar->GetValueOnAnyThread() != 0) && UseVirtualTexturing(InFeatureLevel, TargetPlatform);
	return bUseVirtualTextureLightmap;
}

RENDERCORE_API bool ExcludeNonPipelinedShaderTypes(EShaderPlatform ShaderPlatform)
{
	if (RHISupportsShaderPipelines(ShaderPlatform))
	{
		static const TConsoleVariableData<int32>* CVarShaderPipelines = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShaderPipelines"));
		bool bShaderPipelinesAreEnabled = CVarShaderPipelines && CVarShaderPipelines->GetValueOnAnyThread(IsInGameThread()) != 0;
		if (bShaderPipelinesAreEnabled)
		{
			static const IConsoleVariable* CVarExcludeNonPipelinedShaders = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Material.ExcludeNonPipelinedShaders"));
			bool bExcludeNonPipelinedShaders = CVarExcludeNonPipelinedShaders && CVarExcludeNonPipelinedShaders->GetInt() != 0;

			return bExcludeNonPipelinedShaders;
		}
	}

	return false;
}

RENDERCORE_API bool PlatformSupportsVelocityRendering(const FStaticShaderPlatform Platform)
{
	if (IsMobilePlatform(Platform))
	{
		// Enable velocity rendering if desktop Gen4 TAA is supported on mobile.
		return SupportsGen4TAA(Platform);
	}

	return true;
}

RENDERCORE_API bool DoesPlatformSupportNanite(EShaderPlatform Platform, bool bCheckForProjectSetting)
{
	// Nanite allowed for this project
	if (bCheckForProjectSetting)
	{
		const bool bNaniteSupported = GNaniteProjectEnabled != 0;
		if (UNLIKELY(!bNaniteSupported))
		{
			return false;
		}
	}

	// Make sure the current platform has DDPI definitions.
	const bool bValidPlatform = FDataDrivenShaderPlatformInfo::IsValid(Platform);

	// GPUScene is required for Nanite
	const bool bSupportGPUScene = FDataDrivenShaderPlatformInfo::GetSupportsGPUScene(Platform);

	// Nanite specific check
	const bool bSupportNanite = FDataDrivenShaderPlatformInfo::GetSupportsNanite(Platform);

	const bool bFullCheck = bValidPlatform && bSupportGPUScene && bSupportNanite;
	return bFullCheck;
}

/** Returns whether DBuffer decals are enabled for a given shader platform */
RENDERCORE_API bool IsUsingDBuffers(const FStaticShaderPlatform Platform)
{
	extern RENDERCORE_API uint64 GDBufferPlatformMask;
	return !!(GDBufferPlatformMask & (1ull << Platform));
}

RENDERCORE_API bool AreSkinCacheShadersEnabled(EShaderPlatform Platform)
{
	static FShaderPlatformCachedIniValue<bool> PerPlatformCVar(TEXT("r.SkinCache.CompileShaders"));
	return (PerPlatformCVar.Get(Platform) != 0);
}

RENDERCORE_API bool DoesRuntimeSupportOnePassPointLightShadows(EShaderPlatform Platform)
{
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Shadow.DetectVertexShaderLayerAtRuntime"));

	return RHISupportsVertexShaderLayer(Platform)
		|| (CVar->GetValueOnAnyThread() != 0 && GRHISupportsArrayIndexFromAnyShader != 0);
}
