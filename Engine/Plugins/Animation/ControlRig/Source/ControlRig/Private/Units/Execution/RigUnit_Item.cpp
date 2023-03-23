// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigUnit_Item.h"
#include "Units/Core/RigUnit_Name.h"
#include "Units/RigUnitContext.h"

FRigUnit_ItemExists_Execute()
{
    DECLARE_SCOPE_HIERARCHICAL_COUNTER_RIGUNIT()

	Exists = false;

	switch (Context.State)
	{
		case EControlRigState::Init:
		{
			CachedIndex.Reset();
			// fall through to update
		}
		case EControlRigState::Update:
		{
			Exists = CachedIndex.UpdateCache(Item, Context.Hierarchy);
	    	break;
	    }
	    default:
	    {
	    	break;
	    }
	}
}

FRigUnit_ItemReplace_Execute()
{
    DECLARE_SCOPE_HIERARCHICAL_COUNTER_RIGUNIT()

	Result = Item;
	FRigUnit_NameReplace::StaticExecute(RigVMExecuteContext, Item.Name, Old, New, Result.Name, Context);
}

FRigUnit_ItemEquals_Execute()
{
	Result = (A == B);
}

FRigUnit_ItemNotEquals_Execute()
{
	Result = (A != B);
}

FRigUnit_ItemTypeEquals_Execute()
{
	Result = (A.Type == B.Type);
}

FRigUnit_ItemTypeNotEquals_Execute()
{
	Result = (A.Type != B.Type);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Units/RigUnitTest.h"

IMPLEMENT_RIGUNIT_AUTOMATION_TEST(FRigUnit_ItemReplace)
{
	Unit.Item.Name = FName(TEXT("OldItemName"));
	Unit.Item.Type = ERigElementType::Bone;

	Unit.Old = FName(TEXT("Old"));
	Unit.New = FName(TEXT("New"));
	
	Execute();
	AddErrorIfFalse(Unit.Result == FRigElementKey(TEXT("NewItemName"), ERigElementType::Bone), TEXT("unexpected result"));

	Unit.Item.Name = FName(TEXT("OldItemName"));
	Unit.Item.Type = ERigElementType::Bone;

	Unit.Old = FName(TEXT("Old"));
	Unit.New = NAME_None;

	Execute();
	AddErrorIfFalse(Unit.Result == FRigElementKey(TEXT("ItemName"), ERigElementType::Bone), TEXT("unexpected result when New is None"));

	Unit.Item.Name = FName(TEXT("OldItemName"));
	Unit.Item.Type = ERigElementType::Bone;

	Unit.Old = NAME_None;
	Unit.New = FName(TEXT("New")); 

	Execute();
	AddErrorIfFalse(Unit.Result == FRigElementKey(TEXT("OldItemName"), ERigElementType::Bone), TEXT("unexpected result when Old is None"));
	return true;
}
#endif