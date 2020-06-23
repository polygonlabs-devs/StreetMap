// Copyright 2017 Mike Fricker. All Rights Reserved.
#pragma once

#include "StreetMap.h"
#include "Components/MeshComponent.h"
#include "Interfaces/Interface_CollisionDataProvider.h"
#include "../StreetMapSceneProxy.h"
#include "./PredictiveData.h"
#include "StreetMapComponent.generated.h"

class UBodySetup;

/**
 * Component that represents a section of street map roads and buildings
 */
UCLASS(meta = (BlueprintSpawnableComponent), hidecategories = (Physics))
class STREETMAPRUNTIME_API UStreetMapComponent : public UMeshComponent, public IInterface_CollisionDataProvider
{
	GENERATED_BODY()

private: 
	TMap<FName, float> mFlowData;
	TMap<FName, FPredictiveData> mPredictiveData;
	TMap<FGuid, TArray<FStreetMapLink>> mTraces;
	TMap<FName, int> mTMC2RoadIndex;
	TMap<FStreetMapLink, int> mLink2RoadIndex;

	const float HighSpeedRatio = 0.8f;
	const float MedSpeedRatio = 0.5f;
public:

	/** UStreetMapComponent constructor */
	UStreetMapComponent(const class FObjectInitializer& ObjectInitializer);

	/** @return Gets the street map object associated with this component */
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		UStreetMap* GetStreetMap()
	{
		return StreetMap;
	}

	/** Returns StreetMap asset object name  */
	FString GetStreetMapAssetName() const;

	/** Returns true if we have valid cached mesh data from our assigned street map asset */
	bool HasValidMesh() const
	{
		return (StreetVertices.Num() != 0 && StreetIndices.Num() != 0) ||
			(MajorRoadVertices.Num() != 0 && MajorRoadIndices.Num() != 0) ||
			(HighwayVertices.Num() != 0 && HighwayIndices.Num() != 0) ||
			(BuildingVertices.Num() != 0 && BuildingIndices.Num() != 0);
	}

	/** Returns Cached raw mesh vertices */
	TArray< struct FStreetMapVertex > GetRawMeshVertices(EVertexType type) const
	{
		switch (type) {
		case EVertexType::VBuilding:
			return BuildingVertices;
		case EVertexType::VMajorRoad:
			return MajorRoadVertices;
		case EVertexType::VHighway:
			return HighwayVertices;
		default:
			return StreetVertices;
		}
	}

	/** Returns Cached raw mesh triangle indices */
	TArray< uint32 > GetRawMeshIndices(EVertexType type) const
	{
		switch (type) {
		case EVertexType::VBuilding:
			return BuildingIndices;
		case EVertexType::VMajorRoad:
			return MajorRoadIndices;
		case EVertexType::VHighway:
			return HighwayIndices;
		default:
			return StreetIndices;
		}
	}

	/**
	* Returns StreetMap Default Material if a valid one is found in plugin's content folder.
	* Otherwise , it returns the default surface 3d material.
	*/
	UMaterialInterface* GetDefaultMaterial() const
	{
		return StreetMapDefaultMaterial != nullptr ? StreetMapDefaultMaterial : UMaterial::GetDefaultMaterial(MD_Surface);
	}

	/** Returns true, if the input PropertyName correspond to a collision property. */
	bool IsCollisionProperty(const FName& PropertyName) const
	{
		return PropertyName == TEXT("bGenerateCollision") || PropertyName == TEXT("bAllowDoubleSidedGeometry");
	}

	/**
	*	Returns sub-meshes count.
	*	In this case we are creating one single mesh section, so it will return 1.
	*	If cached mesh data are not valid , it will return 0.
	*/
	int32 GetNumMeshSections() const
	{
		return HasValidMesh() ? 1 : 0;
	}

	/**
	 * Assigns a street map asset to this component.  Render state will be updated immediately.
	 *
	 * @param NewStreetMap The street map to use
	 *
	 * @param bRebuildMesh : Rebuilds map mesh based on the new map asset
	 *
	 * @return Sets the street map object
	 */
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void SetStreetMap(UStreetMap* NewStreetMap, bool bClearPreviousMeshIfAny = false, bool bRebuildMesh = false);



	//** Begin Interface_CollisionDataProvider Interface */
	virtual bool GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool InUseAllTriData) override;
	virtual bool ContainsPhysicsTriMeshData(bool InUseAllTriData) const override;
	virtual bool WantsNegXTriMesh() override;
	//** End Interface_CollisionDataProvider Interface *//

protected:

	/**
	* Ensures the body setup is initialized/configured and updates it if needed.
	* @param bForceCreation : Force new BodySetup creation even if a valid one already exists.
	*/
	void CreateBodySetupIfNeeded(bool bForceCreation = false);

	/** Marks collision data as dirty, and re-create on instance if necessary */
	void GenerateCollision();

	/** Wipes out and invalidate collision data. */
	void ClearCollision();

public:

	// UPrimitiveComponent interface
	virtual  UBodySetup* GetBodySetup() override;
	virtual FPrimitiveSceneProxy* CreateSceneProxy() override;
	virtual FBoxSphereBounds CalcBounds(const FTransform& LocalToWorld) const override;
	virtual int32 GetNumMaterials() const override;
#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Wipes out our cached mesh data. Designed to be called on demand.*/
	void InvalidateMesh();

	/** Rebuilds the graphics and physics mesh representation if we don't have one right now.  Designed to be called on demand. */
	void BuildMesh();

	/** Rebuilds road mesh only */
	void BuildRoadMesh(EStreetMapRoadType Type);

	/** Get speed & color from flow/predictive data, returns false if no data is found */
	bool GetSpeedAndColorFromData(const FStreetMapRoad* Road, float& OutSpeed, float& OutSpeedLimit, float& OutSpeedRatio, FColor& OutColor, FColor HighFlowColor, FColor MedFlowColor, FColor LowFlowColor);
	bool GetSpeedAndColorFromData(const FStreetMapRoad* Road, float& OutSpeed, float& OutSpeedLimit, float& OutSpeedRatio, FColor& OutColor);
	bool GetSpeedAndColorFromData(FName TMC, float SpeedLimit, float& OutSpeed, float& OutSpeedRatio, FColor& OutColor, FColor HighFlowColor, FColor MedFlowColor, FColor LowFlowColor);
	bool GetSpeedAndColorFromData(FName TMC, float SpeedLimit, float& OutSpeed, float& OutSpeedRatio, FColor& OutColor);

	/** Color road meshes using flow data */
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, FColor DefaultColor, FColor LowFlowColor, FColor MedFlowColor, FColor HighFlowColor, bool OverwriteTrace, float ZOffset);
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, FLinearColor DefaultColor, bool OverwriteTrace = false, float ZOffset = 0.0f);
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, FLinearColor DefaultColor, FLinearColor LowFlowColor, FLinearColor MedFlowColor, FLinearColor HighFlowColor, bool OverwriteTrace = false, float ZOffset = 0.0f);
	
	/** Same as above but target specific links */
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, TArray<FStreetMapLink> Links, FColor DefaultColor, FColor LowFlowColor, FColor MedFlowColor, FColor HighFlowColor, bool OverwriteTrace, float ZOffset);
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, TArray<FStreetMapLink> Links, FLinearColor DefaultColor, bool OverwriteTrace, float ZOffset = 0.0f);
	void ColorRoadMeshFromData(TArray<FStreetMapVertex>& Vertices, TArray<FStreetMapLink> Links, FLinearColor DefaultColor, FLinearColor LowFlowColor, FLinearColor MedFlowColor, FLinearColor HighFlowColor, bool OverwriteTrace, float ZOffset = 0.0f);

	/** Color road meshes in vertex array */
	void ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, bool IsTrace = false, float ZOffset = 0.0f);

	/** Color road meshes by Link ID */
	void ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, FStreetMapLink Link, bool IsTrace = false, float ZOffset = 0.0f);
	void ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, TArray<FStreetMapLink> Links, bool IsTrace = false, float ZOffset = 0.0f);

	/** Color road meshes by TMC */
	void ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, FName TMC, bool IsTrace = false, float ZOffset = 0.0f);
	void ColorRoadMesh(FLinearColor val, TArray<FStreetMapVertex>& Vertices, TArray<FName> TMCs, bool IsTrace = false, float ZOffset = 0.0f);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetThickness(float val, EStreetMapRoadType type);
	
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void RefreshStreetColors();

	/** Override default flow colors */
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void OverrideFlowColors(FLinearColor LowFlowColor, FLinearColor MedFlowColor, FLinearColor HighFlowColor);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetColor(FLinearColor val, EStreetMapRoadType type);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetColorByLink(FLinearColor val, EStreetMapRoadType type, FStreetMapLink Link);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetColorByLinks(FLinearColor val, EStreetMapRoadType type, TArray<FStreetMapLink> Links);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetColorByTMC(FLinearColor val, EStreetMapRoadType type, FName TMC);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ChangeStreetColorByTMCs(FLinearColor val, EStreetMapRoadType type, TArray<FName> TMCs);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void AddOrUpdateFlowData(FName TMC, float Speed);
	
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void DeleteFlowData(FName TMC);
	
	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ClearFlowData();

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void AddOrUpdatePredictiveData(FName TMC, float S0, float S15, float S30, float S45);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void DeletePredictiveData(FName TMC);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void ClearPredictiveData();

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		FGuid AddTrace(FLinearColor Color, TArray<FStreetMapLink> Links);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		bool GetTraceDetails(FGuid GUID, float& OutAvgSpeed, float& OutDistance, float& OutTravelTime, float& OutIdealTravelTime);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		bool DeleteTrace(FGuid GUID);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		bool GetSpeed(FStreetMapLink Link, float& OutSpeed, float& OutSpeedLimit, float& OutSpeedRatio);

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		EColorMode GetColorMode();

	UFUNCTION(BlueprintCallable, Category = "StreetMap")
		void SetColorMode(EColorMode ColorMode);

protected:

	/** Giving a default material to the mesh if no valid material is already assigned or materials array is empty. */
	void AssignDefaultMaterialIfNeeded();

	/** Updating navoctree entry for this component , if need/possible. */
	void UpdateNavigationIfNeeded();

	/** Generates a cached mesh from raw street map data */
	void GenerateMesh();

	/** Adds a 2D line to the raw mesh */
	void AddThick2DLine(
		const FVector2D Start, 
		const FVector2D End, 
		const float Z, 
		const float Thickness, 
		const float MaxThickness, 
		const FColor& StartColor, 
		const FColor& EndColor, 
		FBox& MeshBoundingBox, 
		TArray<FStreetMapVertex>* Vertices, 
		TArray<uint32>* Indices, 
		EVertexType VertexType, 
		int64 LinkId = -1, 
		FString LinkDir = "", 
		FName TMC = "", 
		int SpeedLimit = 25, 
		float SpeedRatio = 1.0f,
		float RoadTypeFloat = 0.0f
	);

	/** Adds 3D triangles to the raw mesh */
	void AddTriangles(
		const TArray<FVector>& Points, 
		const TArray<int32>& PointIndices, 
		const FVector& ForwardVector, 
		const FVector& UpVector, 
		const FColor& Color, 
		FBox& MeshBoundingBox, 
		TArray<FStreetMapVertex>& Vertices, 
		TArray<uint32>& Indices
	);

	/** Generate a quad for a road segment */
	void CheckRoadSmoothQuadList(FStreetMapRoad& road
		, const bool Start
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);

	void StartSmoothQuadList(const FVector2D& Prev
		, const FVector2D Start
		, const FVector2D& Mid
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);
	
	void StartSmoothQuadList(const FVector2D& Start
		, const FVector2D& Mid
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);


	/** Generate a quad for a road segment */
	void AddSmoothQuad(const FVector2D& Start
		, const FVector2D& Mid
		, const FVector2D& End
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);

	void EndSmoothQuadList(const FVector2D& Mid
		, const FVector2D& End
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);

	void EndSmoothQuadList(const FVector2D& Mid
		, const FVector2D& End
		, const FVector2D& Next
		, const float Z
		, const float Thickness
		, const float MaxThickness
		, const FColor& StartColor
		, const FColor& EndColor
		, float& VAccumulation
		, FBox& MeshBoundingBox
		, TArray<FStreetMapVertex>* Vertices
		, TArray<uint32>* Indices
		, EVertexType VertexType
		, int64 LinkId = -1
		, FString LinkDir = ""
		, FName TMC = ""
		, int SpeedLimit = 25
		, float SpeedRatio = 1.0f
		, float RoadTypeFloat = 0.0f
	);

private:
	void findConnectedRoad(const FStreetMapRoad& Road
		, int32 RoadCheckIndex
		, const bool Start
		, FString LinkDir
		, int32& ChosenRoadIndex
		, bool& fromBack);
protected:

	/** The street map we're representing. */
	UPROPERTY(EditAnywhere, Category = "StreetMap")
		UStreetMap* StreetMap;
	
	UPROPERTY(EditAnywhere, Category = "StreetMap")
		FStreetMapMeshBuildSettings MeshBuildSettings;

	UPROPERTY(EditAnywhere, Category = "StreetMap")
		FStreetMapCollisionSettings CollisionSettings;

	UPROPERTY(EditAnywhere, Category = "Landscape")
		FStreetMapLandscapeBuildSettings LandscapeSettings;

	UPROPERTY(EditAnywhere, Category = "Railway")
		FStreetMapRailwayBuildSettings RailwaySettings;

	UPROPERTY(EditAnywhere, Category = "Roads")
		FStreetMapRoadBuildSettings RoadSettings;

	UPROPERTY(EditAnywhere, Category = "Splines")
		FStreetMapSplineBuildSettings SplineSettings;

	//** Physics data for mesh collision. */
	UPROPERTY(Transient)
		UBodySetup* StreetMapBodySetup;



	friend class FStreetMapComponentDetails;

protected:
	//
	// Cached mesh representation
	//

	/** Cached raw mesh vertices */
	UPROPERTY()
		TArray< struct FStreetMapVertex > StreetVertices;
	UPROPERTY()
		TArray< struct FStreetMapVertex > MajorRoadVertices;
	UPROPERTY()
		TArray< struct FStreetMapVertex > HighwayVertices;
	UPROPERTY()
		TArray< struct FStreetMapVertex > BuildingVertices;

	/** Cached raw mesh triangle indices */
	UPROPERTY()
		TArray< uint32 > StreetIndices;
	UPROPERTY()
		TArray< uint32 > MajorRoadIndices;
	UPROPERTY()
		TArray< uint32 > HighwayIndices;
	UPROPERTY()
		TArray< uint32 > BuildingIndices;

	/** Cached bounding box */
	UPROPERTY()
		FBoxSphereBounds CachedLocalBounds;

	/** Cached StreetMap DefaultMaterial */
	UPROPERTY()
		UMaterialInterface* StreetMapDefaultMaterial;
};
