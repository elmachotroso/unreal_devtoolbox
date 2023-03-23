// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatModule.h"
#include "Interfaces/ITextureFormatManagerModule.h"
#include "Serialization/CompactBinary.h"
#include "Serialization/CompactBinaryWriter.h"
#include "TextureCompressorModule.h"

/**
 * Version of ITextureFormat that handles a child texture format that is used as a "post-process" after compressing textures, useful for
 * several platforms that need to modify already compressed texture data for optimal data
 */
class FChildTextureFormat : public ITextureFormat
{
public:

	FChildTextureFormat(const TCHAR* PlatformFormatPrefix)
		: FormatPrefix(PlatformFormatPrefix)
	{

	}

protected:
	void AddBaseTextureFormatModules(const TCHAR* ModuleNameWildcard)
	{
		TArray<FName> Modules;
		FModuleManager::Get().FindModules(ModuleNameWildcard, Modules);

		for (FName ModuleName : Modules)
		{
			ITextureFormatModule * TFModule = FModuleManager::LoadModulePtr<ITextureFormatModule>(ModuleName);
			if ( TFModule != nullptr )
			{
				ITextureFormat* BaseFormat = TFModule->GetTextureFormat();
				BaseFormat->GetSupportedFormats(BaseFormats);
			}
		}

		SupportedFormatsCached.Reset();
		for (FName BaseFormat : BaseFormats)
		{
			FName ChildFormat(*(FormatPrefix + BaseFormat.ToString()));
			SupportedFormatsCached.Add(ChildFormat);
		}
	}

	FName GetBaseFormatName(FName PlatformName) const
	{
		return FName(*(PlatformName.ToString().Replace(*FormatPrefix, TEXT(""))));
	}

	FCbObjectView GetBaseFormatConfigOverride(const FCbObjectView& ObjView) const
	{
		return ObjView.FindView("BaseTextureFormatConfig").AsObjectView();
	}

	FTextureBuildSettings GetBaseTextureBuildSettings(const FTextureBuildSettings& BuildSettings) const
	{
		FTextureBuildSettings BaseSettings = BuildSettings;
		BaseSettings.TextureFormatName = GetBaseFormatName(BuildSettings.TextureFormatName);
		BaseSettings.FormatConfigOverride = GetBaseFormatConfigOverride(BuildSettings.FormatConfigOverride);
		return BaseSettings;
	}

	/**
	 * Given a platform specific format name, get the parent texture format object
	 */
	const ITextureFormat* GetBaseFormatObject(FName FormatName) const
	{
		FName BaseFormatName = GetBaseFormatName(FormatName);

		ITextureFormatManagerModule& TFM = FModuleManager::LoadModuleChecked<ITextureFormatManagerModule>("TextureFormat");
		const ITextureFormat* FormatObject = TFM.FindTextureFormat(BaseFormatName);

		checkf(FormatObject != nullptr, TEXT("Bad FormatName %s passed to FChildTextureFormat::GetBaseFormatObject()"));

		return FormatObject;
	}

	/**
	 * The final version is a combination of parent and child formats, 8 bits for each
	 */
	virtual uint8 GetChildFormatVersion(FName Format, const FTextureBuildSettings* BuildSettings) const = 0;

	/**
	 * Make the child type think about if they need a key string or not, by making it pure virtual
	 */
	virtual FString GetChildDerivedDataKeyString(const FTextureBuildSettings& BuildSettings) const = 0;

	/**
	 * Obtains the global format config object for this texture format.
	 * 
	 * @param BuildSettings Build settings.
	 * @returns The global format config object or an empty object if no format settings are defined for this texture format.
	 */
	virtual FCbObject ExportGlobalChildFormatConfig(const FTextureBuildSettings& BuildSettings) const
	{
		return FCbObject();
	}

	/**
	 * Obtains the format config appropriate for the build .
	 * 
	 * @param ObjView A view of the entire format config container or null if none exists.
	 * @returns The format settings object view or a null view if the active global format config should be used.
	 */
	virtual FCbObjectView GetChildFormatConfigOverride(const FCbObjectView& ObjView) const
	{
		return ObjView.FindView("ChildTextureFormatConfig").AsObjectView();
	}

public:

	//// ITextureFormat interface ////

	virtual void GetSupportedFormats(TArray<FName>& OutFormats) const override
	{
		OutFormats.Append(SupportedFormatsCached);
	}

	virtual bool SupportsEncodeSpeed(FName Format) const override
	{
		return GetBaseFormatObject(Format)->SupportsEncodeSpeed(Format);
	}

	virtual FName GetEncoderName(FName Format) const override
	{
		return GetBaseFormatObject(Format)->GetEncoderName(Format);
	}

	virtual uint16 GetVersion(FName Format, const FTextureBuildSettings* BuildSettings) const final
	{
		uint16 BaseVersion = GetBaseFormatObject(Format)->GetVersion(Format, BuildSettings);
		checkf(BaseVersion < 256, TEXT("BaseFormat for %s had too large a version (%d), must fit in 8bits"), *Format.ToString(), BaseVersion);

		uint8 ChildVersion = GetChildFormatVersion(Format, BuildSettings);

		// 8 bits for each version
		return (BaseVersion << 8) | ChildVersion;
	}

	virtual FString GetDerivedDataKeyString(const FTextureBuildSettings& BuildSettings) const final
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		FString BaseString = GetBaseFormatObject(BuildSettings.TextureFormatName)->GetDerivedDataKeyString(BaseSettings);
		FString ChildString = GetChildDerivedDataKeyString(BuildSettings);

		return BaseString + ChildString;
	}

	virtual EPixelFormat GetPixelFormatForImage(const FTextureBuildSettings& BuildSettings, const struct FImage& ExampleImage, bool bImageHasAlphaChannel) const override 
	{
		FTextureBuildSettings Settings = GetBaseTextureBuildSettings(BuildSettings);
		return GetBaseFormatObject(BuildSettings.TextureFormatName)->GetPixelFormatForImage(Settings, ExampleImage, bImageHasAlphaChannel);
	}

	bool CompressBaseImage(
		const FImage& InImage,
		const FTextureBuildSettings& BuildSettings,
		FStringView DebugTexturePathName,
		bool bImageHasAlphaChannel,
		FCompressedImage2D& OutCompressedImage
	) const
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		// pass along the compression to the base format
		if (GetBaseFormatObject(BuildSettings.TextureFormatName)->CompressImage(InImage, BaseSettings, DebugTexturePathName, bImageHasAlphaChannel, OutCompressedImage) == false)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to compress with base compressor [format %s]"), *BaseSettings.TextureFormatName.ToString());
			return false;
		}
		return true;
	}

	bool CompressBaseImageTiled(
		const FImage* Images,
		uint32 NumImages,
		const FTextureBuildSettings& BuildSettings,
		FStringView DebugTexturePathName,
		bool bImageHasAlphaChannel,
		TSharedPtr<FTilerSettings>& TilerSettings,
		FCompressedImage2D& OutCompressedImage
	) const
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		// pass along the compression to the base format
		if (GetBaseFormatObject(BuildSettings.TextureFormatName)->CompressImageTiled(Images, NumImages, BaseSettings, DebugTexturePathName, bImageHasAlphaChannel, TilerSettings, OutCompressedImage) == false)
		{
			UE_LOG(LogTemp, Error, TEXT("Failed to compress with base tiled compressor [format %s]"), *BaseSettings.TextureFormatName.ToString());
			return false;
		}
		return true;
	}

	bool PrepareTiling(
		const FImage* Images,
		const uint32 NumImages,
		const FTextureBuildSettings& BuildSettings,
		bool bImageHasAlphaChannel,
		TSharedPtr<FTilerSettings>& OutTilerSettings,
		TArray<FCompressedImage2D>& OutCompressedImages
	) const override
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		return GetBaseFormatObject(BuildSettings.TextureFormatName)->PrepareTiling(Images, NumImages, BaseSettings, bImageHasAlphaChannel, OutTilerSettings, OutCompressedImages);
	}

	bool SetTiling(
		const FTextureBuildSettings& BuildSettings,
		TSharedPtr<FTilerSettings>& TilerSettings,
		const TArray64<uint8>& ReorderedBlocks,
		uint32 NumBlocks
	) const override
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		return GetBaseFormatObject(BuildSettings.TextureFormatName)->SetTiling(BaseSettings, TilerSettings, ReorderedBlocks, NumBlocks);
	}

	void ReleaseTiling(const FTextureBuildSettings& BuildSettings, TSharedPtr<FTilerSettings>& TilerSettings) const override
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		return GetBaseFormatObject(BuildSettings.TextureFormatName)->ReleaseTiling(BuildSettings, TilerSettings);
	}


	virtual FCbObject ExportGlobalFormatConfig(const FTextureBuildSettings& BuildSettings) const override
	{
		FTextureBuildSettings BaseSettings = GetBaseTextureBuildSettings(BuildSettings);

		FCbObject BaseObj = GetBaseFormatObject(BuildSettings.TextureFormatName)->ExportGlobalFormatConfig(BaseSettings);
		FCbObject ChildObj = ExportGlobalChildFormatConfig(BuildSettings);

		if (!BaseObj && !ChildObj)
		{
			return FCbObject();
		}

		FCbWriter Writer;
		Writer.BeginObject("TextureFormatConfig");

		if (BaseObj)
		{
			Writer.AddObject("BaseTextureFormatConfig", BaseObj);
		}

		if (ChildObj)
		{
			Writer.AddObject("ChildTextureFormatConfig", ChildObj);
		}

		Writer.EndObject();

		return Writer.Save().AsObject();
	}

protected:

	// Prefix put before all formats from parent formats
	const FString FormatPrefix;

	// List of base formats that. Combined with FormatPrefix, this contains all formats this can handle
	TArray<FName> BaseFormats;

	// List of combined BaseFormats with FormatPrefix.
	TArray<FName> SupportedFormatsCached;
};
