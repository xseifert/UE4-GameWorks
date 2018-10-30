// Copyright 1998-2013 Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NvFlex.h"
#include "NvFlexExt.h"

#include "PhysXIncludes.h"

// UE types
class UFlexContainer;
class UFlexComponent;
class UFlexAsset;
class UFlexSoftJointComponent;
class UFlexCollisionComponent;
struct FFlexPhase;
struct IFlexContainerClient;

#if STATS

DECLARE_STATS_GROUP(TEXT("Flex"), STATGROUP_Flex, STATCAT_Advanced);
DECLARE_STATS_GROUP_VERBOSE(TEXT("FlexGpu"), STATGROUP_FlexGpu, STATCAT_Advanced);

#endif


// one container per-phys scene
struct FLEX_API FFlexContainerInstance : public PxDeletionListener
{
	FFlexContainerInstance(UFlexContainer* Template, FPhysScene* Owner);
	virtual ~FFlexContainerInstance();

	int32 CreateParticle();
	int32 CreateParticles(int32* Indices, int32 NumIndices);
	void SetParticle(int32 Index, const FVector4& Pos, const FVector& Vel, int32 Phase);
	int32 CreateParticle(const FVector4& Pos, const FVector& Vel, int32 Phase);
	void DestroyParticle(int32 Index);
	void DestroyParticles(const int32* Indices, int32 NumIndices);

	void CopyParticle(int32 Source, int32 Dest);

	// spawns a new instance of an asset into the container
	NvFlexExtInstance* CreateInstance(const NvFlexExtAsset* Asset, const FMatrix& Mat, const FVector& Velocity, int32 Phase);
	void DestroyInstance(NvFlexExtInstance* Asset);

	// spawns a new instance of a soft joint into the container
	NvFlexExtSoftJoint* CreateSoftJointInstance(const TArray<int32>& ParticleIndices, const TArray<FVector>& ParticleLocalPositions, const int32 NumParticles, const float Stiffness);
	void DestroySoftJointInstance(NvFlexExtSoftJoint* joint);

	// convert a phase to the solver format, will allocate a new group if requested
	int32 GetPhase(const FFlexPhase& Phase);

	// returns a cached copy of the PxShape 
	const NvFlexTriangleMeshId GetTriangleMesh(const PxTriangleMesh* TriMesh);
	const NvFlexTriangleMeshId GetTriangleMesh(const PxHeightField* HeightField);
	const NvFlexConvexMeshId GetConvexMesh(const PxConvexMesh* ConvexMesh);

	// kicks off the simulation update and all compute kernels, unmaps particle data, calls unmap()
	void Simulate(float DeltaTime);	
	// starts synchronization phase, should be called after GPU work has finished, calls map()
	void Synchronize();

	// maps particle data, synchronizing with GPU, should only be called by Synchronize()
	void Map();
	// unmaps data, should only be called by Simulate()
	void Unmap();	
	// returns true if data is mapped, if so then reads/writes may occur, otherwise they are illegal
	bool IsMapped();

	// helper to setup report shapes and indices during UpdateCollisionData
	void SetupCollisionReport(void* Shape, UFlexCollisionComponent* ReportComponent);

	// gather and send collision data to the solver
	void UpdateCollisionData();
	// send particle and parameter data to the solver
	void UpdateSimData();

	// register component to receive callbacks 
	void Register(IFlexContainerClient* Comp);
	void Unregister(IFlexContainerClient* Comp);

	// add a radial force for one frame 
	void AddRadialForce(FVector Origin, float Radius, float Strength, ERadialImpulseFalloff Falloff);
	// add a radial impulse for one frame 
	void AddRadialImpulse(FVector Origin, float Radius, float Strength, ERadialImpulseFalloff Falloff, bool bVelChange);

	// helper methods
	void ComputeSteppingParam(float& Dt, int32& NumSubsteps, float& NewLeftOverTime, float DeltaTime) const;
	void DebugDraw();

	int GetActiveParticleCount();
	int GetMaxParticleCount();

	// PxDeletionListener callback used for invalidating shape cache
	virtual void onRelease(const PxBase* observed, void* userData, PxDeletionEventFlag::Enum deletionEvent);

	NvFlexExtContainer* Container;
	NvFlexSolver* Solver;
	NvFlexExtForceFieldCallback* ForceFieldCallback;

	// mapped extensions data
	NvFlexExtParticleData MappedData;

	// cache the converted triangle meshes
	TMap<void*, NvFlexTriangleMeshId> TriangleMeshes;
	TMap<void*, NvFlexConvexMeshId> ConvexMeshes;

	// pointers into the container's mapped memory, only valid during Synchronize(), these are a typed alias for contents of MappedData
	FVector4* Particles;
	FVector4* ParticleRestPositions;
	FVector* Velocities;
	FVector4* Normals;
	int* Phases;

	// copy of particle data
	NvFlexVector<FVector4> Anisotropy1;
	NvFlexVector<FVector4> Anisotropy2;
	NvFlexVector<FVector4> Anisotropy3;
	NvFlexVector<FVector4> SmoothPositions;

	static const int MaxContactsPerParticle = 6;

	NvFlexVector<int32> ContactIndices;
	NvFlexVector<FVector4> ContactVelocities;
	NvFlexVector<uint32> ContactCounts;

	FPhysScene* Owner;
	FBoxSphereBounds Bounds;

	TArray<IFlexContainerClient*> Components;

	TArray<UFlexSoftJointComponent*> SoftJointComponents;

	TWeakObjectPtr<UFlexContainer> TemplateRef;
	UFlexContainer* Template;

	// surface rendering
	class UFlexFluidSurfaceComponent* FluidSurfaceComponent;

	// incrementing group counter used to auto-assign unique groups to rigids
	int GroupCounter;

	NvFlexVector<NvFlexCollisionGeometry> ShapeGeometry;
	NvFlexVector<int32> ShapeFlags;
	NvFlexVector<FVector4> ShapePositions;
	NvFlexVector<FQuat> ShapeRotations;
	NvFlexVector<FVector4> ShapePositionsPrev;
	NvFlexVector<FQuat> ShapeRotationsPrev;

	TArray<int32> CollisionReportIndices;
	TArray<UFlexCollisionComponent*> CollisionReportComponents;
	TMap<void*, int32> ShapeToCollisionReportIndex;

	// temporary buffers used during collision shape building
	NvFlexVector<FVector4> TriMeshVerts;
	NvFlexVector<int32> TriMeshIndices;	
	NvFlexVector<FVector4> ConvexMeshPlanes;

	TArray<NvFlexExtForceField> ForceFields;

	float LeftOverTime;
	float AverageDeltaTime;

	static bool sGlobalDebugDraw;

};
