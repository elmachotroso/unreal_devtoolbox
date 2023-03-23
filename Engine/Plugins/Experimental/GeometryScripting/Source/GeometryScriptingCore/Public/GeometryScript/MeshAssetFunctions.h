// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "MeshAssetFunctions.generated.h"

class UStaticMesh;
class UDynamicMesh;
class UMaterialInterface;



USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTINGCORE_API FGeometryScriptCopyMeshFromAssetOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bApplyBuildSettings = true;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bRequestTangents = true;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bIgnoreRemoveDegenerates = true;
};

/**
 * Configuration settings for Nanite Rendering on StaticMesh Assets
 */
USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTINGCORE_API FGeometryScriptNaniteOptions
{
	GENERATED_BODY()

	/** Set Nanite to Enabled/Disabled */
	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bEnabled = true;

	/** Percentage of triangles to maintain in Fallback Mesh used when Nanite is unavailable */
	UPROPERTY(BlueprintReadWrite, Category = Options)
	float FallbackPercentTriangles = 100.0f;

	/** Relative Error to maintain in Fallback Mesh used when Nanite is unavailable. Overrides FallbackPercentTriangles. Set to 0 to only use FallbackPercentTriangles (default). */
	UPROPERTY(BlueprintReadWrite, Category = Options)
	float FallbackRelativeError = 0.0f;
};


USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTINGCORE_API FGeometryScriptCopyMeshToAssetOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bEnableRecomputeNormals = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bEnableRecomputeTangents = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bEnableRemoveDegenerates = false;

	
	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bReplaceMaterials = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	TArray<UMaterialInterface*> NewMaterials;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	TArray<FName> NewMaterialSlotNames;

	/** If enabled, NaniteSettings will be applied to the target Asset if possible */
	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bApplyNaniteSettings = false;

	/** Nanite Settings applied to the target Asset, if bApplyNaniteSettings = true */
	UPROPERTY(BlueprintReadWrite, Category = Options)
	FGeometryScriptNaniteOptions NaniteSettings = FGeometryScriptNaniteOptions();

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bEmitTransaction = true;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bDeferMeshPostEditChange = false;
};




UCLASS(meta = (ScriptName = "GeometryScript_AssetUtils"))
class GEOMETRYSCRIPTINGCORE_API UGeometryScriptLibrary_StaticMeshFunctions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:

	UFUNCTION(BlueprintCallable, Category = "GeometryScript|StaticMesh", meta = (ExpandEnumAsExecs = "Outcome"))
	static UPARAM(DisplayName = "Dynamic Mesh") UDynamicMesh* 
	CopyMeshFromStaticMesh(
		UStaticMesh* FromStaticMeshAsset, 
		UDynamicMesh* ToDynamicMesh, 
		FGeometryScriptCopyMeshFromAssetOptions AssetOptions,
		FGeometryScriptMeshReadLOD RequestedLOD,
		TEnumAsByte<EGeometryScriptOutcomePins>& Outcome,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|StaticMesh", meta = (ExpandEnumAsExecs = "Outcome"))
	static UPARAM(DisplayName = "Dynamic Mesh") UDynamicMesh* 
	CopyMeshToStaticMesh(
		UDynamicMesh* FromDynamicMesh, 
		UStaticMesh* ToStaticMeshAsset, 
		FGeometryScriptCopyMeshToAssetOptions Options,
		FGeometryScriptMeshWriteLOD TargetLOD,
		TEnumAsByte<EGeometryScriptOutcomePins>& Outcome,
		UGeometryScriptDebug* Debug = nullptr);



	UFUNCTION(BlueprintCallable, Category = "GeometryScript|StaticMesh", meta = (ExpandEnumAsExecs = "Outcome"))
	static void
	GetSectionMaterialListFromStaticMesh(
		UStaticMesh* FromStaticMeshAsset, 
		FGeometryScriptMeshReadLOD RequestedLOD,
		TArray<UMaterialInterface*>& MaterialList,
		TArray<int32>& MaterialIndex,
		TEnumAsByte<EGeometryScriptOutcomePins>& Outcome,
		UGeometryScriptDebug* Debug = nullptr);
};


