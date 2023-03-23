// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/CoreDelegates.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UnrealType.h"
#include "Engine/Blueprint.h"
#include "Components/ActorComponent.h"
#include "Components/SceneComponent.h"
#include "GameFramework/Actor.h"
#include "Components/ChildActorComponent.h"
#include "Components/PrimitiveComponent.h"
#include "AI/NavigationSystemBase.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/DataLayer/WorldDataLayers.h"
#include "WorldPartition/DataLayer/DataLayer.h"
#include "EditorSupportDelegates.h"
#include "Logging/TokenizedMessage.h"
#include "Logging/MessageLog.h"
#include "Misc/UObjectToken.h"
#include "LevelUtils.h"
#include "Misc/MapErrors.h"
#include "ActorEditorUtils.h"
#include "EngineGlobals.h"

#if WITH_EDITOR

#include "Editor.h"
#include "Engine/LevelStreaming.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "Folder.h"
#include "ActorFolder.h"
#include "WorldPersistentFolders.h"

#define LOCTEXT_NAMESPACE "ErrorChecking"

void AActor::PreEditChange(FProperty* PropertyThatWillChange)
{
	Super::PreEditChange(PropertyThatWillChange);

	FObjectProperty* ObjProp = CastField<FObjectProperty>(PropertyThatWillChange);
	UBlueprintGeneratedClass* BPGC = Cast<UBlueprintGeneratedClass>(GetClass());
	if ( BPGC != nullptr && ObjProp != nullptr )
	{
		BPGC->UnbindDynamicDelegatesForProperty(this, ObjProp);
	}

	// During SIE, allow components to be unregistered here, and then reregistered and reconstructed in PostEditChangeProperty.
	if ((GEditor && GEditor->bIsSimulatingInEditor) || ReregisterComponentsWhenModified())
	{
		UnregisterAllComponents();
	}

	PreEditChangeDataLayers.Reset();
	if (PropertyThatWillChange != nullptr &&
		(PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, DataLayers) ||
		 PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(FActorDataLayer, Name)))
	{
		PreEditChangeDataLayers = DataLayers;
	}
}

bool AActor::CanEditChange(const FProperty* PropertyThatWillChange) const
{
	if ((PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, Layers)) ||
		(PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, ActorGuid)))
	{
		return false;
	}

	const bool bIsSpatiallyLoadedProperty = PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, bIsSpatiallyLoaded);
	const bool bIsRuntimeGridProperty = PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, RuntimeGrid);
	const bool bIsDataLayersProperty = PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, DataLayers);
	const bool bIsHLODLayerProperty = PropertyThatWillChange->GetFName() == GET_MEMBER_NAME_CHECKED(AActor, HLODLayer);

	if (bIsSpatiallyLoadedProperty || bIsRuntimeGridProperty || bIsDataLayersProperty || bIsHLODLayerProperty)
	{
		if (!IsTemplate())
		{
			if (UWorld* World = GetTypedOuter<UWorld>())
			{
				const bool bIsPartitionedWorld = UWorld::HasSubsystem<UWorldPartitionSubsystem>(World);
				if (!bIsPartitionedWorld)
				{
					return false;
				}
			}
		}
	}

	if (bIsSpatiallyLoadedProperty && !CanChangeIsSpatiallyLoadedFlag())
	{
		return false;
	}

	if (bIsDataLayersProperty && !SupportsDataLayer())
	{
		return false;
	}

	return Super::CanEditChange(PropertyThatWillChange);
}

static FName Name_RelativeLocation = USceneComponent::GetRelativeLocationPropertyName();
static FName Name_RelativeRotation = USceneComponent::GetRelativeRotationPropertyName();
static FName Name_RelativeScale3D = USceneComponent::GetRelativeScale3DPropertyName();

void AActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	FProperty* MemberPropertyThatChanged = PropertyChangedEvent.MemberProperty;
	const FName MemberPropertyName = MemberPropertyThatChanged != NULL ? MemberPropertyThatChanged->GetFName() : NAME_None;

	if (IsPropertyChangedAffectingDataLayers(PropertyChangedEvent))
	{
		FixupDataLayers(/*bRevertChangesOnLockedDataLayer*/true);
	}

	const bool bTransformationChanged = (MemberPropertyName == Name_RelativeLocation || MemberPropertyName == Name_RelativeRotation || MemberPropertyName == Name_RelativeScale3D);

	// During SIE, allow components to reregistered and reconstructed in PostEditChangeProperty.
	// This is essential as construction is deferred during spawning / duplication when in SIE.
	if ((GEditor && GEditor->bIsSimulatingInEditor && GetWorld() != nullptr) || ReregisterComponentsWhenModified())
	{
		// In the Undo case we have an annotation storing information about constructed components and we do not want
		// to improperly apply out of date changes so we need to skip registration of all blueprint created components
		// and defer instance components attached to them until after rerun
		if (CurrentTransactionAnnotation.IsValid())
		{
			UnregisterAllComponents();

			TInlineComponentArray<UActorComponent*> Components;
			GetComponents(Components);

			Components.Sort([](UActorComponent& A, UActorComponent& B)
			{
				if (&B == B.GetOwner()->GetRootComponent())
				{
					return false;
				}
				if (USceneComponent* ASC = Cast<USceneComponent>(&A))
				{
					if (ASC->GetAttachParent() == &B)
					{
						return false;
					}
				}
				return true;
			});

			bool bRequiresReregister = false;
			for (UActorComponent* Component : Components)
			{
				if (Component->CreationMethod == EComponentCreationMethod::Native)
				{
					Component->RegisterComponent();
				}
				else if (Component->CreationMethod == EComponentCreationMethod::Instance)
				{
					USceneComponent* SC = Cast<USceneComponent>(Component);
					if (SC == nullptr || SC == RootComponent || (SC->GetAttachParent() && SC->GetAttachParent()->IsRegistered()))
					{
						Component->RegisterComponent();
					}
					else
					{
						bRequiresReregister = true;
					}
				}
				else
				{
					bRequiresReregister = true;
				}
			}

			RerunConstructionScripts();

			if (bRequiresReregister)
			{
				ReregisterAllComponents();
			}
			else
			{
				PostRegisterAllComponents();
			}
		}
		else
		{
			UnregisterAllComponents();
			RerunConstructionScripts();
			ReregisterAllComponents();
		}
	}

	// Let other systems know that an actor was moved
	if (bTransformationChanged)
	{
		GEngine->BroadcastOnActorMoved( this );
	}

	FEditorSupportDelegates::UpdateUI.Broadcast();
	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void AActor::PostEditMove(bool bFinished)
{
	if ( ReregisterComponentsWhenModified() && !FLevelUtils::IsMovingLevel())
	{
		UBlueprint* Blueprint = Cast<UBlueprint>(GetClass()->ClassGeneratedBy);
		if (bFinished || bRunConstructionScriptOnDrag || (Blueprint && Blueprint->bRunConstructionScriptOnDrag))
		{
			FNavigationLockContext NavLock(GetWorld(), ENavigationLockReason::AllowUnregister);
			RerunConstructionScripts();
		}
	}

	if (!FLevelUtils::IsMovingLevel())
	{
		GEngine->BroadcastOnActorMoving(this);
	}

	if ( bFinished )
	{
		UWorld* World = GetWorld();

		World->UpdateCullDistanceVolumes(this);
		World->bAreConstraintsDirty = true;

		FEditorSupportDelegates::RefreshPropertyWindows.Broadcast();

		// Let other systems know that an actor was moved
		GEngine->BroadcastOnActorMoved( this );

		FEditorSupportDelegates::UpdateUI.Broadcast();
	}

	// If the root component was not just recreated by the construction script - call PostEditComponentMove on it
	if(RootComponent != NULL && !RootComponent->IsCreatedByConstructionScript())
	{
		RootComponent->PostEditComponentMove(bFinished);
	}

	if (bFinished)
	{
		FNavigationSystem::OnPostEditActorMove(*this);
	}
}

bool AActor::ReregisterComponentsWhenModified() const
{
	// For child actors, redirect to the parent's owner (we do the same in RerunConstructionScripts).
	if (const AActor* ParentActor = GetParentActor())
	{
		return ParentActor->ReregisterComponentsWhenModified();
	}

	return !bActorIsBeingConstructed && !IsTemplate() && !GetOutermost()->HasAnyPackageFlags(PKG_PlayInEditor) && GetWorld() != nullptr;
}

void AActor::DebugShowComponentHierarchy(  const TCHAR* Info, bool bShowPosition )
{	
	TArray<AActor*> ParentedActors;
	GetAttachedActors( ParentedActors );
	if( Info  )
	{
		UE_LOG( LogActor, Warning, TEXT("--%s--"), Info );
	}
	else
	{
		UE_LOG( LogActor, Warning, TEXT("--------------------------------------------------") );
	}
	UE_LOG( LogActor, Warning, TEXT("--------------------------------------------------") );
	UE_LOG( LogActor, Warning, TEXT("Actor [%x] (%s)"), this, *GetFName().ToString() );
	USceneComponent* SceneComp = GetRootComponent();
	if( SceneComp )
	{
		int32 NestLevel = 0;
		DebugShowOneComponentHierarchy( SceneComp, NestLevel, bShowPosition );			
	}
	else
	{
		UE_LOG( LogActor, Warning, TEXT("Actor has no root.") );		
	}
	UE_LOG( LogActor, Warning, TEXT("--------------------------------------------------") );
}

void AActor::DebugShowOneComponentHierarchy( USceneComponent* SceneComp, int32& NestLevel, bool bShowPosition )
{
	FString Nest = "";
	for (int32 iNest = 0; iNest < NestLevel ; iNest++)
	{
		Nest = Nest + "---->";	
	}
	NestLevel++;
	FString PosString;
	if( bShowPosition )
	{
		FVector Posn = SceneComp->GetComponentTransform().GetLocation();
		//PosString = FString::Printf( TEXT("{R:%f,%f,%f- W:%f,%f,%f}"), SceneComp->RelativeLocation.X, SceneComp->RelativeLocation.Y, SceneComp->RelativeLocation.Z, Posn.X, Posn.Y, Posn.Z );
		PosString = FString::Printf( TEXT("{R:%f- W:%f}"), SceneComp->GetRelativeLocation().Z, Posn.Z );
	}
	else
	{
		PosString = "";
	}
	AActor* OwnerActor = SceneComp->GetOwner();
	if( OwnerActor )
	{
		UE_LOG(LogActor, Warning, TEXT("%sSceneComp [%x] (%s) Owned by %s %s"), *Nest, SceneComp, *SceneComp->GetFName().ToString(), *OwnerActor->GetFName().ToString(), *PosString );
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("%sSceneComp [%x] (%s) No Owner"), *Nest, SceneComp, *SceneComp->GetFName().ToString() );
	}
	if( SceneComp->GetAttachParent())
	{
		if( bShowPosition )
		{
			FVector Posn = SceneComp->GetComponentTransform().GetLocation();
			//PosString = FString::Printf( TEXT("{R:%f,%f,%f- W:%f,%f,%f}"), SceneComp->RelativeLocation.X, SceneComp->RelativeLocation.Y, SceneComp->RelativeLocation.Z, Posn.X, Posn.Y, Posn.Z );
			PosString = FString::Printf( TEXT("{R:%f- W:%f}"), SceneComp->GetRelativeLocation().Z, Posn.Z );
		}
		else
		{
			PosString = "";
		}
		UE_LOG(LogActor, Warning, TEXT("%sAttachParent [%x] (%s) %s"), *Nest, SceneComp->GetAttachParent(), *SceneComp->GetAttachParent()->GetFName().ToString(), *PosString );
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("%s[NO PARENT]"), *Nest );
	}

	if( SceneComp->GetAttachChildren().Num() != 0 )
	{
		for (USceneComponent* EachSceneComp : SceneComp->GetAttachChildren())
		{			
			DebugShowOneComponentHierarchy(EachSceneComp,NestLevel, bShowPosition );
		}
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("%s[NO CHILDREN]"), *Nest );
	}
}

TSharedRef<AActor::FActorTransactionAnnotation> AActor::FActorTransactionAnnotation::Create()
{
	return MakeShareable(new FActorTransactionAnnotation());
}

TSharedRef<AActor::FActorTransactionAnnotation> AActor::FActorTransactionAnnotation::Create(const AActor* InActor, const bool InCacheRootComponentData)
{
	return MakeShareable(new FActorTransactionAnnotation(InActor, FComponentInstanceDataCache(InActor), InCacheRootComponentData));
}

TSharedPtr<AActor::FActorTransactionAnnotation> AActor::FActorTransactionAnnotation::CreateIfRequired(const AActor* InActor, const bool InCacheRootComponentData)
{
	// Don't create a transaction annotation for something that has no instance data, or a root component that's created by a construction script
	FComponentInstanceDataCache TempComponentInstanceData(InActor);
	if (!TempComponentInstanceData.HasInstanceData())
	{
		USceneComponent* ActorRootComponent = InActor->GetRootComponent();
		if (!InCacheRootComponentData || !ActorRootComponent || !ActorRootComponent->IsCreatedByConstructionScript())
		{
			return nullptr;
		}
	}

	return MakeShareable(new FActorTransactionAnnotation(InActor, MoveTemp(TempComponentInstanceData), InCacheRootComponentData));
}

AActor::FActorTransactionAnnotation::FActorTransactionAnnotation()
{
	ActorTransactionAnnotationData.bRootComponentDataCached = false;
}

AActor::FActorTransactionAnnotation::FActorTransactionAnnotation(const AActor* InActor, FComponentInstanceDataCache&& InComponentInstanceData, const bool InCacheRootComponentData)
{
	ActorTransactionAnnotationData.ComponentInstanceData = MoveTemp(InComponentInstanceData);
	ActorTransactionAnnotationData.Actor = InActor;

	USceneComponent* ActorRootComponent = InActor->GetRootComponent();
	if (InCacheRootComponentData && ActorRootComponent && ActorRootComponent->IsCreatedByConstructionScript())
	{
		ActorTransactionAnnotationData.bRootComponentDataCached = true;
		FActorRootComponentReconstructionData& RootComponentData = ActorTransactionAnnotationData.RootComponentData;
		RootComponentData.Transform = ActorRootComponent->GetComponentTransform();
		RootComponentData.Transform.SetTranslation(ActorRootComponent->GetComponentLocation()); // take into account any custom location
		RootComponentData.TransformRotationCache = ActorRootComponent->GetRelativeRotationCache();

		if (ActorRootComponent->GetAttachParent())
		{
			RootComponentData.AttachedParentInfo.Actor = ActorRootComponent->GetAttachParent()->GetOwner();
			RootComponentData.AttachedParentInfo.AttachParent = ActorRootComponent->GetAttachParent();
			RootComponentData.AttachedParentInfo.AttachParentName = ActorRootComponent->GetAttachParent()->GetFName();
			RootComponentData.AttachedParentInfo.SocketName = ActorRootComponent->GetAttachSocketName();
			RootComponentData.AttachedParentInfo.RelativeTransform = ActorRootComponent->GetRelativeTransform();
		}

		for (USceneComponent* AttachChild : ActorRootComponent->GetAttachChildren())
		{
			AActor* ChildOwner = (AttachChild ? AttachChild->GetOwner() : NULL);
			if (ChildOwner && ChildOwner != InActor)
			{
				// Save info about actor to reattach
				FActorRootComponentReconstructionData::FAttachedActorInfo Info;
				Info.Actor = ChildOwner;
				Info.SocketName = AttachChild->GetAttachSocketName();
				Info.RelativeTransform = AttachChild->GetRelativeTransform();
				RootComponentData.AttachedToInfo.Add(Info);
			}
		}
	}
	else
	{
		ActorTransactionAnnotationData.bRootComponentDataCached = false;
	}
}

void AActor::FActorTransactionAnnotation::AddReferencedObjects(FReferenceCollector& Collector)
{
	ActorTransactionAnnotationData.ComponentInstanceData.AddReferencedObjects(Collector);
}

void AActor::FActorTransactionAnnotation::Serialize(FArchive& Ar)
{
	Ar << ActorTransactionAnnotationData;
}

bool AActor::FActorTransactionAnnotation::HasInstanceData() const
{
	return (ActorTransactionAnnotationData.bRootComponentDataCached || ActorTransactionAnnotationData.ComponentInstanceData.HasInstanceData());
}

TSharedPtr<ITransactionObjectAnnotation> AActor::FactoryTransactionAnnotation(const ETransactionAnnotationCreationMode InCreationMode) const
{
	if (InCreationMode == ETransactionAnnotationCreationMode::DefaultInstance)
	{
		return FActorTransactionAnnotation::Create();
	}

	if (CurrentTransactionAnnotation.IsValid())
	{
		return CurrentTransactionAnnotation;
	}

	return FActorTransactionAnnotation::CreateIfRequired(this);
}

void AActor::PreEditUndo()
{
	// Check if this Actor needs to be re-instanced
	UClass* OldClass = GetClass();
	UClass* NewClass = OldClass->GetAuthoritativeClass();
	if (NewClass != OldClass)
	{
		// Empty the OwnedComponents array, it's filled with invalid information
		OwnedComponents.Empty();
	}

	IntermediateOwner = Owner;
	// Since child actor components will rebuild themselves get rid of the Actor before we make changes
	TInlineComponentArray<UChildActorComponent*> ChildActorComponents;
	GetComponents(ChildActorComponents);

	for (UChildActorComponent* ChildActorComponent : ChildActorComponents)
	{
		if (ChildActorComponent->IsCreatedByConstructionScript())
		{
			ChildActorComponent->DestroyChildActor();
		}
	}

	// let navigation system know to not care about this actor anymore
	FNavigationSystem::RemoveActorData(*this);

	Super::PreEditUndo();
}

bool AActor::InternalPostEditUndo()
{
	if (IntermediateOwner != Owner)
	{
		AActor* TempOwner = Owner;
		Owner = IntermediateOwner.Get();
		SetOwner(TempOwner);
	}
	IntermediateOwner = nullptr;

	// Check if this Actor needs to be re-instanced
	UClass* OldClass = GetClass();
	if (OldClass->HasAnyClassFlags(CLASS_NewerVersionExists))
	{
		UClass* NewClass = OldClass->GetAuthoritativeClass();
		if (!ensure(NewClass != OldClass))
		{
			UE_LOG(LogActor, Warning, TEXT("WARNING: %s is out of date and is the same as its AuthoritativeClass during PostEditUndo!"), *OldClass->GetName());
		};

		// Early exit, letting anything more occur would be invalid due to the REINST_ class
		return false;
	}

	// Notify LevelBounds actor that level bounding box might be changed
	if (!IsTemplate())
	{
		if (ULevel* Level = GetLevel())
		{
			Level->MarkLevelBoundsDirty();
		}
	}

	// Restore OwnedComponents array
	if (IsValid(this))
	{
		ResetOwnedComponents();

		// BP created components are not serialized, so this should be cleared and will be filled in as the construction scripts are run
		BlueprintCreatedComponents.Reset();

		// notify navigation system
		FNavigationSystem::UpdateActorAndComponentData(*this);
	}
	else
	{
		FNavigationSystem::RemoveActorData(*this);
	}

	// This is a normal undo, so call super
	return true;
}

void AActor::PostTransacted(const FTransactionObjectEvent& TransactionEvent)
{
	Super::PostTransacted(TransactionEvent);
	if (TransactionEvent.HasOuterChange())
	{
		GEngine->BroadcastLevelActorOuterChanged(this, StaticFindObject(ULevel::StaticClass(), nullptr, *TransactionEvent.GetOriginalObjectOuterPathName().ToString()));
	}
}

void AActor::PostEditUndo()
{
	if (InternalPostEditUndo())
	{
		Super::PostEditUndo();
	}

	// Do not immediately update all primitive scene infos for brush actor
	// undo/redo transactions since they require the render thread to wait until
	// after the transactions are processed to guarantee that the model data
	// is safe to access.
	UWorld* World = GetWorld();
	if (World && World->Scene && !FActorEditorUtils::IsABrush(this))
	{
		ENQUEUE_RENDER_COMMAND(UpdateAllPrimitiveSceneInfosCmd)([Scene = World->Scene](FRHICommandListImmediate& RHICmdList) {
			Scene->UpdateAllPrimitiveSceneInfos(RHICmdList);
		});
	}
}

void AActor::PostEditUndo(TSharedPtr<ITransactionObjectAnnotation> TransactionAnnotation)
{
	CurrentTransactionAnnotation = StaticCastSharedPtr<FActorTransactionAnnotation>(TransactionAnnotation);

	if (InternalPostEditUndo())
	{
		Super::PostEditUndo(TransactionAnnotation);
	}
}

// @todo: Remove this hack once we have decided on the scaling method to use.
bool AActor::bUsePercentageBasedScaling = false;

void AActor::EditorApplyTranslation(const FVector& DeltaTranslation, bool bAltDown, bool bShiftDown, bool bCtrlDown)
{
	if( RootComponent != NULL )
	{
		FTransform NewTransform = GetRootComponent()->GetComponentTransform();
		NewTransform.SetTranslation(NewTransform.GetTranslation() + DeltaTranslation);
		GetRootComponent()->SetWorldTransform(NewTransform);
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("WARNING: EditorApplyTranslation %s has no root component"), *GetName() );
	}
}

void AActor::EditorApplyRotation(const FRotator& DeltaRotation, bool bAltDown, bool bShiftDown, bool bCtrlDown)
{
	if( RootComponent != NULL )
	{
		FRotator Rot = RootComponent->GetAttachParent() != NULL ? GetActorRotation() : RootComponent->GetRelativeRotation();
		FRotator ActorRotWind, ActorRotRem;
		Rot.GetWindingAndRemainder(ActorRotWind, ActorRotRem);
		const FQuat ActorQ = ActorRotRem.Quaternion();
		const FQuat DeltaQ = DeltaRotation.Quaternion();

		FRotator NewActorRotRem;
		if(RootComponent->GetAttachParent() != NULL )
		{
			//first we get the new rotation in relative space.
			const FQuat ResultQ = DeltaQ * ActorQ;
			NewActorRotRem = FRotator(ResultQ);
			FRotator DeltaRot = NewActorRotRem - ActorRotRem;
			FRotator NewRotation = Rot + DeltaRot;
			FQuat NewRelRotation = NewRotation.Quaternion();
			NewRelRotation = RootComponent->GetRelativeRotationFromWorld(NewRelRotation);
			NewActorRotRem = FRotator(NewRelRotation);
			//now we need to get current relative rotation to find the diff
			Rot = RootComponent->GetRelativeRotation();
			Rot.GetWindingAndRemainder(ActorRotWind, ActorRotRem);
		}
		else
		{
			const FQuat ResultQ = DeltaQ * ActorQ;
			NewActorRotRem = FRotator(ResultQ);
		}

		ActorRotRem.SetClosestToMe(NewActorRotRem);
		FRotator DeltaRot = NewActorRotRem - ActorRotRem;
		DeltaRot.Normalize();
		RootComponent->SetRelativeRotationExact( Rot + DeltaRot );
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("WARNING: EditorApplyRotation %s has no root component"), *GetName() );
	}
}


void AActor::EditorApplyScale( const FVector& DeltaScale, const FVector* PivotLocation, bool bAltDown, bool bShiftDown, bool bCtrlDown )
{
	if( RootComponent != NULL )
	{
		const FVector CurrentScale = GetRootComponent()->GetRelativeScale3D();

		// @todo: Remove this hack once we have decided on the scaling method to use.
		FVector ScaleToApply;

		if( AActor::bUsePercentageBasedScaling )
		{
			ScaleToApply = CurrentScale * (FVector(1.0f) + DeltaScale);
		}
		else
		{
			ScaleToApply = CurrentScale + DeltaScale;
		}

		GetRootComponent()->SetRelativeScale3D(ScaleToApply);

		if (PivotLocation)
		{
			const FVector CurrentScaleSafe(CurrentScale.X ? CurrentScale.X : 1.0f,
										   CurrentScale.Y ? CurrentScale.Y : 1.0f,
										   CurrentScale.Z ? CurrentScale.Z : 1.0f);

			const FRotator ActorRotation = GetActorRotation();
			const FVector WorldDelta = GetActorLocation() - (*PivotLocation);
			const FVector LocalDelta = (ActorRotation.GetInverse()).RotateVector(WorldDelta);
			const FVector LocalScaledDelta = LocalDelta * (ScaleToApply / CurrentScaleSafe);
			const FVector WorldScaledDelta = ActorRotation.RotateVector(LocalScaledDelta);

			GetRootComponent()->SetWorldLocation(WorldScaledDelta + (*PivotLocation));
		}
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("WARNING: EditorApplyTranslation %s has no root component"), *GetName() );
	}

	FEditorSupportDelegates::UpdateUI.Broadcast();
}


void AActor::EditorApplyMirror(const FVector& MirrorScale, const FVector& PivotLocation)
{
	const FRotationMatrix TempRot( GetActorRotation() );
	const FVector New0( TempRot.GetScaledAxis( EAxis::X ) * MirrorScale );
	const FVector New1( TempRot.GetScaledAxis( EAxis::Y ) * MirrorScale );
	const FVector New2( TempRot.GetScaledAxis( EAxis::Z ) * MirrorScale );
	// Revert the handedness of the rotation, but make up for it in the scaling.
	// Arbitrarily choose the X axis to remain fixed.
	const FMatrix NewRot( -New0, New1, New2, FVector::ZeroVector );

	if( RootComponent != NULL )
	{
		GetRootComponent()->SetRelativeRotationExact( NewRot.Rotator() );
		FVector Loc = GetActorLocation();
		Loc -= PivotLocation;
		Loc *= MirrorScale;
		Loc += PivotLocation;
		GetRootComponent()->SetRelativeLocation( Loc );

		FVector Scale3D = GetRootComponent()->GetRelativeScale3D();
		Scale3D.X = -Scale3D.X;
		GetRootComponent()->SetRelativeScale3D(Scale3D);
	}
	else
	{
		UE_LOG(LogActor, Warning, TEXT("WARNING: EditorApplyMirror %s has no root component"), *GetName() );
	}
}

void AActor::EditorGetUnderlyingActors(TSet<AActor*>& OutUnderlyingActors) const
{
	TInlineComponentArray<UChildActorComponent*> ChildActorComponents;
	GetComponents(ChildActorComponents);

	OutUnderlyingActors.Reserve(OutUnderlyingActors.Num() + ChildActorComponents.Num());
	
	for (UChildActorComponent* ChildActorComponent : ChildActorComponents)
	{
		if (AActor* ChildActor = ChildActorComponent->GetChildActor())
		{
			bool bAlreadySet = false;
			OutUnderlyingActors.Add(ChildActor, &bAlreadySet);
			if (!bAlreadySet)
			{
				ChildActor->EditorGetUnderlyingActors(OutUnderlyingActors);
			}
		}
	}
}

bool AActor::IsHiddenEd() const
{
	// If any of the standard hide flags are set, return true
	if( bHiddenEdLayer || !bEditable || ( GIsEditor && ( IsTemporarilyHiddenInEditor() || bHiddenEdLevel ) ) )
	{
		return true;
	}
	// Otherwise, it's visible
	return false;
}

void AActor::SetIsTemporarilyHiddenInEditor( bool bIsHidden )
{
	if( bHiddenEdTemporary != bIsHidden )
	{
		bHiddenEdTemporary = bIsHidden;
		MarkComponentsRenderStateDirty();
	}
}

bool AActor::SetIsHiddenEdLayer(bool bIsHiddenEdLayer)
{
	if (bHiddenEdLayer != bIsHiddenEdLayer)
	{
		bHiddenEdLayer = bIsHiddenEdLayer;
		MarkComponentsRenderStateDirty();
		return true;
	}
	return false;
}

bool AActor::SupportsLayers() const
{
	const bool bIsHidden = (GetClass()->GetDefaultObject<AActor>()->bHiddenEd == true);
	const bool bIsInEditorWorld = (GetWorld()->WorldType == EWorldType::Editor);
	const bool bIsPartitionedActor = GetLevel()->bIsPartitioned;
	const bool bIsValid = !bIsHidden && bIsInEditorWorld && !bIsPartitionedActor;

	if (bIsValid)
	{
		// Actors part of Level Instance are not valid for layers
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			if (ALevelInstance* LevelInstance = LevelInstanceSubsystem->GetParentLevelInstance(this))
			{
				return false;
			}
		}
	}

	return bIsValid;
}

bool AActor::IsEditable() const
{
	return bEditable;
}

bool AActor::IsSelectable() const
{
	return true;
}

bool AActor::IsListedInSceneOutliner() const
{
	return bListedInSceneOutliner;
}

bool AActor::EditorCanAttachTo(const AActor* InParent, FText& OutReason) const
{
	return true;
}

AActor* AActor::GetSceneOutlinerParent() const
{
	return GetAttachParentActor();
}

class UHLODLayer* AActor::GetHLODLayer() const
{
	return HLODLayer;
}

void AActor::SetHLODLayer(class UHLODLayer* InHLODLayer)
{
	HLODLayer = InHLODLayer;
}

void AActor::SetPackageExternal(bool bExternal, bool bShouldDirty)
{
	// @todo_ow: Call FExternalPackageHelper::SetPackagingMode and keep calling the actor specific code here (components). 
	//           The only missing part is GetExternalObjectsPath defaulting to a different folder than the one used by external actors.
	if (bExternal == IsPackageExternal())
	{
		return;
	}

    // Mark the current actor & package as dirty
	Modify(bShouldDirty);

	UPackage* LevelPackage = GetLevel()->GetPackage(); 
	if (bExternal)
	{
		UPackage* NewActorPackage = ULevel::CreateActorPackage(LevelPackage, GetLevel()->GetActorPackagingScheme(), GetPathName());
		SetExternalPackage(NewActorPackage);
	}
	else 
	{
		UPackage* ActorPackage = GetExternalPackage();
		// Detach the linker exports so it doesn't resolve to this actor anymore
		ResetLinkerExports(ActorPackage);
		SetExternalPackage(nullptr);
	}

	for (UActorComponent* ActorComponent : GetComponents())
	{
		if (ActorComponent && ActorComponent->IsRegistered())
		{
			ActorComponent->SetPackageExternal(bExternal, bShouldDirty);
		}
	}

	OnPackagingModeChanged.Broadcast(this, bExternal);
	
	// Mark the new actor package dirty
	MarkPackageDirty();
}

void AActor::OnPlayFromHere()
{
	check(bCanPlayFromHere);
}

TUniquePtr<FWorldPartitionActorDesc> AActor::CreateClassActorDesc() const
{
	return TUniquePtr<FWorldPartitionActorDesc>(new FWorldPartitionActorDesc());
}

TUniquePtr<FWorldPartitionActorDesc> AActor::CreateActorDesc() const
{
	check(!HasAnyFlags(RF_ArchetypeObject | RF_ClassDefaultObject));
	
	TUniquePtr<FWorldPartitionActorDesc> ActorDesc(CreateClassActorDesc());
		
	ActorDesc->Init(this);
	
	return ActorDesc;
}

TUniquePtr<class FWorldPartitionActorDesc> AActor::StaticCreateClassActorDesc(const TSubclassOf<AActor>& ActorClass)
{
	return CastChecked<AActor>(ActorClass->GetDefaultObject())->CreateClassActorDesc();
}

FString AActor::GetDefaultActorLabel() const
{
	UClass* ActorClass = GetClass();

	FString DefaultActorLabel = ActorClass->GetName();

	// Strip off the ugly "_C" suffix for Blueprint class actor instances
	if (Cast<UBlueprint>(ActorClass->ClassGeneratedBy))
	{
		DefaultActorLabel.RemoveFromEnd(TEXT("_C"), ESearchCase::CaseSensitive);
	}

	return DefaultActorLabel;
}

const FString& AActor::GetActorLabel(bool bCreateIfNone) const
{
	// If the label string is empty then we'll use the default actor label (usually the actor's class name.)
	// We actually cache the default name into our ActorLabel property.  This will be saved out with the
	// actor if the actor gets saved.  The reasons we like caching the name here is:
	//
	//		a) We can return it by const&	(performance)
	//		b) Calling GetDefaultActorLabel() is slow because of FName stuff  (performance)
	//		c) If needed, we could always empty the ActorLabel string if it matched the default
	//
	// Remember, ActorLabel is currently an editor-only property.

	if( ActorLabel.IsEmpty() && bCreateIfNone )
	{
		FString DefaultActorLabel = GetDefaultActorLabel();

		// We want the actor's label to be initially unique, if possible, so we'll use the number of the
		// actor's FName when creating the initially.  It doesn't actually *need* to be unique, this is just
		// an easy way to tell actors apart when observing them in a list.  The user can always go and rename
		// these labels such that they're no longer unique.
		if (!FActorSpawnUtils::IsGloballyUniqueName(GetFName()))
		{
			// Don't bother adding a suffix for number '0'
			const int32 NameNumber = NAME_INTERNAL_TO_EXTERNAL( GetFName().GetNumber() );
			if( NameNumber != 0 )
			{
				DefaultActorLabel.AppendInt(NameNumber);
			}
		}

		// Remember, there could already be an actor with the same label in the level.  But that's OK, because
		// actor labels aren't supposed to be unique.  We just try to make them unique initially to help
		// disambiguate when opening up a new level and there are hundreds of actors of the same type.
		ActorLabel = MoveTemp(DefaultActorLabel);
	}

	return ActorLabel;
}

void AActor::SetActorLabel(const FString& NewActorLabelDirty, bool bMarkDirty)
{
	// Clean up the incoming string a bit
	FString NewActorLabel = NewActorLabelDirty.TrimStartAndEnd();

	// Validate incoming string before proceeding
	FText OutErrorMessage;
	if (!FActorEditorUtils::ValidateActorName(FText::FromString(NewActorLabel), OutErrorMessage))
	{
		//Invalid actor name
		UE_LOG(LogActor, Warning, TEXT("SetActorLabel failed: %s"), *OutErrorMessage.ToString());
	}
	else
	{
		// First, update the actor label
		{
			// Has anything changed?
			if (FCString::Strcmp(*NewActorLabel, *GetActorLabel()) != 0)
			{
				// Store new label
				Modify(bMarkDirty);
				ActorLabel = MoveTemp(NewActorLabel);
			}
		}
	}

	FPropertyChangedEvent PropertyEvent( FindFProperty<FProperty>( AActor::StaticClass(), "ActorLabel" ) );
	PostEditChangeProperty(PropertyEvent);

	FCoreDelegates::OnActorLabelChanged.Broadcast(this);
}

bool AActor::IsActorLabelEditable() const
{
	return bActorLabelEditable && !FActorEditorUtils::IsABuilderBrush(this);
}

void AActor::ClearActorLabel()
{
	ActorLabel.Reset();
	FCoreDelegates::OnActorLabelChanged.Broadcast(this);
}

FFolder AActor::GetFolder() const
{
	return FFolder(GetFolderPath(), GetFolderRootObject());
}

FFolder::FRootObject AActor::GetFolderRootObject() const
{
	return FFolder::GetOptionalFolderRootObject(GetLevel()).Get(FFolder::GetDefaultRootObject());
}

static bool IsUsingActorFolders(const AActor* InActor)
{
	return InActor && InActor->GetLevel() && InActor->GetLevel()->IsUsingActorFolders();
}

bool AActor::IsActorFolderValid() const
{
	return !IsUsingActorFolders(this) || (FolderPath.IsNone() && !FolderGuid.IsValid()) || GetActorFolder();
}

bool AActor::CreateOrUpdateActorFolder()
{
	check(GetLevel());
	check(IsUsingActorFolders(this));

	// First time this function is called, FolderPath can be valid and FolderGuid is invalid.
	if (FolderPath.IsNone() && !FolderGuid.IsValid())
	{
		// Nothing to do
		return true;
	}

	// Remap deleted folder or fixup invalid guid
	UActorFolder* ActorFolder = nullptr;
	if (FolderGuid.IsValid())
	{
		check(FolderPath.IsNone());
		ActorFolder = GetActorFolder(/*bSkipDeleted*/false);
		if (!ActorFolder || ActorFolder->IsMarkedAsDeleted())
		{
			FixupActorFolder();
			check(IsActorFolderValid());
			return true;
		}
	}

	// If not found, create actor folder using FolderPath
	if (!ActorFolder)
	{
		check(!FolderPath.IsNone());
		ActorFolder = FWorldPersistentFolders::GetActorFolder(FFolder(FolderPath, GetFolderRootObject()), GetWorld(), /*bAllowCreate*/ true);
	}

	// At this point, actor folder should always be valid
	if (ensure(ActorFolder))
	{
		SetFolderGuidInternal(ActorFolder ? ActorFolder->GetGuid() : FGuid());

		// Make sure actor folder is in the correct packaging mode
		ActorFolder->SetPackageExternal(GetLevel()->IsUsingExternalObjects());
	}
	return IsActorFolderValid();
}

UActorFolder* AActor::GetActorFolder(bool bSkipDeleted) const
{
	UActorFolder* ActorFolder = nullptr;
	if (ULevel* Level = GetLevel())
	{
		if (FolderGuid.IsValid())
		{
			ActorFolder = Level->GetActorFolder(FolderGuid, bSkipDeleted);
		}
		else if (!FolderPath.IsNone())
		{
			ActorFolder = Level->GetActorFolder(FolderPath, bSkipDeleted);
		}
	}
	return ActorFolder;
}

void AActor::FixupActorFolder()
{
	check(GetLevel());

	if (!IsUsingActorFolders(this))
	{
		if (FolderGuid.IsValid())
		{
			UE_LOG(LogLevel, Warning, TEXT("Actor folder %s for actor %s encountered when not using actor folders"), *FolderGuid.ToString(), *GetName());
			FolderGuid = FGuid();
		}
	}
	else
	{
		// First detect and fixup reference to deleted actor folders
		UActorFolder* ActorFolder = GetActorFolder(/*bSkipDeleted*/ false);
		if (ActorFolder)
		{
			// Remap to skip deleted actor folder
			if (ActorFolder->IsMarkedAsDeleted())
			{
				ActorFolder = ActorFolder->GetParent();
				SetFolderGuidInternal(ActorFolder ? ActorFolder->GetGuid() : FGuid(), /*bBroadcastChange*/ false);
			}
			// We found actor folder using its path, update actor folder guid
			else if (!FolderPath.IsNone())
			{
				SetFolderGuidInternal(ActorFolder ? ActorFolder->GetGuid() : FGuid(), /*bBroadcastChange*/ false);
			}
		}

		// If still invalid, warn and fallback to root
		if (!IsActorFolderValid())
		{
			UE_LOG(LogLevel, Warning, TEXT("Missing actor folder for actor %s"), *GetName());
			SetFolderGuidInternal(FGuid(), /*bBroadcastChange*/ false);
		}

		if (!FolderPath.IsNone())
		{
			UE_LOG(LogLevel, Warning, TEXT("Actor folder path %s for actor %s encountered when using actor folders"), *FolderPath.ToString(), *GetName());
			FolderPath = NAME_None;
		}
	}
}

FGuid AActor::GetFolderGuid() const
{
	return IsUsingActorFolders(this) ? FolderGuid : FGuid();
}

FName AActor::GetFolderPath() const
{
	static const FName RootPath = FFolder::GetEmptyPath();
	if (!FFolder::GetOptionalFolderRootObject(GetLevel()))
	{
		return RootPath;
	}
	if (IsUsingActorFolders(this))
	{
		if (UActorFolder* ActorFolder = GetActorFolder())
		{
			return ActorFolder->GetPath();
		}
		return RootPath;
	}
	return FolderPath;
}

void AActor::SetFolderPath(const FName& InNewFolderPath)
{
	if (IsUsingActorFolders(this))
	{
		UActorFolder* ActorFolder = nullptr;
		UWorld* World = GetWorld();
		if (!InNewFolderPath.IsNone() && World)
		{
			FFolder NewFolder(InNewFolderPath, GetFolderRootObject());
			ActorFolder = FWorldPersistentFolders::GetActorFolder(NewFolder, World);
			if (!ActorFolder)
			{
				ActorFolder = FWorldPersistentFolders::GetActorFolder(NewFolder, World, /*bAllowCreate*/ true);
			}
		}
		SetFolderGuidInternal(ActorFolder ? ActorFolder->GetGuid() : FGuid());
	}
	else
	{
		SetFolderPathInternal(InNewFolderPath);
	}
}

void AActor::SetFolderGuidInternal(const FGuid& InFolderGuid, bool bInBroadcastChange)
{
	if ((FolderGuid == InFolderGuid) && FolderPath.IsNone())
	{
		return;
	}

	FName OldPath = !FolderPath.IsNone() ? FolderPath : GetFolderPath();
	
	Modify();
	FolderPath = NAME_None;
	FolderGuid = InFolderGuid;

	if (GEngine && bInBroadcastChange)
	{
		GEngine->BroadcastLevelActorFolderChanged(this, OldPath);
	}
}

void AActor::SetFolderPathInternal(const FName& InNewFolderPath, bool bInBroadcastChange)
{
	FName OldPath = FolderPath;
	if (InNewFolderPath.IsEqual(OldPath, ENameCase::CaseSensitive))
	{
		return;
	}

	Modify();
	FolderPath = InNewFolderPath;
	FolderGuid.Invalidate();

	if (GEngine && bInBroadcastChange)
	{
		GEngine->BroadcastLevelActorFolderChanged(this, OldPath);
	}
}

void AActor::SetFolderPath_Recursively(const FName& NewFolderPath)
{
	FActorEditorUtils::TraverseActorTree_ParentFirst(this, [&](AActor* InActor){
		InActor->SetFolderPath(NewFolderPath);
		return true;
	});
}

void AActor::CheckForDeprecated()
{
	if ( GetClass()->HasAnyClassFlags(CLASS_Deprecated) )
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("ActorName"), FText::FromString(GetPathName()));
		FMessageLog("MapCheck").Warning()
			->AddToken(FUObjectToken::Create(this))
			->AddToken(FTextToken::Create(FText::Format(LOCTEXT( "MapCheck_Message_ActorIsObselete_Deprecated", "{ActorName} : Obsolete and must be removed! (Class is deprecated)" ), Arguments) ))
			->AddToken(FMapErrorToken::Create(FMapErrors::ActorIsObselete));
	}
	// don't check to see if this is an abstract class if this is the CDO
	if ( !(GetFlags() & RF_ClassDefaultObject) && GetClass()->HasAnyClassFlags(CLASS_Abstract) )
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("ActorName"), FText::FromString(GetPathName()));
		FMessageLog("MapCheck").Warning()
			->AddToken(FUObjectToken::Create(this))
			->AddToken(FTextToken::Create(FText::Format(LOCTEXT( "MapCheck_Message_ActorIsObselete_Abstract", "{ActorName} : Obsolete and must be removed! (Class is abstract)" ), Arguments) ) )
			->AddToken(FMapErrorToken::Create(FMapErrors::ActorIsObselete));
	}
}

void AActor::CheckForErrors()
{
	int32 OldNumWarnings = FMessageLog("MapCheck").NumMessages(EMessageSeverity::Warning);
	CheckForDeprecated();
	if (OldNumWarnings < FMessageLog("MapCheck").NumMessages(EMessageSeverity::Warning))
	{
		return;
	}

	UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(RootComponent);
	if (PrimComp && (PrimComp->Mobility != EComponentMobility::Movable) && PrimComp->BodyInstance.bSimulatePhysics)
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("ActorName"), FText::FromString(GetPathName()));
		FMessageLog("MapCheck").Warning()
			->AddToken(FUObjectToken::Create(this))
			->AddToken(FTextToken::Create(FText::Format(LOCTEXT( "MapCheck_Message_StaticPhysNone", "{ActorName} : Static object with bSimulatePhysics set to true" ), Arguments) ))
			->AddToken(FMapErrorToken::Create(FMapErrors::StaticPhysNone));
	}

	if (RootComponent)
	{
		const FVector LocalRelativeScale3D = RootComponent->GetRelativeScale3D();
		if (FMath::IsNearlyZero(LocalRelativeScale3D.X * LocalRelativeScale3D.Y * LocalRelativeScale3D.Z))
		{
			FFormatNamedArguments Arguments;
			Arguments.Add(TEXT("ActorName"), FText::FromString(GetPathName()));
			FMessageLog("MapCheck").Error()
				->AddToken(FUObjectToken::Create(this))
				->AddToken(FTextToken::Create(FText::Format(LOCTEXT("MapCheck_Message_InvalidDrawscale", "{ActorName} : Invalid DrawScale/DrawScale3D"), Arguments)))
				->AddToken(FMapErrorToken::Create(FMapErrors::InvalidDrawscale));
		}
	}

	// Route error checking to components.
	for (UActorComponent* ActorComponent : GetComponents())
	{
		if (ActorComponent && ActorComponent->IsRegistered())
		{
			ActorComponent->CheckForErrors();
		}
	}
}

bool AActor::GetReferencedContentObjects( TArray<UObject*>& Objects ) const
{
	UBlueprint* Blueprint = UBlueprint::GetBlueprintFromClass( GetClass() );
	if (Blueprint)
	{
		Objects.AddUnique(Blueprint);
	}
	return true;
}

EDataValidationResult AActor::IsDataValid(TArray<FText>& ValidationErrors)
{
	// Do not run asset validation on external actors, validation will be caught through map check
	if (IsPackageExternal())
	{
		return EDataValidationResult::NotValidated;
	}

	bool bSuccess = CheckDefaultSubobjects();
	if (!bSuccess)
	{
		FText ErrorMsg = FText::Format(LOCTEXT("IsDataValid_Failed_CheckDefaultSubobjectsInternal", "{0} failed CheckDefaultSubobjectsInternal()"), FText::FromString(GetName()));
		ValidationErrors.Add(ErrorMsg);
	}

	int32 OldNumMapWarningsAndErrors = FMessageLog("MapCheck").NumMessages(EMessageSeverity::Warning);
	CheckForErrors();
	int32 NewNumMapWarningsAndErrors = FMessageLog("MapCheck").NumMessages(EMessageSeverity::Warning);
	if (NewNumMapWarningsAndErrors != OldNumMapWarningsAndErrors)
	{
		FFormatNamedArguments Arguments;
		Arguments.Add(TEXT("ActorName"), FText::FromString(GetName()));
		FText ErrorMsg = FText::Format(LOCTEXT("IsDataValid_Failed_CheckForErrors", "{ActorName} is not valid. See the MapCheck log messages for details."), Arguments);
		ValidationErrors.Add(ErrorMsg);
		bSuccess = false;
	}

	EDataValidationResult Result = bSuccess ? EDataValidationResult::Valid : EDataValidationResult::Invalid;

	// check the components
	for (UActorComponent* Component : GetComponents())
	{
		if (Component)
		{
			// if any component is invalid, our result is invalid
			// in the future we may want to update this to say that the actor was not validated if any of its components returns EDataValidationResult::NotValidated
			EDataValidationResult ComponentResult = Component->IsDataValid(ValidationErrors);
			if (ComponentResult == EDataValidationResult::Invalid)
			{
				Result = EDataValidationResult::Invalid;
			}
		}
	}

	return Result;
}

//---------------------------------------------------------------------------
// DataLayers (begin)

bool AActor::AddDataLayer(const UDataLayer* DataLayer)
{
	bool bActorWasModified = false;
	if (SupportsDataLayer() && DataLayer && !ContainsDataLayer(DataLayer))
	{
		if (!bActorWasModified)
		{
			Modify();
			bActorWasModified = true;
		}

		DataLayers.Emplace(DataLayer->GetFName());
	}
	return bActorWasModified;
}

bool AActor::RemoveDataLayer(const UDataLayer* DataLayer)
{
	bool bActorWasModified = false;
	if (ContainsDataLayer(DataLayer))
	{
		if (!bActorWasModified)
		{
			Modify();
			bActorWasModified = true;
		}

		DataLayers.Remove(FActorDataLayer(DataLayer->GetFName()));
	}
	return bActorWasModified;
}

bool AActor::RemoveAllDataLayers()
{
	if (HasDataLayers())
	{
		Modify();
		DataLayers.Empty();
		return true;
	}
	return false;
}

bool AActor::ContainsDataLayer(const UDataLayer* DataLayer) const
{
	return DataLayer && DataLayers.Contains(FActorDataLayer(DataLayer->GetFName()));
}

bool AActor::HasDataLayers() const
{
	return DataLayers.Num() > 0;
}

bool AActor::HasValidDataLayers() const
{
	if (const AWorldDataLayers* WorldDataLayers = GetWorld()->GetWorldDataLayers())
	{
		for (const FActorDataLayer& DataLayer : DataLayers)
		{
			if (const UDataLayer* DataLayerObject = WorldDataLayers->GetDataLayerFromName(DataLayer.Name))
			{
				return true;
			}
		}
	}
	return false;
}

bool AActor::HasAllDataLayers(const TArray<const UDataLayer*>& InDataLayers) const
{
	if (DataLayers.Num() < InDataLayers.Num())
	{
		return false;
	}

	for (const UDataLayer* DataLayer : InDataLayers)
	{
		if (!ContainsDataLayer(DataLayer))
		{
			return false;
		}
	}
	return true;
}

TArray<FName> AActor::GetDataLayerNames() const
{
	const AWorldDataLayers* WorldDataLayers = GetWorld()->GetWorldDataLayers();
	return WorldDataLayers ? WorldDataLayers->GetDataLayerNames(DataLayers) : TArray<FName>();
}

TArray<const UDataLayer*> AActor::GetDataLayerObjects() const
{
	return GetWorld() ? GetDataLayerObjects(GetWorld()->GetWorldDataLayers()) : TArray<const UDataLayer*>();
}

TArray<const UDataLayer*> AActor::GetDataLayerObjects(const AWorldDataLayers* WorldDataLayers) const
{
	return WorldDataLayers ? WorldDataLayers->GetDataLayerObjects(DataLayers) : TArray<const UDataLayer*>();
}

bool AActor::HasAnyOfDataLayers(const TArray<FName>& DataLayerNames) const
{
	for (const FActorDataLayer& DataLayer : DataLayers)
	{
		if (DataLayerNames.Contains(DataLayer.Name))
		{
			return true;
		}
	}
	return false;
}

void AActor::FixupDataLayers(bool bRevertChangesOnLockedDataLayer /*= false*/)
{
	if (!GetPackage()->HasAnyPackageFlags(PKG_PlayInEditor))
	{
		if (!SupportsDataLayer())
		{
			DataLayers.Empty();
			return;
		}

		if (GetWorld())
		{
			if (const AWorldDataLayers* WorldDataLayers = GetWorld()->GetWorldDataLayers())
			{
				if (bRevertChangesOnLockedDataLayer)
				{
					// Since it's not possible to prevent changes of particular elements of an array, rollback change on locked DataLayers.
					TSet<FActorDataLayer> PreEdit(PreEditChangeDataLayers);
					TSet<FActorDataLayer> PostEdit(DataLayers);

					auto DifferenceContainsLockedDataLayers = [WorldDataLayers](const TSet<FActorDataLayer>& A, const TSet<FActorDataLayer>& B)
					{
						TSet<FActorDataLayer> Diff = A.Difference(B);
						for (const FActorDataLayer& ActorDataLayer : Diff)
						{
							const UDataLayer* DataLayer = WorldDataLayers->GetDataLayerFromName(ActorDataLayer);
							if (DataLayer && DataLayer->IsLocked())
							{
								return true;
							}
						}
						return false;
					};
					
					if (DifferenceContainsLockedDataLayers(PreEdit, PostEdit) || 
						DifferenceContainsLockedDataLayers(PostEdit, PreEdit))
					{
						DataLayers = PreEditChangeDataLayers;
					}
				}

				TSet<FName> ExistingDataLayers;
				for (int32 Index = 0; Index < DataLayers.Num();)
				{
					const FName& DataLayer = DataLayers[Index].Name;
					if (!WorldDataLayers->GetDataLayerFromName(DataLayer) || ExistingDataLayers.Contains(DataLayer))
					{
						DataLayers.RemoveAtSwap(Index);
					}
					else
					{
						ExistingDataLayers.Add(DataLayer);
						++Index;
					}
				}
			}
		}
	}
}

bool AActor::IsPropertyChangedAffectingDataLayers(FPropertyChangedEvent& PropertyChangedEvent) const
{
	if (PropertyChangedEvent.Property != nullptr)
	{
		FProperty* MemberPropertyThatChanged = PropertyChangedEvent.MemberProperty;
		const FName MemberPropertyName = MemberPropertyThatChanged != NULL ? MemberPropertyThatChanged->GetFName() : NAME_None;

		static const FName NAME_DataLayers = GET_MEMBER_NAME_CHECKED(AActor, DataLayers);
		static const FName NAME_FActorDataLayerName = GET_MEMBER_NAME_CHECKED(FActorDataLayer, Name);

		if (MemberPropertyName == NAME_DataLayers &&
			PropertyChangedEvent.ChangeType == EPropertyChangeType::ValueSet &&
			PropertyChangedEvent.Property->GetFName() == NAME_FActorDataLayerName)
		{
			return true;
		}
		else
		{
			const FName PropertyName = PropertyChangedEvent.GetPropertyName();
			if (PropertyName == NAME_DataLayers && 
				((PropertyChangedEvent.ChangeType == EPropertyChangeType::ValueSet) || 
				 (PropertyChangedEvent.ChangeType == EPropertyChangeType::ArrayClear) ||
				 (PropertyChangedEvent.ChangeType == EPropertyChangeType::Duplicate)))
			{
				return true;
			}
		}
	}
	return false;
}

bool AActor::IsValidForDataLayer() const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}

	const bool bIsPartitionedActor = UWorld::HasSubsystem<UWorldPartitionSubsystem>(World);
	const bool bIsInEditorWorld = World->WorldType == EWorldType::Editor;
	const bool bIsBuilderBrush = FActorEditorUtils::IsABuilderBrush(this);
	const bool bIsHidden = GetClass()->GetDefaultObject<AActor>()->bHiddenEd;
	const bool bIsValid = !bIsHidden && !bIsBuilderBrush && bIsInEditorWorld && bIsPartitionedActor;

	return bIsValid;
}

// DataLayers (end)
//---------------------------------------------------------------------------

#undef LOCTEXT_NAMESPACE

#endif // WITH_EDITOR
