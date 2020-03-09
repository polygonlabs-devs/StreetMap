// Copyright 2017 Mike Fricker. All Rights Reserved.

#include "Public/StreetMapComponent.h"
#include "StreetMapRuntime.h"
#include "StreetMapSceneProxy.h"
#include "Runtime/Engine/Classes/Engine/StaticMesh.h"
#include "Runtime/Engine/Public/StaticMeshResources.h"
#include "PolygonTools.h"

#include <algorithm>

#include "PhysicsEngine/BodySetup.h"

#if WITH_EDITOR
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "LandscapeLayerInfoObject.h"
#include "Public\StreetMapComponent.h"
#endif //WITH_EDITOR

// #define BAKE_THICKNESS

UStreetMapComponent::UStreetMapComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer),
	StreetMap(nullptr),
	CachedLocalBounds(FBox(ForceInitToZero))
{
	// We make sure our mesh collision profile name is set to NoCollisionProfileName at initialization. 
	// Because we don't have collision data yet!
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);

	// We don't currently need to be ticked.  This can be overridden in a derived class though.
	PrimaryComponentTick.bCanEverTick = false;
	this->bAutoActivate = false;	// NOTE: Components instantiated through C++ are not automatically active, so they'll only tick once and then go to sleep!

	// We don't currently need InitializeComponent() to be called on us.  This can be overridden in a
	// derived class though.
	bWantsInitializeComponent = false;

	// Turn on shadows.  It looks better.
	CastShadow = true;

	// Our mesh is too complicated to be a useful occluder.
	bUseAsOccluder = false;

	// Our mesh can influence navigation.
	bCanEverAffectNavigation = true;

	static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultMaterialAsset(TEXT("/StreetMap/StreetMapDefaultInstanceMaterial"));
	StreetMapDefaultMaterial = DefaultMaterialAsset.Object;

	mFlowData.Empty();
	mTraces.Empty();

#if WITH_EDITOR
	if (GEngine)
	{
		static ConstructorHelpers::FObjectFinder<UMaterialInterface> DefaultLandscapeMaterialAsset(TEXT("/StreetMap/LandscapeDefaultMaterial"));
		LandscapeSettings.Material = DefaultLandscapeMaterialAsset.Object;

		TArray<FName> LayerNames = ALandscapeProxy::GetLayersFromMaterial(LandscapeSettings.Material);
		LandscapeSettings.Layers.Reset(LayerNames.Num());
		for (int32 i = 0; i < LayerNames.Num(); i++)
		{
			const FName& LayerName = LayerNames[i];

			const FString LayerInfoAssetPath = TEXT("/StreetMap/Landscape_") + LayerName.ToString() + TEXT("_DefaultLayerInfo");
			ConstructorHelpers::FObjectFinder<ULandscapeLayerInfoObject> DefaultLandscapeLayerInfoAsset(*LayerInfoAssetPath);

			FLandscapeImportLayerInfo NewImportLayer;
			NewImportLayer.LayerName = LayerName;
			NewImportLayer.LayerInfo = DefaultLandscapeLayerInfoAsset.Object;

			LandscapeSettings.Layers.Add(MoveTemp(NewImportLayer));
		}
	}
#endif
}


FPrimitiveSceneProxy* UStreetMapComponent::CreateSceneProxy()
{
	FStreetMapSceneProxy* StreetMapSceneProxy = nullptr;

	if (HasValidMesh())
	{
		StreetMapSceneProxy = new FStreetMapSceneProxy(this);
		StreetMapSceneProxy->Init(this, EVertexType::VBuilding, BuildingVertices, BuildingIndices);
		StreetMapSceneProxy->Init(this, EVertexType::VStreet, StreetVertices, StreetIndices);
		StreetMapSceneProxy->Init(this, EVertexType::VMajorRoad, MajorRoadVertices, MajorRoadIndices);
		StreetMapSceneProxy->Init(this, EVertexType::VHighway, HighwayVertices, HighwayIndices);
	}

	return StreetMapSceneProxy;
}


int32 UStreetMapComponent::GetNumMaterials() const
{
	// NOTE: This is a bit of a weird thing about Unreal that we need to deal with when defining a component that
	// can have materials assigned.  UPrimitiveComponent::GetNumMaterials() will return 0, so we need to override it 
	// to return the number of overridden materials, which are the actual materials assigned to the component.
	return HasValidMesh() ? GetNumMeshSections() : GetNumOverrideMaterials();
}


void UStreetMapComponent::SetStreetMap(class UStreetMap* NewStreetMap, bool bClearPreviousMeshIfAny /*= false*/, bool bRebuildMesh /*= false */)
{
	if (StreetMap != NewStreetMap)
	{
		StreetMap = NewStreetMap;

		if (bClearPreviousMeshIfAny)
			InvalidateMesh();

		if (bRebuildMesh)
			BuildMesh();
	}
}


bool UStreetMapComponent::GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool InUseAllTriData)
{
	if (!CollisionSettings.bGenerateCollision || !HasValidMesh())
	{
		return false;
	}

	// Copy vertices data
	const int32 NumVertices = BuildingVertices.Num();
	CollisionData->Vertices.Empty();
	CollisionData->Vertices.AddUninitialized(NumVertices);

	for (int32 VertexIndex = 0; VertexIndex < NumVertices; VertexIndex++)
	{
		CollisionData->Vertices[VertexIndex] = BuildingVertices[VertexIndex].Position;
	}

	// Copy indices data
	const int32 NumTriangles = BuildingIndices.Num() / 3;
	FTriIndices TempTriangle;
	for (int32 TriangleIndex = 0; TriangleIndex < NumTriangles * 3; TriangleIndex += 3)
	{

		TempTriangle.v0 = BuildingIndices[TriangleIndex + 0];
		TempTriangle.v1 = BuildingIndices[TriangleIndex + 1];
		TempTriangle.v2 = BuildingIndices[TriangleIndex + 2];


		CollisionData->Indices.Add(TempTriangle);
		CollisionData->MaterialIndices.Add(0);
	}

	CollisionData->bFlipNormals = true;
	CollisionData->bDeformableMesh = true;

	return HasValidMesh();
}


bool UStreetMapComponent::ContainsPhysicsTriMeshData(bool InUseAllTriData) const
{
	return HasValidMesh() && CollisionSettings.bGenerateCollision;
}


bool UStreetMapComponent::WantsNegXTriMesh()
{
	return false;
}


void UStreetMapComponent::CreateBodySetupIfNeeded(bool bForceCreation /*= false*/)
{
	if (StreetMapBodySetup == nullptr || bForceCreation == true)
	{
		// Creating new BodySetup Object.
		StreetMapBodySetup = NewObject<UBodySetup>(this);
		StreetMapBodySetup->BodySetupGuid = FGuid::NewGuid();
		StreetMapBodySetup->bDoubleSidedGeometry = CollisionSettings.bAllowDoubleSidedGeometry;

		// shapes per poly shape for collision (Not working in simulation mode).
		StreetMapBodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
	}
}


void UStreetMapComponent::GenerateCollision()
{
	if (!CollisionSettings.bGenerateCollision || !HasValidMesh())
	{
		return;
	}

	// create a new body setup
	CreateBodySetupIfNeeded(true);


	if (GetCollisionProfileName() == UCollisionProfile::NoCollision_ProfileName)
	{
		SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}

	// Rebuild the body setup
	StreetMapBodySetup->InvalidatePhysicsData();
	StreetMapBodySetup->CreatePhysicsMeshes();

	UpdateNavigationIfNeeded();
}


void UStreetMapComponent::ClearCollision()
{

	if (StreetMapBodySetup != nullptr)
	{
		StreetMapBodySetup->InvalidatePhysicsData();
		StreetMapBodySetup = nullptr;
	}

	if (GetCollisionProfileName() != UCollisionProfile::NoCollision_ProfileName)
	{
		SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
	}

	UpdateNavigationIfNeeded();
}

class UBodySetup* UStreetMapComponent::GetBodySetup()
{
	if (CollisionSettings.bGenerateCollision == true)
	{
		// checking if we have a valid body setup. 
		// A new one is created only if a valid body setup is not found.
		CreateBodySetupIfNeeded();
		return StreetMapBodySetup;
	}

	if (StreetMapBodySetup != nullptr) StreetMapBodySetup = nullptr;

	return nullptr;
}

void UStreetMapComponent::GenerateMesh()
{
	/////////////////////////////////////////////////////////
	// Visual tweakables for generated Street Map mesh
	//
	const float StreetOffsetZ = MeshBuildSettings.StreetOffsetZ;
	const float MajorRoadOffsetZ = MeshBuildSettings.MajorRoadOffsetZ;
	const float HighwayOffsetZ = MeshBuildSettings.HighwayOffsetZ;
	const bool bWantSmoothStreets = MeshBuildSettings.bWantSmoothStreets;
	const bool bWantConnectStreets = MeshBuildSettings.bWantConnectStreets;
	const bool bWant3DBuildings = MeshBuildSettings.bWant3DBuildings;
	const float BuildingLevelFloorFactor = MeshBuildSettings.BuildingLevelFloorFactor;
	const bool bWantLitBuildings = MeshBuildSettings.bWantLitBuildings;
	const bool bWantBuildingBorderOnGround = !bWant3DBuildings;
	const float StreetThickness = MeshBuildSettings.StreetThickness;
	const FColor StreetColor = MeshBuildSettings.StreetColor.ToFColor(false);
	const float MajorRoadThickness = MeshBuildSettings.MajorRoadThickness;
	const FColor MajorRoadColor = MeshBuildSettings.MajorRoadColor.ToFColor(false);
	const float HighwayThickness = MeshBuildSettings.HighwayThickness;
	const FColor HighwayColor = MeshBuildSettings.HighwayColor.ToFColor(false);
	const float BuildingBorderThickness = MeshBuildSettings.BuildingBorderThickness;
	FLinearColor BuildingBorderLinearColor = MeshBuildSettings.BuildingBorderLinearColor;
	const float BuildingBorderZ = MeshBuildSettings.BuildingBorderZ;
	const FColor BuildingBorderColor(BuildingBorderLinearColor.ToFColor(false));
	const FColor BuildingFillColor(FLinearColor(BuildingBorderLinearColor * 0.33f).CopyWithNewOpacity(1.0f).ToFColor(false));
	const FColor LowFlowColor = MeshBuildSettings.LowFlowColor.ToFColor(false);
	const FColor MedFlowColor = MeshBuildSettings.MedFlowColor.ToFColor(false);
	const FColor HighFlowColor = MeshBuildSettings.HighFlowColor.ToFColor(false);
	const bool bColorByFlow = MeshBuildSettings.bColorByFlow;
	/////////////////////////////////////////////////////////


	CachedLocalBounds = FBox(ForceInitToZero);

	BuildingVertices.Reset();
	BuildingIndices.Reset();
	StreetVertices.Reset();
	StreetIndices.Reset();
	MajorRoadVertices.Reset();
	MajorRoadIndices.Reset();
	HighwayVertices.Reset();
	HighwayIndices.Reset();

	const float MaxThickness = std::max(std::max(StreetThickness, MajorRoadThickness), HighwayThickness);

	if (StreetMap != nullptr)
	{
		FBox MeshBoundingBox;
		MeshBoundingBox.Init();

		const auto& Roads = StreetMap->GetRoads();
		const auto& Nodes = StreetMap->GetNodes();
		const auto& Buildings = StreetMap->GetBuildings();

		for (const auto& Road : Roads)
		{
			float RoadThickness;
			FColor RoadColor;
			EVertexType Type;
			float RoadZ;
			TArray<FStreetMapVertex>* Vertices = nullptr;
			TArray<uint32>* Indices = nullptr;

			const int FlowNum = mFlowData.Num();
			if (bColorByFlow && FlowNum > 0) {
				const FString TMC = Road.TMC;
				const float Speed = mFlowData[TMC];
				const float SpeedRatio = Speed / Road.SpeedLimit;

				if (SpeedRatio > 0.8f) {
					RoadColor = HighFlowColor;
				}
				else if (SpeedRatio > 0.5f) {
					RoadColor = MedFlowColor;
				}
				else {
					RoadColor = LowFlowColor;
				}
			}

			switch (Road.RoadType)
			{
			case EStreetMapRoadType::Highway:
				RoadThickness = HighwayThickness;
				if (!bColorByFlow || FlowNum == 0) {
					RoadColor = HighwayColor;
				}
				RoadZ = HighwayOffsetZ;
				Type = EVertexType::VHighway;
				Vertices = &HighwayVertices;
				Indices = &HighwayIndices;
				break;

			case EStreetMapRoadType::MajorRoad:
				RoadThickness = MajorRoadThickness;
				if (!bColorByFlow || FlowNum == 0) {
					RoadColor = MajorRoadColor;
				}
				RoadZ = MajorRoadOffsetZ;
				Type = EVertexType::VMajorRoad;
				Vertices = &MajorRoadVertices;
				Indices = &MajorRoadIndices;
				break;

			case EStreetMapRoadType::Street:
			case EStreetMapRoadType::Other:
			case EStreetMapRoadType::Bridge:
				RoadThickness = StreetThickness;
				if (!bColorByFlow || FlowNum == 0) {
					RoadColor = StreetColor;
				}
				RoadZ = StreetOffsetZ;
				Type = EVertexType::VStreet;
				Vertices = &StreetVertices;
				Indices = &StreetIndices;
				break;

			default:
				check(0);
				break;
			}

			if (Vertices && Indices)
			{
				auto newWay = bWantSmoothStreets && Road.RoadPoints.Num() >= 2;
				if (newWay)
				{
					if (bWantConnectStreets)
					{
						CheckRoadSmoothQuadList(Road,
							true,
							RoadZ,
							RoadThickness,
							MaxThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit);
					}
					else
					{
						StartSmoothQuadList(Road.RoadPoints[0],
							Road.RoadPoints[1],
							RoadZ,
							RoadThickness,
							MaxThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit
						);
					}
					int32 PointIndex = 0;
					for (; PointIndex < Road.RoadPoints.Num() - 2; ++PointIndex)
					{
						AddSmoothQuad(
							Road.RoadPoints[PointIndex],
							Road.RoadPoints[PointIndex + 1],
							Road.RoadPoints[PointIndex + 2],
							RoadZ,
							RoadThickness,
							MaxThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit
						);
					}
					if (bWantConnectStreets)
					{
						CheckRoadSmoothQuadList(Road,
							false,
							RoadZ,
							RoadThickness,
							MaxThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit);
					}
					else
					{
						EndSmoothQuadList(Road.RoadPoints[PointIndex],
							Road.RoadPoints[PointIndex + 1],
							RoadZ,
							RoadThickness,
							MaxThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit);
					}
				}
				else
				{
					for (int32 PointIndex = 0; PointIndex < Road.RoadPoints.Num() - 1; ++PointIndex)
					{
						AddThick2DLine(
							Road.RoadPoints[PointIndex],
							Road.RoadPoints[PointIndex + 1],
							RoadZ,
							RoadThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit
						);
					}
				}
			}
		}

		TArray< int32 > TempIndices;
		TArray< int32 > TriangulatedVertexIndices;
		TArray< FVector > TempPoints;
		for (int32 BuildingIndex = 0; BuildingIndex < Buildings.Num(); ++BuildingIndex)
		{
			const auto& Building = Buildings[BuildingIndex];

			// Building mesh (or filled area, if the building has no height)

			// Triangulate this building
			// @todo: Performance: Triangulating lots of building polygons is quite slow.  We could easily do this 
			//        as part of the import process and store tessellated geometry instead of doing this at load time.
			bool WindsClockwise;
			if (FPolygonTools::TriangulatePolygon(Building.BuildingPoints, TempIndices, /* Out */ TriangulatedVertexIndices, /* Out */ WindsClockwise))
			{
				// @todo: Performance: We could preprocess the building shapes so that the points always wind
				//        in a consistent direction, so we can skip determining the winding above.

				const int32 FirstTopVertexIndex = this->BuildingVertices.Num();

				// calculate fill Z for buildings
				// either use the defined height or extrapolate from building level count
				float BuildingFillZ = MeshBuildSettings.BuildDefaultZ;
				if (bWant3DBuildings) {
					if (Building.Height > 0) {
						BuildingFillZ = Building.Height;
					}
					else if (Building.BuildingLevels > 0) {
						BuildingFillZ = (float)Building.BuildingLevels * BuildingLevelFloorFactor;
					}
				}

				// Top of building
				{
					TempPoints.SetNum(Building.BuildingPoints.Num(), false);
					for (int32 PointIndex = 0; PointIndex < Building.BuildingPoints.Num(); ++PointIndex)
					{
						TempPoints[PointIndex] = FVector(Building.BuildingPoints[(Building.BuildingPoints.Num() - PointIndex) - 1], BuildingFillZ);
					}
					AddTriangles(TempPoints, TriangulatedVertexIndices, FVector::ForwardVector, FVector::UpVector, BuildingFillColor, MeshBoundingBox, BuildingVertices, BuildingIndices);
				}

				if (bWant3DBuildings && (Building.Height > KINDA_SMALL_NUMBER || Building.BuildingLevels > 0 || MeshBuildSettings.BuildDefaultZ > 0.0))
				{
					// NOTE: Lit buildings can't share vertices beyond quads (all quads have their own face normals), so this uses a lot more geometry!
					if (bWantLitBuildings)
					{
						// Create edges for the walls of the 3D buildings
						for (int32 LeftPointIndex = 0; LeftPointIndex < Building.BuildingPoints.Num(); ++LeftPointIndex)
						{
							const int32 RightPointIndex = (LeftPointIndex + 1) % Building.BuildingPoints.Num();

							TempPoints.SetNum(4, false);

							const int32 TopLeftVertexIndex = 0;
							TempPoints[TopLeftVertexIndex] = FVector(Building.BuildingPoints[WindsClockwise ? RightPointIndex : LeftPointIndex], BuildingFillZ);

							const int32 TopRightVertexIndex = 1;
							TempPoints[TopRightVertexIndex] = FVector(Building.BuildingPoints[WindsClockwise ? LeftPointIndex : RightPointIndex], BuildingFillZ);

							const int32 BottomRightVertexIndex = 2;
							TempPoints[BottomRightVertexIndex] = FVector(Building.BuildingPoints[WindsClockwise ? LeftPointIndex : RightPointIndex], 0.0f);

							const int32 BottomLeftVertexIndex = 3;
							TempPoints[BottomLeftVertexIndex] = FVector(Building.BuildingPoints[WindsClockwise ? RightPointIndex : LeftPointIndex], 0.0f);


							TempIndices.SetNum(6, false);

							TempIndices[0] = BottomLeftVertexIndex;
							TempIndices[1] = TopLeftVertexIndex;
							TempIndices[2] = BottomRightVertexIndex;

							TempIndices[3] = BottomRightVertexIndex;
							TempIndices[4] = TopLeftVertexIndex;
							TempIndices[5] = TopRightVertexIndex;

							const FVector FaceNormal = FVector::CrossProduct((TempPoints[0] - TempPoints[2]).GetSafeNormal(), (TempPoints[0] - TempPoints[1]).GetSafeNormal());
							const FVector ForwardVector = FVector::UpVector;
							const FVector UpVector = FaceNormal;
							AddTriangles(TempPoints, TempIndices, ForwardVector, UpVector, BuildingFillColor, MeshBoundingBox, BuildingVertices, BuildingIndices);
						}
					}
					else
					{
						// Create vertices for the bottom
						const int32 FirstBottomVertexIndex = this->BuildingVertices.Num();
						for (int32 PointIndex = 0; PointIndex < Building.BuildingPoints.Num(); ++PointIndex)
						{
							const FVector2D Point = Building.BuildingPoints[PointIndex];

							FStreetMapVertex& NewVertex = *new(this->BuildingVertices)FStreetMapVertex();
							NewVertex.Position = FVector(Point, 0.0f);
							NewVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);	// NOTE: We're not using texture coordinates for anything yet
							NewVertex.TextureCoordinate3 = FVector2D(0.0f, 0.0f);
							NewVertex.TextureCoordinate3 = FVector2D(0.0f, 1.0f); // Thicknesses
							NewVertex.TangentX = FVector::ForwardVector;	 // NOTE: Tangents aren't important for these unlit buildings
							NewVertex.TangentZ = FVector::UpVector;
							NewVertex.Color = BuildingFillColor;

							MeshBoundingBox += NewVertex.Position;
						}

						// Create edges for the walls of the 3D buildings
						for (int32 LeftPointIndex = 0; LeftPointIndex < Building.BuildingPoints.Num(); ++LeftPointIndex)
						{
							const int32 RightPointIndex = (LeftPointIndex + 1) % Building.BuildingPoints.Num();

							const int32 BottomLeftVertexIndex = FirstBottomVertexIndex + LeftPointIndex;
							const int32 BottomRightVertexIndex = FirstBottomVertexIndex + RightPointIndex;
							const int32 TopRightVertexIndex = FirstTopVertexIndex + RightPointIndex;
							const int32 TopLeftVertexIndex = FirstTopVertexIndex + LeftPointIndex;

							this->BuildingIndices.Add(BottomLeftVertexIndex);
							this->BuildingIndices.Add(TopLeftVertexIndex);
							this->BuildingIndices.Add(BottomRightVertexIndex);

							this->BuildingIndices.Add(BottomRightVertexIndex);
							this->BuildingIndices.Add(TopLeftVertexIndex);
							this->BuildingIndices.Add(TopRightVertexIndex);
						}
					}
				}
			}
			else
			{
				// @todo: Triangulation failed for some reason, possibly due to degenerate polygons.  We can
				//        probably improve the algorithm to avoid this happening.
			}

			// Building border
			if (bWantBuildingBorderOnGround)
			{
				for (int32 PointIndex = 0; PointIndex < Building.BuildingPoints.Num(); ++PointIndex)
				{
					AddThick2DLine(
						Building.BuildingPoints[PointIndex],
						Building.BuildingPoints[(PointIndex + 1) % Building.BuildingPoints.Num()],
						BuildingBorderZ,
						BuildingBorderThickness,		// Thickness
						BuildingBorderColor,
						BuildingBorderColor,
						MeshBoundingBox,
						&BuildingVertices,
						&BuildingIndices);
				}
			}
		}

		CachedLocalBounds = MeshBoundingBox;
	}
}

void UStreetMapComponent::BuildRoadMesh(EStreetMapRoadType Type)
{
	/////////////////////////////////////////////////////////
	// Visual tweakables for generated Street Map mesh
	//
	const float StreetOffsetZ = MeshBuildSettings.StreetOffsetZ;
	const float MajorRoadOffsetZ = MeshBuildSettings.MajorRoadOffsetZ;
	const float HighwayOffsetZ = MeshBuildSettings.HighwayOffsetZ;
	const float StreetThickness = MeshBuildSettings.StreetThickness;
	const FColor StreetColor = MeshBuildSettings.StreetColor.ToFColor(false);
	const float MajorRoadThickness = MeshBuildSettings.MajorRoadThickness;
	const FColor MajorRoadColor = MeshBuildSettings.MajorRoadColor.ToFColor(false);
	const float HighwayThickness = MeshBuildSettings.HighwayThickness;
	const FColor HighwayColor = MeshBuildSettings.HighwayColor.ToFColor(false);
	const FColor LowFlowColor = MeshBuildSettings.LowFlowColor.ToFColor(false);
	const FColor MedFlowColor = MeshBuildSettings.MedFlowColor.ToFColor(false);
	const FColor HighFlowColor = MeshBuildSettings.HighFlowColor.ToFColor(false);
	const bool bColorByFlow = MeshBuildSettings.bColorByFlow;
	/////////////////////////////////////////////////////////


	CachedLocalBounds = FBox(ForceInitToZero);

	switch (Type) {
	case EStreetMapRoadType::Highway:
		HighwayIndices.Reset();
		break;
	case EStreetMapRoadType::MajorRoad:
		MajorRoadIndices.Reset();
		break;
	case EStreetMapRoadType::Street:
		StreetIndices.Reset();
		break;
	}

	if (StreetMap != nullptr)
	{
		FBox MeshBoundingBox;
		MeshBoundingBox.Init();

		const auto& Roads = StreetMap->GetRoads();

		for (const auto& Road : Roads)
		{
			if (Road.RoadType == Type) {
				float RoadThickness;
				FColor RoadColor;
				float RoadZ;
				TArray<FStreetMapVertex>* Vertices = nullptr;
				TArray<uint32>* Indices = nullptr;
				
				const int FlowNum = mFlowData.Num();
				if (bColorByFlow && FlowNum > 0) {
					const FString TMC = Road.TMC;
					const float Speed = mFlowData[TMC];
					const float SpeedRatio = Speed / Road.SpeedLimit;

					if (SpeedRatio > 0.8f) {
						RoadColor = HighFlowColor;
					}
					else if (SpeedRatio > 0.5f) {
						RoadColor = MedFlowColor;
					}
					else {
						RoadColor = LowFlowColor;
					}
				}

				switch (Road.RoadType)
				{
				case EStreetMapRoadType::Highway:
					RoadThickness = HighwayThickness;
					if (!bColorByFlow || FlowNum == 0) {
						RoadColor = HighwayColor;
					}
					RoadZ = HighwayOffsetZ;
					Vertices = &HighwayVertices;
					Indices = &HighwayIndices;
					break;

				case EStreetMapRoadType::MajorRoad:
					RoadThickness = MajorRoadThickness;
					if (!bColorByFlow || FlowNum == 0) {
						RoadColor = MajorRoadColor;
					}
					RoadZ = MajorRoadOffsetZ;
					Vertices = &MajorRoadVertices;
					Indices = &MajorRoadIndices;
					break;

				case EStreetMapRoadType::Street:
				case EStreetMapRoadType::Other:
					RoadThickness = StreetThickness;
					if (!bColorByFlow || FlowNum == 0) {
						RoadColor = StreetColor;
					}
					RoadZ = StreetOffsetZ;
					Vertices = &StreetVertices;
					Indices = &StreetIndices;
					break;

				default:
					check(0);
					break;
				}

				if (Vertices && Indices) {
					for (int32 PointIndex = 0; PointIndex < Road.RoadPoints.Num() - 1; ++PointIndex)
					{
						AddThick2DLine(
							Road.RoadPoints[PointIndex],
							Road.RoadPoints[PointIndex + 1],
							RoadZ,
							RoadThickness,
							RoadColor,
							RoadColor,
							MeshBoundingBox,
							Vertices,
							Indices,
							Road.Link.LinkId,
							Road.Link.LinkDir,
							Road.TMC,
							Road.SpeedLimit);
					}
				}
			}
		}

		CachedLocalBounds = MeshBoundingBox;

		if (HasValidMesh())
		{
			// We have a new bounding box
			UpdateBounds();
		}
		else
		{
			// No mesh was generated
		}

		GenerateCollision();

		// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
		MarkRenderStateDirty();

		AssignDefaultMaterialIfNeeded();

		Modify();
	}
}

void UStreetMapComponent::ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, bool IsTrace, float ZOffset)
{
	int NumVertices = Vertices.Num();

	for (int i = 0; i < NumVertices; i++) {
		Vertices[i].Color = val.ToFColor(false);
		Vertices[i].IsTrace = IsTrace;
		if (ZOffset != 0.0f) {
			Vertices[i].Position.Z = ZOffset;
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, FStreetMapLink Link, bool IsTrace, float ZOffset)
{
	/*auto FilteredVertices = Vertices.FilterByPredicate([LinkId, LinkDir](const FStreetMapVertex& Vertex) {
		return Vertex.LinkId == LinkId && Vertex.LinkDir.Equals(LinkDir, ESearchCase::IgnoreCase);
		});*/

	int NumVertices = Vertices.Num();

	for (int i = 0; i < NumVertices; i++) {

		if (Vertices[i].LinkId == Link.LinkId && Vertices[i].LinkDir.Equals(Link.LinkDir, ESearchCase::IgnoreCase)) {
			Vertices[i].Color = val.ToFColor(false);
			Vertices[i].IsTrace = IsTrace;
			if (ZOffset != 0.0f) {
				Vertices[i].Position.Z = ZOffset;
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, TArray<FStreetMapLink> Links, bool IsTrace, float ZOffset)
{
	int NumVertices = Vertices.Num();

	for (int i = 0; i < NumVertices; i++) {
		auto LinkId = Vertices[i].LinkId;
		auto LinkDir = Vertices[i].LinkDir;
		auto LinkPtr = Links.FindByPredicate([LinkId, LinkDir] (const FStreetMapLink& Link)
		{
			return Link.LinkId == LinkId && Link.LinkDir == LinkDir;
		});
			
		if (LinkPtr != nullptr) {
			Vertices[i].Color = val.ToFColor(false);
			Vertices[i].IsTrace = IsTrace;
			if (ZOffset != 0.0f) {
				Vertices[i].Position.Z = ZOffset;
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, FString TMC, bool IsTrace, float ZOffset)
{
	/*auto FilteredVertices = Vertices.FilterByPredicate([TMC](const FStreetMapVertex& Vertex) {
		return Vertex.TMC == TMC;
		});*/

	int NumVertices = Vertices.Num();

	for (int i = 0; i < NumVertices; i++) {
		if (TMC.Equals(Vertices[i].TMC)) {
			Vertices[i].Color = val.ToFColor(false);
			Vertices[i].IsTrace = IsTrace;
			if (ZOffset != 0.0f) {
				Vertices[i].Position.Z = ZOffset;
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, TArray<FString> TMCs, bool IsTrace, float ZOffset)
{
	/*auto FilteredVertices = Vertices.FilterByPredicate([TMCs](const FStreetMapVertex& Vertex) {
		return TMCs.Find(Vertex.TMC) != INDEX_NONE;
		});*/

	int NumVertices = Vertices.Num();

	for (int i = 0; i < NumVertices; i++) {
		if (TMCs.Find(Vertices[i].TMC) != INDEX_NONE) {
			Vertices[i].Color = val.ToFColor(false);
			Vertices[i].IsTrace = IsTrace;
			if (ZOffset != 0.0f) {
				Vertices[i].Position.Z = ZOffset;
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMeshFromFlowData(TArray<FStreetMapVertex>& Vertices, FLinearColor DefaultColor, bool OverwriteTrace, float ZOffset) {
	const FColor LowFlowColor = MeshBuildSettings.LowFlowColor.ToFColor(false);
	const FColor MedFlowColor = MeshBuildSettings.MedFlowColor.ToFColor(false);
	const FColor HighFlowColor = MeshBuildSettings.HighFlowColor.ToFColor(false);
	
	const int FlowNum = mFlowData.Num();
	int NumVertices = Vertices.Num();
	for (int i = 0; i < NumVertices; i++) {
		auto* Vertex = &Vertices[i];
		if (!Vertex->IsTrace || OverwriteTrace) {
			auto RoadColor = DefaultColor;
			const FString TMC = Vertex->TMC;

			if (FlowNum > 0 && mFlowData.Find(TMC) != nullptr) {
				const float Speed = mFlowData[TMC];
				const float SpeedRatio = Speed / Vertex->SpeedLimit;

				if (SpeedRatio > 0.8f) {
					RoadColor = HighFlowColor;
				}
				else if (SpeedRatio > 0.5f) {
					RoadColor = MedFlowColor;
				}
				else {
					RoadColor = LowFlowColor;
				}
			}

			Vertex->IsTrace = false;
			Vertex->Color = RoadColor.ToFColor(false);
			if (ZOffset != 0.0f) {
				Vertex->Position.Z = ZOffset;
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::ColorRoadMeshFromFlowData(TArray<FStreetMapVertex>& Vertices, FLinearColor DefaultColor, TArray<FStreetMapLink> Links, bool OverwriteTrace, float ZOffset) {
	const FColor LowFlowColor = MeshBuildSettings.LowFlowColor.ToFColor(false);
	const FColor MedFlowColor = MeshBuildSettings.MedFlowColor.ToFColor(false);
	const FColor HighFlowColor = MeshBuildSettings.HighFlowColor.ToFColor(false);

	const int FlowNum = mFlowData.Num();
	int NumVertices = Vertices.Num();
	for (int i = 0; i < NumVertices; i++) {
		auto* Vertex = &Vertices[i];
		if (!Vertex->IsTrace || OverwriteTrace) {
			auto RoadColor = DefaultColor;
			auto LinkId = Vertices[i].LinkId;
			auto LinkDir = Vertices[i].LinkDir;
			auto LinkPtr = Links.FindByPredicate([LinkId, LinkDir](const FStreetMapLink& Link)
				{
					return Link.LinkId == LinkId && Link.LinkDir == LinkDir;
				});

			if (LinkPtr != nullptr) {
				const FString TMC = Vertex->TMC;

				if (FlowNum > 0 && mFlowData.Find(TMC) != nullptr) {
					const float Speed = mFlowData[TMC];
					const float SpeedRatio = Speed / Vertex->SpeedLimit;

					if (SpeedRatio > 0.8f) {
						RoadColor = HighFlowColor;
					}
					else if (SpeedRatio > 0.5f) {
						RoadColor = MedFlowColor;
					}
					else {
						RoadColor = LowFlowColor;
					}
				}

				Vertex->IsTrace = false;
				Vertex->Color = RoadColor.ToFColor(false);
				
				if (ZOffset != 0.0f) {
					Vertex->Position.Z = ZOffset;
				}
			}
		}
	}

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}

void UStreetMapComponent::RefreshStreetColor()
{
	const FColor StreetColor = MeshBuildSettings.StreetColor.ToFColor(false);
	const FColor MajorRoadColor = MeshBuildSettings.MajorRoadColor.ToFColor(false);
	const FColor HighwayColor = MeshBuildSettings.HighwayColor.ToFColor(false);
	const bool bColorByFlow = MeshBuildSettings.bColorByFlow;

	if (bColorByFlow) {
		ColorRoadMeshFromFlowData(HighwayVertices, HighwayColor);
		ColorRoadMeshFromFlowData(MajorRoadVertices, MajorRoadColor);
		ColorRoadMeshFromFlowData(StreetVertices, StreetColor);
	} else  {
		ColorRoadMesh(HighwayColor, HighwayVertices);
		ColorRoadMesh(MajorRoadColor, MajorRoadVertices);
		ColorRoadMesh(StreetColor, StreetVertices);
	}
}

void UStreetMapComponent::ChangeStreetThickness(float val, EStreetMapRoadType type)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		MeshBuildSettings.HighwayThickness = val;
		break;
	case EStreetMapRoadType::MajorRoad:
		MeshBuildSettings.MajorRoadThickness = val;
		break;
	case EStreetMapRoadType::Street:
		MeshBuildSettings.StreetThickness = val;
		break;
	default:
		break;
	}

	BuildRoadMesh(type);
}

void UStreetMapComponent::ChangeStreetColor(FLinearColor val, EStreetMapRoadType type)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		MeshBuildSettings.HighwayColor = val;
		ColorRoadMesh(val, HighwayVertices);
		break;
	case EStreetMapRoadType::MajorRoad:
		MeshBuildSettings.MajorRoadColor = val;
		ColorRoadMesh(val, MajorRoadVertices);
		break;
	case EStreetMapRoadType::Street:
		MeshBuildSettings.StreetColor = val;
		ColorRoadMesh(val, StreetVertices);
		break;
	default:
		break;
	}
}

void UStreetMapComponent::ChangeStreetColorByLink(FLinearColor val, EStreetMapRoadType type, FStreetMapLink Link)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		ColorRoadMesh(val, HighwayVertices, Link);
		break;
	case EStreetMapRoadType::MajorRoad:
		ColorRoadMesh(val, MajorRoadVertices, Link);
		break;
	case EStreetMapRoadType::Street:
		ColorRoadMesh(val, StreetVertices, Link);
		break;

	default:
		break;
	}
}

void UStreetMapComponent::ChangeStreetColorByLinks(FLinearColor val, EStreetMapRoadType type, TArray<FStreetMapLink> Links)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		ColorRoadMesh(val, HighwayVertices, Links);
		break;
	case EStreetMapRoadType::MajorRoad:
		ColorRoadMesh(val, MajorRoadVertices, Links);
		break;
	case EStreetMapRoadType::Street:
		ColorRoadMesh(val, StreetVertices, Links);
		break;

	default:
		break;
	}
}

void UStreetMapComponent::ChangeStreetColorByTMC(FLinearColor val, EStreetMapRoadType type, FString TMC)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		ColorRoadMesh(val, HighwayVertices, TMC);
		break;
	case EStreetMapRoadType::MajorRoad:
		ColorRoadMesh(val, MajorRoadVertices, TMC);
		break;
	case EStreetMapRoadType::Street:
		ColorRoadMesh(val, StreetVertices, TMC);
		break;

	default:
		break;
	}
}

void UStreetMapComponent::ChangeStreetColorByTMCs(FLinearColor val, EStreetMapRoadType type, TArray<FString> TMCs)
{
	switch (type)
	{
	case EStreetMapRoadType::Highway:
		ColorRoadMesh(val, HighwayVertices, TMCs);
		break;
	case EStreetMapRoadType::MajorRoad:
		ColorRoadMesh(val, MajorRoadVertices, TMCs);
		break;
	case EStreetMapRoadType::Street:
		ColorRoadMesh(val, StreetVertices, TMCs);
		break;

	default:
		break;
	}
}


#if WITH_EDITOR
void UStreetMapComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	bool bNeedRefreshCustomizationModule = false;

	// Check to see if the "StreetMap" property changed.
	if (PropertyChangedEvent.Property != nullptr)
	{
		const FName PropertyName(PropertyChangedEvent.Property->GetFName());
		if (PropertyName == GET_MEMBER_NAME_CHECKED(UStreetMapComponent, StreetMap))
		{
			bNeedRefreshCustomizationModule = true;
		}
		else if (IsCollisionProperty(PropertyName)) // For some unknown reason , GET_MEMBER_NAME_CHECKED(UStreetMapComponent, CollisionSettings) is not working ??? "TO CHECK LATER"
		{
			if (CollisionSettings.bGenerateCollision == true)
			{
				GenerateCollision();
			}
			else
			{
				ClearCollision();
			}
			bNeedRefreshCustomizationModule = true;
		}
	}

	if (bNeedRefreshCustomizationModule)
	{
		FPropertyEditorModule& PropertyModule = FModuleManager::GetModuleChecked<FPropertyEditorModule>("PropertyEditor");
		PropertyModule.NotifyCustomizationModuleChanged();
	}

	// Call the parent implementation of this function
	Super::PostEditChangeProperty(PropertyChangedEvent);
}
#endif	// WITH_EDITOR


void UStreetMapComponent::BuildMesh()
{
	// Wipes out our cached mesh data. Maybe unnecessary in case GenerateMesh is clearing cached mesh data and creating a new SceneProxy  !
	InvalidateMesh();

	GenerateMesh();

	if (HasValidMesh())
	{
		// We have a new bounding box
		UpdateBounds();
	}
	else
	{
		// No mesh was generated
	}

	GenerateCollision();

	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();

	AssignDefaultMaterialIfNeeded();

	Modify();
}


void UStreetMapComponent::AssignDefaultMaterialIfNeeded()
{
	if (this->GetNumMaterials() == 0 || this->GetMaterial(0) == nullptr)
	{
		if (!HasValidMesh() || GetDefaultMaterial() == nullptr)
			return;

		this->SetMaterial(0, GetDefaultMaterial());
	}

	UMaterialInstanceDynamic* dynamicInstance = Cast<UMaterialInstanceDynamic>(this->GetMaterial(0));
	if (dynamicInstance)
	{
		dynamicInstance->SetScalarParameterValue(TEXT("street_hide_threshold"), MeshBuildSettings.StreetThickness);
		dynamicInstance->SetScalarParameterValue(TEXT("majorroad_hide_threshold"), MeshBuildSettings.MajorRoadThickness);
	}
	else
	{
		UMaterialInstanceDynamic* MI = UMaterialInstanceDynamic::Create(this->GetMaterial(0), this);
		this->SetMaterial(0, MI);
		MI->SetScalarParameterValue(TEXT("street_hide_threshold"), MeshBuildSettings.StreetThickness);
		MI->SetScalarParameterValue(TEXT("majorroad_hide_threshold"), MeshBuildSettings.MajorRoadThickness);
	}
	
}


void UStreetMapComponent::UpdateNavigationIfNeeded()
{
	if (bCanEverAffectNavigation || bNavigationRelevant)
	{
		FNavigationSystem::UpdateComponentData(*this);
	}
}

void UStreetMapComponent::InvalidateMesh()
{
	BuildingVertices.Reset();
	BuildingIndices.Reset();
	StreetVertices.Reset();
	StreetIndices.Reset();
	MajorRoadVertices.Reset();
	MajorRoadIndices.Reset();
	HighwayVertices.Reset();
	HighwayIndices.Reset();

	CachedLocalBounds = FBoxSphereBounds(FBox(ForceInitToZero));
	ClearCollision();
	// Mark our render state dirty so that CreateSceneProxy can refresh it on demand
	MarkRenderStateDirty();
	Modify();
}

FBoxSphereBounds UStreetMapComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	if (HasValidMesh())
	{
		FBoxSphereBounds WorldSpaceBounds = CachedLocalBounds.TransformBy(LocalToWorld);
		WorldSpaceBounds.BoxExtent *= BoundsScale;
		WorldSpaceBounds.SphereRadius *= BoundsScale;
		return WorldSpaceBounds;
	}
	else
	{
		return FBoxSphereBounds(LocalToWorld.GetLocation(), FVector::ZeroVector, 0.0f);
	}
}


void UStreetMapComponent::AddThick2DLine(const FVector2D Start, const FVector2D End, const float Z, const float Thickness, const FColor& StartColor, const FColor& EndColor, FBox& MeshBoundingBox, TArray<FStreetMapVertex>* Vertices, TArray<uint32>* Indices, int64 LinkId, FString LinkDir, FString TMC, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;
	
	const float Distance = (End - Start).Size();
	const float XRatio = Distance / Thickness;
	const FVector2D LineDirection = (End - Start).GetSafeNormal();
	const FVector2D RightVector(-LineDirection.Y, LineDirection.X);

	const int32 BottomLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomLeftVertex = *new(*Vertices)FStreetMapVertex();
	BottomLeftVertex.LinkId = LinkId;
	BottomLeftVertex.LinkDir = LinkDir;
	BottomLeftVertex.TMC = TMC;
	BottomLeftVertex.SpeedLimit = SpeedLimit;
	BottomLeftVertex.Position = FVector(Start - RightVector * HalfThickness, Z);
	BottomLeftVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);
	BottomLeftVertex.TangentX = FVector(LineDirection, 0.0f);
	BottomLeftVertex.TangentZ = FVector::UpVector;
	BottomLeftVertex.Color = StartColor;
	MeshBoundingBox += BottomLeftVertex.Position;

	const int32 BottomRightVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomRightVertex = *new(*Vertices)FStreetMapVertex();
	BottomRightVertex.LinkId = LinkId;
	BottomRightVertex.LinkDir = LinkDir;
	BottomRightVertex.TMC = TMC;
	BottomRightVertex.SpeedLimit = SpeedLimit;
	BottomRightVertex.Position = FVector(Start + RightVector * HalfThickness, Z);
	BottomRightVertex.TextureCoordinate = FVector2D(1.0f, 0.0f);
	BottomRightVertex.TangentX = FVector(LineDirection, 0.0f);
	BottomRightVertex.TangentZ = FVector::UpVector;
	BottomRightVertex.Color = StartColor;
	MeshBoundingBox += BottomRightVertex.Position;

	const int32 TopRightVertexIndex = Vertices->Num();
	FStreetMapVertex& TopRightVertex = *new(*Vertices)FStreetMapVertex();
	TopRightVertex.LinkId = LinkId;
	TopRightVertex.LinkDir = LinkDir;
	TopRightVertex.TMC = TMC;
	TopRightVertex.SpeedLimit = SpeedLimit;
	TopRightVertex.Position = FVector(End + RightVector * HalfThickness, Z);
	TopRightVertex.TextureCoordinate = FVector2D(1.0f, XRatio);
	TopRightVertex.TangentX = FVector(LineDirection, 0.0f);
	TopRightVertex.TangentZ = FVector::UpVector;
	TopRightVertex.Color = EndColor;
	MeshBoundingBox += TopRightVertex.Position;

	const int32 TopLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& TopLeftVertex = *new(*Vertices)FStreetMapVertex();
	TopLeftVertex.LinkId = LinkId;
	TopLeftVertex.LinkDir = LinkDir;
	TopLeftVertex.TMC = TMC;
	TopLeftVertex.SpeedLimit = SpeedLimit;
	TopLeftVertex.Position = FVector(End - RightVector * HalfThickness, Z);
	TopLeftVertex.TextureCoordinate = FVector2D(0.0f, XRatio);
	TopLeftVertex.TangentX = FVector(LineDirection, 0.0f);
	TopLeftVertex.TangentZ = FVector::UpVector;
	TopLeftVertex.Color = EndColor;
	MeshBoundingBox += TopLeftVertex.Position;

	Indices->Add(BottomLeftVertexIndex);
	Indices->Add(BottomRightVertexIndex);
	Indices->Add(TopRightVertexIndex);

	Indices->Add(BottomLeftVertexIndex);
	Indices->Add(TopRightVertexIndex);
	Indices->Add(TopLeftVertexIndex);
};


void UStreetMapComponent::AddTriangles(const TArray<FVector>& Points, const TArray<int32>& PointIndices, const FVector& ForwardVector, const FVector& UpVector, const FColor& Color, FBox& MeshBoundingBox, TArray<FStreetMapVertex>& Vertices, TArray<uint32>& Indices)
{
	const int32 FirstVertexIndex = Vertices.Num();

	for (FVector Point : Points)
	{
		FStreetMapVertex& NewVertex = *new(Vertices)FStreetMapVertex();
		NewVertex.Position = Point;
		NewVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);	// NOTE: We're not using texture coordinates for anything yet
		NewVertex.TextureCoordinate3 = FVector2D(0.0f, 0.0f);
		NewVertex.TextureCoordinate3 = FVector2D(0.0f, 1.0f); // Thicknesses
		NewVertex.TangentX = ForwardVector;
		NewVertex.TangentZ = UpVector;
		NewVertex.Color = Color;

		MeshBoundingBox += NewVertex.Position;
	}

	for (int32 PointIndex : PointIndices)
	{
		Indices.Add(FirstVertexIndex + PointIndex);
	}
};

void UStreetMapComponent::CheckRoadSmoothQuadList(const FStreetMapRoad& Road
	, const bool Start
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	if (!StreetMap)
	{
		return;
	}
	const auto& Roads = StreetMap->GetRoads();
	const auto& Nodes = StreetMap->GetNodes();

	int32 RoadCheckIndex = 0;
	if (!Start)
	{
		RoadCheckIndex = Road.NodeIndices.Num() - 1;
	}

	if (Road.NodeIndices[RoadCheckIndex] < Nodes.Num())
	{
		auto& Node = Nodes[Road.NodeIndices[RoadCheckIndex]];
		auto RoadIndex = Road.GetRoadIndex(*StreetMap);
		int RefCount = Node.RoadRefs.Num();

		int actualRefCount = 0;
		// check the number of roads we can connect to
		for (auto& OtherRoadNode : Node.RoadRefs)
		{
			if (OtherRoadNode.RoadIndex != RoadIndex)
			{
				auto& OtherRoad = Roads[OtherRoadNode.RoadIndex];
				if (OtherRoad.RoadType == Road.RoadType && OtherRoadNode.RoadIndex != RoadIndex)
				{
					++actualRefCount;
				}
			}
		}

		if (RefCount > 1)
		{
			auto RoadInNodeIndex = Node.RoadRefs.IndexOfByKey(RoadIndex);
			int32 ChosenRoadIndex = -1;
			bool fromBack = false;
			float CosAlpha = MeshBuildSettings.fThresholdConnectStreets;

			for (auto& OtherRoadNode : Node.RoadRefs)
			{
				if (OtherRoadNode.RoadIndex != RoadIndex)
				{
					auto& OtherRoad = Roads[OtherRoadNode.RoadIndex];
					if (OtherRoad.RoadType != Road.RoadType
						|| Road.NodeIndices[RoadCheckIndex] == INDEX_NONE)
					{
						continue;
					}

					if (INDEX_NONE != OtherRoad.NodeIndices[0] && OtherRoad.NodeIndices[0] == Road.NodeIndices[RoadCheckIndex])
					{
						fromBack = false;
					}
					else if (INDEX_NONE != OtherRoad.NodeIndices.Last() && OtherRoad.NodeIndices.Last() == Road.NodeIndices[RoadCheckIndex])
					{
						fromBack = true;
					}
					else
					{
						continue;
					}

					{
						// check angle between the 2 road segments
						const FVector2D* Prev = nullptr, *Mid = nullptr, *Next = nullptr;
						if (Start)
						{
							Prev = fromBack ? &OtherRoad.RoadPoints.Last(1) : &OtherRoad.RoadPoints[1];
							Mid = &Road.RoadPoints[0];
							Next = &Road.RoadPoints[1];
						}
						else
						{
							Prev = &Road.RoadPoints.Last(1);
							Mid = &Road.RoadPoints.Last();
							Next = fromBack ? &OtherRoad.RoadPoints.Last(1) : &OtherRoad.RoadPoints[1];
						}

						if (Prev && Mid && Next)
						{
							auto direction1 = (*Prev - *Mid).GetSafeNormal();
							auto direction2 = (*Next - *Mid).GetSafeNormal();

							auto cosAlpha = FVector2D::DotProduct(direction1, direction2);

							if (cosAlpha < -CosAlpha || actualRefCount == 1)
							{
								CosAlpha = -cosAlpha;
								ChosenRoadIndex = OtherRoad.GetRoadIndex(*StreetMap);
							}
						}
					}
				}
			}

			if (ChosenRoadIndex >= 0)
			{
				auto& OtherRoad = Roads[ChosenRoadIndex];
				if (Start)
				{
					StartSmoothQuadList(fromBack ? OtherRoad.RoadPoints.Last(1) : OtherRoad.RoadPoints[1],
						Road.RoadPoints[0],
						Road.RoadPoints[1],
						Z,
						Thickness,
						MaxThickness,
						StartColor,
						EndColor,
						MeshBoundingBox,
						Vertices,
						Indices,
						LinkId,
						LinkDir,
						TMC,
						SpeedLimit);
				}
				else
				{
					EndSmoothQuadList(Road.RoadPoints.Last(1),
						Road.RoadPoints.Last(),
						fromBack ? OtherRoad.RoadPoints.Last(1) : OtherRoad.RoadPoints[1],
						Z,
						Thickness,
						MaxThickness,
						StartColor,
						EndColor,
						MeshBoundingBox,
						Vertices,
						Indices,
						LinkId,
						LinkDir,
						TMC,
						SpeedLimit);
				}
				return;
			}
		}

	}

	// fall through to default
	if (Start)
	{
		StartSmoothQuadList(Road.RoadPoints[0],
			Road.RoadPoints[1],
			Z,
			Thickness,
			MaxThickness,
			StartColor,
			EndColor,
			MeshBoundingBox,
			Vertices,
			Indices,
			LinkId,
			LinkDir,
			TMC,
			SpeedLimit);
	}
	else
	{
		EndSmoothQuadList(Road.RoadPoints.Last(1),
			Road.RoadPoints.Last(),
			Z,
			Thickness,
			MaxThickness,
			StartColor,
			EndColor,
			MeshBoundingBox,
			Vertices,
			Indices,
			LinkId,
			LinkDir,
			TMC,
			SpeedLimit
		);
	}
}

void UStreetMapComponent::StartSmoothQuadList(const FVector2D& Prev
	, const FVector2D Start
	, const FVector2D& Mid
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;

	const FVector2D LineDirection1 = (Start - Prev).GetSafeNormal();
	const FVector2D LineDirection2 = (Mid - Start).GetSafeNormal();

	auto alteredLineDirection = LineDirection1 + LineDirection2;
	alteredLineDirection.Normalize();

	const FVector2D RightVector(-alteredLineDirection.Y, alteredLineDirection.X);

	const int32 BottomLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomLeftVertex = *new(*Vertices)FStreetMapVertex();
	BottomLeftVertex.LinkId = LinkId;
	BottomLeftVertex.LinkDir = LinkDir;
	BottomLeftVertex.TMC = TMC;
	BottomLeftVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	 BottomLeftVertex.Position = FVector(Start - RightVector * HalfThickness, Z);
#else
	BottomLeftVertex.Position = FVector(Start, Z);
#endif
	BottomLeftVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);
	BottomLeftVertex.TextureCoordinate2 = FVector2D(-RightVector.X, -RightVector.Y);
	BottomLeftVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	BottomLeftVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	BottomLeftVertex.TangentZ = FVector::UpVector;
	BottomLeftVertex.Color = StartColor;
	MeshBoundingBox += BottomLeftVertex.Position;

	const int32 BottomRightVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomRightVertex = *new(*Vertices)FStreetMapVertex();
	BottomRightVertex.LinkId = LinkId;
	BottomRightVertex.LinkDir = LinkDir;
	BottomRightVertex.TMC = TMC;
	BottomRightVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	BottomRightVertex.Position = FVector(Start + RightVector * HalfThickness, Z);
#else
	BottomRightVertex.Position = FVector(Start, Z);
#endif
	BottomRightVertex.TextureCoordinate = FVector2D(1.0f, 0.0f);
	BottomRightVertex.TextureCoordinate2 = FVector2D(RightVector.X, RightVector.Y);
	BottomRightVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	BottomRightVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	BottomRightVertex.TangentZ = FVector::UpVector;
	BottomRightVertex.Color = StartColor;
	MeshBoundingBox += BottomRightVertex.Position;

	Indices->Add(BottomLeftVertexIndex);
	Indices->Add(BottomRightVertexIndex);
}

void UStreetMapComponent::StartSmoothQuadList(const FVector2D& Start
	, const FVector2D& Mid
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;

	const FVector2D LineDirection1 = (Mid - Start).GetSafeNormal();

	const FVector2D RightVector(-LineDirection1.Y, LineDirection1.X);
	const int32 BottomLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomLeftVertex = *new(*Vertices)FStreetMapVertex();
	BottomLeftVertex.LinkId = LinkId;
	BottomLeftVertex.LinkDir = LinkDir;
	BottomLeftVertex.TMC = TMC;
	BottomLeftVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	BottomLeftVertex.Position = FVector(Start - RightVector * HalfThickness, Z);
#else
	BottomLeftVertex.Position = FVector(Start, Z);
#endif
	BottomLeftVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);
	BottomLeftVertex.TextureCoordinate2 = FVector2D(-RightVector.X, -RightVector.Y);
	BottomLeftVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	BottomLeftVertex.TangentX = FVector(LineDirection1, 0.0f);
	BottomLeftVertex.TangentZ = FVector::UpVector;
	BottomLeftVertex.Color = StartColor;
	MeshBoundingBox += BottomLeftVertex.Position;

	const int32 BottomRightVertexIndex = Vertices->Num();
	FStreetMapVertex& BottomRightVertex = *new(*Vertices)FStreetMapVertex();
	BottomRightVertex.LinkId = LinkId;
	BottomRightVertex.LinkDir = LinkDir;
	BottomRightVertex.TMC = TMC;
	BottomRightVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	BottomRightVertex.Position = FVector(Start + RightVector * HalfThickness, Z);
#else
	BottomRightVertex.Position = FVector(Start, Z);
#endif
	BottomRightVertex.TextureCoordinate = FVector2D(1.0f, 0.0f);
	BottomRightVertex.TextureCoordinate2 = FVector2D(RightVector.X, RightVector.Y);
	BottomRightVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	BottomRightVertex.TangentX = FVector(LineDirection1, 0.0f);
	BottomRightVertex.TangentZ = FVector::UpVector;
	BottomRightVertex.Color = StartColor;
	MeshBoundingBox += BottomRightVertex.Position;

	Indices->Add(BottomLeftVertexIndex);
	Indices->Add(BottomRightVertexIndex);
}


/** Generate a quad for a road segment */
void UStreetMapComponent::AddSmoothQuad(const FVector2D& Start
	, const FVector2D& Mid
	, const FVector2D& End
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;

	const FVector2D LineDirection1 = (Mid - Start).GetSafeNormal();
	const FVector2D LineDirection2 = (End - Mid).GetSafeNormal();

	auto alteredLineDirection = LineDirection1 + LineDirection2;
	alteredLineDirection.Normalize();
	const FVector2D RightVector(-alteredLineDirection.Y, alteredLineDirection.X);

	const int32 MidLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& MidLeftVertex = *new(*Vertices)FStreetMapVertex();
	MidLeftVertex.LinkId = LinkId;
	MidLeftVertex.LinkDir = LinkDir;
	MidLeftVertex.TMC = TMC;
	MidLeftVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	MidLeftVertex.Position = FVector(Mid - RightVector * HalfThickness, Z);
#else
	MidLeftVertex.Position = FVector(Mid, Z);
#endif
	MidLeftVertex.TextureCoordinate = FVector2D(0.0f, 0.0f);
	MidLeftVertex.TextureCoordinate2 = FVector2D(-RightVector.X, -RightVector.Y);
	MidLeftVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	MidLeftVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	MidLeftVertex.TangentZ = FVector::UpVector;
	MidLeftVertex.Color = StartColor;
	MeshBoundingBox += MidLeftVertex.Position;

	const int32 MidRightVertexIndex = Vertices->Num();
	FStreetMapVertex& MidRightVertex = *new(*Vertices)FStreetMapVertex();
	MidRightVertex.LinkId = LinkId;
	MidRightVertex.LinkDir = LinkDir;
	MidRightVertex.TMC = TMC;
	MidRightVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	MidRightVertex.Position = FVector(Mid + RightVector * HalfThickness, Z);
#else
	MidRightVertex.Position = FVector(Mid, Z);
#endif
	MidRightVertex.TextureCoordinate = FVector2D(1.0f, 0.0f);
	MidRightVertex.TextureCoordinate2 = FVector2D(RightVector.X, RightVector.Y);
	MidRightVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	MidRightVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	MidRightVertex.TangentZ = FVector::UpVector;
	MidRightVertex.Color = StartColor;
	MeshBoundingBox += MidRightVertex.Position;

	auto numIdx = Indices->Num();
	auto BottomRightVertexIndex = (*Indices)[numIdx - 1];
	auto BottomLeftVertexIndex = (*Indices)[numIdx - 2];
	
	// Finish Last trinagle
	Indices->Add(MidRightVertexIndex);

	Indices->Add(BottomLeftVertexIndex);
	Indices->Add(MidRightVertexIndex);
	Indices->Add(MidLeftVertexIndex);

	// Start of next triangle
	Indices->Add(MidLeftVertexIndex);
	Indices->Add(MidRightVertexIndex);

}


void UStreetMapComponent::EndSmoothQuadList(const FVector2D& Mid
	, const FVector2D& End
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;

	const FVector2D LineDirection2 = (End - Mid).GetSafeNormal();
	const FVector2D RightVector(-LineDirection2.Y, LineDirection2.X);

	const int32 TopLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& TopLeftVertex = *new(*Vertices)FStreetMapVertex();
	TopLeftVertex.LinkId = LinkId;
	TopLeftVertex.LinkDir = LinkDir;
	TopLeftVertex.TMC = TMC;
	TopLeftVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	TopLeftVertex.Position = FVector(End - RightVector * HalfThickness, Z);
#else
	TopLeftVertex.Position = FVector(End, Z);
#endif
	TopLeftVertex.TextureCoordinate = FVector2D(0.0f, 1.0f);
	TopLeftVertex.TextureCoordinate2 = FVector2D(-RightVector.X, -RightVector.Y);
	TopLeftVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	TopLeftVertex.TangentX = FVector(LineDirection2, 0.0f);
	TopLeftVertex.TangentZ = FVector::UpVector;
	TopLeftVertex.Color = EndColor;
	MeshBoundingBox += TopLeftVertex.Position;

	const int32 TopRightVertexIndex = Vertices->Num();
	FStreetMapVertex& TopRightVertex = *new(*Vertices)FStreetMapVertex();
	TopRightVertex.LinkId = LinkId;
	TopRightVertex.LinkDir = LinkDir;
	TopRightVertex.TMC = TMC;
	TopRightVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	TopRightVertex.Position = FVector(End + RightVector * HalfThickness, Z);
#else
	TopRightVertex.Position = FVector(End, Z);
#endif
	TopRightVertex.TextureCoordinate = FVector2D(1.0f, 1.0f);
	TopRightVertex.TextureCoordinate2 = FVector2D(RightVector.X, RightVector.Y);
	TopRightVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	TopRightVertex.TangentX = FVector(LineDirection2, 0.0f);
	TopRightVertex.TangentZ = FVector::UpVector;
	TopRightVertex.Color = EndColor;
	MeshBoundingBox += TopRightVertex.Position;

	auto numIdx = Indices->Num();

	auto MidRightVertexIndex = (*Indices)[numIdx - 1];
	auto MidLeftVertexIndex = (*Indices)[numIdx - 2];

	Indices->Add(TopRightVertexIndex);

	Indices->Add(MidLeftVertexIndex);
	Indices->Add(TopRightVertexIndex);
	Indices->Add(TopLeftVertexIndex);
}

void UStreetMapComponent::EndSmoothQuadList(const FVector2D& Start
	, const FVector2D& Mid
	, const FVector2D& End
	, const float Z
	, const float Thickness
	, const float MaxThickness
	, const FColor& StartColor
	, const FColor& EndColor
	, FBox& MeshBoundingBox
	, TArray<FStreetMapVertex>* Vertices
	, TArray<uint32>* Indices
	, int64 LinkId
	, FString LinkDir
	, FString TMC
	, int SpeedLimit)
{
	const float HalfThickness = Thickness * 0.5f;

	const FVector2D LineDirection1 = (Mid - Start).GetSafeNormal();
	const FVector2D LineDirection2 = (End - Mid).GetSafeNormal();

	auto alteredLineDirection = LineDirection1 + LineDirection2;
	alteredLineDirection.Normalize();
	const FVector2D RightVector(-alteredLineDirection.Y, alteredLineDirection.X);

	const int32 TopLeftVertexIndex = Vertices->Num();
	FStreetMapVertex& TopLeftVertex = *new(*Vertices)FStreetMapVertex();
	TopLeftVertex.LinkId = LinkId;
	TopLeftVertex.LinkDir = LinkDir;
	TopLeftVertex.TMC = TMC;
	TopLeftVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	TopLeftVertex.Position = FVector(Mid - RightVector * HalfThickness, Z);
#else
	TopLeftVertex.Position = FVector(Mid, Z);
#endif
	TopLeftVertex.TextureCoordinate = FVector2D(0.0f, 1.0f);
	TopLeftVertex.TextureCoordinate2 = FVector2D(-RightVector.X, -RightVector.Y);
	TopLeftVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	TopLeftVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	TopLeftVertex.TangentZ = FVector::UpVector;
	TopLeftVertex.Color = EndColor;
	MeshBoundingBox += TopLeftVertex.Position;

	const int32 TopRightVertexIndex = Vertices->Num();
	FStreetMapVertex& TopRightVertex = *new(*Vertices)FStreetMapVertex();
	TopRightVertex.LinkId = LinkId;
	TopRightVertex.LinkDir = LinkDir;
	TopRightVertex.TMC = TMC;
	TopRightVertex.SpeedLimit = SpeedLimit;
#ifdef BAKE_THICKNESS
	TopRightVertex.Position = FVector(Mid + RightVector * HalfThickness, Z);
#else
	TopRightVertex.Position = FVector(Mid, Z);
#endif
	TopRightVertex.TextureCoordinate = FVector2D(1.0f, 1.0f);
	TopRightVertex.TextureCoordinate2 = FVector2D(RightVector.X, RightVector.Y);
	TopRightVertex.TextureCoordinate3 = FVector2D(HalfThickness, MaxThickness);
	TopRightVertex.TangentX = FVector(alteredLineDirection, 0.0f);
	TopRightVertex.TangentZ = FVector::UpVector;
	TopRightVertex.Color = EndColor;
	MeshBoundingBox += TopRightVertex.Position;

	auto numIdx = Indices->Num();

	auto MidRightVertexIndex = (*Indices)[numIdx - 1];
	auto MidLeftVertexIndex = (*Indices)[numIdx - 2];

	Indices->Add(TopRightVertexIndex);

	Indices->Add(MidLeftVertexIndex);
	Indices->Add(TopRightVertexIndex);
	Indices->Add(TopLeftVertexIndex);
}

FString UStreetMapComponent::GetStreetMapAssetName() const
{
	return StreetMap != nullptr ? StreetMap->GetName() : FString(TEXT("NONE"));
}

void UStreetMapComponent::AddOrUpdateFlowData(FString TMC, float Speed)
{
	if (mFlowData.Find(TMC) == nullptr) {
		mFlowData.Add(TMC, Speed);
	}
	else {
		mFlowData[TMC] = Speed;
	}
}

void UStreetMapComponent::DeleteFlowData(FString TMC)
{
	mFlowData.Remove(TMC);
}

FGuid UStreetMapComponent::AddTrace(FLinearColor Color, TArray<FStreetMapLink> Links)
{
	FGuid NewGuid = FGuid::NewGuid();

	mTraces.Add(NewGuid, Links);

	this->ColorRoadMesh(Color, HighwayVertices, Links, true, 600.0f);
	this->ColorRoadMesh(Color, MajorRoadVertices, Links, true, 600.0f);
	this->ColorRoadMesh(Color, StreetVertices, Links, true, 600.0f);

	return NewGuid;
}

void UStreetMapComponent::DeleteTrace(FGuid GUID)
{
	const FColor StreetColor = MeshBuildSettings.StreetColor.ToFColor(false);
	const FColor MajorRoadColor = MeshBuildSettings.MajorRoadColor.ToFColor(false);
	const FColor HighwayColor = MeshBuildSettings.HighwayColor.ToFColor(false);
	const float StreetOffsetZ = MeshBuildSettings.StreetOffsetZ;
	const float MajorRoadOffsetZ = MeshBuildSettings.MajorRoadOffsetZ;
	const float HighwayOffsetZ = MeshBuildSettings.HighwayOffsetZ;
	const bool bColorByFlow = MeshBuildSettings.bColorByFlow;

	if (mTraces.Find(GUID) != nullptr) {
		auto Trace = mTraces[GUID];
		
		if (bColorByFlow) {
			this->ColorRoadMeshFromFlowData(HighwayVertices, HighwayColor, Trace, true, HighwayOffsetZ);
			this->ColorRoadMeshFromFlowData(MajorRoadVertices, MajorRoadColor, Trace, true, MajorRoadOffsetZ);
			this->ColorRoadMeshFromFlowData(StreetVertices, StreetColor, Trace, true, StreetOffsetZ);
		}
		else {
			this->ColorRoadMesh(HighwayColor, HighwayVertices, Trace, false, HighwayOffsetZ);
			this->ColorRoadMesh(MajorRoadColor, MajorRoadVertices, Trace, false, MajorRoadOffsetZ);
			this->ColorRoadMesh(StreetColor, StreetVertices, Trace, false, StreetOffsetZ);
		}

		mTraces.Remove(GUID);
	}
}
