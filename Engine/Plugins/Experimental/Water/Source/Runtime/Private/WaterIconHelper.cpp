// Copyright Epic Games, Inc. All Rights Reserved.

#include "WaterIconHelper.h"

#if WITH_EDITOR

#include "Components/BillboardComponent.h"
#include "Engine/Texture2D.h"
#include "WaterRuntimeSettings.h"
#include "WaterSubsystem.h"
#include "Modules/ModuleManager.h"
#include "WaterModule.h"

UBillboardComponent* FWaterIconHelper::EnsureSpriteComponentCreated_Internal(AActor* Actor, UClass* InClass, const TCHAR* InIconTextureName)
{
	UBillboardComponent* ActorIcon = nullptr;
	
	ActorIcon = Actor->FindComponentByClass<UBillboardComponent>();
	if (ActorIcon == nullptr)
	{
		ActorIcon = Actor->CreateEditorOnlyDefaultSubobject<UBillboardComponent>(TEXT("Sprite"), true);
	}
	if (ActorIcon != nullptr)
	{
		ConstructorHelpers::FObjectFinderOptional<UTexture2D> Texture(InIconTextureName);
		IWaterModuleInterface& WaterModule = FModuleManager::GetModuleChecked<IWaterModuleInterface>(TEXT("Water"));
		if (IWaterEditorServices* WaterEditorServices = WaterModule.GetWaterEditorServices())
		{
			WaterEditorServices->RegisterWaterActorSprite(InClass, Texture.Get());
		}
		ActorIcon->Sprite = Texture.Get();
		ActorIcon->bHiddenInGame = true;
		ActorIcon->SpriteInfo.Category = TEXT("Water");
		ActorIcon->SpriteInfo.DisplayName = NSLOCTEXT("SpriteCategory", "Water", "Water");
		ActorIcon->SetupAttachment(Actor->GetRootComponent());
		UpdateSpriteComponent(Actor, ActorIcon->Sprite);
	}
	return ActorIcon;
}

void FWaterIconHelper::UpdateSpriteComponent(AActor* Actor, UTexture2D* InTexture)
{
	if (UBillboardComponent* ActorIcon = Actor->FindComponentByClass<UBillboardComponent>())
	{
		float TargetSize = GetDefault<UWaterRuntimeSettings>()->WaterBodyIconWorldSize;
		FVector ZOffset(0.0f, 0.0f, GetDefault<UWaterRuntimeSettings>()->WaterBodyIconWorldZOffset);
		if (InTexture != nullptr)
		{
			// Use the texture source's size as the texture might not having finished loading when this runs, in which case the default texture's size would be returned : 
			int32 TextureSize = FMath::Max(InTexture->Source.GetSizeX(), InTexture->Source.GetSizeY());
			float Scale = (TextureSize > 0) ? (TargetSize / (float)TextureSize) : 1.0f;
			ActorIcon->SetRelativeScale3D(FVector(Scale));
		}
		ActorIcon->Sprite = InTexture;
		ActorIcon->SetRelativeLocation(ZOffset);
		ActorIcon->bIsScreenSizeScaled = true;
		ActorIcon->MarkRenderStateDirty();
	}
}

#endif