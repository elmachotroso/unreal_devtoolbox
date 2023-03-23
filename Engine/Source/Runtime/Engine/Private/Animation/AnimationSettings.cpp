// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/AnimationSettings.h"
#include "Animation/AnimCompress_BitwiseCompressOnly.h"

UAnimationSettings::UAnimationSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, CompressCommandletVersion(2)	// Bump this up to trigger full recompression. Otherwise only new animations imported will be recompressed.
	, ForceRecompression(false)
	, bForceBelowThreshold(false)
	, bFirstRecompressUsingCurrentOrDefault(true)
	, bRaiseMaxErrorToExisting(false)
	, bEnablePerformanceLog(false)
	, bTickAnimationOnSkeletalMeshInit(true)
	, DefaultAttributeBlendMode(ECustomAttributeBlendType::Blend)
{
	SectionName = TEXT("Animation");

	KeyEndEffectorsMatchNameArray.Add(TEXT("IK"));
	KeyEndEffectorsMatchNameArray.Add(TEXT("eye"));
	KeyEndEffectorsMatchNameArray.Add(TEXT("weapon"));
	KeyEndEffectorsMatchNameArray.Add(TEXT("hand"));
	KeyEndEffectorsMatchNameArray.Add(TEXT("attach"));
	KeyEndEffectorsMatchNameArray.Add(TEXT("camera"));

	MirrorFindReplaceExpressions = {
		FMirrorFindReplaceExpression("r_", "l_", EMirrorFindReplaceMethod::Prefix), FMirrorFindReplaceExpression("l_", "r_", EMirrorFindReplaceMethod::Prefix),
		FMirrorFindReplaceExpression("R_", "L_", EMirrorFindReplaceMethod::Prefix), FMirrorFindReplaceExpression("L_", "R_", EMirrorFindReplaceMethod::Prefix),
		FMirrorFindReplaceExpression("_l", "_r", EMirrorFindReplaceMethod::Suffix), FMirrorFindReplaceExpression("_r", "_l",  EMirrorFindReplaceMethod::Suffix),
		FMirrorFindReplaceExpression("_R", "_L", EMirrorFindReplaceMethod::Suffix), FMirrorFindReplaceExpression("_L", "_R", EMirrorFindReplaceMethod::Suffix),
		FMirrorFindReplaceExpression("right", "left", EMirrorFindReplaceMethod::Prefix), FMirrorFindReplaceExpression("left", "right",  EMirrorFindReplaceMethod::Prefix),
		FMirrorFindReplaceExpression("Right", "Left", EMirrorFindReplaceMethod::Prefix), FMirrorFindReplaceExpression("Left", "Right",  EMirrorFindReplaceMethod::Prefix),
		FMirrorFindReplaceExpression("((?:^[sS]pine|^[rR]oot|^[pP]elvis|^[nN]eck|^[hH]ead|^ik_hand_gun).*)", "$1", EMirrorFindReplaceMethod::RegularExpression)
	};
}

TArray<FString> UAnimationSettings::GetBoneCustomAttributeNamesToImport() const
{
	TArray<FString> AttributeNames = {
		BoneTimecodeCustomAttributeNameSettings.HourAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.MinuteAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.SecondAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.FrameAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.SubframeAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.RateAttributeName.ToString(),
		BoneTimecodeCustomAttributeNameSettings.TakenameAttributeName.ToString()
	};

	for (const FCustomAttributeSetting& Setting : BoneCustomAttributesNames)
	{
		AttributeNames.AddUnique(Setting.Name);
	}

	return AttributeNames;
}


#if WITH_EDITOR
void UAnimationSettings::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

// 	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;
// 	if (PropertyName == GET_MEMBER_NAME_CHECKED(UAnimationSettings, FrictionCombineMode))
// 	{
// 		UPhysicalMaterial::RebuildPhysicalMaterials();
// 	}
// 	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UAnimationSettings, LockedAxis))
// 	{
// 		UMovementComponent::PhysicsLockedAxisSettingChanged();
// 	}
}


#endif	// WITH_EDITOR
