// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "IHasPersonaToolkit.h"
#include "IKRetargetEditorController.h"
#include "IKRetargetMode.h"
#include "IPersonaPreviewScene.h"
#include "PersonaAssetEditorToolkit.h"
#include "EditorUndoClient.h"

class IAnimationSequenceBrowser;
class UIKRetargetSkeletalMeshComponent;
class IDetailsView;
class FGGAssetEditorToolbar;
class UIKRetargeter;
class FIKRetargetToolbar;
class SIKRetargetViewportTabBody;
class FIKRetargetPreviewScene;
struct FIKRetargetPose;
class SEditableTextBox;

namespace IKRetargetEditorModes
{
	extern const FName IKRetargetEditorMode;
}

class FIKRetargetEditor :
	public FPersonaAssetEditorToolkit,
	public IHasPersonaToolkit,
	public FGCObject,
	public FSelfRegisteringEditorUndoClient,
	public FTickableEditorObject
{
public:

	FIKRetargetEditor();
	virtual ~FIKRetargetEditor() override;

	void InitAssetEditor(
		const EToolkitMode::Type Mode,
		const TSharedPtr< IToolkitHost >& InitToolkitHost,
		UIKRetargeter* Asset);

	/** FAssetEditorToolkit interface */
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FText GetToolkitName() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	/** END FAssetEditorToolkit interface */
	
	/** FGCObject interface */
	virtual void AddReferencedObjects(FReferenceCollector& Collector) override;
	virtual FString GetReferencerName() const override
	{
		return TEXT("FIKRetargetEditor");
	}
	/** END FGCObject interface */

	//** FTickableEditorObject Interface
	virtual void Tick(float DeltaTime) override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
	virtual TStatId GetStatId() const override;
	//~ END FTickableEditorObject Interface

	/** IHasPersonaToolkit interface */
	virtual TSharedRef<IPersonaToolkit> GetPersonaToolkit() const override { return EditorController->PersonaToolkit.ToSharedRef(); }
	/** END IHasPersonaToolkit interface */

	/** FSelfRegisteringEditorUndoClient interface */
	virtual void PostUndo( bool bSuccess );
	virtual void PostRedo( bool bSuccess );
	/** END FSelfRegisteringEditorUndoClient interface */

	TSharedRef<FIKRetargetEditorController> GetController() const {return EditorController;};

private:

	/** toolbar */
	void BindCommands();
	void ExtendToolbar();
	void FillToolbar(FToolBarBuilder& ToolbarBuilder);
	/** END toolbar */
	
	/** preview scene setup */
	void HandlePreviewSceneCreated(const TSharedRef<IPersonaPreviewScene>& InPersonaPreviewScene);
	void HandleDetailsCreated(const TSharedRef<class IDetailsView>& InDetailsView);
	void OnFinishedChangingDetails(const FPropertyChangedEvent& PropertyChangedEvent);
	void SetupAnimInstance();
	/** END preview scene setup */
	
	/** centralized management across all views */
	TSharedRef<FIKRetargetEditorController> EditorController;
	
	friend FIKRetargetMode;
};
