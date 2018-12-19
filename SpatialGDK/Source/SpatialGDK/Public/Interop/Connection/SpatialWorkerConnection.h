// Copyright (c) Improbable Worlds Ltd, All Rights Reserved
#pragma once

#include "Interop/Connection/ConnectionConfig.h"

#include <WorkerSDK/improbable/c_schema.h>
#include <WorkerSDK/improbable/c_worker.h>

#include "SpatialWorkerConnection.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpatialWorkerConnection, Log, All);

DECLARE_DELEGATE(FOnConnectedDelegate);
DECLARE_DELEGATE_OneParam(FOnConnectFailedDelegate, const FString&);

UCLASS()
class SPATIALGDK_API USpatialWorkerConnection : public UObject
{

	GENERATED_BODY()

public:
	virtual void FinishDestroy() override;
	void DestroyConnection();

	void Connect(bool bConnectAsClient);

	FORCEINLINE bool IsConnected() { return bIsConnected; }

	// Worker Connection Interface
	Worker_OpList* GetOpList();
	Worker_RequestId SendReserveEntityIdRequest();
	Worker_RequestId SendReserveEntityIdsRequest(uint32_t NumOfEntities);
	Worker_RequestId SendCreateEntityRequest(uint32_t ComponentCount, const Worker_ComponentData* Components, const Worker_EntityId* EntityId);
	Worker_RequestId SendDeleteEntityRequest(Worker_EntityId EntityId);
	void SendComponentUpdate(Worker_EntityId EntityId, const Worker_ComponentUpdate* ComponentUpdate);
	Worker_RequestId SendCommandRequest(Worker_EntityId EntityId, const Worker_CommandRequest* Request, uint32_t CommandId);
	void SendCommandResponse(Worker_RequestId RequestId, const Worker_CommandResponse* Response);
	void SendLogMessage(const uint8_t Level, const char* LoggerName, const char* Message);
	void SendComponentInterest(Worker_EntityId EntityId, const TArray<Worker_InterestOverride>& ComponentInterest);
	FString GetWorkerId() const;
	Worker_RequestId SendEntityQueryRequest(const Worker_EntityQuery* EntiyQuery);

	FOnConnectedDelegate OnConnected;
	FOnConnectFailedDelegate OnConnectFailed;

	FReceptionistConfig ReceptionistConfig;
	FLocatorConfig LocatorConfig;

private:
	void ConnectToReceptionist(bool bConnectAsClient);
	void ConnectToLocator();

	Worker_ConnectionParameters CreateConnectionParameters(FConnectionConfig& Config);
	bool ShouldConnectWithLocator();

	void GetAndPrintConnectionFailureMessage();

	Worker_Connection* WorkerConnection;
	Worker_Locator* WorkerLocator;

	bool bIsConnected;
};
