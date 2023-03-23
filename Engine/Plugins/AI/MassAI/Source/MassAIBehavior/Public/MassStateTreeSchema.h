// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "StateTreeSchema.h"
#include "MassStateTreeSchema.generated.h"

/**
 * StateTree Schema for Mass behaviors.
 */
UCLASS(BlueprintType, EditInlineNew, CollapseCategories, meta = (DisplayName = "Mass Behavior"))
class MASSAIBEHAVIOR_API UMassStateTreeSchema : public UStateTreeSchema
{
	GENERATED_BODY()

protected:

	virtual UScriptStruct* GetStorageSuperStruct() const override;
	virtual bool IsStructAllowed(const UScriptStruct* InScriptStruct) const override;
	virtual bool IsExternalItemAllowed(const UStruct& InStruct) const override;
};

