// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "Chaos/Box.h"
#include "Chaos/Capsule.h"
#include "Chaos/EPA.h"
#include "Chaos/GJKShape.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Simplex.h"
#include "Chaos/Sphere.h"

#include "ChaosCheck.h"
#include "ChaosLog.h"


// Platform specific vector intrinsics include.
#if PLATFORM_ENABLE_VECTORINTRINSICS_NEON || PLATFORM_ENABLE_VECTORINTRINSICS
#include "Math/VectorRegister.h"
#include "Chaos/VectorUtility.h"
#include "Chaos/SimplexVectorized.h"
#include "Chaos/EPAVectorized.h"
#define GJK_VECTORIZED 1
#else
#define GJK_VECTORIZED 0
#endif

namespace Chaos
{

	// Calculate the margins used for queries based on shape radius, shape margins and shape types
	template <typename TGeometryA, typename TGeometryB, typename T>
	void CalculateQueryMargins(const TGeometryA& A, const TGeometryB& B, T& outMarginA, T& outMarginB)
	{
		// Margin selection logic: we only need a small margin for sweeps since we only move the sweeping object
		// to the point where it just touches.
		// Spheres and Capsules: always use the core shape and full "margin" because it represents the radius
		// Sphere/Capsule versus OtherShape: no margin on other
		// OtherShape versus OtherShape: use margin of the smaller shape, zero margin on the other
		const T RadiusA = A.GetRadius();
		const T RadiusB = B.GetRadius();
		const bool bHasRadiusA = RadiusA > 0;
		const bool bHasRadiusB = RadiusB > 0;

		// The sweep margins if required. Only one can be non-zero (we keep the smaller one)
		const T SweepMarginScale = 0.05f;
		const bool bAIsSmallest = A.GetMargin() < B.GetMargin();
		const T SweepMarginA = (bHasRadiusA || bHasRadiusB) ? 0.0f : (bAIsSmallest ? SweepMarginScale * A.GetMargin() : 0.0f);
		const T SweepMarginB = (bHasRadiusA || bHasRadiusB) ? 0.0f : (bAIsSmallest ? 0.0f : SweepMarginScale * B.GetMargin());

		// Net margin (note: both SweepMargins are zero if either Radius is non-zero, and only one SweepMargin can be non-zero)
		outMarginA = RadiusA + SweepMarginA;
		outMarginB = RadiusB + SweepMarginB;
	}
	
	/** 
		Determines if two convex geometries overlap.
		
		@param A The first geometry
		@param B The second geometry
		@param BToATM The transform of B in A's local space
		@param ThicknessA The amount of geometry inflation for Geometry A(for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
		@param InitialDir The first direction we use to search the CSO
		@param ThicknessB The amount of geometry inflation for Geometry B(for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
		@return True if the geometries overlap, False otherwise 
	 */
#if GJK_VECTORIZED
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKIntersection(const TGeometryA& RESTRICT A, const TGeometryB& RESTRICT B, const TRigidTransform<T, 3>& BToATM, const T InThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T InThicknessB = 0)
	{

		const UE::Math::TQuat<T>& RotationDouble = BToATM.GetRotation();
		VectorRegister4Float RotationSimd = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(RotationDouble.X, RotationDouble.Y, RotationDouble.Z, RotationDouble.W));

		const UE::Math::TVector<T>& TranslationDouble = BToATM.GetTranslation();
		const VectorRegister4Float TranslationSimd = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(TranslationDouble.X, TranslationDouble.Y, TranslationDouble.Z, 0.0));
		// Normalize rotation
		RotationSimd = VectorNormalizeSafe(RotationSimd, GlobalVectorConstants::Float0001);

		const VectorRegister4Float InitialDirSimd = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(InitialDir[0], InitialDir[1], InitialDir[2], 0.0));
		
		VectorRegister4Float VSimd = VectorNegate(InitialDirSimd);
		VSimd = VectorNormalizeSafe(VSimd, MakeVectorRegisterFloatConstant(-1.f, 0.f, 0.f, 0.f));

		const VectorRegister4Float AToBRotationSimd = VectorQuaternionInverse(RotationSimd);
		bool bTerminate;
		bool bNearZero = false;
		int NumIterations = 0;
		VectorRegister4Float PrevDist2Simd = MakeVectorRegisterFloatConstant(FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX);

		VectorRegister4Float SimplexSimd[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };
		//VectorRegister4Float As[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };
		//VectorRegister4Float Bs[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };
		VectorRegister4Float BarycentricSimd;
		VectorRegister4Int NumVerts = GlobalVectorConstants::IntZero;

		T ThicknessA;
		T ThicknessB;
		CalculateQueryMargins(A, B, ThicknessA, ThicknessB);
		ThicknessA += InThicknessA;
		ThicknessB += InThicknessB;


		const T Inflation = ThicknessA + ThicknessB + static_cast<T>(1e-3);
		const VectorRegister4Float InflationSimd = MakeVectorRegisterFloat((float)Inflation, (float)Inflation, (float)Inflation, (float)Inflation);

		int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;
		do
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}
			const VectorRegister4Float NegVSimd = VectorNegate(VSimd);
			const VectorRegister4Float SupportASimd = A.SupportCoreSimd(NegVSimd, ThicknessA);
			const VectorRegister4Float VInBSimd = VectorQuaternionRotateVector(AToBRotationSimd, VSimd);
			const VectorRegister4Float SupportBLocalSimd = B.SupportCoreSimd(VInBSimd, ThicknessB);
			//const TVector<T, 3> SupportB = BToATM.TransformPositionNoScale(SupportBLocal);  // Original code
			const VectorRegister4Float SupportBSimd = VectorAdd(VectorQuaternionRotateVector(RotationSimd, SupportBLocalSimd), TranslationSimd);
			const VectorRegister4Float WSimd = VectorSubtract(SupportASimd, SupportBSimd);

			if (VectorMaskBits(VectorCompareGT(VectorDot3(VSimd, WSimd), InflationSimd)))
			{
				return false;
			}

			{
				// Convert simdInt to int
				alignas(16) int32 NumVertsInts[4];
				VectorIntStoreAligned(NumVerts, NumVertsInts);
				const int32 NumVertsInt = NumVertsInts[0];
				SimplexSimd[NumVertsInt] = WSimd;
			}

			NumVerts = VectorIntAdd(NumVerts, GlobalVectorConstants::IntOne);

			VSimd = VectorSimplexFindClosestToOrigin<false>(SimplexSimd, NumVerts, BarycentricSimd, nullptr, nullptr);

			const VectorRegister4Float NewDist2Simd = VectorDot3(VSimd, VSimd);///
			bNearZero = VectorMaskBits(VectorCompareLT(NewDist2Simd, VectorMultiply(InflationSimd, InflationSimd))) != 0;

			//as simplices become degenerate we will stop making progress. This is a side-effect of precision, in that case take V as the current best approximation
			//question: should we take previous v in case it's better?
			bool bMadeProgress = VectorMaskBits(VectorCompareLT(NewDist2Simd, PrevDist2Simd)) != 0;
			bTerminate = bNearZero || !bMadeProgress;
			PrevDist2Simd = NewDist2Simd;

			if (!bTerminate)
			{
				VSimd = VectorDivide(VSimd,VectorSqrt(NewDist2Simd));
			}

		} while (!bTerminate);

		return bNearZero;
	}
#else
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKIntersection(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, const T InThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T InThicknessB = 0)
	{
		TVector<T, 3> V = -InitialDir;
		if (V.SafeNormalize() == 0)
		{
			V = TVec3<T>(-1, 0, 0);
		}

		FSimplex SimplexIDs;
		TVector<T, 3> Simplex[4];
		T Barycentric[4] = { -1,-1,-1,-1 };	//not needed, but compiler warns
		const TRotation<T, 3> AToBRotation = BToATM.GetRotation().Inverse();
		bool bTerminate;
		bool bNearZero = false;
		int NumIterations = 0;
		T PrevDist2 = FLT_MAX;
		T ThicknessA;
		T ThicknessB;
		CalculateQueryMargins(A, B, ThicknessA, ThicknessB);
		ThicknessA += InThicknessA;
		ThicknessB += InThicknessB;
		const T Inflation = ThicknessA + ThicknessB + static_cast<T>(1e-3);
		const T Inflation2 = Inflation * Inflation;
		int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;
		do
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}
			const TVector<T, 3> NegV = -V;
			const TVector<T, 3> SupportA = A.SupportCore(NegV, ThicknessA, nullptr, VertexIndexA);
			const TVector<T, 3> VInB = AToBRotation * V;
			const TVector<T, 3> SupportBLocal = B.SupportCore(VInB, ThicknessB, nullptr, VertexIndexB);
			const TVector<T, 3> SupportB = BToATM.TransformPositionNoScale(SupportBLocal);
			const TVector<T, 3> W = SupportA - SupportB;

			if (TVector<T, 3>::DotProduct(V, W) > Inflation)
			{
				return false;
			}

			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			Simplex[SimplexIDs.NumVerts++] = W;

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric);

			T NewDist2 = V.SizeSquared();
			bNearZero = NewDist2 < Inflation2;

			//as simplices become degenerate we will stop making progress. This is a side-effect of precision, in that case take V as the current best approximation
			//question: should we take previous v in case it's better?
			const bool bMadeProgress = NewDist2 < PrevDist2;
			bTerminate = bNearZero || !bMadeProgress;

			PrevDist2 = NewDist2;

			if (!bTerminate)
			{
				V /= FMath::Sqrt(NewDist2);
			}

		} while (!bTerminate);

		return bNearZero;
	}

#endif

	/** 
		Determines if two convex geometries in the same space overlap
		IMPORTANT: the two convex geometries must be in the same space!

		@param A The first geometry
		@param B The second geometry
		@param ThicknessA The amount of geometry inflation for Geometry A(for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
		@param InitialDir The first direction we use to search the CSO
		@param ThicknessB The amount of geometry inflation for Geometry B(for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
		@return True if the geometries overlap, False otherwise 
	*/
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKIntersectionSameSpace(const TGeometryA& A, const TGeometryB& B, const T InThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T InThicknessB = 0)
	{
		TVector<T, 3> V = -InitialDir;
		if (V.SafeNormalize() == 0)
		{
			V = TVec3<T>(-1, 0, 0);
		}

		FSimplex SimplexIDs;
		TVector<T, 3> Simplex[4];
		T Barycentric[4] = { -1,-1,-1,-1 };	//not needed, but compiler warns
		bool bTerminate;
		bool bNearZero = false;
		int NumIterations = 0;
		T PrevDist2 = FLT_MAX;
		T ThicknessA;
		T ThicknessB;
		CalculateQueryMargins(A, B, ThicknessA, ThicknessB);
		ThicknessA += InThicknessA;
		ThicknessB += InThicknessB;
		const T Inflation = ThicknessA + ThicknessB + static_cast<T>(1e-3);
		const T Inflation2 = Inflation * Inflation;
		int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;
		do
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}
			const TVector<T, 3> NegV = -V;
			const TVector<T, 3> SupportA = A.SupportCore(NegV, ThicknessA, nullptr, VertexIndexA);
			const TVector<T, 3> VInB = V; // same space
			const TVector<T, 3> SupportB = B.SupportCore(VInB, ThicknessB, nullptr, VertexIndexB);
			const TVector<T, 3> W = SupportA - SupportB;

			if (TVector<T, 3>::DotProduct(V, W) > Inflation)
			{
				return false;
			}

			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			Simplex[SimplexIDs.NumVerts++] = W;

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric);

			T NewDist2 = V.SizeSquared();
			bNearZero = NewDist2 < Inflation2;

			//as simplices become degenerate we will stop making progress. This is a side-effect of precision, in that case take V as the current best approximation
			//question: should we take previous v in case it's better?
			const bool bMadeProgress = NewDist2 < PrevDist2;
			bTerminate = bNearZero || !bMadeProgress;

			PrevDist2 = NewDist2;

			if (!bTerminate)
			{
				V /= FMath::Sqrt(NewDist2);
			}

		} while (!bTerminate);

		return bNearZero;
	}

	/**
	 * @brief Internal simplex data for GJK that can also be stored for warm-starting subsequent calls
	 * @tparam T the number type (float or double)
	 *
	 * @see GJKPenetrationWarmStartable
	*/
	template<typename T>
	class TGJKSimplexData
	{
	public:
		TGJKSimplexData()
			: NumVerts(0)
		{}

		// Clear the data - used to start a GJK search from the default search direction
		void Reset()
		{
			NumVerts = 0;
		}

		// Save any data that was not directly updated while iterating in GJK
		void Save(const FSimplex InSimplexIDs)
		{
			// We don't need to store the simplex vertex order because the indices are always
			// sorted at the end of each iteration. We just need to know how many vertices we have.
			NumVerts = InSimplexIDs.NumVerts;
		}

		// Recompute the Simplex and separating vector from the stored data at the current relative transform
		// This aborts if we have no simplex data to restore or the origin is inside the simplex. Outputs must already 
		// have reasonable default values for running GJK without a warm-start.
		void Restore(const TRigidTransform<T, 3>& BToATM, FSimplex& OutSimplexIDs, TVec3<T> OutSimplex[], TVec3<T>& OutV, T& OutDistance, const T Epsilon)
		{
			if (NumVerts > 0)
			{
				OutSimplexIDs.NumVerts = NumVerts;

				for (int32 VertIndex = 0; VertIndex < NumVerts; ++VertIndex)
				{
					OutSimplexIDs.Idxs[VertIndex] = VertIndex;
					OutSimplex[VertIndex] = As[VertIndex] - BToATM.TransformPositionNoScale(Bs[VertIndex]);
				}

				const TVec3<T> V = SimplexFindClosestToOrigin(OutSimplex, OutSimplexIDs, Barycentric, As, Bs);
				const T Distance = V.Size();

				// If the origin is inside the simplex at the new transform, we need to abort the restore
				// This is necessary to cover the very-small separation case where we use the normal
				// calculated in the previous iteration in GJKL, but we have no way to restore that.
				// Note: we have already written to the simplex but that's ok because we reset the vert count.
				if (Distance > Epsilon)
				{
					OutV = V / Distance;
					OutDistance = Distance;
				}
				else
				{
					OutSimplexIDs.NumVerts = 0;
				}
			}
		}

		void Restore2(const TRigidTransform<T, 3>& BToATM, int32& OutNumVerts, TVec3<T> OutSimplex[], TVec3<T>& OutV, T& OutDistance, const T Epsilon)
		{
			OutNumVerts = 0;

			if (NumVerts > 0)
			{
				for (int32 VertIndex = 0; VertIndex < NumVerts; ++VertIndex)
				{
					OutSimplex[VertIndex] = As[VertIndex] - BToATM.TransformPositionNoScale(Bs[VertIndex]);
				}

				const TVec3<T> V = SimplexFindClosestToOrigin2(OutSimplex, NumVerts, Barycentric, As, Bs);
				const T DistanceSq = V.SizeSquared();

				// If the origin is inside the simplex at the new transform, we need to abort the restore
				// This is necessary to cover the very-small separation case where we use the normal
				// calculated in the previous iteration in GJKL, but we have no way to restore that.
				// Note: we have already written to the simplex but that's ok because we reset the vert count.
				if (DistanceSq > FMath::Square(Epsilon))
				{
					const T Distance = FMath::Sqrt(DistanceSq);
					OutNumVerts = NumVerts;
					OutV = V / Distance;
					OutDistance = Distance;
				}
			}
		}


		// Maximum number of vertices that a GJK simplex can have
		static const int32 MaxSimplexVerts = 4;

		// Simplex vertices on shape A, in A-local space
		TVec3<T> As[MaxSimplexVerts];

		// Simplex vertices on shape B, in B-local space
		TVec3<T> Bs[MaxSimplexVerts];

		// Barycentric coordinates of closest point to origin on the simplex
		T Barycentric[MaxSimplexVerts];

		// Number of vertices in the simplex. Up to 4.
		int32 NumVerts;
	};

	// GJK warm-start data at default numeric precision
	using FGJKSimplexData = TGJKSimplexData<FReal>;

	/**
	 * @brief Calculate the penetration data for two shapes using GJK and a warm-start buffer.
	 *
	 * @tparam T Number type (float or double)
	 * @tparam TGeometryA The first shape type
	 * @tparam TGeometryB The second shape type
	 * @param A The first shape
	 * @param B The second shape
	 * @param BToATM A transform from B-local space to A-local space
	 * @param OutPenetration The overlap distance (+ve for overlap, -ve for separation)
	 * @param OutClosestA The closest point on A, in A-local space
	 * @param OutClosestB The closest point on B, in B-local space
	 * @param OutNormalA The contact normal pointing away from A in A-local space
	 * @param OutNormalB The contact normal pointing away from A in B-local space
	 * @param OutVertexA The closest vertex on A
	 * @param OutVertexB The closest vertex on B
	 * @param SimplexData In/out simplex data used to initialize and update GJK. Can be stored to improve convergence on subsequent calls for "small" changes in relative rotation.
	 * @param OutMaxContactDelta The maximum error in the contact position as a result of using a margin. This is the difference between the core shape + margin and the outer shape on the supporting vertices
	 * @param Epsilon The separation distance below which GJK aborts and switches to EPA
	 * @return true if the results are populated (always for this implementation, but @see EPA())
	 *
	 * The WarmStartData is an input and output parameter. If the function is called with
	 * a small change in BToATM there's it will converge much faster, usually in 1 iteration
	 * for polygonal shapes.
	 * 
	 * @note This version returns OutClosestB in B's local space, compated to GJKPenetration where all output is in the space of A
	 * 
	 * @todo(chaos): convert GJKPenetration() to use this version (but see note above)
	 *
	*/
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetrationWarmStartable(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormalA, TVec3<T>& OutNormalB, int32& OutVertexA, int32& OutVertexB, TGJKSimplexData<T>& InOutSimplexData, T& OutMaxSupportDelta, const T Epsilon = T(1.e-3), const T EPAEpsilon = T(1.e-2))
	{
		T SupportDeltaA = 0;
		T SupportDeltaB = 0;
		T MaxSupportDelta = 0;

		int32& VertexIndexA = OutVertexA;
		int32& VertexIndexB = OutVertexB;

		// Return the support vertex in A-local space for a vector V in A-local space
		auto SupportAFunc = [&A, &SupportDeltaA, &VertexIndexA](const TVec3<T>& V)
		{
			return A.SupportCore(V, A.GetMargin(), &SupportDeltaA, VertexIndexA);
		};

		const TRotation<T, 3> AToBRotation = BToATM.GetRotation().Inverse();

		// Return the support point on B, in B-local space, for a vector V in A-local space
		auto SupportBFunc = [&B, &AToBRotation, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
		{
			const TVec3<T> VInB = AToBRotation * V;
			return B.SupportCore(VInB, B.GetMargin(), &SupportDeltaB, VertexIndexB);
		};

		// V and Simplex are in A-local space
		TVec3<T> V = TVec3<T>(-1, 0, 0);
		TVec3<T> Simplex[4];
		FSimplex SimplexIDs;
		T Distance = FLT_MAX;

		// If we have warm-start data, rebuild the simplex from the stored data
		InOutSimplexData.Restore(BToATM, SimplexIDs, Simplex, V, Distance, Epsilon);

		TVec3<T> Normal = -V;					// Remember the last good normal (i.e. don't update it if separation goes less than Epsilon and we can no longer normalize)
		bool bIsDegenerate = false;				// True if GJK cannot make any more progress
		bool bIsContact = false;				// True if shapes are within Epsilon or overlapping - GJK cannot provide a solution
		int32 NumIterations = 0;
		const T ThicknessA = A.GetMargin();
		const T ThicknessB = B.GetMargin();
		const T SeparatedDistance = ThicknessA + ThicknessB + Epsilon;
		while (!bIsContact && !bIsDegenerate)
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			const TVec3<T> NegV = -V;
			const TVec3<T> SupportA = SupportAFunc(NegV);
			const TVec3<T> SupportB = SupportBFunc(V);
			const TVec3<T> SupportBInA = BToATM.TransformPositionNoScale(SupportB);
			const TVec3<T> W = SupportA - SupportBInA;
			MaxSupportDelta = FMath::Max(SupportDeltaA, SupportDeltaB);

			// If we didn't move to at least ConvergedDistance or closer, assume we have reached a minimum
			const T ConvergenceTolerance = 1.e-4f;
			const T ConvergedDistance = (1.0f - ConvergenceTolerance) * Distance;
			const T VW = TVec3<T>::DotProduct(V, W);

			InOutSimplexData.As[SimplexIDs.NumVerts] = SupportA;
			InOutSimplexData.Bs[SimplexIDs.NumVerts] = SupportB;
			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			Simplex[SimplexIDs.NumVerts++] = W;

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, InOutSimplexData.Barycentric, InOutSimplexData.As, InOutSimplexData.Bs);
			T NewDistance = V.Size();

			// Are we overlapping or too close for GJK to get a good result?
			bIsContact = (NewDistance < Epsilon);

			// If we did not get closer in this iteration, we are in a degenerate situation
			bIsDegenerate = (NewDistance >= Distance);

			// If we are not too close, update the separating vector and keep iterating
			// If we are too close we drop through to EPA, but if EPA determines the shapes are actually separated 
			// after all, we will end up using the normal from the previous loop
			if (!bIsContact)
			{
				V /= NewDistance;
				Normal = -V;
			}
			Distance = NewDistance;
		}
		// Save the warm-start data (not much to store since we updated most of it in the loop above)
		InOutSimplexData.Save(SimplexIDs);
		if (bIsContact)
		{
			// We did not converge or we detected an overlap situation, so run EPA to get contact data
			// Rebuild the simplex into a variable sized array - EPA can generate any number of vertices
			TArray<TVec3<T>> VertsA;
			TArray<TVec3<T>> VertsB;
			VertsA.Reserve(8);
			VertsB.Reserve(8);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				VertsA.Add(InOutSimplexData.As[i]);
				VertsB.Add(BToATM.TransformPositionNoScale(InOutSimplexData.Bs[i]));
			}
			
			auto SupportBInAFunc = [&B, &BToATM, &AToBRotation, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
			{
				const TVec3<T> VInB = AToBRotation * V;
				const TVec3<T> SupportBLocal = B.SupportCore(VInB, B.GetMargin(), &SupportDeltaB, VertexIndexB);
				return BToATM.TransformPositionNoScale(SupportBLocal);
			};

			T Penetration;
			TVec3<T> MTD, ClosestA, ClosestBInA;
			const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBInAFunc, Penetration, MTD, ClosestA, ClosestBInA, EPAEpsilon);

			switch (EPAResult)
			{
			case EEPAResult::MaxIterations:
				// Possibly a solution but with unknown error. Just return the last EPA state. 
				// Fall through...
			case EEPAResult::Ok:
				// EPA has a solution - return it now
				OutNormalA = MTD;
				OutNormalB = BToATM.InverseTransformVectorNoScale(MTD);
				OutPenetration = Penetration + ThicknessA + ThicknessB;
				OutClosestA = ClosestA + MTD * ThicknessA;
				OutClosestB = BToATM.InverseTransformPositionNoScale(ClosestBInA - MTD * ThicknessB);
				OutMaxSupportDelta = MaxSupportDelta;
				return true;
			case EEPAResult::BadInitialSimplex:
				// The origin is outside the simplex. Must be a touching contact and EPA setup will have calculated the normal
				// and penetration but we keep the position generated by GJK.
				Normal = MTD;
				Distance = -Penetration;
				break;
			case EEPAResult::Degenerate:
				// We hit a degenerate simplex condition and could not reach a solution so use whatever near touching point GJK came up with.
				// The result from EPA under these circumstances is not usable.
				//UE_LOG(LogChaos, Warning, TEXT("EPA Degenerate Case"));
				break;
			}
		}

		// @todo(chaos): handle the case where EPA hits a degenerate triangle in the simplex.
		// Currently we return a touching contact with the last position and normal from GJK. We could run SAT instead.

		// GJK converged or we have a touching contact
		{
			TVec3<T> ClosestA(0);
			TVec3<T> ClosestB(0);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				ClosestA += InOutSimplexData.As[i] * InOutSimplexData.Barycentric[i];
				ClosestB += InOutSimplexData.Bs[i] * InOutSimplexData.Barycentric[i];
			}

			OutNormalA = Normal;
			OutNormalB = BToATM.InverseTransformVectorNoScale(Normal);

			T Penetration = ThicknessA + ThicknessB - Distance;
			OutPenetration = Penetration;
			OutClosestA = ClosestA + OutNormalA * ThicknessA;
			OutClosestB = ClosestB - OutNormalB * ThicknessB;

			OutMaxSupportDelta = MaxSupportDelta;

			// @todo(chaos): we should pass back failure for the degenerate case so we can decide how to handle it externally.
			return true;
		}
	}

	// Same as GJKPenetrationWarmStartable but with an index-less algorithm
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetrationWarmStartable2(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormalA, TVec3<T>& OutNormalB, int32& OutVertexA, int32& OutVertexB, TGJKSimplexData<T>& InOutSimplexData, T& OutMaxSupportDelta, const T Epsilon = T(1.e-3), const T EPAEpsilon = T(1.e-2))
	{
		T SupportDeltaA = 0;
		T SupportDeltaB = 0;
		T MaxSupportDelta = 0;

		int32& VertexIndexA = OutVertexA;
		int32& VertexIndexB = OutVertexB;

		// Return the support vertex in A-local space for a vector V in A-local space
		auto SupportAFunc = [&A, &SupportDeltaA, &VertexIndexA](const TVec3<T>& V)
		{
			return A.SupportCore(V, A.GetMargin(), &SupportDeltaA, VertexIndexA);
		};

		const TRotation<T, 3> AToBRotation = BToATM.GetRotation().Inverse();

		// Return the support point on B, in B-local space, for a vector V in A-local space
		auto SupportBFunc = [&B, &AToBRotation, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
		{
			const TVec3<T> VInB = AToBRotation * V;
			return B.SupportCore(VInB, B.GetMargin(), &SupportDeltaB, VertexIndexB);
		};

		// V and Simplex are in A-local space
		TVec3<T> V = TVec3<T>(-1, 0, 0);
		TVec3<T> Simplex[4];
		int32 NumVerts = 0;
		T Distance = FLT_MAX;

		// If we have warm-start data, rebuild the simplex from the stored data
		InOutSimplexData.Restore2(BToATM, NumVerts, Simplex, V, Distance, Epsilon);

		TVec3<T> Normal = -V;					// Remember the last good normal (i.e. don't update it if separation goes less than Epsilon and we can no longer normalize)
		bool bIsResult = false;					// True if GJK cannot make any more progress
		bool bIsContact = false;				// True if shapes are within Epsilon or overlapping - GJK cannot provide a solution
		int32 NumIterations = 0;
		const T ThicknessA = A.GetMargin();
		const T ThicknessB = B.GetMargin();
		while (!bIsContact && !bIsResult)
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			const TVec3<T> NegV = -V;
			const TVec3<T> SupportA = SupportAFunc(NegV);
			const TVec3<T> SupportB = SupportBFunc(V);
			const TVec3<T> SupportBInA = BToATM.TransformPositionNoScale(SupportB);
			const TVec3<T> W = SupportA - SupportBInA;
			MaxSupportDelta = FMath::Max(SupportDeltaA, SupportDeltaB);

			// If we didn't move to at least ConvergedDistance or closer, assume we have reached a minimum
			const T ConvergenceTolerance = 1.e-4f;
			const T ConvergedDistance = (1.0f - ConvergenceTolerance) * Distance;
			const T VW = TVec3<T>::DotProduct(V, W);

			InOutSimplexData.As[NumVerts] = SupportA;
			InOutSimplexData.Bs[NumVerts] = SupportB;
			Simplex[NumVerts++] = W;

			V = SimplexFindClosestToOrigin2(Simplex, NumVerts, InOutSimplexData.Barycentric, InOutSimplexData.As, InOutSimplexData.Bs);
			T NewDistance = V.Size();

			// Are we overlapping or too close for GJK to get a good result?
			bIsContact = (NewDistance < Epsilon);

			// If we did not get closer in this iteration, we are in a degenerate situation or we have the result
			bIsResult = (NewDistance >= (Distance - Epsilon));

			// If we are not too close, update the separating vector and keep iterating
			// If we are too close we drop through to EPA, but if EPA determines the shapes are actually separated 
			// after all, we will end up using the normal from the previous loop
			if (!bIsContact)
			{
				V /= NewDistance;
				Normal = -V;
			}
			Distance = NewDistance;
		}
		
		// Save the warm-start data (not much to store since we updated most of it in the loop above)
		InOutSimplexData.NumVerts = NumVerts;

		if (bIsContact)
		{
			// We did not converge or we detected an overlap situation, so run EPA to get contact data
			// Rebuild the simplex into a variable sized array - EPA can generate any number of vertices
			TArray<TVec3<T>> VertsA;
			TArray<TVec3<T>> VertsB;
			VertsA.Reserve(8);
			VertsB.Reserve(8);
			for (int i = 0; i < NumVerts; ++i)
			{
				VertsA.Add(InOutSimplexData.As[i]);
				VertsB.Add(BToATM.TransformPositionNoScale(InOutSimplexData.Bs[i]));
			}

			auto SupportBInAFunc = [&B, &BToATM, &AToBRotation, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
			{
				const TVec3<T> VInB = AToBRotation * V;
				const TVec3<T> SupportBLocal = B.SupportCore(VInB, B.GetMargin(), &SupportDeltaB, VertexIndexB);
				return BToATM.TransformPositionNoScale(SupportBLocal);
			};

			T Penetration;
			TVec3<T> MTD, ClosestA, ClosestBInA;
			const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBInAFunc, Penetration, MTD, ClosestA, ClosestBInA, EPAEpsilon);

			switch (EPAResult)
			{
			case EEPAResult::MaxIterations:
				// Possibly a solution but with unknown error. Just return the last EPA state. 
				// Fall through...
			case EEPAResult::Ok:
				// EPA has a solution - return it now
				OutNormalA = MTD;
				OutNormalB = BToATM.InverseTransformVectorNoScale(MTD);
				OutPenetration = Penetration + ThicknessA + ThicknessB;
				OutClosestA = ClosestA + MTD * ThicknessA;
				OutClosestB = BToATM.InverseTransformPositionNoScale(ClosestBInA - MTD * ThicknessB);
				OutMaxSupportDelta = MaxSupportDelta;
				return true;
			case EEPAResult::BadInitialSimplex:
				// The origin is outside the simplex. Must be a touching contact and EPA setup will have calculated the normal
				// and penetration but we keep the position generated by GJK.
				Normal = MTD;
				Distance = -Penetration;
				break;
			case EEPAResult::Degenerate:
				// We hit a degenerate simplex condition and could not reach a solution so use whatever near touching point GJK came up with.
				// The result from EPA under these circumstances is not usable.
				//UE_LOG(LogChaos, Warning, TEXT("EPA Degenerate Case"));
				break;
			}
		}

		// @todo(chaos): handle the case where EPA hits a degenerate triangle in the simplex.
		// Currently we return a touching contact with the last position and normal from GJK. We could run SAT instead.

		// GJK converged or we have a touching contact
		{
			TVec3<T> ClosestA(0);
			TVec3<T> ClosestB(0);
			for (int i = 0; i < NumVerts; ++i)
			{
				ClosestA += InOutSimplexData.As[i] * InOutSimplexData.Barycentric[i];
				ClosestB += InOutSimplexData.Bs[i] * InOutSimplexData.Barycentric[i];
			}

			OutNormalA = Normal;
			OutNormalB = BToATM.InverseTransformVectorNoScale(Normal);

			T Penetration = ThicknessA + ThicknessB - Distance;
			OutPenetration = Penetration;
			OutClosestA = ClosestA + OutNormalA * ThicknessA;
			OutClosestB = ClosestB - OutNormalB * ThicknessB;

			OutMaxSupportDelta = MaxSupportDelta;

			// @todo(chaos): we should pass back failure for the degenerate case so we can decide how to handle it externally.
			return true;
		}
	}

	/**
	 * @brief Calculate the penetration data for two shapes using GJK, assuming both shapes are already in the same space.
	 * This is intended for use with triangles which have been transformed into the space of the convex shape.
	 * 
	 * @tparam T
	 * @tparam TGeometryA First geometry type
	 * @tparam TGeometryB Second geometry type. Usually FTriangle
	 * @param A First geometry
	 * @param B Second geometry (usually triangle)
	 * @param OutPenetration penetration depth (negative for separation)
	 * @param OutClosestA Closest point on A
	 * @param OutClosestB Closest point on B
	 * @param OutNormal Contact normal (points from A to B)
	 * @param OutVertexA The closest vertex on A
	 * @param OutVertexB The closest vertex on B
	 * @param OutMaxSupportDelta When the convex has a margin, an upper bounds on the distance from the contact point to the vertex on the outer hull
	 * @param Epsilon GJK tolerance
	 * @param EPAEpsilon EPA tolerance
	 * @return true always
	*/
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetrationSameSpace(const TGeometryA& A, const TGeometryB& B, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormal, int32& OutVertexA, int32& OutVertexB, T& OutMaxSupportDelta, const T Epsilon = T(1.e-3), const T EPAEpsilon = T(1.e-2))
	{
		TGJKSimplexData<T> SimplexData;
		T SupportDeltaA = 0;
		T SupportDeltaB = 0;
		T MaxSupportDelta = 0;

		int32& VertexIndexA = OutVertexA;
		int32& VertexIndexB = OutVertexB;

		// Return the support vertex in A-local space for a vector V in A-local space
		auto SupportAFunc = [&A, &SupportDeltaA, &VertexIndexA](const TVec3<T>& V)
		{
			return A.SupportCore(V, A.GetMargin(), &SupportDeltaA, VertexIndexA);
		};

		// Return the support point on B, in B-local space, for a vector V in A-local space
		auto SupportBFunc = [&B, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
		{
			return B.SupportCore(V, B.GetMargin(), &SupportDeltaB, VertexIndexB);
		};

		// V and Simplex are in A-local space
		TVec3<T> V = TVec3<T>(-1, 0, 0);
		TVec3<T> Simplex[4];
		FSimplex SimplexIDs;
		T Distance = FLT_MAX;

		TVec3<T> Normal = -V;					// Remember the last good normal (i.e. don't update it if separation goes less than Epsilon and we can no longer normalize)
		bool bIsDegenerate = false;				// True if GJK cannot make any more progress
		bool bIsContact = false;				// True if shapes are within Epsilon or overlapping - GJK cannot provide a solution
		int32 NumIterations = 0;
		const T ThicknessA = A.GetMargin();
		const T ThicknessB = B.GetMargin();
		const T SeparatedDistance = ThicknessA + ThicknessB + Epsilon;
		while (!bIsContact && !bIsDegenerate)
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			const TVec3<T> NegV = -V;
			const TVec3<T> SupportA = SupportAFunc(NegV);
			const TVec3<T> SupportB = SupportBFunc(V);
			const TVec3<T> W = SupportA - SupportB;
			MaxSupportDelta = FMath::Max(SupportDeltaA, SupportDeltaB);

			// If we didn't move to at least ConvergedDistance or closer, assume we have reached a minimum
			const T ConvergenceTolerance = 1.e-4f;
			const T ConvergedDistance = (1.0f - ConvergenceTolerance) * Distance;
			const T VW = TVec3<T>::DotProduct(V, W);

			SimplexData.As[SimplexIDs.NumVerts] = SupportA;
			SimplexData.Bs[SimplexIDs.NumVerts] = SupportB;
			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			Simplex[SimplexIDs.NumVerts++] = W;

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, SimplexData.Barycentric, SimplexData.As, SimplexData.Bs);
			T NewDistance = V.Size();

			// Are we overlapping or too close for GJK to get a good result?
			bIsContact = (NewDistance < Epsilon);

			// If we did not get closer in this iteration, we are in a degenerate situation
			bIsDegenerate = (NewDistance >= Distance);

			// If we are not too close, update the separating vector and keep iterating
			// If we are too close we drop through to EPA, but if EPA determines the shapes are actually separated 
			// after all, we will wend up using the normal from the previous loop
			if (!bIsContact)
			{
				V /= NewDistance;
				Normal = -V;
			}
			Distance = NewDistance;
		}

		// Save the warm-start data (not much to store since we updated most of it in the loop above)
		SimplexData.Save(SimplexIDs);

		if (bIsContact)
		{
			// We did not converge or we detected an overlap situation, so run EPA to get contact data
			// Rebuild the simplex into a variable sized array - EPA can generate any mumber of vertices
			TArray<TVec3<T>> VertsA;
			TArray<TVec3<T>> VertsB;
			VertsA.Reserve(8);
			VertsB.Reserve(8);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				VertsA.Add(SimplexData.As[i]);
				VertsB.Add(SimplexData.Bs[i]);
			}

			T Penetration;
			TVec3<T> MTD, ClosestA, ClosestB;
			const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBFunc, Penetration, MTD, ClosestA, ClosestB, EPAEpsilon);

			switch (EPAResult)
			{
			case EEPAResult::MaxIterations:
				// Possibly a solution but with unknown error. Just return the last EPA state. 
				// Fall through...
			case EEPAResult::Ok:
				// EPA has a solution - return it now
				OutNormal = MTD;
				OutPenetration = Penetration + ThicknessA + ThicknessB;
				OutClosestA = ClosestA + MTD * ThicknessA;
				OutClosestB = ClosestB - MTD * ThicknessB;
				OutMaxSupportDelta = MaxSupportDelta;
				return true;
			case EEPAResult::BadInitialSimplex:
				// The origin is outside the simplex. Must be a touching contact and EPA setup will have calculated the normal
				// and penetration but we keep the position generated by GJK.
				Normal = MTD;
				Distance = -Penetration;
				//UE_LOG(LogChaos, Warning, TEXT("EPA Touching Case"));
				break;
			case EEPAResult::Degenerate:
				// We hit a degenerate simplex condition and could not reach a solution so use whatever near touching point GJK came up with.
				// The result from EPA under these circumstances is not usable.
				//UE_LOG(LogChaos, Warning, TEXT("EPA Degenerate Case"));
				break;
			}
		}

		// @todo(chaos): handle the case where EPA hits a degenerate triangle in the simplex.
		// Currently we return a touching contact with the last position and normal from GJK. We could run SAT instead.

		// GJK converged or we have a touching contact
		{
			TVec3<T> ClosestA(0);
			TVec3<T> ClosestB(0);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				ClosestA += SimplexData.As[i] * SimplexData.Barycentric[i];
				ClosestB += SimplexData.Bs[i] * SimplexData.Barycentric[i];
			}

			OutPenetration = ThicknessA + ThicknessB - Distance;
			OutClosestA = ClosestA + Normal * ThicknessA;
			OutClosestB = ClosestB - Normal * ThicknessB;
			OutNormal = Normal;
			OutMaxSupportDelta = MaxSupportDelta;

			// @todo(chaos): we should pass back failure for the degenerate case so we can decide how to handle it externally.
			return true;
		}
	}

	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetrationSameSpace2(const TGeometryA& A, const TGeometryB& B, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormal, int32& OutVertexA, int32& OutVertexB, T& OutMaxSupportDelta, const T Epsilon = T(1.e-3), const T EPAEpsilon = T(1.e-2))
	{
		TGJKSimplexData<T> SimplexData;
		T SupportDeltaA = 0;
		T SupportDeltaB = 0;
		T MaxSupportDelta = 0;

		int32& VertexIndexA = OutVertexA;
		int32& VertexIndexB = OutVertexB;

		// Return the support vertex in A-local space for a vector V in A-local space
		auto SupportAFunc = [&A, &SupportDeltaA, &VertexIndexA](const TVec3<T>& V)
		{
			return A.SupportCore(V, A.GetMargin(), &SupportDeltaA, VertexIndexA);
		};

		// Return the support point on B, in B-local space, for a vector V in A-local space
		auto SupportBFunc = [&B, &SupportDeltaB, &VertexIndexB](const TVec3<T>& V)
		{
			return B.SupportCore(V, B.GetMargin(), &SupportDeltaB, VertexIndexB);
		};

		// V and Simplex are in A-local space
		TVec3<T> V = TVec3<T>(-1, 0, 0);
		TVec3<T> Simplex[4];
		int32 NumVerts = 0;
		T Distance = FLT_MAX;

		TVec3<T> Normal = -V;					// Remember the last good normal (i.e. don't update it if separation goes less than Epsilon and we can no longer normalize)
		bool bIsResult = false;				// True if GJK cannot make any more progress
		bool bIsContact = false;				// True if shapes are within Epsilon or overlapping - GJK cannot provide a solution
		int32 NumIterations = 0;
		const T ThicknessA = A.GetMargin();
		const T ThicknessB = B.GetMargin();
		const T SeparatedDistance = ThicknessA + ThicknessB + Epsilon;
		while (!bIsContact && !bIsResult)
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			const TVec3<T> NegV = -V;
			const TVec3<T> SupportA = SupportAFunc(NegV);
			const TVec3<T> SupportB = SupportBFunc(V);
			const TVec3<T> W = SupportA - SupportB;
			MaxSupportDelta = FMath::Max(SupportDeltaA, SupportDeltaB);

			// If we didn't move to at least ConvergedDistance or closer, assume we have reached a minimum
			const T ConvergenceTolerance = 1.e-4f;
			const T ConvergedDistance = (1.0f - ConvergenceTolerance) * Distance;
			const T VW = TVec3<T>::DotProduct(V, W);

			SimplexData.As[NumVerts] = SupportA;
			SimplexData.Bs[NumVerts] = SupportB;
			Simplex[NumVerts++] = W;

			V = SimplexFindClosestToOrigin2(Simplex, NumVerts, SimplexData.Barycentric, SimplexData.As, SimplexData.Bs);
			T NewDistance = V.Size();

			// Are we overlapping or too close for GJK to get a good result?
			bIsContact = (NewDistance < Epsilon);

			// If we did not get closer in this iteration, we are in a degenerate situation
			bIsResult = (NewDistance >= (Distance - Epsilon));

			// If we are not too close, update the separating vector and keep iterating
			// If we are too close we drop through to EPA, but if EPA determines the shapes are actually separated 
			// after all, we will wend up using the normal from the previous loop
			if (!bIsContact)
			{
				V /= NewDistance;
				Normal = -V;
			}
			Distance = NewDistance;
		}

		// Save the warm-start data (not much to store since we updated most of it in the loop above)
		SimplexData.NumVerts = NumVerts;

		if (bIsContact)
		{
			// We did not converge or we detected an overlap situation, so run EPA to get contact data
			// Rebuild the simplex into a variable sized array - EPA can generate any mumber of vertices
			TArray<TVec3<T>> VertsA;
			TArray<TVec3<T>> VertsB;
			VertsA.Reserve(8);
			VertsB.Reserve(8);
			for (int i = 0; i < NumVerts; ++i)
			{
				VertsA.Add(SimplexData.As[i]);
				VertsB.Add(SimplexData.Bs[i]);
			}

			T Penetration;
			TVec3<T> MTD, ClosestA, ClosestB;
			const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBFunc, Penetration, MTD, ClosestA, ClosestB, EPAEpsilon);

			switch (EPAResult)
			{
			case EEPAResult::MaxIterations:
				// Possibly a solution but with unknown error. Just return the last EPA state. 
				// Fall through...
			case EEPAResult::Ok:
				// EPA has a solution - return it now
				OutNormal = MTD;
				OutPenetration = Penetration + ThicknessA + ThicknessB;
				OutClosestA = ClosestA + MTD * ThicknessA;
				OutClosestB = ClosestB - MTD * ThicknessB;
				OutMaxSupportDelta = MaxSupportDelta;
				return true;
			case EEPAResult::BadInitialSimplex:
				// The origin is outside the simplex. Must be a touching contact and EPA setup will have calculated the normal
				// and penetration but we keep the position generated by GJK.
				Normal = MTD;
				Distance = -Penetration;
				//UE_LOG(LogChaos, Warning, TEXT("EPA Touching Case"));
				break;
			case EEPAResult::Degenerate:
				// We hit a degenerate simplex condition and could not reach a solution so use whatever near touching point GJK came up with.
				// The result from EPA under these circumstances is not usable.
				//UE_LOG(LogChaos, Warning, TEXT("EPA Degenerate Case"));
				break;
			}
		}

		// @todo(chaos): handle the case where EPA hits a degenerate triangle in the simplex.
		// Currently we return a touching contact with the last position and normal from GJK. We could run SAT instead.

		// GJK converged or we have a touching contact
		{
			TVec3<T> ClosestA(0);
			TVec3<T> ClosestB(0);
			for (int i = 0; i < NumVerts; ++i)
			{
				ClosestA += SimplexData.As[i] * SimplexData.Barycentric[i];
				ClosestB += SimplexData.Bs[i] * SimplexData.Barycentric[i];
			}

			OutPenetration = ThicknessA + ThicknessB - Distance;
			OutClosestA = ClosestA + Normal * ThicknessA;
			OutClosestB = ClosestB - Normal * ThicknessB;
			OutNormal = Normal;
			OutMaxSupportDelta = MaxSupportDelta;

			// @todo(chaos): we should pass back failure for the degenerate case so we can decide how to handle it externally.
			return true;
		}
	}


	template <typename TGeometryA, typename TGeometryB, bool bNegativePenetrationAllowed = false, typename T>
	bool GJKPenetrationImpl(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormal, int32& OutClosestVertexIndexA, int32& OutClosestVertexIndexB, const T InThicknessA = 0.0f, const T InThicknessB = 0.0f, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T Epsilon = 1.e-3f)
	{
		int32 VertexIndexA = INDEX_NONE;
		int32 VertexIndexB = INDEX_NONE;

		auto SupportAFunc = [&A, &VertexIndexA](const TVec3<T>& V)
		{
			return A.SupportCore(V, A.GetMargin(), nullptr, VertexIndexA);
		};

		const TRotation<T, 3> AToBRotation = BToATM.GetRotation().Inverse();

		auto SupportBFunc = [&B, &BToATM, &AToBRotation, &VertexIndexB](const TVec3<T>& V)
		{
			const TVector<T, 3> VInB = AToBRotation * V;
			const TVector<T, 3> SupportBLocal = B.SupportCore(VInB, B.GetMargin(), nullptr, VertexIndexB);
			return BToATM.TransformPositionNoScale(SupportBLocal);
		};

		//todo: refactor all of these similar functions
		TVector<T, 3> V = -InitialDir;
		if (V.SafeNormalize() == 0)
		{
			V = TVec3<T>(-1, 0, 0);
		}

		TVec3<T> As[4];
		TVec3<T> Bs[4];

		FSimplex SimplexIDs;
		TVector<T, 3> Simplex[4];
		T Barycentric[4] = { -1,-1,-1,-1 };		// Initialization not needed, but compiler warns
		TVec3<T> Normal = -V;					// Remember the last good normal (i.e. don't update it if separation goes less than Epsilon and we can no longer normalize)
		bool bIsDegenerate = false;				// True if GJK cannot make any more progress
		bool bIsContact = false;				// True if shapes are within Epsilon or overlapping - GJK cannot provide a solution
		int NumIterations = 0;
		T Distance = FLT_MAX;
		const T ThicknessA = InThicknessA + A.GetMargin();
		const T ThicknessB = InThicknessB + B.GetMargin();
		const T SeparatedDistance = ThicknessA + ThicknessB + Epsilon;
		while (!bIsContact && !bIsDegenerate)
		{
			if (!ensure(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}
			const TVector<T, 3> NegV = -V;
			const TVector<T, 3> SupportA = SupportAFunc(NegV);
			const TVector<T, 3> SupportB = SupportBFunc(V);
			const TVector<T, 3> W = SupportA - SupportB;

			const T VW = TVector<T, 3>::DotProduct(V, W);
			if (!bNegativePenetrationAllowed && (VW > SeparatedDistance))
			{
				// We are separated and don't care about the distance - we can stop now
				return false;
			}

			// If we didn't move to at least ConvergedDistance or closer, assume we have reached a minimum
			const T ConvergenceTolerance = 1.e-4f;
			const T ConvergedDistance = (1.0f - ConvergenceTolerance) * Distance;
			if (VW > ConvergedDistance)
			{
				// We have reached a solution - use the results from the last iteration
				break;
			}

			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			As[SimplexIDs.NumVerts] = SupportA;
			Bs[SimplexIDs.NumVerts] = SupportB;
			Simplex[SimplexIDs.NumVerts++] = W;

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric, As, Bs);
			T NewDistance = V.Size();

			// Are we overlapping or too close for GJK to get a good result?
			bIsContact = (NewDistance < Epsilon);

			// If we did not get closer in this iteration, we are in a degenerate situation
			bIsDegenerate = (NewDistance >= Distance);

			if (!bIsContact)
			{
				V /= NewDistance;
				Normal = -V;
			}
			Distance = NewDistance;
		}

		if (bIsContact)
		{
			// We did not converge or we detected an overlap situation, so run EPA to get contact data
			TArray<TVec3<T>> VertsA;
			TArray<TVec3<T>> VertsB;
			VertsA.Reserve(8);
			VertsB.Reserve(8);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				VertsA.Add(As[i]);
				VertsB.Add(Bs[i]);
			}

			T Penetration;
			TVec3<T> MTD, ClosestA, ClosestBInA;
			const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBFunc, Penetration, MTD, ClosestA, ClosestBInA);
			
			switch (EPAResult)
			{
			case EEPAResult::MaxIterations:
				// Possibly a solution but with unknown error. Just return the last EPA state. 
				// Fall through...
			case EEPAResult::Ok:
				// EPA has a solution - return it now
				OutNormal = MTD;
				OutPenetration = Penetration + ThicknessA + ThicknessB;
				OutClosestA = ClosestA + MTD * ThicknessA;
				OutClosestB = ClosestBInA - MTD * ThicknessB;
				OutClosestVertexIndexA = VertexIndexA;
				OutClosestVertexIndexB = VertexIndexB;
				return true;
			case EEPAResult::BadInitialSimplex:
				// The origin is outside the simplex. Must be a touching contact and EPA setup will have calculated the normal
				// and penetration but we keep the position generated by GJK.
				Normal = MTD;
				Distance = -Penetration;
				break;
			case EEPAResult::Degenerate:
				// We hit a degenerate simplex condition and could not reach a solution so use whatever near touching point GJK came up with.
				// The result from EPA under these circumstances is not usable.
				//UE_LOG(LogChaos, Warning, TEXT("EPA Degenerate Case"));
				break;
			}
		}

		// @todo(chaos): handle the case where EPA hit a degenerate triangle in the simplex.
		// Currently we return a touching contact with the last position and normal from GJK. We could run SAT instead.

		// GJK converged or we have a touching contact
		{
			TVector<T, 3> ClosestA(0);
			TVector<T, 3> ClosestBInA(0);
			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				ClosestA += As[i] * Barycentric[i];
				ClosestBInA += Bs[i] * Barycentric[i];
			}

			OutNormal = Normal;

			T Penetration = ThicknessA + ThicknessB - Distance;
			OutPenetration = Penetration;
			OutClosestA = ClosestA + Normal * ThicknessA;
			OutClosestB = ClosestBInA - Normal * ThicknessB;
			OutClosestVertexIndexA = VertexIndexA;
			OutClosestVertexIndexB = VertexIndexB;

			// If we don't care about separation distance/normal, the return value is true if we are overlapping, false otherwise.
			// If we do care about seperation distance/normal, the return value is true if we found a solution.
			// @todo(chaos): we should pass back failure for the degenerate case so we can decide how to handle it externally.
			return (bNegativePenetrationAllowed || (Penetration >= 0.0f));
		}
	}

	// Calculate the penetration depth (or separating distance) of two geometries.
	//
	// Set bNegativePenetrationAllowed to false (default) if you do not care about the normal and distance when the shapes are separated. The return value
	// will be false if the shapes are separated, and the function will be faster because it does not need to determine the closest point.
	// If the shapes are overlapping, the function will return true and populate the output parameters with the contact information.
	//
	// Set bNegativePenetrationAllowed to true if you need to know the closest point on the shapes, even when they are separated. In this case,
	// we need to iterate to find the best solution even when objects are separated which is more expensive. The return value will be true as long 
	// as the algorithm was able to find a solution (i.e., the return value is not related to whether the shapes are overlapping) and the output 
	// parameters will be populated with the contact information.
	//
	// In all cases, if the function returns false the output parameters are undefined.
	//
	// OutClosestA and OutClosestB are the closest or deepest-penetrating points on the two core geometries, both in the space of A and ignoring the margin.
	//
	// Epsilon is the separation at which GJK considers the objects to be in contact or penetrating and then runs EPA. If this is
	// too small, then the renormalization of the separating vector can lead to arbitrarily wrong normals for almost-touching objects.
	//
	// NOTE: OutPenetration is the penetration including the Thickness (i.e., the actual penetration depth), but the closest points
	// returned are on the core shapes (i.e., ignoring the Thickness). If you want the closest positions on the shape surface (including
	// the Thickness) use GJKPenetration().
	//
	template <bool bNegativePenetrationAllowed = false, typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetration(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, T& OutPenetration, TVec3<T>& OutClosestA, TVec3<T>& OutClosestB, TVec3<T>& OutNormal, int32& OutClosestVertexIndexA, int32& OutClosestVertexIndexB, const T InThicknessA = 0.0f, const T InThicknessB = 0.0f, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T Epsilon = 1.e-3f)
	{
		return GJKPenetrationImpl<TGeometryA, TGeometryB, bNegativePenetrationAllowed, T>(A, B, BToATM, OutPenetration, OutClosestA, OutClosestB, OutNormal, OutClosestVertexIndexA, OutClosestVertexIndexB, InThicknessA, InThicknessB, InitialDir, Epsilon);
	}
	/** Sweeps one geometry against the other
	 @A The first geometry
	 @B The second geometry
	 @StartTM B's starting configuration in A's local space
	 @RayDir The ray's direction (normalized)
	 @RayLength The ray's length
	 @OutTime The time along the ray when the objects first overlap
	 @OutPosition The first point of impact (in A's local space) when the objects first overlap. Invalid if time of impact is 0
	 @OutNormal The impact normal (in A's local space) when the objects first overlap. Invalid if time of impact is 0
	 @ThicknessA The amount of geometry inflation for Geometry A (for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
	 @InitialDir The first direction we use to search the CSO
	 @ThicknessB The amount of geometry inflation for Geometry B (for example if the surface distance of two geometries with thickness 0 would be 2, a thickness of 0.5 would give a distance of 1.5)
	 @return True if the geometries overlap during the sweep, False otherwise 
	 @note If A overlaps B at the start of the ray ("initial overlap" condition) then this function returns true, and sets OutTime = 0, but does not set any other output variables.
	 */

	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKRaycast(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& StartTM, const TVector<T, 3>& RayDir, const T RayLength,
		T& OutTime, TVector<T, 3>& OutPosition, TVector<T, 3>& OutNormal, const T ThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T ThicknessB = 0)
	{
		ensure(FMath::IsNearlyEqual(RayDir.SizeSquared(), (FReal)1, (FReal)KINDA_SMALL_NUMBER));
		ensure(RayLength > 0);
		check(A.IsConvex() && B.IsConvex());
		const TVector<T, 3> StartPoint = StartTM.GetLocation();
		int32 VertexIndexA = INDEX_NONE;
		int32 VertexIndexB = INDEX_NONE;

		TVector<T, 3> Simplex[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };
		TVector<T, 3> As[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };
		TVector<T, 3> Bs[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };

		T Barycentric[4] = { -1,-1,-1,-1 };	//not needed, but compiler warns

		FSimplex SimplexIDs;
		const TRotation<T, 3> BToARotation = StartTM.GetRotation();
		const TRotation<T, 3> AToBRotation = BToARotation.Inverse();
		TVector<T, 3> SupportA = A.Support(InitialDir, ThicknessA, VertexIndexA);	//todo: use Thickness on quadratic geometry
		As[0] = SupportA;

		const TVector<T, 3> InitialDirInB = AToBRotation * (-InitialDir);
		const TVector<T, 3> InitialSupportBLocal = B.Support(InitialDirInB, ThicknessB, VertexIndexB);
		TVector<T, 3> SupportB = BToARotation * InitialSupportBLocal;
		Bs[0] = SupportB;

		T Lambda = 0;
		TVector<T, 3> X = StartPoint;
		TVector<T, 3> Normal(0);
		TVector<T, 3> V = X - (SupportA - SupportB);

		bool bTerminate;
		bool bNearZero = false;
		bool bDegenerate = false;
		int NumIterations = 0;
		T GJKPreDist2 = TNumericLimits<T>::Max();
		do
		{
			//if (!ensure(NumIterations++ < 32))	//todo: take this out
			if (!(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			SupportA = A.Support(V, ThicknessA, VertexIndexA);	//todo: add thickness to quadratic geometry to avoid quadratic vs quadratic when possible
			const TVector<T, 3> VInB = AToBRotation * (-V);
			const TVector<T, 3> SupportBLocal = B.Support(VInB, ThicknessB, VertexIndexB);
			SupportB = BToARotation * SupportBLocal;
			const TVector<T, 3> P = SupportA - SupportB;
			const TVector<T, 3> W = X - P;
			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;	//is this needed?
			As[SimplexIDs.NumVerts] = SupportA;
			Bs[SimplexIDs.NumVerts] = SupportB;

			const T VDotW = TVector<T, 3>::DotProduct(V, W);
			if (VDotW > 0)
			{
				const T VDotRayDir = TVector<T, 3>::DotProduct(V, RayDir);
				if (VDotRayDir >= 0)
				{
					return false;
				}

				const T PreLambda = Lambda;	//use to check for no progress
				// @todo(ccaulfield): this can still overflow - the comparisons against zero above should be changed (though not sure to what yet)
				Lambda = Lambda - VDotW / VDotRayDir;
				if (Lambda > PreLambda)
				{
					if (Lambda > RayLength)
					{
						return false;
					}

					const TVector<T, 3> OldX = X;
					X = StartPoint + Lambda * RayDir;
					Normal = V;

					//Update simplex from (OldX - P) to (X - P)
					const TVector<T, 3> XMinusOldX = X - OldX;
					Simplex[0] += XMinusOldX;
					Simplex[1] += XMinusOldX;
					Simplex[2] += XMinusOldX;
					Simplex[SimplexIDs.NumVerts++] = X - P;

					GJKPreDist2 = TNumericLimits<T>::Max();	//translated origin so restart gjk search
				}
			}
			else
			{
				Simplex[SimplexIDs.NumVerts++] = W;	//this is really X - P which is what we need for simplex computation
			}

			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric, As, Bs);

			T NewDist2 = V.SizeSquared();	//todo: relative error
			bNearZero = NewDist2 < 1e-6;
			bDegenerate = NewDist2 >= GJKPreDist2;
			GJKPreDist2 = NewDist2;
			bTerminate = bNearZero || bDegenerate;

		} while (!bTerminate);

		OutTime = Lambda;

		if (Lambda > 0)
		{
			OutNormal = Normal.GetUnsafeNormal();
			TVector<T, 3> ClosestA(0);
			TVector<T, 3> ClosestB(0);

			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				ClosestB += Bs[i] * Barycentric[i];
			}
			const TVector<T, 3> ClosestLocal = ClosestB;

			OutPosition = StartPoint + RayDir * Lambda + ClosestLocal;
		}

		return true;
	}
	
	template <typename TGeometryA, typename TGeometryB, typename T>
	bool GJKRaycast2Impl(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& StartTM, const TVector<T, 3>& RayDir, const T RayLength,
		T& OutTime, TVector<T, 3>& OutPosition, TVector<T, 3>& OutNormal, const T GivenThicknessA = 0, bool bComputeMTD = false, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T GivenThicknessB = 0)
	{
		ensure(FMath::IsNearlyEqual(RayDir.SizeSquared(), (T)1, (T)KINDA_SMALL_NUMBER));
		ensure(RayLength > 0);

		T MarginA;
		T MarginB;
		CalculateQueryMargins(A,B, MarginA, MarginB);

		const TVector<T, 3> StartPoint = StartTM.GetLocation();

		TVector<T, 3> Simplex[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };
		TVector<T, 3> As[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };
		TVector<T, 3> Bs[4] = { TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0), TVector<T,3>(0) };

		T Barycentric[4] = { -1,-1,-1,-1 };	//not needed, but compiler warns
		const T Inflation = MarginA + MarginB;
		const T Inflation2 = Inflation * Inflation + static_cast<T>(1e-6);

		FSimplex SimplexIDs;
		const TRotation<T, 3> BToARotation = StartTM.GetRotation();
		const TRotation<T, 3> AToBRotation = BToARotation.Inverse();

		auto SupportAFunc = [&A, MarginA](const TVec3<T>& V)
		{
			int32 VertexIndex = INDEX_NONE;
			return A.SupportCore(V, MarginA, nullptr, VertexIndex);
		};

		auto SupportBFunc = [&B, MarginB, &AToBRotation, &BToARotation](const TVec3<T>& V)
		{
			int32 VertexIndex = INDEX_NONE;
			const TVector<T, 3> VInB = AToBRotation * V;
			const TVector<T, 3> SupportBLocal = B.SupportCore(VInB, MarginB, nullptr, VertexIndex);
			return BToARotation * SupportBLocal;
		};
		
		auto SupportBAtOriginFunc = [&B, MarginB, &StartTM, &AToBRotation](const TVec3<T>& Dir)
		{
			int32 VertexIndex = INDEX_NONE;
			const TVector<T, 3> DirInB = AToBRotation * Dir;
			const TVector<T, 3> SupportBLocal = B.SupportCore(DirInB, MarginB, nullptr, VertexIndex);
			return StartTM.TransformPositionNoScale(SupportBLocal);
		};

		TVector<T, 3> SupportA = SupportAFunc(InitialDir);
		As[0] = SupportA;

		TVector<T, 3> SupportB = SupportBFunc(-InitialDir);
		Bs[0] = SupportB;

		T Lambda = 0;
		TVector<T, 3> X = StartPoint;
		TVector<T, 3> V = X - (SupportA - SupportB);
		TVector<T, 3> Normal(0,0,1);

		const T InitialPreDist2 = V.SizeSquared();
		constexpr T Eps2 = 1e-6f;
		//mtd needs to find closest point even in inflation region, so can only skip if we found the closest points
		bool bCloseEnough = InitialPreDist2 < Inflation2 && (!bComputeMTD || InitialPreDist2 < Eps2);
		bool bDegenerate = false;
		bool bTerminate = bCloseEnough;
		bool bInflatedCloseEnough = bCloseEnough;
		int NumIterations = 0;
		T GJKPreDist2 = TNumericLimits<T>::Max();
		while (!bTerminate)
		{
			//if (!ensure(NumIterations++ < 32))	//todo: take this out
			if (!(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			V = V.GetUnsafeNormal();

			SupportA = SupportAFunc(V);
			SupportB = SupportBFunc(-V);
			const TVector<T, 3> P = SupportA - SupportB;
			const TVector<T, 3> W = X - P;
			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;	//is this needed?
			As[SimplexIDs.NumVerts] = SupportA;
			Bs[SimplexIDs.NumVerts] = SupportB;

			const T VDotW = TVector<T, 3>::DotProduct(V, W);

			if (VDotW > Inflation)
			{
				const T VDotRayDir = TVector<T, 3>::DotProduct(V, RayDir);
				if (VDotRayDir >= 0)
				{
					return false;
				}

				const T PreLambda = Lambda;	//use to check for no progress
				// @todo(ccaulfield): this can still overflow - the comparisons against zero above should be changed (though not sure to what yet)
				Lambda = Lambda - (VDotW - Inflation) / VDotRayDir;
				if (Lambda > PreLambda)
				{
					if (Lambda > RayLength)
					{
						return false;
					}

					const TVector<T, 3> OldX = X;
					X = StartPoint + Lambda * RayDir;
					Normal = V;

					//Update simplex from (OldX - P) to (X - P)
					const TVector<T, 3> XMinusOldX = X - OldX;
					Simplex[0] += XMinusOldX;
					Simplex[1] += XMinusOldX;
					Simplex[2] += XMinusOldX;
					Simplex[SimplexIDs.NumVerts++] = X - P;

					GJKPreDist2 = TNumericLimits<T>::Max();	//translated origin so restart gjk search
					bInflatedCloseEnough = false;
				}
			}
			else
			{
				Simplex[SimplexIDs.NumVerts++] = W;	//this is really X - P which is what we need for simplex computation
			}

			if (bInflatedCloseEnough && VDotW >= 0)
			{
				//Inflated shapes are close enough, but we want MTD so we need to find closest point on core shape
				const T VDotW2 = VDotW * VDotW;
				bCloseEnough = GJKPreDist2 <= Eps2 + VDotW2;	//todo: relative error
			}

			if (!bCloseEnough)
			{
				V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric, As, Bs);
				T NewDist2 = V.SizeSquared();	//todo: relative error
				bCloseEnough = NewDist2 < Inflation2;
				bDegenerate = NewDist2 >= GJKPreDist2;
				GJKPreDist2 = NewDist2;


				if (bComputeMTD && bCloseEnough && Lambda == 0 && GJKPreDist2 > 1e-6 && Inflation2 > 1e-6 && SimplexIDs.NumVerts < 4)
				{
					//For mtd of inflated shapes we have to find the closest point, so we have to keep going
					bCloseEnough = false;
					bInflatedCloseEnough = true;
				}
			}
			else
			{
				//It must be that we want MTD and we can terminate. However, we must make one final call to fixup the simplex
				V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric, As, Bs);
			}
			bTerminate = bCloseEnough || bDegenerate;
		}

		OutTime = Lambda;

		if (Lambda > 0)
		{
			OutNormal = Normal;
			TVector<T, 3> ClosestB(0);

			for (int i = 0; i < SimplexIDs.NumVerts; ++i)
			{
				ClosestB += Bs[i] * Barycentric[i];
			}
			const TVector<T, 3> ClosestLocal = ClosestB - OutNormal * MarginB;

			OutPosition = StartPoint + RayDir * Lambda + ClosestLocal;
		}
		else if (bComputeMTD)
		{
			// If Inflation == 0 we would expect GJKPreDist2 to be 0
			// However, due to precision we can still end up with GJK failing.
			// When that happens fall back on EPA
			if (Inflation > 0 && GJKPreDist2 > 1e-6 && GJKPreDist2 < TNumericLimits<T>::Max())
			{
				OutNormal = Normal;
				TVector<T, 3> ClosestA(0);
				TVector<T, 3> ClosestB(0);

				if (NumIterations)
				{
					for (int i = 0; i < SimplexIDs.NumVerts; ++i)
					{
						ClosestA += As[i] * Barycentric[i];
						ClosestB += Bs[i] * Barycentric[i];
					}
				}
				else
				{
					//didn't even go into gjk loop
					ClosestA = As[0];
					ClosestB = Bs[0];
				}
				
				const TVec3<T> ClosestBInA = StartPoint + ClosestB;
				const T InGJKPreDist = FMath::Sqrt(GJKPreDist2);
				OutNormal = V.GetUnsafeNormal();

				const T Penetration = FMath::Clamp<T>(MarginA + MarginB - InGJKPreDist, 0, TNumericLimits<T>::Max());
				const TVector<T, 3> ClosestLocal = ClosestB - OutNormal * MarginB;

				OutPosition = StartPoint + ClosestLocal + OutNormal * Penetration;
				OutTime = -Penetration;
			}
			else
			{
				//use EPA
				TArray<TVec3<T>> VertsA;
				TArray<TVec3<T>> VertsB;

				VertsA.Reserve(8);
				VertsB.Reserve(8);

				if (NumIterations)
				{
					for (int i = 0; i < SimplexIDs.NumVerts; ++i)
					{
						VertsA.Add(As[i]);
						const TVec3<T> BAtOrigin = Bs[i] + X;
						VertsB.Add(BAtOrigin);
					}

					T Penetration;
					TVec3<T> MTD, ClosestA, ClosestBInA;
					const EEPAResult EPAResult = EPA(VertsA, VertsB, SupportAFunc, SupportBAtOriginFunc, Penetration, MTD, ClosestA, ClosestBInA);
					if (IsEPASuccess(EPAResult))
					{
						OutNormal = MTD;
						OutTime = -Penetration - Inflation;
						OutPosition = ClosestA;
					}
					//else if (IsEPADegenerate(EPAResult))
					//{
					//	// @todo(chaos): handle degenerate EPA condition
					//}
					else
					{
						//assume touching hit
						OutTime = -Inflation;
						OutNormal = MTD;
						OutPosition = As[0] + OutNormal * MarginA;
					}
				}
				else
				{
					//didn't even go into gjk loop, touching hit
					OutTime = -Inflation;
					OutNormal = { 0,0,1 };
					OutPosition = As[0] + OutNormal * MarginA;
				}
			}
		}
		else
		{
			// Initial overlap without MTD. These properties are not valid, but assigning them anyway so they don't contain NaNs and cause issues in invoking code.
			OutNormal = { 0,0,1 };
			OutPosition = { 0,0,0 };
		}

		return true;
	}

	template <typename TGeometryA, typename TGeometryB, typename T>
	bool GJKRaycast2ImplSimd(const TGeometryA& A, const TGeometryB& B, const VectorRegister4Float& BToARotation, const VectorRegister4Float& StartPoint, const VectorRegister4Float& RayDir, const T RayLength,
		T& OutTime, VectorRegister4Float& OutPosition, VectorRegister4Float& OutNormal, bool bComputeMTD, const VectorRegister4Float& InitialDir, const TRigidTransform<double, 3>& StartTM)
	{
		ensure(RayLength > 0);

		// Margin selection logic: we only need a small margin for sweeps since we only move the sweeping object
		// to the point where it just touches.
		// Spheres and Capsules: always use the core shape and full "margin" because it represents the radius
		// Sphere/Capsule versus OtherShape: no margin on other
		// OtherShape versus OtherShape: use margin of the smaller shape, zero margin on the other
		const T RadiusA = static_cast<T>(A.GetRadius());
		const T RadiusB = static_cast<T>(B.GetRadius());
		const bool bHasRadiusA = RadiusA > 0;
		const bool bHasRadiusB = RadiusB > 0;

		// The sweep margins if required. Only one can be non-zero (we keep the smaller one)
		const T SweepMarginScale = 0.05f;
		const bool bAIsSmallest = A.GetMargin() < B.GetMargin();
		const T SweepMarginA = (bHasRadiusA || bHasRadiusB) ? 0.0f : (bAIsSmallest ? SweepMarginScale * static_cast<T>(A.GetMargin()) : 0.0f);
		const T SweepMarginB = (bHasRadiusA || bHasRadiusB) ? 0.0f : (bAIsSmallest ? 0.0f : SweepMarginScale * static_cast<T>(B.GetMargin()));

		// Net margin (note: both SweepMargins are zero if either Radius is non-zero, and only one SweepMargin can be non-zero)
		const T MarginA = RadiusA + SweepMarginA;
		const T MarginB = RadiusB + SweepMarginB;

		const VectorRegister4Float MarginASimd = VectorLoadFloat1(&MarginA);
		const VectorRegister4Float MarginBSimd = VectorLoadFloat1(&MarginB);

		VectorRegister4Float Simplex[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };
		VectorRegister4Float As[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };
		VectorRegister4Float Bs[4] = { VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat(), VectorZeroFloat() };

		VectorRegister4Float Barycentric;

		VectorRegister4Float Inflation = VectorAdd(MarginASimd, MarginBSimd);
		constexpr VectorRegister4Float Eps2Simd = MakeVectorRegisterFloatConstant(1e-6f, 1e-6f, 1e-6f, 1e-6f);
		const VectorRegister4Float Inflation2Simd = VectorMultiplyAdd(Inflation, Inflation, Eps2Simd);

		const VectorRegister4Float RayLengthSimd = MakeVectorRegisterFloat(RayLength, RayLength, RayLength, RayLength);

		VectorRegister4Int NumVerts = GlobalVectorConstants::IntZero;

		VectorRegister4Float AToBRotation = VectorQuaternionInverse(BToARotation);

		auto SupportAFunc = [&A, MarginA](const VectorRegister4Float& V)
		{
			return A.SupportCoreSimd(V, MarginA);
		};

		auto SupportBFunc = [&B, MarginB, &AToBRotation, &BToARotation](const VectorRegister4Float& V)
		{
			const VectorRegister4Float VInB = VectorQuaternionRotateVector(AToBRotation, V);
			const VectorRegister4Float SupportBLocal = B.SupportCoreSimd(VInB, MarginB);

			return VectorQuaternionRotateVector(BToARotation, SupportBLocal);
		};


		VectorRegister4Float SupportA = SupportAFunc(InitialDir);
		As[0] = SupportA;

		VectorRegister4Float SupportB = SupportBFunc(VectorNegate(InitialDir));
		Bs[0] = SupportB;

		VectorRegister4Float Lambda = VectorZeroFloat();
		VectorRegister4Float X = StartPoint;
		VectorRegister4Float V = VectorSubtract(X, VectorSubtract(SupportA, SupportB));
		VectorRegister4Float Normal = MakeVectorRegisterFloat(0.f, 0.f, 1.f, 0.f);

		const VectorRegister4Float InitialPreDist2Simd = VectorDot3(V, V);

		FRealSingle InitialPreDist2;
		VectorStoreFloat1(InitialPreDist2Simd, &InitialPreDist2);

		FRealSingle Inflation2;
		VectorStoreFloat1(Inflation2Simd, &Inflation2);
		
		constexpr FRealSingle Eps2 = 1e-6f;

		//mtd needs to find closest point even in inflation region, so can only skip if we found the closest points
		bool bCloseEnough = InitialPreDist2 < Inflation2 && (!bComputeMTD || InitialPreDist2 < Eps2);
		bool bDegenerate = false;
		bool bTerminate = bCloseEnough;
		bool bInflatedCloseEnough = bCloseEnough;
		int NumIterations = 0;
		constexpr VectorRegister4Float LimitMax = MakeVectorRegisterFloatConstant(TNumericLimits<T>::Max(), TNumericLimits<T>::Max(), TNumericLimits<T>::Max(), TNumericLimits<T>::Max());
		VectorRegister4Float GJKPreDist2 = LimitMax;

		while (!bTerminate)
		{
			if (!(NumIterations++ < 32))	//todo: take this out
			{
				break;	//if taking too long just stop. This should never happen
			}

			V = VectorNormalizeAccurate(V);

			SupportA = SupportAFunc(V);
			SupportB = SupportBFunc(VectorNegate(V));
			const VectorRegister4Float P = VectorSubtract(SupportA, SupportB);
			const VectorRegister4Float W = VectorSubtract(X, P);

			// NumVerts store here at the beginning of the loop, it should be safe to reuse it further in the loop 
			alignas(16) int32 NumVertsInts[4];
			VectorIntStoreAligned(NumVerts, NumVertsInts);
			const int32 NumVertsInt = NumVertsInts[0];

			As[NumVertsInt] = SupportA;
			Bs[NumVertsInt] = SupportB;

			const VectorRegister4Float VDotW = VectorDot3(V, W);

			VectorRegister4Float VDotWGTInflationSimd = VectorCompareGT(VDotW, Inflation);

			if (VectorMaskBits(VDotWGTInflationSimd))
			{
				const VectorRegister4Float VDotRayDir = VectorDot3(V, RayDir);
				VectorRegister4Float VDotRayDirGEZero = VectorCompareGE(VDotRayDir, VectorZeroFloat());

				if (VectorMaskBits(VDotRayDirGEZero))
				{
					return false;
				}

				const VectorRegister4Float PreLambda = Lambda;	//use to check for no progress
				// @todo(ccaulfield): this can still overflow - the comparisons against zero above should be changed (though not sure to what yet)
				Lambda = VectorSubtract(Lambda, VectorDivide(VectorSubtract(VDotW, Inflation), VDotRayDir));
				VectorRegister4Float LambdaGTPreLambda = VectorCompareGT(Lambda, PreLambda);
				if (VectorMaskBits(LambdaGTPreLambda))
				{
					VectorRegister4Float LambdaGTRayLength = VectorCompareGT(Lambda, RayLengthSimd);
					if (VectorMaskBits(LambdaGTRayLength))
					{
						return false;
					}

					const VectorRegister4Float OldX = X;
					X = VectorMultiplyAdd(Lambda, RayDir, StartPoint);
					Normal = V;

					//Update simplex from (OldX - P) to (X - P)
					VectorRegister4Float XMinusOldX = VectorSubtract(X, OldX);
					Simplex[0] = VectorAdd(Simplex[0], XMinusOldX);
					Simplex[1] = VectorAdd(Simplex[1], XMinusOldX);
					Simplex[2] = VectorAdd(Simplex[2], XMinusOldX);
					Simplex[NumVertsInt] = VectorSubtract(X, P);
					NumVerts = VectorIntAdd(NumVerts, GlobalVectorConstants::IntOne);

					GJKPreDist2 = LimitMax; //translated origin so restart gjk search
					bInflatedCloseEnough = false;
				}
			}
			else
			{
				Simplex[NumVertsInt] = W;	//this is really X - P which is what we need for simplex computation
				NumVerts = VectorIntAdd(NumVerts, GlobalVectorConstants::IntOne);
			}

			if (bInflatedCloseEnough && VectorMaskBits(VectorCompareGE(VDotW, VectorZeroFloat())))
			{
				//Inflated shapes are close enough, but we want MTD so we need to find closest point on core shape
				const VectorRegister4Float VDotW2 = VectorDot3(VDotW, VDotW);
				bCloseEnough = static_cast<bool>(VectorMaskBits(VectorCompareGE(VectorAdd(Eps2Simd, VDotW), GJKPreDist2)));
			}

			if (!bCloseEnough)
			{
				V = VectorSimplexFindClosestToOrigin(Simplex, NumVerts, Barycentric, As, Bs);

				VectorRegister4Float NewDist2 = VectorDot3(V, V);	//todo: relative error
				bCloseEnough = static_cast<bool>(VectorMaskBits(VectorCompareGT(Inflation2Simd, NewDist2)));
				bDegenerate = static_cast<bool>(VectorMaskBits(VectorCompareGE(NewDist2, GJKPreDist2)));
				GJKPreDist2 = NewDist2;

				if (bComputeMTD && bCloseEnough)
				{
					const VectorRegister4Float LambdaEqZero = VectorCompareEQ(Lambda, VectorZeroFloat());
					const VectorRegister4Float InGJKPreDist2GTEps2 = VectorCompareGT(GJKPreDist2, Eps2Simd);
					const VectorRegister4Float Inflation22GTEps2 = VectorCompareGT(Inflation2Simd, Eps2Simd);
					constexpr VectorRegister4Int fourInt = MakeVectorRegisterIntConstant(4, 4, 4, 4);
					const VectorRegister4Int Is4GTNumVerts = VectorIntCompareGT(fourInt, NumVerts);

					const VectorRegister4Float IsInflatCloseEnough = VectorBitwiseAnd(LambdaEqZero, VectorBitwiseAnd(InGJKPreDist2GTEps2, VectorBitwiseAnd(Inflation22GTEps2, VectorCast4IntTo4Float(Is4GTNumVerts))));

					// Leaving the original code, to explain the logic there
					//if (bComputeMTD && bCloseEnough && Lambda == 0 && GJKPreDist2 > 1e-6 && Inflation2 > 1e-6 && NumVerts < 4)
					//{
					//	bCloseEnough = false;
					//	bInflatedCloseEnough = true;
					//}
					bInflatedCloseEnough = static_cast<bool>(VectorMaskBits(IsInflatCloseEnough));
					bCloseEnough = !bInflatedCloseEnough;
				}
			}
			else
			{
				//It must be that we want MTD and we can terminate. However, we must make one final call to fixup the simplex
				V = VectorSimplexFindClosestToOrigin(Simplex, NumVerts, Barycentric, As, Bs);
			}
			bTerminate = bCloseEnough || bDegenerate;
		}
		VectorStoreFloat1(Lambda, &OutTime);

		if (OutTime > 0)
		{
			OutNormal = Normal;
			VectorRegister4Float ClosestB = VectorZeroFloat();

			VectorRegister4Float Barycentrics[4];
			Barycentrics[0] = VectorSwizzle(Barycentric, 0, 0, 0, 0);
			Barycentrics[1] = VectorSwizzle(Barycentric, 1, 1, 1, 1);
			Barycentrics[2] = VectorSwizzle(Barycentric, 2, 2, 2, 2);
			Barycentrics[3] = VectorSwizzle(Barycentric, 3, 3, 3, 3);


			const VectorRegister4Float  ClosestB1 = VectorMultiplyAdd(Bs[0], Barycentrics[0], ClosestB);
			const VectorRegister4Float  ClosestB2 = VectorMultiplyAdd(Bs[1], Barycentrics[1], ClosestB1);
			const VectorRegister4Float  ClosestB3 = VectorMultiplyAdd(Bs[2], Barycentrics[2], ClosestB2);
			const VectorRegister4Float  ClosestB4 = VectorMultiplyAdd(Bs[3], Barycentrics[3], ClosestB3);

			constexpr VectorRegister4Int TwoInt = MakeVectorRegisterIntConstant(2, 2, 2, 2);
			constexpr VectorRegister4Int ThreeInt = MakeVectorRegisterIntConstant(3, 3, 3, 3);

			const VectorRegister4Float IsB0 = VectorCast4IntTo4Float(VectorIntCompareEQ(NumVerts, GlobalVectorConstants::IntZero));
			const VectorRegister4Float IsB1 = VectorCast4IntTo4Float(VectorIntCompareEQ(NumVerts, GlobalVectorConstants::IntOne));
			const VectorRegister4Float IsB2 = VectorCast4IntTo4Float(VectorIntCompareEQ(NumVerts, TwoInt));
			const VectorRegister4Float IsB3 = VectorCast4IntTo4Float(VectorIntCompareEQ(NumVerts, ThreeInt));

			ClosestB = VectorSelect(IsB0, ClosestB, ClosestB4);
			ClosestB = VectorSelect(IsB1, ClosestB1, ClosestB);
			ClosestB = VectorSelect(IsB2, ClosestB2, ClosestB);
			ClosestB = VectorSelect(IsB3, ClosestB3, ClosestB);

			const VectorRegister4Float ClosestLocal = VectorNegateMultiplyAdd(OutNormal, MarginBSimd, ClosestB);

			OutPosition = VectorAdd(VectorMultiplyAdd(RayDir, Lambda, StartPoint), ClosestLocal);

		}
		else if (bComputeMTD)
		{
			// If Inflation == 0 we would expect GJKPreDist2 to be 0
			// However, due to precision we can still end up with GJK failing.
			// When that happens fall back on EPA
			VectorRegister4Float InflationGTZero = VectorCompareGT(Inflation, VectorZeroFloat());
			VectorRegister4Float InGJKPreDist2GTEps2 = VectorCompareGT(GJKPreDist2, Eps2Simd);
			VectorRegister4Float LimitMaxGTInGJKPreDist2 = VectorCompareGT(LimitMax, GJKPreDist2);
			VectorRegister4Float IsDone = VectorBitwiseAnd(InflationGTZero, VectorBitwiseAnd(InGJKPreDist2GTEps2, LimitMaxGTInGJKPreDist2));
			if (VectorMaskBits(IsDone))
			{
				OutNormal = Normal;
				VectorRegister4Float ClosestA = VectorZeroFloat();
				VectorRegister4Float ClosestB = VectorZeroFloat();

				if (NumIterations)
				{
					VectorRegister4Float Barycentrics[4];
					Barycentrics[0] = VectorSwizzle(Barycentric, 0, 0, 0, 0);
					Barycentrics[1] = VectorSwizzle(Barycentric, 1, 1, 1, 1);
					Barycentrics[2] = VectorSwizzle(Barycentric, 2, 2, 2, 2);
					Barycentrics[3] = VectorSwizzle(Barycentric, 3, 3, 3, 3);

					alignas(16) int32 NumVertsInts[4];
					VectorIntStoreAligned(NumVerts, NumVertsInts);
					const int NumVertsInt = NumVertsInts[0];
					for (int i = 0; i < NumVertsInt; ++i)
					{
						ClosestA = VectorMultiplyAdd(As[i], Barycentrics[i], ClosestA);
						ClosestB = VectorMultiplyAdd(Bs[i], Barycentrics[i], ClosestB);
					}
				}
				else
				{
					//didn't even go into gjk loop
					ClosestA = As[0];
					ClosestB = Bs[0];
				}

				const VectorRegister4Float ClosestBInA = VectorAdd(StartPoint, ClosestB);
				const VectorRegister4Float InGJKPreDist = VectorSqrt(GJKPreDist2);
				OutNormal = VectorNormalizeAccurate(V);

				VectorRegister4Float Penetration = VectorSubtract(VectorAdd(MarginASimd, MarginBSimd), InGJKPreDist);
				Penetration = VectorMin(Penetration, LimitMax);
				Penetration = VectorMax(Penetration, VectorZeroFloat());

				const VectorRegister4Float ClosestLocal = VectorNegateMultiplyAdd(OutNormal, MarginBSimd, ClosestB);

				OutPosition = VectorAdd(VectorMultiplyAdd(OutNormal, Penetration, StartPoint), ClosestLocal);
				Penetration = VectorNegate(Penetration);
				VectorStoreFloat1(Penetration, &OutTime);
			}
			else
			{
				if (NumIterations)
				{
					TArray<VectorRegister4Float> VertsA;
					TArray<VectorRegister4Float> VertsB;

					VertsA.Reserve(8);
					VertsB.Reserve(8);

					alignas(16) int32 NumVertsInts[4];
					VectorIntStoreAligned(NumVerts, NumVertsInts);
					const int32 NumVertsInt = NumVertsInts[0];

					for (int i = 0; i < NumVertsInt; ++i)
					{
						VertsA.Add(As[i]);
						VertsB.Add(VectorAdd(Bs[i], X));
					}

					auto SupportBAtOriginFunc = [&B, MarginB, &BToARotation, &StartPoint, &AToBRotation](const VectorRegister4Float& Dir)
					{
						int32 VertexIndex = INDEX_NONE;
						const VectorRegister4Float DirInB = VectorQuaternionRotateVector(AToBRotation, Dir);
						const VectorRegister4Float SupportBLocal = B.SupportCoreSimd(DirInB, MarginB);

						const VectorRegister4Float RotatedVec = VectorQuaternionRotateVector(BToARotation, SupportBLocal);
						return VectorAdd(RotatedVec, StartPoint);
					};

					VectorRegister4Float Penetration;
					VectorRegister4Float MTD, ClosestA, ClosestBInA;
					const EEPAResult EPAResult = VectorEPA(VertsA, VertsB, SupportAFunc, SupportBAtOriginFunc, Penetration, MTD, ClosestA, ClosestBInA);
					if (IsEPASuccess(EPAResult))
					{
						OutNormal = MTD;
						VectorStoreFloat1(Penetration, &OutTime);
						OutTime = -OutTime - (MarginA + MarginB);
						OutPosition = ClosestA;
					}
					else
					{
						//assume touching hit
						OutTime = -(MarginA + MarginB);
						OutNormal = MTD;
						OutPosition = VectorMultiplyAdd(OutNormal, MarginASimd, As[0]);
					}
				}

				else
				{
					//didn't even go into gjk loop, touching hit
					OutTime = -(MarginA + MarginB);
					OutNormal = MakeVectorRegisterFloat(0.0f, 0.0f, 1.0f, 0.0f);
					OutPosition = VectorMultiplyAdd(OutNormal, MarginASimd, As[0]);
				}
			}
		}
		else
		{
			// Initial overlap without MTD. These properties are not valid, but assigning them anyway so they don't contain NaNs and cause issues in invoking code.
			OutNormal = MakeVectorRegisterFloat(0.0f, 0.0f, 1.0f, 0.0f);
			OutPosition = MakeVectorRegisterFloat(0.0f, 0.0f, 0.0f, 0.0f);
		}

		return true;
	}

#if GJK_VECTORIZED
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKRaycast2(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& StartTM, const TVector<T, 3>& RayDir, const T RayLength,
		T& OutTime, TVector<T, 3>& OutPosition, TVector<T, 3>& OutNormal, const T GivenThicknessA = 0, bool bComputeMTD = false, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T GivenThicknessB = 0)
	{
		const UE::Math::TQuat<T>& RotationDouble = StartTM.GetRotation();
		VectorRegister4Float Rotation = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(RotationDouble.X, RotationDouble.Y, RotationDouble.Z, RotationDouble.W));

		const UE::Math::TVector<T>& TranslationDouble = StartTM.GetTranslation();
		const VectorRegister4Float Translation = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(TranslationDouble.X, TranslationDouble.Y, TranslationDouble.Z, 0.0));

		// Normalize rotation
		Rotation = VectorNormalizeSafe(Rotation, GlobalVectorConstants::Float0001);
		
		const VectorRegister4Float InitialDirSimd = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(InitialDir[0], InitialDir[1], InitialDir[2], 0.0));
		const VectorRegister4Float RayDirSimd = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(RayDir[0], RayDir[1], RayDir[2], 0.0));

		FRealSingle OutTimeFloat;
		VectorRegister4Float OutPositionSimd, OutNormalSimd;
		bool result = GJKRaycast2ImplSimd(A, B, Rotation, Translation, RayDirSimd, static_cast<FRealSingle>(RayLength), OutTimeFloat, OutPositionSimd, OutNormalSimd, bComputeMTD, InitialDirSimd, StartTM);

		OutTime = static_cast<double>(OutTimeFloat);


		alignas(16) FRealSingle OutFloat[4];
		VectorStoreAligned(OutNormalSimd, OutFloat);
		OutNormal.X = OutFloat[0];
		OutNormal.Y = OutFloat[1];
		OutNormal.Z = OutFloat[2];

		VectorStoreAligned(OutPositionSimd, OutFloat);
		OutPosition.X = OutFloat[0];
		OutPosition.Y = OutFloat[1];
		OutPosition.Z = OutFloat[2];

		return result;
	}

#else // GJK_VECTORIZED

	/** Sweeps one geometry against the other
	 @A The first geometry
	 @B The second geometry
	 @StartTM B's starting configuration in A's local space
	 @RayDir The ray's direction (normalized)
	 @RayLength The ray's length
	 @OutTime The time along the ray when the objects first overlap
	 @OutPosition The first point of impact (in A's local space) when the objects first overlap. Invalid if time of impact is 0
	 @OutNormal The impact normal (in A's local space) when the objects first overlap. Invalid if time of impact is 0
	 @ThicknessA The amount of geometry inflation for Geometry A (for example a capsule with radius 5 could pass in its core segnment and a thickness of 5)
	 @InitialDir The first direction we use to search the CSO
	 @ThicknessB The amount of geometry inflation for Geometry B (for example a sphere with radius 5 could pass in its center point and a thickness of 5)
	 @return True if the geometries overlap during the sweep, False otherwise
	 @note If A overlaps B at the start of the ray ("initial overlap" condition) then this function returns true, and sets OutTime = 0, but does not set any other output variables.
	 */

	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKRaycast2(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& StartTM, const TVector<T, 3>& RayDir, const T RayLength,
		T& OutTime, TVector<T, 3>& OutPosition, TVector<T, 3>& OutNormal, const T GivenThicknessA = 0, bool bComputeMTD = false, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T GivenThicknessB = 0)
	{
		return GJKRaycast2Impl(A, B, StartTM, RayDir, RayLength, OutTime, OutPosition, OutNormal, GivenThicknessA, bComputeMTD, InitialDir, GivenThicknessB);
	}
#endif // GJK_VECTORIZED

	/**
	 * Used by GJKDistance. It must return a vector in the Minkowski sum A - B. In principle this can be the vector of any point
	 * in A to any point in B, but some choices will cause GJK to minimize faster (e.g., for two spheres, we can easily calculate
	 * the actual separating vector and GJK will converge immediately).
	 */
	template <typename T, typename TGeometryA, typename TGeometryB>
	TVector<T, 3> GJKDistanceInitialV(const TGeometryA& A, T MarginA, const TGeometryB& B, T MarginB, const TRigidTransform<T, 3>& BToATM)
	{
		int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;
		const TVec3<T> V = -BToATM.GetTranslation();
		const TVector<T, 3> SupportA = A.SupportCore(-V, MarginA, nullptr, VertexIndexA);
		const TVector<T, 3> VInB = BToATM.GetRotation().Inverse() * V;
		const TVector<T, 3> SupportBLocal = B.SupportCore(VInB, MarginB, nullptr, VertexIndexB);
		const TVector<T, 3> SupportB = BToATM.TransformPositionNoScale(SupportBLocal);
		return SupportA - SupportB;
	}

	/**
	 * Used by GJKDistance. Specialization for sphere-sphere gives correct result immediately.
	 */
	template <typename T>
	TVector<T, 3> GJKDistanceInitialV(const TSphere<T, 3>& A, const TSphere<T, 3>& B, const TRigidTransform<T, 3>& BToATM)
	{
		TVector<T, 3> Delta = A.GetCenter() - (B.GetCenter() + BToATM.GetTranslation());
		return Delta;
	}

	// Status of a call to GJKDistance
	enum class EGJKDistanceResult
	{
		// The shapes are separated by a positive amount and all outputs have valid values
		Separated,

		// The shapes are overlapping by less than the net margin and all outputs have valid values (with a negative separation)
		Contact,

		// The shapes are overlapping by more than the net margin and all outputs are invalid
		DeepContact,
	};

	/**
	 * Find the distance and nearest points on two convex geometries A and B.
	 * All calculations are performed in the local-space of object A, and the transform from B-space to A-space must be provided.
	 * For algorithm see "A Fast and Robust GJK Implementation for Collision Detection of Convex Objects", Gino Van Deb Bergen, 1999.
	 * @note This algorithm aborts if objects are overlapping and it does not initialize the out parameters.
	 *
	 * @param A The first object.
	 * @param B The second object.
	 * @param BToATM A transform taking vectors in B-space to A-space
	 * @param B The second object.
	 * @param OutDistance if returns true, the minimum distance between A and B, otherwise not modified.
	 * @param OutNearestA if returns true, the near point on A in local-space, otherwise not modified.
	 * @param OutNearestB if returns true, the near point on B in local-space, otherwise not modified.
	 * @param Epsilon The algorithm terminates when the iterative distance reduction gets below this threshold.
	 * @param MaxIts A limit on the number of iterations. Results may be approximate if this is too low.
	 * @return EGJKDistanceResult - see comments on the enum
	 */
	template <typename T, typename TGeometryA, typename TGeometryB>
	EGJKDistanceResult GJKDistance(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, T& OutDistance, TVector<T, 3>& OutNearestA, TVector<T, 3>& OutNearestB, TVector<T, 3>& OutNormalA, const T Epsilon = (T)1e-3, const int32 MaxIts = 16)
	{
		check(A.IsConvex() && B.IsConvex());

		FSimplex SimplexIDs;
		TVector<T, 3> Simplex[4], SimplexA[4], SimplexB[4];
		T Barycentric[4] = { -1, -1, -1, -1 };

		const TRotation<T, 3> AToBRotation = BToATM.GetRotation().Inverse();
		const T AMargin = A.GetMargin();
		const T BMargin = B.GetMargin();
		T Mu = 0;

		// Select an initial vector in Minkowski(A - B)
		TVector<T, 3> V = GJKDistanceInitialV(A, AMargin, B, BMargin, BToATM);
		T VLen = V.Size();
		int32 VertexIndexA = INDEX_NONE, VertexIndexB = INDEX_NONE;

		int32 It = 0;
		while (VLen > Epsilon)
		{
			// Find a new point in A-B that is closer to the origin
			// NOTE: we do not use support thickness here. Thickness is used when separating objects
			// so that GJK can find a solution, but that can be added in a later step.
			const TVector<T, 3> SupportA = A.SupportCore(-V, AMargin, nullptr, VertexIndexA);
			const TVector<T, 3> VInB = AToBRotation * V;
			const TVector<T, 3> SupportBLocal = B.SupportCore(VInB, BMargin, nullptr, VertexIndexB);
			const TVector<T, 3> SupportB = BToATM.TransformPositionNoScale(SupportBLocal);
			const TVector<T, 3> W = SupportA - SupportB;

			T D = TVector<T, 3>::DotProduct(V, W) / VLen;
			Mu = FMath::Max(Mu, D);

			// See if we are still making progress toward the origin
			bool bCloseEnough = ((VLen - Mu) < Epsilon);
			if (bCloseEnough || (++It > MaxIts))
			{
				// We have reached the minimum to within tolerance. Or we have reached max iterations, in which
				// case we (probably) have a solution but with an error larger than Epsilon (technically we could be missing
				// the fact that we were going to eventually find the origin, but it'll be a close call so the approximation
				// is still good enough).
				if (SimplexIDs.NumVerts == 0)
				{
					// Our initial guess of V was already the minimum separating vector
					OutNearestA = SupportA;
					OutNearestB = SupportBLocal;
				}
				else
				{
					// The simplex vertices are the nearest point/line/face
					OutNearestA = TVector<T, 3>(0, 0, 0);
					OutNearestB = TVector<T, 3>(0, 0, 0);
					for (int32 VertIndex = 0; VertIndex < SimplexIDs.NumVerts; ++VertIndex)
					{
						int32 WIndex = SimplexIDs[VertIndex];
						check(Barycentric[WIndex] >= (T)0);
						OutNearestA += Barycentric[WIndex] * SimplexA[WIndex];
						OutNearestB += Barycentric[WIndex] * SimplexB[WIndex];
					}
				}
				const TVector<T, 3> NormalA = -V / VLen;
				const TVector<T, 3> NormalB = VInB / VLen;
				OutDistance = VLen - (AMargin + BMargin);
				OutNearestA += AMargin * NormalA;
				OutNearestB += BMargin * NormalB;
				OutNormalA = NormalA;

				return (OutDistance >= 0.0f) ? EGJKDistanceResult::Separated : EGJKDistanceResult::Contact;
			}

			// Add the new vertex to the simplex
			SimplexIDs[SimplexIDs.NumVerts] = SimplexIDs.NumVerts;
			Simplex[SimplexIDs.NumVerts] = W;
			SimplexA[SimplexIDs.NumVerts] = SupportA;
			SimplexB[SimplexIDs.NumVerts] = SupportBLocal;
			++SimplexIDs.NumVerts;

			// Find the closest point to the origin on the simplex, and update the simplex to eliminate unnecessary vertices
			V = SimplexFindClosestToOrigin(Simplex, SimplexIDs, Barycentric, SimplexA, SimplexB);
			VLen = V.Size();
		}

		// Our geometries overlap - we did not set any outputs
		return EGJKDistanceResult::DeepContact;
	}





	// Assumes objects are already intersecting, computes a minimum translation
	// distance, deepest penetration positions on each body, and approximates
	// a penetration normal and minimum translation distance.
	//
	// TODO: We want to re-visit how these functions work. Probably should be
	// embedded in GJKOverlap and GJKRaycast so that secondary queries are unnecessary.
	template <typename T, typename TGeometryA, typename TGeometryB>
	bool GJKPenetrationTemp(const TGeometryA& A, const TGeometryB& B, const TRigidTransform<T, 3>& BToATM, TVector<T, 3>& OutPositionA, TVector<T, 3>& OutPositionB, TVector<T, 3>& OutNormal, T& OutDistance, const T ThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T ThicknessB = 0, const T Epsilon = (T)1e-6, const int32 MaxIts = 16)
	{
		//
		// TODO: General case for MTD determination.
		//
		ensure(false);
		OutPositionA = TVector<T, 3>(0.f);
		OutPositionB = TVector<T, 3>(0.f);
		OutNormal = TVector<T, 3>(0.f, 0.f, 1.f);
		OutDistance = 0.f;
		return GJKIntersection(A, B, BToATM, ThicknessA, InitialDir, ThicknessB);
	}

	// Specialization for when getting MTD against a capsule.
	template <typename T, typename TGeometryA>
	bool GJKPenetrationTemp(const TGeometryA& A, const FCapsule& B, const TRigidTransform<T, 3>& BToATM, TVector<T, 3>& OutPositionA, TVector<T, 3>& OutPositionB, TVector<T, 3>& OutNormal, T& OutDistance, const T ThicknessA = 0, const TVector<T, 3>& InitialDir = TVector<T, 3>(1, 0, 0), const T ThicknessB = 0, const T Epsilon = (T)1e-6, const int32 MaxIts = 16)
	{
		T SegmentDistance;
		const TSegment<T>& Segment = B.GetSegment();
		const T MarginB = B.GetRadius();
		TVector<T, 3> PositionBInB;
		if (GJKDistance(A, Segment, BToATM, SegmentDistance, OutPositionA, PositionBInB, Epsilon, MaxIts))
		{
			OutPositionB = BToATM.TransformPosition(PositionBInB);
			OutNormal
				= ensure(SegmentDistance > TNumericLimits<T>::Min())
				? (OutPositionB - OutPositionA) / SegmentDistance
				: TVector<T, 3>(0.f, 0.f, 1.f);
			OutPositionB -= OutNormal * MarginB;
			OutDistance = SegmentDistance - MarginB;

			if (OutDistance > 0.f)
			{
				// In this case, our distance calculation says we're not penetrating.
				//
				// TODO: check(false)! This shouldn't happen.
				// It probably won't happen anymore if we warm-start GJKDistance
				// with a polytope.
				//
				OutDistance = 0.f;
				return false;
			}

			return true;
		}
		else
		{
			// TODO: Deep penetration - do EPA
			ensure(false);
			return true;
		}

		return false;
	}

}