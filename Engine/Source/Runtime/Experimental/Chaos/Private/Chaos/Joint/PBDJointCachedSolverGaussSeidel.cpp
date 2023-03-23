// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/Joint/PBDJointCachedSolverGaussSeidel.h"
#include "Chaos/Joint/ChaosJointLog.h"
#include "Chaos/Joint/JointConstraintsCVars.h"
#include "Chaos/Joint/JointSolverConstraints.h"
#include "Chaos/ParticleHandle.h"
#include "Chaos/PBDJointConstraintUtilities.h"
#include "Chaos/Utilities.h"

namespace Chaos
{
	
FPBDJointCachedSolver::FPBDJointCachedSolver()
{}
	
/** Derived states management */

void FPBDJointCachedSolver::InitDerivedState()
{
	InitConnectorXs[0] = X(0) + R(0) * LocalConnectorXs[0].GetTranslation();
	InitConnectorXs[1] = X(1) + R(1) * LocalConnectorXs[1].GetTranslation();
	InitConnectorRs[0] = R(0) * LocalConnectorXs[0].GetRotation();
	InitConnectorRs[1] = R(1) * LocalConnectorXs[1].GetRotation();
	InitConnectorRs[1].EnforceShortestArcWith(InitConnectorRs[0]);
	
	ComputeBodyState(0);
	ComputeBodyState(1);

	ConnectorRs[1].EnforceShortestArcWith(ConnectorRs[0]);

	ConnectorWDts[0] = FRotation3::CalculateAngularVelocity(InitConnectorRs[0], ConnectorRs[0], 1.0f);
	ConnectorWDts[1] = FRotation3::CalculateAngularVelocity(InitConnectorRs[1], ConnectorRs[1], 1.0f);
}

void FPBDJointCachedSolver::ComputeBodyState(const int32 BodyIndex)
{
	CurrentPs[BodyIndex] = P(BodyIndex);
	CurrentQs[BodyIndex] = Q(BodyIndex);
	ConnectorXs[BodyIndex] = CurrentPs[BodyIndex] + CurrentQs[BodyIndex] * LocalConnectorXs[BodyIndex].GetTranslation();
	ConnectorRs[BodyIndex] = CurrentQs[BodyIndex] * LocalConnectorXs[BodyIndex].GetRotation();
}

void FPBDJointCachedSolver::UpdateDerivedState()
{
	// Kinematic bodies will not be moved, so we don't update derived state during iterations
	if (InvM(0) > SMALL_NUMBER)
	{
		ComputeBodyState(0);
	}
	if (InvM(1) > SMALL_NUMBER)
	{
		ComputeBodyState(1);
	}
	ConnectorRs[1].EnforceShortestArcWith(ConnectorRs[0]);
}

void FPBDJointCachedSolver::UpdateDerivedState(const int32 BodyIndex)
{
	ComputeBodyState(BodyIndex);
	ConnectorRs[1].EnforceShortestArcWith(ConnectorRs[0]);
}

bool FPBDJointCachedSolver::UpdateIsActive()
{
	// NumActiveConstraints is initialized to -1, so there's no danger of getting invalid LastPs/Qs
	// We also check SolverStiffness mainly for testing when solver stiffness is 0 (so we don't exit immediately)
	if ((NumActiveConstraints >= 0) && (SolverStiffness > 0.0f))
	{
		bool bIsSolved =
			FVec3::IsNearlyEqual(Body(0).DP(), LastDPs[0], PositionTolerance)
			&& FVec3::IsNearlyEqual(Body(1).DP(), LastDPs[1], PositionTolerance)
			&& FVec3::IsNearlyEqual(Body(0).DQ(), LastDQs[0], 0.5f * AngleTolerance)
			&& FVec3::IsNearlyEqual(Body(1).DQ(), LastDQs[1], 0.5f * AngleTolerance);
		bIsActive = !bIsSolved;
	}

	LastDPs[0] = Body(0).DP();
	LastDPs[1] = Body(1).DP();
	LastDQs[0] = Body(0).DQ();
	LastDQs[1] = Body(1).DQ();

	return bIsActive;
}

void FPBDJointCachedSolver::Update(
	   const FReal Dt,
	   const FPBDJointSolverSettings& SolverSettings,
	   const FPBDJointSettings& JointSettings)
{
	//UpdateIsActive();
}

void FPBDJointCachedSolver::UpdateMass0()
{
	// @todo(chaos): this needs to recache all the mass-dependent state on the axis data etc
	if ((ConditionedInvMs[0] > 0) && (InvMScales[0] > 0))
	{
		InvMs[0] = InvMScales[0] * ConditionedInvMs[0];
		InvIs[0] = Utilities::ComputeWorldSpaceInertia(CurrentQs[0], InvMScales[0] * ConditionedInvILs[0]);
	}
	else
	{
		InvMs[0] = 0;
		InvIs[0] = FMatrix33(0);
	}
}

void FPBDJointCachedSolver::UpdateMass1()
{
	// @todo(chaos): this needs to recache all the mass-dependent state on the axis data etc
	if ((ConditionedInvMs[1] > 0) && (InvMScales[1] > 0))
	{
		InvMs[1] = InvMScales[1] * ConditionedInvMs[1];
		InvIs[1] = Utilities::ComputeWorldSpaceInertia(CurrentQs[1], InvMScales[1] * ConditionedInvILs[1]);
	}
	else
	{
		InvMs[1] = 0;
		InvIs[1] = FMatrix33(0);
	}
}

void FPBDJointCachedSolver::SetInvMassScales(const FReal InvMScale0, const FReal InvMScale1, const FReal Dt)
{
	bool bNeedsUpdate = false;
	if (InvMScales[0] != InvMScale0)
	{
		InvMScales[0] = InvMScale0;
		UpdateMass0();
		bNeedsUpdate = true;
	}
	if (InvMScales[1] != InvMScale1)
	{
		InvMScales[1] = InvMScale1;
		UpdateMass1();
		bNeedsUpdate = true;
	}
	if(bNeedsUpdate)
	{
		for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
		{
			if(PositionConstraints.bValidDatas[ConstraintIndex])
			{
				InitPositionDatasMass(PositionConstraints, ConstraintIndex, Dt);
			}
			if(RotationConstraints.bValidDatas[ConstraintIndex])
			{
				InitRotationDatasMass(RotationConstraints, ConstraintIndex, Dt);
			}
			if(PositionDrives.bValidDatas[ConstraintIndex])
			{
				InitPositionDatasMass(PositionDrives, ConstraintIndex, Dt);
			}
			if(RotationDrives.bValidDatas[ConstraintIndex])
			{
				InitRotationDatasMass(RotationDrives, ConstraintIndex, Dt);
			}
		}
	}
}

void FPBDJointCachedSolver::EnableProjection()
{
	Body0().SetInvMScale(0);
}

/** Main init function to cache datas that could be reused in the apply */

void FPBDJointCachedSolver::Init(
	const FReal Dt,
	const FSolverBodyPtrPair& SolverBodyPair,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings,
	const FRigidTransform3& XL0,
	const FRigidTransform3& XL1)
{
	SolverBodies[0] = *SolverBodyPair[0];
	SolverBodies[1] = *SolverBodyPair[1];

	LocalConnectorXs[0] = XL0;
	LocalConnectorXs[1] = XL1;

	// \todo(chaos): joint should support parent/child in either order
	SolverBodies[0].SetInvMScale(JointSettings.ParentInvMassScale);
	SolverBodies[1].SetInvMScale(FReal(1));

	InvMScales[0] = FReal(1);
	InvMScales[1] = FReal(1);
	FPBDJointUtilities::ConditionInverseMassAndInertia(Body0().InvM(), Body1().InvM(), Body0().InvILocal(), Body1().InvILocal(), SolverSettings.MinParentMassRatio, SolverSettings.MaxInertiaRatio, ConditionedInvMs[0], ConditionedInvMs[1], ConditionedInvILs[0], ConditionedInvILs[1]);
	
	NetLinearImpulse = FVec3(0);
	NetAngularImpulse = FVec3(0);

	LinearConstraintPadding = FVec3(-1);
	AngularConstraintPadding = FVec3(-1);

	// Tolerances are positional errors below visible detection. But in PBD the errors
	// we leave behind get converted to velocity, so we need to ensure that the resultant
	// movement from that erroneous velocity is less than the desired position tolerance.
	// Assume that the tolerances were defined for a 60Hz simulation, then it must be that
	// the position error is less than the position change from constant external forces
	// (e.g., gravity). So, we are saying that the tolerance was chosen because the position
	// error is less that F.dt^2. We need to scale the tolerance to work at our current dt.
	const FReal ToleranceScale = FMath::Min(1.f, 60.f * 60.f * Dt * Dt);
	PositionTolerance = ToleranceScale * SolverSettings.PositionTolerance;
	AngleTolerance = ToleranceScale * SolverSettings.AngleTolerance;

	NumActiveConstraints = -1;
	bIsActive = true;

	SolverStiffness = 1.0f;

	InitDerivedState();

	UpdateMass0();
	UpdateMass1();

	// Cache all the informations for the position and rotation constraints
	InitPositionConstraints(Dt, SolverSettings, JointSettings);
	InitRotationConstraints(Dt, SolverSettings, JointSettings);

	InitPositionDrives(Dt, SolverSettings, JointSettings);
	InitRotationDrives(Dt, SolverSettings, JointSettings);

	LastDPs[0] = FVec3(0.f);
	LastDPs[1] = FVec3(0.f);
	LastDQs[0] = FVec3(0.f);
	LastDQs[1] = FVec3(0.f);
}
	
void FPBDJointCachedSolver::InitProjection(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	bool bHasLinearProjection = false;
	{
		const FReal LinearProjection = FPBDJointUtilities::GetLinearProjection(SolverSettings, JointSettings);
		bHasLinearProjection = JointSettings.bProjectionEnabled && LinearProjection > 0.0;
	}
	bool bHasAngularProjection = false;
	{
		const FReal AngularProjection = FPBDJointUtilities::GetAngularProjection(SolverSettings, JointSettings);
		bHasAngularProjection = JointSettings.bProjectionEnabled && AngularProjection > 0.0;
	}
	if(bHasLinearProjection || bHasAngularProjection)
	{
		ComputeBodyState(0);
		ComputeBodyState(1);

		ConnectorRs[1].EnforceShortestArcWith(ConnectorRs[0]);

		InvMScales[0] = 0.0;
		InvMScales[1] = 1.0;

		UpdateMass0();
		UpdateMass1();

		if(bHasLinearProjection)
		{
			InitPositionConstraints(Dt, SolverSettings, JointSettings);
		}

		if(bHasAngularProjection)
		{
			InitRotationConstraints(Dt, SolverSettings, JointSettings);
		}
	}
}

void FPBDJointCachedSolver::Deinit()
{
	SolverBodies[0].Reset();
	SolverBodies[1].Reset();
}

/** Main Apply function to solve all the constraint*/

void FPBDJointCachedSolver::ApplyConstraints(
		const FReal Dt,
		const FReal InSolverStiffness,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
{
	NumActiveConstraints = 0;
	SolverStiffness = InSolverStiffness;

	if (SolverSettings.bSolvePositionLast)
	{
		ApplyRotationConstraints(Dt);
		ApplyPositionConstraints(Dt);

		ApplyRotationDrives(Dt);
		ApplyPositionDrives(Dt);
	}
	else
	{
		ApplyPositionConstraints(Dt);
		ApplyRotationConstraints(Dt);

		ApplyPositionDrives(Dt);
		ApplyRotationDrives(Dt);
	}

	//UpdateIsActive();
}

void FPBDJointCachedSolver::ApplyVelocityConstraints(
	const FReal Dt,
	const FReal InSolverStiffness,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	SolverStiffness = InSolverStiffness;

	// This is used for the QuasiPbd solver. If the Pbd step applied impulses to
	// correct position errors, it will have introduced a velocity equal to the 
	// correction divided by the timestep. We ensure that the velocity constraints
	// (including restitution) are also enforced. This also prevents any position
	// errors from the previous frame getting converted into energy.

	if (SolverSettings.bSolvePositionLast)
	{
		ApplyAngularVelocityConstraints();
		ApplyLinearVelocityConstraints();
	}
	else
	{
		ApplyLinearVelocityConstraints();
		ApplyAngularVelocityConstraints();
	}

	// @todo(chaos): We can also apply velocity drives here rather than in the Pbd pass
}

/** UTILS FOR POSITION CONSTRAINT **************************************************************************************/

FORCEINLINE bool ExtractLinearMotion( const FPBDJointSettings& JointSettings,
	TVec3<bool>& bLinearLocked, TVec3<bool>& bLinearLimited)
{
	bool bHasPositionConstraints =
		(JointSettings.LinearMotionTypes[0] != EJointMotionType::Free)
		|| (JointSettings.LinearMotionTypes[1] != EJointMotionType::Free)
		|| (JointSettings.LinearMotionTypes[2] != EJointMotionType::Free);
	if (!bHasPositionConstraints)
	{
		return false;
	}

	const TVec3<EJointMotionType>& LinearMotion = JointSettings.LinearMotionTypes;
	bLinearLocked =
	{
		(LinearMotion[0] == EJointMotionType::Locked),
		(LinearMotion[1] == EJointMotionType::Locked),
		(LinearMotion[2] == EJointMotionType::Locked),
	};
	bLinearLimited =
	{
		(LinearMotion[0] == EJointMotionType::Limited),
		(LinearMotion[1] == EJointMotionType::Limited),
		(LinearMotion[2] == EJointMotionType::Limited),
	};
	return true;
}

/** INIT POSITION CONSTRAINT ******************************************************************************************/

void FPBDJointCachedSolver::InitPositionConstraints(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	PositionConstraints.bValidDatas[0] = false;
	PositionConstraints.bValidDatas[1] = false;
	PositionConstraints.bValidDatas[2] = false;

	TVec3<bool> bLinearLocked, bLinearLimited;
	if(!ExtractLinearMotion(JointSettings, bLinearLocked, bLinearLimited))
		return;

	PositionConstraints.bAccelerationMode = FPBDJointUtilities::GetLinearSoftAccelerationMode(SolverSettings, JointSettings);

	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ ConstraintIndex)
	{
		PositionConstraints.InitDatas(ConstraintIndex,bLinearLimited[ConstraintIndex] &&
			FPBDJointUtilities::GetSoftLinearLimitEnabled(SolverSettings, JointSettings),
			FPBDJointUtilities::GetSoftLinearStiffness(SolverSettings, JointSettings),
			FPBDJointUtilities::GetSoftLinearDamping(SolverSettings, JointSettings),
			FPBDJointUtilities::GetLinearStiffness(SolverSettings, JointSettings));
	}

	const TVec3<EJointMotionType>& LinearMotion = JointSettings.LinearMotionTypes;


	const FVec3 ConstraintArm0Limited = ConnectorXs[1] - CurrentPs[0];
	const FVec3 ConstraintArm1Limited = ConnectorXs[1] - CurrentPs[1];
	
	FVec3 ConstraintArm0Locked = ConstraintArm0Limited;
	FVec3 ConstraintArm1Locked = ConstraintArm1Limited;

	const FVec3 DX = ConnectorXs[1] - ConnectorXs[0];
	const FMatrix33 R0M = ConnectorRs[0].ToMatrix(); 
	FVec3 CX = FVec3::ZeroVector;

	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(bLinearLocked[ConstraintIndex] || bLinearLimited[ConstraintIndex])
		{
			const FVec3 ConstraintAxis = R0M.GetAxis(ConstraintIndex);
			CX[ConstraintIndex] = FVec3::DotProduct(DX, ConstraintAxis);

			if(bLinearLocked[ConstraintIndex])
			{
				ConstraintArm0Locked -= ConstraintAxis * CX[ConstraintIndex];
			}
		}
	}
	
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(bLinearLocked[ConstraintIndex] || bLinearLimited[ConstraintIndex])
		{
			const FVec3 ConstraintAxis = R0M.GetAxis(ConstraintIndex);
			PositionConstraints.MotionType[ConstraintIndex] = LinearMotion[ConstraintIndex];
			
			if(bLinearLocked[ConstraintIndex])
			{
				PositionConstraints.ConstraintLimits[ConstraintIndex] = 0.0f;
				PositionConstraints.UpdateDatas(ConstraintIndex, ConstraintAxis, CX[ConstraintIndex],
					0.0, false, ConstraintArm0Locked, ConstraintArm1Locked);
			}
			else if(bLinearLimited[ConstraintIndex])
			{
				PositionConstraints.ConstraintLimits[ConstraintIndex] =
					FMath::Max(JointSettings.LinearLimit - GetLinearConstraintPadding(ConstraintIndex), (FReal)0.);
				PositionConstraints.UpdateDatas(ConstraintIndex, ConstraintAxis, CX[ConstraintIndex],
					JointSettings.LinearRestitution, true, ConstraintArm0Limited, ConstraintArm1Limited);
			}
			const FVec3 CV0 = V(0) + FVec3::CrossProduct(W(0), PositionConstraints.ConstraintArms[ConstraintIndex][0]);
			const FVec3 CV1 = V(1) + FVec3::CrossProduct(W(1), PositionConstraints.ConstraintArms[ConstraintIndex][1]);
			const FVec3 CV = CV1 - CV0;
			
			InitConstraintAxisLinearVelocities[ConstraintIndex] = FVec3::DotProduct(CV, ConstraintAxis);

			InitPositionDatasMass(PositionConstraints, ConstraintIndex, Dt);
		}
	}
}

void FPBDJointCachedSolver::InitPositionDatasMass(
	FAxisConstraintDatas& PositionDatas, 
	const int32 ConstraintIndex,
	const FReal Dt)
{
	const FVec3 AngularAxis0 = FVec3::CrossProduct(PositionDatas.ConstraintArms[ConstraintIndex][0], PositionDatas.ConstraintAxis[ConstraintIndex]);
	const FVec3 AngularAxis1 = FVec3::CrossProduct(PositionDatas.ConstraintArms[ConstraintIndex][1], PositionDatas.ConstraintAxis[ConstraintIndex]);
	const FVec3 IA0 = Utilities::Multiply(InvI(0), AngularAxis0);
	const FVec3 IA1 = Utilities::Multiply(InvI(1), AngularAxis1);
	const FReal II0 = FVec3::DotProduct(AngularAxis0, IA0);
	const FReal II1 = FVec3::DotProduct(AngularAxis1, IA1);

	PositionDatas.UpdateMass(ConstraintIndex, IA0, IA1, InvM(0) + II0 + InvM(1) + II1, Dt);
}

/** APPLY POSITION CONSTRAINT *****************************************************************************************/

void FPBDJointCachedSolver::ApplyPositionConstraints(
	const FReal Dt)
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(PositionConstraints.bValidDatas[ConstraintIndex])
		{
			ApplyAxisPositionConstraint(ConstraintIndex, Dt);
		}
	}
}
	
void FPBDJointCachedSolver::SolvePositionConstraintDelta(
	const int32 ConstraintIndex, 
	const FReal DeltaLambda,
	const FAxisConstraintDatas& ConstraintDatas)
{
	const FVec3 DX = ConstraintDatas.ConstraintAxis[ConstraintIndex] * DeltaLambda;

	if(Body(0).IsDynamic())
	{
		const FVec3 DP0 = InvM(0) * DX;
		const FVec3 DR0 = ConstraintDatas.ConstraintDRAxis[ConstraintIndex][0] * DeltaLambda;
		ApplyPositionDelta(0,DP0);
		ApplyRotationDelta(0,DR0);
	}
	if(Body(1).IsDynamic())
	{
		const FVec3 DP1 = -InvM(1) * DX;
		const FVec3 DR1 = ConstraintDatas.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda;
		ApplyPositionDelta(1,DP1);
		ApplyRotationDelta(1,DR1);
	}

	NetLinearImpulse += DX;
	++NumActiveConstraints;
}

void FPBDJointCachedSolver::SolvePositionConstraintHard(
	const int32 ConstraintIndex,
	const FReal DeltaConstraint)
{
	const FReal DeltaLambda = SolverStiffness * PositionConstraints.ConstraintHardStiffness[ConstraintIndex] * DeltaConstraint /
		PositionConstraints.ConstraintHardIM[ConstraintIndex];

	PositionConstraints.ConstraintLambda[ConstraintIndex] += DeltaLambda;
	SolvePositionConstraintDelta(ConstraintIndex, DeltaLambda, PositionConstraints);
}

void FPBDJointCachedSolver::SolvePositionConstraintSoft(
	const int32 ConstraintIndex,
	const FReal DeltaConstraint,
	const FReal Dt,
	const FReal TargetVel)
{
	FReal VelDt = 0;
	if (PositionConstraints.ConstraintSoftDamping[ConstraintIndex] > KINDA_SMALL_NUMBER)
	{
		const FVec3 V0Dt = FVec3::CalculateVelocity(InitConnectorXs[0], ConnectorXs[0]+Body(0).DP() + FVec3::CrossProduct(Body(0).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][0]), 1.0f);
		const FVec3 V1Dt = FVec3::CalculateVelocity(InitConnectorXs[1], ConnectorXs[1]+Body(1).DP() + FVec3::CrossProduct(Body(1).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][1]), 1.0f);
		VelDt = TargetVel * Dt + FVec3::DotProduct(V0Dt - V1Dt, PositionConstraints.ConstraintAxis[ConstraintIndex] );
	}

	const FReal DeltaLambda = SolverStiffness * (PositionConstraints.ConstraintSoftStiffness[ConstraintIndex] * DeltaConstraint - PositionConstraints.ConstraintSoftDamping[ConstraintIndex] * VelDt - PositionConstraints.ConstraintLambda[ConstraintIndex]) /
		PositionConstraints.ConstraintSoftIM[ConstraintIndex];
	PositionConstraints.ConstraintLambda[ConstraintIndex] += DeltaLambda;

	SolvePositionConstraintDelta(ConstraintIndex, DeltaLambda, PositionConstraints);
}

void FPBDJointCachedSolver::ApplyAxisPositionConstraint(
	const int32 ConstraintIndex, const FReal Dt)
{
	const FVec3 CX = Body(1).DP()  - Body(0).DP() +
		FVec3::CrossProduct(Body(1).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][1]) -
			FVec3::CrossProduct(Body(0).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][0]) ;
	
	FReal DeltaPosition = PositionConstraints.ConstraintCX[ConstraintIndex] + FVec3::DotProduct(CX, PositionConstraints.ConstraintAxis[ConstraintIndex]);

	bool NeedsSolve = false;
	if(PositionConstraints.bLimitsCheck[ConstraintIndex])
	{
		if(DeltaPosition > PositionConstraints.ConstraintLimits[ConstraintIndex] )
		{
			DeltaPosition -= PositionConstraints.ConstraintLimits[ConstraintIndex];
			NeedsSolve = true;
		}
		else if(DeltaPosition < -PositionConstraints.ConstraintLimits[ConstraintIndex])
		{
			DeltaPosition += PositionConstraints.ConstraintLimits[ConstraintIndex];
			NeedsSolve = true;
		}
	}  
	if (!PositionConstraints.bLimitsCheck[ConstraintIndex] || (PositionConstraints.bLimitsCheck[ConstraintIndex] && NeedsSolve && FMath::Abs(DeltaPosition ) > PositionTolerance))
	{
		if ((PositionConstraints.MotionType[ConstraintIndex] == EJointMotionType::Limited) && PositionConstraints.bSoftLimit[ConstraintIndex])
		{
			SolvePositionConstraintSoft(ConstraintIndex, DeltaPosition, Dt, 0.0f);
		}
		else if (PositionConstraints.MotionType[ConstraintIndex] != EJointMotionType::Free)
		{
			if (PositionConstraints.ConstraintRestitution[ConstraintIndex] > 0.0f)
			{
				CalculateLinearConstraintPadding(ConstraintIndex, Dt, PositionConstraints.ConstraintRestitution[ConstraintIndex], DeltaPosition);
			}
			SolvePositionConstraintHard(ConstraintIndex, DeltaPosition);
		}
	}
}

/** APPLY LINEAR VELOCITY *********************************************************************************************/

void FPBDJointCachedSolver::ApplyLinearVelocityConstraints()
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(PositionConstraints.bValidDatas[ConstraintIndex])
		{
			ApplyAxisVelocityConstraint(ConstraintIndex);
		}
	}
}

void FPBDJointCachedSolver::SolveLinearVelocityConstraint(
	const int32 ConstraintIndex,
	const FReal TargetVel)
{
	const FVec3 CV0 = V(0) + FVec3::CrossProduct(W(0), PositionConstraints.ConstraintArms[ConstraintIndex][0]);
	const FVec3 CV1 = V(1) + FVec3::CrossProduct(W(1), PositionConstraints.ConstraintArms[ConstraintIndex][1]);
	const FVec3 CV = CV1 - CV0;

	const FReal DeltaLambda = SolverStiffness * PositionConstraints.ConstraintHardStiffness[ConstraintIndex] *
	 (FVec3::DotProduct(CV, PositionConstraints.ConstraintAxis[ConstraintIndex]) - TargetVel) / PositionConstraints.ConstraintHardIM[ConstraintIndex];
	
	const FVec3 MDV = DeltaLambda * PositionConstraints.ConstraintAxis[ConstraintIndex];

	if(Body(0).IsDynamic())
	{
		const FVec3 DV0 = InvM(0) * MDV;
		const FVec3 DW0 = PositionConstraints.ConstraintDRAxis[ConstraintIndex][0] * DeltaLambda;

		Body(0).ApplyVelocityDelta(DV0, DW0);
	}
	if(Body(1).IsDynamic())
	{
		const FVec3 DV1 = -InvM(1) * MDV;
		const FVec3 DW1 = PositionConstraints.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda;

		Body(1).ApplyVelocityDelta(DV1, DW1);
	}
}

void FPBDJointCachedSolver::ApplyAxisVelocityConstraint(const int32 ConstraintIndex)
{
	if(!NetLinearImpulse.IsNearlyZero() && (FMath::Abs(PositionConstraints.ConstraintLambda[ConstraintIndex]) > SMALL_NUMBER) )
	{
		FReal TargetVel = 0.0f;
		if (PositionConstraints.MotionType[ConstraintIndex] == EJointMotionType::Limited && PositionConstraints.ConstraintRestitution[ConstraintIndex] != 0.0f)
		{
			const FReal InitVel = InitConstraintAxisLinearVelocities[ConstraintIndex];
			TargetVel = InitVel > Chaos_Joint_LinearVelocityThresholdToApplyRestitution ?
				-PositionConstraints.ConstraintRestitution[ConstraintIndex] * InitVel : 0.0f; 
		}
		SolveLinearVelocityConstraint(ConstraintIndex, TargetVel);
	}
}

/** UTILS FOR ROTATION CONSTRAINT **************************************************************************************/

FORCEINLINE bool ExtractAngularMotion( const FPBDJointSettings& JointSettings,
		TVec3<bool>& bAngularLocked, TVec3<bool>& bAngularLimited, TVec3<bool>& bAngularFree)
{
	bool bHasRotationConstraints =
			  (JointSettings.AngularMotionTypes[0] != EJointMotionType::Free)
		   || (JointSettings.AngularMotionTypes[1] != EJointMotionType::Free)
		   || (JointSettings.AngularMotionTypes[2] != EJointMotionType::Free);
	if (!bHasRotationConstraints)
	{
		return false;
	}

	const TVec3<EJointMotionType>& AngularMotion = JointSettings.AngularMotionTypes;
	bAngularLocked =
	{
		(AngularMotion[0] == EJointMotionType::Locked),
		(AngularMotion[1] == EJointMotionType::Locked),
		(AngularMotion[2] == EJointMotionType::Locked),
	};
	bAngularLimited =
	{
		(AngularMotion[0] == EJointMotionType::Limited),
		(AngularMotion[1] == EJointMotionType::Limited),
		(AngularMotion[2] == EJointMotionType::Limited),
	};
	bAngularFree=
	{
		(AngularMotion[0] == EJointMotionType::Free),
		(AngularMotion[1] == EJointMotionType::Free),
		(AngularMotion[2] == EJointMotionType::Free),
	};
	return true;
}

/** INIT ROTATION CONSTRAINT ******************************************************************************************/

void FPBDJointCachedSolver::InitRotationConstraints(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	RotationConstraints.bValidDatas[0] = false;
	RotationConstraints.bValidDatas[1] = false;
	RotationConstraints.bValidDatas[2] = false;

	TVec3<bool> bAngularLocked, bAngularLimited, bAngularFree;
	if(!ExtractAngularMotion(JointSettings, bAngularLocked, bAngularLimited, bAngularFree))
		return;

	RotationConstraints.bAccelerationMode = FPBDJointUtilities::GetAngularSoftAccelerationMode(SolverSettings, JointSettings);

	const int32 TW = (int32)EJointAngularConstraintIndex::Twist;
	const int32 S1 = (int32)EJointAngularConstraintIndex::Swing1;
	const int32 S2 = (int32)EJointAngularConstraintIndex::Swing2;

	RotationConstraints.InitDatas(TW,FPBDJointUtilities::GetSoftTwistLimitEnabled(SolverSettings, JointSettings) && !bAngularLocked[TW],
FPBDJointUtilities::GetSoftTwistStiffness(SolverSettings, JointSettings),
							   FPBDJointUtilities::GetSoftTwistDamping(SolverSettings, JointSettings),
					FPBDJointUtilities::GetTwistStiffness(SolverSettings, JointSettings));

	RotationConstraints.InitDatas(S1,FPBDJointUtilities::GetSoftSwingLimitEnabled(SolverSettings, JointSettings) && !bAngularLocked[S1],
	FPBDJointUtilities::GetSoftSwingStiffness(SolverSettings, JointSettings),
					FPBDJointUtilities::GetSoftSwingDamping(SolverSettings, JointSettings),
					FPBDJointUtilities::GetSwingStiffness(SolverSettings, JointSettings));

	RotationConstraints.InitDatas(S2, FPBDJointUtilities::GetSoftSwingLimitEnabled(SolverSettings, JointSettings) && !bAngularLocked[S2],
	FPBDJointUtilities::GetSoftSwingStiffness(SolverSettings, JointSettings),
					FPBDJointUtilities::GetSoftSwingDamping(SolverSettings, JointSettings),
					FPBDJointUtilities::GetSwingStiffness(SolverSettings, JointSettings));

	const FVec3 Twist0 = ConnectorRs[0] * FJointConstants::TwistAxis();
	const FVec3 Twist1 = ConnectorRs[1] * FJointConstants::TwistAxis();
	const bool bDegenerate = (FVec3::DotProduct(Twist0, Twist1) < Chaos_Joint_DegenerateRotationLimit);

	// Apply twist constraint
	// NOTE: Cannot calculate twist angle at 180degree swing
	if (SolverSettings.bEnableTwistLimits)
	{
		if (bAngularLimited[TW] && !bDegenerate)
		{
			InitTwistConstraint(JointSettings, Dt);
		}
	}

	// Apply swing constraints
	// NOTE: Cannot separate swing angles at 180degree swing (but we can still apply locks)
	if (SolverSettings.bEnableSwingLimits)
	{
		if (bAngularLimited[S1] && bAngularLimited[S2])
		{
			// When using non linear solver, the cone swing direction could change at each iteration
			// stabilizing the solver. In the linear case we need to constraint along the 2 directions
			// for better stability
			InitPyramidSwingConstraint(JointSettings, Dt, true, true);
		}
		else if (bAngularLimited[S1] && bAngularLocked[S2])
		{
			if (!bDegenerate)
			{
				InitPyramidSwingConstraint(JointSettings, Dt, true, false);
			}
		}
		else if (bAngularLimited[S1] && bAngularFree[S2])
		{
			if (!bDegenerate)
			{
				InitDualConeSwingConstraint(JointSettings, Dt, EJointAngularConstraintIndex::Swing1);
			}
		}
		else if (bAngularLocked[S1] && bAngularLimited[S2])
		{
			if (!bDegenerate)
			{
				InitPyramidSwingConstraint(JointSettings, Dt, false, true);
			}
		}
		else if (bAngularFree[S1] && bAngularLimited[S2])
		{
			if (!bDegenerate)
			{
				InitDualConeSwingConstraint(JointSettings, Dt, EJointAngularConstraintIndex::Swing2);
			}
		}
	}

	// Note: single-swing locks are already handled above so we only need to do something here if both are locked
	const bool bLockedTwist = SolverSettings.bEnableTwistLimits && bAngularLocked[TW];
	const bool bLockedSwing1 = SolverSettings.bEnableSwingLimits && bAngularLocked[S1];
	const bool bLockedSwing2 = SolverSettings.bEnableSwingLimits && bAngularLocked[S2];
	if (bLockedTwist || bLockedSwing1 || bLockedSwing2)
	{
		InitLockedRotationConstraints(JointSettings, Dt, bLockedTwist, bLockedSwing1, bLockedSwing2);
	}
}

void FPBDJointCachedSolver::InitRotationDatasMass(
		FAxisConstraintDatas& RotationDatas,
		const int32 ConstraintIndex,
		const FReal Dt)
{
	const FVec3 IA0 = Utilities::Multiply(InvI(0), RotationDatas.ConstraintAxis[ConstraintIndex]);
	const FVec3 IA1 = Utilities::Multiply(InvI(1), RotationDatas.ConstraintAxis[ConstraintIndex]);
	const FReal II0 = FVec3::DotProduct(RotationDatas.ConstraintAxis[ConstraintIndex], IA0);
	const FReal II1 = FVec3::DotProduct(RotationDatas.ConstraintAxis[ConstraintIndex], IA1);

	RotationDatas.UpdateMass(ConstraintIndex,  IA0, IA1, II0 + II1, Dt);
}

void FPBDJointCachedSolver::InitRotationConstraintDatas(
		const FPBDJointSettings& JointSettings,
		const int32 ConstraintIndex,
		const FVec3& ConstraintAxis,
		const FReal ConstraintAngle,
		const FReal ConstraintRestitution,
		const FReal Dt,
		const bool bCheckLimit)
{
	const FVec3 LocalAxis = (ConstraintAngle < 0.0f) ? -ConstraintAxis : ConstraintAxis;
	const FReal LocalAngle = (ConstraintAngle < 0.0f) ? -ConstraintAngle : ConstraintAngle;

	RotationConstraints.UpdateDatas(ConstraintIndex, LocalAxis, LocalAngle, ConstraintRestitution, bCheckLimit);

	RotationConstraints.ConstraintLimits[ConstraintIndex] = FMath::Max(
		JointSettings.AngularLimits[ConstraintIndex] - GetAngularConstraintPadding(ConstraintIndex), (FReal)0.);

	InitConstraintAxisAngularVelocities[ConstraintIndex] = FVec3::DotProduct(W(1) - W(0), LocalAxis);

	InitRotationDatasMass(RotationConstraints, ConstraintIndex, Dt);
}

void FPBDJointCachedSolver::CorrectAxisAngleConstraint(
		const FPBDJointSettings& JointSettings,
		const int32 ConstraintIndex,
		FVec3& ConstraintAxis,
		FReal& ConstraintAngle) const
{
	const FReal AngleMax = FMath::Max(JointSettings.AngularLimits[ConstraintIndex] -
		GetAngularConstraintPadding(ConstraintIndex), (FReal)0.);

	if (ConstraintAngle > AngleMax)
	{
		ConstraintAngle = ConstraintAngle - AngleMax;
	}
	else if (ConstraintAngle < -AngleMax)
	{
		// Keep Twist error positive
		ConstraintAngle = -ConstraintAngle - AngleMax;
		ConstraintAxis = -ConstraintAxis;
	}
	else
	{
		ConstraintAngle = 0;
	}
}

void FPBDJointCachedSolver::InitTwistConstraint(
		const FPBDJointSettings& JointSettings,
		const FReal Dt)
{
	 FVec3 TwistAxis;
	 FReal TwistAngle;
	 FPBDJointUtilities::GetTwistAxisAngle(ConnectorRs[0], ConnectorRs[1], TwistAxis, TwistAngle);
	
	 // Project the angle directly to avoid checking the limits during the solve.
	 InitRotationConstraintDatas( JointSettings, (int32)EJointAngularConstraintIndex::Twist, TwistAxis, TwistAngle, JointSettings.TwistRestitution, Dt, true);
}

void FPBDJointCachedSolver::InitPyramidSwingConstraint(
   const FPBDJointSettings& JointSettings,
   const FReal Dt,
   const bool bApplySwing1,
   const bool bApplySwing2)
{
	// Decompose rotation of body 1 relative to body 0 into swing and twist rotations, assuming twist is X axis
	FRotation3 R01Twist, R01Swing;
	FPBDJointUtilities::DecomposeSwingTwistLocal(ConnectorRs[0], ConnectorRs[1], R01Swing, R01Twist);

	const FRotation3 R0Swing = ConnectorRs[0] * R01Swing;

	if(bApplySwing1)
	{
		const FVec3 SwingAxis = R0Swing * FJointConstants::Swing1Axis();
		const FReal SwingAngle = 4.0 * FMath::Atan2(R01Swing.Z, (FReal)(1. + R01Swing.W));
		InitRotationConstraintDatas( JointSettings, (int32)EJointAngularConstraintIndex::Swing1, SwingAxis, SwingAngle, JointSettings.SwingRestitution, Dt, true);
	}
	if(bApplySwing2)
	{
		const FVec3 SwingAxis = R0Swing * FJointConstants::Swing2Axis();
		const FReal SwingAngle = 4.0 * FMath::Atan2(R01Swing.Y, (FReal)(1. + R01Swing.W));
		InitRotationConstraintDatas( JointSettings, (int32)EJointAngularConstraintIndex::Swing2, SwingAxis, SwingAngle, JointSettings.SwingRestitution, Dt, true);
	}
}

void FPBDJointCachedSolver::InitConeConstraint(
   const FPBDJointSettings& JointSettings,
   const FReal Dt)
{
	FVec3 SwingAxisLocal;
	FReal SwingAngle = 0.0f;

	FPBDJointUtilities::GetEllipticalConeAxisErrorLocal(ConnectorRs[0], ConnectorRs[1], 0.0, 0.0, SwingAxisLocal, SwingAngle);
	SwingAxisLocal.SafeNormalize();
	
	const FVec3 SwingAxis = ConnectorRs[0] * SwingAxisLocal;
	InitRotationConstraintDatas( JointSettings, (int32)EJointAngularConstraintIndex::Swing2, SwingAxis, SwingAngle, JointSettings.SwingRestitution, Dt, true);
}

void FPBDJointCachedSolver::InitSingleLockedSwingConstraint(
	const FPBDJointSettings& JointSettings,
	const FReal Dt,
	const EJointAngularConstraintIndex SwingConstraintIndex)
{
	//NOTE: SwingAxis is not normalized in this mode. It has length Sin(SwingAngle).
	//Likewise, the SwingAngle is actually Sin(SwingAngle)
    // FVec3 SwingAxis;
    // FReal SwingAngle;
    // FPBDJointUtilities::GetLockedSwingAxisAngle(ConnectorRs[0], ConnectorRs[1], SwingConstraintIndex, SwingAxis, SwingAngle);
    //SwingAxis.SafeNormalize();

    // Using the locked swing axis angle results in potential axis switching since this axis is the result of OtherSwing x TwistAxis
	FVec3 SwingAxis;
	FReal SwingAngle;
	FPBDJointUtilities::GetSwingAxisAngle(ConnectorRs[0], ConnectorRs[1], 0.0, SwingConstraintIndex, SwingAxis, SwingAngle);

	InitRotationConstraintDatas(JointSettings, (int32)SwingConstraintIndex, SwingAxis, SwingAngle, 0.0, Dt, false);
}


void FPBDJointCachedSolver::InitDualConeSwingConstraint(
	const FPBDJointSettings& JointSettings,
	const FReal Dt,
	const EJointAngularConstraintIndex SwingConstraintIndex)
	
{
	FVec3 SwingAxis;
	FReal SwingAngle;
	FPBDJointUtilities::GetDualConeSwingAxisAngle(ConnectorRs[0], ConnectorRs[1], SwingConstraintIndex, SwingAxis, SwingAngle);

	InitRotationConstraintDatas(JointSettings, (int32)SwingConstraintIndex, SwingAxis, SwingAngle, JointSettings.SwingRestitution, Dt, true);

}

void FPBDJointCachedSolver::InitSwingConstraint(
	const FPBDJointSettings& JointSettings,
	const FPBDJointSolverSettings& SolverSettings,
	const FReal Dt,
	const EJointAngularConstraintIndex SwingConstraintIndex)
{
	FVec3 SwingAxis;
	FReal SwingAngle;
	FPBDJointUtilities::GetSwingAxisAngle(ConnectorRs[0], ConnectorRs[1], SolverSettings.SwingTwistAngleTolerance, SwingConstraintIndex, SwingAxis, SwingAngle);

	InitRotationConstraintDatas( JointSettings, (int32)SwingConstraintIndex, SwingAxis, SwingAngle, JointSettings.SwingRestitution, Dt, true);
}

void FPBDJointCachedSolver::InitLockedRotationConstraints(
	  const FPBDJointSettings& JointSettings,
	  const FReal Dt,
	  const bool bApplyTwist,
	  const bool bApplySwing1,
	  const bool bApplySwing2)
{
	FVec3 Axis0, Axis1, Axis2;
	FPBDJointUtilities::GetLockedRotationAxes(ConnectorRs[0], ConnectorRs[1], Axis0, Axis1, Axis2);

	const FRotation3 R01 = ConnectorRs[0].Inverse() * ConnectorRs[1];

	if (bApplyTwist)
	{
		InitRotationConstraintDatas(JointSettings, (int32)EJointAngularConstraintIndex::Twist, Axis0, R01.X, 0.0, Dt, false);
	}

	if (bApplySwing1)
	{
		InitRotationConstraintDatas(JointSettings, (int32)EJointAngularConstraintIndex::Swing1, Axis2, R01.Z, 0.0, Dt, false);
	}

	if (bApplySwing2)
	{
		InitRotationConstraintDatas(JointSettings, (int32)EJointAngularConstraintIndex::Swing2, Axis1, R01.Y, 0.0, Dt, false);
	}
}

/** APPLY ROTATION CONSTRAINT ******************************************************************************************/

void FPBDJointCachedSolver::ApplyRotationConstraints(
	const FReal Dt)
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(RotationConstraints.bValidDatas[ConstraintIndex])
		{
			ApplyRotationConstraint(ConstraintIndex, Dt);
		}
	}
}

void FPBDJointCachedSolver::SolveRotationConstraintDelta(
		const int32 ConstraintIndex, 
		const FReal DeltaLambda,
		const bool bIsSoftConstraint,
		const FAxisConstraintDatas& ConstraintDatas)
{
	const FVec3 DeltaImpulse =  ConstraintDatas.ConstraintAxis[ConstraintIndex]  * DeltaLambda;
	if(Body(0).IsDynamic())
	{
		const FVec3 DR0 = !bIsSoftConstraint ? ConstraintDatas.ConstraintDRAxis[ConstraintIndex][0] * DeltaLambda :
			DeltaImpulse *  (FVec3::DotProduct(ConstraintDatas.ConstraintAxis[ConstraintIndex], ConstraintDatas.ConstraintDRAxis[ConstraintIndex][0]));
		ApplyRotationDelta(0, DR0);
	}
	if(Body(1).IsDynamic())
	{
		const FVec3 DR1 = !bIsSoftConstraint ? ConstraintDatas.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda :
			DeltaImpulse * ( FVec3::DotProduct(ConstraintDatas.ConstraintAxis[ConstraintIndex], ConstraintDatas.ConstraintDRAxis[ConstraintIndex][1]));
		ApplyRotationDelta(1, DR1);
	}
	NetAngularImpulse += DeltaImpulse;
	++NumActiveConstraints;
}

void FPBDJointCachedSolver::SolveRotationConstraintHard(
			const int32 ConstraintIndex,
			const FReal DeltaConstraint)
{
	const FReal DeltaLambda = SolverStiffness * RotationConstraints.ConstraintHardStiffness[ConstraintIndex] * DeltaConstraint /
		RotationConstraints.ConstraintHardIM[ConstraintIndex];

	RotationConstraints.ConstraintLambda[ConstraintIndex] += DeltaLambda;
	SolveRotationConstraintDelta(ConstraintIndex, DeltaLambda, false, RotationConstraints);
}

void FPBDJointCachedSolver::SolveRotationConstraintSoft(
			const int32 ConstraintIndex,
			const FReal DeltaConstraint,
			const FReal Dt,
			const FReal TargetVel)
{
	// Damping angular velocity
	FReal AngVelDt = 0;
	if (RotationConstraints.ConstraintSoftDamping[ConstraintIndex] > KINDA_SMALL_NUMBER)
	{
		const FVec3 W0Dt = FVec3(Body(0).DQ()) + ConnectorWDts[0];
		const FVec3 W1Dt = FVec3(Body(1).DQ()) + ConnectorWDts[1];
		AngVelDt = TargetVel * Dt + FVec3::DotProduct(RotationConstraints.ConstraintAxis[ConstraintIndex] , W0Dt - W1Dt);
	}

	const FReal DeltaLambda = SolverStiffness * (RotationConstraints.ConstraintSoftStiffness[ConstraintIndex] * DeltaConstraint -
		RotationConstraints.ConstraintSoftDamping[ConstraintIndex] * AngVelDt - RotationConstraints.ConstraintLambda[ConstraintIndex]) /
		RotationConstraints.ConstraintSoftIM[ConstraintIndex];
	RotationConstraints.ConstraintLambda[ConstraintIndex] += DeltaLambda;

	SolveRotationConstraintDelta(ConstraintIndex, DeltaLambda,false, RotationConstraints);
}

void FPBDJointCachedSolver::ApplyRotationConstraint(
	const int32 ConstraintIndex,
	const FReal Dt)
{
	FReal DeltaAngle = RotationConstraints.ConstraintCX[ConstraintIndex] +
		FVec3::DotProduct(Body(1).DQ() - Body(0).DQ(), RotationConstraints.ConstraintAxis[ConstraintIndex]);
	
	bool NeedsSolve = false;
	if(RotationConstraints.bLimitsCheck[ConstraintIndex])
	{
		if(DeltaAngle > RotationConstraints.ConstraintLimits[ConstraintIndex] )
		{
			DeltaAngle -= RotationConstraints.ConstraintLimits[ConstraintIndex];
			NeedsSolve = true;
		}
		else if(DeltaAngle < -RotationConstraints.ConstraintLimits[ConstraintIndex])
		{
			DeltaAngle += RotationConstraints.ConstraintLimits[ConstraintIndex];
			NeedsSolve = true;
			
		}
	}

	if (!RotationConstraints.bLimitsCheck[ConstraintIndex]|| (RotationConstraints.bLimitsCheck[ConstraintIndex] && NeedsSolve && FMath::Abs(DeltaAngle) > AngleTolerance))
	{
		if (RotationConstraints.bSoftLimit[ConstraintIndex])
		{
			SolveRotationConstraintSoft(ConstraintIndex, DeltaAngle, Dt, 0.0f);
		}
		else
		{
			if (RotationConstraints.ConstraintRestitution[ConstraintIndex] > 0.0f)
			{
				CalculateAngularConstraintPadding(ConstraintIndex, RotationConstraints.ConstraintRestitution[ConstraintIndex], DeltaAngle);
			}
			SolveRotationConstraintHard(ConstraintIndex, DeltaAngle);
		}
	}
}

/** APPLY ANGULAR VELOCITY CONSTRAINT *********************************************************************************/

void FPBDJointCachedSolver::ApplyAngularVelocityConstraints()
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(RotationConstraints.bValidDatas[ConstraintIndex])
		{
			ApplyAngularVelocityConstraint(ConstraintIndex);
		}
	}
}

void FPBDJointCachedSolver::SolveAngularVelocityConstraint(
	const int32 ConstraintIndex,
	const FReal TargetVel)
{
	const FVec3 CW = W(1) - W(0);

	const FReal DeltaLambda = SolverStiffness * RotationConstraints.ConstraintHardStiffness[ConstraintIndex] *
	 (FVec3::DotProduct(CW, RotationConstraints.ConstraintAxis[ConstraintIndex]) - TargetVel) / RotationConstraints.ConstraintHardIM[ConstraintIndex];

	if(Body(0).IsDynamic())
	{
		const FVec3 DW0 = RotationConstraints.ConstraintDRAxis[ConstraintIndex][0] * DeltaLambda;
	
		Body(0).ApplyAngularVelocityDelta(DW0);
	}
	if(Body(1).IsDynamic())
	{
		const FVec3 DW1 = RotationConstraints.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda;
	
		Body(1).ApplyAngularVelocityDelta(DW1);
	}
}

void FPBDJointCachedSolver::ApplyAngularVelocityConstraint(const int32 ConstraintIndex)
{
	if(!NetAngularImpulse.IsNearlyZero() && (FMath::Abs(RotationConstraints.ConstraintLambda[ConstraintIndex]) > SMALL_NUMBER))
	{
		FReal TargetVel = 0.0f;
		if (RotationConstraints.ConstraintRestitution[ConstraintIndex] != 0.0f)
		{ 
			const FReal InitVel = InitConstraintAxisAngularVelocities[ConstraintIndex];
			TargetVel = InitVel > Chaos_Joint_AngularVelocityThresholdToApplyRestitution ?
				-RotationConstraints.ConstraintRestitution[ConstraintIndex] * InitVel : 0.0f;
		}
		SolveAngularVelocityConstraint(ConstraintIndex, TargetVel);
	}
}

/** INIT POSITION DRIVES *********************************************************************************/

void FPBDJointCachedSolver::InitPositionDrives(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	PositionDrives.bValidDatas[0] = false;
	PositionDrives.bValidDatas[1] = false;
	PositionDrives.bValidDatas[2] = false;

	if (SolverSettings.bEnableDrives)
	{
		TVec3<bool> bDriven =
		{
			(JointSettings.bLinearPositionDriveEnabled[0] || JointSettings.bLinearVelocityDriveEnabled[0]) && (JointSettings.LinearMotionTypes[0] != EJointMotionType::Locked),
			(JointSettings.bLinearPositionDriveEnabled[1] || JointSettings.bLinearVelocityDriveEnabled[1]) && (JointSettings.LinearMotionTypes[1] != EJointMotionType::Locked),
			(JointSettings.bLinearPositionDriveEnabled[2] || JointSettings.bLinearVelocityDriveEnabled[2]) && (JointSettings.LinearMotionTypes[2] != EJointMotionType::Locked),
		};
	
		PositionDrives.bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);

		// Rectangular position drives
		if (bDriven[0] || bDriven[1] || bDriven[2])
		{
			const FMatrix33 R0M = ConnectorRs[0].ToMatrix();
			const FVec3 XTarget = ConnectorXs[0] + ConnectorRs[0] * JointSettings.LinearDrivePositionTarget;
			const FVec3 VTarget = ConnectorRs[0] * JointSettings.LinearDriveVelocityTarget;
			const FVec3 CX = ConnectorXs[1] - XTarget;

			for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
			{
				if (bDriven[AxisIndex])
				{
					PositionDrives.InitDatas(AxisIndex, true, FPBDJointUtilities::GetLinearDriveStiffness(SolverSettings, JointSettings, AxisIndex),
						FPBDJointUtilities::GetLinearDriveDamping(SolverSettings, JointSettings, AxisIndex), 0.0f);
					const FVec3 Axis = R0M.GetAxis(AxisIndex);
				
					if ((FMath::Abs(FVec3::DotProduct(CX,Axis)) > PositionTolerance) || (PositionDrives.ConstraintSoftDamping[AxisIndex] > 0.0f))
					{
						InitAxisPositionDrive(AxisIndex, Axis, CX, VTarget, Dt);
					}
				}
			}
		}
	}
}

void FPBDJointCachedSolver::InitAxisPositionDrive(
		const int32 ConstraintIndex,
		const FVec3& ConstraintAxis,
		const FVec3& DeltaPosition,
		const FVec3& DeltaVelocity,
		const FReal Dt)
{	
	const FVec3 ConstraintArm0 = ConnectorXs[0] - CurrentPs[0];
	const FVec3 ConstraintArm1 = ConnectorXs[1] - CurrentPs[1];

	PositionDrives.UpdateDatas(ConstraintIndex, ConstraintAxis, FVec3::DotProduct(DeltaPosition, ConstraintAxis),
		0.0f,  true, ConstraintArm0, ConstraintArm1,
		FVec3::DotProduct(DeltaVelocity, ConstraintAxis));

	InitPositionDatasMass(PositionDrives, ConstraintIndex, Dt);
}
/** APPLY POSITION PROJECTIONS *********************************************************************************/
	
void FPBDJointCachedSolver::ApplyProjections(
		const FReal Dt,
		const FReal InSolverStiffness,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings,
		const bool bLastIteration)
{
	if (!IsDynamic(1))
	{
		// If child is kinematic, return. 
		return;
	}

	SolverStiffness = InSolverStiffness;
	

	if (SolverSettings.bSolvePositionLast)
	{
		ApplyRotationProjection(Dt, SolverSettings, JointSettings);
		ApplyPositionProjection(Dt, SolverSettings, JointSettings);
	}
	else
	{
		ApplyPositionProjection(Dt, SolverSettings, JointSettings);
		ApplyRotationProjection(Dt, SolverSettings, JointSettings);
	}
	if(bLastIteration)
	{
		//Final position fixup
		const TVec3<EJointMotionType>& LinearMotion = JointSettings.LinearMotionTypes;
		const bool bLinearLocked = (LinearMotion[0] == EJointMotionType::Locked) && (LinearMotion[1] == EJointMotionType::Locked) && (LinearMotion[2] == EJointMotionType::Locked);
		if (bLinearLocked)
		{
			const FReal LinearProjection = FPBDJointUtilities::GetLinearProjection(SolverSettings, JointSettings);
			if (JointSettings.bProjectionEnabled && (LinearProjection > 0))
			{
				const FVec3 DP1 = -LinearProjection * (ConnectorXs[1] - ConnectorXs[0]);
				ApplyPositionDelta(1, DP1);
			}
		
			// Add velocity correction from the net projection motion
			if (Chaos_Joint_VelProjectionAlpha > 0.0f)
			{
				const FSolverReal VelocityScale = Chaos_Joint_VelProjectionAlpha  / static_cast<FSolverReal>(Dt);
				const FSolverVec3 DV1 = Body1().DP() * VelocityScale;
				const FSolverVec3 DW1 = Body1().DQ()* VelocityScale;
		
				Body(1).ApplyVelocityDelta(DV1, DW1);
			}
		}
	}
}

void FPBDJointCachedSolver::ApplyRotationProjection(
			const FReal Dt,
			const FPBDJointSolverSettings& SolverSettings,
			const FPBDJointSettings& JointSettings)
{
	const FReal AngularProjection = FPBDJointUtilities::GetAngularProjection(SolverSettings, JointSettings);
	
	const TVec3<EJointMotionType>& LinearMotion = JointSettings.LinearMotionTypes;
	const bool bLinearLocked = (LinearMotion[0] == EJointMotionType::Locked) && (LinearMotion[1] == EJointMotionType::Locked) && (LinearMotion[2] == EJointMotionType::Locked);

	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(RotationConstraints.bValidDatas[ConstraintIndex])
		{
			if (JointSettings.bProjectionEnabled && (AngularProjection > 0.0f))
			{
				FReal DeltaAngle = RotationConstraints.ConstraintCX[ConstraintIndex] +
					FVec3::DotProduct(Body(1).DQ() - Body(0).DQ(), RotationConstraints.ConstraintAxis[ConstraintIndex]);
	
				bool NeedsSolve = false;
				if(RotationConstraints.bLimitsCheck[ConstraintIndex])
				{
					if(DeltaAngle > RotationConstraints.ConstraintLimits[ConstraintIndex] )
					{
						DeltaAngle -= RotationConstraints.ConstraintLimits[ConstraintIndex];
						NeedsSolve = true;
					}
					else if(DeltaAngle < -RotationConstraints.ConstraintLimits[ConstraintIndex])
					{
						DeltaAngle += RotationConstraints.ConstraintLimits[ConstraintIndex];
						NeedsSolve = true;
			
					}
				}

				if (!RotationConstraints.bLimitsCheck[ConstraintIndex] || (RotationConstraints.bLimitsCheck[ConstraintIndex] && NeedsSolve && FMath::Abs(DeltaAngle) > AngleTolerance))
				{
					const FReal IM = -FVec3::DotProduct(RotationConstraints.ConstraintAxis[ConstraintIndex], RotationConstraints.ConstraintDRAxis[ConstraintIndex][1]);
					const FReal DeltaLambda = SolverStiffness * RotationConstraints.ConstraintHardStiffness[ConstraintIndex] * DeltaAngle / IM;
					
					const FVec3 DR1 = AngularProjection * RotationConstraints.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda;
					ApplyRotationDelta(1, DR1);

					if(bLinearLocked)
					{
						const FVec3 DP1 = -AngularProjection * FVec3::CrossProduct(DR1, PositionConstraints.ConstraintArms[ConstraintIndex][1]);
						ApplyPositionDelta(1,DP1);
					}
				}
			}
		}
	}
}

void FPBDJointCachedSolver::ApplyPositionProjection(
		const FReal Dt,
		const FPBDJointSolverSettings& SolverSettings,
		const FPBDJointSettings& JointSettings)
{
	const FReal LinearProjection = FPBDJointUtilities::GetLinearProjection(SolverSettings, JointSettings);

	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(PositionConstraints.bValidDatas[ConstraintIndex])
		{
			if (JointSettings.bProjectionEnabled && (LinearProjection > 0.0f))
			{
				const FVec3 CX = Body(1).DP()  - Body(0).DP() +
					FVec3::CrossProduct(Body(1).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][1]) -
					FVec3::CrossProduct(Body(0).DQ(), PositionConstraints.ConstraintArms[ConstraintIndex][0]);
				
				FReal DeltaPosition = PositionConstraints.ConstraintCX[ConstraintIndex] + FVec3::DotProduct(CX, PositionConstraints.ConstraintAxis[ConstraintIndex]);
				
				bool NeedsSolve = false;
				if(PositionConstraints.bLimitsCheck[ConstraintIndex])
				{
					if(DeltaPosition > PositionConstraints.ConstraintLimits[ConstraintIndex] )
					{
						DeltaPosition -= PositionConstraints.ConstraintLimits[ConstraintIndex];
						NeedsSolve = true;
					}
					else if(DeltaPosition < -PositionConstraints.ConstraintLimits[ConstraintIndex])
					{
						DeltaPosition += PositionConstraints.ConstraintLimits[ConstraintIndex];
						NeedsSolve = true;
					}
				}
				if (!PositionConstraints.bLimitsCheck[ConstraintIndex] || (PositionConstraints.bLimitsCheck[ConstraintIndex] && NeedsSolve && FMath::Abs(DeltaPosition ) > PositionTolerance))
				{
					const FVec3 AngularAxis1 = FVec3::CrossProduct(PositionConstraints.ConstraintArms[ConstraintIndex][1], PositionConstraints.ConstraintAxis[ConstraintIndex]);
					const FReal IM = InvM(1) - FVec3::DotProduct(AngularAxis1, PositionConstraints.ConstraintDRAxis[ConstraintIndex][1]);
					const FReal DeltaLambda = SolverStiffness * PositionConstraints.ConstraintHardStiffness[ConstraintIndex] * DeltaPosition / IM;
	
					const FVec3 DX = PositionConstraints.ConstraintAxis[ConstraintIndex] * DeltaLambda;
					
					const FVec3 DP1 = -LinearProjection * InvM(1) * DX;
					const FVec3 DR1 = LinearProjection * PositionConstraints.ConstraintDRAxis[ConstraintIndex][1] * DeltaLambda;
					
					ApplyPositionDelta(1,DP1);
					ApplyRotationDelta(1,DR1);
				}
			}
		}
	}
}

/** APPLY POSITION  DRIVES *********************************************************************************/

void FPBDJointCachedSolver::ApplyPositionDrives(
		const FReal Dt)
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(PositionDrives.bValidDatas[ConstraintIndex])
		{
			ApplyAxisPositionDrive(ConstraintIndex, Dt);
		}
	}
}
	

void FPBDJointCachedSolver::ApplyAxisPositionDrive(
		const int32 ConstraintIndex,
		const FReal Dt)
{
	const FVec3 Delta0 = Body(0).DP() + FVec3::CrossProduct(Body(0).DQ(), PositionDrives.ConstraintArms[ConstraintIndex][0]);
	const FVec3 Delta1 = Body(1).DP() + FVec3::CrossProduct(Body(1).DQ(), PositionDrives.ConstraintArms[ConstraintIndex][1]);

	const FReal DeltaPos = PositionDrives.ConstraintCX[ConstraintIndex] + FVec3::DotProduct(Delta1 - Delta0, PositionDrives.ConstraintAxis[ConstraintIndex]);

	FReal VelDt = 0;
	if (PositionDrives.ConstraintSoftDamping[ConstraintIndex] > KINDA_SMALL_NUMBER)
	{
		const FVec3 V0Dt = FVec3::CalculateVelocity(InitConnectorXs[0], ConnectorXs[0]+ Delta0, 1.0f);
		const FVec3 V1Dt = FVec3::CalculateVelocity(InitConnectorXs[1], ConnectorXs[1]+ Delta1, 1.0f);
		VelDt = PositionDrives.ConstraintVX[ConstraintIndex] * Dt + FVec3::DotProduct(V0Dt - V1Dt, PositionDrives.ConstraintAxis[ConstraintIndex] );
	}

	const FReal DeltaLambda = SolverStiffness * (PositionDrives.ConstraintSoftStiffness[ConstraintIndex] * DeltaPos -
		PositionDrives.ConstraintSoftDamping[ConstraintIndex] * VelDt - PositionDrives.ConstraintLambda[ConstraintIndex]) /
		PositionDrives.ConstraintSoftIM[ConstraintIndex];
	PositionDrives.ConstraintLambda[ConstraintIndex] += DeltaLambda;

	SolvePositionConstraintDelta(ConstraintIndex, DeltaLambda, PositionDrives);
}

/** INIT ROTATION DRIVES *********************************************************************************/

void FPBDJointCachedSolver::InitRotationDrives(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	RotationDrives.bValidDatas[0] = false;
	RotationDrives.bValidDatas[1] = false;
	RotationDrives.bValidDatas[2] = false;

	bool bHasRotationDrives =
		JointSettings.bAngularTwistPositionDriveEnabled
			|| JointSettings.bAngularTwistVelocityDriveEnabled
			|| JointSettings.bAngularSwingPositionDriveEnabled
			|| JointSettings.bAngularSwingVelocityDriveEnabled
			|| JointSettings.bAngularSLerpPositionDriveEnabled
			|| JointSettings.bAngularSLerpVelocityDriveEnabled;
	if (!bHasRotationDrives)
	{
		return;
	}

	EJointMotionType TwistMotion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Twist];
	EJointMotionType Swing1Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing1];
	EJointMotionType Swing2Motion = JointSettings.AngularMotionTypes[(int32)EJointAngularConstraintIndex::Swing2];

	if (SolverSettings.bEnableDrives)
	{
		bool bTwistLocked = TwistMotion == EJointMotionType::Locked;
		bool bSwing1Locked = Swing1Motion == EJointMotionType::Locked;
		bool bSwing2Locked = Swing2Motion == EJointMotionType::Locked;

		// No SLerp drive if we have a locked rotation (it will be grayed out in the editor in this case, but could still have been set before the rotation was locked)
		// @todo(ccaulfield): setting should be cleaned up before being passed to the solver
		if ((JointSettings.bAngularSLerpPositionDriveEnabled || JointSettings.bAngularSLerpVelocityDriveEnabled) && !bTwistLocked && !bSwing1Locked && !bSwing2Locked)
		{
			InitSLerpDrive(Dt, SolverSettings, JointSettings);
		}
		else
		{
			const bool bTwistDriveEnabled = ((JointSettings.bAngularTwistPositionDriveEnabled || JointSettings.bAngularTwistVelocityDriveEnabled) && !bTwistLocked);
			const bool bSwingDriveEnabled = (JointSettings.bAngularSwingPositionDriveEnabled || JointSettings.bAngularSwingVelocityDriveEnabled);
			const bool bSwing1DriveEnabled = bSwingDriveEnabled && !bSwing1Locked;
			const bool bSwing2DriveEnabled = bSwingDriveEnabled && !bSwing2Locked;
			if (bTwistDriveEnabled || bSwing1DriveEnabled || bSwing2DriveEnabled)
			{
				InitSwingTwistDrives(Dt, SolverSettings, JointSettings, bTwistDriveEnabled, bSwing1DriveEnabled, bSwing2DriveEnabled);
			}
		}
	}
}

void FPBDJointCachedSolver::InitRotationConstraintDrive(
			const int32 ConstraintIndex,
			const FVec3& ConstraintAxis,
			const FReal Dt,
			const FReal DeltaAngle)
{
	RotationDrives.UpdateDatas(ConstraintIndex, ConstraintAxis, DeltaAngle,0.0f);

	InitRotationDatasMass(RotationDrives, ConstraintIndex, Dt);
}

void FPBDJointCachedSolver::InitSwingTwistDrives(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings,
	const bool bTwistDriveEnabled,
	const bool bSwing1DriveEnabled,
	const bool bSwing2DriveEnabled)
{
	FRotation3 R1Target = ConnectorRs[0] * JointSettings.AngularDrivePositionTarget;
	R1Target.EnforceShortestArcWith(ConnectorRs[1]);
	FRotation3 R1Error = R1Target.Inverse() * ConnectorRs[1];
	FVec3 R1TwistAxisError = R1Error * FJointConstants::TwistAxis();

	// Angle approximation Angle ~= Sin(Angle) for small angles, underestimates for large angles
	const FReal DTwistAngle = 2.0f * R1Error.X;
	const FReal DSwing1Angle = R1TwistAxisError.Y;
	const FReal DSwing2Angle = -R1TwistAxisError.Z;

	const int32 TW = (int32)EJointAngularConstraintIndex::Twist;
	const int32 S1 = (int32)EJointAngularConstraintIndex::Swing1;
	const int32 S2 = (int32)EJointAngularConstraintIndex::Swing2;

	RotationDrives.InitDatas(TW, true, FPBDJointUtilities::GetAngularTwistDriveStiffness(SolverSettings, JointSettings),
		FPBDJointUtilities::GetAngularTwistDriveDamping(SolverSettings, JointSettings), 0.0);
	RotationDrives.InitDatas(S1, true, FPBDJointUtilities::GetAngularSwingDriveStiffness(SolverSettings, JointSettings),
		FPBDJointUtilities::GetAngularSwingDriveDamping(SolverSettings, JointSettings), 0.0);
	RotationDrives.InitDatas(S2, true, FPBDJointUtilities::GetAngularSwingDriveStiffness(SolverSettings, JointSettings),
		FPBDJointUtilities::GetAngularSwingDriveDamping(SolverSettings, JointSettings), 0.0);

	RotationDrives.bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);

	const bool bUseTwistDrive = bTwistDriveEnabled && (((FMath::Abs(DTwistAngle) > AngleTolerance) && (RotationDrives.ConstraintSoftStiffness[TW] > 0.0f)) || (RotationDrives.ConstraintSoftDamping[TW]  > 0.0f));
	if (bUseTwistDrive)
	{
		InitRotationConstraintDrive(TW, ConnectorRs[1] * FJointConstants::TwistAxis(), Dt, DTwistAngle);
		RotationDrives.ConstraintVX[TW] = JointSettings.AngularDriveVelocityTarget[TW];
	}

	const bool bUseSwing1Drive = bSwing1DriveEnabled && (((FMath::Abs(DSwing1Angle) > AngleTolerance) && (RotationDrives.ConstraintSoftStiffness[S1] > 0.0f)) || (RotationDrives.ConstraintSoftDamping[S1] > 0.0f));
	if (bUseSwing1Drive)
	{
		InitRotationConstraintDrive(S1, ConnectorRs[1] * FJointConstants::Swing1Axis(),  Dt, DSwing1Angle);
		RotationDrives.ConstraintVX[S1] = JointSettings.AngularDriveVelocityTarget[S1];
	}

	const bool bUseSwing2Drive = bSwing2DriveEnabled && (((FMath::Abs(DSwing2Angle) > AngleTolerance) && (RotationDrives.ConstraintSoftStiffness[S2] > 0.0f)) || (RotationDrives.ConstraintSoftDamping[S2] > 0.0f));
	if (bUseSwing2Drive)
	{
		InitRotationConstraintDrive(S2, ConnectorRs[1] * FJointConstants::Swing2Axis(),  Dt, DSwing2Angle);
		RotationDrives.ConstraintVX[S2] = JointSettings.AngularDriveVelocityTarget[S2];
	}
}

void FPBDJointCachedSolver::InitSLerpDrive(
	const FReal Dt,
	const FPBDJointSolverSettings& SolverSettings,
	const FPBDJointSettings& JointSettings)
{
	for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
	{
		RotationDrives.InitDatas(AxisIndex, true, FPBDJointUtilities::GetAngularSLerpDriveStiffness(SolverSettings, JointSettings),
						FPBDJointUtilities::GetAngularSLerpDriveDamping(SolverSettings, JointSettings), 0.0);
	}
	RotationDrives.bAccelerationMode = FPBDJointUtilities::GetDriveAccelerationMode(SolverSettings, JointSettings);

	// If damping is enabled, we need to apply the drive about all 3 axes, but without damping we can just drive along the axis of error
	if (RotationDrives.ConstraintSoftDamping[0]  > 0.0f)
	{
		// NOTE: Slerp target velocity only works properly if we have a stiffness of zero.
		FVec3 Axes[3] = { FVec3(1, 0, 0), FVec3(0, 1, 0), FVec3(0, 0, 1) };
		if (RotationDrives.ConstraintSoftStiffness[0] > 0.0f)
		{
			FPBDJointUtilities::GetLockedRotationAxes(ConnectorRs[0], ConnectorRs[1], Axes[0], Axes[1], Axes[2]);
			Utilities::NormalizeSafe(Axes[0], KINDA_SMALL_NUMBER);
			Utilities::NormalizeSafe(Axes[1], KINDA_SMALL_NUMBER);
			Utilities::NormalizeSafe(Axes[2], KINDA_SMALL_NUMBER);
		}
		const FRotation3 R01 = ConnectorRs[0].Inverse() * ConnectorRs[1];
		FRotation3 TargetAngPos = JointSettings.AngularDrivePositionTarget;
		TargetAngPos.EnforceShortestArcWith(R01);
		const FRotation3 R1Error = TargetAngPos.Inverse() * R01;
		FReal AxisAngles[3] = 
		{ 
			2.0f * FMath::Asin(R1Error.X), 
			2.0f * FMath::Asin(R1Error.Y), 
			2.0f * FMath::Asin(R1Error.Z) 
		};

		const FVec3 TargetAngVel = ConnectorRs[0] * JointSettings.AngularDriveVelocityTarget;

		for (int32 AxisIndex = 0; AxisIndex < 3; ++AxisIndex)
		{
			InitRotationConstraintDrive(AxisIndex, Axes[AxisIndex],  Dt, AxisAngles[AxisIndex]);
			RotationDrives.ConstraintVX[AxisIndex] = FVec3::DotProduct(TargetAngVel, RotationDrives.ConstraintAxis[AxisIndex]);
		}
	}
	else
	{
		const FRotation3 TargetR1 = ConnectorRs[0] * JointSettings.AngularDrivePositionTarget;
		const FRotation3 DR = TargetR1 * ConnectorRs[1].Inverse();

		FVec3 SLerpAxis;
		FReal SLerpAngle;
		if (DR.ToAxisAndAngleSafe(SLerpAxis, SLerpAngle, FVec3(1, 0, 0)))
		{
			if (SLerpAngle > (FReal)PI)
			{
				SLerpAngle = SLerpAngle - (FReal)2 * PI;
			}

			if (FMath::Abs(SLerpAngle) > AngleTolerance)
			{
				FReal AngVelTarget = (JointSettings.AngularDriveDamping > FReal(0)) ? FVec3::DotProduct(SLerpAxis, ConnectorRs[0] * JointSettings.AngularDriveVelocityTarget) : 0.0f;
				InitRotationConstraintDrive((int32)EJointAngularConstraintIndex::Swing1, SLerpAxis, Dt, -SLerpAngle);
				RotationDrives.ConstraintVX[(int32)EJointAngularConstraintIndex::Swing1] = AngVelTarget;
			}
		}
	}
}

/** APPLY ROTATION DRIVES *********************************************************************************/

void FPBDJointCachedSolver::ApplyRotationDrives(
	const FReal Dt)
{
	for(int32 ConstraintIndex = 0; ConstraintIndex < 3; ++ConstraintIndex)
	{
		if(RotationDrives.bValidDatas[ConstraintIndex])
		{
			ApplyAxisRotationDrive(ConstraintIndex, Dt);
		}
	}
}

void FPBDJointCachedSolver::ApplyAxisRotationDrive(
		const int32 ConstraintIndex,
		const FReal Dt)
{
	const FReal DeltaConstraint = RotationDrives.ConstraintCX[ConstraintIndex]+
		FVec3::DotProduct(Body(1).DQ()-Body(0).DQ(), RotationDrives.ConstraintAxis[ConstraintIndex]);

	// Damping angular velocity
	FReal AngVelDt = 0;
	if (RotationDrives.ConstraintSoftDamping[ConstraintIndex] > KINDA_SMALL_NUMBER)
	{
		const FVec3 W0Dt = FVec3(Body(0).DQ()) + ConnectorWDts[0];
		const FVec3 W1Dt = FVec3(Body(1).DQ()) + ConnectorWDts[1];
		AngVelDt = RotationDrives.ConstraintVX[ConstraintIndex] * Dt + FVec3::DotProduct(RotationDrives.ConstraintAxis[ConstraintIndex] , W0Dt - W1Dt);
	}

	const FReal DeltaLambda = SolverStiffness * (RotationDrives.ConstraintSoftStiffness[ConstraintIndex] * DeltaConstraint -
		RotationDrives.ConstraintSoftDamping[ConstraintIndex] * AngVelDt - RotationDrives.ConstraintLambda[ConstraintIndex]) /
		RotationDrives.ConstraintSoftIM[ConstraintIndex];
	RotationDrives.ConstraintLambda[ConstraintIndex] += DeltaLambda;

	SolveRotationConstraintDelta(ConstraintIndex, DeltaLambda,true, RotationDrives);
}

 // Joint utilities

void FPBDJointCachedSolver::ApplyPositionDelta(
	const int32 BodyIndex,
	const FVec3& DP)
{
	Body(BodyIndex).ApplyPositionDelta(DP);
}

void FPBDJointCachedSolver::ApplyRotationDelta(
	const int32 BodyIndex,
	const FVec3& DR)
{
	Body(BodyIndex).ApplyRotationDelta(DR);
}

// Used for non-zero restitution. We pad constraints by an amount such that the velocity
// calculated after solving constraint positions will as required for the restitution.
void FPBDJointCachedSolver::CalculateLinearConstraintPadding(
	const int32 ConstraintIndex,
	const FReal Dt,
	const FReal Restitution,
	FReal& InOutPos)
{
	// NOTE: We only calculate the padding after the constraint is first violated, and after
	// that the padding is fixed for the rest of the iterations in the current step.
	if ((Restitution > 0.0f) && (InOutPos > 0.0f) && !HasLinearConstraintPadding(ConstraintIndex))
	{
		SetLinearConstraintPadding(ConstraintIndex, 0.0f);

		// Calculate the velocity we want to match
    
		const FVec3 V0Dt = FVec3::CalculateVelocity(InitConnectorXs[0], ConnectorXs[0]+Body(0).DP() + FVec3::CrossProduct(Body(0).DQ(), PositionConstraints.ConstraintArms[PointPositionConstraintIndex][0]), 1.0f);
		const FVec3 V1Dt = FVec3::CalculateVelocity(InitConnectorXs[1], ConnectorXs[1]+Body(1).DP() + FVec3::CrossProduct(Body(1).DQ(), PositionConstraints.ConstraintArms[PointPositionConstraintIndex][1]), 1.0f);
		const FReal AxisVDt = FVec3::DotProduct(V1Dt - V0Dt, PositionConstraints.ConstraintAxis[ConstraintIndex]);

		// Calculate the padding to apply to the constraint that will result in the
		// desired outward velocity (assuming the constraint is fully resolved)
		const FReal Padding = (1.0f + Restitution) * AxisVDt - InOutPos;
		if (Padding > 0.0f)
		{
			SetLinearConstraintPadding(ConstraintIndex, Padding);
			InOutPos += Padding;
		}
	}
}

// Used for non-zero restitution. We pad constraints by an amount such that the velocity
// calculated after solving constraint positions will as required for the restitution.
void FPBDJointCachedSolver::CalculateAngularConstraintPadding(
	const int32 ConstraintIndex,
	const FReal Restitution,
	FReal& InOutAngle)
{
	// NOTE: We only calculate the padding after the constraint is first violated, and after
	// that the padding is fixed for the rest of the iterations in the current step.
	if ((Restitution > 0.0f) && (InOutAngle > 0.0f) && !HasAngularConstraintPadding(ConstraintIndex))
	{
		SetAngularConstraintPadding(ConstraintIndex, 0.0f);

		// Calculate the velocity we want to match
		const FVec3 W0Dt = FVec3(Body(0).DQ()) + ConnectorWDts[0];
		const FVec3 W1Dt = FVec3(Body(1).DQ()) + ConnectorWDts[1];
		const FReal AxisWDt = FVec3::DotProduct(W1Dt - W0Dt, RotationConstraints.ConstraintAxis[(int32)ConstraintIndex]);

		// Calculate the padding to apply to the constraint that will result in the
		// desired outward velocity (assuming the constraint is fully resolved)
		const FReal Padding = (1.0f + Restitution) * AxisWDt - InOutAngle;
		if (Padding > 0.0f)
		{
			SetAngularConstraintPadding(ConstraintIndex, Padding);
			InOutAngle += Padding;
		}
	}
}

void FAxisConstraintDatas::InitDatas(
	const int32 ConstraintIndex,
	const bool bHasSoftLimits,
	const FReal SoftStiffness,
	const FReal SoftDamping,
	const FReal HardStiffness)
{
	bSoftLimit[ConstraintIndex] = bHasSoftLimits;
	ConstraintHardStiffness[ConstraintIndex] = HardStiffness;
	ConstraintSoftStiffness[ConstraintIndex] = SoftStiffness;
	ConstraintSoftDamping[ConstraintIndex] = SoftDamping;
	SettingsSoftStiffness[ConstraintIndex] = SoftStiffness;
	SettingsSoftDamping[ConstraintIndex] = SoftDamping;
	bValidDatas[ConstraintIndex] = false;
	bLimitsCheck[ConstraintIndex] = true;
	ConstraintLambda = FVec3::Zero();
	ConstraintLimits = FVec3::Zero();
	MotionType[ConstraintIndex] = EJointMotionType::Free;
}

void FAxisConstraintDatas::UpdateDatas(
	const int32 ConstraintIndex,
	const FVec3& DatasAxis,
	const FReal DatasCX,
	const FReal DatasRestitution,
	const bool bCheckLimit,
	const FVec3& DatasArm0 ,
	const FVec3& DatasArm1 ,
	const FReal DatasVX)
{
	bValidDatas[ConstraintIndex] = true;
	bLimitsCheck[ConstraintIndex] = bCheckLimit;

	ConstraintCX[ConstraintIndex] = DatasCX;
	ConstraintVX[ConstraintIndex] = DatasVX;
	ConstraintAxis[ConstraintIndex] = DatasAxis;
	ConstraintRestitution[ConstraintIndex] = DatasRestitution;
	ConstraintArms[ConstraintIndex][0] = DatasArm0;
	ConstraintArms[ConstraintIndex][1] = DatasArm1;
}

void FAxisConstraintDatas::UpdateMass(
		const int32 ConstraintIndex,
		const FVec3& DatasIA0,
		const FVec3& DatasIA1,
		const FReal DatasIM,
		const FReal Dt)
{
	ConstraintDRAxis[ConstraintIndex][0] = DatasIA0;
	ConstraintDRAxis[ConstraintIndex][1] = -DatasIA1;
	ConstraintHardIM[ConstraintIndex] = DatasIM;
	ConstraintLambda = FVec3::Zero();

	if(bSoftLimit[ConstraintIndex])
	{
		const FReal SpringMassScale = (bAccelerationMode) ? (FReal)1 / (ConstraintHardIM[ConstraintIndex]) : (FReal)1;
		ConstraintSoftStiffness[ConstraintIndex] = SpringMassScale * SettingsSoftStiffness[ConstraintIndex] * Dt * Dt;
		ConstraintSoftDamping[ConstraintIndex] = SpringMassScale * SettingsSoftDamping[ConstraintIndex] * Dt;
		ConstraintSoftIM[ConstraintIndex] = (ConstraintSoftStiffness[ConstraintIndex] + ConstraintSoftDamping[ConstraintIndex]) * ConstraintHardIM[ConstraintIndex] + (FReal)1;
	}
}
	
}


