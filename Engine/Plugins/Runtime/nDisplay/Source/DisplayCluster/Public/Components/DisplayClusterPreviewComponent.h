// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"

#include "DisplayClusterConfigurationTypes_Base.h"

#include "Render/Viewport/IDisplayClusterViewportManager.h"

#include "DisplayClusterPreviewComponent.generated.h"

class IDisplayClusterProjectionPolicy;
class UTextureRenderTarget2D;
class UTexture;
class UDisplayClusterConfigurationViewport;
class UMaterial;
class UMaterialInstanceDynamic;
class UDisplayClusterProjectionPolicyParameters;
class ADisplayClusterRootActor;
class UMeshComponent;

/**
 * nDisplay Viewport preview component (Editor)
 */
UCLASS(ClassGroup = (DisplayCluster))
class DISPLAYCLUSTER_API UDisplayClusterPreviewComponent
	: public UActorComponent
{
	friend class FDisplayClusterPreviewComponentDetailsCustomization;

	GENERATED_BODY()

public:
	UDisplayClusterPreviewComponent(const FObjectInitializer& ObjectInitializer);

#if WITH_EDITOR
public:
	virtual void OnComponentCreated() override;
	virtual void DestroyComponent(bool bPromoteChildren = false) override;

public:
	bool InitializePreviewComponent(ADisplayClusterRootActor* RootActor, const FString& ClusterNodeId, const FString& ViewportId, UDisplayClusterConfigurationViewport* ViewportConfig);

	const FString& GetViewportId() const
	{ 
		return ViewportId; 
	}

	const FString& GetClusterNodeId() const
	{
		return ClusterNodeId;
	}

	UDisplayClusterConfigurationViewport* GetViewportConfig() const
	{ 
		return ViewportConfig;
	}
	
	UTextureRenderTarget2D* GetRenderTargetTexture() const
	{
		return RenderTarget;
	}

	void UpdatePreviewResources();

	UMeshComponent* GetPreviewMesh()
	{
		UpdatePreviewMeshReference();

		return PreviewMesh;
	}

	/** Create and retrieve a render texture 2d from the render target. */
	UTexture* GetViewportPreviewTexture2D();

	void ResetPreviewComponent(bool bInRestoreSceneMaterial);

protected:
	bool IsPreviewEnabled() const;

	class IDisplayClusterViewport* GetCurrentViewport() const;
	bool GetPreviewTextureSettings(FIntPoint& OutSize, EPixelFormat& OutTextureFormat, float& OutGamma, bool& bOutSRGB) const;

	void UpdatePreviewRenderTarget();
	void ReleasePreviewRenderTarget();

	bool UpdatePreviewMesh();
	void ReleasePreviewMesh();
	void UpdatePreviewMeshReference();

	void InitializePreviewMaterial();
	void ReleasePreviewMaterial();
	void UpdatePreviewMaterial();

	void RestorePreviewMeshMaterial();
	void SetPreviewMeshMaterial();
#endif /* WITH_EDITOR */

#if WITH_EDITORONLY_DATA
protected:
	// Texture for preview material
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Preview", meta = (DisplayName = "Render Target"))
	UTextureRenderTarget2D* RenderTarget;

private:
	// Saved mesh policy params
	UPROPERTY()
	FDisplayClusterConfigurationProjection WarpMeshSavedProjectionPolicy;

	UPROPERTY()
	ADisplayClusterRootActor* RootActor = nullptr;

	UPROPERTY()
	FString ViewportId;

	UPROPERTY()
	FString ClusterNodeId;

	UPROPERTY()
	UDisplayClusterConfigurationViewport* ViewportConfig = nullptr;

	UPROPERTY()
	UMeshComponent* PreviewMesh = nullptr;

	UPROPERTY()
	bool bIsRootActorPreviewMesh = false;

	UPROPERTY()
	UMaterial* OriginalMaterial = nullptr;

	UPROPERTY()
	UMaterial* PreviewMaterial = nullptr;

	UPROPERTY()
	UMaterialInstanceDynamic* PreviewMaterialInstance = nullptr;

#endif /*WITH_EDITORONLY_DATA*/
};
