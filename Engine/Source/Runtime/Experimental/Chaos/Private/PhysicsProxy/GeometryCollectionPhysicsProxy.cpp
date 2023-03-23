// Copyright Epic Games, Inc. All Rights Reserved.


#include "PhysicsProxy/GeometryCollectionPhysicsProxy.h"
#include "PhysicsProxy/FieldSystemProxyHelper.h"

#include "PhysicsSolver.h"
#include "ChaosStats.h"
#include "Chaos/ArrayCollectionArray.h"
#include "Chaos/BVHParticles.h"
#include "Chaos/Transform.h"
#include "Chaos/ParallelFor.h"
#include "Chaos/Particles.h"
#include "Chaos/TriangleMesh.h"
#include "Chaos/MassProperties.h"
#include "ChaosSolversModule.h"
#include "Chaos/PBDCollisionConstraintsUtil.h"
#include "Chaos/PerParticleGravity.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Convex.h"
#include "Chaos/Serializable.h"
#include "Chaos/ErrorReporter.h"
#include "Chaos/PBDRigidClustering.h"
#include "GeometryCollection/GeometryCollectionAlgo.h"
#include "GeometryCollection/GeometryCollectionUtility.h"
#include "GeometryCollection/GeometryCollectionSizeSpecificUtility.h"
#include "GeometryCollection/GeometryCollectionSimulationTypes.h"
#include "GeometryCollection/ManagedArrayCollection.h"
#include "Modules/ModuleManager.h"
#include "Chaos/PullPhysicsDataImp.h"
#include "Chaos/PBDRigidsEvolution.h"

#ifndef TODO_REIMPLEMENT_INIT_COMMANDS
#define TODO_REIMPLEMENT_INIT_COMMANDS 0
#endif

#ifndef TODO_REIMPLEMENT_FRACTURE
#define TODO_REIMPLEMENT_FRACTURE 0
#endif

#ifndef TODO_REIMPLEMENT_RIGID_CACHING
#define TODO_REIMPLEMENT_RIGID_CACHING 0
#endif

float CollisionParticlesPerObjectFractionDefault = 1.0f;
FAutoConsoleVariableRef CVarCollisionParticlesPerObjectFractionDefault(
	TEXT("p.CollisionParticlesPerObjectFractionDefault"), 
	CollisionParticlesPerObjectFractionDefault, 
	TEXT("Fraction of verts"));

bool DisableGeometryCollectionGravity = false;
FAutoConsoleVariableRef CVarGeometryCollectionDisableGravity(
	TEXT("p.GeometryCollectionDisableGravity"),
	DisableGeometryCollectionGravity,
	TEXT("Disable gravity for geometry collections"));

bool GeometryCollectionCollideAll = false;
FAutoConsoleVariableRef CVarGeometryCollectionCollideAll(
	TEXT("p.GeometryCollectionCollideAll"),
	GeometryCollectionCollideAll,
	TEXT("Bypass the collision matrix and make geometry collections collide against everything"));


bool bGeometryCollectionEnabledNestedChildTransformUpdates = true;
FAutoConsoleVariableRef CVarEnabledNestedChildTransformUpdates(
	TEXT("p.GeometryCollection.EnabledNestedChildTransformUpdates"),
	bGeometryCollectionEnabledNestedChildTransformUpdates,
	TEXT("Enable updates for driven, disabled, child bodies. Used for line trace results against geometry collections.[def: true]"));

bool bGeometryCollectionAlwaysGenerateGTCollisionForClusters = true;
FAutoConsoleVariableRef CVarGeometryCollectionAlwaysGenerateGTCollisionForClusters(
	TEXT("p.GeometryCollection.AlwaysGenerateGTCollisionForClusters"),
	bGeometryCollectionAlwaysGenerateGTCollisionForClusters,
	TEXT("When enabled, always generate a game thread side collision for clusters.[def: true]"));

DEFINE_LOG_CATEGORY_STATIC(UGCC_LOG, Error, All);

//==============================================================================
// FGeometryCollectionResults
//==============================================================================

FGeometryCollectionResults::FGeometryCollectionResults()
	: IsObjectDynamic(false)
	, IsObjectLoading(false)
{}

void FGeometryCollectionResults::Reset()
{
	SolverDt = 0.0f;
	DisabledStates.SetNum(0);
	GlobalTransforms.SetNum(0);
	ParticleToWorldTransforms.SetNum(0);
	IsObjectDynamic = false;
	IsObjectLoading = false;
}

//==============================================================================
// FGeometryCollectionPhysicsProxy helper functions
//==============================================================================


Chaos::FTriangleMesh* CreateTriangleMesh(
	const int32 FaceStart,
	const int32 FaceCount, 
	const TManagedArray<bool>& Visible, 
	const TManagedArray<FIntVector>& Indices,
	bool bRotateWinding)
{
	TArray<Chaos::TVector<int32, 3>> Faces;
	Faces.Reserve(FaceCount);
	
	const int32 FaceEnd = FaceStart + FaceCount;
	for (int Idx = FaceStart; Idx < FaceEnd; ++Idx)
	{
		// Note: This function used to cull small triangles.  As one of the purposes 
		// of the tri mesh this function creates is for level set rasterization, we 
		// don't want to do that.  Keep the mesh intact, which hopefully is water tight.
		if (Visible[Idx])
		{
			const FIntVector& Tri = Indices[Idx];

			if(bRotateWinding)
			{
				Faces.Add(Chaos::TVector<int32, 3>(Tri.Z, Tri.Y, Tri.X));
			}
			else
			{
				Faces.Add(Chaos::TVector<int32, 3>(Tri.X, Tri.Y, Tri.Z));
			}
		}
	}
	return new Chaos::FTriangleMesh(MoveTemp(Faces)); // Culls geometrically degenerate faces
}

TArray<int32> ComputeTransformToGeometryMap(const FGeometryCollection& Collection)
{
	const int32 NumTransforms = Collection.NumElements(FGeometryCollection::TransformGroup);
	const int32 NumGeometries = Collection.NumElements(FGeometryCollection::GeometryGroup);
	const TManagedArray<int32>& TransformIndex = Collection.TransformIndex;

	TArray<int32> TransformToGeometryMap;
	TransformToGeometryMap.AddUninitialized(NumTransforms);
	for(int32 GeometryIndex = 0; GeometryIndex < NumGeometries; ++GeometryIndex)
	{
		const int32 TransformGroupIndex = TransformIndex[GeometryIndex];
		TransformToGeometryMap[TransformGroupIndex] = GeometryIndex;
	}

	return TransformToGeometryMap;
}


DECLARE_CYCLE_STAT(TEXT("FGeometryCollectionPhysicsProxy::PopulateSimulatedParticle"), STAT_PopulateSimulatedParticle, STATGROUP_Chaos);
void PopulateSimulatedParticle(
	Chaos::TPBDRigidParticleHandle<Chaos::FReal,3>* Handle,
	const FSharedSimulationParameters& SharedParams,
	const FCollisionStructureManager::FSimplicial* Simplicial,
	FGeometryDynamicCollection::FSharedImplicit Implicit,
	const FCollisionFilterData SimFilterIn,
	const FCollisionFilterData QueryFilterIn,
	Chaos::FReal MassIn,
	Chaos::TVec3<Chaos::FRealSingle> InertiaTensorVec,
	const FTransform& WorldTransform, 
	const uint8 DynamicState, 
	const int16 CollisionGroup,
	float CollisionParticlesPerObjectFraction)
{
	SCOPE_CYCLE_COUNTER(STAT_PopulateSimulatedParticle);
	Handle->SetDisabledLowLevel(false);
	Handle->SetX(WorldTransform.GetTranslation());
	Handle->SetV(Chaos::FVec3(0.f));
	Handle->SetR(WorldTransform.GetRotation().GetNormalized());
	Handle->SetW(Chaos::FVec3(0.f));
	Handle->SetP(Handle->X());
	Handle->SetQ(Handle->R());
	Handle->SetIslandIndex(INDEX_NONE);
	Handle->SetConstraintGraphIndex(INDEX_NONE);
	Handle->SetCenterOfMass(FVector3f::ZeroVector);
	Handle->SetRotationOfMass(FQuat::Identity);

	//
	// Setup Mass
	//
	{
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Uninitialized);

		if (!CHAOS_ENSURE_MSG(FMath::IsWithinInclusive<Chaos::FReal>(MassIn, SharedParams.MinimumMassClamp, SharedParams.MaximumMassClamp),
			TEXT("Clamped mass[%3.5f] to range [%3.5f,%3.5f]"), MassIn, SharedParams.MinimumMassClamp, SharedParams.MaximumMassClamp))
		{
			MassIn = FMath::Clamp<Chaos::FReal>(MassIn, SharedParams.MinimumMassClamp, SharedParams.MaximumMassClamp);
		}

		if (!CHAOS_ENSURE_MSG(!FMath::IsNaN(InertiaTensorVec[0]) && !FMath::IsNaN(InertiaTensorVec[1]) && !FMath::IsNaN(InertiaTensorVec[2]),
			TEXT("Nan Tensor, reset to unit tesor")))
		{
			InertiaTensorVec = FVector3f(1);
		}
		else if (!CHAOS_ENSURE_MSG(FMath::IsWithinInclusive(InertiaTensorVec[0], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp)
			&& FMath::IsWithinInclusive(InertiaTensorVec[1], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp)
			&& FMath::IsWithinInclusive(InertiaTensorVec[2], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp),
			TEXT("Clamped Inertia tensor[%3.5f,%3.5f,%3.5f]. Clamped each element to [%3.5f, %3.5f,]"), InertiaTensorVec[0], InertiaTensorVec[1], InertiaTensorVec[2],
			SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp))
		{
			InertiaTensorVec[0] = FMath::Clamp(InertiaTensorVec[0], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp);
			InertiaTensorVec[1] = FMath::Clamp(InertiaTensorVec[1], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp);
			InertiaTensorVec[2] = FMath::Clamp(InertiaTensorVec[2], SharedParams.MinimumInertiaTensorDiagonalClamp, SharedParams.MaximumInertiaTensorDiagonalClamp);
		}

		Handle->SetM(MassIn);
		Handle->SetI(InertiaTensorVec);
		const Chaos::FReal MassInv = (MassIn > 0.0f) ? 1.0f / MassIn : 0.0f;
		const Chaos::FVec3 InertiaInv = (MassIn > 0.0f) ? Chaos::FVec3(InertiaTensorVec).Reciprocal() : Chaos::FVec3::ZeroVector;
		Handle->SetInvM(MassInv);
		Handle->SetInvI(InertiaInv);
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Dynamic); // this step sets InvM, InvInertia, P, Q
	}

	Handle->SetCollisionGroup(CollisionGroup);

	// @todo(GCCollisionShapes) : add support for multiple shapes, currently just one. 
	FCollectionCollisionTypeData SingleSupportedCollisionTypeData = FCollectionCollisionTypeData();
	if (SharedParams.SizeSpecificData.Num() && SharedParams.SizeSpecificData[0].CollisionShapesData.Num())
	{
		SingleSupportedCollisionTypeData = SharedParams.SizeSpecificData[0].CollisionShapesData[0];
	}
	const FVector Scale = WorldTransform.GetScale3D();
	if (Implicit)	//todo(ocohen): this is only needed for cases where clusters have no proxy. Kind of gross though, should refactor
	{
		auto DeepCopyImplicit = [&Scale](FGeometryDynamicCollection::FSharedImplicit ImplicitToCopy) -> TUniquePtr<Chaos::FImplicitObject>
		{
			if (Scale.Equals(FVector::OneVector))
			{
				return ImplicitToCopy->DeepCopy();
			}
			else
			{
				return ImplicitToCopy->DeepCopyWithScale(Scale);
			}
		};

		TSharedPtr<Chaos::FImplicitObject, ESPMode::ThreadSafe> SharedImplicitTS(DeepCopyImplicit(Implicit).Release());
		FCollisionStructureManager::UpdateImplicitFlags(SharedImplicitTS.Get(), SingleSupportedCollisionTypeData.CollisionType);
		Handle->SetSharedGeometry(SharedImplicitTS);
		Handle->SetHasBounds(true);
		Handle->SetLocalBounds(SharedImplicitTS->BoundingBox());
		const Chaos::FRigidTransform3 Xf(Handle->X(), Handle->R());
		Handle->UpdateWorldSpaceState(Xf, Chaos::FVec3(0));
	}

	if (Simplicial && SingleSupportedCollisionTypeData.CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric)
	{
		Handle->CollisionParticlesInitIfNeeded();

		TUniquePtr<Chaos::FBVHParticles>& CollisionParticles = Handle->CollisionParticles();
		CollisionParticles.Reset(Simplicial->NewCopy()); // @chaos(optimize) : maybe just move this memory instead. 

		const int32 NumCollisionParticles = CollisionParticles->Size();
		const int32 AdjustedNumCollisionParticles = FMath::TruncToInt(CollisionParticlesPerObjectFraction * (float)NumCollisionParticles);
		int32 CollisionParticlesSize = FMath::Max<int32>(0, FMath::Min<int32>(AdjustedNumCollisionParticles, NumCollisionParticles));
		CollisionParticles->Resize(CollisionParticlesSize); // Truncates! ( particles are already sorted by importance )

		Chaos::FAABB3 ImplicitShapeDomain = Chaos::FAABB3::FullAABB();
		if (Implicit && Implicit->GetType() == Chaos::ImplicitObjectType::LevelSet && Implicit->HasBoundingBox())
		{
			ImplicitShapeDomain = Implicit->BoundingBox();
			ImplicitShapeDomain.Scale(Scale);
		}

		// we need to account for scale and check if the particle is still within its domain
		for (int32 ParticleIndex = 0; ParticleIndex < (int32)CollisionParticles->Size(); ++ParticleIndex)
		{
			CollisionParticles->X(ParticleIndex) *= Scale;
			
			// Make sure the collision particles are at least in the domain 
			// of the implicit shape.
			ensure(ImplicitShapeDomain.Contains(CollisionParticles->X(ParticleIndex)));
		}

		// @todo(remove): IF there is no simplicial we should not be forcing one. 
		if (!CollisionParticles->Size())
		{
			CollisionParticles->AddParticles(1);
			CollisionParticles->X(0) = Chaos::FVec3(0);
		}
		CollisionParticles->UpdateAccelerationStructures();
	}

	if (GeometryCollectionCollideAll) // cvar
	{
		// Override collision filters and make this body collide with everything.
		int32 CurrShape = 0;
		FCollisionFilterData FilterData;
		FilterData.Word1 = 0xFFFF; // this body channel
		FilterData.Word3 = 0xFFFF; // collision candidate channels
		for (const TUniquePtr<Chaos::FPerShapeData>& Shape : Handle->ShapesArray())
		{
			Shape->SetSimEnabled(true);
			Shape->SetCollisionTraceType(Chaos::EChaosCollisionTraceFlag::Chaos_CTF_UseDefault);
			//Shape->CollisionTraceType = Chaos::EChaosCollisionTraceFlag::Chaos_CTF_UseSimpleAndComplex;
			Shape->SetSimData(FilterData);
			Shape->SetQueryData(FCollisionFilterData());
		}
	}
	else
	{
		for (const TUniquePtr<Chaos::FPerShapeData>& Shape : Handle->ShapesArray())
		{
			Shape->SetSimData(SimFilterIn);
			Shape->SetQueryData(QueryFilterIn);
		}
	}

	//
	//  Manage Object State
	//

	// Only sleep if we're not replaying a simulation
	// #BG TODO If this becomes an issue, recorded tracks should track awake state as well as transforms
	if (DynamicState == (uint8)EObjectStateTypeEnum::Chaos_Object_Sleeping)
	{
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Sleeping);
	}
	else if (DynamicState == (uint8)EObjectStateTypeEnum::Chaos_Object_Kinematic)
	{
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Kinematic);
	}
	else if (DynamicState == (uint8)EObjectStateTypeEnum::Chaos_Object_Static)
	{
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Static);
	}
	else
	{
		Handle->SetObjectStateLowLevel(Chaos::EObjectStateType::Dynamic);
	}
}

//==============================================================================
// FGeometryCollectionPhysicsProxy
//==============================================================================


FGeometryCollectionPhysicsProxy::FGeometryCollectionPhysicsProxy(
	UObject* InOwner,
	FGeometryDynamicCollection& GameThreadCollectionIn,
	const FSimulationParameters& SimulationParameters,
	FCollisionFilterData InSimFilter,
	FCollisionFilterData InQueryFilter,
	const Chaos::EMultiBufferMode BufferMode)
	: Base(InOwner)
	, Parameters(SimulationParameters)
	, NumParticles(INDEX_NONE)
	, BaseParticleIndex(INDEX_NONE)
	, IsObjectDynamic(false)
	, IsObjectLoading(true)
	, IsObjectDeleting(false)
	, SimFilter(InSimFilter)
	, QueryFilter(InQueryFilter)
#if TODO_REIMPLEMENT_RIGID_CACHING
	, ProxySimDuration(0.0f)
	, LastSyncCountGT(MAX_uint32)
#endif
	, CollisionParticlesPerObjectFraction(CollisionParticlesPerObjectFractionDefault)

	, GameThreadCollection(GameThreadCollectionIn)
	, bIsPhysicsThreadWorldTransformDirty(false)
{
	// We rely on a guarded buffer.
	check(BufferMode == Chaos::EMultiBufferMode::TripleGuarded);
}


FGeometryCollectionPhysicsProxy::~FGeometryCollectionPhysicsProxy()
{}

float ReportHighParticleFraction = -1.f;
FAutoConsoleVariableRef CVarReportHighParticleFraction(TEXT("p.gc.ReportHighParticleFraction"), ReportHighParticleFraction, TEXT("Report any objects with particle fraction above this threshold"));

void FGeometryCollectionPhysicsProxy::Initialize(Chaos::FPBDRigidsEvolutionBase *Evolution)
{
	check(IsInGameThread());
	//
	// Game thread initilization. 
	//
	//  1) Create a input buffer to store all game thread side data. 
	//  2) Populate the buffer with the necessary data.
	//  3) Deep copy the data to the other buffers. 
	//
	FGeometryDynamicCollection& DynamicCollection = GameThreadCollection;

	InitializeDynamicCollection(DynamicCollection, *Parameters.RestCollection, Parameters);

	// Attach the external particles to the gamethread collection
	if (DynamicCollection.HasAttribute(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup))
		DynamicCollection.RemoveAttribute(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup);
	DynamicCollection.AddExternalAttribute<TUniquePtr<Chaos::FGeometryParticle>>(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup, GTParticles);


	NumParticles = DynamicCollection.NumElements(FGeometryCollection::TransformGroup);
	BaseParticleIndex = 0; // Are we always zero indexed now?
	SolverClusterID.Init(nullptr, NumParticles);
	SolverClusterHandles.Init(nullptr, NumParticles);
	SolverParticleHandles.Init(nullptr, NumParticles);

	// compatibility requirement to make sure we at least initialize GameThreadPerFrameData properly
	GameThreadPerFrameData.SetWorldTransform(Parameters.WorldTransform);

	//
	// Collision vertices down sampling validation.  
	//
	CollisionParticlesPerObjectFraction = Parameters.CollisionSampleFraction * CollisionParticlesPerObjectFractionDefault;
	if (ReportHighParticleFraction > 0)
	{
		for (const FSharedSimulationSizeSpecificData& Data : Parameters.Shared.SizeSpecificData)
		{
			if (ensure(Data.CollisionShapesData.Num()))
			{
				if (Data.CollisionShapesData[0].CollisionParticleData.CollisionParticlesFraction >= ReportHighParticleFraction)
				{
					ensureMsgf(false, TEXT("Collection with small particle fraction"));
					UE_LOG(LogChaos, Warning, TEXT("Collection with small particle fraction(%f):%s"), Data.CollisionShapesData[0].CollisionParticleData.CollisionParticlesFraction, *Parameters.Name);
				}
			}
		}
	}

	// Initialise GT/External particles
	const int32 NumTransforms = DynamicCollection.Transform.Num();

	// Attach the external particles to the gamethread collection
	if (GameThreadCollection.HasAttribute(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup))
	{ 
		GameThreadCollection.RemoveAttribute(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup);
	}
		
	GameThreadCollection.AddExternalAttribute<TUniquePtr<Chaos::FGeometryParticle>>(FGeometryCollection::ParticlesAttribute, FTransformCollection::TransformGroup, GTParticles);
	

	TArray<int32> ChildrenToCheckForParentFix;
	if(ensure(NumTransforms == GameThreadCollection.Implicits.Num() && NumTransforms == GTParticles.Num())) // Implicits are in the transform group so this invariant should always hold
	{
		for(int32 Index = 0; Index < NumTransforms; ++Index)
		{
			GTParticles[Index] = Chaos::FGeometryParticle::CreateParticle();
			Chaos::FGeometryParticle* P = GTParticles[Index].Get();

			GTParticles[Index]->SetUniqueIdx(Evolution->GenerateUniqueIdx());

			const FTransform& T = Parameters.WorldTransform * GameThreadCollection.Transform[Index];
			P->SetX(T.GetTranslation(), false);
			P->SetR(T.GetRotation(), false);
			P->SetUserData(Parameters.UserData);
			P->SetProxy(this);
			P->SetGeometry(GameThreadCollection.Implicits[Index]);

			// this step is necessary for Phase 2 where we need to walk back the hierarchy from children to parent 
			if (bGeometryCollectionAlwaysGenerateGTCollisionForClusters && GameThreadCollection.Children[Index].Num() == 0)
			{
				ChildrenToCheckForParentFix.Add(Index);
			}
			
			// IMPORTANT: we need to set the right spatial index because GT particle is static and PT particle is rigid
			// this is causing a mismatch when using the separate acceleration structures optimization which can cause crashes when destroying the particle while async tracing 
			// todo(chaos) we should eventually refactor this code to use rigid particles on the GT side for geometry collection  
			P->SetSpatialIdx(Chaos::FSpatialAccelerationIdx{ 0,1 });
		}

		if (bGeometryCollectionAlwaysGenerateGTCollisionForClusters)
		{
			// second phase: fixing parent geometries
			// @todo(chaos) this could certainly be done ahead at generation time rather than runtime
			TSet<int32> ParentToPotentiallyFix;
			while (ChildrenToCheckForParentFix.Num())
			{
				// step 1 : find parents
				for(const int32 ChildIndex: ChildrenToCheckForParentFix)
				{
					const int32 ParentIndex = GameThreadCollection.Parent[ChildIndex];
					if (ParentIndex != INDEX_NONE)
					{
						ParentToPotentiallyFix.Add(ParentIndex);
					}
				}

				// step 2: fix the parent if necessary
				for (const int32 ParentToFixIndex: ParentToPotentiallyFix)
				{
					if (GameThreadCollection.Implicits[ParentToFixIndex] == nullptr)
					{
						const Chaos::FRigidTransform3 ParentShapeTransform =  GameThreadCollection.MassToLocal[ParentToFixIndex] * GameThreadCollection.Transform[ParentToFixIndex];
				
						// Make a union of the children geometry
						TArray<TUniquePtr<Chaos::FImplicitObject>> ChildImplicits;
						for (const int32& ChildIndex: GameThreadCollection.Children[ParentToFixIndex])
						{
							using FImplicitObjectTransformed = Chaos::TImplicitObjectTransformed<Chaos::FReal, 3>;

							Chaos::FGeometryParticle* ChildParticle = GTParticles[ChildIndex].Get();
							const FGeometryDynamicCollection::FSharedImplicit& ChildImplicit = GameThreadCollection.Implicits[ChildIndex];
							if (ChildImplicit)
							{
								const Chaos::FRigidTransform3 ChildShapeTransform =  GameThreadCollection.MassToLocal[ChildIndex] * GameThreadCollection.Transform[ChildIndex];
								const Chaos::FRigidTransform3 RelativeShapeTransform = ChildShapeTransform.GetRelativeTransform(ParentShapeTransform);
						
								// assumption that we only have can only have one level of union for any child
								if (ChildImplicit->GetType() == Chaos::ImplicitObjectType::Union)
								{
									if (Chaos::FImplicitObjectUnion* Union = ChildImplicit->GetObject<Chaos::FImplicitObjectUnion>())
									{
										for (const TUniquePtr<Chaos::FImplicitObject>& ImplicitObject : Union->GetObjects())
										{
											TUniquePtr<Chaos::FImplicitObject> CopiedChildImplicit = ImplicitObject->DeepCopy();
											FImplicitObjectTransformed* TransformedChildImplicit = new FImplicitObjectTransformed(MoveTemp(CopiedChildImplicit), RelativeShapeTransform);  
											ChildImplicits.Add(TUniquePtr<FImplicitObjectTransformed>(TransformedChildImplicit));
										}
									}
								}
								else
								{
									TUniquePtr<Chaos::FImplicitObject> CopiedChildImplicit = GameThreadCollection.Implicits[ChildIndex]->DeepCopy();
									FImplicitObjectTransformed* TransformedChildImplicit = new FImplicitObjectTransformed(MoveTemp(CopiedChildImplicit), RelativeShapeTransform);  
									ChildImplicits.Add(TUniquePtr<FImplicitObjectTransformed>(TransformedChildImplicit));
								}
							}
						}
						if (ChildImplicits.Num() > 0)
						{
							Chaos::FImplicitObject* UnionImplicit = new Chaos::FImplicitObjectUnion(MoveTemp(ChildImplicits));
							GameThreadCollection.Implicits[ParentToFixIndex] = FGeometryDynamicCollection::FSharedImplicit(UnionImplicit);
						}
						GTParticles[ParentToFixIndex]->SetGeometry(GameThreadCollection.Implicits[ParentToFixIndex]);
					}
				}

				// step 3 : make the parent the new child to go up the hierarchy and continue the fixing
				ChildrenToCheckForParentFix = ParentToPotentiallyFix.Array(); 
				ParentToPotentiallyFix.Reset();
			}
		}
		
		// Phase 3 : finalization of shapes
		for(int32 Index = 0; Index < NumTransforms; ++Index)
		{
			Chaos::FGeometryParticle* P = GTParticles[Index].Get();
			const Chaos::FShapesArray& Shapes = P->ShapesArray();
			const int32 NumShapes = Shapes.Num();
			for(int32 ShapeIndex = 0; ShapeIndex < NumShapes; ++ShapeIndex)
			{
				Chaos::FPerShapeData* Shape = Shapes[ShapeIndex].Get();
				Shape->SetSimData(SimFilter);
				Shape->SetQueryData(QueryFilter);
				Shape->SetProxy(this);
				Shape->SetMaterial(Parameters.PhysicalMaterialHandle);
			}
		}
	}

	// Skip simplicials, as they're owned by unique pointers.
	TMap<FName, TSet<FName>> SkipList;
	TSet<FName>& TransformGroupSkipList = SkipList.Emplace(FTransformCollection::TransformGroup);
	TransformGroupSkipList.Add(DynamicCollection.SimplicialsAttribute);

	PhysicsThreadCollection.CopyMatchingAttributesFrom(DynamicCollection, &SkipList);

	// Copy simplicials.
	// TODO: Ryan - Should we just transfer ownership of the SimplicialsAttribute from the DynamicCollection to
	// the PhysicsThreadCollection?
	{
		if (DynamicCollection.HasAttribute(DynamicCollection.SimplicialsAttribute, FTransformCollection::TransformGroup))
		{
			const auto& SourceSimplicials = DynamicCollection.GetAttribute<TUniquePtr<FSimplicial>>(
				DynamicCollection.SimplicialsAttribute, FTransformCollection::TransformGroup);
			for (int32 Index = PhysicsThreadCollection.NumElements(FTransformCollection::TransformGroup) - 1; 0 <= Index; Index--)
			{
				PhysicsThreadCollection.Simplicials[Index].Reset(
					SourceSimplicials[Index] ? SourceSimplicials[Index]->NewCopy() : nullptr);
			}
		}
		else
		{
			for (int32 Index = PhysicsThreadCollection.NumElements(FTransformCollection::TransformGroup) - 1; 0 <= Index; Index--)
			{
				PhysicsThreadCollection.Simplicials[Index].Reset();
			}
		}
	}
}


void FGeometryCollectionPhysicsProxy::InitializeDynamicCollection(FGeometryDynamicCollection& DynamicCollection, const FGeometryCollection& RestCollection, const FSimulationParameters& Params)
{
	// @todo(GCCollisionShapes) : add support for multiple shapes, currently just one. 

	// 
	// This function will use the rest collection to populate the dynamic collection. 
	//

	TMap<FName, TSet<FName>> SkipList;
	TSet<FName>& KeepFromDynamicCollection = SkipList.Emplace(FTransformCollection::TransformGroup);
	KeepFromDynamicCollection.Add(FTransformCollection::TransformAttribute);
	KeepFromDynamicCollection.Add(FTransformCollection::ParentAttribute);
	KeepFromDynamicCollection.Add(FTransformCollection::ChildrenAttribute);
	KeepFromDynamicCollection.Add(FGeometryCollection::SimulationTypeAttribute);
	KeepFromDynamicCollection.Add(DynamicCollection.SimplicialsAttribute);
	KeepFromDynamicCollection.Add(DynamicCollection.ActiveAttribute);
	KeepFromDynamicCollection.Add(DynamicCollection.CollisionGroupAttribute);
	DynamicCollection.CopyMatchingAttributesFrom(RestCollection, &SkipList);


	//
	// User defined initial velocities need to be populated. 
	//
	{
		if (Params.InitialVelocityType == EInitialVelocityTypeEnum::Chaos_Initial_Velocity_User_Defined)
		{
			DynamicCollection.InitialLinearVelocity.Fill(FVector3f(Params.InitialLinearVelocity));
			DynamicCollection.InitialAngularVelocity.Fill(FVector3f(Params.InitialAngularVelocity));
		}
	}

	// process simplicials
	{
		// CVar defined in BodyInstance but pertinent here as we will need to copy simplicials in the case that this is set.
		// Original CVar is read-only so taking a static ptr here is fine as the value cannot be changed
		static IConsoleVariable* AnalyticDisableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("p.IgnoreAnalyticCollisionsOverride"));
		static const bool bAnalyticsDisabled = (AnalyticDisableCVar && AnalyticDisableCVar->GetBool());

		if (RestCollection.HasAttribute(DynamicCollection.SimplicialsAttribute, FTransformCollection::TransformGroup)
			&& Params.Shared.SizeSpecificData[0].CollisionShapesData.Num()
			&& (Params.Shared.SizeSpecificData[0].CollisionShapesData[0].CollisionType == ECollisionTypeEnum::Chaos_Surface_Volumetric || bAnalyticsDisabled))
		{
			const auto& RestSimplicials = RestCollection.GetAttribute<TUniquePtr<FSimplicial>>(
				DynamicCollection.SimplicialsAttribute, FTransformCollection::TransformGroup);
			for (int32 Index = DynamicCollection.NumElements(FTransformCollection::TransformGroup) - 1; 0 <= Index; Index--)
			{
				DynamicCollection.Simplicials[Index].Reset(
					RestSimplicials[Index] ? RestSimplicials[Index]->NewCopy() : nullptr);
			}
		}
		else
		{
			for (int32 Index = DynamicCollection.NumElements(FTransformCollection::TransformGroup) - 1; 0 <= Index; Index--)
			{
				DynamicCollection.Simplicials[Index].Reset();
			}
		}
	}

	// Process Activity
	{
		const int32 NumTransforms = DynamicCollection.SimulatableParticles.Num();
		if (!RestCollection.HasAttribute(FGeometryCollection::SimulatableParticlesAttribute, FTransformCollection::TransformGroup))
		{
			// If no simulation data is available then default to the simulation of just the rigid geometry.
			for (int32 TransformIdx = 0; TransformIdx < NumTransforms; TransformIdx++)
			{
				if (DynamicCollection.Children[TransformIdx].Num())
				{
					DynamicCollection.SimulatableParticles[TransformIdx] = false;
				}
				else
				{
					DynamicCollection.SimulatableParticles[TransformIdx] = DynamicCollection.Active[TransformIdx];
				}
			}
		}
	}
}

int32 ReportTooManyChildrenNum = -1;
FAutoConsoleVariableRef CVarReportTooManyChildrenNum(TEXT("p.ReportTooManyChildrenNum"), ReportTooManyChildrenNum, TEXT("Issue warning if more than this many children exist in a single cluster"));

void FGeometryCollectionPhysicsProxy::InitializeBodiesPT(Chaos::FPBDRigidsSolver* RigidsSolver, typename Chaos::FPBDRigidsSolver::FParticlesType& Particles)
{
	const FGeometryCollection* RestCollection = Parameters.RestCollection;
	const FGeometryDynamicCollection& DynamicCollection = PhysicsThreadCollection;

	if (Parameters.Simulating)
	{
		const TManagedArray<int32>& TransformIndex = RestCollection->TransformIndex;
		const TManagedArray<int32>& BoneMap = RestCollection->BoneMap;
		const TManagedArray<int32>& SimulationType = RestCollection->SimulationType;
		const TManagedArray<FVector3f>& Vertex = RestCollection->Vertex;
		const TManagedArray<float>& Mass = RestCollection->GetAttribute<float>("Mass", FTransformCollection::TransformGroup);
		const TManagedArray<FVector3f>& InertiaTensor = RestCollection->GetAttribute<FVector3f>("InertiaTensor", FTransformCollection::TransformGroup);

		const int32 NumTransforms = DynamicCollection.NumElements(FTransformCollection::TransformGroup);
		const TManagedArray<int32>& DynamicState = DynamicCollection.DynamicState;
		const TManagedArray<int32>& CollisionGroup = DynamicCollection.CollisionGroup;
		const TManagedArray<bool>& SimulatableParticles = DynamicCollection.SimulatableParticles;
		const TManagedArray<FTransform>& MassToLocal = DynamicCollection.MassToLocal;
		const TManagedArray<FVector3f>& InitialAngularVelocity = DynamicCollection.InitialAngularVelocity;
		const TManagedArray<FVector3f>& InitialLinearVelocity = DynamicCollection.InitialLinearVelocity;
		const TManagedArray<FGeometryDynamicCollection::FSharedImplicit>& Implicits = DynamicCollection.Implicits;
		const TManagedArray<TUniquePtr<FCollisionStructureManager::FSimplicial>>& Simplicials = DynamicCollection.Simplicials;
		const TManagedArray<TSet<int32>>& Children = DynamicCollection.Children;
		const TManagedArray<int32>& Parent = DynamicCollection.Parent;

		TArray<FTransform> Transform;
		GeometryCollectionAlgo::GlobalMatrices(DynamicCollection.Transform, Parent, Transform);

		//const int NumRigids = 0; // ryan - Since we're doing SOA, we start at zero?
		int NumRigids = 0;
		BaseParticleIndex = NumRigids;

		// Gather unique indices from GT to pass into PT handle creation
		TArray<Chaos::FUniqueIdx> UniqueIndices;
		UniqueIndices.Reserve(SimulatableParticles.Num());

		// Count geometry collection leaf node particles to add
		int NumSimulatedParticles = 0;
		for (int32 Idx = 0; Idx < SimulatableParticles.Num(); ++Idx)
		{
			NumSimulatedParticles += SimulatableParticles[Idx];
			if (SimulatableParticles[Idx] && !RestCollection->IsClustered(Idx) && RestCollection->IsGeometry(Idx))
			{
				NumRigids++;
				UniqueIndices.Add(GTParticles[Idx]->UniqueIdx());
			}
		}

		// Add entries into simulation array
		RigidsSolver->GetEvolution()->ReserveParticles(NumSimulatedParticles);
		TArray<Chaos::TPBDGeometryCollectionParticleHandle<Chaos::FReal, 3>*> Handles = RigidsSolver->GetEvolution()->CreateGeometryCollectionParticles(NumRigids, UniqueIndices.GetData());

		int32 NextIdx = 0;
		for (int32 Idx = 0; Idx < SimulatableParticles.Num(); ++Idx)
		{
			SolverParticleHandles[Idx] = nullptr;
			if (SimulatableParticles[Idx] && !RestCollection->IsClustered(Idx))
			{
				// todo: Unblocked read access of game thread data on the physics thread.

				Chaos::TPBDGeometryCollectionParticleHandle<Chaos::FReal, 3>* Handle = Handles[NextIdx++];

				Handle->SetPhysicsProxy(this);

				SolverParticleHandles[Idx] = Handle;
				HandleToTransformGroupIndex.Add(Handle, Idx);

				// We're on the physics thread here but we've already set up the GT particles and we're just linking here
				Handle->GTGeometryParticle() = GTParticles[Idx].Get();

				check(SolverParticleHandles[Idx]->GetParticleType() == Handle->GetParticleType());
				RigidsSolver->GetEvolution()->CreateParticle(Handle);
			}
		}

		const float StrainDefault = Parameters.DamageThreshold.Num() ? Parameters.DamageThreshold[0] : 0;
		// Add the rigid bodies

		const FVector WorldScale = Parameters.WorldTransform.GetScale3D();
		const FVector::FReal MassScale = WorldScale.X * WorldScale.Y * WorldScale.Z;
		
		// Iterating over the geometry group is a fast way of skipping everything that's
		// not a leaf node, as each geometry has a transform index, which is a shortcut
		// for the case when there's a 1-to-1 mapping between transforms and geometries.
		// At the point that we start supporting instancing, this assumption will no longer
		// hold, and those reverse mappints will be INDEX_NONE.
		ParallelFor(NumTransforms, [&](int32 TransformGroupIndex)
		{
			if (FClusterHandle* Handle = SolverParticleHandles[TransformGroupIndex])
			{
				// Mass space -> Composed parent space -> world
				const FTransform WorldTransform = 
					MassToLocal[TransformGroupIndex] * Transform[TransformGroupIndex] * Parameters.WorldTransform;

				const Chaos::TVec3<float> ScaledInertia = Chaos::Utilities::ScaleInertia<float>((Chaos::TVec3<float>)InertiaTensor[TransformGroupIndex], (Chaos::TVec3<float>)(WorldScale), true);
				
				PopulateSimulatedParticle(
					Handle,
					Parameters.Shared,
					Simplicials[TransformGroupIndex].Get(),
					Implicits[TransformGroupIndex],
					SimFilter,
					QueryFilter,
					Mass[TransformGroupIndex] * MassScale,
					ScaledInertia,
					WorldTransform,
					static_cast<uint8>(DynamicState[TransformGroupIndex]),
					static_cast<int16>(CollisionGroup[TransformGroupIndex]),
					CollisionParticlesPerObjectFraction);

				if (Parameters.EnableClustering)
				{
					Handle->SetClusterGroupIndex(Parameters.ClusterGroupIndex);
					Handle->SetStrain(StrainDefault);
				}

				// #BGTODO - non-updating parameters - remove lin/ang drag arrays and always query material if this stays a material parameter
				Chaos::FChaosPhysicsMaterial* SolverMaterial = RigidsSolver->GetSimMaterials().Get(Parameters.PhysicalMaterialHandle.InnerHandle);
				if(SolverMaterial)
				{
					Handle->SetLinearEtherDrag(SolverMaterial->LinearEtherDrag);
					Handle->SetAngularEtherDrag(SolverMaterial->AngularEtherDrag);
				}

				const Chaos::FShapesArray& Shapes = Handle->ShapesArray();
				for(const TUniquePtr<Chaos::FPerShapeData>& Shape : Shapes)
				{
					Shape->SetMaterial(Parameters.PhysicalMaterialHandle);
				}
			}
		},true);

		// After population, the states of each particle could have changed
		Particles.UpdateGeometryCollectionViews();

		for (FFieldSystemCommand& Cmd : Parameters.InitializationCommands)
		{
			if(Cmd.MetaData.Contains(FFieldSystemMetaData::EMetaType::ECommandData_ProcessingResolution))
			{
				Cmd.MetaData.Remove(FFieldSystemMetaData::EMetaType::ECommandData_ProcessingResolution);
			}

			FFieldSystemMetaDataProcessingResolution* ResolutionData = new FFieldSystemMetaDataProcessingResolution(EFieldResolutionType::Field_Resolution_Maximum);

			Cmd.MetaData.Add( FFieldSystemMetaData::EMetaType::ECommandData_ProcessingResolution, TUniquePtr<FFieldSystemMetaDataProcessingResolution>(ResolutionData));
			Commands.Add(Cmd);
		}
		Parameters.InitializationCommands.Empty();
		FieldParameterUpdateCallback(RigidsSolver, false);

		if (Parameters.InitialVelocityType == EInitialVelocityTypeEnum::Chaos_Initial_Velocity_User_Defined)
		{
			// A previous implementation of this went wide on this loop.  The general 
			// rule of thumb for parallelization is that each thread needs at least
			// 1000 operations in order to overcome the expense of threading.  I don't
			// think that's generally going to be the case here...
			for (int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransforms; ++TransformGroupIndex)
			{
				if (Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* Handle = SolverParticleHandles[TransformGroupIndex])
				{
					if (DynamicState[TransformGroupIndex] == (int32)EObjectStateTypeEnum::Chaos_Object_Dynamic)
					{
						Handle->SetV(InitialLinearVelocity[TransformGroupIndex]);
						Handle->SetW(InitialAngularVelocity[TransformGroupIndex]);
					}
				}
			}
		}

#if TODO_REIMPLEMENT_FRACTURE
		InitializeRemoveOnFracture(Particles, DynamicState);
#endif // TODO_REIMPLEMENT_FRACTURE

		// #BG Temporary - don't cluster when playing back. Needs to be changed when kinematics are per-proxy to support
		// kinematic to dynamic transition for clusters.
		if (Parameters.EnableClustering)// && Parameters.CacheType != EGeometryCollectionCacheType::Play)
		{
			// "RecursiveOrder" means bottom up - children come before their parents.
			const TArray<int32> RecursiveOrder = GeometryCollectionAlgo::ComputeRecursiveOrder(*RestCollection);

			// Propagate simulated particle flags up the hierarchy from children 
			// to their parents, grandparents, etc...
			TArray<bool> SubTreeContainsSimulatableParticle;
			SubTreeContainsSimulatableParticle.SetNumZeroed(RecursiveOrder.Num());
			for (const int32 TransformGroupIndex : RecursiveOrder)
			{
				if (SimulatableParticles[TransformGroupIndex] && !RestCollection->IsClustered(TransformGroupIndex))

				{
					// Rigid node
					SubTreeContainsSimulatableParticle[TransformGroupIndex] =
						SolverParticleHandles[TransformGroupIndex] != nullptr;
				}
				else
				{
					// Cluster parent
					const TSet<int32>& ChildIndices = Children[TransformGroupIndex];
					for (const int32 ChildIndex : ChildIndices)
					{
						if(SubTreeContainsSimulatableParticle[ChildIndex])
						{
							SubTreeContainsSimulatableParticle[TransformGroupIndex] = true;
							break;
						}
					}
				}
			}

			TArray<Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>*> ClusterHandles;
			// Ryan - It'd be better to batch allocate cluster particles ahead of time,
			// but if ClusterHandles is empty, then new particles will be allocated
			// on the fly by TPBDRigidClustering::CreateClusterParticle(), which 
			// needs to work before this does...
			//ClusterHandles = GetSolver()->GetEvolution()->CreateClusteredParticles(NumClusters);

			int32 ClusterHandlesIndex = 0;
			TArray<Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>*> RigidChildren;
			TArray<int32> RigidChildrenTransformGroupIndex;
			for (const int32 TransformGroupIndex : RecursiveOrder)
			{
				// Don't construct particles for branches of the hierarchy that  
				// don't contain any simulated particles.
				if (!SubTreeContainsSimulatableParticle[TransformGroupIndex])
				{
					continue;
				}

				RigidChildren.Reset(Children.Num());
				RigidChildrenTransformGroupIndex.Reset(Children.Num());
				for (const int32 ChildIndex : Children[TransformGroupIndex])
				{
					if (Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* Handle = SolverParticleHandles[ChildIndex])
					{
						RigidChildren.Add(Handle);
						RigidChildrenTransformGroupIndex.Add(ChildIndex);
					}
				}

				if (RigidChildren.Num())
				{
					if (ReportTooManyChildrenNum >= 0 && RigidChildren.Num() > ReportTooManyChildrenNum)
					{
						UE_LOG(LogChaos, Warning, TEXT("Too many children (%d) in a single cluster:%s"), 
							RigidChildren.Num(), *Parameters.Name);
					}

					Chaos::FClusterCreationParameters CreationParameters;
					CreationParameters.ClusterParticleHandle = ClusterHandles.Num() ? ClusterHandles[ClusterHandlesIndex++] : nullptr;
					CreationParameters.Scale = Parameters.WorldTransform.GetScale3D();

					// Hook the handle up with the GT particle
					Chaos::FGeometryParticle* GTParticle = GTParticles[TransformGroupIndex].Get();

					Chaos::FUniqueIdx ExistingIndex = GTParticle->UniqueIdx();
					Chaos::FPBDRigidClusteredParticleHandle* Handle = BuildClusters(TransformGroupIndex, RigidChildren, RigidChildrenTransformGroupIndex, CreationParameters, &ExistingIndex);
					Handle->GTGeometryParticle() = GTParticle;

					int32 RigidChildrenIdx = 0;
					for(const int32 ChildTransformIndex : RigidChildrenTransformGroupIndex)
					{
						SolverClusterID[ChildTransformIndex] = RigidChildren[RigidChildrenIdx++]->CastToClustered()->ClusterIds().Id;;
					}
					SolverClusterID[TransformGroupIndex] = Handle->ClusterIds().Id;					

					SolverClusterHandles[TransformGroupIndex] = Handle;
					SolverParticleHandles[TransformGroupIndex] = Handle;
					HandleToTransformGroupIndex.Add(Handle, TransformGroupIndex);
					Handle->SetPhysicsProxy(this);

					// Dirty for SQ
					RigidsSolver->GetEvolution()->DirtyParticle(*Handle);

					// If we're not simulating we would normally not write any results back to the game thread.
					// This will force a single write in this case because we've updated the transform on the cluster
					// and it should be updated on the game thread also
					// #TODO Consider building this information at edit-time / offline
					if(!Parameters.Simulating)
					{
						RigidsSolver->GetEvolution()->GetParticles().MarkTransientDirtyParticle(Handle);
					}
				}
			}

			// We've likely changed the state of leaf nodes, which are geometry
			// collection particles.  Update which particle views they belong in,
			// as well as views of clustered particles.
			Particles.UpdateGeometryCollectionViews(true); 

			// Set cluster connectivity.  TPBDRigidClustering::CreateClusterParticle() 
			// will optionally do this, but we switch that functionality off in BuildClusters().
			for(int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransforms; ++TransformGroupIndex)
			{
				if (RestCollection->IsClustered(TransformGroupIndex))
				{
					if (SolverClusterHandles[TransformGroupIndex])
					{
						Chaos::FClusterCreationParameters ClusterParams;
						// #todo: should other parameters be set here?  Previously, there was no parameters being sent, and it is unclear
						// where some of these parameters are defined (ie: CoillisionThicknessPercent)
						ClusterParams.ConnectionMethod = Parameters.ClusterConnectionMethod;
						
						RigidsSolver->GetEvolution()->GetRigidClustering().GenerateConnectionGraph(SolverClusterHandles[TransformGroupIndex], ClusterParams);
					}
				}
			}
		} // end if EnableClustering
 

#if TODO_REIMPLEMENT_RIGID_CACHING
		// If we're recording and want to start immediately caching then we should cache the rest state
		if (Parameters.IsCacheRecording() && Parameters.CacheBeginTime == 0.0f)
		{
			if (UpdateRecordedStateCallback)
			{
				UpdateRecordedStateCallback(0.0f, RigidBodyID, Particles, RigidSolver->GetCollisionConstraints());
			}
		}
#endif // TODO_REIMPLEMENT_RIGID_CACHING



		if (DisableGeometryCollectionGravity) // cvar
		{
			// Our assumption is that you'd only ever want to wholesale opt geometry 
			// collections out of gravity for debugging, so we keep this conditional
			// out of the loop above and on it's own.  This means we can't turn gravity
			// back on once it's off, but even if we didn't enclose this in an if(),
			// this function won't be called again unless something dirties the proxy.

			Chaos::FPerParticleGravity& GravityForces = RigidsSolver->GetEvolution()->GetGravityForces();
			for (int32 HandleIdx = 0; HandleIdx < SolverParticleHandles.Num(); ++HandleIdx)
			{
				if (Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* Handle = SolverParticleHandles[HandleIdx])
				{
					Handle->SetGravityEnabled(false);
				}
			}
		}

		// call DirtyParticle to make sure the acceleration structure is up to date with all the changes happening here
		for (int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransforms; ++TransformGroupIndex)
		{
			if (FClusterHandle* Handle = SolverParticleHandles[TransformGroupIndex])
			{
				// Sleeping Geometry Collections:
				//   A sleeping geometry collection is dynamic internally, and then the top level
				//   active clusters are set to sleeping. Sleeping is not propagated up from the 
				//   leaf nodes like kinematic or dynamic clusters. 
				if (!Handle->Disabled() && Parameters.ObjectType == EObjectStateTypeEnum::Chaos_Object_Sleeping)
				{
					RigidsSolver->GetEvolution()->SetParticleObjectState(Handle, Chaos::EObjectStateType::Sleeping);
				}

				RigidsSolver->GetEvolution()->DirtyParticle(*Handle);
			}
		}

	} // end if simulating...

}

int32 ReportNoLevelsetCluster = 0;
FAutoConsoleVariableRef CVarReportNoLevelsetCluster(TEXT("p.gc.ReportNoLevelsetCluster"), ReportNoLevelsetCluster, TEXT("Report any cluster objects without levelsets"));

DECLARE_CYCLE_STAT(TEXT("FGeometryCollectionPhysicsProxy::BuildClusters"), STAT_BuildClusters, STATGROUP_Chaos);
DECLARE_CYCLE_STAT(TEXT("FGeometryCollectionPhysicsProxy::BuildClusters:GlobalMatrices"), STAT_BuildClustersGlobalMatrices, STATGROUP_Chaos);



Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>*
FGeometryCollectionPhysicsProxy::BuildClusters(
	const uint32 CollectionClusterIndex, // TransformGroupIndex
	TArray<Chaos::TPBDRigidParticleHandle<Chaos::FReal,3>*>& ChildHandles,
	const TArray<int32>& ChildTransformGroupIndices,
	const Chaos::FClusterCreationParameters & ClusterParameters,
	const Chaos::FUniqueIdx* ExistingIndex)
{
	SCOPE_CYCLE_COUNTER(STAT_BuildClusters);

	check(CollectionClusterIndex != INDEX_NONE);
	check(ChildHandles.Num() != 0);

	FGeometryDynamicCollection& DynamicCollection = PhysicsThreadCollection;
	TManagedArray<int32>& DynamicState = DynamicCollection.DynamicState;
	TManagedArray<int32>& ParentIndex = DynamicCollection.Parent;
	TManagedArray<TSet<int32>>& Children = DynamicCollection.Children;
	TManagedArray<FTransform>& Transform = DynamicCollection.Transform;
	TManagedArray<FTransform>& MassToLocal = DynamicCollection.MassToLocal;
	//TManagedArray<TSharedPtr<FCollisionStructureManager::FSimplicial> >& Simplicials = DynamicCollection.Simplicials;
	TManagedArray<FGeometryDynamicCollection::FSharedImplicit>& Implicits = DynamicCollection.Implicits;

	//If we are a root particle use the world transform, otherwise set the relative transform
	const FTransform CollectionSpaceTransform = GeometryCollectionAlgo::GlobalMatrix(Transform, ParentIndex, CollectionClusterIndex);
	const Chaos::TRigidTransform<Chaos::FReal, 3> ParticleTM = MassToLocal[CollectionClusterIndex] * CollectionSpaceTransform * Parameters.WorldTransform;

	//create new cluster particle
	//The reason we need to pass in a mass orientation override is as follows:
	//Consider a pillar made up of many boxes along the Y-axis. In this configuration we could generate a proxy pillar along the Y with identity rotation.
	//Now if we instantiate the pillar and rotate it so that it is along the X-axis, we would still like to use the same pillar proxy.
	//Since the mass orientation is computed in world space in both cases we'd end up with a diagonal inertia matrix and identity rotation that looks like this: [big, small, big] or [small, big, big].
	//Because of this we need to know how to rotate collision particles and geometry to match with original computation. If it was just geometry we could transform it before passing, but we need collision particles as well
	Chaos::FClusterCreationParameters ClusterCreationParameters = ClusterParameters;
	ClusterCreationParameters.bGenerateConnectionGraph = true;
	// fix... ClusterCreationParameters.CollisionParticles = Simplicials[CollectionClusterIndex];
	ClusterCreationParameters.ConnectionMethod = Parameters.ClusterConnectionMethod;
	if (ClusterCreationParameters.CollisionParticles)
	{
		const Chaos::FReal NumCollisionParticles = static_cast<Chaos::FReal>(ClusterCreationParameters.CollisionParticles->Size());
		const int32 ClampedCollisionParticlesSize = 
			FMath::TruncToInt32(Chaos::FReal(FMath::Max(0, FMath::Min(NumCollisionParticles * CollisionParticlesPerObjectFraction, NumCollisionParticles))));
		ClusterCreationParameters.CollisionParticles->Resize(ClampedCollisionParticlesSize);
	}
	TArray<Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>*> ChildHandlesCopy(ChildHandles);

	// Construct an active cluster particle, disable children, derive M and I from children:
	Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* Parent =
		static_cast<Chaos::FPBDRigidsSolver*>(Solver)->GetEvolution()->GetRigidClustering().CreateClusterParticle(
			Parameters.ClusterGroupIndex, 
			MoveTemp(ChildHandlesCopy),
			ClusterCreationParameters,
			Implicits[CollectionClusterIndex], // union from children if null
			&ParticleTM,
			ExistingIndex
			);

	if (ReportNoLevelsetCluster && 
		Parent->DynamicGeometry())
	{
		//ensureMsgf(false, TEXT("Union object generated for cluster"));
		UE_LOG(LogChaos, Warning, TEXT("Union object generated for cluster:%s"), *Parameters.Name);
	}

	if (Parent->InvM() == 0.0)
	{
		if (Parent->ObjectState() == Chaos::EObjectStateType::Static)
		{
			DynamicState[CollectionClusterIndex] = (uint8)EObjectStateTypeEnum::Chaos_Object_Static;
		}
		else //if (Particles.ObjectState(NewSolverClusterID) == Chaos::EObjectStateType::Kinematic)
		{
			DynamicState[CollectionClusterIndex] = (uint8)EObjectStateTypeEnum::Chaos_Object_Kinematic;
		}
	}

	check(Parameters.RestCollection);
	const TManagedArray<Chaos::FReal>& Mass =
		Parameters.RestCollection->GetAttribute<Chaos::FReal>("Mass", FTransformCollection::TransformGroup);
	const TManagedArray<FVector3f>& InertiaTensor = 
		Parameters.RestCollection->GetAttribute<FVector3f>("InertiaTensor", FTransformCollection::TransformGroup);

	const FVector WorldScale = Parameters.WorldTransform.GetScale3D();
	const FVector::FReal MassScale = WorldScale.X * WorldScale.Y * WorldScale.Z;
	const Chaos::TVec3<float> ScaledInertia = Chaos::Utilities::ScaleInertia<float>((Chaos::TVec3<float>)InertiaTensor[CollectionClusterIndex], FVector3f(WorldScale), true);
	
	PopulateSimulatedParticle(
		Parent,
		Parameters.Shared, 
		nullptr, // CollisionParticles is optionally set from CreateClusterParticle()
		nullptr, // Parent->Geometry() ? Parent->Geometry() : Implicits[CollectionClusterIndex], 
		SimFilter,
		QueryFilter,
		Parent->M() > 0.0 ? Parent->M() : Mass[CollectionClusterIndex] * MassScale, 
		Parent->I() != Chaos::TVec3<float>(0.0) ? Parent->I() : ScaledInertia,
		ParticleTM, 
		(uint8)DynamicState[CollectionClusterIndex], 
		0,
		CollisionParticlesPerObjectFraction); // CollisionGroup

	// two-way mapping
	SolverClusterHandles[CollectionClusterIndex] = Parent;

	const int32 NumThresholds = Parameters.DamageThreshold.Num();
	const int32 Level = FMath::Clamp(CalculateHierarchyLevel(DynamicCollection, CollectionClusterIndex), 0, INT_MAX);
	const float DefaultDamage = NumThresholds > 0 ? Parameters.DamageThreshold[NumThresholds - 1] : 0.f;
	float Damage = Level < NumThresholds ? Parameters.DamageThreshold[Level] : DefaultDamage;

	if(Level >= Parameters.MaxClusterLevel)
	{
		Damage = FLT_MAX;
	}

	if (Parameters.bUseSizeSpecificDamageThresholds)
	{
		// If RelativeSize is available, use that to determine SizeSpecific index, otherwise, fall back to bounds volume.
		int32 SizeSpecificIdx = 0;
		if (Parameters.RestCollection->HasAttribute("Size", FTransformCollection::TransformGroup))
		{
			const TManagedArray<float>& RelativeSize = Parameters.RestCollection->GetAttribute<float>("Size", FTransformCollection::TransformGroup);
			SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(Parameters.Shared.SizeSpecificData, RelativeSize[CollectionClusterIndex]);
		}
		else
		{
			const TManagedArray<FGeometryDynamicCollection::FSharedImplicit>& Implicit = DynamicCollection.Implicits;
			if (Implicit[CollectionClusterIndex] && Implicit[CollectionClusterIndex]->HasBoundingBox())
			{
				FBox LocalBoundingBox(Implicit[CollectionClusterIndex]->BoundingBox().Min(), Implicit[CollectionClusterIndex]->BoundingBox().Max());
				SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(Parameters.Shared.SizeSpecificData, LocalBoundingBox);
			}
		}

		if (0 <= SizeSpecificIdx && SizeSpecificIdx < Parameters.Shared.SizeSpecificData.Num())
		{
			const FSharedSimulationSizeSpecificData& SizeSpecificData = Parameters.Shared.SizeSpecificData[SizeSpecificIdx];
			Damage = SizeSpecificData.DamageThreshold;
		}
	}

	Parent->SetStrains(Damage);


	// #BGTODO This will not automatically update - material properties should only ever exist in the material, not in other arrays
	const Chaos::FChaosPhysicsMaterial* CurMaterial = static_cast<Chaos::FPBDRigidsSolver*>(Solver)->GetSimMaterials().Get(Parameters.PhysicalMaterialHandle.InnerHandle);
	if(CurMaterial)
	{
		Parent->SetLinearEtherDrag(CurMaterial->LinearEtherDrag);
		Parent->SetAngularEtherDrag(CurMaterial->AngularEtherDrag);
	}

	const Chaos::FShapesArray& Shapes = Parent->ShapesArray();
	for(const TUniquePtr<Chaos::FPerShapeData>& Shape : Shapes)
	{
		Shape->SetMaterial(Parameters.PhysicalMaterialHandle);
	}

	const FTransform ParentTransform = GeometryCollectionAlgo::GlobalMatrix(DynamicCollection.Transform, DynamicCollection.Parent, CollectionClusterIndex);

	int32 MinCollisionGroup = INT_MAX;
	for(int32 Idx=0; Idx < ChildHandles.Num(); Idx++)
	{
		Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* Child = ChildHandles[Idx];
		if (Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* ClusteredChild = Child->CastToClustered())
		{
			ClusteredChild->SetStrains(Damage);
		}

		const int32 ChildTransformGroupIndex = ChildTransformGroupIndices[Idx];
		SolverClusterHandles[ChildTransformGroupIndex] = Parent;

		MinCollisionGroup = FMath::Min(Child->CollisionGroup(), MinCollisionGroup);
	}
	Parent->SetCollisionGroup(MinCollisionGroup);

	// Populate bounds as we didn't pass a shared implicit to PopulateSimulatedParticle this will have been skipped, now that we have the full cluster we can build it
	if(Parent->Geometry() && Parent->Geometry()->HasBoundingBox())
	{
		Parent->SetHasBounds(true);
		Parent->SetLocalBounds(Parent->Geometry()->BoundingBox());
		const Chaos::FRigidTransform3 Xf(Parent->X(), Parent->R());
		Parent->UpdateWorldSpaceState(Xf, Chaos::FVec3(0));

		static_cast<Chaos::FPBDRigidsSolver*>(Solver)->GetEvolution()->DirtyParticle(*Parent);
	}

	return Parent;
}

void FGeometryCollectionPhysicsProxy::GetFilteredParticleHandles(
	TArray<Chaos::TGeometryParticleHandle<Chaos::FReal, 3>*>& Handles,
	const Chaos::FPBDRigidsSolver* RigidSolver,
	const EFieldFilterType FilterType,
	const EFieldObjectType ObjectType)
{
	Handles.SetNum(0, false);
	if ((ObjectType == EFieldObjectType::Field_Object_All) || (ObjectType == EFieldObjectType::Field_Object_Destruction) || (ObjectType == EFieldObjectType::Field_Object_Max))
	{
		// only the local handles
		TArray<FClusterHandle*>& ParticleHandles = GetSolverParticleHandles();
		Handles.Reserve(ParticleHandles.Num());

		if (FilterType == EFieldFilterType::Field_Filter_Dynamic)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && (ClusterHandle->ObjectState() == Chaos::EObjectStateType::Dynamic))
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
		else if (FilterType == EFieldFilterType::Field_Filter_Kinematic)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && (ClusterHandle->ObjectState() == Chaos::EObjectStateType::Kinematic))
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
		else if (FilterType == EFieldFilterType::Field_Filter_Static)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && (ClusterHandle->ObjectState() == Chaos::EObjectStateType::Static))
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
		else if (FilterType == EFieldFilterType::Field_Filter_Sleeping)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && (ClusterHandle->ObjectState() == Chaos::EObjectStateType::Sleeping))
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
		else if (FilterType == EFieldFilterType::Field_Filter_Disabled)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && ClusterHandle->Disabled())
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
		else if (FilterType == EFieldFilterType::Field_Filter_All)
		{
			for (FClusterHandle* ClusterHandle : ParticleHandles)
			{
				if (ClusterHandle && (ClusterHandle->ObjectState() != Chaos::EObjectStateType::Uninitialized))
				{
					Handles.Add(ClusterHandle);
				}
			}
		}
	}
}

void FGeometryCollectionPhysicsProxy::GetRelevantParticleHandles(
	TArray<Chaos::TGeometryParticleHandle<Chaos::FReal, 3>*>& Handles,
	const Chaos::FPBDRigidsSolver* RigidSolver, 
	EFieldResolutionType ResolutionType)
{
	Handles.SetNum(0, false);

	// only the local handles
	TArray<FClusterHandle*>& ParticleHandles = GetSolverParticleHandles();
	Handles.Reserve(ParticleHandles.Num());

	if (ResolutionType == EFieldResolutionType::Field_Resolution_Maximum)
	{
		for (FClusterHandle* ClusterHandle : ParticleHandles)
		{
			if (ClusterHandle )
			{
				Handles.Add(ClusterHandle);
			}
		}
	}
	else if (ResolutionType == EFieldResolutionType::Field_Resolution_DisabledParents)
	{
		for (FClusterHandle* ClusterHandle : ParticleHandles)
		{
			if (ClusterHandle && ClusterHandle->ClusterIds().Id == nullptr)
			{
				Handles.Add(ClusterHandle);
			}
		}
	}
	else if (ResolutionType == EFieldResolutionType::Field_Resolution_Minimal)
	{
		const auto& Clustering = RigidSolver->GetEvolution()->GetRigidClustering();
		const auto& ClusterMap = Clustering.GetChildrenMap();

		for (FClusterHandle* ClusterHandle : ParticleHandles)
		{
			if (ClusterHandle && !ClusterHandle->Disabled())
			{
				Handles.Add(ClusterHandle);
				if (ClusterHandle->ClusterIds().NumChildren)
				{
					if (ClusterMap.Contains(ClusterHandle))
					{
						for (Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3> * Child : ClusterMap[ClusterHandle])
						{
							Handles.Add(Child);
						}
					}
				}
			}
		}
	}

#if TODO_REIMPLEMENT_GET_RIGID_PARTICLES
		const Chaos::FPhysicsSolver::FParticlesType & Particles = RigidSolver->GetRigidParticles();
		if (ResolutionType == EFieldResolutionType::Field_Resolution_Minimal)
		{
			const Chaos::TArrayCollectionArray<Chaos::ClusterId>& ClusterIdArray = RigidSolver->GetRigidClustering().GetClusterIdsArray();


			//  Generate a Index mapping between the rigid body indices and 
			//  the particle indices. This allows the geometry collection to
			//  evaluate only its own ACTIVE particles + ClusterChildren
			int32 NumIndices = 0;
			Array.SetNumUninitialized(RigidBodyID.Num());
			for (int32 i = 0; i < RigidBodyID.Num(); i++)
			{
				const int32 RigidBodyIndex = RigidBodyID[i];
				if (RigidBodyIndex != INDEX_NONE && !Particles.Disabled(RigidBodyIndex)) // active bodies
				{
					Array[NumIndices] = { RigidBodyID[i],i };
					NumIndices++;
				}
				if (ClusterIdArray[RigidBodyIndex].Id != INDEX_NONE && !Particles.Disabled(ClusterIdArray[RigidBodyIndex].Id)) // children
				{
					Array[NumIndices] = { RigidBodyID[i],i };
					NumIndices++;
				}
			}
			Array.SetNum(NumIndices);
		}
		else if (ResolutionType == EFieldResolutionType::Field_Resolution_Maximum)
		{
			//  Generate a Index mapping between the rigid body indices and 
			//  the particle indices. This allows the geometry collection to
			//  evaluate only its own particles. 
			int32 NumIndices = 0;
			Array.SetNumUninitialized(RigidBodyID.Num());
			for (int32 i = 0; i < RigidBodyID.Num(); i++)
			{
				const int32 RigidBodyIndex = RigidBodyID[i];
				if (RigidBodyIndex != INDEX_NONE)
				{
					Array[NumIndices] = { RigidBodyIndex, i };
					NumIndices++;
				}
			}
			Array.SetNum(NumIndices);
		}
#endif
}

void FGeometryCollectionPhysicsProxy::DisableParticles(TArray<int32>& TransformGroupIndices)
{
	check(IsInGameThread());

	if (Chaos::FPhysicsSolver* RBDSolver = GetSolver<Chaos::FPhysicsSolver>())
	{
		RBDSolver->EnqueueCommandImmediate([this, RBDSolver, TransformGroupIndices]()
			{
				for (int32 TransformIdx : TransformGroupIndices)
				{
					RBDSolver->GetEvolution()->DisableParticleWithRemovalEvent(SolverParticleHandles[TransformIdx]);
				}
			});
	}
}

int32 FGeometryCollectionPhysicsProxy::CalculateHierarchyLevel(const FGeometryDynamicCollection& GeometryCollection, int32 TransformIndex) const
{
	int32 Level = 0;
	while (GeometryCollection.Parent[TransformIndex] != -1)
	{
		TransformIndex = GeometryCollection.Parent[TransformIndex];
		Level++;
	}
	return Level;
}

void FGeometryCollectionPhysicsProxy::InitializeRemoveOnFracture(FParticlesType& Particles, const TManagedArray<int32>& DynamicState)
{
	/*
	@todo break everything
	if (Parameters.DynamicCollection && Parameters.RemoveOnFractureEnabled)
	{
	//	TManagedArray<FGeometryCollectionBoneNode>& Hierarchy = Parameters.DynamicCollection->BoneHierarchy;

		for (int TransformGroupIndex = 0; TransformGroupIndex < RigidBodyID.Num(); TransformGroupIndex++)
		{
			if (RigidBodyID[TransformGroupIndex] != INDEX_NONE)
			{
				int32 RigidBodyIndex = RigidBodyID[TransformGroupIndex];

				if (Parameters.DynamicCollection->StatusFlags[TransformGroupIndex] & FGeometryCollection::FS_RemoveOnFracture)
				{
					Particles.ToBeRemovedOnFracture(RigidBodyIndex) = true;
				}
			}
		}
	}
	*/
}

void FGeometryCollectionPhysicsProxy::OnRemoveFromSolver(Chaos::FPBDRigidsSolver *RBDSolver)
{
	Chaos::FPBDRigidsEvolutionGBF* Evolution = RBDSolver->GetEvolution();

	TSet< FClusterHandle* > ClustersToRebuild;
	for (int i = 0; i < SolverParticleHandles.Num(); i++)
	{
		if (FClusterHandle* Handle = SolverParticleHandles[i])
		{
			if (FClusterHandle* ParentCluster = Evolution->GetRigidClustering().DestroyClusterParticle(Handle))
			{
				if (ParentCluster->InternalCluster())
				{
					ClustersToRebuild.Add(ParentCluster);
				}
			}
		}
	}

	for (int i = 0; i < SolverParticleHandles.Num(); i++)
	{
		if (FClusterHandle* Handle = SolverParticleHandles[i])
		{
			Chaos::FUniqueIdx UniqueIdx = Handle->UniqueIdx();
			Evolution->DestroyParticle(Handle);
			Evolution->ReleaseUniqueIdx(UniqueIdx);
		}
	}

	for (FClusterHandle* Cluster : ClustersToRebuild)
	{
		ensure(Cluster->InternalCluster());
		if (ensure(Evolution->GetRigidClustering().GetChildrenMap().Contains(Cluster)))
		{
			// copy cluster state for recreation
			int32 ClusterGroupIndex = Cluster->ClusterGroupIndex();
			TArray<FParticleHandle*> Children = Evolution->GetRigidClustering().GetChildrenMap()[Cluster];

			// destroy the invalid cluster
			FClusterHandle* NullHandle = Evolution->GetRigidClustering().DestroyClusterParticle(Cluster);
			ensure(NullHandle == nullptr);

			// create a new cluster if needed
			if (Children.Num())
			{
				if (FClusterHandle* NewParticle = Evolution->GetRigidClustering().CreateClusterParticle(ClusterGroupIndex, MoveTemp(Children)))
				{
					NewParticle->SetInternalCluster(true);
				}
			}
		}		
	}

	IsObjectDeleting = true;
}

void FGeometryCollectionPhysicsProxy::OnRemoveFromScene()
{
#if TODO_REIMPLEMENT_GET_RIGID_PARTICLES
	// #BG TODO This isn't great - we currently cannot handle things being removed from the solver.
	// need to refactor how we handle this and actually remove the particles instead of just constantly
	// growing the array. Currently everything is just tracked by index though so the solver will have
	// to notify all the proxies that a chunk of data was removed - or use a sparse array (undesireable)
	Chaos::FPhysicsSolver::FParticlesType& Particles = GetSolver<FSolver>()->GetRigidParticles();

	// #BG TODO Special case here because right now we reset/realloc the evolution per geom component
	// in endplay which clears this out. That needs to not happen and be based on world shutdown
	if(Particles.Size() == 0)
	{
		return;
	}

	const int32 Begin = BaseParticleIndex;
	const int32 Count = NumParticles;

	if (ensure((int32)Particles.Size() > 0 && (Begin + Count) <= (int32)Particles.Size()))
	{
		for (int32 ParticleIndex = 0; ParticleIndex < Count; ++ParticleIndex)
		{
			GetSolver<FSolver>()->GetEvolution()->DisableParticle(Begin + ParticleIndex);
			GetSolver<FSolver>()->GetRigidClustering().GetTopLevelClusterParents().Remove(Begin + ParticleIndex);
		}
	}
#endif
}

void FGeometryCollectionPhysicsProxy::SyncBeforeDestroy()
{

}

void FGeometryCollectionPhysicsProxy::BufferGameState() 
{
	//
	// There is currently no per advance updates to the GeometryCollection
	//
}


void FGeometryCollectionPhysicsProxy::SetWorldTransform(const FTransform& WorldTransform)
{
	check(IsInGameThread());
	GameThreadPerFrameData.SetWorldTransform(WorldTransform);

	if (Chaos::FPhysicsSolver* RBDSolver = GetSolver<Chaos::FPhysicsSolver>())
	{
		RBDSolver->EnqueueCommandImmediate([this, RBDSolver]()
		{
			RBDSolver->AddDirtyProxy(this);
		});
	}
}

void FGeometryCollectionPhysicsProxy::PushStateOnGameThread(Chaos::FPBDRigidsSolver* InSolver)
{
	// CONTEXT: GAMETHREAD
	// this is running on GAMETHREAD before the PhysicsThread code runs for this frame
	bIsPhysicsThreadWorldTransformDirty = GameThreadPerFrameData.GetIsWorldTransformDirty();
	if (bIsPhysicsThreadWorldTransformDirty)
	{
		Parameters.WorldTransform = GameThreadPerFrameData.GetWorldTransform();
		GameThreadPerFrameData.ResetIsWorldTransformDirty();
	}
}

void FGeometryCollectionPhysicsProxy::PushToPhysicsState()
{
	// CONTEXT: PHYSICSTHREAD
	// because the attached actor can be dynamic, we need to update the kinematic particles properly
	if (bIsPhysicsThreadWorldTransformDirty)
	{
		const FTransform& ActorToWorld = Parameters.WorldTransform;

		// used to avoid doing the work twice if we have a internalCluster parent 
		bool InternalClusterParentUpdated = false;

		int32 NumTransformGroupElements = PhysicsThreadCollection.NumElements(FGeometryCollection::TransformGroup);
		for (int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransformGroupElements; ++TransformGroupIndex)
		{
			Chaos::FPBDRigidClusteredParticleHandle* Handle = SolverParticleHandles[TransformGroupIndex];
			if (Handle)
			{
				if (Handle->ObjectState() == Chaos::EObjectStateType::Kinematic)
				{
					// in the case of cluster union we need to find our Internal Cluster parent and update it
					if (!InternalClusterParentUpdated)
					{
						FClusterHandle* ParentHandle = Handle->Parent();
						if (ParentHandle && ParentHandle->InternalCluster() && !ParentHandle->Disabled() && ParentHandle->ObjectState() == Chaos::EObjectStateType::Kinematic)
						{
							FTransform NewChildWorldTransform = PhysicsThreadCollection.MassToLocal[TransformGroupIndex] * PhysicsThreadCollection.Transform[TransformGroupIndex] * ActorToWorld;
							Chaos::FRigidTransform3 ParentToChildTransform = Handle->ChildToParent().Inverse();
							FTransform NewParentWorldTRansform = ParentToChildTransform * NewChildWorldTransform;
							SetClusteredParticleKinematicTarget(ParentHandle, NewParentWorldTRansform);

							InternalClusterParentUpdated = true;
						}
					}

					if (!Handle->Disabled())
					{
						FTransform WorldTransform = PhysicsThreadCollection.MassToLocal[TransformGroupIndex] * PhysicsThreadCollection.Transform[TransformGroupIndex] * ActorToWorld;
						SetClusteredParticleKinematicTarget(Handle, WorldTransform);
					}
				}
			}
		}
	}
}

void FGeometryCollectionPhysicsProxy::SetClusteredParticleKinematicTarget(Chaos::FPBDRigidClusteredParticleHandle* Handle, const FTransform& NewWorldTransform)
{
	// CONTEXT: PHYSICSTHREAD
	// this should be called only on teh physics thread
	const Chaos::EObjectStateType ObjectState = Handle->ObjectState();
	if (ensure(ObjectState == Chaos::EObjectStateType::Kinematic))
	{
		Chaos::TKinematicTarget<Chaos::FReal, 3> NewKinematicTarget;
		NewKinematicTarget.SetTargetMode(NewWorldTransform);

		if (Chaos::FPhysicsSolver* RBDSolver = GetSolver<Chaos::FPhysicsSolver>())
		{
			RBDSolver->GetEvolution()->SetParticleKinematicTarget(Handle, NewKinematicTarget);
			RBDSolver->GetEvolution()->DirtyParticle(*Handle);
		}
	}
}
void FGeometryCollectionPhysicsProxy::BufferPhysicsResults(Chaos::FPBDRigidsSolver* CurrentSolver, Chaos::FDirtyGeometryCollectionData& BufferData)
{
	/**
	 * CONTEXT: PHYSICSTHREAD
	 * Called per-tick after the simulation has completed. The proxy should cache the results of their
	 * simulation into the local buffer. 
	 */
	using namespace Chaos;
	SCOPE_CYCLE_COUNTER(STAT_CacheResultGeomCollection);
	if (IsObjectDeleting) return;
	BufferData.SetProxy(*this);

	IsObjectDynamic = false;
	FGeometryCollectionResults& TargetResults = BufferData.Results;
	TargetResults.SolverDt = CurrentSolver->GetLastDt();	//todo: should this use timestamp for async mode?

	int32 NumTransformGroupElements = PhysicsThreadCollection.NumElements(FGeometryCollection::TransformGroup);
	if (TargetResults.NumTransformGroup() != NumTransformGroupElements)
	{
		TargetResults.InitArrays(PhysicsThreadCollection);
	}

	const FTransform& ActorToWorld = Parameters.WorldTransform;
	const TManagedArray<int32>& Parent = PhysicsThreadCollection.Parent;
	const TManagedArray<TSet<int32>>& Children = PhysicsThreadCollection.Children;
	const bool IsActorScaled = !ActorToWorld.GetScale3D().Equals(FVector::OneVector);
	const FTransform ActorScaleTransform(FQuat::Identity,  FVector::ZeroVector, ActorToWorld.GetScale3D());

	if(NumTransformGroupElements > 0)
	{ 
		SCOPE_CYCLE_COUNTER(STAT_CalcParticleToWorld);
		
		for (int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransformGroupElements; ++TransformGroupIndex)
		{
			TargetResults.Transforms[TransformGroupIndex] = PhysicsThreadCollection.Transform[TransformGroupIndex];
			TargetResults.Parent[TransformGroupIndex] = PhysicsThreadCollection.Parent[TransformGroupIndex];

			TargetResults.DisabledStates[TransformGroupIndex] = true;
			Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* Handle = SolverParticleHandles[TransformGroupIndex];
			if (!Handle)
			{
				PhysicsThreadCollection.Active[TransformGroupIndex] = !TargetResults.DisabledStates[TransformGroupIndex];
				continue;
			}

			// Dynamic state is also updated by the solver during field interaction.
			if (!Handle->Sleeping())
			{
				const Chaos::EObjectStateType ObjectState = Handle->ObjectState();
				switch (ObjectState)
				{
				case Chaos::EObjectStateType::Kinematic:
					TargetResults.DynamicState[TransformGroupIndex] = (int)EObjectStateTypeEnum::Chaos_Object_Kinematic;
					break;
				case Chaos::EObjectStateType::Static:
					TargetResults.DynamicState[TransformGroupIndex] = (int)EObjectStateTypeEnum::Chaos_Object_Static;
					break;
				case Chaos::EObjectStateType::Sleeping:
					TargetResults.DynamicState[TransformGroupIndex] = (int)EObjectStateTypeEnum::Chaos_Object_Sleeping;
					break;
				case Chaos::EObjectStateType::Dynamic:
				case Chaos::EObjectStateType::Uninitialized:
				default:
					TargetResults.DynamicState[TransformGroupIndex] = (int)EObjectStateTypeEnum::Chaos_Object_Dynamic;
					break;
				}
			}
			else
			{
				TargetResults.DynamicState[TransformGroupIndex] = (int)EObjectStateTypeEnum::Chaos_Object_Sleeping;
			}

			// Update the transform and parent hierarchy of the active rigid bodies. Active bodies can be either
			// rigid geometry defined from the leaf nodes of the collection, or cluster bodies that drive an entire
			// branch of the hierarchy within the GeometryCollection.
			// - Active bodies are directly driven from the global position of the corresponding
			//   rigid bodies within the solver ( cases where RigidBodyID[TransformGroupIndex] is not disabled ). 
			// - Deactivated bodies are driven from the transforms of their active parents. However the solver can
			//   take ownership of the parents during the simulation, so it might be necessary to force deactivated
			//   bodies out of the collections hierarchy during the simulation.  
			if (!Handle->Disabled())
			{
				// Update the transform of the active body. The active body can be either a single rigid
				// or a collection of rigidly attached geometries (Clustering). The cluster is represented as a
				// single transform in the GeometryCollection, and all children are stored in the local space
				// of the parent cluster.
	
				FTransform& ParticleToWorld = TargetResults.ParticleToWorldTransforms[TransformGroupIndex];
				ParticleToWorld = Chaos::FRigidTransform3(Handle->X(), Handle->R());
				const FTransform MassToLocal = PhysicsThreadCollection.MassToLocal[TransformGroupIndex];

				TargetResults.Transforms[TransformGroupIndex] = MassToLocal.GetRelativeTransformReverse(ParticleToWorld).GetRelativeTransform(ActorToWorld);
				TargetResults.Transforms[TransformGroupIndex].NormalizeRotation();
				if (IsActorScaled)
				{
					TargetResults.Transforms[TransformGroupIndex] = MassToLocal.Inverse() * ActorScaleTransform * MassToLocal * TargetResults.Transforms[TransformGroupIndex];
				}

				PhysicsThreadCollection.Transform[TransformGroupIndex] = TargetResults.Transforms[TransformGroupIndex];

				// Indicate that this object needs to be updated and the proxy is active.
				TargetResults.DisabledStates[TransformGroupIndex] = false;
				IsObjectDynamic = true;

				// If the parent of this NON DISABLED body is set to anything other than INDEX_NONE,
				// then it was just unparented, likely either by rigid clustering or by fields.  We
				// need to force all such enabled rigid bodies out of the transform hierarchy.
				TargetResults.Parent[TransformGroupIndex] = INDEX_NONE;
				if (PhysicsThreadCollection.Parent[TransformGroupIndex] != INDEX_NONE)
				{
					//GeometryCollectionAlgo::UnparentTransform(&PhysicsThreadCollection,TransformGroupIndex);
					PhysicsThreadCollection.Children[PhysicsThreadCollection.Parent[TransformGroupIndex]].Remove(TransformGroupIndex);
					PhysicsThreadCollection.Parent[TransformGroupIndex] = INDEX_NONE;
				}

				// When a leaf node rigid body is removed from a cluster, the rigid
				// body will become active and needs its clusterID updated.  This just
				// syncs the clusterID all the time.
				TPBDRigidParticleHandle<Chaos::FReal, 3>* ClusterParentId = Handle->ClusterIds().Id;
				SolverClusterID[TransformGroupIndex] = ClusterParentId;
			}
			else    // Handle->Disabled()
			{
				// The rigid body parent cluster has changed within the solver, and its
				// parent body is not tracked within the geometry collection. So we need to
				// pull the rigid bodies out of the transform hierarchy, and just drive
				// the positions directly from the solvers cluster particle.
				if(TPBDRigidParticleHandle<Chaos::FReal, 3>* ClusterParentBase = Handle->ClusterIds().Id)
				{
					if(Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* ClusterParent = ClusterParentBase->CastToClustered())
					{
						// syncronize parents if it has changed.
						if(SolverClusterID[TransformGroupIndex] != ClusterParent)
						{
							// Force all driven rigid bodies out of the transform hierarchy
							if(Parent[TransformGroupIndex] != INDEX_NONE)
							{
								// If the parent of this NON DISABLED body is set to anything other than INDEX_NONE,
								// then it was just unparented, likely either by rigid clustering or by fields.  We
								// need to force all such enabled rigid bodies out of the transform hierarchy.
								TargetResults.Parent[TransformGroupIndex] = INDEX_NONE;

								// GeometryCollectionAlgo::UnparentTransform(&PhysicsThreadCollection, ChildIndex);
								PhysicsThreadCollection.Children[PhysicsThreadCollection.Parent[TransformGroupIndex]].Remove(TransformGroupIndex);
								PhysicsThreadCollection.Parent[TransformGroupIndex] = INDEX_NONE;

								// Indicate that this object needs to be updated and the proxy is active.
								TargetResults.DisabledStates[TransformGroupIndex] = false;
								IsObjectDynamic                                   = true;
							}
							SolverClusterID[TransformGroupIndex] = Handle->ClusterIds().Id;
						}

						if(ClusterParent->InternalCluster())
						{
							Chaos::TPBDRigidClusteredParticleHandle<Chaos::FReal, 3>* ProxyElementHandle = SolverParticleHandles[TransformGroupIndex];

							FTransform& ParticleToWorld = TargetResults.ParticleToWorldTransforms[TransformGroupIndex];
							ParticleToWorld             = ProxyElementHandle->ChildToParent() * FRigidTransform3(ClusterParent->X(), ClusterParent->R());    // aka ClusterChildToWorld

							// GeomToActor = ActorToWorld.Inv() * ClusterChildToWorld * MassToLocal.Inv();
							const FTransform MassToLocal                  = PhysicsThreadCollection.MassToLocal[TransformGroupIndex];
							TargetResults.Transforms[TransformGroupIndex] = MassToLocal.GetRelativeTransformReverse(ParticleToWorld).GetRelativeTransform(ActorToWorld);
							TargetResults.Transforms[TransformGroupIndex].NormalizeRotation();
							if (IsActorScaled)
							{
								TargetResults.Transforms[TransformGroupIndex] = MassToLocal.Inverse() * ActorScaleTransform * MassToLocal * TargetResults.Transforms[TransformGroupIndex];
							}

							PhysicsThreadCollection.Transform[TransformGroupIndex] = TargetResults.Transforms[TransformGroupIndex];

							// Indicate that this object needs to be updated and the proxy is active.
							TargetResults.DisabledStates[TransformGroupIndex] = false;
							IsObjectDynamic                                   = true;

							ProxyElementHandle->X() = ParticleToWorld.GetTranslation();
							ProxyElementHandle->R() = ParticleToWorld.GetRotation();
							CurrentSolver->GetEvolution()->DirtyParticle(*ProxyElementHandle);
						}

						if (bGeometryCollectionEnabledNestedChildTransformUpdates)
						{
							if (!ClusterParent->Disabled())
							{
								FTransform ChildToWorld = Handle->ChildToParent() * FRigidTransform3(ClusterParent->X(), ClusterParent->R());
								Handle->X() = Handle->P() = ChildToWorld.GetTranslation();
								Handle->R() = Handle->Q() = ChildToWorld.GetRotation();
								Handle->UpdateWorldSpaceState(ChildToWorld, FVec3(0));
								CurrentSolver->GetEvolution()->DirtyParticle(*Handle);
							}
						}
					}
				}
			}    // end if

			PhysicsThreadCollection.Active[TransformGroupIndex] = !TargetResults.DisabledStates[TransformGroupIndex];
		}    // end for
	}        // STAT_CalcParticleToWorld scope
	
	// If object is dynamic, compute global matrices	
	if (IsObjectDynamic || TargetResults.GlobalTransforms.Num() == 0)
	{
		SCOPE_CYCLE_COUNTER(STAT_CalcGlobalGCMatrices);
		check(TargetResults.Transforms.Num() == TargetResults.Parent.Num());
		GeometryCollectionAlgo::GlobalMatrices(TargetResults.Transforms, TargetResults.Parent, TargetResults.GlobalTransforms);
	}
	

	// Advertise to game thread
	TargetResults.IsObjectDynamic = IsObjectDynamic;
	TargetResults.IsObjectLoading = IsObjectLoading;
}

void FGeometryCollectionPhysicsProxy::FlipBuffer()
{
	/**
	 * CONTEXT: PHYSICSTHREAD (Write Locked)
	 * Called by the physics thread to signal that it is safe to perform any double-buffer flips here.
	 * The physics thread has pre-locked an RW lock for this operation so the game thread won't be reading
	 * the data
	 */

	PhysToGameInterchange.FlipProducer();
}

// Called from FPhysScene_ChaosInterface::SyncBodies(), NOT the solver.
bool FGeometryCollectionPhysicsProxy::PullFromPhysicsState(const Chaos::FDirtyGeometryCollectionData& BufferData, const int32 SolverSyncTimestamp)
{
	if(IsObjectDeleting) return false;

	/**
	 * CONTEXT: GAMETHREAD (Read Locked)
	 * Perform a similar operation to Sync, but take the data from a gamethread-safe buffer. This will be called
	 * from the game thread when it cannot sync to the physics thread. The simulation is very likely to be running
	 * when this happens so never read any physics thread data here!
	 *
	 * Note: A read lock will have been acquired for this - so the physics thread won't force a buffer flip while this
	 * sync is ongoing
	 */

	const FGeometryCollectionResults& TargetResults = BufferData.Results;

	FGeometryDynamicCollection& DynamicCollection = GameThreadCollection;

	TManagedArray<FVector3f>* LinearVelocity = DynamicCollection.FindAttributeTyped<FVector3f>("LinearVelocity", FTransformCollection::TransformGroup);

	// We should never be changing the number of entries, this would break other 
	// attributes in the transform group.
	const int32 NumTransforms = DynamicCollection.Transform.Num();
	if (ensure(NumTransforms == TargetResults.Transforms.Num()))
	{
		for (int32 TransformGroupIndex = 0; TransformGroupIndex < NumTransforms; ++TransformGroupIndex)
		{
			if (!TargetResults.DisabledStates[TransformGroupIndex])
			{
				DynamicCollection.Parent[TransformGroupIndex] = TargetResults.Parent[TransformGroupIndex];
				const FTransform& LocalTransform = TargetResults.Transforms[TransformGroupIndex];
				const FTransform& ParticleToWorld = TargetResults.ParticleToWorldTransforms[TransformGroupIndex];

				DynamicCollection.Transform[TransformGroupIndex] = LocalTransform;

				Chaos::FGeometryParticle* GTParticle = GTParticles[TransformGroupIndex].Get();

				if(LinearVelocity)
				{
					TManagedArray<FVector3f>* AngularVelocity = DynamicCollection.FindAttributeTyped<FVector3f>("AngularVelocity", FTransformCollection::TransformGroup);
					check(AngularVelocity);
					FVector DiffX = ParticleToWorld.GetTranslation() - GTParticle->X();
					FVector DiffR = (ParticleToWorld.GetRotation().Euler() - GTParticle->R().Euler()) * (PI / 180.0f);

					(*LinearVelocity)[TransformGroupIndex] = FVector3f(DiffX / TargetResults.SolverDt);
					(*AngularVelocity)[TransformGroupIndex] = FVector3f(DiffR / TargetResults.SolverDt);
				}

				GTParticles[TransformGroupIndex]->SetX(ParticleToWorld.GetTranslation());
				GTParticles[TransformGroupIndex]->SetR(ParticleToWorld.GetRotation());
				GTParticles[TransformGroupIndex]->UpdateShapeBounds();
			}

			DynamicCollection.DynamicState[TransformGroupIndex] = TargetResults.DynamicState[TransformGroupIndex];
			DynamicCollection.Active[TransformGroupIndex] = !TargetResults.DisabledStates[TransformGroupIndex];
		}

		//question: why do we need this? Sleeping objects will always have to update GPU
		DynamicCollection.MakeDirty();

	}

	return true;
}

//==============================================================================
// STATIC SETUP FUNCTIONS
//==============================================================================



/** 
	NOTE - Making any changes to data stored on the rest collection below MUST be accompanied
	by a rotation of the DDC key in FDerivedDataGeometryCollectionCooker::GetVersionString
*/
void FGeometryCollectionPhysicsProxy::InitializeSharedCollisionStructures(
	Chaos::FErrorReporter& ErrorReporter,
	FGeometryCollection& RestCollection,
	const FSharedSimulationParameters& SharedParams)
{
	check(SharedParams.SizeSpecificData.Num());

	FString BaseErrorPrefix = ErrorReporter.GetPrefix();

	// fracture tools can create an empty GC before appending new geometry
	if (RestCollection.NumElements(FGeometryCollection::GeometryGroup) == 0)
	{
		return;
	}

	// clamps
	const float MinBoundsExtents = SharedParams.MinimumBoundingExtentClamp;
	const float MaxBoundsExtents = SharedParams.MaximumBoundingExtentClamp;
	const float MinVolume = SharedParams.MinimumVolumeClamp();
	const float MaxVolume = SharedParams.MaximumVolumeClamp();
	const float MinMass = FMath::Max(SMALL_NUMBER, SharedParams.MaximumMassClamp);
	const float MaxMass = SharedParams.MinimumMassClamp;


	//TArray<TArray<TArray<int32>>> BoundaryVertexIndices;
	//GeometryCollectionAlgo::FindOpenBoundaries(&RestCollection, 1e-2, BoundaryVertexIndices);
	//GeometryCollectionAlgo::TriangulateBoundaries(&RestCollection, BoundaryVertexIndices);
	//RestCollection.ReindexMaterials();

	using namespace Chaos;

	// TransformGroup
	const TManagedArray<int32>& BoneMap = RestCollection.BoneMap;
	const TManagedArray<int32>& Parent = RestCollection.Parent;
	const TManagedArray<TSet<int32>>& Children = RestCollection.Children;
	const TManagedArray<int32>& SimulationType = RestCollection.SimulationType;
	TManagedArray<bool>& CollectionSimulatableParticles =
		RestCollection.GetAttribute<bool>(
			FGeometryCollection::SimulatableParticlesAttribute, FTransformCollection::TransformGroup);
	TManagedArray<FVector3f>& CollectionInertiaTensor =
		RestCollection.AddAttribute<FVector3f>(
			TEXT("InertiaTensor"), FTransformCollection::TransformGroup);
	TManagedArray<FRealSingle>& CollectionMass =
		RestCollection.AddAttribute<FRealSingle>(
			TEXT("Mass"), FTransformCollection::TransformGroup);
	TManagedArray<TUniquePtr<FSimplicial>>& CollectionSimplicials =
		RestCollection.AddAttribute<TUniquePtr<FSimplicial>>(
			FGeometryDynamicCollection::SimplicialsAttribute, FTransformCollection::TransformGroup);

	RestCollection.RemoveAttribute(
		FGeometryDynamicCollection::ImplicitsAttribute, FTransformCollection::TransformGroup);
	TManagedArray<FGeometryDynamicCollection::FSharedImplicit>& CollectionImplicits =
		RestCollection.AddAttribute<FGeometryDynamicCollection::FSharedImplicit>(
			FGeometryDynamicCollection::ImplicitsAttribute, FTransformCollection::TransformGroup);

	TManagedArray<TSet<int32>>* TransformToConvexIndices = RestCollection.FindAttribute<TSet<int32>>("TransformToConvexIndices", FTransformCollection::TransformGroup);
	TManagedArray<TUniquePtr<Chaos::FConvex>>* ConvexGeometry = RestCollection.FindAttribute<TUniquePtr<Chaos::FConvex>>("ConvexHull", "Convex");

	bool bUseRelativeSize = RestCollection.HasAttribute(TEXT("Size"), FTransformCollection::TransformGroup);
	if (!bUseRelativeSize)
	{
		UE_LOG(LogChaos, Display, TEXT("Relative Size not found on Rest Collection. Using bounds volume for SizeSpecificData indexing instead."));
	}


	// @todo(chaos_transforms) : do we still use this?
	TManagedArray<FTransform>& CollectionMassToLocal =
		RestCollection.AddAttribute<FTransform>(
			TEXT("MassToLocal"), FTransformCollection::TransformGroup);
	FTransform IdentityXf(FQuat::Identity, FVector(0));
	IdentityXf.NormalizeRotation();
	CollectionMassToLocal.Fill(IdentityXf);

	// VerticesGroup
	const TManagedArray<FVector3f>& Vertex = RestCollection.Vertex;

	// FacesGroup
	const TManagedArray<bool>& Visible = RestCollection.Visible;
	const TManagedArray<FIntVector>& Indices = RestCollection.Indices;

	// GeometryGroup
	const TManagedArray<int32>& TransformIndex = RestCollection.TransformIndex;
	const TManagedArray<FBox>& BoundingBox = RestCollection.BoundingBox;
	TManagedArray<Chaos::FRealSingle>& InnerRadius = RestCollection.InnerRadius;
	TManagedArray<Chaos::FRealSingle>& OuterRadius = RestCollection.OuterRadius;
	const TManagedArray<int32>& VertexStart = RestCollection.VertexStart;
	const TManagedArray<int32>& VertexCount = RestCollection.VertexCount;
	const TManagedArray<int32>& FaceStart = RestCollection.FaceStart;
	const TManagedArray<int32>& FaceCount = RestCollection.FaceCount;


	TArray<FTransform> CollectionSpaceTransforms;
	{ // tmp scope
		const TManagedArray<FTransform>& HierarchyTransform = RestCollection.Transform;
		GeometryCollectionAlgo::GlobalMatrices(HierarchyTransform, Parent, CollectionSpaceTransforms);
	} // tmp scope

	const int32 NumTransforms = CollectionSpaceTransforms.Num();
	const int32 NumGeometries = RestCollection.NumElements(FGeometryCollection::GeometryGroup);

	TArray<TUniquePtr<FTriangleMesh>> TriangleMeshesArray;	//use to union trimeshes in cluster case
	TriangleMeshesArray.AddDefaulted(NumTransforms);

	FParticles MassSpaceParticles;
	MassSpaceParticles.AddParticles(Vertex.Num());
	for (int32 Idx = 0; Idx < Vertex.Num(); ++Idx)
	{
		MassSpaceParticles.X(Idx) = Vertex[Idx];	//mass space computation done later down
	}

	TArray<FMassProperties> MassPropertiesArray;
	MassPropertiesArray.AddUninitialized(NumGeometries);

	TArray<bool> InertiaComputationNeeded;
	InertiaComputationNeeded.Init(false, NumGeometries);

	// We skip very small geometry and log as a warning. To avoid log spamming, we wait
	// until we complete the loop before reporting the skips.
	bool bSkippedSmallGeometry = false;
	
	FReal TotalVolume = 0.f;
	// The geometry group has a set of transform indices that maps a geometry index
	// to a transform index, but only in the case where there is a 1-to-1 mapping 
	// between the two.  In the event where a geometry is instanced for multiple
	// transforms, the transform index on the geometry group should be INDEX_NONE.
	// Otherwise, iterating over the geometry group is a convenient way to iterate
	// over all the leaves of the hierarchy.
	check(!TransformIndex.Contains(INDEX_NONE)); // TODO: implement support for instanced bodies
	for (int32 GeometryIndex = 0; GeometryIndex < NumGeometries; GeometryIndex++)
	{
		const int32 TransformGroupIndex = TransformIndex[GeometryIndex];
		if (SimulationType[TransformGroupIndex] > FGeometryCollection::ESimulationTypes::FST_None)
		{
			TUniquePtr<FTriangleMesh> TriMesh(
				CreateTriangleMesh(
					FaceStart[GeometryIndex],
					FaceCount[GeometryIndex],
					Visible,
					Indices));

			FMassProperties& MassProperties = MassPropertiesArray[GeometryIndex];

			{
				MassProperties.CenterOfMass = FVector3f::ZeroVector;
				MassProperties.RotationOfMass = FRotation3(FQuat::Identity).GetNormalized();
				MassProperties.Volume = 0.f;
				MassProperties.InertiaTensor = FMatrix33(1,1,1);
				MassProperties.Mass = 1.0f; // start with unit mass, scaled later by density

				if (BoundingBox[GeometryIndex].GetExtent().GetAbsMin() < MinVolume)
				{
					bSkippedSmallGeometry = true;
					CollectionSimulatableParticles[TransformGroupIndex] = false;	//do not simulate tiny particles
					MassProperties.Mass = 0.f;
					MassProperties.InertiaTensor = FMatrix33(0,0,0);
				}
				else
				{
					CalculateVolumeAndCenterOfMass(MassSpaceParticles, TriMesh->GetElements(), MassProperties.Volume, MassProperties.CenterOfMass);
					InertiaComputationNeeded[GeometryIndex] = true;
					if(MassProperties.Volume == 0)
					{
						FVector Extents = FReal(2) * BoundingBox[GeometryIndex].GetExtent(); // FBox::GetExtent() returns half the size, but FAABB::Extents() returns total size
						MassProperties.Volume = Extents.X * Extents.Y * Extents.Z;
						FReal ExtentsYZ = Extents.Y * Extents.Y + Extents.Z * Extents.Z;
						FReal ExtentsXZ = Extents.X * Extents.X + Extents.Z * Extents.Z;
						FReal ExtentsXY = Extents.X * Extents.X + Extents.Y * Extents.Y;
						MassProperties.InertiaTensor = PMatrix<FReal, 3, 3>(ExtentsYZ / 12.f, ExtentsXZ / 12.f, ExtentsXY / 12.f);
						MassProperties.CenterOfMass = BoundingBox[GeometryIndex].GetCenter();
						CollectionMassToLocal[TransformGroupIndex] = FTransform(FQuat::Identity, MassProperties.CenterOfMass);
						InertiaComputationNeeded[GeometryIndex] = false;
					}

					if (MassProperties.Volume < MinVolume)
					{
						// For rigid bodies outside of range just defaut to a clamped bounding box, and warn the user.
						MassProperties.Volume = MinVolume;
						CollectionMassToLocal[TransformGroupIndex] = FTransform(FQuat::Identity, BoundingBox[GeometryIndex].GetCenter());
						InertiaComputationNeeded[GeometryIndex] = false;
					}
					else if (MaxVolume < MassProperties.Volume)
					{
						// For rigid bodies outside of range just defaut to a clamped bounding box, and warn the user
						MassProperties.Volume = MaxVolume;
						CollectionMassToLocal[TransformGroupIndex] = FTransform(FQuat::Identity, BoundingBox[GeometryIndex].GetCenter());
						InertiaComputationNeeded[GeometryIndex] = false;
					}
					else
					{
						CollectionMassToLocal[TransformGroupIndex] = FTransform(FQuat::Identity, MassProperties.CenterOfMass);
					}

					FVector MassTranslation = CollectionMassToLocal[TransformGroupIndex].GetTranslation();
					if (!FMath::IsNearlyZero(MassTranslation.SizeSquared()))
					{
						const int32 IdxStart = VertexStart[GeometryIndex];
						const int32 IdxEnd = IdxStart + VertexCount[GeometryIndex];
						for (int32 Idx = IdxStart; Idx < IdxEnd; ++Idx)
						{
							MassSpaceParticles.X(Idx) -= MassTranslation;
						}
					}
				}
			}

			if (InnerRadius[GeometryIndex] == 0.0f || OuterRadius[GeometryIndex] == 0.0f)
			{
				const int32 VCount = VertexCount[GeometryIndex];
				if (VCount != 0)
				{
					const FVector3f Center = (FVector3f)BoundingBox[GeometryIndex].GetCenter();
					const int32 VStart = VertexStart[GeometryIndex];

					InnerRadius[GeometryIndex] = VCount ? TNumericLimits<FRealSingle>::Max() : 0.0f;
					OuterRadius[GeometryIndex] = 0.0f;
					for (int32 VIdx = 0; VIdx < VCount; ++VIdx)
					{
						const int32 PtIdx = VStart + VIdx;
						const FVector3f& Pt = Vertex[PtIdx];
						const float DistSq = FVector3f::DistSquared(Pt, Center);
						if (InnerRadius[GeometryIndex] > DistSq)
						{
							InnerRadius[GeometryIndex] = DistSq;
						}
						if (OuterRadius[GeometryIndex] < DistSq)
						{
							OuterRadius[GeometryIndex] = DistSq;
						}
					}
					InnerRadius[GeometryIndex] = FMath::Sqrt(InnerRadius[GeometryIndex]);
					OuterRadius[GeometryIndex] = FMath::Sqrt(OuterRadius[GeometryIndex]);
				}
			}

			TotalVolume += MassProperties.Volume;
			TriangleMeshesArray[TransformGroupIndex] = MoveTemp(TriMesh);
		}
		else
		{
			CollectionSimulatableParticles[TransformGroupIndex] = false;
		}
	}

	if (bSkippedSmallGeometry)
	{
		UE_LOG(LogChaos, Warning, TEXT("Some geometry is too small to be simulated and has been skipped."));
	}

	//User provides us with total mass or density.
	//Density must be the same for individual parts and the total. Density_i = Density = Mass_i / Volume_i
	//Total mass must equal sum of individual parts. Mass_i = TotalMass * Volume_i / TotalVolume => Density_i = TotalMass / TotalVolume
	TotalVolume = FMath::Max(TotalVolume, MinBoundsExtents * MinBoundsExtents * MinBoundsExtents);
	const FReal DesiredTotalMass = SharedParams.bMassAsDensity ? SharedParams.Mass * TotalVolume : SharedParams.Mass;
	const FReal ClampedTotalMass = FMath::Clamp(DesiredTotalMass, SharedParams.MinimumMassClamp, SharedParams.MaximumMassClamp);
	const FReal DesiredDensity = ClampedTotalMass / TotalVolume;

	FVec3 MaxChildBounds(1);
	ParallelFor(NumGeometries, [&](int32 GeometryIndex)
	//for (int32 GeometryIndex = 0; GeometryIndex < NumGeometries; GeometryIndex++)
	{
		// Need a new error reporter for parallel for loop here as it wouldn't be thread-safe to write to the prefix
		Chaos::FErrorReporter LocalErrorReporter;
		const int32 TransformGroupIndex = TransformIndex[GeometryIndex];

		const FReal Volume_i = MassPropertiesArray[GeometryIndex].Volume;
		if (CollectionSimulatableParticles[TransformGroupIndex])
		{
			//Must clamp each individual mass regardless of desired density
			if (DesiredDensity * Volume_i > SharedParams.MaximumMassClamp)
			{
				// For rigid bodies outside of range just defaut to a clamped bounding box, and warn the user.
				LocalErrorReporter.ReportError(*FString::Printf(TEXT("Geometry has invalid mass (too large)")));
				LocalErrorReporter.HandleLatestError();

				CollectionSimulatableParticles[TransformGroupIndex] = false;
			}
		}

		if (CollectionSimulatableParticles[TransformGroupIndex])
		{
			TUniquePtr<FTriangleMesh>& TriMesh = TriangleMeshesArray[TransformGroupIndex];
			FMassProperties& MassProperties = MassPropertiesArray[GeometryIndex];

			const FReal Mass_i = FMath::Max(DesiredDensity * Volume_i, SharedParams.MinimumMassClamp);
			const FReal Density_i = Mass_i / Volume_i;
			CollectionMass[TransformGroupIndex] = (FRealSingle)Mass_i;

			if (InertiaComputationNeeded[GeometryIndex])
			{
				// Note: particles already in CoM space, so passing in zero as CoM
				CalculateInertiaAndRotationOfMass(MassSpaceParticles, TriMesh->GetSurfaceElements(), Density_i, FVec3(0), MassProperties.InertiaTensor, MassProperties.RotationOfMass);
				CollectionInertiaTensor[TransformGroupIndex] = FVector3f((float)MassProperties.InertiaTensor.M[0][0], (float)MassProperties.InertiaTensor.M[1][1], (float)MassProperties.InertiaTensor.M[2][2]);
				CollectionMassToLocal[TransformGroupIndex] = FTransform(MassProperties.RotationOfMass, MassProperties.CenterOfMass);


				if (!MassProperties.RotationOfMass.Equals(FQuat::Identity))
				{
					FTransform InverseMassRotation = FTransform(MassProperties.RotationOfMass.Inverse());
					const int32 IdxStart = VertexStart[GeometryIndex];
					const int32 IdxEnd = IdxStart + VertexCount[GeometryIndex];
					for (int32 Idx = IdxStart; Idx < IdxEnd; ++Idx)
					{
						MassSpaceParticles.X(Idx) = InverseMassRotation.TransformPosition(MassSpaceParticles.X(Idx));
					}
				}
			}
			else
			{
				const FVec3 DiagonalInertia(MassProperties.InertiaTensor.M[0][0], MassProperties.InertiaTensor.M[1][1], MassProperties.InertiaTensor.M[2][2]);
				CollectionInertiaTensor[TransformGroupIndex] = FVector3f(DiagonalInertia * Mass_i);
			}

			FBox InstanceBoundingBox(EForceInit::ForceInitToZero);
			if (TriMesh->GetElements().Num())
			{
				const TSet<int32> MeshVertices = TriMesh->GetVertices();
				for (const int32 Idx : MeshVertices)
				{
					InstanceBoundingBox += MassSpaceParticles.X(Idx);
				}
			}
			else if(VertexCount[GeometryIndex])
			{
				const int32 IdxStart = VertexStart[GeometryIndex];
				const int32 IdxEnd = IdxStart + VertexCount[GeometryIndex];
				for (int32 Idx = IdxStart; Idx < IdxEnd; ++Idx)
				{
					InstanceBoundingBox += MassSpaceParticles.X(Idx);
				}
			}
			else
			{
				InstanceBoundingBox = FBox(MassProperties.CenterOfMass, MassProperties.CenterOfMass);
			}

			// If we have a normalized Size available, use that to determine SizeSpecific index, otherwise fall back on Bounds volume.
			int32 SizeSpecificIdx;
			if (bUseRelativeSize)
			{
				const TManagedArray<float>& RelativeSize = RestCollection.GetAttribute<float>(TEXT("Size"), FTransformCollection::TransformGroup);
				SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(SharedParams.SizeSpecificData, RelativeSize[TransformGroupIndex]);
			}
			else
			{
				SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(SharedParams.SizeSpecificData, InstanceBoundingBox);
			}
			
			const FSharedSimulationSizeSpecificData& SizeSpecificData = SharedParams.SizeSpecificData[SizeSpecificIdx];

			if (SizeSpecificData.CollisionShapesData.Num())
			{
				//
				//  Build the simplicial for the rest collection. This will be used later in the DynamicCollection to 
				//  populate the collision structures of the simulation. 
				//
				if (ensureMsgf(TriMesh, TEXT("No Triangle representation")))
				{
					Chaos::FBVHParticles* Simplicial =
						FCollisionStructureManager::NewSimplicial(
							MassSpaceParticles,
							BoneMap,
							SizeSpecificData.CollisionShapesData[0].CollisionType,
							*TriMesh,
							SizeSpecificData.CollisionShapesData[0].CollisionParticleData.CollisionParticlesFraction);
					CollectionSimplicials[TransformGroupIndex] = TUniquePtr<FSimplicial>(Simplicial); // CollectionSimplicials is in the TransformGroup
					//ensureMsgf(CollectionSimplicials[TransformGroupIndex], TEXT("No simplicial representation."));
					if (!CollectionSimplicials[TransformGroupIndex]->Size())
					{
						ensureMsgf(false, TEXT("Simplicial is empty."));
					}

					if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_LevelSet)
					{
						LocalErrorReporter.SetPrefix(BaseErrorPrefix + " | Transform Index: " + FString::FromInt(TransformGroupIndex) + " of " + FString::FromInt(TransformIndex.Num()));
						CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitLevelset(
								LocalErrorReporter,
								MassSpaceParticles,
								*TriMesh,
								InstanceBoundingBox,
								SizeSpecificData.CollisionShapesData[0].LevelSetData.MinLevelSetResolution,
								SizeSpecificData.CollisionShapesData[0].LevelSetData.MaxLevelSetResolution,
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));
						// Fall back on sphere if level set rasterization failed.
						if (!CollectionImplicits[TransformGroupIndex])
						{
							CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
								FCollisionStructureManager::NewImplicitSphere(
									InnerRadius[GeometryIndex],
									SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
									SizeSpecificData.CollisionShapesData[0].CollisionType));
						}
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box)
					{
						CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitBox(
								InstanceBoundingBox,
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere)
					{
						CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitSphere(
								InnerRadius[GeometryIndex],
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Convex)
					{
						if (ConvexGeometry && TransformToConvexIndices)
						{
							CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
									FCollisionStructureManager::NewImplicitConvex(
										(*TransformToConvexIndices)[TransformGroupIndex].Array(),
										ConvexGeometry,
										SizeSpecificData.CollisionShapesData[0].CollisionType,
										CollectionMassToLocal[TransformGroupIndex],
										(Chaos::FReal)SizeSpecificData.CollisionShapesData[0].CollisionMarginFraction
									)
							);
						}
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Capsule)
					{
						CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitCapsule(
								InstanceBoundingBox,
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_None)
					{
						CollectionImplicits[TransformGroupIndex] = nullptr;
					}
					else
					{
						ensure(false); // unsupported implicit type!
					}

					if (CollectionImplicits[TransformGroupIndex] && CollectionImplicits[TransformGroupIndex]->HasBoundingBox())
					{
						const auto Implicit = CollectionImplicits[TransformGroupIndex];
						const auto BBox = Implicit->BoundingBox();
						const FVec3 Extents = BBox.Extents(); // Chaos::FAABB3::Extents() is Max - Min
						MaxChildBounds = MaxChildBounds.ComponentwiseMax(Extents);
					}
				}
			}
		}
	});

	// question: at the moment we always build cluster data in the asset. This 
	// allows for per instance toggling. Is this needed? It increases memory 
	// usage for all geometry collection assets.
	const bool bEnableClustering = true;	
	if (bEnableClustering)
	{
		//Put all children into collection space so we can compute mass properties.
		TUniquePtr<TPBDRigidClusteredParticles<FReal, 3>> CollectionSpaceParticles(new TPBDRigidClusteredParticles<FReal, 3>());
		CollectionSpaceParticles->AddParticles(NumTransforms);

		// Init to -FLT_MAX for debugging purposes
		for (int32 Idx = 0; Idx < NumTransforms; Idx++)
		{
			CollectionSpaceParticles->X(Idx) = Chaos::FVec3(-TNumericLimits<FReal>::Max());
		}

		//
		// TODO: We generate particles & handles for leaf nodes so that we can use some 
		// runtime clustering functions.  That's adding a lot of work and dependencies
		// just so we can make an API happy.  We should refactor the common routines
		// to have a handle agnostic implementation.
		//

		TMap<const TGeometryParticleHandle<FReal, 3>*, int32> HandleToTransformIdx;
		TArray<TUniquePtr<TPBDRigidClusteredParticleHandle<FReal, 3>>> Handles;
		Handles.Reserve(NumTransforms);
		for (int32 Idx = 0; Idx < NumTransforms; Idx++)
		{
			Handles.Add(TPBDRigidClusteredParticleHandle<FReal, 3>::CreateParticleHandle(
				MakeSerializable(CollectionSpaceParticles), Idx, Idx));
			HandleToTransformIdx.Add(Handles[Handles.Num() - 1].Get(), Idx);
		}

		// We use PopulateSimulatedParticle here just to give us some valid particles to operate on - with correct
		// position, mass and inertia so we can accumulate data for clusters just below.
 		for (int32 GeometryIdx = 0; GeometryIdx < NumGeometries; ++GeometryIdx)
 		{
 			const int32 TransformGroupIndex = TransformIndex[GeometryIdx];

 			if (CollectionSimulatableParticles[TransformGroupIndex])
 			{
				FTransform GeometryWorldTransform = CollectionMassToLocal[TransformGroupIndex] * CollectionSpaceTransforms[TransformGroupIndex];

 				PopulateSimulatedParticle(
 					Handles[TransformGroupIndex].Get(),
 					SharedParams, 
 					CollectionSimplicials[TransformGroupIndex].Get(),
 					CollectionImplicits[TransformGroupIndex],
 					FCollisionFilterData(),		// SimFilter
 					FCollisionFilterData(),		// QueryFilter
 					CollectionMass[TransformGroupIndex],
 					CollectionInertiaTensor[TransformGroupIndex], 
					GeometryWorldTransform,
 					(uint8)EObjectStateTypeEnum::Chaos_Object_Dynamic, 
 					INDEX_NONE,  // CollisionGroup
					1.0f // todo(chaos) CollisionParticlesPerObjectFraction is not accessible right there for now but we can pass 1.0 for the time being
				);
 			}
 		}

		const TArray<int32> RecursiveOrder = GeometryCollectionAlgo::ComputeRecursiveOrder(RestCollection);
		const TArray<int32> TransformToGeometry = ComputeTransformToGeometryMap(RestCollection);

		TArray<bool> IsClusterSimulated;
		IsClusterSimulated.Init(false, CollectionSpaceParticles->Size());
		//build collision structures depth first
		for (const int32 TransformGroupIndex : RecursiveOrder)
		{
			if (RestCollection.IsClustered(TransformGroupIndex))
			{
				const int32 ClusterTransformIdx = TransformGroupIndex;
				//update mass 
				TSet<TPBDRigidParticleHandle<FReal,3>*> ChildrenIndices;
				{ // tmp scope
					ChildrenIndices.Reserve(Children[ClusterTransformIdx].Num());
					for (int32 ChildIdx : Children[ClusterTransformIdx])
					{
						if (CollectionSimulatableParticles[ChildIdx] || IsClusterSimulated[ChildIdx])
						{
							ChildrenIndices.Add(Handles[ChildIdx].Get());
						}
					}
					if (!ChildrenIndices.Num())
					{
						continue;
					}
				} // tmp scope

				//CollectionSimulatableParticles[TransformGroupIndex] = true;
				IsClusterSimulated[TransformGroupIndex] = true;


				// TODO: This needs to be rotated to diagonal, used to update I()/InvI() from diagonal, and update transform with rotation.
				FMatrix33 ClusterInertia(0);
				UpdateClusterMassProperties(Handles[ClusterTransformIdx].Get(), ChildrenIndices, ClusterInertia);	//compute mass properties
				const FTransform ClusterMassToCollection = 
					FTransform(CollectionSpaceParticles->R(ClusterTransformIdx), 
							   CollectionSpaceParticles->X(ClusterTransformIdx));

				CollectionMassToLocal[ClusterTransformIdx] = 
					ClusterMassToCollection.GetRelativeTransform(
						CollectionSpaceTransforms[ClusterTransformIdx]);

				//update geometry
				//merge children meshes and move them into cluster's mass space
				TArray<TVector<int32, 3>> UnionMeshIndices;
				int32 BiggestNumElements = 0;
				{ // tmp scope
					int32 NumChildIndices = 0;
					for (TPBDRigidParticleHandle<FReal, 3>* Child : ChildrenIndices)
					{
						const int32 ChildTransformIdx = HandleToTransformIdx[Child];
						if (Chaos::FTriangleMesh* ChildMesh = TriangleMeshesArray[ChildTransformIdx].Get())
						{
							BiggestNumElements = FMath::Max(BiggestNumElements, ChildMesh->GetNumElements());
							NumChildIndices += ChildMesh->GetNumElements();
						}
					}
					UnionMeshIndices.Reserve(NumChildIndices);
				} // tmp scope

				FBox InstanceBoundingBox(EForceInit::ForceInitToZero);
				{ // tmp scope
					TSet<int32> VertsAdded;
					VertsAdded.Reserve(BiggestNumElements);
					for (TPBDRigidParticleHandle<FReal, 3>* Child : ChildrenIndices)
					{
						const int32 ChildTransformIdx = HandleToTransformIdx[Child];
						if (Chaos::FTriangleMesh* ChildMesh = TriangleMeshesArray[ChildTransformIdx].Get())
						{
							const TArray<TVector<int32, 3>>& ChildIndices = ChildMesh->GetSurfaceElements();
							UnionMeshIndices.Append(ChildIndices);

							// To move a particle from mass-space in the child to mass-space in the cluster parent, calculate
							// the relative transform between the mass-space origin for both the parent and child before
							// transforming the mass space particles into the parent mass-space.
							const FTransform ChildMassToClusterMass = (CollectionMassToLocal[ChildTransformIdx] * CollectionSpaceTransforms[ChildTransformIdx]).GetRelativeTransform(CollectionMassToLocal[ClusterTransformIdx] * CollectionSpaceTransforms[ClusterTransformIdx]);

							ChildMesh->GetVertexSet(VertsAdded);
							for (const int32 VertIdx : VertsAdded)
							{
								//Update particles so they are in the cluster's mass space
								MassSpaceParticles.X(VertIdx) =
									ChildMassToClusterMass.TransformPosition(MassSpaceParticles.X(VertIdx));
								InstanceBoundingBox += MassSpaceParticles.X(VertIdx);
							}
						}
					}
				} // tmp scope

				TUniquePtr<FTriangleMesh> UnionMesh(new FTriangleMesh(MoveTemp(UnionMeshIndices)));
				// TODO: Seems this should rotate full matrix and not discard off diagonals.
				const FVec3& InertiaDiagonal = CollectionSpaceParticles->I(ClusterTransformIdx);
				CollectionInertiaTensor[ClusterTransformIdx] = FVector3f(InertiaDiagonal);	// LWC_TODO: Precision loss
				CollectionMass[ClusterTransformIdx] = (FRealSingle)CollectionSpaceParticles->M(ClusterTransformIdx);


				int32 SizeSpecificIdx;
				if (bUseRelativeSize)
				{
					const TManagedArray<float>& RelativeSize = RestCollection.GetAttribute<float>(TEXT("Size"), FTransformCollection::TransformGroup);
					SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(SharedParams.SizeSpecificData, RelativeSize[TransformGroupIndex]);
				}
				else
				{
					SizeSpecificIdx = GeometryCollection::SizeSpecific::FindIndexForVolume(SharedParams.SizeSpecificData, InstanceBoundingBox);	
				}
				const FSharedSimulationSizeSpecificData& SizeSpecificData = SharedParams.SizeSpecificData[SizeSpecificIdx];

				if (SizeSpecificData.CollisionShapesData.Num())
				{
					if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_LevelSet)
					{
						const FVec3 Scale = 2 * InstanceBoundingBox.GetExtent() / MaxChildBounds; // FBox's extents are 1/2 (Max - Min)
						const FReal ScaleMax = Scale.GetAbsMax();
						const FReal ScaleMin = Scale.GetAbsMin();

						FReal MinResolution = ScaleMin * (FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MinLevelSetResolution;
						MinResolution = FMath::Clamp(MinResolution,
							(FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MinLevelSetResolution,
							(FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MinClusterLevelSetResolution);

						FReal MaxResolution = ScaleMax * (FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MaxLevelSetResolution;
						MaxResolution = FMath::Clamp(MaxResolution,
							(FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MaxLevelSetResolution,
							(FReal)SizeSpecificData.CollisionShapesData[0].LevelSetData.MaxClusterLevelSetResolution);

						//don't support non level-set serialization
						ErrorReporter.SetPrefix(BaseErrorPrefix + " | Cluster Transform Index: " + FString::FromInt(ClusterTransformIdx));
						CollectionImplicits[ClusterTransformIdx] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitLevelset(
								ErrorReporter,
								MassSpaceParticles,
								*UnionMesh,
								InstanceBoundingBox,
								FMath::FloorToInt32(MinResolution),
								FMath::FloorToInt32(MaxResolution),
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));
						// Fall back on sphere if level set rasterization failed.
						if (!CollectionImplicits[ClusterTransformIdx])
						{
							CollectionImplicits[ClusterTransformIdx] = FGeometryDynamicCollection::FSharedImplicit(
								FCollisionStructureManager::NewImplicitSphere(
									InstanceBoundingBox.GetExtent().GetAbsMin(), // FBox's extents are 1/2 (Max - Min)
									SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
									SizeSpecificData.CollisionShapesData[0].CollisionType));
						}

						CollectionSimplicials[ClusterTransformIdx] = TUniquePtr<FSimplicial>(
							FCollisionStructureManager::NewSimplicial(MassSpaceParticles, *UnionMesh, CollectionImplicits[ClusterTransformIdx].Get(),
							SharedParams.MaximumCollisionParticleCount));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Box)
					{
						CollectionImplicits[ClusterTransformIdx] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitBox(
								InstanceBoundingBox,
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));

						CollectionSimplicials[ClusterTransformIdx] = TUniquePtr<FSimplicial>(
							FCollisionStructureManager::NewSimplicial(MassSpaceParticles, *UnionMesh, CollectionImplicits[ClusterTransformIdx].Get(),
							SharedParams.MaximumCollisionParticleCount));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Sphere)
					{
						CollectionImplicits[ClusterTransformIdx] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitSphere(
								InstanceBoundingBox.GetExtent().GetAbsMin(), // FBox's extents are 1/2 (Max - Min)
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));

						CollectionSimplicials[ClusterTransformIdx] = TUniquePtr<FSimplicial>(
							FCollisionStructureManager::NewSimplicial(MassSpaceParticles, *UnionMesh, CollectionImplicits[ClusterTransformIdx].Get(),
							SharedParams.MaximumCollisionParticleCount));
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Convex)
					{
						if (ConvexGeometry && TransformToConvexIndices)
						{
							CollectionImplicits[TransformGroupIndex] = FGeometryDynamicCollection::FSharedImplicit(
								FCollisionStructureManager::NewImplicitConvex(
									(*TransformToConvexIndices)[TransformGroupIndex].Array(),
									ConvexGeometry,
									SizeSpecificData.CollisionShapesData[0].CollisionType,
									CollectionMassToLocal[TransformGroupIndex],
									(Chaos::FReal)SizeSpecificData.CollisionShapesData[0].CollisionMarginFraction)
							);
						}
					}
					else if (SizeSpecificData.CollisionShapesData[0].ImplicitType == EImplicitTypeEnum::Chaos_Implicit_Capsule)
					{
						CollectionImplicits[ClusterTransformIdx] = FGeometryDynamicCollection::FSharedImplicit(
							FCollisionStructureManager::NewImplicitCapsule(
								InstanceBoundingBox,
								SizeSpecificData.CollisionShapesData[0].CollisionObjectReductionPercentage,
								SizeSpecificData.CollisionShapesData[0].CollisionType));

						CollectionSimplicials[ClusterTransformIdx] = TUniquePtr<FSimplicial>(
							FCollisionStructureManager::NewSimplicial(MassSpaceParticles, *UnionMesh, CollectionImplicits[ClusterTransformIdx].Get(),
								SharedParams.MaximumCollisionParticleCount));
					}
					else // Assume it's a union???
					{
						CollectionImplicits[ClusterTransformIdx].Reset();	//union so just set as null
						CollectionSimplicials[ClusterTransformIdx].Reset();
					}
				}

				TriangleMeshesArray[ClusterTransformIdx] = MoveTemp(UnionMesh);
			}
		}

		InitRemoveOnFracture(RestCollection, SharedParams);
	}
}

void FGeometryCollectionPhysicsProxy::InitRemoveOnFracture(FGeometryCollection& RestCollection, const FSharedSimulationParameters& SharedParams)
{
	if (SharedParams.RemoveOnFractureIndices.Num() == 0)
	{
		return;
	}

	// Markup Node Hierarchy Status with FS_RemoveOnFracture flags where geometry is ALL glass
	const int32 NumGeometries = RestCollection.NumElements(FGeometryCollection::GeometryGroup);
	for (int32 Idx = 0; Idx < NumGeometries; Idx++)
	{
		const int32 TransformIndex = RestCollection.TransformIndex[Idx];
		const int32 Start = RestCollection.FaceStart[Idx];
		const int32 End = RestCollection.FaceCount[Idx];
		bool IsToBeRemoved = true;
		for (int32 Face = Start; Face < Start + End; Face++)
		{
			bool FoundMatch = false;
			for (int32 MaterialIndex : SharedParams.RemoveOnFractureIndices)
			{
				if (RestCollection.MaterialID[Face] == MaterialIndex)
				{
					FoundMatch = true;
					break;
				}
			}
			if (!FoundMatch)
			{
				IsToBeRemoved = false;
				break;
			}
		}
		if (IsToBeRemoved)
		{
			RestCollection.SetFlags(TransformIndex, FGeometryCollection::FS_RemoveOnFracture);
		}
		else
		{
			RestCollection.ClearFlags(TransformIndex, FGeometryCollection::FS_RemoveOnFracture);
		}
	}
}

void IdentifySimulatableElements(Chaos::FErrorReporter& ErrorReporter, FGeometryCollection& GeometryCollection)
{
	// Determine which collection particles to simulate

	// Geometry group
	const TManagedArray<int32>& TransformIndex = GeometryCollection.TransformIndex;
	const TManagedArray<FBox>& BoundingBox = GeometryCollection.BoundingBox;
	const TManagedArray<int32>& VertexCount = GeometryCollection.VertexCount;

	const int32 NumTransforms = GeometryCollection.NumElements(FGeometryCollection::TransformGroup);
	const int32 NumTransformMappings = TransformIndex.Num();

	// Faces group
	const TManagedArray<FIntVector>& Indices = GeometryCollection.Indices;
	const TManagedArray<bool>& Visible = GeometryCollection.Visible;
	// Vertices group
	const TManagedArray<int32>& BoneMap = GeometryCollection.BoneMap;

	// Do not simulate hidden geometry
	TArray<bool> HiddenObject;
	HiddenObject.Init(true, NumTransforms);
	int32 PrevObject = INDEX_NONE;
	bool bContiguous = true;
	for(int32 i = 0; i < Indices.Num(); i++)
	{
		if(Visible[i]) // Face index i is visible
		{
			const int32 ObjIdx = BoneMap[Indices[i][0]]; // Look up associated bone to the faces X coord.
			HiddenObject[ObjIdx] = false;

			if (!ensure(ObjIdx >= PrevObject))
			{
				bContiguous = false;
			}

			PrevObject = ObjIdx;
		}
	}

	if (!bContiguous)
	{
		// What assumptions???  How are we ever going to know if this is still the case?
		ErrorReporter.ReportError(TEXT("Objects are not contiguous. This breaks assumptions later in the pipeline"));
		ErrorReporter.HandleLatestError();
	}

	//For now all simulation data is a non compiled attribute. Not clear what we want for simulated vs kinematic collections
	TManagedArray<bool>& SimulatableParticles = 
		GeometryCollection.AddAttribute<bool>(
			FGeometryCollection::SimulatableParticlesAttribute, FTransformCollection::TransformGroup);
	SimulatableParticles.Fill(false);

	for(int i = 0; i < NumTransformMappings; i++)
	{
		int32 Tdx = TransformIndex[i];
		checkSlow(0 <= Tdx && Tdx < NumTransforms);
		if (GeometryCollection.IsGeometry(Tdx) && // checks that TransformToGeometryIndex[Tdx] != INDEX_NONE
			VertexCount[i] &&					 // must have vertices to be simulated?
			0.f < BoundingBox[i].GetSize().SizeSquared() && // must have a non-zero bbox to be simulated?  No single point?
			!HiddenObject[Tdx])					 // must have 1 associated face
		{
			SimulatableParticles[Tdx] = true;
		}
	}
}

void BuildSimulationData(Chaos::FErrorReporter& ErrorReporter, FGeometryCollection& GeometryCollection, const FSharedSimulationParameters& SharedParams)
{
	IdentifySimulatableElements(ErrorReporter, GeometryCollection);
	FGeometryCollectionPhysicsProxy::InitializeSharedCollisionStructures(ErrorReporter, GeometryCollection, SharedParams);
}

//==============================================================================
// FIELDS
//==============================================================================

void FGeometryCollectionPhysicsProxy::FieldParameterUpdateCallback(Chaos::FPBDRigidsSolver* RigidSolver, const bool bUpdateViews)
{
	SCOPE_CYCLE_COUNTER(STAT_ParamUpdateField_Object);

	// We are updating the Collection from the InitializeBodiesPT, so we need the PT collection
	FGeometryDynamicCollection& Collection = PhysicsThreadCollection;
	Chaos::FPBDPositionConstraints PositionTarget;
	TMap<int32, int32> TargetedParticles;

	// Process Particle-Collection commands
	int32 NumCommands = Commands.Num();
	if (NumCommands && RigidSolver && !RigidSolver->IsShuttingDown() && Collection.Transform.Num())
	{
		TArray<int32> CommandsToRemove;
		CommandsToRemove.Reserve(NumCommands);

		EFieldResolutionType PrevResolutionType = EFieldResolutionType::Field_Resolution_Max;
		EFieldFilterType PrevFilterType = EFieldFilterType::Field_Filter_Max;
		EFieldObjectType PrevObjectType = EFieldObjectType::Field_Object_Max;
		EFieldPositionType PrevPositionType = EFieldPositionType::Field_Position_Max;

		for (int32 CommandIndex = 0; CommandIndex < NumCommands; CommandIndex++)
		{
			FFieldSystemCommand& FieldCommand = Commands[CommandIndex];
			if (IsParameterFieldValid(FieldCommand) || FieldCommand.PhysicsType == EFieldPhysicsType::Field_InitialLinearVelocity || FieldCommand.PhysicsType == EFieldPhysicsType::Field_InitialAngularVelocity)
			{
				if (Chaos::BuildFieldSamplePoints(this, RigidSolver, FieldCommand, ExecutionDatas, PrevResolutionType, PrevFilterType, PrevObjectType, PrevPositionType))
				{
					const Chaos::FReal TimeSeconds = RigidSolver->GetSolverTime() - FieldCommand.TimeCreation;

					FFieldContext FieldContext(
						ExecutionDatas,
						FieldCommand.MetaData,
						TimeSeconds);

					TArray<Chaos::FGeometryParticleHandle*>& ParticleHandles = ExecutionDatas.ParticleHandles[(uint8)EFieldCommandHandlesType::InsideHandles];

					if (FieldCommand.RootNode->Type() == FFieldNodeBase::EFieldType::EField_Int32)
					{
						TArray<int32>& FinalResults = ExecutionDatas.IntegerResults[(uint8)EFieldCommandResultType::FinalResult];
						ResetResultsArray < int32 >(ExecutionDatas.SamplePositions.Num(), FinalResults, 0);

						TFieldArrayView<int32> ResultsView(FinalResults, 0, FinalResults.Num());

						if (FieldCommand.PhysicsType == EFieldPhysicsType::Field_DynamicState)
						{
							SCOPE_CYCLE_COUNTER(STAT_ParamUpdateField_DynamicState);
							{
								bool bHasStateChanged = false;
								InitDynamicStateResults(ParticleHandles, FieldContext, FinalResults);

								static_cast<const FFieldNode<int32>*>(FieldCommand.RootNode.Get())->Evaluate(FieldContext, ResultsView);
								for (const FFieldContextIndex& Index : FieldContext.GetEvaluatedSamples())
								{
									Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* RigidHandle = ParticleHandles[Index.Sample]->CastToRigidParticle();
									if (RigidHandle)
									{
										const int32 CurrResult = ResultsView[Index.Result];
										check(CurrResult <= std::numeric_limits<int8>::max() &&
											CurrResult >= std::numeric_limits<int8>::min());

										const int8 ResultState = static_cast<int8>(CurrResult);
										const int32 TransformIndex = HandleToTransformGroupIndex[RigidHandle];

										// Update of the handles object state. No need to update 
										// the initial velocities since it is done after this function call in InitializeBodiesPT
										if (bUpdateViews && (Parameters.InitialVelocityType == EInitialVelocityTypeEnum::Chaos_Initial_Velocity_User_Defined))
										{
											bHasStateChanged |= ReportDynamicStateResult(RigidSolver, static_cast<Chaos::EObjectStateType>(ResultState), RigidHandle,
												true, Collection.InitialLinearVelocity[TransformIndex], true, Collection.InitialAngularVelocity[TransformIndex]);
										}
										else
										{
											bHasStateChanged |= ReportDynamicStateResult(RigidSolver, static_cast<Chaos::EObjectStateType>(ResultState), RigidHandle,
												false, Chaos::FVec3(0), false, Chaos::FVec3(0));
										}
										// Update of the Collection dynamic state. It will be used just after to set the initial velocity
										Collection.DynamicState[TransformIndex] = ResultState;
									}
								}
								if (bUpdateViews)
								{
									UpdateSolverParticlesState(RigidSolver, bHasStateChanged);
								}
							}
						}
						else
						{
							Chaos::FieldIntegerParameterUpdate(RigidSolver, FieldCommand, ExecutionDatas.ParticleHandles[(uint8)EFieldCommandHandlesType::InsideHandles],
								FieldContext, PositionTarget, TargetedParticles, FinalResults);
						}
					}
					else if (FieldCommand.RootNode->Type() == FFieldNodeBase::EFieldType::EField_FVector)
					{
						TArray<FVector>& FinalResults = ExecutionDatas.VectorResults[(uint8)EFieldCommandResultType::FinalResult];
						ResetResultsArray < FVector >(ExecutionDatas.SamplePositions.Num(), FinalResults, FVector::ZeroVector);

						TFieldArrayView<FVector> ResultsView(FinalResults, 0, FinalResults.Num());

						if (FieldCommand.PhysicsType == EFieldPhysicsType::Field_InitialLinearVelocity)
						{
							if (Parameters.InitialVelocityType == EInitialVelocityTypeEnum::Chaos_Initial_Velocity_User_Defined)
							{
								SCOPE_CYCLE_COUNTER(STAT_ParamUpdateField_LinearVelocity);
								{
									static_cast<const FFieldNode<FVector>*>(FieldCommand.RootNode.Get())->Evaluate(FieldContext, ResultsView);
									for (const FFieldContextIndex& Index : FieldContext.GetEvaluatedSamples())
									{
										Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* RigidHandle = ParticleHandles[Index.Sample]->CastToRigidParticle();
										if (RigidHandle)
										{
											Collection.InitialLinearVelocity[HandleToTransformGroupIndex[RigidHandle]] = FVector3f(ResultsView[Index.Result]);
										}
									}
								}
							}
							else
							{
								UE_LOG(LogChaos, Error, TEXT("Field based evaluation of the simulations 'InitialLinearVelocity' requires the geometry collection be set to User Defined Initial Velocity"));
							}
						}
						else if (FieldCommand.PhysicsType == EFieldPhysicsType::Field_InitialAngularVelocity)
						{
							if (Parameters.InitialVelocityType == EInitialVelocityTypeEnum::Chaos_Initial_Velocity_User_Defined)
							{
								SCOPE_CYCLE_COUNTER(STAT_ParamUpdateField_AngularVelocity);
								{
									static_cast<const FFieldNode<FVector>*>(FieldCommand.RootNode.Get())->Evaluate(FieldContext, ResultsView);
									for (const FFieldContextIndex& Index : FieldContext.GetEvaluatedSamples())
									{
										Chaos::TPBDRigidParticleHandle<Chaos::FReal, 3>* RigidHandle = ParticleHandles[Index.Sample]->CastToRigidParticle();
										if (RigidHandle)
										{
											Collection.InitialAngularVelocity[HandleToTransformGroupIndex[RigidHandle]] = FVector3f(ResultsView[Index.Result]);
										}
									}
								}
							}
							else
							{
								UE_LOG(LogChaos, Error, TEXT("Field based evaluation of the simulations 'InitialAngularVelocity' requires the geometry collection be set to User Defined Initial Velocity"));
							}
						}
						else
						{
							Chaos::FieldVectorParameterUpdate(RigidSolver, FieldCommand, ParticleHandles,
								FieldContext, PositionTarget, TargetedParticles, FinalResults);
						}
					}
					else if (FieldCommand.RootNode->Type() == FFieldNodeBase::EFieldType::EField_Float)
					{
						TArray<float>& FinalResults = ExecutionDatas.ScalarResults[(uint8)EFieldCommandResultType::FinalResult];
						ResetResultsArray<float>(ExecutionDatas.SamplePositions.Num(), FinalResults, 0.0f);

						TFieldArrayView<float> ResultsView(FinalResults, 0, FinalResults.Num());

						Chaos::FieldScalarParameterUpdate(RigidSolver, FieldCommand, ParticleHandles,
							FieldContext, PositionTarget, TargetedParticles, FinalResults);
					}
				}
				CommandsToRemove.Add(CommandIndex);
			}
		}		
		
		for (int32 Index = CommandsToRemove.Num() - 1; Index >= 0; --Index)
		{
			Commands.RemoveAt(CommandsToRemove[Index]);
		}
	}
}

void FGeometryCollectionPhysicsProxy::FieldForcesUpdateCallback(Chaos::FPBDRigidsSolver* RigidSolver)
{
	SCOPE_CYCLE_COUNTER(STAT_ForceUpdateField_Object);

	const int32 NumCommands = Commands.Num();
	if (NumCommands && RigidSolver && !RigidSolver->IsShuttingDown())
	{
		TArray<int32> CommandsToRemove;
		CommandsToRemove.Reserve(NumCommands);

		EFieldResolutionType PrevResolutionType = EFieldResolutionType::Field_Resolution_Max;
		EFieldFilterType PrevFilterType = EFieldFilterType::Field_Filter_Max;
		EFieldObjectType PrevObjectType = EFieldObjectType::Field_Object_Max;
		EFieldPositionType PrevPositionType = EFieldPositionType::Field_Position_Max;

		for (int32 CommandIndex = 0; CommandIndex < NumCommands; CommandIndex++)
		{
			const FFieldSystemCommand& FieldCommand = Commands[CommandIndex];
			if (IsForceFieldValid(FieldCommand))
			{
				if (Chaos::BuildFieldSamplePoints(this, RigidSolver, FieldCommand, ExecutionDatas, PrevResolutionType, PrevFilterType, PrevObjectType, PrevPositionType))
				{
					const Chaos::FReal TimeSeconds = RigidSolver->GetSolverTime() - FieldCommand.TimeCreation;

					FFieldContext FieldContext(
						ExecutionDatas,
						FieldCommand.MetaData,
						TimeSeconds);

					TArray<Chaos::FGeometryParticleHandle*>& ParticleHandles = ExecutionDatas.ParticleHandles[(uint8)EFieldCommandHandlesType::InsideHandles];

					if (FieldCommand.RootNode->Type() == FFieldNode<FVector>::StaticType())
					{
						TArray<FVector>& FinalResults = ExecutionDatas.VectorResults[(uint8)EFieldCommandResultType::FinalResult];
						ResetResultsArray < FVector >(ExecutionDatas.SamplePositions.Num(), FinalResults, FVector::ZeroVector);

						Chaos::FieldVectorForceUpdate(RigidSolver, FieldCommand, ParticleHandles,
							FieldContext, FinalResults);
					}
				}
				CommandsToRemove.Add(CommandIndex);
			}
		}
		for (int32 Index = CommandsToRemove.Num() - 1; Index >= 0; --Index)
		{
			Commands.RemoveAt(CommandsToRemove[Index]);
		}
	}
}
