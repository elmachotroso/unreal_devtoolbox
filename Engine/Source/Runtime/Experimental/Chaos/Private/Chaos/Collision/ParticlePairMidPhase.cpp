// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Collision/ParticlePairMidPhase.h"
#include "Chaos/Collision/CollisionConstraintAllocator.h"
#include "Chaos/Collision/CollisionContext.h"
#include "Chaos/Collision/CollisionFilter.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/CollisionResolution.h"
#include "Chaos/ImplicitObject.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDCollisionConstraints.h"

#include "ChaosStats.h"

extern bool Chaos_Collision_NarrowPhase_AABBBoundsCheck;

namespace Chaos
{
	namespace CVars
	{
		bool bChaos_Collision_MidPhase_EnableBoundsChecks = true;
		FAutoConsoleVariableRef CVarChaos_Collision_EnableBoundsChecks(TEXT("p.Chaos.Collision.EnableBoundsChecks"), bChaos_Collision_MidPhase_EnableBoundsChecks, TEXT(""));

		Chaos::FRealSingle Chaos_Collision_CullDistanceScaleInverseSize = 0.01f;	// 100cm
		Chaos::FRealSingle Chaos_Collision_MinCullDistanceScale = 1.0f;
		FAutoConsoleVariableRef CVarChaos_Collision_CullDistanceReferenceSize(TEXT("p.Chaos.Collision.CullDistanceReferenceSize"), Chaos_Collision_CullDistanceScaleInverseSize, TEXT(""));
		FAutoConsoleVariableRef CVarChaos_Collision_MinCullDistanecScale(TEXT("p.Chaos.Collision.MinCullDistanceScale"), Chaos_Collision_MinCullDistanceScale, TEXT(""));

	}
	using namespace CVars;

	inline bool ImplicitOverlapOBBToAABB(
		const FImplicitObject* Implicit0, 
		const FImplicitObject* Implicit1, 
		const FRigidTransform3& ShapeWorldTransform0, 
		const FRigidTransform3& ShapeWorldTransform1, 
		const FReal CullDistance)
	{
		if (Implicit0->HasBoundingBox() && Implicit1->HasBoundingBox())
		{
			const FRigidTransform3 Box1ToBox0TM = ShapeWorldTransform1.GetRelativeTransform(ShapeWorldTransform0);
			const FAABB3 Box1In0 = Implicit1->CalculateTransformedBounds(Box1ToBox0TM).Thicken(CullDistance);
			const FAABB3 Box0 = Implicit0->BoundingBox();
			return Box0.Intersects(Box1In0);
		}
		return true;
	}

	TUniquePtr<FPBDCollisionConstraint> CreateShapePairConstraint(
		FGeometryParticleHandle* Particle0,
		const FPerShapeData* InShape0,
		FGeometryParticleHandle* Particle1,
		const FPerShapeData* InShape1,
		const FReal CullDistance,
		const EContactShapesType ShapePairType,
		const bool bUseManifold)
	{
		const FImplicitObject* Implicit0 = InShape0->GetLeafGeometry();
		const FBVHParticles* BVHParticles0 = FConstGenericParticleHandle(Particle0)->CollisionParticles().Get();
		const FRigidTransform3& ShapeRelativeTransform0 = (FRigidTransform3)InShape0->GetLeafRelativeTransform();
		const FImplicitObject* Implicit1 = InShape1->GetLeafGeometry();
		const FBVHParticles* BVHParticles1 = FConstGenericParticleHandle(Particle1)->CollisionParticles().Get();
		const FRigidTransform3& ShapeRelativeTransform1 = (FRigidTransform3)InShape1->GetLeafRelativeTransform();

		return FPBDCollisionConstraint::Make(Particle0, Implicit0, InShape0, BVHParticles0, ShapeRelativeTransform0, Particle1, Implicit1, InShape1, BVHParticles1, ShapeRelativeTransform1, CullDistance, bUseManifold, ShapePairType);
	}

	TUniquePtr<FPBDCollisionConstraint> CreateImplicitPairConstraint(
		FGeometryParticleHandle* Particle0,
		const FImplicitObject* Implicit0,
		const FPerShapeData* Shape0,
		const FBVHParticles* BVHParticles0,
		const FRigidTransform3& ShapeRelativeTransform0,
		FGeometryParticleHandle* Particle1,
		const FImplicitObject* Implicit1,
		const FPerShapeData* Shape1,
		const FBVHParticles* BVHParticles1,
		const FRigidTransform3& ShapeRelativeTransform1,
		const FReal CullDistance,
		const EContactShapesType ShapePairType,
		const bool bUseManifold)
	{
		return FPBDCollisionConstraint::Make(Particle0, Implicit0, Shape0, BVHParticles0, ShapeRelativeTransform0, Particle1, Implicit1, Shape1, BVHParticles1, ShapeRelativeTransform1, CullDistance, bUseManifold, ShapePairType);
	}


	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////


	FSingleShapePairCollisionDetector::FSingleShapePairCollisionDetector(
		FGeometryParticleHandle* InParticle0,
		const FPerShapeData* InShape0,
		FGeometryParticleHandle* InParticle1,
		const FPerShapeData* InShape1,
		const EContactShapesType InShapePairType, 
		FParticlePairMidPhase& InMidPhase)
		: MidPhase(InMidPhase)
		, Constraint(nullptr)
		, Particle0(InParticle0)
		, Particle1(InParticle1)
		, Shape0(InShape0)
		, Shape1(InShape1)
		, ShapePairType(InShapePairType)
		, SphereBoundsCheckSize(0)
		, LastUsedEpoch(-1)
		, Flags()
	{
		const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
		const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();
		const bool bHasBounds0 = (Implicit0 != nullptr) && Implicit0->HasBoundingBox();
		const bool bHasBounds1 = (Implicit1 != nullptr) && Implicit1->HasBoundingBox();
		const EImplicitObjectType ImplicitType0 = (Implicit0 != nullptr) ? GetInnerType(Implicit0->GetCollisionType()) : ImplicitObjectType::Unknown;
		const EImplicitObjectType ImplicitType1 = (Implicit1 != nullptr) ? GetInnerType(Implicit1->GetCollisionType()) : ImplicitObjectType::Unknown;
		const bool bIsSphere0 = (ImplicitType0 == ImplicitObjectType::Sphere);
		const bool bIsSphere1 = (ImplicitType1 == ImplicitObjectType::Sphere);
		const bool bIsCapsule0 = (ImplicitType0 == ImplicitObjectType::Capsule);
		const bool bIsCapsule1 = (ImplicitType1 == ImplicitObjectType::Capsule);
		const bool bIsTriangle0 = (ImplicitType0 == ImplicitObjectType::TriangleMesh) || (ImplicitType0 == ImplicitObjectType::HeightField);
		const bool bIsTriangle1 = (ImplicitType1 == ImplicitObjectType::TriangleMesh) || (ImplicitType1 == ImplicitObjectType::HeightField);
		const bool bIsLevelSet = ((ShapePairType == EContactShapesType::LevelSetLevelSet) || (ShapePairType == EContactShapesType::Unknown));

		const bool bAllowBoundsChecked = bChaos_Collision_MidPhase_EnableBoundsChecks && bHasBounds0 && bHasBounds1;
		Flags.bEnableAABBCheck = bAllowBoundsChecked && !(bIsSphere0 && bIsSphere1);	// No AABB test if both are spheres
		Flags.bEnableOBBCheck0 = bAllowBoundsChecked && !bIsSphere0;					// No OBB test for spheres
		Flags.bEnableOBBCheck1 = bAllowBoundsChecked && !bIsSphere1;					// No OBB test for spheres
		
		if (bAllowBoundsChecked && bIsSphere0 && bIsSphere1)
		{
			SphereBoundsCheckSize = Implicit0->GetMargin() + Implicit1->GetMargin();	// Sphere-Sphere bounds test
		}

		// Do not try to reuse manifold points for capsules or spheres (against anything)
		// NOTE: This can also be disabled for all shape types by the solver (see GenerateCollisionImpl and the Context)
		Flags.bEnableManifoldUpdate = !bIsSphere0 && !bIsSphere1 && !bIsCapsule0 && !bIsCapsule1 && !bIsTriangle0 && !bIsTriangle1 && !bIsLevelSet;
	}

	FSingleShapePairCollisionDetector::~FSingleShapePairCollisionDetector()
	{
	}

	FSingleShapePairCollisionDetector::FSingleShapePairCollisionDetector(FSingleShapePairCollisionDetector&& R)
		: MidPhase(R.MidPhase)
		, Constraint(MoveTemp(R.Constraint))
		, Particle0(R.Particle0)
		, Particle1(R.Particle1)
		, Shape0(R.Shape0)
		, Shape1(R.Shape1)
		, ShapePairType(R.ShapePairType)
		, SphereBoundsCheckSize(R.SphereBoundsCheckSize)
		, Flags(R.Flags)
	{
	}

	bool FSingleShapePairCollisionDetector::DoBoundsOverlap(const FReal CullDistance)
	{
		PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_ShapeBounds);

		const FAABB3& ShapeWorldBounds0 = Shape0->GetWorldSpaceInflatedShapeBounds();
		const FAABB3& ShapeWorldBounds1 = Shape1->GetWorldSpaceInflatedShapeBounds();

		// World-space expanded bounds check
		if (Flags.bEnableAABBCheck)
		{
			if (!ShapeWorldBounds0.Intersects(ShapeWorldBounds1))
			{
				return false;
			}
		}

		// World-space sphere bounds check
		if (SphereBoundsCheckSize > FReal(0))
		{
			const FVec3 Separation = ShapeWorldBounds0.GetCenter() - ShapeWorldBounds1.GetCenter();
			const FReal SeparationSq = Separation.SizeSquared();
			const FReal CullDistanceSq = FMath::Square(CullDistance + SphereBoundsCheckSize);
			if (SeparationSq > CullDistanceSq)
			{
				return false;
			}
		}

		// OBB-AABB test on both directions. This is beneficial for shapes which do not fit their AABBs very well,
		// which includes boxes and other shapes that are not roughly spherical. It is especially beneficial when
		// one shape is long and thin (i.e., it does not fit an AABB well when the shape is rotated).
		// However, it is quite expensive to do this all the time so we only do this test when we did not 
		// collide last frame. This is ok if we assume not much changes from frame to frame, but it means
		// we might call the narrow phase one time too many when shapes become separated.
		const int32 LastEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch() - 1;
		const bool bCollidedLastTick = IsUsedSince(LastEpoch);
		if ((Flags.bEnableOBBCheck0 || Flags.bEnableOBBCheck1) && !bCollidedLastTick)
		{
			const FRigidTransform3& ShapeWorldTransform0 = Shape0->GetLeafWorldTransform(Particle0);
			const FRigidTransform3& ShapeWorldTransform1 = Shape1->GetLeafWorldTransform(Particle1);
			const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
			const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();

			if (Flags.bEnableOBBCheck0)
			{
				if (!ImplicitOverlapOBBToAABB(Implicit0, Implicit1, ShapeWorldTransform0, ShapeWorldTransform1, CullDistance))
				{
					return false;
				}
			}

			if (Flags.bEnableOBBCheck1)
			{
				if (!ImplicitOverlapOBBToAABB(Implicit1, Implicit0, ShapeWorldTransform1, ShapeWorldTransform0, CullDistance))
				{
					return false;
				}
			}
		}

		return true;
	}

	int32 FSingleShapePairCollisionDetector::GenerateCollision(
		const FReal CullDistance,
		const FReal Dt,
		FCollisionContext& Context)
	{
		if (DoBoundsOverlap(CullDistance))
		{
			return GenerateCollisionImpl(CullDistance, Dt, Context);
		}
		return 0;
	}

	int32 FSingleShapePairCollisionDetector::GenerateCollisionCCD(
		const FReal CullDistance,
		const FReal Dt,
		FCollisionContext& Context)
	{
		return GenerateCollisionCCDImpl(CullDistance, Dt, Context);
	}

	void FSingleShapePairCollisionDetector::CreateConstraint(const FReal CullDistance, FCollisionContext& Context)
	{
		PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_CreateConstraint);
		check(Constraint == nullptr);

		Constraint = CreateShapePairConstraint(Particle0, Shape0, Particle1, Shape1, CullDistance, ShapePairType, Context.bAllowManifolds);

		Constraint->GetContainerCookie().MidPhase = &MidPhase;
		Constraint->GetContainerCookie().bIsMultiShapePair = false;
		Constraint->GetContainerCookie().CreationEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();
		LastUsedEpoch = -1;
	}

	int32 FSingleShapePairCollisionDetector::GenerateCollisionImpl(
		const FReal CullDistance, 
		const FReal Dt,
		FCollisionContext& Context)
	{
		if (Constraint == nullptr)
		{
			// Lazy creation of the constraint. If a shape pair never gets within CullDistance of each
			// other, we never allocate a constraint for them. Once they overlap, we reuse the constraint
			// until the owing particles are not overlapping. i.e., we keep the constraint even if
			// the shape pairs stop overlapping, reusing it if they start overlapping again.
			CreateConstraint(CullDistance, Context);
		}

		if (Constraint != nullptr)
		{
			PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_UpdateConstraint);

			const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
			const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();
			const FRigidTransform3& ShapeWorldTransform0 = Shape0->GetLeafWorldTransform(Particle0);
			const FRigidTransform3& ShapeWorldTransform1 = Shape1->GetLeafWorldTransform(Particle1);
			const int32 CurrentEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();
			const int32 LastEpoch = CurrentEpoch - 1;
			const bool bWasUpdatedLastTick = IsUsedSince(LastEpoch);

			// Update the world shape transforms on the constraint (we cannot just give it the PerShapeData 
			// pointer because of Unions - see FMultiShapePairCollisionDetector)
			// NOTE: these are not used by CCD which continuously moves the particles
			Constraint->SetShapeWorldTransforms(ShapeWorldTransform0, ShapeWorldTransform1);

			Constraint->SetCCDEnabled(false);

			// If the constraint was not used last frame, it needs to be reset. 
			// Otherwise we will try to reuse it below
			if (!bWasUpdatedLastTick  || (Constraint->GetManifoldPoints().Num() == 0))
			{
				// Clear all manifold data including saved contact data
				Constraint->ResetManifold();
			}

			bool bWasManifoldRestored = false;
			if (Context.bAllowManifoldReuse && Flags.bEnableManifoldUpdate && bWasUpdatedLastTick)
			{
				// Update the existing manifold. We can re-use as-is if none of the points have moved much and the bodies have not moved much
				// NOTE: this can succeed in "restoring" even if we have no manifold points
				// NOTE: this uses the previous world-space shape transforms, so we can only do this if we were updated last tick
				bWasManifoldRestored = Constraint->UpdateAndTryRestoreManifold();
			}
			else
			{
				// We are not trying to reuse manifold points, so reset them but leave stored data intact (for friction)
				Constraint->ResetActiveManifoldContacts();
			}

			if (!bWasManifoldRestored)
			{
				// We will be updating the manifold, if only partially, so update the restore comparison transforms
				Constraint->SetLastShapeWorldTransforms(ShapeWorldTransform0, ShapeWorldTransform1);

				if (!Context.bDeferUpdate)
				{
					// Run the narrow phase
					Collisions::UpdateConstraint(*Constraint.Get(), ShapeWorldTransform0, ShapeWorldTransform1, Dt);
				}
			}

			// If we have a valid contact, add it to the active list
			// We also add it to the active list if collision detection is deferred (which is if per-iteration collision detection is enabled like with RBAN)
			if ((Constraint->GetPhi() <= CullDistance) || Context.bDeferUpdate)
			{
				if (MidPhase.GetCollisionAllocator().ActivateConstraint(Constraint.Get()))
				{
					LastUsedEpoch = CurrentEpoch;
					return 1;
				}
			}
		}

		return 0;
	}

	int32 FSingleShapePairCollisionDetector::GenerateCollisionCCDImpl(
		const FReal CullDistance, 
		const FReal Dt,
		FCollisionContext& Context)
	{
		if (Constraint == nullptr)
		{
			// Lazy creation of the constraint. 
			CreateConstraint(CullDistance, Context);
		}

		if (Constraint != nullptr)
		{
			PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_UpdateConstraintCCD);

			const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
			const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();
			const FRigidTransform3& ShapeWorldTransform0 = Shape0->GetLeafWorldTransform(Particle0);
			const FRigidTransform3& ShapeWorldTransform1 = Shape1->GetLeafWorldTransform(Particle1);

			// Update the world shape transforms on the constraint (we cannot just give it the PerShapeData 
			// pointer because of Unions - see FMultiShapePairCollisionDetector)
			// NOTE: these are not used by CCD which continuously moves the particles
			Constraint->SetShapeWorldTransforms(ShapeWorldTransform0, ShapeWorldTransform1);

			Constraint->SetCCDEnabled(true);
			Constraint->ResetManifold();
			Constraint->ResetActiveManifoldContacts();

			FConstGenericParticleHandle P0 = Particle0;
			FConstGenericParticleHandle P1 = Particle1;
			// For kinematic particles, X = P (at TOI=1), we need to compute P-V*dt to get position at TOI=0. 
			const FVec3 StartX0 = P0->ObjectState() == EObjectStateType::Kinematic ? P0->P() - P0->V() * Dt : P0->X();
			const FVec3 StartX1 = P1->ObjectState() == EObjectStateType::Kinematic ? P1->P() - P1->V() * Dt : P1->X();
			// Note: It is unusual that we are mixing X and Q. 
			// This is due to how CCD rewinds the position (not rotation) and then sweeps to find the first contact at the most recent orientation Q
			// NOTE: These are actor transforms, not CoM transforms
			const FRigidTransform3 CCDParticleWorldTransform0 = FRigidTransform3(StartX0, P0->Q());
			const FRigidTransform3 CCDParticleWorldTransform1 = FRigidTransform3(StartX1, P1->Q());
			const FRigidTransform3 CCDShapeWorldTransform0 = Constraint->ImplicitTransform[0] * CCDParticleWorldTransform0;
			const FRigidTransform3 CCDShapeWorldTransform1 = Constraint->ImplicitTransform[1] * CCDParticleWorldTransform1;
			Collisions::UpdateConstraintSwept(*Constraint.Get(), CCDShapeWorldTransform0, CCDShapeWorldTransform1, Dt);
			MidPhase.GetCollisionAllocator().ActivateConstraint(Constraint.Get());
			LastUsedEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();

			return 1;
		}

		return 0;
	}

	void FSingleShapePairCollisionDetector::WakeCollision(const int32 SleepEpoch)
	{
		if ((Constraint != nullptr) && IsUsedSince(SleepEpoch))
		{
			// We need to refresh the epoch so that the constraint state will be used as the previous
			// state iof the pair is still colliding in the next tick
			const int32 CurrentEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();
			Constraint->GetContainerCookie().LastUsedEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();
			LastUsedEpoch = CurrentEpoch;

			// We have skipped collision detection for this particle because it was asleep, so we need to update the transforms...
			// NOTE: this relies on the shape world transforms being up-to-date. They are usually updated in Integarte which
			// is also skipped for sleeping particles, so they must be updated manually when waking partciles (see IslandManager)
			Constraint->SetShapeWorldTransforms(Shape0->GetLeafWorldTransform(Particle0), Shape1->GetLeafWorldTransform(Particle1));
		}
	}

	void FSingleShapePairCollisionDetector::SetCollision(const FPBDCollisionConstraint& SourceConstraint)
	{
		const int32 CurrentEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();

		if (Constraint == nullptr)
		{
			Constraint = MakeUnique<FPBDCollisionConstraint>();
			Constraint->GetContainerCookie().MidPhase = &MidPhase;
			Constraint->GetContainerCookie().bIsMultiShapePair = false;
			Constraint->GetContainerCookie().CreationEpoch = CurrentEpoch;
		}

		// Copy the constraint over the existing one, taking care to leave the cookie intact
		const FPBDCollisionConstraintContainerCookie Cookie = Constraint->GetContainerCookie();
		*(Constraint.Get()) = SourceConstraint;
		Constraint->GetContainerCookie() = Cookie;

		// Add the constraint to the active list
		// If the constraint already existed and was already active, this will do nothing
		MidPhase.GetCollisionAllocator().ActivateConstraint(Constraint.Get());
		LastUsedEpoch = CurrentEpoch;
	}


	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////


	FMultiShapePairCollisionDetector::FMultiShapePairCollisionDetector(
		FGeometryParticleHandle* InParticle0,
		const FPerShapeData* InShape0,
		FGeometryParticleHandle* InParticle1,
		const FPerShapeData* InShape1,
		FParticlePairMidPhase& InMidPhase)
		: MidPhase(InMidPhase)
		, Constraints()
		, Particle0(InParticle0)
		, Particle1(InParticle1)
		, Shape0(InShape0)
		, Shape1(InShape1)
	{
	}

	FMultiShapePairCollisionDetector::FMultiShapePairCollisionDetector(FMultiShapePairCollisionDetector&& R)
		: MidPhase(R.MidPhase)
		, Constraints(MoveTemp(R.Constraints))
		, Particle0(R.Particle0)
		, Particle1(R.Particle1)
		, Shape0(R.Shape0)
		, Shape1(R.Shape1)
	{
		check(R.NewConstraints.IsEmpty());
	}

	FMultiShapePairCollisionDetector::~FMultiShapePairCollisionDetector()
	{
	}

	int32 FMultiShapePairCollisionDetector::GenerateCollisions(
		const FReal CullDistance,
		const FReal Dt,
		FCollisionContext& Context)
	{
		FConstGenericParticleHandle P0 = Particle0;
		FConstGenericParticleHandle P1 = Particle1;

		const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
		const FBVHParticles* BVHParticles0 = P0->CollisionParticles().Get();
		const FRigidTransform3& ShapeRelativeTransform0 = (FRigidTransform3)Shape0->GetLeafRelativeTransform();
		const FRigidTransform3 ParticleWorldTransform0 = FParticleUtilities::GetActorWorldTransform(P0);
		const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();
		const FBVHParticles* BVHParticles1 = P1->CollisionParticles().Get();
		const FRigidTransform3& ShapeRelativeTransform1 = (FRigidTransform3)Shape1->GetLeafRelativeTransform();
		const FRigidTransform3 ParticleWorldTransform1 = FParticleUtilities::GetActorWorldTransform(P1);

		FCollisionContext LocalContext = Context;
		LocalContext.CollisionAllocator = this;

		Collisions::ConstructConstraints(
			Particle0, 
			Particle1, 
			Implicit0, 
			Shape0,
			BVHParticles0, 
			Implicit1, 
			Shape1,
			BVHParticles1, 
			ParticleWorldTransform0,
			ShapeRelativeTransform0,
			ParticleWorldTransform1,
			ShapeRelativeTransform1,
			CullDistance,
			Dt,
			LocalContext);

		int32 NumActiveConstraints = ProcessNewConstraints();

		// @todo(chaos): we could clean up unused collisions between this pair, but probably not worth it
		// and we would have to prevent cleanup for sleeping particles because the collisions will still
		// be referenced in the IslandManager's constraint graph for the sleeping island.
		//PruneConstraints();

		return NumActiveConstraints;
	}

	FPBDCollisionConstraint* FMultiShapePairCollisionDetector::FindOrCreateConstraint(
		FGeometryParticleHandle* InParticle0,
		const FImplicitObject* Implicit0,
		const FPerShapeData* InShape0,
		const FBVHParticles* BVHParticles0,
		const FRigidTransform3& ShapeRelativeTransform0,
		FGeometryParticleHandle* InParticle1,
		const FImplicitObject* Implicit1,
		const FPerShapeData* InShape1,
		const FBVHParticles* BVHParticles1,
		const FRigidTransform3& ShapeRelativeTransform1,
		const FReal CullDistance,
		const EContactShapesType ShapePairType,
		const bool bUseManifold)
	{
		// This is a callback from the low-level collision function. It should always be the same two particles, though the
		// shapes may be some children in the implicit hierarchy. The particles could be in the opposite order though, and
		// this will depend on the shape types involved. E.g., with two particles each with a sphere and a box in a union
		// would require up to two Sphere-Box contacts, with the particles in opposite orders.
		if (!ensure(((InParticle0 == Particle0) && (InParticle1 == Particle1)) || ((InParticle0 == Particle1) && (InParticle1 == Particle0))))
		{
			// We somehow received a callback for the wrong particle pair...this should not happen
			return nullptr;
		}

		const FCollisionParticlePairConstraintKey Key = FCollisionParticlePairConstraintKey(Implicit0, BVHParticles0, Implicit1, BVHParticles1);
		FPBDCollisionConstraint* Constraint = FindConstraint(Key);

		// @todo(chaos): fix key uniqueness guarantee.  We need a truly unique key gen function
		const bool bIsKeyCollision = (Constraint != nullptr) && ((Constraint->GetImplicit0() != Implicit0) || (Constraint->GetImplicit1() != Implicit1) || (Constraint->GetCollisionParticles0() != BVHParticles0) || (Constraint->GetCollisionParticles1() != BVHParticles1));
		if (bIsKeyCollision)
		{
			// If we get here, we have a key collision. The key uses a hash of pointers which is very likely to be unique for different implicit pairs, 
			// especially since it only needs to be unique for this particle pair, but it is not guaranteed.
			// Creating a new constraint with the same key could cause fatal problems (the original constraint will be deleted when we add the new one 
			// to the map, but if it is asleep it will be referenced in the contact graph) so we just abort and accept we will miss collisions. 
			// It is extremely unlikely to happen but we should fix it at some point.
			ensure(false);
			return nullptr;
		}

		if (Constraint == nullptr)
		{
			// NOTE: Using InParticle0 and InParticle1 here because the order may be different to what we have stored
			Constraint = CreateConstraint(InParticle0, Implicit0, InShape0, BVHParticles0, ShapeRelativeTransform0, InParticle1, Implicit1, InShape1, BVHParticles1, ShapeRelativeTransform1, CullDistance, ShapePairType, bUseManifold, Key);
		}

		// @todo(chaos): we already have the shape world transforms at the calling site - pass them in
		const FRigidTransform3 ParticleTransform0 = FParticleUtilitiesPQ::GetActorWorldTransform(FConstGenericParticleHandle(InParticle0));
		const FRigidTransform3 ParticleTransform1 = FParticleUtilitiesPQ::GetActorWorldTransform(FConstGenericParticleHandle(InParticle1));
		const FRigidTransform3 ShapeWorldTransform0 = ShapeRelativeTransform0 * ParticleTransform0;
		const FRigidTransform3 ShapeWorldTransform1 = ShapeRelativeTransform1 * ParticleTransform1;
		Constraint->SetShapeWorldTransforms(ShapeWorldTransform0, ShapeWorldTransform1);

		NewConstraints.Add(Constraint);
		return Constraint;
	}


	FPBDCollisionConstraint* FMultiShapePairCollisionDetector::FindOrCreateSweptConstraint(
		FGeometryParticleHandle* InParticle0,
		const FImplicitObject* Implicit0,
		const FPerShapeData* InShape0,
		const FBVHParticles* BVHParticles0,
		const FRigidTransform3& ShapeRelativeTransform0,
		FGeometryParticleHandle* InParticle1,
		const FImplicitObject* Implicit1,
		const FPerShapeData* InShape1,
		const FBVHParticles* BVHParticles1,
		const FRigidTransform3& ShapeRelativeTransform1,
		const FReal CullDistance,
		const EContactShapesType ShapePairType)
	{
		const bool bUseManifold = true;
		FPBDCollisionConstraint* Constraint = FindOrCreateConstraint(InParticle0, Implicit0, InShape0, BVHParticles0, ShapeRelativeTransform0, InParticle1, Implicit1, InShape1, BVHParticles1, ShapeRelativeTransform1, CullDistance, ShapePairType, bUseManifold);
		if (Constraint != nullptr)
		{
			Constraint->SetCCDEnabled(true);
		}
		return Constraint;
	}

	FPBDCollisionConstraint* FMultiShapePairCollisionDetector::FindConstraint(const FCollisionParticlePairConstraintKey& Key)
	{
		TUniquePtr<FPBDCollisionConstraint>* Constraint = Constraints.Find(Key.GetKey());
		if (Constraint != nullptr)
		{
			return (*Constraint).Get();
		}
		return nullptr;
	}

	FPBDCollisionConstraint* FMultiShapePairCollisionDetector::CreateConstraint(
		FGeometryParticleHandle* InParticle0,
		const FImplicitObject* Implicit0,
		const FPerShapeData* InShape0,
		const FBVHParticles* BVHParticles0,
		const FRigidTransform3& ShapeRelativeTransform0,
		FGeometryParticleHandle* InParticle1,
		const FImplicitObject* Implicit1,
		const FPerShapeData* InShape1,
		const FBVHParticles* BVHParticles1,
		const FRigidTransform3& ShapeRelativeTransform1,
		const FReal CullDistance,
		const EContactShapesType ShapePairType,
		const bool bUseManifold,
		const FCollisionParticlePairConstraintKey& Key)
	{
		PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_CreateConstraint);
		TUniquePtr<FPBDCollisionConstraint> Constraint = CreateImplicitPairConstraint(InParticle0, Implicit0, InShape0, BVHParticles0, ShapeRelativeTransform0, InParticle1, Implicit1, InShape1, BVHParticles1, ShapeRelativeTransform1, CullDistance, ShapePairType, bUseManifold);
		
		Constraint->GetContainerCookie().MidPhase = &MidPhase;
		Constraint->GetContainerCookie().bIsMultiShapePair = true;
		Constraint->GetContainerCookie().CreationEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();

		return Constraints.Add(Key.GetKey(), MoveTemp(Constraint)).Get();
	}

	void FMultiShapePairCollisionDetector::WakeCollisions(const int32 SleepEpoch)
	{
		const int32 CurrentEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();
		for (auto& KVP : Constraints)
		{
			TUniquePtr<FPBDCollisionConstraint>& Constraint = KVP.Value;
			if (Constraint->GetContainerCookie().LastUsedEpoch >= SleepEpoch)
			{
				Constraint->GetContainerCookie().LastUsedEpoch = CurrentEpoch;
			}
		}
	}

	int32 FMultiShapePairCollisionDetector::ProcessNewConstraints()
	{
		int32 NumActiveConstraints = 0;
		for (FPBDCollisionConstraint* Constraint : NewConstraints)
		{
			if (Constraint->GetPhi() < Constraint->GetCullDistance())
			{
				MidPhase.GetCollisionAllocator().ActivateConstraint(Constraint);
				++NumActiveConstraints;
			}
		}
		NewConstraints.Reset();
		return NumActiveConstraints;
	}

	void FMultiShapePairCollisionDetector::PruneConstraints()
	{
		// We don't prune from NewCollisions - must call ProcessNewCollisions before Prune
		check(NewConstraints.Num() == 0);

		const int32 CurrentEpoch = MidPhase.GetCollisionAllocator().GetCurrentEpoch();

		// Find all the expired collisions
		TArray<uint32> Pruned;
		for (auto& KVP : Constraints)
		{
			const uint32 Key = KVP.Key;
			TUniquePtr<FPBDCollisionConstraint>& Constraint = KVP.Value;
			if (Constraint->GetContainerCookie().LastUsedEpoch < CurrentEpoch)
			{
				Pruned.Add(Key);
			}
		}

		// Destroy expired collisions
		for (uint32 Key : Pruned)
		{
			Constraints.Remove(Key);
		}
	}


	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////
	////////////////////////////////////////////////////////////////////////////////////////


	FParticlePairMidPhase::FParticlePairMidPhase()
		: Particle0(nullptr)
		, Particle1(nullptr)
		, CullDistanceScale(1)
		, Flags()
		, Key()
		, CollisionAllocator(nullptr)
		, LastUsedEpoch(INDEX_NONE)
		, NumActiveConstraints(0)
		, ParticleCollisionsIndex0(INDEX_NONE)
		, ParticleCollisionsIndex1(INDEX_NONE)
		, MultiShapePairDetectors()
		, ShapePairDetectors()
	{
	}

	FParticlePairMidPhase::~FParticlePairMidPhase()
	{
		Reset();
	}

	int32 FParticlePairMidPhase::GetCurrentEpoch() const
	{
		return CollisionAllocator->GetCurrentEpoch();
	}

	void FParticlePairMidPhase::DetachParticle(FGeometryParticleHandle* Particle)
	{
		Reset();

		if (Particle == Particle0)
		{
			Particle0 = nullptr;
		}
		else if (Particle == Particle1)
		{
			Particle1 = nullptr;
		}
	}

	void FParticlePairMidPhase::Reset()
	{
		ShapePairDetectors.Reset();
		MultiShapePairDetectors.Reset();

		Flags.bIsCCD = false;
		Flags.bIsInitialized = false;
		Flags.bIsSleeping = false;
	}

	void FParticlePairMidPhase::Init(
		FGeometryParticleHandle* InParticle0,
		FGeometryParticleHandle* InParticle1,
		const FCollisionParticlePairKey& InKey,
		FCollisionConstraintAllocator& InCollisionAllocator)
	{
		PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, NarrowPhase_Filter);

		Particle0 = InParticle0;
		Particle1 = InParticle1;
		Key = InKey;
		CollisionAllocator = &InCollisionAllocator;

		Flags.bIsCCD = FConstGenericParticleHandle(Particle0)->CCDEnabled() || FConstGenericParticleHandle(Particle1)->CCDEnabled();

		BuildDetectors();

		InitThresholds();

		Flags.bIsInitialized = true;
	}

	void FParticlePairMidPhase::BuildDetectors()
	{
		if (IsValid() && (Particle0 != Particle1))
		{
			const FShapesArray& Shapes0 = Particle0->ShapesArray();
			const FShapesArray& Shapes1 = Particle1->ShapesArray();
			for (int32 ShapeIndex0 = 0; ShapeIndex0 < Shapes0.Num(); ++ShapeIndex0)
			{
				const FPerShapeData* Shape0 = Shapes0[ShapeIndex0].Get();
				for (int32 ShapeIndex1 = 0; ShapeIndex1 < Shapes1.Num(); ++ShapeIndex1)
				{
					const FPerShapeData* Shape1 = Shapes1[ShapeIndex1].Get();
					TryAddShapePair(Shape0, Shape1);
				}
			}
		}
	}

	void FParticlePairMidPhase::TryAddShapePair(const FPerShapeData* Shape0, const FPerShapeData* Shape1)
	{
		const FImplicitObject* Implicit0 = Shape0->GetLeafGeometry();
		const FBVHParticles* BVHParticles0 = FConstGenericParticleHandle(Particle0)->CollisionParticles().Get();
		const EImplicitObjectType ImplicitType0 = (Implicit0 != nullptr) ? GetInnerType(Implicit0->GetCollisionType()) : ImplicitObjectType::Unknown;

		const FImplicitObject* Implicit1 = Shape1->GetLeafGeometry();
		const FBVHParticles* BVHParticles1 = FConstGenericParticleHandle(Particle1)->CollisionParticles().Get();
		const EImplicitObjectType ImplicitType1 = (Implicit1 != nullptr) ? GetInnerType(Implicit1->GetCollisionType()) : ImplicitObjectType::Unknown;

		const bool bDoPassFilter = DoCollide(ImplicitType0, Shape0, ImplicitType1, Shape1);
		if (bDoPassFilter)
		{
			bool bSwap = false;
			const EContactShapesType ShapePairType = Collisions::CalculateShapePairType(Implicit0, BVHParticles0, Implicit1, BVHParticles1, bSwap);

			if (ShapePairType != EContactShapesType::Unknown)
			{
				if (!bSwap)
				{
					ShapePairDetectors.Emplace(FSingleShapePairCollisionDetector(Particle0, Shape0, Particle1, Shape1, ShapePairType, *this));
				}
				else
				{
					ShapePairDetectors.Emplace(FSingleShapePairCollisionDetector(Particle1, Shape1, Particle0, Shape0, ShapePairType, *this));
				}
			}
			else
			{
				if (ensure(!bSwap))
				{
					MultiShapePairDetectors.Emplace(FMultiShapePairCollisionDetector(Particle0, Shape0, Particle1, Shape1, *this));
				}
			}
		}
	}

	bool FParticlePairMidPhase::ShouldEnableCCD(const FReal Dt)
	{
		if (Flags.bIsCCD)
		{
			FConstGenericParticleHandle ConstParticle0 = FConstGenericParticleHandle(Particle0);
			FConstGenericParticleHandle ConstParticle1 = FConstGenericParticleHandle(Particle1);

			FReal LengthCCD = 0;
			FVec3 DirCCD = FVec3(0);
			const FVec3 DeltaX0 = (ConstParticle0->ObjectState() == EObjectStateType::Kinematic) ? (ConstParticle0->V() * Dt) : (ConstParticle0->P() - ConstParticle0->X());
			const FVec3 DeltaX1 = (ConstParticle1->ObjectState() == EObjectStateType::Kinematic) ? (ConstParticle1->V() * Dt) : (ConstParticle1->P() - ConstParticle1->X());
			const bool bUseCCD = Collisions::ShouldUseCCD(Particle0, DeltaX0, Particle1, DeltaX1, DirCCD, LengthCCD, false);

			return bUseCCD;
		}
		return false;
	}

	void FParticlePairMidPhase::InitThresholds()
	{
		// @todo(chaos): improve this threshold calculation for thin objects? Dynamic thin objects have bigger problems so maybe we don't care
		// @todo(chaos): Spheres and capsules need smaller position tolerance - the restore test doesn't work well with rolling
		bool bIsDynamic0 = FConstGenericParticleHandle(Particle0)->IsDynamic();
		bool bIsDynamic1 = FConstGenericParticleHandle(Particle1)->IsDynamic();

		// NOTE: If CullDistance ends up smaller than the thresholds used to restore collisions, we can end up missing
		// collisions as the objects move if we restore a "zero contact" manifold after movement greater than the cull distance. 
		// Currently this should not happen, but it is not explicitly ensured by the way the thresholds and CullDistanceScale are calculated.
		// @todo(chaos): Add a way to enforce a CullDistance big enough to support the reuse thresholds
		const FReal CullDistanceReferenceSizeInv = FReal(Chaos_Collision_CullDistanceScaleInverseSize);
		const FReal MinCullDistanceScale = FReal(Chaos_Collision_MinCullDistanceScale);
		const FReal MaxBoundsSize0 = bIsDynamic0 ? Particle0->LocalBounds().Extents().GetMax() : FReal(0);
		const FReal MaxBoundsSize1 = bIsDynamic1 ? Particle1->LocalBounds().Extents().GetMax() : FReal(0);
		const FReal CullDistanceScale0 = MaxBoundsSize0 * CullDistanceReferenceSizeInv;
		const FReal CullDistanceScale1 = MaxBoundsSize1 * CullDistanceReferenceSizeInv;
		CullDistanceScale = (FRealSingle) FMath::Max3(CullDistanceScale0, CullDistanceScale1, MinCullDistanceScale);
	}

	void FParticlePairMidPhase::GenerateCollisions(
		const FReal InCullDistance,
		const FReal Dt,
		FCollisionContext& Context)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_GenerateCollisions);
		PHYSICS_CSV_SCOPED_EXPENSIVE(PhysicsVerbose, DetectCollisions_NarrowPhase);

		if (!IsValid())
		{
			return;
		}

		// CullDistance is scaled by the size of the dynamic objects.
		const FReal CullDistance = InCullDistance * CullDistanceScale;

		// Enable CCD?
		const bool bUseCCD = Flags.bIsCCD && ShouldEnableCCD(Dt);
		
		// Run collision detection on all potentially colliding shape pairs
		NumActiveConstraints = 0;
		if (bUseCCD) // Generate CCD constraints as long as AABBs overlap
		{
			for (FSingleShapePairCollisionDetector& ShapePair : ShapePairDetectors)
			{
				NumActiveConstraints += ShapePair.GenerateCollisionCCD(CullDistance, Dt, Context);
			}
		}
		else
		{
			for (FSingleShapePairCollisionDetector& ShapePair : ShapePairDetectors)
			{
				NumActiveConstraints += ShapePair.GenerateCollision(CullDistance, Dt, Context);
			}
		}
		for (FMultiShapePairCollisionDetector& MultiShapePair : MultiShapePairDetectors)
		{
			NumActiveConstraints += MultiShapePair.GenerateCollisions(CullDistance, Dt, Context);
		}

		LastUsedEpoch = CollisionAllocator->GetCurrentEpoch();
	}

	void FParticlePairMidPhase::InjectCollision(const FPBDCollisionConstraint& Constraint)
	{
		if (!Constraint.GetContainerCookie().bIsMultiShapePair)
		{
			const FPerShapeData* Shape0 = Constraint.GetShape0();
			const FPerShapeData* Shape1 = Constraint.GetShape1();

			// @todo(chaos): fix O(N) search for shape pair - store the index in the cookie (it will be the same
			// as long as the ShapesArray on each particle has not changed)
			for (FSingleShapePairCollisionDetector& ShapePair : ShapePairDetectors)
			{
				if (((Shape0 == ShapePair.GetShape0()) && (Shape1 == ShapePair.GetShape1())) || ((Shape0 == ShapePair.GetShape1()) && (Shape1 == ShapePair.GetShape0())))
				{
					ShapePair.SetCollision(Constraint);
				}
			}
		}
		else
		{
			// @todo(chaos): implement cluster Resim restore
			ensure(false);
		}
	}

	void FParticlePairMidPhase::SetIsSleeping(const bool bInIsSleeping)
	{
		// This can be called from two locations:
		// 1)	At the start of the tick as a results of some state change from the game thread such as an explicit wake event,
		//		applying an impulse, or moving a particle.
		// 2)	After the constraint solver phase when we put non-moving islands to sleep.
		// 
		// Note that in both cases there is a collision detection phase before the next constraint solving phase.
		//
		// When awakening we re-activate collisions so that we have a "previous" collision to use for static friction etc.
		// We don't need to do anything when going to sleep because sleeping particles pairs are ignored in collision detection 
		// so the next set of active collisions generated will not contain these collisions.

		if (Flags.bIsSleeping != bInIsSleeping)
		{
			// If we are waking particles, reactivate all collisions that were
			// active when we were put to sleep, i.e., all collisions whose LastUsedEpoch
			// is equal to our LastUsedEpoch.
			const bool bWakingUp = !bInIsSleeping;
			if (bWakingUp)
			{
				if (LastUsedEpoch < CollisionAllocator->GetCurrentEpoch())
				{
					// Restore all constraints that were active when we were put to sleep
					for (FSingleShapePairCollisionDetector& ShapePair : ShapePairDetectors)
					{
						ShapePair.WakeCollision(LastUsedEpoch);
					}
					for (FMultiShapePairCollisionDetector& MultiShapePair : MultiShapePairDetectors)
					{
						MultiShapePair.WakeCollisions(LastUsedEpoch);
					}
					LastUsedEpoch = CollisionAllocator->GetCurrentEpoch();
				}
			}
			// If we are going to sleep, there is nothing to do (see comments above)

			Flags.bIsSleeping = bInIsSleeping;
		}
	}

}


