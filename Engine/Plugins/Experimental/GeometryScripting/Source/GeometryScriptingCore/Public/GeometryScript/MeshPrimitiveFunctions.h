// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "GeometryScript/GeometryScriptTypes.h"
#include "MeshPrimitiveFunctions.generated.h"


UENUM(BlueprintType)
enum class EGeometryScriptPrimitivePolygroupMode : uint8
{
	SingleGroup = 0,
	PerFace = 1,
	PerQuad = 2
};

UENUM(BlueprintType)
enum class EGeometryScriptPrimitiveOriginMode : uint8
{
	Center = 0,
	Base = 1
};

UENUM(BlueprintType)
enum class EGeometryScriptPrimitiveUVMode : uint8
{
	Uniform = 0,
	ScaleToFill = 1
};


USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTINGCORE_API FGeometryScriptPrimitiveOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = Options)
	EGeometryScriptPrimitivePolygroupMode PolygroupMode = EGeometryScriptPrimitivePolygroupMode::PerFace;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bFlipOrientation = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	EGeometryScriptPrimitiveUVMode UVMode = EGeometryScriptPrimitiveUVMode::Uniform;
};


USTRUCT(BlueprintType)
struct GEOMETRYSCRIPTINGCORE_API FGeometryScriptRevolveOptions
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintReadWrite, Category = Options)
	float RevolveDegrees = 360.0f;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	float DegreeOffset = 0.0f;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bReverseDirection = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bHardNormals = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	float HardNormalAngle = 30.0f;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bProfileAtMidpoint = false;

	UPROPERTY(BlueprintReadWrite, Category = Options)
	bool bFillPartialRevolveEndcaps = true;
};


UCLASS(meta = (ScriptName = "GeometryScript_Primitives"))
class GEOMETRYSCRIPTINGCORE_API UGeometryScriptLibrary_MeshPrimitiveFunctions : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendBox( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float DimensionX = 100,
		float DimensionY = 100,
		float DimensionZ = 100,
		int32 StepsX = 0,
		int32 StepsY = 0,
		int32 StepsZ = 0,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendSphereLatLong( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float Radius = 50,
		int32 StepsPhi = 10,
		int32 StepsTheta = 16,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Center,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendSphereBox( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float Radius = 50,
		int32 StepsX = 6,
		int32 StepsY = 6,
		int32 StepsZ = 6,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Center,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendCapsule( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float Radius = 30,
		float LineLength = 75,
		int32 HemisphereSteps = 5,
		int32 CircleSteps = 8,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendCylinder( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float Radius = 50,
		float Height = 100,
		int32 RadialSteps = 12,
		int32 HeightSteps = 0,
		bool bCapped = true,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendCone( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float BaseRadius = 50,
		float TopRadius = 5,
		float Height = 100,
		int32 RadialSteps = 12,
		int32 HeightSteps = 4,
		bool bCapped = true,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendTorus( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		FGeometryScriptRevolveOptions RevolveOptions,
		float MajorRadius = 50,
		float MinorRadius = 25,
		int32 MajorSteps = 16,
		int32 MinorSteps = 8,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);

	/**
	 * In the coordinate system of the revolve polygon, +X is towards the "outside" of the revolve donut, and +Y is "up" (ie +Z in local space)
	 * Polygon should be oriented counter-clockwise to produce a correctly-oriented shape, otherwise it will be inside-out
	 * Polygon endpoint is not repeated.
	 */
	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRevolvePolygon( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PolygonVertices,
		FGeometryScriptRevolveOptions RevolveOptions,
		float Radius = 100,
		int32 Steps = 8,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta = (ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh*
	AppendSpiralRevolvePolygon(
		UDynamicMesh* TargetMesh,
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PolygonVertices,
		FGeometryScriptRevolveOptions RevolveOptions,
		float Radius = 100,
		int Steps = 18,
		float RisePerRevolution = 50,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRevolvePath( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PathVertices,
		FGeometryScriptRevolveOptions RevolveOptions,
		int32 Steps = 8,
		bool bCapped = true,
		UGeometryScriptDebug* Debug = nullptr);

	/**
	 * Polygon should be oriented counter-clockwise to produce a correctly-oriented shape, otherwise it will be inside-out
	 * Polygon endpoint is not repeated.
	 */
	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendSimpleExtrudePolygon( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PolygonVertices,
		float Height = 100,
		int32 HeightSteps = 0,
		bool bCapped = true,
		EGeometryScriptPrimitiveOriginMode Origin = EGeometryScriptPrimitiveOriginMode::Base,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendSimpleSweptPolygon( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PolygonVertices,
		const TArray<FVector>& SweepPath,
		bool bLoop = false,
		bool bCapped = true,
		float StartScale = 1.0f,
		float EndScale = 1.0f,
		UGeometryScriptDebug* Debug = nullptr);



	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRectangleXY( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float DimensionX = 100,
		float DimensionY = 100,
		int32 StepsWidth = 0,
		int32 StepsHeight = 0,
		UGeometryScriptDebug* Debug = nullptr);

	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRoundRectangleXY( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float DimensionX = 100,
		float DimensionY = 100,
		float CornerRadius = 5,
		int32 StepsWidth = 0,
		int32 StepsHeight = 0,
		int32 StepsRound = 4,
		UGeometryScriptDebug* Debug = nullptr);

	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendDisc( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float Radius = 50,
		int32 AngleSteps = 16,
		int32 SpokeSteps = 0,
		float StartAngle = 0,
		float EndAngle = 360,
		float HoleRadius = 0,
		UGeometryScriptDebug* Debug = nullptr);

	/**
	* Polygon should be oriented counter-clockwise to produce a correctly-oriented shape, otherwise it will be inside-out
	* Polygon endpoint is not repeated.
	*/
	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendTriangulatedPolygon( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		const TArray<FVector2D>& PolygonVertices,
		bool bAllowSelfIntersections = true,
		UGeometryScriptDebug* Debug = nullptr);



	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendLinearStairs( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float StepWidth = 100.0f,
		float StepHeight = 20.0f,
		float StepDepth = 30.0f,
		int NumSteps = 8,
		bool bFloating = false,
		UGeometryScriptDebug* Debug = nullptr);


	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Primitives", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendCurvedStairs( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float StepWidth = 100.0f,
		float StepHeight = 20.0f,
		float InnerRadius = 150.0f,
		float CurveAngle = 90.0f,
		int NumSteps = 8,
		bool bFloating = false,
		UGeometryScriptDebug* Debug = nullptr);




	//---------------------------------------------
	// Backwards-Compatibility implementations
	//---------------------------------------------
	// These are versions/variants of the above functions that were released
	// in previous UE 5.x versions, that have since been updated. 
	// To avoid breaking user scripts, these previous versions are currently kept and 
	// called via redirectors registered in GeometryScriptingCoreModule.cpp.
	// 
	// These functions may be deprecated in future UE releases.
	//


	/**
	 * 5.0 Preview 1 Compatibility version of AppendRectangleXY. Incorrectly interprets the input dimensions.
	 * Incorrectly divides the input DimensionX and DimensionY by 2.
	 * @warning It is strongly recommended that callers of this function update to the current AppendRectangleXY function!
	 */
	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Compatibility", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRectangle_Compatibility_5_0( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float DimensionX = 100,
		float DimensionY = 100,
		int32 StepsWidth = 0,
		int32 StepsHeight = 0,
		UGeometryScriptDebug* Debug = nullptr);


	/**
	 * 5.0 Preview 1 Compatibility version of AppendRoundRectangleXY. 
	 * Incorrectly divides the input DimensionX and DimensionY by 2.
	 * @warning It is strongly recommended that callers of this function update to the current AppendRoundRectangleXY function!
	 */
	UFUNCTION(BlueprintCallable, Category = "GeometryScript|Compatibility", meta=(ScriptMethod))
	static UPARAM(DisplayName = "Target Mesh") UDynamicMesh* 
	AppendRoundRectangle_Compatibility_5_0( 
		UDynamicMesh* TargetMesh, 
		FGeometryScriptPrimitiveOptions PrimitiveOptions,
		FTransform Transform,
		float DimensionX = 100,
		float DimensionY = 100,
		float CornerRadius = 5,
		int32 StepsWidth = 0,
		int32 StepsHeight = 0,
		int32 StepsRound = 4,
		UGeometryScriptDebug* Debug = nullptr);

};