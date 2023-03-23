// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/ExternalCollisionData.h"
#include "Chaos/PBDCollisionConstraints.h"
#include "Chaos/CollisionResolutionTypes.h"
#include "Chaos/Collision/CollisionApplyType.h"

namespace Chaos
{
	class FCollisionContext;

	namespace Collisions
	{
		// @todo(chaos): remove this
		struct FContactParticleParameters 
		{
			FContactParticleParameters()
				: RestitutionVelocityThreshold(0)
				, bCanDisableContacts(false)
				, Collided(nullptr)
			{}

			FContactParticleParameters(
				FReal InRestitutionVelocityThreshold,
				bool bInCanDisableContacts,
				TArrayCollectionArray<bool>* InCollided)
				: RestitutionVelocityThreshold(InRestitutionVelocityThreshold)
				, bCanDisableContacts(bInCanDisableContacts)
				, Collided(InCollided)
			{}


			FReal RestitutionVelocityThreshold;
			bool bCanDisableContacts;
			TArrayCollectionArray<bool>* Collided;
		};

		struct FContactIterationParameters 
		{
			FContactIterationParameters()
				: Dt(0)
				, Iteration(0)
				, NumIterations(0)
				, NumPairIterations(0)
				, SolverType(EConstraintSolverType::None)
				, NeedsAnotherIteration(nullptr)
			{}

			FContactIterationParameters(
				const FReal InDt,
				const int32 InIteration,
				const int32 InNumIterations,
				const int32 InNumPairIterations,
				const EConstraintSolverType InSolverType,
				bool* InNeedsAnotherIteration)
				: Dt(InDt)
				, Iteration(InIteration)
				, NumIterations(InNumIterations)
				, NumPairIterations(InNumPairIterations)
				, SolverType(InSolverType)
				, NeedsAnotherIteration(InNeedsAnotherIteration)
			{}

			const FReal Dt;
			const int32 Iteration;
			const int32 NumIterations;
			const int32 NumPairIterations;
			const EConstraintSolverType SolverType;	// @todo(chaos): a better way to customize the collision solver
			bool* NeedsAnotherIteration;
		};

		// Update the constraint (re-runs collision detection for this contact)
		extern void Update(FPBDCollisionConstraint& Constraint, const FReal Dt);
		extern void UpdateSwept(FPBDCollisionConstraint& Constraint, const FReal Dt);

		extern void Apply(FPBDCollisionConstraint& Constraint, const FContactIterationParameters& IterationParameters, const FContactParticleParameters& ParticleParameters);

		extern void ApplyPushOut(FPBDCollisionConstraint& Constraint, const FContactIterationParameters& IterationParameters, const FContactParticleParameters& ParticleParameters);
	}

}
