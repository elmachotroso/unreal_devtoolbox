// Copyright Epic Games, Inc. All Rights Reserved.

#include "AnimGraphNode_RetargetPoseFromMesh.h"
#include "Animation/AnimInstance.h"
#include "Kismet2/CompilerResultsLog.h"

#define LOCTEXT_NAMESPACE "AnimGraphNode_IKRig"
const FName UAnimGraphNode_RetargetPoseFromMesh::AnimModeName(TEXT("IKRig.IKRigEditor.IKRigEditMode"));

void UAnimGraphNode_RetargetPoseFromMesh::Draw(FPrimitiveDrawInterface* PDI, USkeletalMeshComponent* PreviewSkelMeshComp) const
{
}

FText UAnimGraphNode_RetargetPoseFromMesh::GetNodeTitle(ENodeTitleType::Type TitleType) const
{
	return LOCTEXT("AnimGraphNode_IKRetargeter_Title", "Retarget Pose From Mesh");
}

void UAnimGraphNode_RetargetPoseFromMesh::CopyNodeDataToPreviewNode(FAnimNode_Base* InPreviewNode)
{
	FAnimNode_RetargetPoseFromMesh* IKRetargeterNode = static_cast<FAnimNode_RetargetPoseFromMesh*>(InPreviewNode);
}

FEditorModeID UAnimGraphNode_RetargetPoseFromMesh::GetEditorMode() const
{
	return AnimModeName;
}

void UAnimGraphNode_RetargetPoseFromMesh::CustomizePinData(UEdGraphPin* Pin, FName SourcePropertyName, int32 ArrayIndex) const
{
	Super::CustomizePinData(Pin, SourcePropertyName, ArrayIndex);
}

void UAnimGraphNode_RetargetPoseFromMesh::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = (PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None);
	if ((PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FAnimNode_RetargetPoseFromMesh, bUseAttachedParent)))
	{
		ReconstructNode();
	}
}

void UAnimGraphNode_RetargetPoseFromMesh::ValidateAnimNodeDuringCompilation(USkeleton* ForSkeleton,	FCompilerResultsLog& MessageLog)
{
	Super::ValidateAnimNodeDuringCompilation(ForSkeleton, MessageLog);

	// validate source mesh component is not null
	if (!Node.bUseAttachedParent)
	{
		if (!IsPinExposedAndLinked(GET_MEMBER_NAME_STRING_CHECKED(FAnimNode_RetargetPoseFromMesh, SourceMeshComponent)))
		{
			MessageLog.Error(TEXT("@@ is missing a Source Skeletal Mesh Component reference."), this);
			return;
		}
	}

	// validate IK Rig asset has been assigned
	if (!Node.IKRetargeterAsset)
	{
		MessageLog.Error(TEXT("@@ is missing an IKRetargeter asset."), this);
		return;
	}

	// validate SOURCE IK Rig asset has been assigned
	if (!Node.IKRetargeterAsset->GetSourceIKRig())
	{
		MessageLog.Error(TEXT("@@ has IK Retargeter that is missing a source IK Rig asset."), this);
	}

	// validate TARGET IK Rig asset has been assigned
	if (!Node.IKRetargeterAsset->GetTargetIKRig())
	{
		MessageLog.Error(TEXT("@@ has IK Retargeter that is missing a target IK Rig asset."), this);
	}

	if (!(Node.IKRetargeterAsset->GetSourceIKRig() && Node.IKRetargeterAsset->GetTargetIKRig()))
	{
		return;
	}
}

void UAnimGraphNode_RetargetPoseFromMesh::PreloadRequiredAssets()
{
	Super::PreloadRequiredAssets();
	
	if (Node.IKRetargeterAsset)
	{
		PreloadObject(Node.IKRetargeterAsset);
		PreloadObject(Node.IKRetargeterAsset->GetSourceIKRigWriteable());
		PreloadObject(Node.IKRetargeterAsset->GetTargetIKRigWriteable());
	}
}

#undef LOCTEXT_NAMESPACE
