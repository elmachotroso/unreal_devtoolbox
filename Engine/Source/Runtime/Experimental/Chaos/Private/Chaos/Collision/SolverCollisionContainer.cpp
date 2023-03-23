// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Collision/SolverCollisionContainer.h"

#include "Chaos/Collision/CollisionApplyType.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/Evolution/SolverBodyContainer.h"
#include "Chaos/Particle/ParticleUtilities.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/PBDCollisionConstraintsContact.h"
#include "Chaos/Utilities.h"

// Private includes
#include "PBDCollisionSolver.h"



namespace Chaos
{
	namespace CVars
	{
		extern bool bChaos_PBDCollisionSolver_VectorRegister;

		extern bool bChaos_PBDCollisionSolver_Position_SolveEnabled;
		extern bool bChaos_PBDCollisionSolver_Velocity_SolveEnabled;
	}
	using namespace CVars;


	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////


	FPBDCollisionSolverSettings::FPBDCollisionSolverSettings()
		: MaxPushOutVelocity(0)
		, NumPositionFrictionIterations(4)
		, NumVelocityFrictionIterations(1)
		, NumPositionShockPropagationIterations(3)
		, NumVelocityShockPropagationIterations(1)
	{
	}


	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////


	/**
	 * @brief A wrapper for FPBDCollisionSolver which binds to a Collision Constraint and adds Gather/Scatter from/to the constraint
	*/
	class FPBDCollisionSolverAdapter
	{
	public:
		FPBDCollisionSolverAdapter()
			: Constraint(nullptr)
			, bIsIncremental(false)
		{
		}

		FPBDCollisionSolver& GetSolver() { return Solver; }
		FPBDCollisionConstraint* GetConstraint() { return Constraint; }

		bool IsIncrementalManifold() const { return bIsIncremental; }

		void PreGatherInput(
			FPBDCollisionConstraint& InConstraint,
			FSolverBodyContainer& SolverBodyContainer)
		{
			Constraint = &InConstraint;
			SolverBodyContainer.FindOrAdd(Constraint->GetParticle0());
			SolverBodyContainer.FindOrAdd(Constraint->GetParticle1());
		}

		/**
		 * @brief Initialize the data required for the solver, and bind to the bodies
		*/
		void GatherInput(
			const FReal Dt,
			FPBDCollisionConstraint& InConstraint,
			const int32 Particle0Level,
			const int32 Particle1Level,
			FSolverBodyContainer& SolverBodyContainer,
			const FPBDCollisionSolverSettings& SolverSettings)
		{
			bIsIncremental = Constraint->GetUseIncrementalCollisionDetection();

			// Find the solver bodies for the particles we constrain. This will add them to the container
			// if they aren't there already, and ensure that they are populated with the latest data.
			FSolverBody* Body0 = SolverBodyContainer.FindOrAdd(Constraint->GetParticle0());
			FSolverBody* Body1 = SolverBodyContainer.FindOrAdd(Constraint->GetParticle1());

			Body0->SetLevel(Particle0Level);
			Body1->SetLevel(Particle1Level);

			// Friction values. Static and Dynamic friction are applied in the position solve for most shapes.
			// For quadratic shapes, we run dynamic friction in the velocity solve for better rolling behaviour.
			// We can also run in a mode without static friction at all. This is faster but stacking is not possible.
			// @todo(chaos): fix static/dynamic friction for quadratic shapes
			const FSolverReal StaticFriction = FSolverReal(Constraint->GetStaticFriction());
			const FSolverReal DynamicFriction = FSolverReal(Constraint->GetDynamicFriction());
			FSolverReal PositionStaticFriction = FSolverReal(0);
			FSolverReal PositionDynamicFriction = FSolverReal(0);
			FSolverReal VelocityDynamicFriction = FSolverReal(0);
			if (SolverSettings.NumPositionFrictionIterations > 0)
			{
				PositionStaticFriction = StaticFriction;
				if (!Constraint->HasQuadraticShape())
				{
					PositionDynamicFriction = DynamicFriction;
				}
				else
				{
					VelocityDynamicFriction = DynamicFriction;
				}
			}
			else
			{
				VelocityDynamicFriction = DynamicFriction;
			}

			Solver.SetFriction(PositionStaticFriction, PositionDynamicFriction, VelocityDynamicFriction);

			Solver.SetStiffness(FSolverReal(Constraint->GetStiffness()));

			Solver.SetSolverBodies(Body0, Body1);

			if (!bChaos_PBDCollisionSolver_VectorRegister)
			{
				GatherManifoldPoints(Dt, Body0, Body1);
			}
			else
			{
				GatherManifoldPointsVectorRegister(Dt, Body0, Body1);
			}

			// We should try to remove this - the Constraint should not need to know about solver objects
			Constraint->SetSolverBodies(Body0, Body1);
		}

		void GatherManifoldPoints(
			const FReal InDt,
			const FSolverBody* Body0,
			const FSolverBody* Body1)
		{
			FSolverReal Dt = FSolverReal(InDt);

			// We handle incremental manifolds by just collecting any new contacts
			const int32 BeginPointIndex = Solver.NumManifoldPoints();
			const int32 EndPointIndex = Solver.SetNumManifoldPoints(Constraint->GetManifoldPoints().Num());

			const FSolverReal RestitutionVelocityThreshold = FSolverReal(Constraint->GetRestitutionThreshold()) * Dt;
			const FSolverReal Restitution = FSolverReal(Constraint->GetRestitution());

			const FRigidTransform3& ShapeWorldTransform0 = Constraint->GetShapeWorldTransform0();
			const FRigidTransform3& ShapeWorldTransform1 = Constraint->GetShapeWorldTransform1();

			for (int32 ManifoldPointIndex = BeginPointIndex; ManifoldPointIndex < EndPointIndex; ++ManifoldPointIndex)
			{
				TArrayView<FManifoldPoint> ManifoldPoints = Constraint->GetManifoldPoints();
				FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];

				const FVec3 WorldContactPoint0 = ShapeWorldTransform0.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
				const FVec3 WorldContactPoint1 = ShapeWorldTransform1.TransformPositionNoScale(ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
				const FVec3 WorldContactPoint = FReal(0.5) * (WorldContactPoint0 + WorldContactPoint1);

				const FSolverVec3 WorldContactNormal = FSolverVec3(ShapeWorldTransform1.TransformVectorNoScale(ManifoldPoint.ContactPoint.ShapeContactNormal));
				const FSolverVec3 RelativeContactPosition0 = FSolverVec3(WorldContactPoint - Body0->P());
				const FSolverVec3 RelativeContactPosition1 = FSolverVec3(WorldContactPoint - Body1->P());
				const FSolverReal TargetPhi = FSolverReal(ManifoldPoint.TargetPhi);

				// If we have contact data from a previous tick, use it to calculate the lateral position delta we need
				// to apply to move the contacts back to their original relative locations (i.e., to enforce static friction)
				// @todo(chaos): we should not be writing back to the constraint here - find a better way to update the friction anchor. See FPBDCollisionConstraint::SetSolverResults
				FSolverVec3 WorldFrictionDelta = FSolverVec3(0);
				const FSavedManifoldPoint* SavedManifoldPoint = Constraint->FindSavedManifoldPoint(ManifoldPoint);
				if (SavedManifoldPoint != nullptr)
				{
					const FSolverVec3 FrictionDelta0 = FSolverVec3(SavedManifoldPoint->ShapeContactPoints[0] - ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
					const FSolverVec3 FrictionDelta1 = FSolverVec3(SavedManifoldPoint->ShapeContactPoints[1] - ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
					WorldFrictionDelta = ShapeWorldTransform0.TransformVectorNoScale(FVector(FrictionDelta0)) - ShapeWorldTransform1.TransformVectorNoScale(FVector(FrictionDelta1));

					ManifoldPoint.ShapeAnchorPoints[0] = SavedManifoldPoint->ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = SavedManifoldPoint->ShapeContactPoints[1];
				}
				else
				{
					const FSolverVec3 ContactVel0 = Body0->V() + FSolverVec3::CrossProduct(Body0->W(), RelativeContactPosition0);
					const FSolverVec3 ContactVel1 = Body1->V() + FSolverVec3::CrossProduct(Body1->W(), RelativeContactPosition1);
					const FSolverVec3 ContactVel = ContactVel0 - ContactVel1;
					WorldFrictionDelta = ContactVel * Dt;

					ManifoldPoint.ShapeAnchorPoints[0] = ManifoldPoint.ContactPoint.ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = ManifoldPoint.ContactPoint.ShapeContactPoints[1];
				}

				// World-space contact tangents. We are treating the normal as the constraint-space Z axis
				// and the Tangent U and V as the constraint-space X and Y axes respectively
				FSolverVec3 WorldContactTangentU = FSolverVec3::CrossProduct(FSolverVec3(0, 1, 0), WorldContactNormal);
				if (!WorldContactTangentU.Normalize(FSolverReal(KINDA_SMALL_NUMBER)))
				{
					WorldContactTangentU = FSolverVec3::CrossProduct(FSolverVec3(1, 0, 0), WorldContactNormal);
					WorldContactTangentU = WorldContactTangentU.GetUnsafeNormal();
				}
				const FSolverVec3 WorldContactTangentV = FSolverVec3::CrossProduct(WorldContactNormal, WorldContactTangentU);

				// The contact point error we are trying to correct in this solver
				const FSolverVec3 WorldContactDelta = FSolverVec3(WorldContactPoint0 - WorldContactPoint1);
				const FSolverReal WorldContactDeltaNormal = FSolverVec3::DotProduct(WorldContactDelta, WorldContactNormal) - TargetPhi;
				const FSolverReal WorldContactDeltaTangentU = FSolverVec3::DotProduct(WorldContactDelta + WorldFrictionDelta, WorldContactTangentU);
				const FSolverReal WorldContactDeltaTangentV = FSolverVec3::DotProduct(WorldContactDelta + WorldFrictionDelta, WorldContactTangentV);

				// Copy all the properties into the solver
				Solver.SetManifoldPoint(
					ManifoldPointIndex,
					Dt,
					Restitution,
					RestitutionVelocityThreshold,
					RelativeContactPosition0,
					RelativeContactPosition1,
					WorldContactNormal,
					WorldContactTangentU,
					WorldContactTangentV,
					WorldContactDeltaNormal,
					WorldContactDeltaTangentU,
					WorldContactDeltaTangentV);
			}
		}

		void GatherManifoldPointsVectorRegister(
			const FReal InDt,
			const FSolverBody* Body0,
			const FSolverBody* Body1)
		{
			// Attempt to make this LWC independent. Actually this isn't easy because we explicitly use MakeVectorRegisterFloatFromDouble

			// We handle incremental manifolds by just collecting any new contacts
			const int32 BeginPointIndex = Solver.NumManifoldPoints();
			const int32 EndPointIndex = Solver.SetNumManifoldPoints(Constraint->GetManifoldPoints().Num());
			TArrayView<FManifoldPoint> ManifoldPoints = Constraint->GetManifoldPoints();

			const FSolverReal Dt = FSolverReal(InDt);
			const FSolverReal RestitutionVelocityThreshold = FSolverReal(Constraint->GetRestitutionThreshold()) * Dt;
			const FSolverReal Restitution = FSolverReal(Constraint->GetRestitution());

			// World space positions - must be LWC compatible
			const VectorRegister ShapeWorldPos0 = Constraint->GetShapeWorldTransform0().GetTranslationRegister();
			const VectorRegister ShapeWorldRot0 = Constraint->GetShapeWorldTransform0().GetRotationRegister();
			const VectorRegister ShapeWorldPos1 = Constraint->GetShapeWorldTransform1().GetTranslationRegister();
			const VectorRegister ShapeWorldRot1 = Constraint->GetShapeWorldTransform1().GetRotationRegister();
			const VectorRegister BodyPos0 = VectorLoadFloat3(&Body0->P());
			const VectorRegister BodyPos1 = VectorLoadFloat3(&Body1->P());

			for (int32 ManifoldPointIndex = BeginPointIndex; ManifoldPointIndex < EndPointIndex; ++ManifoldPointIndex)
			{
				FManifoldPoint& ManifoldPoint = ManifoldPoints[ManifoldPointIndex];
				const FSavedManifoldPoint* SavedManifoldPoint = Constraint->FindSavedManifoldPoint(ManifoldPoint);

				const VectorRegister ShapeContactPos0 = VectorLoadFloat3(&ManifoldPoint.ContactPoint.ShapeContactPoints[0]);
				const VectorRegister ShapeContactPos1 = VectorLoadFloat3(&ManifoldPoint.ContactPoint.ShapeContactPoints[1]);
				const VectorRegister ShapeContactNormal1 = VectorLoadFloat3(&ManifoldPoint.ContactPoint.ShapeContactNormal);

				const VectorRegister WorldContactPoint0 = VectorAdd(ShapeWorldPos0, VectorQuaternionRotateVector(ShapeWorldRot0, ShapeContactPos0));
				const VectorRegister WorldContactPoint1 = VectorAdd(ShapeWorldPos1, VectorQuaternionRotateVector(ShapeWorldRot1, ShapeContactPos1));
				const VectorRegister WorldContactPoint = VectorMultiply(VectorSetFloat1(FSolverReal(0.5)), VectorAdd(WorldContactPoint0, WorldContactPoint1));

				// NOTE: low precision for relative coordinates
				const SolverVectorRegister WorldContactNormal = MakeVectorRegisterFloatFromDouble(VectorQuaternionRotateVector(ShapeWorldRot1, ShapeContactNormal1));
				const SolverVectorRegister RelativeContactPosition0 = MakeVectorRegisterFloatFromDouble(VectorSubtract(WorldContactPoint, BodyPos0));
				const SolverVectorRegister RelativeContactPosition1 = MakeVectorRegisterFloatFromDouble(VectorSubtract(WorldContactPoint, BodyPos1));
				const FSolverReal TargetPhi = FSolverReal(ManifoldPoint.TargetPhi);

				// If we have contact data from a previous tick, use it to calculate the lateral position delta we need
				// to apply to move the contacts back to their original relative locations (i.e., to enforce static friction)
				// @todo(chaos): we should not be writing back to the constraint here - find a better way to update the friction anchor. See FPBDCollisionConstraint::SetSolverResults
				// NOTE: low precision for relative coordinates
				SolverVectorRegister WorldFrictionDelta = VectorZeroFloat();
				if (SavedManifoldPoint != nullptr)
				{
					const VectorRegister SavedShapeContactPos0 = VectorLoadFloat3(&SavedManifoldPoint->ShapeContactPoints[0]);
					const VectorRegister SavedShapeContactPos1 = VectorLoadFloat3(&SavedManifoldPoint->ShapeContactPoints[1]);
					const VectorRegister FrictionDelta0 = VectorSubtract(SavedShapeContactPos0, ShapeContactPos0);
					const VectorRegister FrictionDelta1 = VectorSubtract(SavedShapeContactPos1, ShapeContactPos1);
					WorldFrictionDelta = MakeVectorRegisterFloatFromDouble(VectorSubtract(VectorQuaternionRotateVector(ShapeWorldRot0, FrictionDelta0), VectorQuaternionRotateVector(ShapeWorldRot1, FrictionDelta1)));

					ManifoldPoint.ShapeAnchorPoints[0] = SavedManifoldPoint->ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = SavedManifoldPoint->ShapeContactPoints[1];
				}
				else
				{
					const VectorRegister BodyV0 = VectorLoadFloat3(&Body0->V());
					const VectorRegister BodyW0 = VectorLoadFloat3(&Body0->W());
					const VectorRegister BodyV1 = VectorLoadFloat3(&Body1->V());
					const VectorRegister BodyW1 = VectorLoadFloat3(&Body1->W());
					const VectorRegister ContactVel0 = VectorAdd(BodyV0, VectorCross(BodyW0, RelativeContactPosition0));
					const VectorRegister ContactVel1 = VectorAdd(BodyV1, VectorCross(BodyW1, RelativeContactPosition1));
					const SolverVectorRegister ContactVel = MakeVectorRegisterFloatFromDouble(VectorSubtract(ContactVel0, ContactVel1));
					WorldFrictionDelta = VectorMultiply(ContactVel, VectorSetFloat1(Dt));

					ManifoldPoint.ShapeAnchorPoints[0] = ManifoldPoint.ContactPoint.ShapeContactPoints[0];
					ManifoldPoint.ShapeAnchorPoints[1] = ManifoldPoint.ContactPoint.ShapeContactPoints[1];
				}

				// World-space contact tangents. We are treating the normal as the constraint-space Z axis
				// and the Tangent U and V as the constraint-space X and Y axes respectively
				SolverVectorRegister WorldContactTangentU = VectorCross(MakeVectorRegisterFloatConstant(0, 1, 0, 0), WorldContactNormal);
				SolverVectorRegister WorldContactTangentULenSq = VectorDot3(WorldContactTangentU, WorldContactTangentU);
				SolverVectorRegister WorldContactTangentUCompareMask = VectorCompareGT(WorldContactTangentULenSq, VectorSetFloat1(KINDA_SMALL_NUMBER));
				if (VectorMaskBits(WorldContactTangentUCompareMask))
				{
					WorldContactTangentU = VectorMultiply(WorldContactTangentU, VectorReciprocalSqrt(WorldContactTangentULenSq));
				}
				else
				{
					WorldContactTangentU = VectorCross(MakeVectorRegisterFloatConstant(1, 0, 0, 0), WorldContactNormal);
					WorldContactTangentU = VectorNormalize(WorldContactTangentU);
				}
				const SolverVectorRegister WorldContactTangentV = VectorCross(WorldContactNormal, WorldContactTangentU);

				// The contact point error we are trying to correct in this solver
				const SolverVectorRegister WorldContactDelta = MakeVectorRegisterFloatFromDouble(VectorSubtract(WorldContactPoint0, WorldContactPoint1));
				const FSolverReal WorldContactDeltaNormal = VectorGetComponent(VectorDot3(WorldContactDelta, WorldContactNormal), 0) - TargetPhi;
				const FSolverReal WorldContactDeltaTangentU = VectorGetComponent(VectorDot3(VectorAdd(WorldContactDelta, WorldFrictionDelta), WorldContactTangentU), 0);
				const FSolverReal WorldContactDeltaTangentV = VectorGetComponent(VectorDot3(VectorAdd(WorldContactDelta, WorldFrictionDelta), WorldContactTangentV), 0);

				// Copy all the properties into the solver
				Solver.SetManifoldPoint(
					ManifoldPointIndex,
					Dt,
					Restitution,
					RestitutionVelocityThreshold,
					RelativeContactPosition0,
					RelativeContactPosition1,
					WorldContactNormal,
					WorldContactTangentU,
					WorldContactTangentV,
					WorldContactDeltaNormal,
					WorldContactDeltaTangentU,
					WorldContactDeltaTangentV);
			}
		}


		/**
		 * @brief Send all solver results to the constraint
		*/
		void ScatterOutput(const FReal Dt)
		{
			FVec3 AccumulatedImpulse = FVec3(0);

			Constraint->ResetSolverResults();

			for (int32 PointIndex = 0; PointIndex < Solver.NumManifoldPoints(); ++PointIndex)
			{
				const FPBDCollisionSolverManifoldPoint& SolverManifoldPoint = Solver.GetManifoldPoint(PointIndex);

				const FSolverVec3 NetPushOut = 
					SolverManifoldPoint.NetPushOutNormal * SolverManifoldPoint.WorldContactNormal +
					SolverManifoldPoint.NetPushOutTangentU * SolverManifoldPoint.WorldContactTangentU +
					SolverManifoldPoint.NetPushOutTangentV * SolverManifoldPoint.WorldContactTangentV;

				const FSolverVec3 NetImpulse =
					SolverManifoldPoint.NetImpulseNormal * SolverManifoldPoint.WorldContactNormal +
					SolverManifoldPoint.NetImpulseTangentU * SolverManifoldPoint.WorldContactTangentU +
					SolverManifoldPoint.NetImpulseTangentV * SolverManifoldPoint.WorldContactTangentV;

				Constraint->SetSolverResults(PointIndex, 
					NetPushOut, 
					NetImpulse, 
					SolverManifoldPoint.StaticFrictionRatio,
					Dt);
			}

			Constraint->SetSolverBodies(nullptr, nullptr);
			Constraint = nullptr;
			Solver.ResetSolverBodies();
		}


	private:
		FPBDCollisionSolver Solver;
		FPBDCollisionConstraint* Constraint;
		bool bIsIncremental;
	};


	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////
	//////////////////////////////////////////////////////////////////////////////////////////////////


	FPBDCollisionSolverContainer::FPBDCollisionSolverContainer()
		: FConstraintSolverContainer()
		, bRequiresIncrementalCollisionDetection(false)
	{
	}

	FPBDCollisionSolverContainer::~FPBDCollisionSolverContainer()
	{
	}

	void FPBDCollisionSolverContainer::Reset(const int32 MaxCollisions)
	{
		CollisionSolvers.Reset(MaxCollisions);
	}
	
	void FPBDCollisionSolverContainer::SetNum(const int32 MaxCollisions)
	{
		CollisionSolvers.SetNum(MaxCollisions, false);
	}

	void FPBDCollisionSolverContainer::PreAddConstraintSolver(FPBDCollisionConstraint& Constraint, FSolverBodyContainer& SolverBodyContainer, int32& ConstraintIndex)
	{
		// This container is required to allocate pointers that are valid for the whole tick,
		// so we cannot allow the container to resize during the tick. See Reset()
		check(ConstraintIndex < CollisionSolvers.Num());
		Constraint.SetSolverBodyContainerIndex(ConstraintIndex);

		FPBDCollisionSolverAdapter& CollisionSolver = CollisionSolvers[ConstraintIndex];
		CollisionSolver.GetSolver().Reset();

		CollisionSolver.PreGatherInput(Constraint, SolverBodyContainer);

		++ConstraintIndex;
	}

	void FPBDCollisionSolverContainer::AddConstraintSolver(FReal Dt, FPBDCollisionConstraint& Constraint, const int32 Particle0Level, const int32 Particle1Level, FSolverBodyContainer& SolverBodyContainer, const FPBDCollisionSolverSettings& SolverSettings)
	{
		// This container is required to allocate pointers that are valid for the whole tick,
		// so we cannot allow the container to resize during the tick. See Reset()
		int32 ConstraintIndex = Constraint.GetSolverBodyContainerIndex();
		check(ConstraintIndex < CollisionSolvers.Num());

		FPBDCollisionSolverAdapter& CollisionSolver = CollisionSolvers[ConstraintIndex];

		CollisionSolver.GatherInput(Dt, Constraint, Particle0Level, Particle1Level, SolverBodyContainer, SolverSettings);

		bRequiresIncrementalCollisionDetection |= CollisionSolver.IsIncrementalManifold();
	}

	void FPBDCollisionSolverContainer::UpdatePositionShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		// If this is the first shock propagation iteration, enable it on each solver
		const bool bEnableShockPropagation = (It == NumIts - SolverSettings.NumPositionShockPropagationIterations);
		if (bEnableShockPropagation)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().EnablePositionShockPropagation();
			}
		}
	}

	void FPBDCollisionSolverContainer::UpdateVelocityShockPropagation(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		// Set/reset the shock propagation based on current iteration. The position solve may
		// have left the bodies with a mass scale and we want to change or reset it.
		const bool bEnableShockPropagation = (It == NumIts - SolverSettings.NumVelocityShockPropagationIterations);
		if (bEnableShockPropagation)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().EnableVelocityShockPropagation();
			}
		}
		else if (It == 0)
		{
			for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().DisableShockPropagation();
			}
		}
	}

	bool FPBDCollisionSolverContainer::SolvePositionSerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		return SolvePositionImpl(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings, false);
	}

	bool FPBDCollisionSolverContainer::SolveVelocitySerial(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		return SolveVelocityImpl(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings, false);
	}

	bool FPBDCollisionSolverContainer::SolvePositionParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		return SolvePositionImpl(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings, true);
	}

	bool FPBDCollisionSolverContainer::SolveVelocityParallel(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings)
	{
		return SolveVelocityImpl(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings, true);
	}

	// @todo(chaos): parallel version of SolvePosition
	bool FPBDCollisionSolverContainer::SolvePositionImpl(const FReal Dt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_Apply);
		if (!bChaos_PBDCollisionSolver_Position_SolveEnabled)
		{
			return false;
		}

		UpdatePositionShockPropagation(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings);

		// Only apply friction for the last few (tunable) iterations
		const bool bApplyStaticFriction = (It >= (NumIts - SolverSettings.NumPositionFrictionIterations));

		// Adjust max pushout to attempt to make it iteration count independent
		const FReal MaxPushOut = (SolverSettings.MaxPushOutVelocity > 0) ? (SolverSettings.MaxPushOutVelocity * Dt) / FReal(NumIts) : 0;

		// Apply the position correction
		if (bRequiresIncrementalCollisionDetection)
		{
			return SolvePositionIncrementalImpl(Dt, BeginIndex, EndIndex, MaxPushOut, bApplyStaticFriction);
		}
		else if (bApplyStaticFriction)
		{
			return SolvePositionWithFrictionImpl(Dt, BeginIndex, EndIndex, MaxPushOut, bParallel);
		}
		else
		{
			return SolvePositionNoFrictionImpl(Dt, BeginIndex, EndIndex, MaxPushOut, bParallel);
		}
	}

	// Solve position including support for incremental collision detection
	bool FPBDCollisionSolverContainer::SolvePositionIncrementalImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut, const bool bApplyStaticFriction)
	{
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			FPBDCollisionSolverAdapter& CollisionSolver = CollisionSolvers[SolverIndex];
			if (CollisionSolver.IsIncrementalManifold())
			{
				Collisions::Update(*CollisionSolver.GetConstraint(), Dt);
				CollisionSolver.GatherManifoldPoints(Dt, &CollisionSolver.GetSolver().SolverBody0().SolverBody(), &CollisionSolver.GetSolver().SolverBody1().SolverBody());
			}
			if (bApplyStaticFriction)
			{
				bNeedsAnotherIteration |= CollisionSolver.GetSolver().SolvePositionWithFriction(Dt, MaxPushOut);
			}
			else
			{
				bNeedsAnotherIteration |= CollisionSolver.GetSolver().SolvePositionNoFriction(Dt, MaxPushOut);
			}

		}
		return bNeedsAnotherIteration;
	}

	// Solve position with friction (last few iterations each tick)
	bool FPBDCollisionSolverContainer::SolvePositionWithFrictionImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut, const bool bParallel)
	{
		if (EndIndex == BeginIndex)
		{
			return false;
		}
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		InnerPhysicsParallelForRange(EndIndex - BeginIndex, [&](int32 StartRangeIndex, int32 EndRangeIndex)
		{
			int32 LocalStartIndex = StartRangeIndex + BeginIndex;
			int32 LocalEndIndex = EndRangeIndex + BeginIndex;
			for (int32 SolverIndex = LocalStartIndex; SolverIndex < LocalEndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().SolvePositionWithFriction(Dt, MaxPushOut);
			}
		}, Chaos::LargeBatchSize, !bParallel);
		return true;
	}

	// Solve position without friction (first few iterations each tick)
	bool FPBDCollisionSolverContainer::SolvePositionNoFrictionImpl(const FReal InDt, const int32 BeginIndex, const int32 EndIndex, const FReal InMaxPushOut, const bool bParallel)
	{
		if (EndIndex == BeginIndex)
		{
			return false;
		}
		const FSolverReal Dt = FSolverReal(InDt);
		const FSolverReal MaxPushOut = FSolverReal(InMaxPushOut);

		InnerPhysicsParallelForRange(EndIndex - BeginIndex, [&](int32 StartRangeIndex, int32 EndRangeIndex)
		{
			int32 LocalStartIndex = StartRangeIndex + BeginIndex;
			int32 LocalEndIndex = EndRangeIndex + BeginIndex;
			for (int32 SolverIndex = LocalStartIndex; SolverIndex < LocalEndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].GetSolver().SolvePositionNoFriction(Dt, MaxPushOut);
			}
		}, Chaos::LargeBatchSize, !bParallel);
		return true;
	}

	bool FPBDCollisionSolverContainer::SolveVelocityImpl(const FReal InDt, const int32 It, const int32 NumIts, const int32 BeginIndex, const int32 EndIndex, const FPBDCollisionSolverSettings& SolverSettings, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_ApplyPushOut);
		if (!bChaos_PBDCollisionSolver_Velocity_SolveEnabled)
		{
			return false;
		}
		const FSolverReal Dt = FSolverReal(InDt);

		UpdateVelocityShockPropagation(Dt, It, NumIts, BeginIndex, EndIndex, SolverSettings);

		const bool bApplyDynamicFriction = (It >= NumIts - SolverSettings.NumVelocityFrictionIterations);

		// Apply the velocity correction
		// @todo(chaos): parallel version of SolveVelocity
		bool bNeedsAnotherIteration = false;
		for (int32 SolverIndex = BeginIndex; SolverIndex < EndIndex; ++SolverIndex)
		{
			bNeedsAnotherIteration |= CollisionSolvers[SolverIndex].GetSolver().SolveVelocity(Dt, bApplyDynamicFriction);
		}

		return bNeedsAnotherIteration;
	}

	void FPBDCollisionSolverContainer::ScatterOutput(const FReal Dt, const int32 BeginIndex, const int32 EndIndex)
	{
		return ScatterOutputImpl(Dt, BeginIndex, EndIndex, false);
	}

	void FPBDCollisionSolverContainer::ScatterOutputImpl(const FReal Dt, const int32 BeginIndex, const int32 EndIndex, const bool bParallel)
	{
		SCOPE_CYCLE_COUNTER(STAT_Collisions_Scatter);
		check(BeginIndex >= 0);
		check(EndIndex <= CollisionSolvers.Num());
		if (EndIndex == BeginIndex)
		{
			return;
		}

		InnerPhysicsParallelForRange(EndIndex - BeginIndex, [&](int32 LocalStartIndex, int32 LocalEndIndex)
		{
			int32 LoopStartIndex = LocalStartIndex + BeginIndex;
			int32 LoopEndIndex = LocalEndIndex + BeginIndex;
			for (int32 SolverIndex = LoopStartIndex; SolverIndex < LoopEndIndex; ++SolverIndex)
			{
				CollisionSolvers[SolverIndex].ScatterOutput(Dt);
			}
		}, Chaos::LargeBatchSize);
	}

}
