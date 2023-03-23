// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassEntityTypes.h"
#include "StructUtilsTypes.h"

uint32 FMassArchetypeSharedFragmentValues::CalculateHash() const
{
	checkf(bSorted, TEXT("Expecting the containers to be sorted for the hash caluclation to be correct"));

	// Fragments are not part of the uniqueness 
	uint32 Hash = 0;
	for (const FConstSharedStruct& Fragment : ConstSharedFragments)
	{
		Hash = PointerHash(Fragment.GetMemory(), Hash);
	}

	for (const FSharedStruct& Fragment : SharedFragments)
	{
		Hash = PointerHash(Fragment.GetMemory(), Hash);
	}

	return Hash;
}
