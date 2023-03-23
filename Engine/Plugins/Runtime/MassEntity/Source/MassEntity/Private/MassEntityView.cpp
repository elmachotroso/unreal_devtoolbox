// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassEntityView.h"
#include "MassEntitySubsystem.h"
#include "MassArchetypeData.h"


//////////////////////////////////////////////////////////////////////
// FMassEntityView
FMassEntityView::FMassEntityView(const FMassArchetypeHandle& ArchetypeHandle, FMassEntityHandle InEntity)
{
	Entity = InEntity;
	check(ArchetypeHandle.IsValid());
	Archetype = ArchetypeHandle.DataPtr.Get();
	EntityHandle = Archetype->MakeEntityHandle(Entity);
}

FMassEntityView::FMassEntityView(const UMassEntitySubsystem& EntitySubsystem, FMassEntityHandle InEntity)
{
	Entity = InEntity;
	const FMassArchetypeHandle ArchetypeHandle = EntitySubsystem.GetArchetypeForEntity(Entity);
	check(ArchetypeHandle.IsValid());
	Archetype = ArchetypeHandle.DataPtr.Get();
	EntityHandle = Archetype->MakeEntityHandle(Entity);
}

void* FMassEntityView::GetFragmentPtr(const UScriptStruct& FragmentType) const
{
	checkSlow(Archetype && EntityHandle.IsValid());
	if (const int32* FragmentIndex = Archetype->GetFragmentIndex(&FragmentType))
	{
		// failing the below Find means given entity's archetype is missing given FragmentType
		return Archetype->GetFragmentData(*FragmentIndex, EntityHandle);
	}
	return nullptr;
}

void* FMassEntityView::GetFragmentPtrChecked(const UScriptStruct& FragmentType) const
{
	checkSlow(Archetype && EntityHandle.IsValid());
	const int32 FragmentIndex = Archetype->GetFragmentIndexChecked(&FragmentType);
	return Archetype->GetFragmentData(FragmentIndex, EntityHandle);
}

const void* FMassEntityView::GetConstSharedFragmentPtr(const UScriptStruct& FragmentType) const
{
	const FConstSharedStruct* SharedFragment = Archetype->GetSharedFragmentValues().GetConstSharedFragments().FindByPredicate(FStructTypeEqualOperator(&FragmentType));
	return (SharedFragment != nullptr) ? SharedFragment->GetMemory() : nullptr;
}

const void* FMassEntityView::GetConstSharedFragmentPtrChecked(const UScriptStruct& FragmentType) const
{
	const FConstSharedStruct* SharedFragment = Archetype->GetSharedFragmentValues().GetConstSharedFragments().FindByPredicate(FStructTypeEqualOperator(&FragmentType));
	check(SharedFragment != nullptr);
	return SharedFragment->GetMemory();
}

void* FMassEntityView::GetSharedFragmentPtr(const UScriptStruct& FragmentType) const
{
	const FSharedStruct* SharedFragment = Archetype->GetSharedFragmentValues().GetSharedFragments().FindByPredicate(FStructTypeEqualOperator(&FragmentType));
	return (SharedFragment != nullptr) ? SharedFragment->GetMutableMemory() : nullptr;
}

void* FMassEntityView::GetSharedFragmentPtrChecked(const UScriptStruct& FragmentType) const
{
	const FSharedStruct* SharedFragment = Archetype->GetSharedFragmentValues().GetSharedFragments().FindByPredicate(FStructTypeEqualOperator(&FragmentType));
	check(SharedFragment != nullptr);
	return SharedFragment->GetMutableMemory();
}

bool FMassEntityView::HasTag(const UScriptStruct& TagType) const
{
	checkSlow(Archetype && EntityHandle.IsValid());
	return Archetype->HasTagType(&TagType);
}