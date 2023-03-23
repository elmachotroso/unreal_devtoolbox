// Copyright Epic Games, Inc. All Rights Reserved.

#include "Chaos/Collision/ContactPointsMiscShapes.h"
#include "Chaos/CastingUtilities.h"
#include "Chaos/Collision/ContactPoint.h"
#include "Chaos/Collision/GJKContactPoint.h"
#include "Chaos/Convex.h"
#include "Chaos/Defines.h"
#include "Chaos/GJK.h"
#include "Chaos/GJKShape.h"
#include "Chaos/HeightField.h"
#include "Chaos/ImplicitObjectScaled.h"
#include "Chaos/Sphere.h"

DECLARE_CYCLE_STAT(TEXT("Collisions::CapsuleHeightFieldContactPoint"), STAT_Collisions_CapsuleHeightFieldContactPoint, STATGROUP_ChaosCollision);
DECLARE_CYCLE_STAT(TEXT("Collisions::CapsuleTriangleMeshContactPoint"), STAT_Collisions_CapsuleTriangleMeshContactPoint, STATGROUP_ChaosCollision);
DECLARE_CYCLE_STAT(TEXT("Collisions::CapsuleTriangleMeshSweptContactPoint"), STAT_Collisions_CapsuleTriangleMeshSweptContactPoint, STATGROUP_ChaosCollision);
DECLARE_CYCLE_STAT(TEXT("Collisions::ConvexHeightFieldContactPoint"), STAT_Collisions_ConvexHeightFieldContactPoint, STATGROUP_ChaosCollision);
DECLARE_CYCLE_STAT(TEXT("Collisions::ConvexTriangleMeshContactPoint"), STAT_Collisions_ConvexTriangleMeshContactPoint, STATGROUP_ChaosCollision);
DECLARE_CYCLE_STAT(TEXT("Collisions::ConvexTriangleMeshSweptContactPoint"), STAT_Collisions_ConvexTriangleMeshSweptContactPoint, STATGROUP_ChaosCollision);


extern int32 ConstraintsDetailedStats;

namespace Chaos
{

	template <typename GeometryB>
	FContactPoint GJKImplicitSweptContactPoint(const FImplicitObject& A, const FRigidTransform3& AStartTransform, const GeometryB& B, const FRigidTransform3& BTransform, const FVec3& Dir, const FReal Length, FReal& TOI)
	{
		FContactPoint Contact;
		const FRigidTransform3 AToBTM = AStartTransform.GetRelativeTransform(BTransform);
		const FVec3 LocalDir = BTransform.InverseTransformVectorNoScale(Dir);

		FReal OutTime = FLT_MAX;
		int32 FaceIndex = -1;
		FVec3 FaceNormal;
		FVec3 Location, Normal;

		Utilities::CastHelper(A, AStartTransform, [&](const auto& ADowncast, const FRigidTransform3& AFullTM)
			{
				// @todo(chaos): handle instances with margin
				if (B.SweepGeom(ADowncast, AToBTM, LocalDir, Length, OutTime, Location, Normal, FaceIndex, FaceNormal, 0.0f, true))
				{
					// @todo(chaos): margin
					Contact.ShapeContactPoints[0] = AToBTM.InverseTransformPosition(Location);
					Contact.ShapeContactPoints[1] = Location;
					Contact.ShapeContactNormal = Normal;
					const FVec3 ContactNormal = BTransform.TransformVectorNoScale(Normal);
					ComputeSweptContactPhiAndTOIHelper(ContactNormal, Dir, Length, OutTime, TOI, Contact.Phi);
				}
			});

		return Contact;
	}

	void ComputeSweptContactPhiAndTOIHelper(const FVec3& ContactNormal, const FVec3& Dir, const FReal& Length, const FReal& HitTime, FReal& OutTOI, FReal& OutPhi)
	{
		if (HitTime >= 0.0f)
		{
			// We subtract length to get the total penetration at at end of frame.
			// Project penetration vector onto geometry normal for correct phi.
			FReal Dot = FMath::Abs(FVec3::DotProduct(ContactNormal, -Dir));
			OutPhi = (HitTime - Length) * Dot;

			// TOI is between [0,1], used to compute particle position
			OutTOI = HitTime / Length;
		}
		else
		{
			// Initial overlap case:
			// TOI = 0 as we are overlapping at X.
			// OutTime is penetration value of MTD.
			OutPhi = HitTime;
			OutTOI = 0.0f;
		}
	}

	template <typename GeometryA, typename GeometryB>
	FContactPoint GJKImplicitContactPoint(const FImplicitObject& A, const FRigidTransform3& ATransform, const GeometryB& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		FContactPoint Contact;
		const FRigidTransform3 AToBTM = ATransform.GetRelativeTransform(BTransform);

		FReal ContactPhi = FLT_MAX;
		FVec3 Location, Normal;
		if (const TImplicitObjectScaled<GeometryA>* ScaledConvexImplicit = A.template GetObject<const TImplicitObjectScaled<GeometryA> >())
		{
			if (B.GJKContactPoint(*ScaledConvexImplicit, AToBTM, CullDistance, Location, Normal, ContactPhi))
			{
				Contact.ShapeContactPoints[0] = AToBTM.InverseTransformPosition(Location);
				Contact.ShapeContactPoints[1] = Location - ContactPhi * Normal;
				Contact.ShapeContactNormal = Normal;
				Contact.Phi = ContactPhi;
			}
		}
		else if (const TImplicitObjectInstanced<GeometryA>* InstancedConvexImplicit = A.template GetObject<const TImplicitObjectInstanced<GeometryA> >())
		{
			if (const GeometryA* InstancedInnerObject = static_cast<const GeometryA*>(InstancedConvexImplicit->GetInstancedObject()))
			{
				if (B.GJKContactPoint(*InstancedInnerObject, AToBTM, CullDistance, Location, Normal, ContactPhi))
				{
					Contact.ShapeContactPoints[0] = AToBTM.InverseTransformPosition(Location);
					Contact.ShapeContactPoints[1] = Location - ContactPhi * Normal;
					Contact.ShapeContactNormal = Normal;
					Contact.Phi = ContactPhi;
				}
			}
		}
		else if (const GeometryA* ConvexImplicit = A.template GetObject<const GeometryA>())
		{
			if (B.GJKContactPoint(*ConvexImplicit, AToBTM, CullDistance, Location, Normal, ContactPhi))
			{
				Contact.ShapeContactPoints[0] = AToBTM.InverseTransformPosition(Location);
				Contact.ShapeContactPoints[1] = Location - ContactPhi * Normal;
				Contact.ShapeContactNormal = Normal;
				Contact.Phi = ContactPhi;
			}
		}

		return Contact;
	}

	template <typename GeometryA, typename GeometryB>
	void GJKImplicitManifold(const FImplicitObject& A, const FRigidTransform3& ATransform, const GeometryB& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints)
	{
		const FRigidTransform3 AToBTM = ATransform.GetRelativeTransform(BTransform);

		if (const TImplicitObjectScaled<GeometryA>* ScaledConvexImplicit = A.template GetObject<const TImplicitObjectScaled<GeometryA> >())
		{
			B.ContactManifold(*ScaledConvexImplicit, AToBTM, CullDistance, ContactPoints);
		}
		else if (const TImplicitObjectInstanced<GeometryA>* InstancedConvexImplicit = A.template GetObject<const TImplicitObjectInstanced<GeometryA> >())
		{
			if (const GeometryA* InstancedInnerObject = static_cast<const GeometryA*>(InstancedConvexImplicit->GetInstancedObject()))
			{
				B.ContactManifold(*InstancedInnerObject, AToBTM, CullDistance, ContactPoints);
			}
		}
		else if (const GeometryA* ConvexImplicit = A.template GetObject<const GeometryA>())
		{
			B.ContactManifold(*ConvexImplicit, AToBTM, CullDistance, ContactPoints);
		}
	}

	// A is the implicit here, we want to return a contact point on B (trimesh)
	template <typename GeometryA>
	FContactPoint GJKImplicitScaledTriMeshSweptContactPoint(const FImplicitObject& A, const FRigidTransform3& AStartTransform, const TImplicitObjectScaled<FTriangleMeshImplicitObject>& B, const FRigidTransform3& BTransform, const FVec3& Dir, const FReal Length, FReal& TOI)
	{
		FContactPoint Contact;
		const FRigidTransform3 AToBTM = AStartTransform.GetRelativeTransform(BTransform);
		const FVec3 LocalDir = BTransform.InverseTransformVectorNoScale(Dir);

		if (!ensure(B.GetType() & ImplicitObjectType::TriangleMesh) || !ensure(!IsInstanced(B.GetType())))
		{
			return FContactPoint();
		}

		FReal OutTime = FLT_MAX;
		FVec3 Location, Normal;
		int32 FaceIndex = -1;
		Chaos::FVec3 FaceNormal;

		Utilities::CastHelper(A, AStartTransform, [&](const auto& ADowncast, const FRigidTransform3& AFullTM)
			{
				// @todo(chaos): handle Instanced with margin
				if (B.LowLevelSweepGeom(ADowncast, AToBTM, LocalDir, Length, OutTime, Location, Normal, FaceIndex, FaceNormal, 0.0f, true))
				{
					Contact.ShapeContactPoints[0] = AToBTM.InverseTransformPositionNoScale(Location);
					Contact.ShapeContactPoints[1] = Location;
					Contact.ShapeContactNormal = Normal;

					const FVec3& ContactNormal = BTransform.TransformVectorNoScale(Normal);
					ComputeSweptContactPhiAndTOIHelper(ContactNormal, Dir, Length, OutTime, TOI, Contact.Phi);
				}
			});

		return Contact;
	}


	FContactPoint SphereSphereContactPoint(const TSphere<FReal, 3>& Sphere1, const FRigidTransform3& Sphere1Transform, const TSphere<FReal, 3>& Sphere2, const FRigidTransform3& Sphere2Transform, const FReal CullDistance, const FReal ShapePadding)
	{
		FContactPoint Result;

		const FReal R1 = Sphere1.GetRadius() + 0.5f * ShapePadding;
		const FReal R2 = Sphere2.GetRadius() + 0.5f * ShapePadding;

		// World-space contact
		const FVec3 Center1 = Sphere1Transform.TransformPosition(Sphere1.GetCenter());
		const FVec3 Center2 = Sphere2Transform.TransformPosition(Sphere2.GetCenter());
		const FVec3 Direction = Center1 - Center2;
		const FReal SizeSq = Direction.SizeSquared();
		const FReal CullDistanceSq = FMath::Square(CullDistance + R1 + R2);
		if (SizeSq < CullDistanceSq)
		{
			const FReal Size = FMath::Sqrt(SizeSq);
			const FVec3 Normal = Size > SMALL_NUMBER ? Direction / Size : FVec3(0, 0, 1);
			const FReal NewPhi = Size - (R1 + R2);

			Result.ShapeContactPoints[0] = Sphere1.GetCenter() - Sphere1Transform.InverseTransformVector(R1 * Normal);
			Result.ShapeContactPoints[1] = Sphere2.GetCenter() + Sphere2Transform.InverseTransformVector(R2 * Normal);
			Result.ShapeContactNormal = Sphere2Transform.InverseTransformVector(Normal);
			Result.Phi = NewPhi;
		}
		return Result;
	}

	FContactPoint SpherePlaneContactPoint(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const TPlane<FReal, 3>& Plane, const FRigidTransform3& PlaneTransform, const FReal ShapePadding)
	{
		FContactPoint Result;

		FReal SphereRadius = Sphere.GetRadius() + 0.5f * ShapePadding;

		FVec3 SpherePosWorld = SphereTransform.TransformPosition(Sphere.GetCenter());
		FVec3 SpherePosPlane = PlaneTransform.InverseTransformPosition(SpherePosWorld);

		FVec3 NormalPlane;
		FReal Phi = Plane.PhiWithNormal(SpherePosPlane, NormalPlane) - SphereRadius - 0.5f * ShapePadding;	// Adding plane's share of padding
		FVec3 NormalWorld = PlaneTransform.TransformVector(NormalPlane);
		FVec3 Location = SpherePosWorld - SphereRadius * NormalWorld;

		Result.ShapeContactPoints[0] = SphereTransform.InverseTransformPosition(Location);
		Result.ShapeContactPoints[1] = PlaneTransform.InverseTransformPosition(Location - Phi * NormalWorld);
		Result.ShapeContactNormal = PlaneTransform.InverseTransformVector(NormalWorld);
		Result.Phi = Phi;

		return Result;
	}

	FContactPoint SphereBoxContactPoint(const TSphere<FReal, 3>& Sphere, const FRigidTransform3& SphereTransform, const FImplicitBox3& Box, const FRigidTransform3& BoxTransform, const FReal ShapePadding)
	{
		FContactPoint Result;

		const FVec3 SphereWorld = SphereTransform.TransformPosition(Sphere.GetCenter());	// World-space sphere pos
		const FVec3 SphereBox = BoxTransform.InverseTransformPosition(SphereWorld);			// Box-space sphere pos

		FVec3 NormalBox;																	// Box-space normal
		FReal PhiToSphereCenter = Box.PhiWithNormal(SphereBox, NormalBox);
		FReal Phi = PhiToSphereCenter - Sphere.GetRadius() - ShapePadding;

		FVec3 NormalWorld = BoxTransform.TransformVectorNoScale(NormalBox);
		FVec3 LocationWorld = SphereWorld - (Sphere.GetRadius() + 0.5f * ShapePadding) * NormalWorld;

		Result.ShapeContactPoints[0] = SphereTransform.InverseTransformPosition(LocationWorld);
		Result.ShapeContactPoints[1] = BoxTransform.InverseTransformPosition(LocationWorld - Phi * NormalWorld);
		Result.ShapeContactNormal = NormalBox;
		Result.Phi = Phi;
		return Result;
	}

	FContactPoint SphereCapsuleContactPoint(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const FCapsule& B, const FRigidTransform3& BTransform, const FReal ShapePadding)
	{
		FContactPoint Result;

		FVector A1 = ATransform.TransformPosition(A.GetCenter());
		FVector B1 = BTransform.TransformPosition(B.GetX1());
		FVector B2 = BTransform.TransformPosition(B.GetX2());
		FVector P2 = FMath::ClosestPointOnSegment(A1, B1, B2);

		FVec3 Delta = P2 - A1;
		FReal DeltaLen = Delta.Size();
		if (DeltaLen > KINDA_SMALL_NUMBER)
		{
			FReal NewPhi = DeltaLen - (A.GetRadius() + B.GetRadius()) - ShapePadding;
			FVec3 Dir = Delta / DeltaLen;
			FVec3 LocationA = A1 + Dir * A.GetRadius();
			FVec3 LocationB = P2 - Dir * B.GetRadius();
			FVec3 Normal = -Dir;

			Result.ShapeContactPoints[0] = ATransform.InverseTransformPosition(LocationA);
			Result.ShapeContactPoints[1] = BTransform.InverseTransformPosition(LocationB);
			Result.ShapeContactNormal = BTransform.InverseTransformVector(Normal);
			Result.Phi = NewPhi;
		}

		return Result;
	}

	template <typename TriMeshType>
	FContactPoint SphereTriangleMeshContactPoint(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		return GJKImplicitContactPoint<TSphere<FReal, 3>>(TSphere<FReal, 3>(A), ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	template<typename TriMeshType>
	FContactPoint SphereTriangleMeshSweptContactPoint(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI)
	{
		if (const TImplicitObjectScaled<FTriangleMeshImplicitObject>* ScaledTriangleMesh = B.template GetObject<const TImplicitObjectScaled<FTriangleMeshImplicitObject>>())
		{
			return GJKImplicitScaledTriMeshSweptContactPoint<TSphere<FReal, 3>>(A, ATransform, *ScaledTriangleMesh, BStartTransform, Dir, Length, TOI);
		}
		else if (const FTriangleMeshImplicitObject* TriangleMesh = B.template GetObject<const FTriangleMeshImplicitObject>())
		{
			return GJKImplicitSweptContactPoint(TSphere<FReal, 3>(A), ATransform, *TriangleMesh, BStartTransform, Dir, Length, TOI);
		}

		ensure(false);
		return FContactPoint();
	}

	FContactPoint BoxHeightFieldContactPoint(const FImplicitBox3& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		return GJKImplicitContactPoint<FImplicitBox3>(A, ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	template <typename TriMeshType>
	FContactPoint BoxTriangleMeshContactPoint(const FImplicitBox3& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		return GJKImplicitContactPoint<TBox<FReal, 3>>(A, ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	FContactPoint SphereHeightFieldContactPoint(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		return GJKImplicitContactPoint<TSphere<FReal, 3>>(TSphere<FReal, 3>(A), ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	FContactPoint CapsuleHeightFieldContactPoint(const FCapsule& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_CapsuleHeightFieldContactPoint, ConstraintsDetailedStats);
		return GJKImplicitContactPoint<FCapsule>(A, ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	template <typename TriMeshType>
	FContactPoint CapsuleTriangleMeshContactPoint(const FCapsule& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_CapsuleTriangleMeshContactPoint, ConstraintsDetailedStats);
		return GJKImplicitContactPoint<FCapsule>(A, ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	template <typename TriMeshType>
	FContactPoint CapsuleTriangleMeshSweptContactPoint(const FCapsule& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_CapsuleTriangleMeshSweptContactPoint, ConstraintsDetailedStats);
		if (const TImplicitObjectScaled<FTriangleMeshImplicitObject>* ScaledTriangleMesh = B.template GetObject<const TImplicitObjectScaled<FTriangleMeshImplicitObject>>())
		{
			return GJKImplicitScaledTriMeshSweptContactPoint<FCapsule>(A, ATransform, *ScaledTriangleMesh, BStartTransform, Dir, Length, TOI);
		}
		else if (const FTriangleMeshImplicitObject* TriangleMesh = B.template GetObject<const FTriangleMeshImplicitObject>())
		{
			return GJKImplicitSweptContactPoint(A, ATransform, *TriangleMesh, BStartTransform, Dir, Length, TOI);
		}

		ensure(false);
		return FContactPoint();
	}

	FContactPoint ConvexHeightFieldContactPoint(const FImplicitObject& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_ConvexHeightFieldContactPoint, ConstraintsDetailedStats);
		return GJKImplicitContactPoint<FConvex>(A, ATransform, B, BTransform, CullDistance, ShapePadding);
	}

	FContactPoint ConvexTriangleMeshContactPoint(const FImplicitObject& A, const FRigidTransform3& ATransform, const FImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_ConvexTriangleMeshContactPoint, ConstraintsDetailedStats);

		// Call GJK with the concrete trimesh type (scaled, instanced, raw)
		return Utilities::CastWrapped<FTriangleMeshImplicitObject>(B, 
			[&](auto BConcretePtr)
			{
				check(BConcretePtr != nullptr);
				return GJKImplicitContactPoint<FConvex>(A, ATransform, *BConcretePtr, BTransform, CullDistance, ShapePadding);
			});
	}

	template <typename TriMeshType>
	FContactPoint ConvexTriangleMeshSweptContactPoint(const FImplicitObject& A, const FRigidTransform3& ATransform, const TriMeshType& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI)
	{
		CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_Collisions_ConvexTriangleMeshSweptContactPoint, ConstraintsDetailedStats);
		if (const TImplicitObjectScaled<FTriangleMeshImplicitObject>* ScaledTriangleMesh = B.template GetObject<const TImplicitObjectScaled<FTriangleMeshImplicitObject>>())
		{
			return GJKImplicitScaledTriMeshSweptContactPoint<FConvex>(A, ATransform, *ScaledTriangleMesh, BStartTransform, Dir, Length, TOI);
		}
		else if (const FTriangleMeshImplicitObject* TriangleMesh = B.template GetObject<const FTriangleMeshImplicitObject>())
		{
			return GJKImplicitSweptContactPoint(A, ATransform, *TriangleMesh, BStartTransform, Dir, Length, TOI);
		}

		ensure(false);
		return FContactPoint();
	}

	FContactPoint CapsuleCapsuleContactPoint(const FCapsule& A, const FRigidTransform3& ATransform, const FCapsule& B, const FRigidTransform3& BTransform, const FReal ShapePadding)
	{
		FContactPoint Result;

		FVector A1 = ATransform.TransformPosition(A.GetX1());
		FVector A2 = ATransform.TransformPosition(A.GetX2());
		FVector B1 = BTransform.TransformPosition(B.GetX1());
		FVector B2 = BTransform.TransformPosition(B.GetX2());
		FVector P1, P2;
		FMath::SegmentDistToSegmentSafe(A1, A2, B1, B2, P1, P2);

		FVec3 Delta = P2 - P1;
		FReal DeltaLen = Delta.Size();
		if (DeltaLen > KINDA_SMALL_NUMBER)
		{
			FReal NewPhi = DeltaLen - (A.GetRadius() + B.GetRadius()) - ShapePadding;
			FVec3 Dir = Delta / DeltaLen;
			FVec3 Normal = -Dir;
			FVec3 LocationA = P1 + Dir * A.GetRadius();
			FVec3 LocationB = P2 - Dir * B.GetRadius();

			Result.ShapeContactPoints[0] = ATransform.InverseTransformPosition(LocationA);
			Result.ShapeContactPoints[1] = BTransform.InverseTransformPosition(LocationB);
			Result.ShapeContactNormal = BTransform.InverseTransformVector(Normal);
			Result.Phi = NewPhi;
		}

		return Result;
	}

	FContactPoint CapsuleBoxContactPoint(const FCapsule& A, const FRigidTransform3& ATransform, const FImplicitBox3& B, const FRigidTransform3& BTransform, const FVec3& InitialDir, const FReal ShapePadding)
	{
		return GJKContactPoint(A, ATransform, B, BTransform, InitialDir, ShapePadding);
	}


	// Template  Instantiations
	template FContactPoint GJKImplicitSweptContactPoint<FHeightField>(const FImplicitObject& A, const FRigidTransform3& AStartTransform, const FHeightField& B, const FRigidTransform3& BTransform, const FVec3& Dir, const FReal Length, FReal& TOI);

	template FContactPoint SphereTriangleMeshSweptContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint SphereTriangleMeshSweptContactPoint<FTriangleMeshImplicitObject>(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint CapsuleTriangleMeshSweptContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FCapsule& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint CapsuleTriangleMeshSweptContactPoint<FTriangleMeshImplicitObject>(const FCapsule& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint ConvexTriangleMeshSweptContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint ConvexTriangleMeshSweptContactPoint<FTriangleMeshImplicitObject>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BStartTransform, const FVec3& Dir, const FReal Length, FReal& TOI);
	template FContactPoint BoxTriangleMeshContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitBox3& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);
	template FContactPoint BoxTriangleMeshContactPoint<FTriangleMeshImplicitObject>(const FImplicitBox3& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);
	template FContactPoint SphereTriangleMeshContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);
	template FContactPoint SphereTriangleMeshContactPoint<FTriangleMeshImplicitObject>(const TSphere<FReal, 3>& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);
	template FContactPoint CapsuleTriangleMeshContactPoint<TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FCapsule& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);
	template FContactPoint CapsuleTriangleMeshContactPoint<FTriangleMeshImplicitObject>(const FCapsule& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding);

	template void GJKImplicitManifold<FConvex, FHeightField>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);
	template void GJKImplicitManifold<FCapsule, FHeightField>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);

	template void GJKImplicitManifold<FConvex, TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);
	template void GJKImplicitManifold<FConvex, FTriangleMeshImplicitObject>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);

	template void GJKImplicitManifold<FCapsule, TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);
	template void GJKImplicitManifold<FCapsule, FTriangleMeshImplicitObject>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);

	template void GJKImplicitManifold<FImplicitBox3, FHeightField>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FHeightField& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);

	template void GJKImplicitManifold<FImplicitBox3, TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>>(const FImplicitObject& A, const FRigidTransform3& ATransform, const TImplicitObjectScaled<class Chaos::FTriangleMeshImplicitObject, 1>& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);
	template void GJKImplicitManifold<FImplicitBox3, FTriangleMeshImplicitObject>(const FImplicitObject& A, const FRigidTransform3& ATransform, const FTriangleMeshImplicitObject& B, const FRigidTransform3& BTransform, const FReal CullDistance, const FReal ShapePadding, TArray<FContactPoint>& ContactPoints);
}
