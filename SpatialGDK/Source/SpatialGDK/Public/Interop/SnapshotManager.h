// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "UObject/NoExportTypes.h"

#include "EngineClasses/SpatialNetDriver.h"
#include "Utils/SchemaUtils.h"

#include <WorkerSDK/improbable/c_schema.h>
#include <WorkerSDK/improbable/c_worker.h>

#include "SnapshotManager.generated.h"

class UGlobalStateManager;
class USpatialReceiver;

DECLARE_LOG_CATEGORY_EXTERN(LogSnapshotManager, Log, All)

UCLASS()
class SPATIALGDK_API USnapshotManager : public UObject
{
	GENERATED_BODY()

public:
	void Init(USpatialNetDriver* InNetDriver);

	void WorldWipe(const USpatialNetDriver::PostWorldWipeDelegate& Delegate);
	void DeleteEntities(const Worker_EntityQueryResponseOp& Op);
	void LoadSnapshot(const FString& SnapshotName);

private:
	UPROPERTY()
	USpatialNetDriver* NetDriver;

	UPROPERTY()
	UGlobalStateManager* GlobalStateManager;

	UPROPERTY()
	USpatialReceiver* Receiver;
};
