// Copyright Epic Games, Inc. All Rights Reserved.

#include "MovieSceneTracksComponentTypes.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "EntitySystem/BuiltInComponentTypes.h"
#include "EntitySystem/MovieSceneEntityManager.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "EntitySystem/MovieSceneComponentRegistry.h"
#include "EntitySystem/MovieSceneBlenderSystem.h"
#include "Systems/MovieScenePiecewiseBoolBlenderSystem.h"
#include "Systems/MovieScenePiecewiseByteBlenderSystem.h"
#include "Systems/MovieScenePiecewiseEnumBlenderSystem.h"
#include "Systems/MovieScenePiecewiseIntegerBlenderSystem.h"
#include "Systems/MovieScenePiecewiseFloatBlenderSystem.h"
#include "Systems/MovieScenePiecewiseDoubleBlenderSystem.h"
#include "EntitySystem/MovieScenePropertyComponentHandler.h"
#include "EntitySystem/MovieSceneEntityFactoryTemplates.h"
#include "EntitySystem/MovieScenePropertyMetaDataTraits.inl"
#include "PreAnimatedState/MovieScenePreAnimatedComponentTransformStorage.h"
#include "Systems/MovieSceneColorPropertySystem.h"
#include "Systems/MovieSceneVectorPropertySystem.h"
#include "MovieSceneObjectBindingID.h"
#include "GameFramework/Actor.h"
#include "Misc/App.h"

namespace UE
{
namespace MovieScene
{

/* ---------------------------------------------------------------------------
 * Transform conversion functions
 * ---------------------------------------------------------------------------*/
void ConvertOperationalProperty(const FIntermediate3DTransform& In, FEulerTransform& Out)
{
	Out.Location = In.GetTranslation();
	Out.Rotation = In.GetRotation();
	Out.Scale = In.GetScale();
}
void ConvertOperationalProperty(const FEulerTransform& In, FIntermediate3DTransform& Out)
{
	Out = FIntermediate3DTransform(In.Location, In.Rotation, In.Scale);
}

void ConvertOperationalProperty(const FIntermediate3DTransform& In, FTransform& Out)
{
	Out = FTransform(In.GetRotation().Quaternion(), In.GetTranslation(), In.GetScale());
}
void ConvertOperationalProperty(const FTransform& In, FIntermediate3DTransform& Out)
{
	FVector Location = In.GetTranslation();
	FRotator Rotation = In.GetRotation().Rotator();
	FVector Scale = In.GetScale3D();

	Out = FIntermediate3DTransform(Location, Rotation, Scale);
}

/* ---------------------------------------------------------------------------
 * Color conversion functions
 * ---------------------------------------------------------------------------*/
void ConvertOperationalProperty(const FIntermediateColor& InColor, FColor& Out)
{
	Out = InColor.GetColor();
}

void ConvertOperationalProperty(const FIntermediateColor& InColor, FLinearColor& Out)
{
	Out = InColor.GetLinearColor();
}

void ConvertOperationalProperty(const FIntermediateColor& InColor, FSlateColor& Out)
{
	Out = InColor.GetSlateColor();
}

void ConvertOperationalProperty(const FColor& InColor, FIntermediateColor& OutIntermediate)
{
	OutIntermediate = FIntermediateColor(InColor);
}

void ConvertOperationalProperty(const FLinearColor& InColor, FIntermediateColor& OutIntermediate)
{
	OutIntermediate = FIntermediateColor(InColor);
}

void ConvertOperationalProperty(const FSlateColor& InColor, FIntermediateColor& OutIntermediate)
{
	OutIntermediate = FIntermediateColor(InColor);
}


/* ---------------------------------------------------------------------------
 * Vector conversion functions
 * ---------------------------------------------------------------------------*/
void ConvertOperationalProperty(const FFloatIntermediateVector& InVector, FVector2f& Out)
{
	Out = FVector2f(InVector.X, InVector.Y);
}

void ConvertOperationalProperty(const FFloatIntermediateVector& InVector, FVector3f& Out)
{
	Out = FVector3f(InVector.X, InVector.Y, InVector.Z);
}

void ConvertOperationalProperty(const FFloatIntermediateVector& InVector, FVector4f& Out)
{
	Out = FVector4f(InVector.X, InVector.Y, InVector.Z, InVector.W);
}

void ConvertOperationalProperty(const FVector2f& In, FFloatIntermediateVector& Out)
{
	Out = FFloatIntermediateVector(In.X, In.Y);
}

void ConvertOperationalProperty(const FVector3f& In, FFloatIntermediateVector& Out)
{
	Out = FFloatIntermediateVector(In.X, In.Y, In.Z);
}

void ConvertOperationalProperty(const FVector4f& In, FFloatIntermediateVector& Out)
{
	Out = FFloatIntermediateVector(In.X, In.Y, In.Z, In.W);
}

void ConvertOperationalProperty(const FDoubleIntermediateVector& InVector, FVector2d& Out)
{
	Out = FVector2d(InVector.X, InVector.Y);
}

void ConvertOperationalProperty(const FDoubleIntermediateVector& InVector, FVector3d& Out)
{
	Out = FVector3d(InVector.X, InVector.Y, InVector.Z);
}

void ConvertOperationalProperty(const FDoubleIntermediateVector& InVector, FVector4d& Out)
{
	Out = FVector4d(InVector.X, InVector.Y, InVector.Z, InVector.W);
}

void ConvertOperationalProperty(const FVector2d& In, FDoubleIntermediateVector& Out)
{
	Out = FDoubleIntermediateVector(In.X, In.Y);
}

void ConvertOperationalProperty(const FVector3d& In, FDoubleIntermediateVector& Out)
{
	Out = FDoubleIntermediateVector(In.X, In.Y, In.Z);
}

void ConvertOperationalProperty(const FVector4d& In, FDoubleIntermediateVector& Out)
{
	Out = FDoubleIntermediateVector(In.X, In.Y, In.Z, In.W);
}


FIntermediate3DTransform GetComponentTransform(const UObject* Object)
{
	const USceneComponent* SceneComponent = CastChecked<const USceneComponent>(Object);
	FIntermediate3DTransform Result(SceneComponent->GetRelativeLocation(), SceneComponent->GetRelativeRotation(), SceneComponent->GetRelativeScale3D());
	return Result;
}

void SetComponentTransform(USceneComponent* SceneComponent, const FIntermediate3DTransform& InTransform)
{
	// If this is a simulating component, teleport since sequencer takes over. 
	// Teleport will not have no velocity, but it's computed later by sequencer so that it will be correct for physics.
	// @todo: We would really rather not 
	AActor* Actor = SceneComponent->GetOwner();
	USceneComponent* RootComponent = Actor ? Actor->GetRootComponent() : nullptr;
	bool bIsSimulatingPhysics = RootComponent ? RootComponent->IsSimulatingPhysics() : false;

	FVector Translation = InTransform.GetTranslation();
	FRotator Rotation = InTransform.GetRotation();
	SceneComponent->SetRelativeLocationAndRotation(Translation, Rotation, false, nullptr, bIsSimulatingPhysics ? ETeleportType::ResetPhysics : ETeleportType::None);
	SceneComponent->SetRelativeScale3D(InTransform.GetScale());

	// Force the location and rotation values to avoid Rot->Quat->Rot conversions
	SceneComponent->SetRelativeLocation_Direct(Translation);
	SceneComponent->SetRelativeRotation_Direct(Rotation);
}

void SetComponentTransformAndVelocity(UObject* Object, const FIntermediate3DTransform& InTransform)
{
	InTransform.ApplyTo(CastChecked<USceneComponent>(Object));
}

FIntermediateColor GetLightComponentLightColor(const UObject* Object, EColorPropertyType InColorType)
{
	ensure(InColorType == EColorPropertyType::Color);

	const ULightComponent* LightComponent = CastChecked<const ULightComponent>(Object);
	return FIntermediateColor(LightComponent->GetLightColor());
}

void SetLightComponentLightColor(UObject* Object, EColorPropertyType InColorType, const FIntermediateColor& InColor)
{
	// This is a little esoteric - ULightComponentBase::LightColor is the UPROPERTY that generates the meta-data
	// for this custom callback, but it is an FColor, even though the public get/set functions expose it as an
	// FLinearColor. FIntermediateColor is always blended and dealt with in linear space, so it's fine to 
	// simply reinterpret the color
	ensure(InColorType == EColorPropertyType::Color);

	const bool bConvertBackToSRgb = true;
	ULightComponent* LightComponent = CastChecked<ULightComponent>(Object);
	LightComponent->SetLightColor(InColor.GetLinearColor(), bConvertBackToSRgb);
}

FIntermediateColor GetSkyLightComponentLightColor(const UObject* Object, EColorPropertyType InColorType)
{
	ensure(InColorType == EColorPropertyType::Color);

	const USkyLightComponent* SkyLightComponent = CastChecked<const USkyLightComponent>(Object);
	return FIntermediateColor(SkyLightComponent->GetLightColor());
}

void SetSkyLightComponentLightColor(UObject* Object, EColorPropertyType InColorType, const FIntermediateColor& InColor)
{
	// This is a little esoteric - ULightComponentBase::LightColor is the UPROPERTY that generates the meta-data
	// for this custom callback, but it is an FColor, even though the public get/set functions expose it as an
	// FLinearColor. FIntermediateColor is always blended and dealt with in linear space, so it's fine to 
	// simply reinterpret the color
	ensure(InColorType == EColorPropertyType::Color);

	USkyLightComponent* SkyLightComponent = CastChecked<USkyLightComponent>(Object);
	SkyLightComponent->SetLightColor(InColor.GetLinearColor());
}

float GetSecondFogDataFogDensity(const UObject* Object, bool bIsDouble)
{
	check(!bIsDouble);
	const UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<const UExponentialHeightFogComponent>(Object);
	return ExponentialHeightFogComponent->SecondFogData.FogDensity;
}

void SetSecondFogDataFogDensity(UObject* Object, bool bIsDouble, float InFogDensity)
{
	check(!bIsDouble);
	UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<UExponentialHeightFogComponent>(Object);
	ExponentialHeightFogComponent->SecondFogData.FogDensity = InFogDensity;
}

float GetSecondFogDataFogHeightFalloff(const UObject* Object, bool bIsDouble)
{
	check(!bIsDouble);
	const UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<const UExponentialHeightFogComponent>(Object);
	return ExponentialHeightFogComponent->SecondFogData.FogHeightFalloff;
}

void SetSecondFogDataFogHeightFalloff(UObject* Object, bool bIsDouble, float InFogHeightFalloff)
{
	check(!bIsDouble);
	UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<UExponentialHeightFogComponent>(Object);
	ExponentialHeightFogComponent->SecondFogData.FogHeightFalloff = InFogHeightFalloff;
}

float GetSecondFogDataFogHeightOffset(const UObject* Object, bool bIsDouble)
{
	check(!bIsDouble);
	const UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<const UExponentialHeightFogComponent>(Object);
	return ExponentialHeightFogComponent->SecondFogData.FogHeightOffset;
}

void SetSecondFogDataFogHeightOffset(UObject* Object, bool bIsDouble, float InFogHeightOffset)
{
	check(!bIsDouble);
	UExponentialHeightFogComponent* ExponentialHeightFogComponent = CastChecked<UExponentialHeightFogComponent>(Object);
	ExponentialHeightFogComponent->SecondFogData.FogHeightOffset = InFogHeightOffset;
}

void FIntermediate3DTransform::ApplyTo(USceneComponent* SceneComponent) const
{
	double DeltaTime = FApp::GetDeltaTime();
	if (DeltaTime <= 0)
	{
		SetComponentTransform(SceneComponent, *this);
	}
	else
	{
		/* Cache initial absolute position */
		FVector PreviousPosition = SceneComponent->GetComponentLocation();

		SetComponentTransform(SceneComponent, *this);

		/* Get current absolute position and set component velocity */
		FVector CurrentPosition = SceneComponent->GetComponentLocation();
		FVector ComponentVelocity = (CurrentPosition - PreviousPosition) / DeltaTime;
		SceneComponent->ComponentVelocity = ComponentVelocity;
	}
}

USceneComponent* FComponentAttachParamsDestination::ResolveAttachment(AActor* InParentActor) const
{
	if (SocketName != NAME_None)
	{
		if (ComponentName != NAME_None )
		{
			TInlineComponentArray<USceneComponent*> PotentialAttachComponents(InParentActor);
			for (USceneComponent* PotentialAttachComponent : PotentialAttachComponents)
			{
				if (PotentialAttachComponent->GetFName() == ComponentName && PotentialAttachComponent->DoesSocketExist(SocketName))
				{
					return PotentialAttachComponent;
				}
			}
		}
		else if (InParentActor->GetRootComponent()->DoesSocketExist(SocketName))
		{
			return InParentActor->GetRootComponent();
		}
	}
	else if (ComponentName != NAME_None )
	{
		TInlineComponentArray<USceneComponent*> PotentialAttachComponents(InParentActor);
		for (USceneComponent* PotentialAttachComponent : PotentialAttachComponents)
		{
			if (PotentialAttachComponent->GetFName() == ComponentName)
			{
				return PotentialAttachComponent;
			}
		}
	}

	if (InParentActor->GetDefaultAttachComponent())
	{
		return InParentActor->GetDefaultAttachComponent();
	}
	else
	{
		return InParentActor->GetRootComponent();
	}
}

void FComponentAttachParams::ApplyAttach(USceneComponent* ChildComponentToAttach, USceneComponent* NewAttachParent, const FName& SocketName) const
{
	if (ChildComponentToAttach->GetAttachParent() != NewAttachParent || ChildComponentToAttach->GetAttachSocketName() != SocketName)
	{
		FAttachmentTransformRules AttachmentRules(AttachmentLocationRule, AttachmentRotationRule, AttachmentScaleRule, false);

		ChildComponentToAttach->AttachToComponent(NewAttachParent, AttachmentRules, SocketName);
	}

	// Match the component velocity of the parent. If the attached child has any transformation, the velocity will be 
	// computed by the component transform system.
	if (ChildComponentToAttach->GetAttachParent())
	{
		ChildComponentToAttach->ComponentVelocity = ChildComponentToAttach->GetAttachParent()->GetComponentVelocity();
	}
}

void FComponentDetachParams::ApplyDetach(USceneComponent* ChildComponentToAttach, USceneComponent* NewAttachParent, const FName& SocketName) const
{
	// Detach if there was no pre-existing parent
	if (!NewAttachParent)
	{
		FDetachmentTransformRules DetachmentRules(DetachmentLocationRule, DetachmentRotationRule, DetachmentScaleRule, false);
		ChildComponentToAttach->DetachFromComponent(DetachmentRules);
	}
	else
	{
		ChildComponentToAttach->AttachToComponent(NewAttachParent, FAttachmentTransformRules::KeepRelativeTransform, SocketName);
	}
}


static bool GMovieSceneTracksComponentTypesDestroyed = false;
static TUniquePtr<FMovieSceneTracksComponentTypes> GMovieSceneTracksComponentTypes;

struct FFloatHandler : TPropertyComponentHandler<FFloatPropertyTraits, float>
{
	virtual void DispatchInitializePropertyMetaDataTasks(const FPropertyDefinition& Definition, FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents, UMovieSceneEntitySystemLinker* Linker) override
	{
		FBuiltInComponentTypes* BuiltInComponents = FBuiltInComponentTypes::Get();
		FMovieSceneTracksComponentTypes* TrackComponents = FMovieSceneTracksComponentTypes::Get();

		FEntityTaskBuilder()
		.Read(BuiltInComponents->BoundObject)
		.Read(BuiltInComponents->PropertyBinding)
		.Write(TrackComponents->Float.MetaDataComponents.GetType<0>())
		.FilterAll({ BuiltInComponents->Tags.NeedsLink })
		.Iterate_PerEntity(&Linker->EntityManager, [](UObject* Object, const FMovieScenePropertyBinding& Binding, bool& OutIsDouble)
		{
			FProperty* BoundProperty = FTrackInstancePropertyBindings::FindProperty(Object, Binding.PropertyPath.ToString());
			if (ensure(BoundProperty))
			{
				OutIsDouble = BoundProperty->IsA<FDoubleProperty>();
			}
			else
			{
				OutIsDouble = false;
			}
		});
	}
};

struct FColorHandler : TPropertyComponentHandler<FColorPropertyTraits, float, float, float, float>
{
	virtual void DispatchInitializePropertyMetaDataTasks(const FPropertyDefinition& Definition, FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents, UMovieSceneEntitySystemLinker* Linker) override
	{
		FBuiltInComponentTypes* BuiltInComponents = FBuiltInComponentTypes::Get();
		FMovieSceneTracksComponentTypes* TrackComponents = FMovieSceneTracksComponentTypes::Get();

		FEntityTaskBuilder()
		.Read(BuiltInComponents->BoundObject)
		.Read(BuiltInComponents->PropertyBinding)
		.Write(TrackComponents->Color.MetaDataComponents.GetType<0>())
		.FilterAll({ BuiltInComponents->Tags.NeedsLink })
		.Iterate_PerEntity(&Linker->EntityManager, [](UObject* Object, const FMovieScenePropertyBinding& Binding, EColorPropertyType& OutType)
		{
			FStructProperty* BoundProperty = CastField<FStructProperty>(FTrackInstancePropertyBindings::FindProperty(Object, Binding.PropertyPath.ToString()));
			if (ensure(BoundProperty && BoundProperty->Struct))
			{
				if (BoundProperty->Struct == TBaseStructure<FColor>::Get())
				{
					// We assume the color we get back is in sRGB, assigning it to a linear color will implicitly
					// convert it to a linear color instead of using ReinterpretAsLinear which will just change the
					// bytes into floats using divide by 255.
					OutType = EColorPropertyType::Color;
				}
				else if (BoundProperty->Struct == TBaseStructure<FSlateColor>::Get())
				{
					OutType = EColorPropertyType::Slate;
				}
				else
				{
					ensure(BoundProperty->Struct == TBaseStructure<FLinearColor>::Get());
					OutType = EColorPropertyType::Linear;
				}
			}
			else
			{
				OutType = EColorPropertyType::Linear;
			}
		});
	}
};


struct FFloatVectorHandler : TPropertyComponentHandler<FFloatVectorPropertyTraits, float, float, float, float>
{
	virtual void DispatchInitializePropertyMetaDataTasks(const FPropertyDefinition& Definition, FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents, UMovieSceneEntitySystemLinker* Linker) override
	{
		FBuiltInComponentTypes* BuiltInComponents = FBuiltInComponentTypes::Get();
		FMovieSceneTracksComponentTypes* TrackComponents = FMovieSceneTracksComponentTypes::Get();

		FEntityTaskBuilder()
		.Read(BuiltInComponents->BoundObject)
		.Read(BuiltInComponents->PropertyBinding)
		.Write(TrackComponents->FloatVector.MetaDataComponents.GetType<0>())
		.FilterAll({ BuiltInComponents->Tags.NeedsLink })
		.Iterate_PerEntity(&Linker->EntityManager, [](UObject* Object, const FMovieScenePropertyBinding& Binding, FVectorPropertyMetaData& OutMetaData)
		{
			FStructProperty* BoundProperty = CastField<FStructProperty>(FTrackInstancePropertyBindings::FindProperty(Object, Binding.PropertyPath.ToString()));
			if (ensure(BoundProperty && BoundProperty->Struct))
			{
				if (BoundProperty->Struct == TBaseStructure<FVector2D>::Get() || BoundProperty->Struct == TVariantStructure<FVector2f>::Get()) // LWC_TODO: Fix LWC type FName case
				{
					OutMetaData.NumChannels = 2;
					OutMetaData.bIsDouble = (BoundProperty->Struct == TBaseStructure<FVector2D>::Get()); // LWC_TODO: Fix LWC type FName case
				}
				else if (BoundProperty->Struct == TBaseStructure<FVector>::Get() || BoundProperty->Struct == TVariantStructure<FVector3f>::Get() || BoundProperty->Struct == TVariantStructure<FVector3d>::Get())
				{
					OutMetaData.NumChannels = 3;
					OutMetaData.bIsDouble = (BoundProperty->Struct == TBaseStructure<FVector>::Get() || BoundProperty->Struct == TVariantStructure<FVector3d>::Get());
				}
				else
				{
					ensure(BoundProperty->Struct == TBaseStructure<FVector4>::Get() || BoundProperty->Struct == TVariantStructure<FVector4f>::Get() || BoundProperty->Struct == TVariantStructure<FVector4d>::Get());
					OutMetaData.NumChannels = 4;
					OutMetaData.bIsDouble = (BoundProperty->Struct == TBaseStructure<FVector4>::Get() || BoundProperty->Struct == TVariantStructure<FVector4d>::Get());
				}
			}
			else
			{
				OutMetaData.NumChannels = 4;
			}
		});
	}
};


struct FDoubleVectorHandler : TPropertyComponentHandler<FDoubleVectorPropertyTraits, double, double, double, double>
{
	virtual void DispatchInitializePropertyMetaDataTasks(const FPropertyDefinition& Definition, FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents, UMovieSceneEntitySystemLinker* Linker) override
	{
		FBuiltInComponentTypes* BuiltInComponents = FBuiltInComponentTypes::Get();
		FMovieSceneTracksComponentTypes* TrackComponents = FMovieSceneTracksComponentTypes::Get();

		FEntityTaskBuilder()
		.Read(BuiltInComponents->BoundObject)
		.Read(BuiltInComponents->PropertyBinding)
		.Write(TrackComponents->DoubleVector.MetaDataComponents.GetType<0>())
		.FilterAll({ BuiltInComponents->Tags.NeedsLink })
		.Iterate_PerEntity(&Linker->EntityManager, [](UObject* Object, const FMovieScenePropertyBinding& Binding, FVectorPropertyMetaData& OutMetaData)
		{
			FStructProperty* BoundProperty = CastField<FStructProperty>(FTrackInstancePropertyBindings::FindProperty(Object, Binding.PropertyPath.ToString()));
			if (ensure(BoundProperty && BoundProperty->Struct))
			{
				if (BoundProperty->Struct == TBaseStructure<FVector2D>::Get())
				{
					OutMetaData.NumChannels = 2;
				}
				else if (BoundProperty->Struct->GetFName() == NAME_Vector3d || BoundProperty->Struct->GetFName() == NAME_Vector)
				{
					OutMetaData.NumChannels = 3;
				}
				else
				{
					ensure(BoundProperty->Struct == TBaseStructure<FVector4>::Get() || (BoundProperty->Struct->GetFName() == NAME_Vector4d) || (BoundProperty->Struct->GetFName() == NAME_Vector4));
					OutMetaData.NumChannels = 4;
				}
			}
			else
			{
				OutMetaData.NumChannels = 4;
			}
		});
	}
};


struct FComponentTransformHandler : TPropertyComponentHandler<FComponentTransformPropertyTraits, double, double, double, double, double, double, double, double, double>
{
	TSharedPtr<IPreAnimatedStorage> GetPreAnimatedStateStorage(const FPropertyDefinition& Definition, FPreAnimatedStateExtension* Container) override
	{
		return Container->GetOrCreateStorage<FPreAnimatedComponentTransformStorage>();
	}
};

FMovieSceneTracksComponentTypes::FMovieSceneTracksComponentTypes()
{
	FComponentRegistry* ComponentRegistry = UMovieSceneEntitySystemLinker::GetComponents();

	ComponentRegistry->NewPropertyType(Bool, TEXT("bool"));
	ComponentRegistry->NewPropertyType(Byte, TEXT("byte"));
	ComponentRegistry->NewPropertyType(Enum, TEXT("enum"));
	ComponentRegistry->NewPropertyType(Float, TEXT("float"));
	ComponentRegistry->NewPropertyType(Double, TEXT("double"));
	ComponentRegistry->NewPropertyType(Color, TEXT("color"));
	ComponentRegistry->NewPropertyType(Integer, TEXT("int32"));
	ComponentRegistry->NewPropertyType(FloatVector, TEXT("float vector"));
	ComponentRegistry->NewPropertyType(DoubleVector, TEXT("double vector"));

	ComponentRegistry->NewPropertyType(Transform, TEXT("FTransform"));
	ComponentRegistry->NewPropertyType(EulerTransform, TEXT("FEulerTransform"));
	ComponentRegistry->NewPropertyType(ComponentTransform, TEXT("Component Transform"));

	Float.MetaDataComponents.Initialize(ComponentRegistry, TEXT("Is Double"));
	Color.MetaDataComponents.Initialize(ComponentRegistry, TEXT("Color Type"));
	FloatVector.MetaDataComponents.Initialize(ComponentRegistry, TEXT("Num Float Vector Channels"));
	DoubleVector.MetaDataComponents.Initialize(ComponentRegistry, TEXT("Num Double Vector Channels"));

	ComponentRegistry->NewComponentType(&QuaternionRotationChannel[0], TEXT("Quaternion Rotation Channel 0"));
	ComponentRegistry->NewComponentType(&QuaternionRotationChannel[1], TEXT("Quaternion Rotation Channel 1"));
	ComponentRegistry->NewComponentType(&QuaternionRotationChannel[2], TEXT("Quaternion Rotation Channel 2"));

	ComponentRegistry->NewComponentType(&AttachParent, TEXT("Attach Parent"));
	ComponentRegistry->NewComponentType(&AttachComponent, TEXT("Attachment Component"));
	ComponentRegistry->NewComponentType(&AttachParentBinding, TEXT("Attach Parent Binding"));

	ComponentRegistry->NewComponentType(&LevelVisibility, TEXT("Level Visibility"));
	ComponentRegistry->NewComponentType(&DataLayer, TEXT("Data Layer"));

	FBuiltInComponentTypes* BuiltInComponents = FBuiltInComponentTypes::Get();

	// --------------------------------------------------------------------------------------------
	// Set up bool properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Bool)
	.AddSoleChannel(BuiltInComponents->BoolResult)
	.SetBlenderSystem<UMovieScenePiecewiseBoolBlenderSystem>()
	.SetCustomAccessors(&Accessors.Bool)
	.Commit();

	// Set up FTransform properties
	BuiltInComponents->PropertyRegistry.DefineCompositeProperty(Transform)
	.AddComposite(BuiltInComponents->DoubleResult[0], &FIntermediate3DTransform::T_X)
	.AddComposite(BuiltInComponents->DoubleResult[1], &FIntermediate3DTransform::T_Y)
	.AddComposite(BuiltInComponents->DoubleResult[2], &FIntermediate3DTransform::T_Z)
	.AddComposite(BuiltInComponents->DoubleResult[3], &FIntermediate3DTransform::R_X)
	.AddComposite(BuiltInComponents->DoubleResult[4], &FIntermediate3DTransform::R_Y)
	.AddComposite(BuiltInComponents->DoubleResult[5], &FIntermediate3DTransform::R_Z)
	.AddComposite(BuiltInComponents->DoubleResult[6], &FIntermediate3DTransform::S_X)
	.AddComposite(BuiltInComponents->DoubleResult[7], &FIntermediate3DTransform::S_Y)
	.AddComposite(BuiltInComponents->DoubleResult[8], &FIntermediate3DTransform::S_Z)
	.SetBlenderSystem<UMovieScenePiecewiseDoubleBlenderSystem>()
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up byte properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Byte)
	.AddSoleChannel(BuiltInComponents->ByteResult)
	.SetBlenderSystem<UMovieScenePiecewiseByteBlenderSystem>()
	.SetCustomAccessors(&Accessors.Byte)
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up enum properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Enum)
	.AddSoleChannel(BuiltInComponents->ByteResult)
	.SetBlenderSystem<UMovieScenePiecewiseEnumBlenderSystem>()
	.SetCustomAccessors(&Accessors.Enum)
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up integer properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Integer)
	.AddSoleChannel(BuiltInComponents->IntegerResult)
	.SetBlenderSystem<UMovieScenePiecewiseIntegerBlenderSystem>()
	.SetCustomAccessors(&Accessors.Integer)
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up float properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Float)
	.AddSoleChannel(BuiltInComponents->FloatResult[0])
	.SetBlenderSystem<UMovieScenePiecewiseFloatBlenderSystem>()
	.SetCustomAccessors(&Accessors.Float)
	.Commit(FFloatHandler());

	// --------------------------------------------------------------------------------------------
	// Set up double properties
	BuiltInComponents->PropertyRegistry.DefineProperty(Double)
	.AddSoleChannel(BuiltInComponents->DoubleResult[0])
	.SetBlenderSystem<UMovieScenePiecewiseDoubleBlenderSystem>()
	.SetCustomAccessors(&Accessors.Double)
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up color properties
	BuiltInComponents->PropertyRegistry.DefineCompositeProperty(Color)
	.AddComposite(BuiltInComponents->FloatResult[0], &FIntermediateColor::R)
	.AddComposite(BuiltInComponents->FloatResult[1], &FIntermediateColor::G)
	.AddComposite(BuiltInComponents->FloatResult[2], &FIntermediateColor::B)
	.AddComposite(BuiltInComponents->FloatResult[3], &FIntermediateColor::A)
	.SetBlenderSystem<UMovieScenePiecewiseFloatBlenderSystem>()
	.SetCustomAccessors(&Accessors.Color)
	.Commit(FColorHandler());

	// We have some custom accessors for well-known types.
	Accessors.Color.Add(
			ULightComponent::StaticClass(), GET_MEMBER_NAME_CHECKED(ULightComponent, LightColor), 
			GetLightComponentLightColor, SetLightComponentLightColor);
	Accessors.Color.Add(
			USkyLightComponent::StaticClass(), GET_MEMBER_NAME_CHECKED(USkyLightComponent, LightColor), 
			GetSkyLightComponentLightColor, SetSkyLightComponentLightColor);
	
	const FString SecondFogDataFogDensityPath = FString::Printf(TEXT("%s.%s"), GET_MEMBER_NAME_STRING_CHECKED(UExponentialHeightFogComponent, SecondFogData), GET_MEMBER_NAME_STRING_CHECKED(FExponentialHeightFogData, FogDensity));
	Accessors.Float.Add(
			UExponentialHeightFogComponent::StaticClass(), *SecondFogDataFogDensityPath,
			GetSecondFogDataFogDensity, SetSecondFogDataFogDensity);
	const FString SecondFogDataFogHeightFalloffPath = FString::Printf(TEXT("%s.%s"), GET_MEMBER_NAME_STRING_CHECKED(UExponentialHeightFogComponent, SecondFogData), GET_MEMBER_NAME_STRING_CHECKED(FExponentialHeightFogData, FogHeightFalloff));
	Accessors.Float.Add(
			UExponentialHeightFogComponent::StaticClass(), *SecondFogDataFogHeightFalloffPath,
			GetSecondFogDataFogHeightFalloff, SetSecondFogDataFogHeightFalloff);
	const FString SecondFogDataFogHeightOffsetPath = FString::Printf(TEXT("%s.%s"), GET_MEMBER_NAME_STRING_CHECKED(UExponentialHeightFogComponent, SecondFogData), GET_MEMBER_NAME_STRING_CHECKED(FExponentialHeightFogData, FogHeightOffset));
	Accessors.Float.Add(
			UExponentialHeightFogComponent::StaticClass(), *SecondFogDataFogHeightOffsetPath,
			GetSecondFogDataFogHeightOffset, SetSecondFogDataFogHeightOffset);

	// --------------------------------------------------------------------------------------------
	// Set up vector properties
	BuiltInComponents->PropertyRegistry.DefineCompositeProperty(FloatVector)
	.AddComposite(BuiltInComponents->FloatResult[0], &FFloatIntermediateVector::X)
	.AddComposite(BuiltInComponents->FloatResult[1], &FFloatIntermediateVector::Y)
	.AddComposite(BuiltInComponents->FloatResult[2], &FFloatIntermediateVector::Z)
	.AddComposite(BuiltInComponents->FloatResult[3], &FFloatIntermediateVector::W)
	.SetBlenderSystem<UMovieScenePiecewiseFloatBlenderSystem>()
	.SetCustomAccessors(&Accessors.FloatVector)
	.Commit(FFloatVectorHandler());

	BuiltInComponents->PropertyRegistry.DefineCompositeProperty(DoubleVector)
	.AddComposite(BuiltInComponents->DoubleResult[0], &FDoubleIntermediateVector::X)
	.AddComposite(BuiltInComponents->DoubleResult[1], &FDoubleIntermediateVector::Y)
	.AddComposite(BuiltInComponents->DoubleResult[2], &FDoubleIntermediateVector::Z)
	.AddComposite(BuiltInComponents->DoubleResult[3], &FDoubleIntermediateVector::W)
	.SetBlenderSystem<UMovieScenePiecewiseDoubleBlenderSystem>()
	.SetCustomAccessors(&Accessors.DoubleVector)
	.Commit(FDoubleVectorHandler());

	// --------------------------------------------------------------------------------------------
	// Set up FEulerTransform properties
	BuiltInComponents->PropertyRegistry.DefineCompositeProperty(EulerTransform)
	.AddComposite(BuiltInComponents->DoubleResult[0], &FIntermediate3DTransform::T_X)
	.AddComposite(BuiltInComponents->DoubleResult[1], &FIntermediate3DTransform::T_Y)
	.AddComposite(BuiltInComponents->DoubleResult[2], &FIntermediate3DTransform::T_Z)
	.AddComposite(BuiltInComponents->DoubleResult[3], &FIntermediate3DTransform::R_X)
	.AddComposite(BuiltInComponents->DoubleResult[4], &FIntermediate3DTransform::R_Y)
	.AddComposite(BuiltInComponents->DoubleResult[5], &FIntermediate3DTransform::R_Z)
	.AddComposite(BuiltInComponents->DoubleResult[6], &FIntermediate3DTransform::S_X)
	.AddComposite(BuiltInComponents->DoubleResult[7], &FIntermediate3DTransform::S_Y)
	.AddComposite(BuiltInComponents->DoubleResult[8], &FIntermediate3DTransform::S_Z)
	.SetBlenderSystem<UMovieScenePiecewiseDoubleBlenderSystem>()
	.Commit();

	// --------------------------------------------------------------------------------------------
	// Set up component transforms
	{
		Accessors.ComponentTransform.Add(USceneComponent::StaticClass(), "Transform", &GetComponentTransform, &SetComponentTransformAndVelocity);

		BuiltInComponents->PropertyRegistry.DefineCompositeProperty(ComponentTransform)
		.AddComposite(BuiltInComponents->DoubleResult[0], &FIntermediate3DTransform::T_X)
		.AddComposite(BuiltInComponents->DoubleResult[1], &FIntermediate3DTransform::T_Y)
		.AddComposite(BuiltInComponents->DoubleResult[2], &FIntermediate3DTransform::T_Z)
		.AddComposite(BuiltInComponents->DoubleResult[3], &FIntermediate3DTransform::R_X)
		.AddComposite(BuiltInComponents->DoubleResult[4], &FIntermediate3DTransform::R_Y)
		.AddComposite(BuiltInComponents->DoubleResult[5], &FIntermediate3DTransform::R_Z)
		.AddComposite(BuiltInComponents->DoubleResult[6], &FIntermediate3DTransform::S_X)
		.AddComposite(BuiltInComponents->DoubleResult[7], &FIntermediate3DTransform::S_Y)
		.AddComposite(BuiltInComponents->DoubleResult[8], &FIntermediate3DTransform::S_Z)
		.SetBlenderSystem<UMovieScenePiecewiseDoubleBlenderSystem>()
		.SetCustomAccessors(&Accessors.ComponentTransform)
		.Commit(FComponentTransformHandler());
	}

	// --------------------------------------------------------------------------------------------
	// Set up quaternion rotation components
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(QuaternionRotationChannel); ++Index)
	{
		ComponentRegistry->Factories.DuplicateChildComponent(QuaternionRotationChannel[Index]);
		ComponentRegistry->Factories.DefineMutuallyInclusiveComponent(QuaternionRotationChannel[Index], BuiltInComponents->DoubleResult[Index + 3]);
		ComponentRegistry->Factories.DefineMutuallyInclusiveComponent(QuaternionRotationChannel[Index], BuiltInComponents->EvalTime);
	}

	// --------------------------------------------------------------------------------------------
	// Set up attachment components
	ComponentRegistry->Factories.DefineChildComponent(AttachParentBinding, AttachParent);

	ComponentRegistry->Factories.DuplicateChildComponent(AttachParentBinding);
	ComponentRegistry->Factories.DuplicateChildComponent(AttachComponent);
}

FMovieSceneTracksComponentTypes::~FMovieSceneTracksComponentTypes()
{
}

void FMovieSceneTracksComponentTypes::Destroy()
{
	GMovieSceneTracksComponentTypes.Reset();
	GMovieSceneTracksComponentTypesDestroyed = true;
}

FMovieSceneTracksComponentTypes* FMovieSceneTracksComponentTypes::Get()
{
	if (!GMovieSceneTracksComponentTypes.IsValid())
	{
		check(!GMovieSceneTracksComponentTypesDestroyed);
		GMovieSceneTracksComponentTypes.Reset(new FMovieSceneTracksComponentTypes);
	}
	return GMovieSceneTracksComponentTypes.Get();
}


} // namespace MovieScene
} // namespace UE
