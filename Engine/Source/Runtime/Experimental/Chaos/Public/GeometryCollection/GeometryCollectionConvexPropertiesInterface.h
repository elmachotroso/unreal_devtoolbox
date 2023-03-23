// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GeometryCollection/ManagedArrayCollection.h"

class FGeometryCollection;

class CHAOS_API FGeometryCollectionConvexPropertiesInterface : public FManagedArrayInterface
{
public :
	typedef FManagedArrayInterface Super;
	using FManagedArrayInterface::ManagedCollection;

	// Convex Properties Group Name
	static const FName ConvexPropertiesGroup; 
	// Attribute
	static const FName ConvexIndexAttribute;
	// Attribute
	static const FName ConvexEnable;
	// Attribute
	static const FName ConvexFractionRemoveAttribute;
	// Attribute
	static const FName ConvexSimplificationThresholdAttribute;
	// Attribute
	static const FName ConvexCanExceedFractionAttribute;


	struct FConvexCreationProperties {
		bool Enable = true;
		float FractionRemove = 0.5f;
		float SimplificationThreshold = 10.0f;
		float CanExceedFraction = 0.5f;
	};

	FGeometryCollectionConvexPropertiesInterface(FGeometryCollection* InGeometryCollection);

	void InitializeInterface() override;

	void CleanInterfaceForCook() override;
	
	void RemoveInterfaceAttributes() override;

	FConvexCreationProperties GetConvexProperties(int TransformGroupIndex = INDEX_NONE) const;
	void SetConvexProperties(const FConvexCreationProperties&, int TransformGroupIndex = INDEX_NONE);
private:

	void SetDefaultProperty();
	FConvexCreationProperties GetDefaultProperty() const;
};

