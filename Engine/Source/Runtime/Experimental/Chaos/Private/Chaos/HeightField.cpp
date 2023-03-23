// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/HeightField.h"
#include "Chaos/Collision/ContactPoint.h"
#include "Chaos/Collision/PBDCollisionConstraint.h"
#include "Chaos/CollisionOneShotManifolds.h"
#include "Chaos/Core.h"
#include "Chaos/Convex.h"
#include "Chaos/Box.h"
#include "Chaos/GJK.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Capsule.h"
#include "Chaos/GeometryQueries.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Chaos/Triangle.h"
#include "Chaos/TriangleRegister.h"

namespace Chaos
{
	extern FRealSingle Chaos_Collision_EdgePrunePlaneDistance;

	int32 bOneSidedHeightField = 1;
	static FAutoConsoleVariableRef CVarOneSidedHeightField(TEXT("p.Chaos.OneSidedHeightField"), bOneSidedHeightField, TEXT("When enabled, extra steps will ensure that FHeightField::GJKContactPointImp never results in internal-facing contact data."));

	int32 bOneSidedHeightfieldAlwaysSweep = 1;
	static FAutoConsoleVariableRef CVarOneSidedHeightfieldAlwaysSweep(TEXT("p.Chaos.OneSidedHeightfieldAlwaysSweep"), bOneSidedHeightfieldAlwaysSweep, TEXT("When enabled, always use a sweep to ensure FHeightField::GJKContactPointImp never results \
	in internal-facing contact data. Else, we only sweep if we detect an inward facing normal. Note that the sweep results can be inaccurate in some cases."));

	class FHeightfieldRaycastVisitor
	{
	public:
		FHeightfieldRaycastVisitor(const typename FHeightField::FDataType* InData, const FVec3& InStart, const FVec3& InDir, const FReal InThickness)
			: OutTime(TNumericLimits<FReal>::Max())
			, OutFaceIndex(INDEX_NONE)
			, GeomData(InData)
			, Start(InStart)
			, Dir(InDir)
			, Thickness(InThickness)
		{
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				bParallel[Axis] = FMath::IsNearlyZero(Dir[Axis], (FReal)1.e-8);
				InvDir[Axis] = bParallel[Axis] ? 0 : 1 / Dir[Axis];
			}
		}

		enum class ERaycastType
		{
			Raycast,
			Sweep
		};

		/**
		* Ray / triangle  intersection
		* this provides a double sided test
		* note : this method assumes that the triangle formed by A,B and C is well formed
		*/
		FORCEINLINE_DEBUGGABLE bool RayTriangleIntersection(
			const FVec3& RayStart, const FVec3& RayDir, FReal RayLength,
			const FVec3& A, const FVec3& B, const FVec3& C,
			FReal& OutT, FVec3& OutN
		)
		{
			const FVec3 AB = B - A; // edge 1
			const FVec3 AC = C - A; // edge 2
			const FVec3 Normal = FVec3::CrossProduct(AB, AC);
			const FVec3 NegRayDir = -RayDir;

			FReal Den = FVec3::DotProduct(NegRayDir, Normal);
			if (FMath::Abs(Den) < SMALL_NUMBER)
			{
				// ray is parallel or away to the triangle plane it is a miss
				return false;
			}

			const FReal InvDen = (FReal)1 / Den;

			// let's compute the time to intersection
			const FVec3 RayToA = RayStart - A;
			const FReal Time = FVec3::DotProduct(RayToA, Normal) * InvDen;
			if (Time < (FReal)0 || Time > RayLength)
			{
				return false;
			}

			// now compute baricentric coordinates
			const FVec3 RayToACrossNegDir = FVec3::CrossProduct(NegRayDir, RayToA);
			constexpr FReal Epsilon = SMALL_NUMBER;
			const FReal UU = FVec3::DotProduct(AC, RayToACrossNegDir) * InvDen;
			if (UU < -Epsilon || UU >(1 + Epsilon))
			{
				return false; // outside of the triangle
			}
			const FReal VV = -FVec3::DotProduct(AB, RayToACrossNegDir) * InvDen;
			if (VV < -Epsilon || (VV + UU) >(1 + Epsilon))
			{
				return false; // outside of the triangle
			}

			// point is within the triangle, let's compute 
			OutT = Time;
			OutN = Normal.GetSafeNormal();
			OutN *= FMath::Sign(Den);
			return true;
		}


		FORCEINLINE_DEBUGGABLE bool VisitRaycast(int32 Payload, FReal& CurrentLength)
		{
			const int32 SubX = Payload % (GeomData->NumCols - 1);
			const int32 SubY = Payload / (GeomData->NumCols - 1);

			const int32 FullIndex = Payload + SubY;

			const FReal Radius = Thickness + SMALL_NUMBER;
			const FReal Radius2 = Radius * Radius;
			bool bIntersection = false;

			// return if the triangle was hit or not 
			auto TestTriangle = [&](int32 FaceIndex, const FVec3& A, const FVec3& B, const FVec3& C) -> bool
			{
				FReal Time;
				FVec3 Normal;
				if (RayTriangleIntersection(Start, Dir, CurrentLength, A, B, C, Time, Normal))
				{
					if (Time < OutTime)
					{
						bool bHole = false;

						const int32 CellIndex = FaceIndex / 2;
						if (GeomData->MaterialIndices.IsValidIndex(CellIndex))
						{
							bHole = GeomData->MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max();
						}

						if (!bHole)
						{
							OutPosition = Start + (Dir * Time);
							OutNormal = Normal;
							OutTime = Time;
							OutFaceIndex = FaceIndex;
							CurrentLength = Time;
							return true;
						}
					}
				}

				return false;
			};

			FVec3 Points[4];
			FAABB3 CellBounds;
			GeomData->GetPointsAndBoundsScaled(FullIndex, Points, CellBounds);
			CellBounds.Thicken(Thickness);

			// Check cell bounds
			//todo: can do it without raycast
			FReal TOI;
			FVec3 HitPoint;
			bool bHit = false;
			if (CellBounds.RaycastFast(Start, Dir, InvDir, bParallel, CurrentLength, 1.0f / CurrentLength, TOI, HitPoint))
			{
				// Test both triangles that are in this cell, as we could hit both in any order
				bHit |= TestTriangle(Payload * 2, Points[0], Points[1], Points[3]);
				bHit |= TestTriangle(Payload * 2 + 1, Points[0], Points[3], Points[2]);
			}
			const bool bShouldContinueVisiting = !bHit;
			return bShouldContinueVisiting;
		}

		bool VisitSweep(int32 Payload, FReal& CurrentLength)
		{
			const int32 SubX = Payload % (GeomData->NumCols - 1);
			const int32 SubY = Payload / (GeomData->NumCols - 1);

			const int32 FullIndex = Payload + SubY;

			const FReal Radius = Thickness + SMALL_NUMBER;
			const FReal Radius2 = Radius * Radius;
			bool bIntersection = false;

			// return if the triangle was hit or not 
			auto TestTriangle = [&](int32 FaceIndex, const FVec3& A, const FVec3& B, const FVec3& C) -> bool
			{
				const FVec3 AB = B - A;
				const FVec3 AC = C - A;

				FVec3 Normal = FVec3::CrossProduct(AB, AC);
				const FReal Len2 = Normal.SafeNormalize();

				if(!ensure(Len2 > SMALL_NUMBER))
				{
					// Bad triangle, co-linear points or very thin
					return false;
				}

				const TPlane<FReal, 3> TrianglePlane(A, Normal);

				FVec3 ResultPosition(0);
				FVec3 ResultNormal(0);
				FReal Time = TNumericLimits<FReal>::Max();
				int32 DummyFaceIndex = INDEX_NONE;

				if(TrianglePlane.Raycast(Start, Dir, CurrentLength, Thickness, Time, ResultPosition, ResultNormal, DummyFaceIndex))
				{
					if(Time == 0)
					{
						// Initial overlap
						const FVec3 ClosestPtOnTri = FindClosestPointOnTriangle(TrianglePlane, A, B, C, Start);
						const FReal DistToTriangle2 = (Start - ClosestPtOnTri).SizeSquared();
						if(DistToTriangle2 <= Radius2)
						{
							OutTime = 0;
							OutPosition = ClosestPtOnTri;
							OutNormal = Normal;
							OutFaceIndex = FaceIndex;
							return true;
						}
					}
					else
					{
						const FVec3 ClosestPtOnTri = FindClosestPointOnTriangle(ResultPosition, A, B, C, ResultPosition);
						const FReal DistToTriangle2 = (ResultPosition - ClosestPtOnTri).SizeSquared();
						bIntersection = DistToTriangle2 <= SMALL_NUMBER;
					}
				}

				if(!bIntersection)
				{
					//sphere is not immediately touching the triangle, but it could start intersecting the perimeter as it sweeps by
					FVec3 BorderPositions[3];
					FVec3 BorderNormals[3];
					FReal BorderTimes[3];
					bool bBorderIntersections[3];

					const FCapsule ABCapsule(A, B, Thickness);
					bBorderIntersections[0] = ABCapsule.Raycast(Start, Dir, CurrentLength, 0, BorderTimes[0], BorderPositions[0], BorderNormals[0], DummyFaceIndex);

					const FCapsule BCCapsule(B, C, Thickness);
					bBorderIntersections[1] = BCCapsule.Raycast(Start, Dir, CurrentLength, 0, BorderTimes[1], BorderPositions[1], BorderNormals[1], DummyFaceIndex);

					const FCapsule ACCapsule(A, C, Thickness);
					bBorderIntersections[2] = ACCapsule.Raycast(Start, Dir, CurrentLength, 0, BorderTimes[2], BorderPositions[2], BorderNormals[2], DummyFaceIndex);

					int32 MinBorderIdx = INDEX_NONE;
					FReal MinBorderTime = 0;

					for(int32 BorderIdx = 0; BorderIdx < 3; ++BorderIdx)
					{
						if(bBorderIntersections[BorderIdx])
						{
							if(!bIntersection || BorderTimes[BorderIdx] < MinBorderTime)
							{
								MinBorderTime = BorderTimes[BorderIdx];
								MinBorderIdx = BorderIdx;
								bIntersection = true;
							}
						}
					}

					if(MinBorderIdx != INDEX_NONE)
					{
						ResultNormal = BorderNormals[MinBorderIdx];
						ResultPosition = BorderPositions[MinBorderIdx] - ResultNormal * Thickness;

						if(Time == 0)
						{
							//we were initially overlapping with triangle plane so no normal was given. Compute it now
							FVec3 TmpNormal;
							const FReal SignedDistance = TrianglePlane.PhiWithNormal(Start, TmpNormal);
							ResultNormal = SignedDistance >= 0 ? TmpNormal : -TmpNormal;
						}

						Time = MinBorderTime;
					}
				}

				if(bIntersection)
				{
					if(Time < OutTime)
					{
						bool bHole = false;

						const int32 CellIndex = FaceIndex / 2;
						if(GeomData->MaterialIndices.IsValidIndex(CellIndex))
						{
							bHole = GeomData->MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max();
						}

						if(!bHole)
						{
							OutPosition = ResultPosition;
							OutNormal = ResultNormal;
							OutTime = Time;
							OutFaceIndex = FaceIndex;
							CurrentLength = Time;
							return true;
						}
					}
				}

				return false;
			};

			FVec3 Points[4];
			FAABB3 CellBounds;
			GeomData->GetPointsAndBoundsScaled(FullIndex, Points, CellBounds);
			CellBounds.Thicken(Thickness);

			// Check cell bounds
			//todo: can do it without raycast
			FReal TOI;
			FVec3 HitPoint;
			bool bHit = false;
			if (CellBounds.RaycastFast(Start, Dir, InvDir, bParallel, CurrentLength, 1.0f / CurrentLength, TOI, HitPoint))
			{
				// Test both triangles that are in this cell, as we could hit both in any order
				bHit |= TestTriangle(Payload * 2, Points[0], Points[1], Points[3]);
				bHit |= TestTriangle(Payload * 2 + 1, Points[0], Points[3], Points[2]);
			}
			const bool bShouldContinueVisiting = !bHit;
			return bShouldContinueVisiting;
		}

		FReal OutTime;
		FVec3 OutPosition;
		FVec3 OutNormal;
		int32 OutFaceIndex;

	private:

		const typename FHeightField::FDataType* GeomData;

		FVec3 Start;
		FVec3 Dir;
		FVec3 InvDir;
		bool  bParallel[3];
		FReal Thickness;
	};

	template<typename GeomQueryType>
	class THeightfieldSweepVisitor
	{
	public:

		THeightfieldSweepVisitor(const typename FHeightField::FDataType* InData, const GeomQueryType& InQueryGeom, const FRigidTransform3& InStartTM, const FVec3& InDir, const FReal InThickness, bool InComputeMTD)
			: OutTime(TNumericLimits<FReal>::Max())
			, OutFaceIndex(INDEX_NONE)
			, HfData(InData)
			, StartTM(InStartTM)
			, OtherGeom(InQueryGeom)
			, Dir(InDir)
			, Thickness(InThickness)
			, bComputeMTD(InComputeMTD)
		{
			const FAABB3 QueryBounds = InQueryGeom.BoundingBox();
			StartPoint = StartTM.TransformPositionNoScale(QueryBounds.Center());
			Inflation3D = QueryBounds.Extents() * 0.5 + FVec3(Thickness);
			for (int Axis = 0; Axis < 3; ++Axis)
			{
				bParallel[Axis] = FMath::IsNearlyZero(Dir[Axis], (FReal)1.e-8);
				InvDir[Axis] = bParallel[Axis] ? 0 : 1 / Dir[Axis];
			}
		}

		bool VisitSweep(int32 Payload, FReal& CurrentLength)
		{
			const int32 SubX = Payload % (HfData->NumCols - 1);
			const int32 SubY = Payload / (HfData->NumCols - 1);

			const int32 FullIndex = Payload + SubY;

			auto TestTriangle = [&](int32 FaceIndex, const FVec3& A, const FVec3& B, const FVec3& C) -> bool
			{
				if(OutTime == 0)
				{
					return false;
				}

				//Convert into local space of A to get better precision
				VectorRegister4Float AReg = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(A.X, A.Y, A.Z, 0.0));
				VectorRegister4Float BReg = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(B.X, B.Y, B.Z, 0.0));
				VectorRegister4Float CReg = MakeVectorRegisterFloatFromDouble(MakeVectorRegister(C.X, C.Y, C.Z, 0.0));

				FTriangleRegister Triangle(VectorZero(), VectorSubtract(BReg, AReg), VectorSubtract(CReg, AReg));

				FReal Time;
				FVec3 LocalHitPosition;
				FVec3 HitNormal;
				const FRigidTransform3 LocalStartTM(StartTM.GetTranslation() - A,StartTM.GetRotation());
				if(GJKRaycast2<FReal>(Triangle, OtherGeom, LocalStartTM, Dir, CurrentLength, Time, LocalHitPosition, HitNormal, Thickness, bComputeMTD))
				{
					if(Time < OutTime)
					{
						bool bHole = false;

						const int32 CellIndex = FaceIndex / 2;
						if(HfData->MaterialIndices.IsValidIndex(CellIndex))
						{
							bHole = HfData->MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max();
						}

						if(!bHole)
						{
							OutNormal = HitNormal;
							OutPosition = LocalHitPosition + A;
							OutTime = Time;
							OutFaceIndex = FaceIndex;

							if(Time <= 0) //initial overlap or MTD, so stop
							{
								// This is incorrect. To prevent objects pushing through the surface of the heightfield
								// we adopt the triangle normal but this leaves us with an incorrect MTD from the GJK call
								// above. #TODO possibly re-do GJK with a plane, or some geom vs.plane special case to solve
								// both triangles as planes 
								const FVec3 AB = B - A;
								const FVec3 AC = C - A;

								FVec3 TriNormal = FVec3::CrossProduct(AB, AC);
								TriNormal.SafeNormalize();

								OutNormal = TriNormal;
								CurrentLength = 0;
								return false;
							}

							CurrentLength = Time;
						}
					}
				}

				return true;
			};

			FVec3 Points[4];
			FAABB3 CellBounds;
			HfData->GetPointsAndBoundsScaled(FullIndex, Points, CellBounds);
			CellBounds.ThickenSymmetrically(Inflation3D);

			// Check cell bounds
			//todo: can do it without raycast
			FReal TOI;
			FVec3 HitPoint;
			if (CellBounds.RaycastFast(StartPoint, Dir, InvDir, bParallel, CurrentLength, 1.0f / CurrentLength, TOI, HitPoint))
			{
				bool bContinue = TestTriangle(Payload * 2, Points[0], Points[1], Points[3]);
				if (bContinue)
				{
					TestTriangle(Payload * 2 + 1, Points[0], Points[3], Points[2]);
				}
			}
			return OutTime > 0;
		}

		FReal OutTime;
		FVec3 OutPosition;
		FVec3 OutNormal;
		int32 OutFaceIndex;

	private:

		const typename FHeightField::FDataType* HfData;
		const FRigidTransform3 StartTM;
		const GeomQueryType& OtherGeom;
		const FVec3& Dir;
		FVec3 InvDir;
		bool  bParallel[3];
		const FReal Thickness;
		bool bComputeMTD;
		FVec3 StartPoint;
		FVec3 Inflation3D;

	};

	template<typename BufferType>
	void BuildGeomData(TArrayView<BufferType> BufferView, TArrayView<uint8> MaterialIndexView, int32 NumRows, int32 NumCols, const FVec3& InScale, TUniqueFunction<FReal(const BufferType)> ToRealFunc, typename FHeightField::FDataType& OutData, FAABB3& OutBounds)
	{
		using FDataType = typename FHeightField::FDataType;

		const bool bHaveMaterials = MaterialIndexView.Num() > 0;
		const bool bOnlyDefaultMaterial = MaterialIndexView.Num() == 1;
		ensure(BufferView.Num() == NumRows * NumCols);
		ensure(NumRows > 1);
		ensure(NumCols > 1);

		// Populate data.
		const int32 NumHeights = BufferView.Num();
		OutData.Heights.SetNum(NumHeights);

		OutData.NumRows = static_cast<uint16>(NumRows);
		OutData.NumCols = static_cast<uint16>(NumCols);
		OutData.MinValue = ToRealFunc(BufferView[0]);
		OutData.MaxValue = ToRealFunc(BufferView[0]);
		OutData.Scale = InScale;

		for(int32 HeightIndex = 1; HeightIndex < NumHeights; ++HeightIndex)
		{
			const FReal CurrHeight = ToRealFunc(BufferView[HeightIndex]);

			if(CurrHeight > OutData.MaxValue)
			{
				OutData.MaxValue = CurrHeight;
			}
			else if(CurrHeight < OutData.MinValue)
			{
				OutData.MinValue = CurrHeight;
			}
		}

		OutData.Range = OutData.MaxValue - OutData.MinValue;
		OutData.HeightPerUnit = OutData.Range / FDataType::StorageRange;

		for(int32 HeightIndex = 0; HeightIndex < NumHeights; ++HeightIndex)
		{
			OutData.Heights[HeightIndex] = static_cast<typename FDataType::StorageType>((ToRealFunc(BufferView[HeightIndex]) - OutData.MinValue) / OutData.HeightPerUnit);

			int32 X = HeightIndex % (NumCols);
			int32 Y = HeightIndex / (NumCols);
			FVec3 Position(FReal(X), FReal(Y), OutData.MinValue + OutData.Heights[HeightIndex] * OutData.HeightPerUnit);
			if(HeightIndex == 0)
			{
				OutBounds = FAABB3(Position * InScale, Position * InScale);
			}
			else
			{
				OutBounds.GrowToInclude(Position * InScale);
			}
		}
		OutBounds.Thicken(KINDA_SMALL_NUMBER);

		if(bHaveMaterials)
		{
			if(bOnlyDefaultMaterial)
			{
				OutData.MaterialIndices.Add(0);
			}
			else
			{
				const int32 NumCells = NumHeights - NumRows - NumCols + 1;
				ensure(MaterialIndexView.Num() == NumCells);
				OutData.MaterialIndices.Empty();
				OutData.MaterialIndices.Append(MaterialIndexView.GetData(), MaterialIndexView.Num());
			}
		}
	}

	template<typename BufferType>
	void EditGeomData(TArrayView<BufferType> BufferView, int32 InBeginRow, int32 InBeginCol, int32 NumRows, int32 NumCols, TUniqueFunction<FReal(const BufferType)> ToRealFunc, typename FHeightField::FDataType& OutData, FAABB3& OutBounds)
	{
		using FDataType = typename FHeightField::FDataType;

		FReal MinValue = TNumericLimits<FReal>::Max();
		FReal MaxValue = TNumericLimits<FReal>::Min();

		for(BufferType& Value : BufferView)
		{
			MinValue = FMath::Min(MinValue, ToRealFunc(Value));
			MaxValue = FMath::Max(MaxValue, ToRealFunc(Value));
		}

		const int32 EndRow = InBeginRow + NumRows;
		const int32 EndCol = InBeginCol + NumCols;

		// If our range now falls outside of the original ranges we need to resample the whole heightfield to perform the edit.
		// Here we resample everything outside of the edit and update our ranges
		const bool bNeedsResample = MinValue < OutData.MinValue || MaxValue > OutData.MaxValue;
		if(bNeedsResample)
		{
			const FReal NewMin = FMath::Min(MinValue, OutData.MinValue);
			const FReal NewMax = FMath::Max(MaxValue, OutData.MaxValue);
			const FReal NewRange = NewMax - NewMin;
			const FReal NewHeightPerUnit = NewRange / FDataType::StorageRange;

			for(int32 RowIdx = 0; RowIdx < OutData.NumRows; ++RowIdx)
			{
				for(int32 ColIdx = 0; ColIdx < OutData.NumCols; ++ColIdx)
				{
					// Provided buffer has inverted column index, invert col to ensure Heights is filled out the same was as BuildGeomData.
					const int32 HeightIndex = RowIdx * OutData.NumCols + (OutData.NumCols - 1 - ColIdx);

					if(RowIdx >= InBeginRow && RowIdx < EndRow &&
						ColIdx >= InBeginCol && ColIdx < EndCol)
					{
						// From the new set
						const int32 NewSetIndex = (RowIdx - InBeginRow) * NumCols + (ColIdx - InBeginCol);
						OutData.Heights[HeightIndex] = static_cast<typename FDataType::StorageType>((ToRealFunc(BufferView[NewSetIndex]) - NewMin) / NewHeightPerUnit);
					}
					else
					{
						// Resample existing
						const FReal ExpandedHeight = OutData.MinValue + OutData.Heights[HeightIndex] * OutData.HeightPerUnit;
						OutData.Heights[HeightIndex] = static_cast<typename FDataType::StorageType>((ExpandedHeight - NewMin) / NewHeightPerUnit);
					}

					int32 X = HeightIndex % (OutData.NumCols);
					int32 Y = HeightIndex / (OutData.NumCols);
					FVec3 Position(FReal(X), FReal(Y), NewMin + OutData.Heights[HeightIndex] * NewHeightPerUnit);
					if(HeightIndex == 0)
					{
						OutBounds = FAABB3(Position, Position);
					}
					else
					{
						OutBounds.GrowToInclude(Position);
					}
				}
			}

			OutBounds.Thicken(KINDA_SMALL_NUMBER);

			OutData.MinValue = NewMin;
			OutData.MaxValue = NewMax;
			OutData.HeightPerUnit = NewHeightPerUnit;
			OutData.Range = NewRange;
		}
		else
		{
			// No resample, just push new heights into the data
			for(int32 RowIdx = InBeginRow; RowIdx < EndRow; ++RowIdx)
			{
				for(int32 ColIdx = InBeginCol; ColIdx < EndCol; ++ColIdx)
				{
					// Provided buffer has inverted column index, invert col to ensure Heights is filled out the same was as BuildGeomData.
					const int32 HeightIndex = RowIdx * OutData.NumCols + (OutData.NumCols - 1 - ColIdx);
					const int32 NewSetIndex = (RowIdx - InBeginRow) * NumCols + (ColIdx - InBeginCol);
					OutData.Heights[HeightIndex] = static_cast<typename FDataType::StorageType>((ToRealFunc(BufferView[NewSetIndex]) - OutData.MinValue) / OutData.HeightPerUnit);
				}
			}
		}
	}

	FHeightField::FHeightField(TArray<FReal>&& Height, TArray<uint8>&& InMaterialIndices, int32 NumRows, int32 NumCols, const FVec3& InScale)
		: FImplicitObject(EImplicitObject::HasBoundingBox, ImplicitObjectType::HeightField)
	{
		TUniqueFunction<FReal(FReal)> ConversionFunc = [](const FReal InVal) -> FReal
		{
			return InVal;
		};

		BuildGeomData<FReal>(MakeArrayView(Height), MakeArrayView(InMaterialIndices), NumRows, NumCols, FVec3(1), MoveTemp(ConversionFunc), GeomData, LocalBounds);
		CalcBounds();
		SetScale(InScale);
	}

	Chaos::FHeightField::FHeightField(TArrayView<const uint16> InHeights, TArrayView<uint8> InMaterialIndices, int32 InNumRows, int32 InNumCols, const FVec3& InScale)
		: FImplicitObject(EImplicitObject::HasBoundingBox, ImplicitObjectType::HeightField)
	{
		TUniqueFunction<FReal(const uint16)> ConversionFunc = [](const uint16 InVal) -> FReal
		{
			return (FReal)((int32)InVal - 32768);
		};

		BuildGeomData<const uint16>(InHeights, InMaterialIndices, InNumRows, InNumCols, FVec3(1), MoveTemp(ConversionFunc), GeomData, LocalBounds);
		CalcBounds();
		SetScale(InScale);
	}

	void Chaos::FHeightField::EditHeights(TArrayView<const uint16> InHeights, int32 InBeginRow, int32 InBeginCol, int32 InNumRows, int32 InNumCols)
	{
		const int32 NumExpectedValues = InNumRows * InNumCols;
		const int32 EndRow = InBeginRow + InNumRows - 1;
		const int32 EndCol = InBeginCol + InNumCols - 1;

		if(ensure(InHeights.Num() == NumExpectedValues && InBeginRow >= 0 && InBeginCol >= 0 && EndRow < GeomData.NumRows && EndCol < GeomData.NumCols))
		{
			TUniqueFunction<FReal(const uint16)> ConversionFunc = [](const uint16 InVal) -> FReal
			{
				return (FReal)((int32)InVal - 32768);
			};

			EditGeomData<const uint16>(InHeights, InBeginRow, InBeginCol, InNumRows, InNumCols, MoveTemp(ConversionFunc), GeomData, LocalBounds);

			// Slow and dumb. TODO: Actually fix CellHeights inside EditGeomData.
			CalcBounds();
		}
	}

	void Chaos::FHeightField::EditHeights(TArrayView<FReal> InHeights, int32 InBeginRow, int32 InBeginCol, int32 InNumRows, int32 InNumCols)
	{
		const int32 NumExpectedValues = InNumRows * InNumCols;
		const int32 EndRow = InBeginRow + InNumRows - 1;
		const int32 EndCol = InBeginCol + InNumCols - 1;

		if(ensure(InHeights.Num() == NumExpectedValues && InBeginRow >= 0 && InBeginCol >= 0 && EndRow < GeomData.NumRows && EndCol < GeomData.NumCols))
		{
			TUniqueFunction<FReal(FReal)> ConversionFunc = [](const FReal InVal) -> FReal
			{
				return InVal;
			};

			EditGeomData<FReal>(InHeights, InBeginRow, InBeginCol, InNumRows, InNumCols, MoveTemp(ConversionFunc), GeomData, LocalBounds);

			// Slow and dumb. TODO: Actually fix CellHeights inside EditGeomData.
			CalcBounds();
		}
	}

	bool Chaos::FHeightField::GetCellBounds2D(const TVec2<int32> InCoord, FBounds2D& OutBounds, const FVec2& InInflate /*= {0}*/) const
	{
		if (FlatGrid.IsValid(InCoord))
		{
			OutBounds.Min = FVec2(static_cast<FReal>(InCoord[0]), static_cast<FReal>(InCoord[1]));
			OutBounds.Max = FVec2(static_cast<FReal>(InCoord[0] + 1), static_cast<FReal>(InCoord[1] + 1));
			OutBounds.Min -= InInflate;
			OutBounds.Max += InInflate;

			return true;
		}

		return false;
	}

	FReal Chaos::FHeightField::GetHeight(int32 InIndex) const
	{
		if (CHAOS_ENSURE(InIndex >= 0 && InIndex < GeomData.Heights.Num()))
		{
			return GeomData.GetPoint(InIndex).Z;
		}

		return TNumericLimits<FReal>::Max();
	}

	FReal Chaos::FHeightField::GetHeight(int32 InX, int32 InY) const
	{
		const int32 Index = InY * GeomData.NumCols + InX;
		return GetHeight(Index);
	}

	uint8 Chaos::FHeightField::GetMaterialIndex(int32 InIndex) const
	{
		if(CHAOS_ENSURE(InIndex >= 0 && InIndex < GeomData.MaterialIndices.Num()))
		{
			return GeomData.MaterialIndices[InIndex];
		}

		return TNumericLimits<uint8>::Max();
	}

	uint8 Chaos::FHeightField::GetMaterialIndex(int32 InX, int32 InY) const
	{
		const int32 Index = InY * (GeomData.NumCols - 1) + InX;
		return GetMaterialIndex(Index);
	}

	bool Chaos::FHeightField::IsHole(int32 InIndex) const
	{
		return GetMaterialIndex(InIndex) == TNumericLimits<uint8>::Max();
	}

	bool Chaos::FHeightField::IsHole(int32 InCellX, int32 InCellY) const
	{
		// Convert to single cell index
		const int32 Index = InCellY * (GeomData.NumCols - 1) + InCellX;
		return IsHole(Index);
	}

	struct FHeightNormalResult
	{
		Chaos::FReal Height = TNumericLimits<Chaos::FReal>::Max();
		Chaos::FVec3 Normal = Chaos::FVec3(0);
	};

	/**
	* GetHeightNormalAt always return a valid normal
	* if the point is outside of the grid, the edge is extended infinitely 
	**/
	template<bool bFillHeight, bool bFillNormal>
	FHeightNormalResult GetHeightNormalAt(const FVec2& InGridLocationLocal, const Chaos::FHeightField::FDataType& InGeomData, const TUniformGrid<Chaos::FReal, 2>& InGrid)
	{
		FHeightNormalResult Result;

		const FVec2 ClampedGridLocationLocal = InGrid.Clamp(InGridLocationLocal);
		
		TVec2<int32> CellCoord = InGrid.Cell(ClampedGridLocationLocal);
		CellCoord = InGrid.ClampIndex(CellCoord);

		const int32 SingleIndex = CellCoord[1] * (InGeomData.NumCols) + CellCoord[0];
		FVec3 Pts[4];
		InGeomData.GetPointsScaled(SingleIndex, Pts);

		const Chaos::FReal FractionX = FMath::Frac(ClampedGridLocationLocal[0]);
		const Chaos::FReal FractionY = FMath::Frac(ClampedGridLocationLocal[1]);

		if(FractionX > FractionY)
		{
			if(bFillHeight)
			{
				const static FVector Tri[3] = {FVector(0.0f, 0.0f, 0.0f), FVector(1.0f, 1.0f, 0.0f), FVector(0.0f, 1.0f, 0.0f)};
				FVector Bary = FMath::GetBaryCentric2D ({ FractionX, FractionY, 0.0f }, Tri[0], Tri[1], Tri[2]);

				Result.Height = Pts[0].Z * Bary[0] + Pts[3].Z * Bary[1] + Pts[2].Z * Bary[2];
			}

			if(bFillNormal)
			{
				const FVec3 AB = Pts[3] - Pts[0];
				const FVec3 AC = Pts[2] - Pts[0];
				Result.Normal = FVec3::CrossProduct(AB, AC).GetUnsafeNormal();
			}
		}
		else
		{
			if(bFillHeight)
			{
				const static FVector Tri[3] = {FVector(0.0f, 0.0f, 0.0f), FVector(1.0f, 0.0f, 0.0f), FVector(1.0f, 1.0f, 0.0f)};
				FVector Bary = FMath::GetBaryCentric2D({ FractionX, FractionY, 0.0f }, Tri[0], Tri[1], Tri[2]);

				Result.Height = Pts[0].Z * Bary[0] + Pts[1].Z * Bary[1] + Pts[3].Z * Bary[2];
			}

			if(bFillNormal)
			{
				const FVec3 AB = Pts[1] - Pts[0];
				const FVec3 AC = Pts[3] - Pts[0];
				Result.Normal = FVec3::CrossProduct(AB, AC).GetUnsafeNormal();
			}
		}

		return Result;
	}

	Chaos::FVec3 Chaos::FHeightField::GetNormalAt(const TVec2<FReal>& InGridLocationLocal) const
	{
		return GetHeightNormalAt<false, true>(InGridLocationLocal, GeomData, FlatGrid).Normal;

	}

	FReal Chaos::FHeightField::GetHeightAt(const TVec2<FReal>& InGridLocationLocal) const
	{
		return GetHeightNormalAt<true, false>(InGridLocationLocal, GeomData, FlatGrid).Height;
	}

	bool Chaos::FHeightField::GetCellBounds3D(const TVec2<int32> InCoord, FVec3& OutMin, FVec3& OutMax, const FVec3& InInflate /*= {0}*/) const
	{
		if (FlatGrid.IsValid(InCoord))
		{
			//todo: just compute max height, avoid extra work since this is called from tight loop
			FVec3 Min,Max;
			CalcCellBounds3D(InCoord,Min,Max);

			OutMin = FVec3(static_cast<FReal>(InCoord[0]), static_cast<FReal>(InCoord[1]), GeomData.GetMinHeight());
			OutMax = FVec3(static_cast<FReal>(InCoord[0] + 1), static_cast<FReal>(InCoord[1] + 1), Max[2]);
			OutMin = OutMin - InInflate;
			OutMax = OutMax + InInflate;

			return true;
		}

		return false;
	}

	bool Chaos::FHeightField::GetCellBounds2DScaled(const TVec2<int32> InCoord, FBounds2D& OutBounds, const FVec2& InInflate /*= {0}*/) const
	{
		if (FlatGrid.IsValid(InCoord))
		{
			OutBounds.Min = FVec2(static_cast<FReal>(InCoord[0]), static_cast<FReal>(InCoord[1]));
			OutBounds.Max = FVec2(static_cast<FReal>(InCoord[0] + 1), static_cast<FReal>(InCoord[1] + 1));
			OutBounds.Min -= InInflate;
			OutBounds.Max += InInflate;
			const FVec2 Scale2D = FVec2(GeomData.Scale[0], GeomData.Scale[1]);
			OutBounds.Min *= Scale2D;
			OutBounds.Max *= Scale2D;
			return true;
		}

		return false;
	}

	bool Chaos::FHeightField::GetCellBounds3DScaled(const TVec2<int32> InCoord, FVec3& OutMin, FVec3& OutMax, const FVec3& InInflate /*= {0}*/) const
	{
		if (FlatGrid.IsValid(InCoord))
		{
			//todo: just compute max height, avoid extra work since this is called from tight loop
			FVec3 Min,Max;
			CalcCellBounds3D(InCoord,Min,Max);

			OutMin = FVec3(static_cast<FReal>(InCoord[0]), static_cast<FReal>(InCoord[1]), GeomData.GetMinHeight());
			OutMax = FVec3(static_cast<FReal>(InCoord[0] + 1), static_cast<FReal>(InCoord[1] + 1), Max[2]);

			const FAABB3 CellBoundScaled = FAABB3::FromPoints(OutMin * GeomData.Scale, OutMax * GeomData.Scale);

			OutMin = CellBoundScaled.Min() - InInflate;
			OutMax = CellBoundScaled.Max() + InInflate;
			return true;
		}

		return false;
	}

	bool Chaos::FHeightField::CalcCellBounds3D(const TVec2<int32> InCoord, FVec3& OutMin, FVec3& OutMax, const FVec3& InInflate /*= {0}*/) const
	{
		if(FlatGrid.IsValid(InCoord))
		{
			int32 Index = InCoord[1] * (GeomData.NumCols) + InCoord[0];
			FVec3 Points[4];
			GeomData.GetPoints(Index, Points);

			const FAABB3 CellBound = FAABB3::FromPoints(Points[0], Points[1], Points[2], Points[3]);

			OutMin = CellBound.Min();
			OutMax = CellBound.Max();
			OutMin -= InInflate;
			OutMax += InInflate;

			return true;
		}

		return false;
	}


	bool FHeightField::GridCast(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, FHeightfieldRaycastVisitor& Visitor) const
	{
		//Is this check needed?
		if(Length < 1e-4)
		{
			return false;
		}

		FReal CurrentLength = Length;
		FVec2 ClippedFlatRayStart;
		FVec2 ClippedFlatRayEnd;

		// Data for fast box cast
		FVec3 Min, Max, HitPoint;
		bool bParallel[3];
		FVec3 InvDir;

		FReal InvCurrentLength = 1 / CurrentLength;
		for(int Axis = 0; Axis < 3; ++Axis)
		{
			bParallel[Axis] = FMath::IsNearlyZero(Dir[Axis], (FReal)1.e-8);
			InvDir[Axis] = bParallel[Axis] ? 0 : 1 / Dir[Axis];
		}
		
		FReal RayEntryTime;
		FReal RayExitTime;
		if(CachedBounds.RaycastFast(StartPoint, Dir, InvDir, bParallel, Length, InvCurrentLength, RayEntryTime, RayExitTime))
		{
			CurrentLength = RayExitTime + SMALL_NUMBER; // to account for precision errors 
			FVec3 NextStart = StartPoint + (Dir * RayEntryTime);

			const FVec2 Scale2D(GeomData.Scale[0],GeomData.Scale[1]);
			TVec2<int32> CellIdx = FlatGrid.Cell(TVec2<int32>(static_cast<int32>(NextStart[0] / Scale2D[0]), static_cast<int32>(NextStart[1] / Scale2D[1])));
			const FReal ZDx = CachedBounds.Extents()[2];
			const FReal ZMidPoint = CachedBounds.Min()[2] + ZDx * 0.5f;
			const FVec3 ScaledDx(FlatGrid.Dx()[0] * Scale2D[0],FlatGrid.Dx()[1] * Scale2D[1],ZDx);
			const FVec2 ScaledDx2D(ScaledDx[0],ScaledDx[1]);
			const FVec2 ScaledMin = FlatGrid.MinCorner() * Scale2D;
			const FVec3 ScaleSign = GeomData.Scale.GetSignVector();

			//START
			do
			{
				if (FlatGrid.IsValid(CellIdx))
				{
					PHYSICS_CSV_CUSTOM_VERY_EXPENSIVE(PhysicsCounters, NumRayHeightfieldCellVisited, 1, ECsvCustomStatOp::Accumulate);
					// Test for the cell bounding box is done in the visitor at the same time as fetching the points for the triangles
					// this avoid fetching the points twice ( here and in the visitor ) 
					bool bContinue = Visitor.VisitRaycast(CellIdx[1] * (GeomData.NumCols - 1) + CellIdx[0], CurrentLength);
					if (!bContinue)
					{
						return false;
					}
				}

				//find next cell

				//We want to know which plane we used to cross into next cell
				const FVec2 ScaledCellCenter2D = ScaledMin + FVec2(static_cast<FReal>(CellIdx[0]) + 0.5f, static_cast<FReal>(CellIdx[1]) + 0.5f) * ScaledDx2D;
				const FVec3 ScaledCellCenter(ScaledCellCenter2D[0], ScaledCellCenter2D[1], ZMidPoint);

				FReal Times[3];
				FReal BestTime = CurrentLength;
				bool bTerminate = true;
				for(int Axis = 0; Axis < 3; ++Axis)
				{
					if(!bParallel[Axis])
					{
						const FReal CrossPoint = (Dir[Axis] * ScaleSign[Axis]) > 0 ? ScaledCellCenter[Axis] + ScaledDx[Axis] / 2 : ScaledCellCenter[Axis] - ScaledDx[Axis] / 2;
						const FReal Distance = CrossPoint - NextStart[Axis];	//note: CellCenter already has /2, we probably want to use the corner instead
						const FReal Time = Distance * InvDir[Axis];
						Times[Axis] = Time;
						if(Time < BestTime)
						{
							bTerminate = false;	//found at least one plane to pass through
							BestTime = Time;
						}
					} else
					{
						Times[Axis] = TNumericLimits<FReal>::Max();
					}
				}

				if(bTerminate)
				{
					return false;
				}

				const TVec2<int32> PrevIdx = CellIdx;

				for(int Axis = 0; Axis < 2; ++Axis)
				{
					CellIdx[Axis] += (Times[Axis] <= BestTime) ? ((Dir[Axis] * ScaleSign[Axis]) > 0 ? 1 : -1) : 0;
					if(CellIdx[Axis] < 0 || CellIdx[Axis] >= FlatGrid.Counts()[Axis])
					{
						return false;
					}
				}

				if(PrevIdx == CellIdx)
				{
					//crossed on z plane which means no longer in heightfield bounds
					return false;
				}

				NextStart = NextStart + Dir * BestTime;
			} while(true);
		}

		return false;
	}

	struct F2DGridSet
	{
		F2DGridSet(TVec2<int32> Size)
			: NumX(Size[0])
			, NumY(Size[1])
		{
			int32 BitsNeeded = NumX * NumY;
			DataSize = 1 + (BitsNeeded) / 8;
			Data = MakeUnique<uint8[]>(DataSize);
			FMemory::Memzero(Data.Get(), DataSize);
		}

		bool Contains(const TVec2<int32>& Coordinate)
		{
			int32 Idx = Coordinate[1] * NumX + Coordinate[0];
			int32 ByteIdx = Idx / 8;
			int32 BitIdx = Idx % 8;
			check(ByteIdx >= 0 && ByteIdx < DataSize);
			bool bContains = (Data[ByteIdx] >> BitIdx) & 0x1;
			return bContains;
		}

		void Add(const TVec2<int32>& Coordinate)
		{
			int32 Idx = Coordinate[1] * NumX + Coordinate[0];
			int32 ByteIdx = Idx / 8;
			int32 BitIdx = Idx % 8;
			uint8 Mask = static_cast<uint8>(1 << BitIdx);
			check(ByteIdx >= 0 && ByteIdx < DataSize);
			Data[ByteIdx] |= Mask;
		}

	private:
		int32 NumX;
		int32 NumY;
		TUniquePtr<uint8[]> Data;
		int32 DataSize;
	};

	template<typename SQVisitor>
	bool FHeightField::GridSweep(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, const FVec3 InHalfExtents, SQVisitor& Visitor) const
	{
		// Take the 2D portion of the extent and inflate the grid query bounds for checking against the 2D height field grid
		// to account for the thickness when querying outside but near to the edge of the grid.
		const FVec2 Inflation2D(InHalfExtents[0], InHalfExtents[1]);
		
		FBounds2D InflatedBounds = GetFlatBounds();
		InflatedBounds.Min -= Inflation2D;
		InflatedBounds.Max += Inflation2D;

		// Full extents required when querying against the actual cell geometry bounds
		const FVec3 HalfExtents3D(InHalfExtents[0], InHalfExtents[1], InHalfExtents[2]);

		const FVec3 EndPoint = StartPoint + Dir * Length;
		const FVec2 Start2D(StartPoint[0], StartPoint[1]);
		const FVec2 End2D(EndPoint[0], EndPoint[1]);
		const FVec2 Scale2D(GeomData.Scale[0], GeomData.Scale[1]);
		
		FVec2 ClippedStart;
		FVec2 ClippedEnd;

		if(InflatedBounds.ClipLine(StartPoint, StartPoint + Dir * Length, ClippedStart, ClippedEnd))
		{
			// Rasterize the line over the grid
			TVec2<int32> StartCell = FlatGrid.Cell(ClippedStart / Scale2D);
			TVec2<int32> EndCell = FlatGrid.Cell(ClippedEnd / Scale2D);

			const int32 DeltaX = FMath::Abs(EndCell[0] - StartCell[0]);
			const int32 DeltaY = -FMath::Abs(EndCell[1] - StartCell[1]);
			const bool bSameCell = DeltaX == 0 && DeltaY == 0;

			const int32 DirX = StartCell[0] < EndCell[0] ? 1 : -1;
			const int32 DirY = StartCell[1] < EndCell[1] ? 1 : -1;
			int32 Error = DeltaX + DeltaY;
			const TVec2<int32> ThickenDir = FMath::Abs(DeltaX) > FMath::Abs(DeltaY) ? TVec2<int32>(0, 1) : TVec2<int32>(1, 0);

			struct FQueueEntry
			{
				TVec2<int32> Index;
				FReal ToI;
			};

			// Tracking data for cells to query (similar to bounding volume approach)
			F2DGridSet Seen(FlatGrid.Counts());
			TArray<FQueueEntry> Queue;
			Queue.Add({StartCell, -1});
			Seen.Add(StartCell);

			// Data for fast box cast
			FVec3 Min, Max, HitPoint;
			FReal ToI;
			bool bParallel[3];
			FVec3 InvDir;

			FReal CurrentLength = Length;
			FReal InvCurrentLength = 1 / CurrentLength;

			for(int Axis = 0; Axis < 3; ++Axis)
			{
				bParallel[Axis] = FMath::IsNearlyZero(Dir[Axis], (FReal)1.e-8);
				InvDir[Axis] = bParallel[Axis] ? 0 : 1 / Dir[Axis];
			}

			int32 QueueIndex = 0;
			while(QueueIndex < Queue.Num())
			{
				// Copy so we don't lost the entry through reallocs
				FQueueEntry CellCoord = Queue[QueueIndex++];

				if(CellCoord.ToI > CurrentLength)
				{
					continue;
				}

				if(bSameCell)
				{
					// Test the current cell
					bool bContinue = Visitor.VisitSweep(CellCoord.Index[1] * (GeomData.NumCols - 1) + CellCoord.Index[0], CurrentLength);
					
					if(!bContinue)
					{
						return true;
					}

					// Flatten out a double loop and skip the centre cell
					// to search cells immediately adjacent to the current cell
					static const TVec2<int32> Neighbors[] =
					{
						{-1, -1}, {0, -1}, {1, -1},
						{-1, 0}, {1, 0},
						{-1, 1}, {0, 1}, {1, 1}
					};

					for(const TVec2<int32>& Neighbor : Neighbors)
					{
						TVec2<int32> NeighCoord = CellCoord.Index + Neighbor;

						FBounds2D CellBounds;
						if(GetCellBounds3DScaled(NeighCoord, Min, Max, HalfExtents3D) && !Seen.Contains(NeighCoord))
						{
							if(FAABB3(Min,Max).RaycastFast(StartPoint, Dir, InvDir, bParallel, CurrentLength, InvCurrentLength, ToI, HitPoint))
							{
								Seen.Add(NeighCoord);
								Queue.Add({NeighCoord, ToI});
							}
						}
					}
				}
				else
				{
					// Expand each cell along the thicken direction
					// Although the line should minimally thicken around the perpendicular to the line direction
					// it's cheaper to just expand in the cardinal opposite the current major direction. We end up
					// doing a broad test on more cells but avoid having to run many rasterize/walk steps for each
					// perpendicular step.
					auto Expand = [&](const TVec2<int32>& Begin, const TVec2<int32>& Direction, const int32 NumSteps)
					{
						TVec2<int32> CurrentCell = Begin;

						for(int32 CurrStep = 0; CurrStep < NumSteps; ++CurrStep)
						{
							CurrentCell += Direction;

							// Fail if we leave the grid
							if(CurrentCell[0] < 0 || CurrentCell[1] < 0 || CurrentCell[0] > FlatGrid.Counts()[0] - 1 || CurrentCell[1] > FlatGrid.Counts()[1] - 1)
							{
								break;
							}

							// No intersections here. We set the ToI to zero to cause an intersection check to happen
							// without any expansion when we reach this cell in the queue.
							if(!Seen.Contains(CurrentCell))
							{
								Seen.Add(CurrentCell);
								Queue.Add({CurrentCell, 0});
							}
						}
					};

					// Check the current cell, if we hit its 3D bound we can move on to narrow phase
					const TVec2<int32> Coord = CellCoord.Index;
					if (FlatGrid.IsValid(Coord))
					{
						bool bContinue = Visitor.VisitSweep(CellCoord.Index[1] * (GeomData.NumCols - 1) + CellCoord.Index[0], CurrentLength);
						if (!bContinue)
						{
							return true;
						}
					}

					// This time isn't used to reject things for this method but to flag cells that should be expanded
					if(CellCoord.ToI < 0)
					{
						// Perform expansion for thickness
						int32 ExpandAxis;
						if(ThickenDir[0] == 0)
						{
							ExpandAxis = 1;
						}
						else
						{
							ExpandAxis = 0;
						}
						FReal ExpandSize = HalfExtents3D[ExpandAxis];
						int32 Steps = FMath::TruncToInt32(FMath::RoundFromZero(ExpandSize / FMath::Abs(GeomData.Scale[ExpandAxis])));

						Expand(Coord, ThickenDir, Steps);
						Expand(Coord, -ThickenDir, Steps);

						// Walk the line and add to the queue
						if(StartCell != EndCell)
						{
							const int32 DoubleError = Error * 2;

							if(DoubleError >= DeltaY)
							{
								Error += DeltaY;
								StartCell[0] += DirX;
							}

							if(DoubleError <= DeltaX)
							{
								Error += DeltaX;
								StartCell[1] += DirY;
							}

							if(!Seen.Contains(StartCell))
							{
								Seen.Add(StartCell);
								Queue.Add({StartCell, -1});
							}
						}
					}
				}
			}
		}

		return false;
	}

	bool FHeightField::Raycast(const FVec3& StartPoint, const FVec3& Dir, const FReal Length, const FReal Thickness, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex) const
	{
		OutFaceIndex = INDEX_NONE;

		FHeightfieldRaycastVisitor Visitor(&GeomData, StartPoint, Dir, Thickness);

		if(Thickness > 0)
		{
			GridSweep(StartPoint, Dir, Length, FVec3(Thickness), Visitor);
		}
		else
		{
			GridCast(StartPoint, Dir, Length, Visitor);
		}

		if(Visitor.OutTime <= Length)
		{
			OutTime = Visitor.OutTime;
			OutPosition = Visitor.OutPosition;
			OutNormal = Visitor.OutNormal;
			OutFaceIndex = Visitor.OutFaceIndex;
			return true;
		}

		return false;
	}

	bool Chaos::FHeightField::GetGridIntersections(FBounds2D InFlatBounds, TArray<TVec2<int32>>& OutIntersections) const
	{
		OutIntersections.Reset();

		const FBounds2D FlatBounds = GetFlatBounds();
		FVec2 Scale2D(GeomData.Scale[0], GeomData.Scale[1]);

		InFlatBounds = FBounds2D::FromPoints(FlatBounds.Clamp(InFlatBounds.Min) / Scale2D, FlatBounds.Clamp(InFlatBounds.Max) / Scale2D);

		TVec2<int32> MinCell = FlatGrid.Cell(InFlatBounds.Min);
		TVec2<int32> MaxCell = FlatGrid.Cell(InFlatBounds.Max);

		// We want to capture the first cell (delta == 0) as well
		const int32 NumX = MaxCell[0] - MinCell[0] + 1;
		const int32 NumY = MaxCell[1] - MinCell[1] + 1;

		OutIntersections.Reserve(NumX * NumY);
		for(int32 CurrX = 0; CurrX < NumX; ++CurrX)
		{
			for(int32 CurrY = 0; CurrY < NumY; ++CurrY)
			{
				const TVec2<int32> Cell(MinCell[0] + CurrX, MinCell[1] + CurrY);
				check(FlatGrid.IsValid(Cell));
				OutIntersections.Add(Cell);
			}
		}

		return OutIntersections.Num() > 0;
	}

	typename Chaos::FHeightField::FBounds2D Chaos::FHeightField::GetFlatBounds() const
	{
		FBounds2D Result;
		Result.Min = FVec2(CachedBounds.Min()[0], CachedBounds.Min()[1]);
		Result.Max = FVec2(CachedBounds.Max()[0], CachedBounds.Max()[1]);
		return Result;
	}

	bool FHeightField::Overlap(const FVec3& Point, const FReal Thickness) const
	{
		auto OverlapTriangle = [&](const FVec3& A, const FVec3& B, const FVec3& C) -> bool
		{
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;
			FVec3 Normal = FVec3::CrossProduct(AB, AC);
			const FReal NormalLength = Normal.SafeNormalize();

			if(!ensure(NormalLength > KINDA_SMALL_NUMBER))
			{
				return false;
			}

			const TPlane<FReal, 3> TriPlane{A, Normal};
			const FVec3 ClosestPointOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, Point);
			const FReal Distance2 = (ClosestPointOnTri - Point).SizeSquared();

			if(Distance2 <= Thickness * Thickness)	//This really only has a hope in working if thickness is > 0
			{
				return true;
			}

			return false;
		};

		FAABB3 QueryBounds(Point, Point);
		QueryBounds.Thicken(Thickness);

		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = FVec2(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = FVec2(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVec2<int32>> Intersections;
		FVec3 Points[4];

		GetGridIntersections(FlatQueryBounds, Intersections);

		for(const TVec2<int32>& Cell : Intersections)
		{
			const int32 SingleIndex = Cell[1] * (GeomData.NumCols) + Cell[0];
			GeomData.GetPointsScaled(SingleIndex, Points);

			if(OverlapTriangle(Points[0], Points[1], Points[3]))
			{
				return true;
			}

			if(OverlapTriangle(Points[0], Points[3], Points[2]))
			{
				return true;
			}
		}
		
		return false;
	}

	template <typename GeomType>
	bool FHeightField::ContactManifoldNonPlanarConvexImp(const GeomType& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		auto OverlapTriangle = [&](const FVec3& A, const FVec3& B, const FVec3& C,
			FPBDCollisionConstraint& Constraint)
		{
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;

			const FVec3 Offset = FVec3::CrossProduct(AB, AC);

			FTriangle TriangleConvex(A, B, C);
			Collisions::ConstructConvexConvexOneShotManifold(QueryGeom, QueryTM, TriangleConvex, FRigidTransform3::Identity, 0, Constraint);
		};

		auto InsertSorted = [&](const FContactPoint& ContactPoint)
		{
			int32 PointIndex = 0;
			bool done = false;
			const FReal SamePointErrorMarginSqr = 0.01f;

			int32 ContactPointsNum = ContactPoints.Num();
			for (; PointIndex < ContactPointsNum; PointIndex++)
			{
				FVec3 DiffVector = ContactPoint.ShapeContactPoints[1] - ContactPoints[PointIndex].ShapeContactPoints[1];
				// Check if point is the same (or close)
				if (DiffVector.SizeSquared() < SamePointErrorMarginSqr)
				{
					done = true;
					break;
				}

				if (ContactPoint.Phi < ContactPoints[PointIndex].Phi)
				{
					ContactPoints.Insert(ContactPoint, PointIndex);
					done = true;
					break;
				}
			}

			if (!done)
			{
				ContactPoints.Add(ContactPoint);
			}
		};

		bool bResult = false;
		FAABB3 QueryBounds = QueryGeom.BoundingBox();
		QueryBounds.Thicken(Thickness);
		QueryBounds = QueryBounds.TransformedAABB(QueryTM);

		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = FVec2(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = FVec2(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVec2<int32>> Intersections;
		FVec3 Points[4];

		GetGridIntersections(FlatQueryBounds, Intersections);

		FReal MinContactPhi = FLT_MAX;
		FVec3 LocalContactLocation, LocalContactNormal;
		for (const TVec2<int32>& Cell : Intersections)
		{
			const int32 SingleIndex = Cell[1] * GeomData.NumCols + Cell[0];
			const int32 CellIndex = Cell[1] * (GeomData.NumCols - 1) + Cell[0];

			// Check for holes and skip checking if we'll never collide
			if (GeomData.MaterialIndices.IsValidIndex(CellIndex) && GeomData.MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max())
			{
				continue;
			}

			// @todo(chaos): we should not be creating constraints just for collecting contacts...
			FPBDCollisionConstraint Constraint = FPBDCollisionConstraint::MakeTriangle(&QueryGeom);

			// The triangle is solid so proceed to test it
			FAABB3 CellBounds;
			GeomData.GetPointsAndBoundsScaled(SingleIndex, Points, CellBounds);
			if (CellBounds.Intersects(QueryBounds))
			{
				// First Triangle
				{
					Constraint.ResetManifold();
					Constraint.GetGJKWarmStartData().Reset();
					OverlapTriangle(Points[0], Points[1], Points[3], Constraint);
					for (FManifoldPoint& ManifoldPoint : Constraint.GetManifoldPoints())
					{
						ManifoldPoint.ContactPoint.FaceIndex = CellIndex * 2;
						InsertSorted(ManifoldPoint.ContactPoint);
					}
				}
				// Second Triangle
				{
					Constraint.ResetManifold();
					Constraint.GetGJKWarmStartData().Reset();
					OverlapTriangle(Points[0], Points[3], Points[2], Constraint);
					for (FManifoldPoint& ManifoldPoint : Constraint.GetManifoldPoints())
					{
						ManifoldPoint.ContactPoint.FaceIndex = CellIndex * 2 + 1;
						InsertSorted(ManifoldPoint.ContactPoint);
					}
				}
			}
		}

		// Remove edge contacts that are "hidden" by face contacts
		// EdgePruneDistance should be some fraction of the convex margin...
		const FReal EdgePruneDistance = Chaos_Collision_EdgePrunePlaneDistance;
		Collisions::PruneEdgeContactPointsOrdered(ContactPoints, EdgePruneDistance);

		// Remove all points (except for the deepest one, and ones with phis similar to it)
		const FReal CullMargin = 0.1f;
		int32 NewContactPointCount = ContactPoints.Num() > 0 ? 1 : 0;
		for (int32 Index = 1; Index < ContactPoints.Num(); Index++)
		{
			if (ContactPoints[Index].Phi < 0 || ContactPoints[Index].Phi - ContactPoints[0].Phi < CullMargin)
			{
				NewContactPointCount++;
			}
			else
			{
				break;
			}
		}
		ContactPoints.SetNum(NewContactPointCount, false);

		// Reduce to only 4 contact points from here
		Collisions::ReduceManifoldContactPointsTriangeMesh(ContactPoints);

		return true;
	}

	template <typename GeomType>
	bool FHeightField::ContactManifoldPlanarConvexImp(const GeomType& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		auto OverlapTriangle = [&](const FVec3& A, const FVec3& B, const FVec3& C, TCArray<FContactPoint, 4>& TriangleContactPoints)
		{
			// Create triangle in query space
			FTriangle TriangleConvex(
				QueryTM.InverseTransformPositionNoScale(A), 
				QueryTM.InverseTransformPositionNoScale(B), 
				QueryTM.InverseTransformPositionNoScale(C));

			Collisions::ConstructPlanarConvexTriangleOneShotManifold(QueryGeom, TriangleConvex, Thickness, TriangleContactPoints);

			// Convert back to shape-local space
			for (FContactPoint& ContactPoint : TriangleContactPoints)
			{
				ContactPoint.ShapeContactPoints[1] = QueryTM.TransformPositionNoScale(ContactPoint.ShapeContactPoints[1]);
				ContactPoint.ShapeContactNormal = QueryTM.TransformVectorNoScale(ContactPoint.ShapeContactNormal);
			}
		};

		auto InsertSorted = [&](const FContactPoint& ContactPoint)
		{
			int32 PointIndex = 0;
			bool done = false;
			const FReal SamePointErrorMarginSqr = 0.01f;
			
			int32 ContactPointsNum = ContactPoints.Num();
			for( ; PointIndex < ContactPointsNum; PointIndex++ )
			{
				FVec3 DiffVector = ContactPoint.ShapeContactPoints[1] - ContactPoints[PointIndex].ShapeContactPoints[1];
				// Check if point is the same (or close)
				if (DiffVector.SizeSquared() < SamePointErrorMarginSqr)
				{
					done = true;
					break;
				}

				if (ContactPoint.Phi < ContactPoints[PointIndex].Phi)
				{
					ContactPoints.Insert(ContactPoint, PointIndex);
					done = true;
					break;
				}
			}

			if (!done)
			{
				ContactPoints.Add(ContactPoint);
			}
		};		
		
		bool bResult = false;
		FAABB3 QueryBounds = QueryGeom.BoundingBox();
		QueryBounds.Thicken(Thickness);
		QueryBounds = QueryBounds.TransformedAABB(QueryTM);

		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = FVec2(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = FVec2(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVec2<int32>> Intersections;
		FVec3 Points[4];

		GetGridIntersections(FlatQueryBounds, Intersections);

		FReal MinContactPhi = FLT_MAX;
		FVec3 LocalContactLocation, LocalContactNormal;
		TCArray<FContactPoint, 4> TriangleContactPoints;

		for (const TVec2<int32>& Cell : Intersections)
		{
			const int32 SingleIndex = Cell[1] * GeomData.NumCols + Cell[0];
			const int32 CellIndex = Cell[1] * (GeomData.NumCols - 1) + Cell[0];

			// Check for holes and skip checking if we'll never collide
			if (GeomData.MaterialIndices.IsValidIndex(CellIndex) && GeomData.MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max())
			{
				continue;
			}

			// The triangle is solid so proceed to test it
			FAABB3 CellBounds;
			GeomData.GetPointsAndBoundsScaled(SingleIndex, Points, CellBounds);
			if (CellBounds.Intersects(QueryBounds))
			{
				// First Triangle
				{
					TriangleContactPoints.Reset();
					OverlapTriangle(Points[0], Points[1], Points[3], TriangleContactPoints);
					for (FContactPoint& ContactPoint : TriangleContactPoints)
					{
						ContactPoint.FaceIndex = CellIndex * 2;
						InsertSorted(ContactPoint);
					}
				}
				// Second Triangle
				{
					TriangleContactPoints.Reset();
					OverlapTriangle(Points[0], Points[3], Points[2], TriangleContactPoints);
					for (FContactPoint& ContactPoint : TriangleContactPoints)
					{
						ContactPoint.FaceIndex = CellIndex * 2 + 1;
						InsertSorted(ContactPoint);
					}
				}
			}
		}

		// Remove edge contacts that are "hidden" by face contacts
		// EdgePruneDistance should be some fraction of the convex margin...
		const FReal EdgePruneDistance = Chaos_Collision_EdgePrunePlaneDistance;
		Collisions::PruneEdgeContactPointsOrdered(ContactPoints, EdgePruneDistance);

		// Remove all points (except for the deepest one, and ones with phis similar to it)
		const FReal CullMargin = 0.1f;
		int32 NewContactPointCount = ContactPoints.Num() > 0 ? 1 : 0;
		for (int32 Index = 1; Index < ContactPoints.Num(); Index++)
		{
			if (ContactPoints[Index].Phi < 0 || ContactPoints[Index].Phi - ContactPoints[0].Phi < CullMargin)
			{
				NewContactPointCount++;
			}
			else
			{
				break;
			}
		}
		ContactPoints.SetNum(NewContactPointCount, false);

		// Reduce to only 4 contact points from here
		Collisions::ReduceManifoldContactPointsTriangeMesh(ContactPoints);

		return true;
	}

	template <typename GeomType>
	bool FHeightField::GJKContactPointImp(const GeomType& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness,
		FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		auto OverlapTriangle = [&](const FVec3& A, const FVec3& B, const FVec3& C,
			FVec3& LocalContactLocation, FVec3& LocalContactNormal, FReal& LocalContactPhi) -> bool
		{
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;

			const FVec3 Offset = FVec3::CrossProduct(AB, AC);
			const FVec3 TriNormal = Offset.GetUnsafeNormal();
			FTriangle TriangleConvex(A, B, C);
			FTriangleRegister TriangleConvexReg(
				MakeVectorRegisterFloatFromDouble(MakeVectorRegister(A.X, A.Y, A.Z, 0.0f)),
				MakeVectorRegisterFloatFromDouble(MakeVectorRegister(B.X, B.Y, B.Z, 0.0f)),
				MakeVectorRegisterFloatFromDouble(MakeVectorRegister(C.X, C.Y, C.Z, 0.0f)));


			FReal Penetration;
			FVec3 ClosestA, ClosestB, Normal;

			auto SweepAgainstTriangle = [&]() -> bool
			{
				//
				// BUG: This does not detect collisions when we specify a cull distance. It is as if
				// Thickness is always zero...
				//

				const FAABB3 Bounds = QueryGeom.BoundingBox();
				const FReal ApproximateSizeOfObject = Bounds.Extents()[Bounds.LargestAxis()];
				const FReal ApproximateDistToObject = FVec3::DistSquared(QueryTM.GetLocation(), A);
				const FReal SweepLength = ApproximateSizeOfObject + ApproximateDistToObject;
				const FRigidTransform3 QueryStartTM(QueryTM.GetLocation() + TriNormal * SweepLength, QueryTM.GetRotation());
				if (GJKRaycast2(TriangleConvexReg, QueryGeom, QueryStartTM, -TriNormal, SweepLength, Penetration, ClosestB, Normal, (FReal)0., true))
				{
					LocalContactLocation = ClosestB;
					LocalContactNormal = TriNormal;
					LocalContactPhi = Penetration - SweepLength;
					return true;
				}
				return false;
			};

			if (bOneSidedHeightField)
			{
				// HACK:
				// The regular penetration calculation vs a triangle may result in inward facing normals.
				// To protect against this, we sweep against the triangle from a distance to ensure an outward
				// facing normal and MTD.

				if (bOneSidedHeightfieldAlwaysSweep)
				{
					return SweepAgainstTriangle();
				}
				else
				{
					int32 ClosestVertexIndexA, ClosestVertexIndexB;
					if (GJKPenetration(TriangleConvex, QueryGeom, QueryTM, Penetration, ClosestA, ClosestB, Normal, ClosestVertexIndexA, ClosestVertexIndexB, (FReal)0))
					{
						if (FVec3::DotProduct(TriNormal, Normal) < 0)
						{
							return SweepAgainstTriangle();
						}
						LocalContactLocation = ClosestB;
						LocalContactNormal = Normal;
						LocalContactPhi = -Penetration;
						return true;
					}
				}
				
			}
			else
			{
				int32 ClosestVertexIndexA, ClosestVertexIndexB;
				if (GJKPenetration(TriangleConvex, QueryGeom, QueryTM, Penetration, ClosestA, ClosestB, Normal, ClosestVertexIndexA, ClosestVertexIndexB, (FReal)0))
				{
					LocalContactLocation = ClosestB;
					LocalContactNormal = Normal; 
					LocalContactPhi = -Penetration;
					return true;
				}
			}

			return false;
		};

		bool bResult = false;
		FAABB3 QueryBounds = QueryGeom.BoundingBox();
		QueryBounds.Thicken(Thickness);
		QueryBounds = QueryBounds.TransformedAABB(QueryTM);

		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = FVec2(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = FVec2(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVec2<int32>> Intersections;
		FVec3 Points[4];

		GetGridIntersections(FlatQueryBounds, Intersections);

		FReal LocalContactPhi = FLT_MAX;
		FVec3 LocalContactLocation, LocalContactNormal;
		for (const TVec2<int32>& Cell : Intersections)
		{
			const int32 SingleIndex = Cell[1] * GeomData.NumCols + Cell[0];
			const int32 CellIndex = Cell[1] * (GeomData.NumCols - 1) + Cell[0];
			
			// Check for holes and skip checking if we'll never collide
			if(GeomData.MaterialIndices.IsValidIndex(CellIndex) && GeomData.MaterialIndices[CellIndex] == TNumericLimits<uint8>::Max())
			{
				continue;
			}

			// The triangle is solid so proceed to test it
			GeomData.GetPointsScaled(SingleIndex, Points);

			if (OverlapTriangle(Points[0], Points[1], Points[3], LocalContactLocation, LocalContactNormal, LocalContactPhi))
			{
				if (LocalContactPhi < ContactPhi)
				{
					ContactPhi = LocalContactPhi;
					ContactLocation = LocalContactLocation;
					ContactNormal = LocalContactNormal;
				}
			}

			if (OverlapTriangle(Points[0], Points[3], Points[2], LocalContactLocation, LocalContactNormal, LocalContactPhi))
			{
				if (LocalContactPhi < ContactPhi)
				{
					ContactPhi = LocalContactPhi;
					ContactLocation = LocalContactLocation;
					ContactNormal = LocalContactNormal;
				}
			}
		}

		if(ContactPhi < 0)
			return true;
		return false;
	}

	bool FHeightField::GJKContactPoint(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const TSphere<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const FCapsule& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const FConvex& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const TImplicitObjectScaled < TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const TImplicitObjectScaled < TSphere<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const TImplicitObjectScaled < FCapsule>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::GJKContactPoint(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FVec3& ContactLocation, FVec3& ContactNormal, FReal& ContactPhi) const
	{
		return GJKContactPointImp(QueryGeom, QueryTM, Thickness, ContactLocation, ContactNormal, ContactPhi);
	}

	bool FHeightField::ContactManifold(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	/*bool FHeightField::ContactManifold(const TSphere<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}*/

	bool FHeightField::ContactManifold(const FCapsule& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldNonPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	bool FHeightField::ContactManifold(const FConvex& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	bool FHeightField::ContactManifold(const TImplicitObjectScaled<TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	bool FHeightField::ContactManifold(const TImplicitObjectScaled<FCapsule>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldNonPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	bool FHeightField::ContactManifold(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, TArray<FContactPoint>& ContactPoints) const
	{
		return ContactManifoldPlanarConvexImp(QueryGeom, QueryTM, Thickness, ContactPoints);
	}

	template <typename QueryGeomType>
	bool FHeightField::OverlapGeomImp(const QueryGeomType& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		if (OutMTD)
		{
			OutMTD->Normal = FVec3(0);
			OutMTD->Penetration = TNumericLimits<FReal>::Lowest();
		}

		auto OverlapTriangleMTD = [&](const FVec3& A, const FVec3& B, const FVec3& C, FMTDInfo* InnerMTD) -> bool
		{
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;

			//It's most likely that the query object is in front of the triangle since queries tend to be on the outside.
			//However, maybe we should check if it's behind the triangle plane. Also, we should enforce this winding in some way
			const FVec3 Offset = FVec3::CrossProduct(AB, AC);

			FTriangle TriangleConvex(A, B, C);
			FVec3 TriangleNormal(0);
			FReal Penetration = 0;
			FVec3 ClosestA(0);
			FVec3 ClosestB(0);
			int32 ClosestVertexIndexA, ClosestVertexIndexB;
			if (GJKPenetration(TriangleConvex, QueryGeom, QueryTM, Penetration, ClosestA, ClosestB, TriangleNormal, ClosestVertexIndexA, ClosestVertexIndexB, Thickness))
			{
				// Use Deepest MTD.
				if (Penetration > InnerMTD->Penetration)
				{
					InnerMTD->Penetration = Penetration;
					InnerMTD->Normal = TriangleNormal;
				}
				return true;
			}

			return false;
		};

		auto OverlapTriangleNoMTD = [&](const FVec3& A, const FVec3& B, const FVec3& C) -> bool
		{
			// points are assumed to be in the same space as the overlap geometry
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;

			//It's most likely that the query object is in front of the triangle since queries tend to be on the outside.
			//However, maybe we should check if it's behind the triangle plane. Also, we should enforce this winding in some way
			const FVec3 Offset = FVec3::CrossProduct(AB, AC);

			FTriangle TriangleConvex(A, B, C);
			return GJKIntersectionSameSpace(TriangleConvex, QueryGeom, Thickness, Offset);
		};

		bool bResult = false;
		FAABB3 QueryBounds = QueryGeom.BoundingBox();
		QueryBounds.Thicken(Thickness);
		QueryBounds = QueryBounds.TransformedAABB(QueryTM);

		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = FVec2(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = FVec2(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVec2<int32>> Intersections;
		FVec3 Points[4];
		FAABB3 CellBounds;

		GetGridIntersections(FlatQueryBounds, Intersections);

		bool bOverlaps = false;
		if (OutMTD)
		{
			for (const TVec2<int32>& Cell : Intersections)
			{
				const int32 SingleIndex = Cell[1] * (GeomData.NumCols) + Cell[0];
				GeomData.GetPointsAndBoundsScaled(SingleIndex, Points, CellBounds);

				if(CellBounds.Intersects(QueryBounds))
				{
					bOverlaps |= OverlapTriangleMTD(Points[0], Points[1], Points[3], OutMTD);
					bOverlaps |= OverlapTriangleMTD(Points[0], Points[3], Points[2], OutMTD);
				}
			}
			return bOverlaps;
		}
		else
		{
			for (const TVec2<int32>& Cell : Intersections)
			{
				const int32 SingleIndex = Cell[1] * (GeomData.NumCols) + Cell[0];
				GeomData.GetPointsAndBoundsScaled(SingleIndex, Points, CellBounds);

				if (CellBounds.Intersects(QueryBounds))
				{
					// pre-transform the triangle in overlap geometry space
					Points[0] = QueryTM.InverseTransformPositionNoScale(Points[0]);
					Points[1] = QueryTM.InverseTransformPositionNoScale(Points[1]);
					Points[2] = QueryTM.InverseTransformPositionNoScale(Points[2]);
					Points[3] = QueryTM.InverseTransformPositionNoScale(Points[3]);

					if (OverlapTriangleNoMTD(Points[0], Points[1], Points[3]))
					{
						return true;
					}
					if (OverlapTriangleNoMTD(Points[0], Points[3], Points[2]))
					{
						return true;
					}
				}
			}
			return false;
		}
	}

	bool FHeightField::OverlapGeom(const TSphere<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const FCapsule& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const FConvex& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const TImplicitObjectScaled<TSphere<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const TImplicitObjectScaled<TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const TImplicitObjectScaled<FCapsule>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	bool FHeightField::OverlapGeom(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& QueryTM, const FReal Thickness, FMTDInfo* OutMTD) const
	{
		return OverlapGeomImp(QueryGeom, QueryTM, Thickness, OutMTD);
	}

	template <typename QueryGeomType>
	bool FHeightField::SweepGeomImp(const QueryGeomType& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, const FReal Thickness, bool bComputeMTD) const
	{
		bool bHit = false;
		THeightfieldSweepVisitor<QueryGeomType> SQVisitor(&GeomData, QueryGeom, StartTM, Dir, Thickness, bComputeMTD);
		const FAABB3 QueryBounds = QueryGeom.BoundingBox();
		const FVec3 StartPoint = StartTM.TransformPositionNoScale(QueryBounds.Center());

		const FVec3 Inflation3D = QueryBounds.Extents() * 0.5 + FVec3(Thickness);
		GridSweep(StartPoint, Dir, Length, FVec3(Inflation3D[0], Inflation3D[1], Inflation3D[2]), SQVisitor);

		if(SQVisitor.OutTime <= Length)
		{
			OutTime = SQVisitor.OutTime;
			OutPosition = SQVisitor.OutPosition;
			OutNormal = SQVisitor.OutNormal;
			OutFaceIndex = SQVisitor.OutFaceIndex;
			bHit = true;
		}

		return bHit;
	}

	bool FHeightField::SweepGeom(const TSphere<FReal, 3>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const TBox<FReal, 3>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const FCapsule& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const FConvex& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const TImplicitObjectScaled<TSphere<FReal, 3>>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const TImplicitObjectScaled<TBox<FReal, 3>>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const TImplicitObjectScaled<FCapsule>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	bool FHeightField::SweepGeom(const TImplicitObjectScaled<FConvex>& QueryGeom, const FRigidTransform3& StartTM, const FVec3& Dir, const FReal Length, FReal& OutTime, FVec3& OutPosition, FVec3& OutNormal, int32& OutFaceIndex, FVec3& OutFaceNormal, const FReal Thickness, bool bComputeMTD) const
	{
		return SweepGeomImp(QueryGeom, StartTM, Dir, Length, OutTime, OutPosition, OutNormal, OutFaceIndex, Thickness, bComputeMTD);
	}

	void FHeightField::VisitTriangles(const FAABB3& QueryBounds, const TFunction<void(const FTriangle& Triangle)>& Visitor) const
	{
		FBounds2D FlatQueryBounds;
		FlatQueryBounds.Min = TVector<FReal, 2>(QueryBounds.Min()[0], QueryBounds.Min()[1]);
		FlatQueryBounds.Max = TVector<FReal, 2>(QueryBounds.Max()[0], QueryBounds.Max()[1]);

		TArray<TVector<int32, 2>> Intersections;
		FVec3 Points[4];

		GetGridIntersections(FlatQueryBounds, Intersections);

		bool bOverlaps = false;
		for (const TVector<int32, 2>&Cell : Intersections)
		{
			const int32 SingleIndex = Cell[1] * (GeomData.NumCols) + Cell[0];
			GeomData.GetPointsScaled(SingleIndex, Points);

			Visitor(FTriangle(Points[0], Points[1], Points[3]));
			Visitor(FTriangle(Points[0], Points[3], Points[2]));
		}
	}

	int32 FHeightField::FindMostOpposingFace(const FVec3& Position, const FVec3& UnitDir, int32 HintFaceIndex, FReal SearchDist) const
	{
		const FReal SearchDist2 = SearchDist * SearchDist;

		FAABB3 QueryBounds(Position - FVec3(SearchDist), Position + FVec3(SearchDist));
		const FBounds2D FlatBounds(QueryBounds);
		TArray<TVec2<int32>> PotentialIntersections;
		GetGridIntersections(FlatBounds, PotentialIntersections);

		FReal MostOpposingDot = TNumericLimits<FReal>::Max();
		int32 MostOpposingFace = HintFaceIndex;

		auto CheckTriangle = [&](int32 FaceIndex, const FVec3& A, const FVec3& B, const FVec3& C)
		{
			const FVec3 AB = B - A;
			const FVec3 AC = C - A;
			FVec3 Normal = FVec3::CrossProduct(AB, AC);
			const FReal NormalLength = Normal.SafeNormalize();
			if(!ensure(NormalLength > KINDA_SMALL_NUMBER))
			{
				//hitting degenerate triangle - should be fixed before we get to this stage
				return;
			}

			const TPlane<FReal, 3> TriPlane{A, Normal};
			const FVec3 ClosestPointOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, Position);
			const FReal Distance2 = (ClosestPointOnTri - Position).SizeSquared();
			if(Distance2 < SearchDist2)
			{
				const FReal Dot = FVec3::DotProduct(Normal, UnitDir);
				if(Dot < MostOpposingDot)
				{
					MostOpposingDot = Dot;
					MostOpposingFace = FaceIndex;
				}
			}
		};

		ensure(PotentialIntersections.Num());
		for(const TVec2<int32>& CellCoord : PotentialIntersections)
		{
			const int32 CellIndex = CellCoord[1] * (GeomData.NumCols - 1) + CellCoord[0];
			const int32 SubX = CellIndex % (GeomData.NumCols - 1);
			const int32 SubY = CellIndex / (GeomData.NumCols - 1);

			const int32 FullIndex = CellIndex + SubY;

			FVec3 Points[4];

			GeomData.GetPointsScaled(FullIndex, Points);

			CheckTriangle(CellIndex * 2, Points[0], Points[1], Points[3]);
			CheckTriangle(CellIndex * 2 + 1, Points[0], Points[3], Points[2]);
		}

		return MostOpposingFace;
	}

	FHeightField::FClosestFaceData FHeightField::FindClosestFace(const FVec3& Position, FReal SearchDist) const
	{
		FClosestFaceData Result;

		auto TestInSphere = [](const FVec3& Origin, const FReal Radius2, const FVec3& TestPosition) -> bool
		{
			return (TestPosition - Origin).SizeSquared() <= Radius2;
		};

		const FReal SearchDist2 = SearchDist * SearchDist;

		FAABB3 QueryBounds(Position - FVec3(SearchDist), Position + FVec3(SearchDist));
		const FBounds2D FlatBounds(QueryBounds);
		TArray<TVec2<int32>> PotentialIntersections;
		GetGridIntersections(FlatBounds, PotentialIntersections);

		auto CheckTriangle = [&](int32 FaceIndex, const FVec3& A, const FVec3& B, const FVec3& C)
		{
			if(TestInSphere(Position, SearchDist2, A) || TestInSphere(Position, SearchDist2, B) || TestInSphere(Position, SearchDist2, C))
			{

				const FVec3 AB = B - A;
				const FVec3 AC = C - A;
				FVec3 Normal = FVec3::CrossProduct(AB, AC);

				const FReal NormalLength = Normal.SafeNormalize();
				if(!ensure(NormalLength > KINDA_SMALL_NUMBER))
				{
					//hitting degenerate triangle - should be fixed before we get to this stage
					return;
				}

				const TPlane<FReal, 3> TriPlane{ A, Normal };
				const FVec3 ClosestPointOnTri = FindClosestPointOnTriangle(TriPlane, A, B, C, Position);
				const FReal Distance2 = (ClosestPointOnTri - Position).SizeSquared();
				if(Distance2 < SearchDist2 && Distance2 < Result.DistanceToFaceSq)
				{
					Result.DistanceToFaceSq = Distance2;
					Result.FaceIndex = FaceIndex;

					const FVec3 ToTriangle = ClosestPointOnTri - Position;
					Result.bWasSampleBehind = FVec3::DotProduct(ToTriangle, Normal) > 0.0f;
				}
			}
		};

		for(const TVec2<int32> & CellCoord : PotentialIntersections)
		{
			const int32 CellIndex = CellCoord[1] * (GeomData.NumCols - 1) + CellCoord[0];
			const int32 SubX = CellIndex % (GeomData.NumCols - 1);
			const int32 SubY = CellIndex / (GeomData.NumCols - 1);

			const int32 FullIndex = CellIndex + SubY;

			FVec3 Points[4];

			GeomData.GetPointsScaled(FullIndex, Points);

			CheckTriangle(CellIndex * 2, Points[0], Points[1], Points[3]);
			CheckTriangle(CellIndex * 2 + 1, Points[0], Points[3], Points[2]);
		}

		return Result;
	}

	FVec3 FHeightField::FindGeometryOpposingNormal(const FVec3& DenormDir, int32 FaceIndex, const FVec3& OriginalNormal) const
	{
		if(ensure(FaceIndex != INDEX_NONE))
		{
			bool bSecondFace = FaceIndex % 2 != 0;

			int32 CellIndex = FaceIndex / 2;
			int32 CellY = CellIndex / (GeomData.NumCols - 1);

			FVec3 Points[4];

			GeomData.GetPointsScaled(CellIndex + CellY, Points);

			FVec3 A;
			FVec3 B;
			FVec3 C;

			if(bSecondFace)
			{
				A = Points[0];
				B = Points[3];
				C = Points[2];
			}
			else
			{
				A = Points[0];
				B = Points[1];
				C = Points[3];
			}

			const FVec3 AB = B - A;
			const FVec3 AC = C - A;
			const FVec3 ScaleSigns = GeomData.Scale.GetSignVector();
			const FReal ScaleInversion = ScaleSigns.X * ScaleSigns.Y * ScaleSigns.Z;
			FVec3 Normal = FVec3::CrossProduct(AB, AC) * ScaleInversion;
			const FReal Length = Normal.SafeNormalize();
			ensure(Length > 0);
			return Normal;
		}

		return FVec3(0, 0, 1);
	}

	void FHeightField::CalcBounds()
	{
		// Flatten out the Z axis
		FlattenedBounds = GetFlatBounds();

		BuildQueryData();

		// Cache per-cell bounds
		const int32 NumX = GeomData.NumCols - 1;
		const int32 NumY = GeomData.NumRows - 1;
	}

	void FHeightField::BuildQueryData()
	{
		// NumCols and NumRows are the actual heights, there are n-1 cells between those heights
		TVec2<int32> Cells(GeomData.NumCols - 1, GeomData.NumRows - 1);
		
		FVec2 MinCorner(0, 0);
		FVec2 MaxCorner(static_cast<FReal>(GeomData.NumCols - 1), static_cast<FReal>(GeomData.NumRows - 1));
		//MaxCorner *= {GeomData.Scale[0], GeomData.Scale[1]};

		FlatGrid = TUniformGrid<FReal, 2>(MinCorner, MaxCorner, Cells);
	}

	Chaos::FReal FHeightField::PhiWithNormal(const FVec3& x, FVec3& Normal) const
	{
		Chaos::FVec2 HeightField2DPosition(x.X / GeomData.Scale.X, x.Y / GeomData.Scale.Y);

		FHeightNormalResult HeightNormal = GetHeightNormalAt<true, true>(HeightField2DPosition, GeomData, FlatGrid);
		ensure(!HeightNormal.Normal.IsZero());

		// Assume the cell below us is the correct result initially
		const Chaos::FReal& HeightAtPoint = HeightNormal.Height;
		Normal = HeightNormal.Normal;
		FReal OutPhi = x.Z - HeightAtPoint;
		
		// Need to sample all cells in a Phi radius circle on the 2D grid. Large cliffs mean that the actual
		// Phi could be in an entirely different cell.
		const FClosestFaceData ClosestFace = FindClosestFace(x, FMath::Abs(OutPhi));

		if(ClosestFace.FaceIndex > INDEX_NONE)
		{
			Normal = FindGeometryOpposingNormal(FVec3(0.0f), ClosestFace.FaceIndex, FVec3(0.0f));
			OutPhi = ClosestFace.bWasSampleBehind ? -FMath::Sqrt(ClosestFace.DistanceToFaceSq) : FMath::Sqrt(ClosestFace.DistanceToFaceSq);
		}

		return OutPhi;
	}
}
