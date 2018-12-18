// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"

#include "EngineClasses/SpatialActorChannel.h"
#include "EngineClasses/SpatialNetDriver.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "Interop/SpatialTypebindingManager.h"
#include "Schema/SpawnData.h"
#include "Schema/StandardLibrary.h"
#include "UObject/improbable/UnrealObjectRef.h"

#include <WorkerSDK/improbable/c_schema.h>
#include <WorkerSDK/improbable/c_worker.h>

#include "SpatialReceiver.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpatialReceiver, Log, All);

class USpatialSender;
class UGlobalStateManager;

using FChannelObjectPair = TPair<TWeakObjectPtr<USpatialActorChannel>, TWeakObjectPtr<UObject>>;
using FUnresolvedObjectsMap = TMap<Schema_FieldId, TSet<const UObject*>>;
struct FObjectReferences;
using FObjectReferencesMap = TMap<int32, FObjectReferences>;
using FReliableRPCMap = TMap<Worker_RequestId, TSharedRef<struct FPendingRPCParams>>;

struct PendingAddComponentWrapper
{
	PendingAddComponentWrapper() = default;
	PendingAddComponentWrapper(Worker_EntityId InEntityId, Worker_ComponentId InComponentId, const TSharedPtr<improbable::Component>& InData)
		: EntityId(InEntityId), ComponentId(InComponentId), Data(InData) {}

	Worker_EntityId EntityId;
	Worker_ComponentId ComponentId;
	TSharedPtr<improbable::Component> Data;
};

struct FObjectReferences
{
	FObjectReferences() = default;
	FObjectReferences(FObjectReferences&& Other)
		: UnresolvedRefs(MoveTemp(Other.UnresolvedRefs))
		, bSingleProp(Other.bSingleProp)
		, Buffer(MoveTemp(Other.Buffer))
		, NumBufferBits(Other.NumBufferBits)
		, Array(MoveTemp(Other.Array))
		, ParentIndex(Other.ParentIndex)
		, Property(Other.Property) {}

	// Single property constructor
	FObjectReferences(const FUnrealObjectRef& InUnresolvedRef, int32 InParentIndex, UProperty* InProperty)
		: bSingleProp(true), ParentIndex(InParentIndex), Property(InProperty)
	{
		UnresolvedRefs.Add(InUnresolvedRef);
	}

	// Struct (memory stream) constructor
	FObjectReferences(const TArray<uint8>& InBuffer, int32 InNumBufferBits, const TSet<FUnrealObjectRef>& InUnresolvedRefs, int32 InParentIndex, UProperty* InProperty)
		: UnresolvedRefs(InUnresolvedRefs), bSingleProp(false), Buffer(InBuffer), NumBufferBits(InNumBufferBits), ParentIndex(InParentIndex), Property(InProperty) {}

	// Array constructor
	FObjectReferences(FObjectReferencesMap* InArray, int32 InParentIndex, UProperty* InProperty)
		: bSingleProp(false), Array(InArray), ParentIndex(InParentIndex), Property(InProperty) {}

	TSet<FUnrealObjectRef>				UnresolvedRefs;

	bool								bSingleProp;
	TArray<uint8>						Buffer;
	int32								NumBufferBits;

	TUniquePtr<FObjectReferencesMap>	Array;
	int32								ParentIndex;
	UProperty*							Property;
};

struct FPendingIncomingRPC
{
	FPendingIncomingRPC(const TSet<FUnrealObjectRef>& InUnresolvedRefs, UObject* InTargetObject, UFunction* InFunction, const TArray<uint8>& InPayloadData, int64 InCountBits)
		: UnresolvedRefs(InUnresolvedRefs), TargetObject(InTargetObject), Function(InFunction), PayloadData(InPayloadData), CountBits(InCountBits) {}

	TSet<FUnrealObjectRef> UnresolvedRefs;
	TWeakObjectPtr<UObject> TargetObject;
	UFunction* Function;
	TArray<uint8> PayloadData;
	int64 CountBits;
};

using FIncomingRPCArray = TArray<TSharedPtr<FPendingIncomingRPC>>;

DECLARE_DELEGATE_OneParam(EntityQueryDelegate, Worker_EntityQueryResponseOp&);
DECLARE_DELEGATE_OneParam(ReserveEntityIDsDelegate, Worker_ReserveEntityIdsResponseOp&);

UCLASS()
class USpatialReceiver : public UObject
{
	GENERATED_BODY()

public:
	void Init(USpatialNetDriver* NetDriver, FTimerManager* InTimerManager);

	// Dispatcher Calls
	void OnCriticalSection(bool InCriticalSection);
	void OnAddEntity(Worker_AddEntityOp& Op);
	void OnAddComponent(Worker_AddComponentOp& Op);
	void OnRemoveEntity(Worker_RemoveEntityOp& Op);
	void OnAuthorityChange(Worker_AuthorityChangeOp& Op);

	void OnComponentUpdate(Worker_ComponentUpdateOp& Op);
	void OnCommandRequest(Worker_CommandRequestOp& Op);
	void OnCommandResponse(Worker_CommandResponseOp& Op);

	void OnReserveEntityIdResponse(Worker_ReserveEntityIdResponseOp& Op);
	void OnReserveEntityIdsResponse(Worker_ReserveEntityIdsResponseOp& Op);
	void OnCreateEntityResponse(Worker_CreateEntityResponseOp& Op);

	void AddPendingActorRequest(Worker_RequestId RequestId, USpatialActorChannel* Channel);
	void AddPendingReliableRPC(Worker_RequestId RequestId, TSharedRef<struct FPendingRPCParams> Params);

	void AddEntityQueryDelegate(Worker_RequestId RequestId, EntityQueryDelegate Delegate);
	void AddReserveEntityIdsDelegate(Worker_RequestId RequestId, ReserveEntityIDsDelegate Delegate);

	void OnEntityQueryResponse(Worker_EntityQueryResponseOp& Op);

	void CleanupDeletedEntity(Worker_EntityId EntityId);

	void ResolvePendingOperations(UObject* Object, const FUnrealObjectRef& ObjectRef);

private:
	void EnterCriticalSection();
	void LeaveCriticalSection();

	void ReceiveActor(Worker_EntityId EntityId);
	void RemoveActor(Worker_EntityId EntityId);
	AActor* CreateActor(improbable::Position* Position, improbable::SpawnData* SpawnData, UClass* ActorClass, bool bDeferred);

	void HandleActorAuthority(Worker_AuthorityChangeOp& Op);

	void ApplyComponentData(Worker_EntityId EntityId, Worker_ComponentData& Data, USpatialActorChannel* Channel);
	void ApplyComponentUpdate(const Worker_ComponentUpdate& ComponentUpdate, UObject* TargetObject, USpatialActorChannel* Channel, bool bIsHandover);

	void ReceiveRPCCommandRequest(const Worker_CommandRequest& CommandRequest, UObject* TargetObject, UFunction* Function);
	void ReceiveMulticastUpdate(const Worker_ComponentUpdate& ComponentUpdate, UObject* TargetObject, const TArray<UFunction*>& RPCArray);
	void ApplyRPC(UObject* TargetObject, UFunction* Function, TArray<uint8>& PayloadData, int64 CountBits);

	void ReceiveCommandResponse(Worker_CommandResponseOp& Op);

	void QueueIncomingRepUpdates(FChannelObjectPair ChannelObjectPair, const FObjectReferencesMap& ObjectReferencesMap, const TSet<FUnrealObjectRef>& UnresolvedRefs);
	void QueueIncomingRPC(const TSet<FUnrealObjectRef>& UnresolvedRefs, UObject* TargetObject, UFunction* Function, const TArray<uint8>& PayloadData, int64 CountBits);

	void ResolvePendingOperations_Internal(UObject* Object, const FUnrealObjectRef& ObjectRef);
	void ResolveIncomingOperations(UObject* Object, const FUnrealObjectRef& ObjectRef);
	void ResolveIncomingRPCs(UObject* Object, const FUnrealObjectRef& ObjectRef);
	void ResolveObjectReferences(FRepLayout& RepLayout, UObject* ReplicatedObject, FObjectReferencesMap& ObjectReferencesMap, uint8* RESTRICT StoredData, uint8* RESTRICT Data, int32 MaxAbsOffset, TArray<UProperty*>& RepNotifies, bool& bOutSomeObjectsWereMapped, bool& bOutStillHasUnresolved);

	void ProcessQueuedResolvedObjects();

	USpatialActorChannel* PopPendingActorRequest(Worker_RequestId RequestId);

private:
	template <typename T>
	friend T* GetComponentData(USpatialReceiver& Receiver, Worker_EntityId EntityId);

	UPROPERTY()
	USpatialNetDriver* NetDriver;

	UPROPERTY()
	USpatialStaticComponentView* StaticComponentView;

	UPROPERTY()
	USpatialSender* Sender;

	UPROPERTY()
	USpatialPackageMapClient* PackageMap;

	UPROPERTY()
	UWorld* World;

	UPROPERTY()
	USpatialTypebindingManager* TypebindingManager;

	UPROPERTY()
	UGlobalStateManager* GlobalStateManager;

	FTimerManager* TimerManager;

	// TODO: Figure out how to remove entries when Channel/Actor gets deleted - UNR:100
	TMap<FUnrealObjectRef, TSet<FChannelObjectPair>> IncomingRefsMap;
	TMap<FChannelObjectPair, FObjectReferencesMap> UnresolvedRefsMap;
	TArray<TPair<UObject*, FUnrealObjectRef>> ResolvedObjectQueue;

	TMap<FUnrealObjectRef, FIncomingRPCArray> IncomingRPCMap;

	bool bInCriticalSection;
	TArray<Worker_EntityId> PendingAddEntities;
	TArray<Worker_AuthorityChangeOp> PendingAuthorityChanges;
	TArray<PendingAddComponentWrapper> PendingAddComponents;
	TArray<Worker_EntityId> PendingRemoveEntities;

	TMap<Worker_RequestId, USpatialActorChannel*> PendingActorRequests;
	FReliableRPCMap PendingReliableRPCs;

	TMap<Worker_RequestId, EntityQueryDelegate> EntityQueryDelegates;
	TMap<Worker_RequestId, ReserveEntityIDsDelegate> ReserveEntityIDsDelegates;
};
