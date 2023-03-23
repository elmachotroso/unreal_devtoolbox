// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	AnimMontage.cpp: Montage classes that contains slots
=============================================================================*/ 

#include "Animation/EditorAnimCompositeSegment.h"
#include "Animation/AnimComposite.h"



UEditorAnimCompositeSegment::UEditorAnimCompositeSegment(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	AnimSegmentIndex = 0;
}

void UEditorAnimCompositeSegment::InitAnimSegment(int AnimSegmentIndexIn)
{
	AnimSegmentIndex = AnimSegmentIndexIn;
	if(UAnimComposite* Composite = Cast<UAnimComposite>(AnimObject))
	{
		if(Composite->AnimationTrack.AnimSegments.IsValidIndex(AnimSegmentIndex))
		{
			AnimSegment = Composite->AnimationTrack.AnimSegments[AnimSegmentIndex];
		}
	}
}

bool UEditorAnimCompositeSegment::ApplyChangesToMontage()
{
	if(UAnimComposite* Composite = Cast<UAnimComposite>(AnimObject))
	{
		if(Composite->AnimationTrack.AnimSegments.IsValidIndex(AnimSegmentIndex))
		{
			if (AnimSegment.AnimReference && Composite->GetSkeleton()->IsCompatible(AnimSegment.AnimReference->GetSkeleton()))
			{
				Composite->AnimationTrack.AnimSegments[AnimSegmentIndex] = AnimSegment;
				return true;
			}
			else
			{
				AnimSegment.AnimReference = Composite->AnimationTrack.AnimSegments[AnimSegmentIndex].AnimReference;
				return false;
			}
		}
	}

	return false;
}

bool UEditorAnimCompositeSegment::PropertyChangeRequiresRebuild(FPropertyChangedEvent& PropertyChangedEvent)
{
	const FName PropertyName = PropertyChangedEvent.Property ? PropertyChangedEvent.Property->GetFName() : NAME_None;

	// Changing the end or start time of the segment length can't change the order of the montage segments.
	// Return false so that the montage editor does not fully rebuild its UI and we can keep this UEditorAnimSegment object 
	// in the details view. (A better solution would be handling the rebuilding in a way that didn't possibly invalidate the UEditorMontageObj in the details view)
	return !(PropertyName == GET_MEMBER_NAME_CHECKED(FAnimSegment, AnimEndTime) || PropertyName == GET_MEMBER_NAME_CHECKED(FAnimSegment, AnimStartTime) || PropertyName == GET_MEMBER_NAME_CHECKED(FAnimSegment, AnimPlayRate) || PropertyName == GET_MEMBER_NAME_CHECKED(FAnimSegment, LoopingCount));
}
