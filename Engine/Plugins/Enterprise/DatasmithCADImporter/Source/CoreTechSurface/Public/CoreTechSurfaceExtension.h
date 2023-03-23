// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "ParametricSurfaceData.h"
#include "DatasmithAdditionalData.h"
#include "DatasmithCustomAction.h"
#include "DatasmithImportOptions.h"
#include "DatasmithUtils.h"

#include "CoreTechSurfaceExtension.generated.h"

UCLASS(meta = (DisplayName = "Kernel IO Parametric Surface Data"))
class CORETECHSURFACE_API UTempCoreTechParametricSurfaceData : public UParametricSurfaceData
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString SourceFile;
	
	virtual bool SetFile(const TCHAR* SourceFile) override;
	virtual bool Tessellate(UStaticMesh& StaticMesh, const FDatasmithRetessellationOptions& RetessellateOptions) override;

protected:
	virtual void Serialize(FArchive& Ar) override;
};
