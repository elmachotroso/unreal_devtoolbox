// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NiagaraSystem.h"
#include "NiagaraEmitter.h"
#include "NiagaraScript.h"
#include "Engine/DeveloperSettings.h"
#include "InputCoreTypes.h"
#include "NiagaraSettings.generated.h"

// This enum must match the order in NiagaraDataInterfaceSkeletalMesh.ush
UENUM()
namespace ENDISkelMesh_GpuMaxInfluences
{
	enum Type
	{
		/** Allow up to 4 bones to be sampled. */
		AllowMax4 = 0,
		/** Allow up to 8 bones to be sampled. */
		AllowMax8 = 1,
		/** Allow an unlimited amount of bones to be sampled. */
		Unlimited = 2,
	};
}

// This enum must match the order in NiagaraDataInterfaceSkeletalMesh.ush
UENUM()
namespace ENDISkelMesh_GpuUniformSamplingFormat
{
	enum Type
	{
		/** 64 bits per entry. Allow for the full int32 range of triangles (2 billion). */
		Full = 0,
		/** 32 bits per entry. Allow for ~16.7 million triangles and 8 bits of probability precision. */
		Limited_24_8 = 1,
		/** 32 bits per entry. Allow for ~8.4 millions triangles and 9 bits of probability precision. */
		Limited_23_9 = 2,
	};
}

// This enum must match the order in NiagaraDataInterfaceSkeletalMesh.ush
UENUM()
namespace ENDISkelMesh_AdjacencyTriangleIndexFormat
{
	enum Type
	{
		/** 32 bits per entry. Allow for the full int32 range of triangles (2 billion). */
		Full = 0,
		/** 16 bits per entry. Allow for half (int16) range of triangles (64k). */
		Half = 1,
	};
}

UENUM()
enum class ENiagaraDefaultRendererPixelCoverageMode : uint8
{
	/** When renderer is set to automatic mode pixel coverage is enabled. */
	Enabled,
	/** When renderer is set to automatic mode pixel coverage is disabled. */
	Disabled,
};

UENUM()
namespace ENDICollisionQuery_AsyncGpuTraceProvider
{
	enum Type
	{
		Default = 0 UMETA(DisplayName = "Project Default"),
		HWRT = 1 UMETA(DisplayName = "HW Ray Tracing"),
		GSDF = 2 UMETA(DisplayName = "Global Signed Distance Fields"),
		None = 3 UMETA(DisplayName = "Disabled"),
	};
}

UCLASS(config = Niagara, defaultconfig, meta=(DisplayName="Niagara"))
class NIAGARA_API UNiagaraSettings : public UDeveloperSettings
{
	GENERATED_UCLASS_BODY()

#if WITH_EDITORONLY_DATA
	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = (AllowedClasses = "ScriptStruct"))
	TArray<FSoftObjectPath> AdditionalParameterTypes;

	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = (AllowedClasses = "ScriptStruct"))
	TArray<FSoftObjectPath> AdditionalPayloadTypes;

	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = (AllowedClasses = "Enum"))
	TArray<FSoftObjectPath> AdditionalParameterEnums;

	/** Sets the default navigation behavior for the system preview viewport. */
	UPROPERTY(config, EditAnywhere, Category = Viewport)
	bool bSystemViewportInOrbitMode = true;

	/** If true then the "link input" menu will also show variables of different types, as long as there is a conversion script for them. */
	UPROPERTY(config, EditAnywhere, Category = Niagara)
	bool bShowConvertibleInputsInStack = false;
#endif // WITH_EDITORONLY_DATA

	/** If true then active effects rebase the simulation positions to not lose precision. Can be turned off if not needed to skip unnecessary rebasing calculations. */
	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = ( ConfigRestartRequired = true ))
	bool bSystemsSupportLargeWorldCoordinates = true;

	/**
	 If set to true, types like positions and vectors cannot be assigned to each other without an explicit conversion step.
	 If false, type checks are loosened and some types can be implicitly converted into each other.
	 It is recommended to not disable this when working with large world coordinates. */
	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = ( DisplayName = "Enforce strict type checks in the graph" ))
	bool bEnforceStrictStackTypes = true;

	/** Default effect type to use for effects that don't define their own. Can be null. */
	UPROPERTY(config, EditAnywhere, Category = Niagara, meta = (AllowedClasses = "NiagaraEffectType"))
	FSoftObjectPath DefaultEffectType;

	/** Position pin type color. The other pin colors are defined in the general editor settings. */
	UPROPERTY(config, EditAnywhere, Category=Niagara)
	FLinearColor PositionPinTypeColor;

	/** The quality levels Niagara uses. */
	UPROPERTY(config, EditAnywhere, Category = Scalability)
	TArray<FText> QualityLevels;

	/** Info texts that the component renderer shows the user depending on the selected component class. */
	UPROPERTY(config, EditAnywhere, Category = Renderer)
	TMap<FString, FText> ComponentRendererWarningsPerClass;

	/** The default render target format used by all Niagara Render Target Data Interfaces unless overridden. */
	UPROPERTY(config, EditAnywhere, Category = Renderer)
	TEnumAsByte<ETextureRenderTargetFormat> DefaultRenderTargetFormat = RTF_RGBA16f;

	/** The default buffer format used by all Niagara Grid Data Interfaces unless overridden. */
	UPROPERTY(config, EditAnywhere, Category = Renderer)
	ENiagaraGpuBufferFormat DefaultGridFormat = ENiagaraGpuBufferFormat::HalfFloat;

	/** The default setting for motion vectors in Niagara renderers */
	UPROPERTY(config, EditAnywhere, Category = Renderer)
	ENiagaraDefaultRendererMotionVectorSetting DefaultRendererMotionVectorSetting = ENiagaraDefaultRendererMotionVectorSetting::Precise;

	/** The default setting for pixel coverage mode when automatic is set on the Niagara Renderer. */
	UPROPERTY(config, EditAnywhere, Category = Renderer)
	ENiagaraDefaultRendererPixelCoverageMode DefaultPixelCoverageMode = ENiagaraDefaultRendererPixelCoverageMode::Enabled;

	UPROPERTY(config, EditAnywhere, Category=SkeletalMeshDI, meta = ( DisplayName = "Gpu Max Bone Influences", ToolTip = "Controls the maximum number of influences we allow the Skeletal Mesh Data Interface to use on the GPU.  Changing this setting requires restarting the editor.", ConfigRestartRequired = true))
	TEnumAsByte<ENDISkelMesh_GpuMaxInfluences::Type> NDISkelMesh_GpuMaxInfluences;

	UPROPERTY(config, EditAnywhere, Category = SkeletalMeshDI, meta = (DisplayName = "Gpu Uniform Sampling Format", ToolTip = "Controls the format used for uniform sampling on the GPU.  Changing this setting requires restarting the editor.", ConfigRestartRequired = true))
	TEnumAsByte<ENDISkelMesh_GpuUniformSamplingFormat::Type> NDISkelMesh_GpuUniformSamplingFormat;

	UPROPERTY(config, EditAnywhere, Category = SkeletalMeshDI, meta = (DisplayName = "Adjacency Triangle Index Format", ToolTip = "Controls the format used for specifying triangle indexes in adjacency buffers.  Changing this setting requires restarting the editor.", ConfigRestartRequired = true))
	TEnumAsByte<ENDISkelMesh_AdjacencyTriangleIndexFormat::Type> NDISkelMesh_AdjacencyTriangleIndexFormat;

	/**
	When enabled the static mesh data interface is allowed to sample from the distance field data (if present) on the GPU.
	Enabling this feature will move all systems that contain static mesh samples into PostRenderOpaque tick group regardless of the features used.
	Changing this setting requires restarting the editor.
	*/
	UPROPERTY(config, EditAnywhere, Category = StaticMeshDI, meta = (DisplayName = "Allow Distance Fields (Experimental)", ConfigRestartRequired = true))
	bool NDIStaticMesh_AllowDistanceFields = false;

	/** 
	Defines how traces tagged as 'Project Default' will be interpreted when using the AsyncGpuTrace data interface.
	The system will go through (starting at element 0) to find the first provider that is available.
	*/
	UPROPERTY(config, EditAnywhere, Category = AsyncGpuTraceDI, meta = (DisplayName = "Trace Provider Priorities (Experimental)", ConfigRestartRequired = true))
	TArray<TEnumAsByte<ENDICollisionQuery_AsyncGpuTraceProvider::Type>> NDICollisionQuery_AsyncGpuTraceProviderOrder;

	// Begin UDeveloperSettings Interface
	virtual FName GetCategoryName() const override;
#if WITH_EDITOR
	void AddEnumParameterType(UEnum* Enum);
	virtual FText GetSectionText() const override;
#endif
	// END UDeveloperSettings Interface

	UNiagaraEffectType* GetDefaultEffectType()const;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;

public:
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnNiagaraSettingsChanged, const FName&, const UNiagaraSettings*);

	/** Gets a multicast delegate which is called whenever one of the parameters in this settings object changes. */
	static FOnNiagaraSettingsChanged& OnSettingsChanged();

protected:
	static FOnNiagaraSettingsChanged SettingsChangedDelegate;
#endif
};