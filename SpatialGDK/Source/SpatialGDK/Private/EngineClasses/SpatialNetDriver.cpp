// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "EngineClasses/SpatialNetDriver.h"

#include "EngineGlobals.h"
#include "Engine/ActorChannel.h"
#include "Engine/ChildConnection.h"
#include "Engine/Engine.h"
#include "Engine/NetworkObjectList.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameNetworkManager.h"
#include "Net/DataReplication.h"
#include "Net/RepLayout.h"
#include "SocketSubsystem.h"
#include "UObject/UObjectIterator.h"

#include "Interop/Connection/SpatialWorkerConnection.h"
#include "Interop/GlobalStateManager.h"
#include "Interop/SnapshotManager.h"
#include "Interop/SpatialPlayerSpawner.h"
#include "Interop/SpatialReceiver.h"
#include "Interop/SpatialSender.h"
#include "Interop/SpatialTypebindingManager.h"
#include "Interop/SpatialDispatcher.h"
#include "EngineClasses/SpatialActorChannel.h"
#include "EngineClasses/SpatialGameInstance.h"
#include "EngineClasses/SpatialNetConnection.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "EngineClasses/SpatialPendingNetGame.h"
#include "SpatialConstants.h"
#include "Utils/EntityRegistry.h"

DEFINE_LOG_CATEGORY(LogSpatialOSNetDriver);

bool USpatialNetDriver::InitBase(bool bInitAsClient, FNetworkNotify* InNotify, const FURL& URL, bool bReuseAddressAndPort, FString& Error)
{
	if (!Super::InitBase(bInitAsClient, InNotify, URL, bReuseAddressAndPort, Error))
	{
		return false;
	}

	bConnectAsClient = bInitAsClient;
	bAuthoritativeDestruction = true;

	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &USpatialNetDriver::OnMapLoaded);

	// Make absolutely sure that the actor channel that we are using is our Spatial actor channel
	ChannelClasses[CHTYPE_Actor] = USpatialActorChannel::StaticClass();

	TypebindingManager = NewObject<USpatialTypebindingManager>();
	TypebindingManager->Init(this);

	// Extract the snapshot to load (if any) from the map URL so that once we are connected to a deployment we can load that snapshot into the Spatial deployment.
	SnapshotToLoad = URL.GetOption(*SpatialConstants::SnapshotURLOption, TEXT(""));

	// We do this here straight away to trigger LoadMap.
	if (bInitAsClient)
	{
		FWorldContext* WorldContext = GEngine->GetWorldContextFromPendingNetGameNetDriver(this);
		check(WorldContext);

		// Here we need to fake a few things to start ticking the level travel on client.
		if (WorldContext->PendingNetGame)
		{
			WorldContext->PendingNetGame->bSuccessfullyConnected = true;
			WorldContext->PendingNetGame->bSentJoinRequest = false;
		}

		// If the URL has not specified to keep the clients connection then we should create a new one.
		bPersistSpatialConnection = URL.HasOption(*SpatialConstants::ClientsStayConnectedURLOption);
	}
	else
	{
		// Servers should never disconnect from a deployment.
		bPersistSpatialConnection = true;

		// The server should already have a world.
		OnMapLoaded(GetWorld());
	}

	return true;
}

void USpatialNetDriver::PostInitProperties()
{
	Super::PostInitProperties();

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		// GuidCache will be allocated as an FNetGUIDCache above. To avoid an engine code change, we re-do it with the Spatial equivalent.
		GuidCache = MakeShareable(new FSpatialNetGUIDCache(this));
	}
}

void USpatialNetDriver::OnMapLoaded(UWorld* LoadedWorld)
{
	if (LoadedWorld->GetNetDriver() != this)
	{
		// In PIE, if we have more than 2 clients, then OnMapLoaded is going to be triggered once each client loads the world.
		// As the delegate is a global variable, it triggers all 3 USpatialNetDriver::OnMapLoaded callbacks. As a result, we should
		// make sure that the net driver of this world is in fact us.
		return;
	}

	if (Connection)
	{
		// If the connection already exists in this NetDriver we don't need to continue this function.
		return;
	}

	// Handle Spatial connection configurations.
	UE_LOG(LogSpatialOSNetDriver, Log, TEXT("Loaded Map %s. Connecting to SpatialOS."), *LoadedWorld->GetName());

	// Set the timer manager.
	TimerManager = &LoadedWorld->GetTimerManager();

	// Set up manager objects.
	EntityRegistry = NewObject<UEntityRegistry>(this);

	USpatialGameInstance* GameInstance = Cast<USpatialGameInstance>(GetWorld()->GetGameInstance());

	if (!GameInstance)
	{
		UE_LOG(LogSpatialOSNetDriver, Error, TEXT("A SpatialGameInstance is required. Make sure your game's GameInstance inherits from SpatialGameInstance"));
		return;
	}

	if (!bPersistSpatialConnection)
	{
		// Destroy the old connection
		GameInstance->SpatialConnection->DestroyConnection();

		// Create a new SpatialWorkerConnection in the SpatialGameInstance.
		GameInstance->CreateNewSpatialWorkerConnection();
	}

	// Grab the SpatialWorkerConnection from the SpatialGameInstance (stored there for persistence in server travel). 
	Connection = GameInstance->SpatialConnection;

	if (LoadedWorld->URL.HasOption(TEXT("locator")))
	{
		Connection->LocatorConfig.ProjectName = LoadedWorld->URL.GetOption(TEXT("project="), TEXT(""));
		Connection->LocatorConfig.DeploymentName = LoadedWorld->URL.GetOption(TEXT("deployment="), TEXT(""));
		Connection->LocatorConfig.LoginToken = LoadedWorld->URL.GetOption(TEXT("token="), TEXT(""));
		Connection->LocatorConfig.UseExternalIp = true;
	}
	else
	{
		// Check for overrides in the travel URL.
		if (!LoadedWorld->URL.Host.IsEmpty())
		{
			Connection->ReceptionistConfig.ReceptionistHost = LoadedWorld->URL.Host;
			Connection->ReceptionistConfig.ReceptionistPort = LoadedWorld->URL.Port;
		}

		bool bHasUseExternalIpOption = LoadedWorld->URL.HasOption(TEXT("useExternalIpForBridge"));
		if (bHasUseExternalIpOption)
		{
			FString UseExternalIpOption = LoadedWorld->URL.GetOption(TEXT("useExternalIpForBridge"), TEXT(""));
			if (UseExternalIpOption.Equals(TEXT("false"), ESearchCase::IgnoreCase))
			{
				Connection->ReceptionistConfig.UseExternalIp = false;
			}
			else
			{
				Connection->ReceptionistConfig.UseExternalIp = true;
			}
		}
	}

	Connect();
}

void USpatialNetDriver::Connect()
{
	Connection->OnConnected.BindLambda([this]
	{
		OnMapLoadedAndConnected();
	});
	Connection->OnConnectFailed.BindLambda([this](const FString& Reason)
	{
		OnConnectFailed(Reason);
	});

	Connection->Connect(bConnectAsClient);
}

void USpatialNetDriver::OnMapLoadedAndConnected()
{
	UE_LOG(LogSpatialOSNetDriver, Log, TEXT("Connected to SpatialOS and map has been loaded."));

	SpatialOutputDevice = MakeUnique<FSpatialOutputDevice>(Connection, TEXT("Unreal"));

	Dispatcher = NewObject<USpatialDispatcher>();
	Sender = NewObject<USpatialSender>();
	Receiver = NewObject<USpatialReceiver>();
	GlobalStateManager = NewObject<UGlobalStateManager>();
	PlayerSpawner = NewObject<USpatialPlayerSpawner>();
	StaticComponentView = NewObject<USpatialStaticComponentView>();
	SnapshotManager = NewObject<USnapshotManager>();

	PlayerSpawner->Init(this, TimerManager);

	// Each connection stores a URL with various optional settings (host, port, map, netspeed...)
	// We currently don't make use of any of these as some are meaningless in a SpatialOS world, and some are less of a priority.
	// So for now we just give the connection a dummy url, might change in the future.
	FURL DummyURL;

	// If we're the server, we will spawn the special Spatial connection that will route all updates to SpatialOS.
	// There may be more than one of these connections in the future for different replication conditions.
	if (!ServerConnection)
	{
		USpatialNetConnection* NetConnection = NewObject<USpatialNetConnection>(GetTransientPackage(), NetConnectionClass);
		check(NetConnection);

		ISocketSubsystem* SocketSubsystem = GetSocketSubsystem();
		TSharedRef<FInternetAddr> FromAddr = SocketSubsystem->CreateInternetAddr();

		NetConnection->InitRemoteConnection(this, nullptr, DummyURL, *FromAddr, USOCK_Open);
		Notify->NotifyAcceptedConnection(NetConnection);
		NetConnection->bReliableSpatialConnection = true;
		AddClientConnection(NetConnection);
		//Since this is not a "real" client connection, we immediately pretend that it is fully logged on.
		NetConnection->SetClientLoginState(EClientLoginState::Welcomed);
	}

	PackageMap = Cast<USpatialPackageMapClient>(GetSpatialOSNetConnection()->PackageMap);

	Dispatcher->Init(this);
	Sender->Init(this);
	Receiver->Init(this, TimerManager);
	GlobalStateManager->Init(this, TimerManager);
	SnapshotManager->Init(this);

	// Bind the ProcessServerTravel delegate to the spatial variant. This ensures that if ServerTravel is called and Spatial networking is enabled, we can travel properly.
	GetWorld()->SpatialProcessServerTravelDelegate.BindStatic(SpatialProcessServerTravel);

	// If we're the client, we can now ask the server to spawn our controller.
	if (ServerConnection)
	{
		// If we know the GSM is already accepting players, simply spawn.
		if (GlobalStateManager->bAcceptingPlayers && GetWorld()->RemovePIEPrefix(GlobalStateManager->DeploymentMapURL) == GetWorld()->RemovePIEPrefix(GetWorld()->URL.Map))
		{
			PlayerSpawner->SendPlayerSpawnRequest();
		}
		else
		{
			// Register our interest in spawning.
			bWaitingForAcceptingPlayersToSpawn = true;

			// Begin querying the state of the GSM so we know the state of AcceptingPlayers.
			GlobalStateManager->QueryGSM(true /*bRetryUntilAcceptingPlayers*/);
		}
	}

	// Here if we are a server and this is server travel (there is a snapshot to load) we want to load the snapshot.
	if (!ServerConnection && !SnapshotToLoad.IsEmpty() && Cast<USpatialGameInstance>(GetWorld()->GetGameInstance())->bResponsibleForSnapshotLoading)
	{
		UE_LOG(LogSpatialOSNetDriver, Log, TEXT("Worker authoriative over the GSM is loading snapshot: %s"), *SnapshotToLoad);
		SnapshotManager->LoadSnapshot(SnapshotToLoad);

		// Once we've finished loading the snapshot we must update our bResponsibleForSnapshotLoading in-case we do not gain authority over the new GSM.
		Cast<USpatialGameInstance>(GetWorld()->GetGameInstance())->bResponsibleForSnapshotLoading = false;
	}
}

void USpatialNetDriver::OnConnectFailed(const FString& Reason)
{
	UE_LOG(LogSpatialOSNetDriver, Error, TEXT("Could not connect to SpatialOS. Reason: %s"), *Reason);
}

void USpatialNetDriver::OnAcceptingPlayersChanged(bool bAcceptingPlayers)
{
	// If the deployment is now accepting players and we are waiting to spawn. Spawn.
	if (bWaitingForAcceptingPlayersToSpawn && bAcceptingPlayers)
	{
		// If we have the correct map loaded then ask to spawn.
		if (GetWorld()->RemovePIEPrefix(GlobalStateManager->DeploymentMapURL) == GetWorld()->RemovePIEPrefix(GetWorld()->URL.Map))
		{
			PlayerSpawner->SendPlayerSpawnRequest();

			// Unregister our interest in spawning on accepting players changing again.
			bWaitingForAcceptingPlayersToSpawn = false;
		}
		else // Load the correct map based on the GSM URL
		{
			UE_LOG(LogSpatialOSNetDriver, Log, TEXT("Welcomed by server (Level: %s)"), *GlobalStateManager->DeploymentMapURL);

			// Extract map name and options
			FWorldContext* WorldContext = GEngine->GetWorldContextFromWorld(GetWorld());
			check(WorldContext);

			FURL RedirectURL = FURL(&WorldContext->LastURL, *GlobalStateManager->DeploymentMapURL, (ETravelType)WorldContext->TravelType);
			RedirectURL.Host = WorldContext->LastURL.Host;
			RedirectURL.Port = WorldContext->LastURL.Port;
			RedirectURL.Op.Append(WorldContext->LastURL.Op);

			WorldContext->TravelURL = RedirectURL.ToString();
		}
	}
}

// NOTE: This method is a clone of the ProcessServerTravel located in GameModeBase with modifications to support Spatial.
// Will be called via a delegate that has been set in the UWorld instead of the original in GameModeBase.
void USpatialNetDriver::SpatialProcessServerTravel(const FString& URL, bool bAbsolute, AGameModeBase* GameMode)
{
#if WITH_SERVER_CODE

	UWorld* World = GameMode->GetWorld();
	USpatialNetDriver* NetDriver = Cast<USpatialNetDriver>(World->GetNetDriver());

	if (!NetDriver->StaticComponentView->HasAuthority(NetDriver->GlobalStateManager->GlobalStateManagerEntityId, SpatialConstants::DEPLOYMENT_MAP_COMPONENT_ID))
	{
		// TODO: UNR-678 Send a command to the GSM to initiate server travel on the correct server.
		UE_LOG(LogGameMode, Warning, TEXT("Trying to server travel on a server which is not authoritative over the GSM."));
		return;
	}

	// Register that this server will be responsible for loading the snapshot once it has finished wiping the world + loading the new map.
	Cast<USpatialGameInstance>(World->GetGameInstance())->bResponsibleForSnapshotLoading = true;

	GameMode->StartToLeaveMap();

	// Force an old style load screen if the server has been up for a long time so that TimeSeconds doesn't overflow and break everything
	bool bSeamless = (GameMode->bUseSeamlessTravel && World->TimeSeconds < 172800.0f); // 172800 seconds == 48 hours

	FString NextMap;
	if (URL.ToUpper().Contains(TEXT("?RESTART")))
	{
		NextMap = UWorld::RemovePIEPrefix(GameMode->GetOutermost()->GetName());
	}
	else
	{
		int32 OptionStart = URL.Find(TEXT("?"));
		if (OptionStart == INDEX_NONE)
		{
			NextMap = URL;
		}
		else
		{
			NextMap = URL.Left(OptionStart);
		}
	}

	FGuid NextMapGuid = UEngine::GetPackageGuid(FName(*NextMap), World->IsPlayInEditor());

	FString NewURL = URL;

	bool SnapshotOption = NewURL.Contains(TEXT("snapshot="));
	if (!SnapshotOption)
	{
		// In the case that we don't have a snapshot option, we assume the map name will be the snapshot name.
		// Remove any leading path before the map name.
		FString Path;
		FString MapName;
		NextMap.Split(TEXT("/"), &Path, &MapName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		NewURL.Append(FString::Printf(TEXT("?snapshot=%s"), *MapName));
	}

	// Notify clients we're switching level and give them time to receive.
	FString URLMod = NewURL;
	APlayerController* LocalPlayer = GameMode->ProcessClientTravel(URLMod, NextMapGuid, bSeamless, bAbsolute);

	// We can't have the NextURL set this early when using SpatialProcessServerTravel so empty the string here.
	// The reason for this is, on the next WorldTick the current World and NetDriver will be unloaded.
	// During the deployment wipe we are waiting for an entity query response of all entities in the deployment.
	// If the NetDriver has been unloaded in that time, the delegate to delete all these entities will be lost and server travel will fail.
	World->NextURL.Empty();

	ENetMode NetMode = GameMode->GetNetMode();

	// FinishServerTravel - Allows Unreal to finish it's normal server travel.
	USpatialNetDriver::PostWorldWipeDelegate FinishServerTravel;
	FinishServerTravel.BindLambda([World, NetDriver, NewURL, NetMode, bSeamless, bAbsolute]
	{
		UE_LOG(LogGameMode, Log, TEXT("SpatialServerTravel - Finishing Server Travel : %s"), *NewURL);
		check(World);
		World->NextURL = NewURL;

		if (bSeamless)
		{
			World->SeamlessTravel(World->NextURL, bAbsolute);
			World->NextURL = TEXT("");
		}
		// Switch immediately if not networking.
		else if (NetMode != NM_DedicatedServer && NetMode != NM_ListenServer)
		{
			World->NextSwitchCountdown = 0.0f;
		}
	});

	UE_LOG(LogGameMode, Log, TEXT("SpatialServerTravel - Wiping the world"), *NewURL);
	NetDriver->WipeWorld(FinishServerTravel);
#endif // WITH_SERVER_CODE
}

bool USpatialNetDriver::IsLevelInitializedForActor(const AActor* InActor, const UNetConnection* InConnection) const
{
	//In our case, the connection is not specific to a client. Thus, it's not relevant whether the level is initialized.
	return true;
}

void USpatialNetDriver::NotifyActorDestroyed(AActor* ThisActor, bool IsSeamlessTravel /*= false*/)
{
	// Intentionally does not call Super::NotifyActorDestroyed, but most of the functionality is copied here 
	// The native UNetDriver would normally store destruction info here for "StartupActors" - replicated actors
	// placed in the level, but we handle this flow differently in the GDK

	// Remove the actor from the property tracker map
	RepChangedPropertyTrackerMap.Remove(ThisActor);

	const bool bIsServer = ServerConnection == nullptr;

	if (bIsServer)
	{
		for (int32 i = ClientConnections.Num() - 1; i >= 0; i--)
		{
			UNetConnection* ClientConnection = ClientConnections[i];
			if (ThisActor->bNetTemporary)
			{
				ClientConnection->SentTemporaries.Remove(ThisActor);
			}

			if (UActorChannel* Channel = ClientConnection->ActorChannelMap().FindRef(ThisActor))
			{
				check(Channel->OpenedLocally);
				Channel->bClearRecentActorRefs = false;
				Channel->Close();
			}

			// Remove it from any dormancy lists
			ClientConnection->DormantReplicatorMap.Remove(ThisActor);
		}
	}

	// Remove this actor from the network object list
	GetNetworkObjectList().Remove(ThisActor);

	// Remove from renamed list if destroyed
	RenamedStartupActors.Remove(ThisActor->GetFName());
}

//SpatialGDK: Functions in the ifdef block below are modified versions of the UNetDriver:: implementations.
#if WITH_SERVER_CODE

// Returns true if this actor should replicate to *any* of the passed in connections
static FORCEINLINE_DEBUGGABLE bool IsActorRelevantToConnection(const AActor* Actor, const TArray<FNetViewer>& ConnectionViewers)
{
	//SpatialGDK: Currently we're just returning true as a worker replicates all the known actors in our design.
	// We might make some exceptions in the future, so keeping this function.
	return true;
}

// Returns true if this actor is considered dormant (and all properties caught up) to the current connection
static FORCEINLINE_DEBUGGABLE bool IsActorDormant(FNetworkObjectInfo* ActorInfo, UNetConnection* Connection)
{
	// If actor is already dormant on this channel, then skip replication entirely
	return ActorInfo->DormantConnections.Contains(Connection);
}

// Returns true if this actor wants to go dormant for a particular connection
static FORCEINLINE_DEBUGGABLE bool ShouldActorGoDormant(AActor* Actor, const TArray<FNetViewer>& ConnectionViewers, UActorChannel* Channel, const float Time, const bool bLowNetBandwidth)
{
	if (Actor->NetDormancy <= DORM_Awake || !Channel || Channel->bPendingDormancy || Channel->Dormant)
	{
		// Either shouldn't go dormant, or is already dormant
		return false;
	}

	if (Actor->NetDormancy == DORM_DormantPartial)
	{
		for (int32 viewerIdx = 0; viewerIdx < ConnectionViewers.Num(); viewerIdx++)
		{
			if (!Actor->GetNetDormancy(ConnectionViewers[viewerIdx].ViewLocation, ConnectionViewers[viewerIdx].ViewDir, ConnectionViewers[viewerIdx].InViewer, ConnectionViewers[viewerIdx].ViewTarget, Channel, Time, bLowNetBandwidth))
			{
				return false;
			}
		}
	}

	return true;
}

int32 USpatialNetDriver::ServerReplicateActors_PrepConnections(const float DeltaSeconds)
{
	int32 NumClientsToTick = ClientConnections.Num();

	bool bFoundReadyConnection = false;

	for (int32 ConnIdx = 0; ConnIdx < ClientConnections.Num(); ConnIdx++)
	{
		USpatialNetConnection* SpatialConnection = Cast<USpatialNetConnection>(ClientConnections[ConnIdx]);
		check(SpatialConnection);
		check(SpatialConnection->State == USOCK_Pending || SpatialConnection->State == USOCK_Open || SpatialConnection->State == USOCK_Closed);
		checkSlow(SpatialConnection->GetUChildConnection() == NULL);

		// Handle not ready channels.
		//@note: we cannot add Connection->IsNetReady(0) here to check for saturation, as if that's the case we still want to figure out the list of relevant actors
		//			to reset their NetUpdateTime so that they will get sent as soon as the connection is no longer saturated
		AActor* OwningActor = SpatialConnection->OwningActor;

		//SpatialGDK: We allow a connection without an owner to process if it's meant to be the connection to the fake SpatialOS client.
		if ((SpatialConnection->bReliableSpatialConnection || OwningActor != NULL) && SpatialConnection->State == USOCK_Open && (SpatialConnection->Driver->Time - SpatialConnection->LastReceiveTime < 1.5f))
		{
			check(SpatialConnection->bReliableSpatialConnection || World == OwningActor->GetWorld());

			bFoundReadyConnection = true;

			// the view target is what the player controller is looking at OR the owning actor itself when using beacons
			SpatialConnection->ViewTarget = SpatialConnection->PlayerController ? SpatialConnection->PlayerController->GetViewTarget() : OwningActor;

			for (int32 ChildIdx = 0; ChildIdx < SpatialConnection->Children.Num(); ChildIdx++)
			{
				UNetConnection *Child = SpatialConnection->Children[ChildIdx];
				APlayerController* ChildPlayerController = Child->PlayerController;
				if (ChildPlayerController != NULL)
				{
					Child->ViewTarget = ChildPlayerController->GetViewTarget();
				}
				else
				{
					Child->ViewTarget = NULL;
				}
			}
		}
		else
		{
			SpatialConnection->ViewTarget = NULL;
			for (int32 ChildIdx = 0; ChildIdx < SpatialConnection->Children.Num(); ChildIdx++)
			{
				SpatialConnection->Children[ChildIdx]->ViewTarget = NULL;
			}
		}
	}

	return bFoundReadyConnection ? NumClientsToTick : 0;
}

int32 USpatialNetDriver::ServerReplicateActors_PrioritizeActors(UNetConnection* InConnection, const TArray<FNetViewer>& ConnectionViewers, const TArray<FNetworkObjectInfo*> ConsiderList, const bool bCPUSaturated, FActorPriority*& OutPriorityList, FActorPriority**& OutPriorityActors)
{
	// Get list of visible/relevant actors.

	NetTag++;
	InConnection->TickCount++;

	// Set up to skip all sent temporary actors
	for (int32 j = 0; j < InConnection->SentTemporaries.Num(); j++)
	{
		InConnection->SentTemporaries[j]->NetTag = NetTag;
	}

	int32 FinalSortedCount = 0;
	int32 DeletedCount = 0;

	const int32 MaxSortedActors = ConsiderList.Num() + DestroyedStartupOrDormantActors.Num();
	if (MaxSortedActors > 0)
	{
		OutPriorityList = new (FMemStack::Get(), MaxSortedActors) FActorPriority;
		OutPriorityActors = new (FMemStack::Get(), MaxSortedActors) FActorPriority*;

		AGameNetworkManager* const NetworkManager = World->NetworkManager;
		const bool bLowNetBandwidth = NetworkManager ? NetworkManager->IsInLowBandwidthMode() : false;

		for (FNetworkObjectInfo* ActorInfo : ConsiderList)
		{
			AActor* Actor = ActorInfo->Actor;

			UActorChannel* Channel = InConnection->ActorChannelMap().FindRef(Actor);

			UNetConnection* PriorityConnection = InConnection;

			// Skip Actor if dormant
			if (IsActorDormant(ActorInfo, InConnection))
			{
				continue;
			}

			// See of actor wants to try and go dormant
			if (ShouldActorGoDormant(Actor, ConnectionViewers, Channel, Time, bLowNetBandwidth))
			{
				// Channel is marked to go dormant now once all properties have been replicated (but is not dormant yet)
				Channel->StartBecomingDormant();
			}

			// SpatialGDK: This actor should only be replicated if GetNetworkConnection() matches this connection. However, if this actor doesn't have a connection
			// (which implies that it's owned by the server rather than a client), then it should fall back to the "catch all" SpatialOS connection which is
			// ClientConnections[0]. The below condition means that each actor should only be replicated once, unless "ClientConnections" contain duplicates,
			// which should never happen.
			UNetConnection* ActorConnection = Actor->GetNetConnection();
			if (ActorConnection != InConnection)
			{
				if (ActorConnection == nullptr && InConnection == ClientConnections[0])
				{
					UE_LOG(LogSpatialOSNetDriver, Verbose, TEXT("Actor %s will be replicated on the catch-all connection"), *Actor->GetName());
				}
				else
				{
					continue;
				}
			}
			else
			{
				UE_LOG(LogSpatialOSNetDriver, Verbose, TEXT("Actor %s will be replicated on the connection %s"), *Actor->GetName(), *InConnection->GetName());
			}

			//SpatialGDK: Here, Unreal does initial relevancy checking and level load checking.
			// We have removed the level load check because it doesn't apply.
			// Relevancy checking is also mostly just a pass through, might be removed later.
			if (!IsActorRelevantToConnection(Actor, ConnectionViewers))
			{
				// If not relevant (and we don't have a channel), skip
				continue;
			}

			// Actor is relevant to this connection, add it to the list
			// NOTE - We use NetTag to make sure SentTemporaries didn't already mark this actor to be skipped
			if (Actor->NetTag != NetTag)
			{
				UE_LOG(LogNetTraffic, Log, TEXT("Consider %s alwaysrelevant %d frequency %f "), *Actor->GetName(), Actor->bAlwaysRelevant, Actor->NetUpdateFrequency);

				Actor->NetTag = NetTag;

				OutPriorityList[FinalSortedCount] = FActorPriority(PriorityConnection, Channel, ActorInfo, ConnectionViewers, bLowNetBandwidth);
				OutPriorityActors[FinalSortedCount] = OutPriorityList + FinalSortedCount;

				FinalSortedCount++;

				if (DebugRelevantActors)
				{
					LastPrioritizedActors.Add(Actor);
				}
			}
		}

		// Add in deleted actors
		for (auto It = InConnection->GetDestroyedStartupOrDormantActorGUIDs().CreateIterator(); It; ++It)
		{
			FActorDestructionInfo& DInfo = *DestroyedStartupOrDormantActors.FindChecked(*It);
			OutPriorityList[FinalSortedCount] = FActorPriority(InConnection, &DInfo, ConnectionViewers);
			OutPriorityActors[FinalSortedCount] = OutPriorityList + FinalSortedCount;
			FinalSortedCount++;
			DeletedCount++;
		}

		// Sort by priority
		Sort(OutPriorityActors, FinalSortedCount, FCompareFActorPriority());
	}

	UE_LOG(LogNetTraffic, Log, TEXT("ServerReplicateActors_PrioritizeActors: Potential %04i ConsiderList %03i FinalSortedCount %03i"), MaxSortedActors, ConsiderList.Num(), FinalSortedCount);

	return FinalSortedCount;
}

int32 USpatialNetDriver::ServerReplicateActors_ProcessPrioritizedActors(UNetConnection* InConnection, const TArray<FNetViewer>& ConnectionViewers, FActorPriority** PriorityActors, const int32 FinalSortedCount, int32& OutUpdated)
{
	if (!InConnection->IsNetReady(0))
	{
		// Connection saturated, don't process any actors
		return 0;
	}

	int32 ActorUpdatesThisConnection = 0;
	int32 ActorUpdatesThisConnectionSent = 0;
	int32 FinalRelevantCount = 0;

	for (int32 j = 0; j < FinalSortedCount; j++)
	{
		// Deletion entry
		if (PriorityActors[j]->ActorInfo == NULL && PriorityActors[j]->DestructionInfo)
		{
			// Make sure client has streaming level loaded
			if (PriorityActors[j]->DestructionInfo->StreamingLevelName != NAME_None && !InConnection->ClientVisibleLevelNames.Contains(PriorityActors[j]->DestructionInfo->StreamingLevelName))
			{
				// This deletion entry is for an actor in a streaming level the connection doesn't have loaded, so skip it
				continue;
			}

			UActorChannel* Channel = (UActorChannel*)InConnection->CreateChannel(CHTYPE_Actor, 1);
			if (Channel)
			{
				FinalRelevantCount++;
				UE_LOG(LogNetTraffic, Log, TEXT("Server replicate actor creating destroy channel for NetGUID <%s,%s> Priority: %d"), *PriorityActors[j]->DestructionInfo->NetGUID.ToString(), *PriorityActors[j]->DestructionInfo->PathName, PriorityActors[j]->Priority);

				InConnection->GetDestroyedStartupOrDormantActorGUIDs().Remove(PriorityActors[j]->DestructionInfo->NetGUID); // Remove from connections to-be-destroyed list (close bunch of reliable, so it will make it there)
			}
			continue;
		}

#if !( UE_BUILD_SHIPPING || UE_BUILD_TEST )
		static IConsoleVariable* DebugObjectCvar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.PackageMap.DebugObject"));
		static IConsoleVariable* DebugAllObjectsCvar = IConsoleManager::Get().FindConsoleVariable(TEXT("net.PackageMap.DebugAll"));
		if (PriorityActors[j]->ActorInfo &&
			((DebugObjectCvar && !DebugObjectCvar->GetString().IsEmpty() && PriorityActors[j]->ActorInfo->Actor->GetName().Contains(DebugObjectCvar->GetString())) ||
			(DebugAllObjectsCvar && DebugAllObjectsCvar->GetInt() != 0)))
		{
			UE_LOG(LogNetPackageMap, Log, TEXT("Evaluating actor for replication %s"), *PriorityActors[j]->ActorInfo->Actor->GetName());
		}
#endif

		// Normal actor replication
		USpatialActorChannel* Channel = Cast<USpatialActorChannel>(PriorityActors[j]->Channel);
		UE_LOG(LogNetTraffic, Log, TEXT(" Maybe Replicate %s"), *PriorityActors[j]->ActorInfo->Actor->GetName());
		if (!Channel || Channel->Actor) //make sure didn't just close this channel
		{
			AActor* Actor = PriorityActors[j]->ActorInfo->Actor;
			bool bIsRelevant = false;

			//SpatialGDK: Here, Unreal would check (again) whether an actor is relevant. Removed such checks.
			// only check visibility on already visible actors every 1.0 + 0.5R seconds
			// bTearOff actors should never be checked
			if (!Actor->GetTearOff() && (!Channel || Time - Channel->RelevantTime > 1.f))
			{
				if (IsActorRelevantToConnection(Actor, ConnectionViewers))
				{
					bIsRelevant = true;
				}
				else if (DebugRelevantActors)
				{
					LastNonRelevantActors.Add(Actor);
				}
			}

			// if the actor is now relevant or was recently relevant
			const bool bIsRecentlyRelevant = bIsRelevant || (Channel && Time - Channel->RelevantTime < RelevantTimeout);

			if (bIsRecentlyRelevant)
			{
				FinalRelevantCount++;

				// Find or create the channel for this actor.
				// we can't create the channel if the client is in a different world than we are
				// or the package map doesn't support the actor's class/archetype (or the actor itself in the case of serializable actors)
				// or it's an editor placed actor and the client hasn't initialized the level it's in
				if (Channel == nullptr && GuidCache->SupportsObject(Actor->GetClass()) && GuidCache->SupportsObject(Actor->IsNetStartupActor() ? Actor : Actor->GetArchetype()))
				{
					// If we're a singleton, and don't have a channel, defer to GSM
					if (Actor->GetClass()->HasAnySpatialClassFlags(SPATIALCLASS_Singleton))
					{
						Channel = GlobalStateManager->AddSingleton(Actor);
					}
					else
					{
						// Create a new channel for this actor.
						Channel = (USpatialActorChannel*)InConnection->CreateChannel(CHTYPE_Actor, 1);
						if (Channel)
						{
							Channel->SetChannelActor(Actor);
						}
						// if we couldn't replicate it for a reason that should be temporary, and this Actor is updated very infrequently, make sure we update it again soon
						else if (Actor->NetUpdateFrequency < 1.0f)
						{
							UE_LOG(LogNetTraffic, Log, TEXT("Unable to replicate %s"), *Actor->GetName());
							PriorityActors[j]->ActorInfo->NextUpdateTime = Actor->GetWorld()->TimeSeconds + 0.2f * FMath::FRand();
						}
					}
				}

				if (Channel)
				{
					// if it is relevant then mark the channel as relevant for a short amount of time
					if (bIsRelevant)
					{
						Channel->RelevantTime = Time + 0.5f * FMath::SRand();
					}
					// if the channel isn't saturated
					if (Channel->IsNetReady(0))
					{
						// replicate the actor
						UE_LOG(LogNetTraffic, Log, TEXT("- Replicate %s. %d"), *Actor->GetName(), PriorityActors[j]->Priority);
						if (DebugRelevantActors)
						{
							LastRelevantActors.Add(Actor);
						}

						if (Channel->ReplicateActor())
						{
							ActorUpdatesThisConnectionSent++;
							if (DebugRelevantActors)
							{
								LastSentActors.Add(Actor);
							}

							// Calculate min delta (max rate actor will update), and max delta (slowest rate actor will update)
							const float MinOptimalDelta = 1.0f / Actor->NetUpdateFrequency;
							const float MaxOptimalDelta = FMath::Max(1.0f / Actor->MinNetUpdateFrequency, MinOptimalDelta);
							const float DeltaBetweenReplications = (World->TimeSeconds - PriorityActors[j]->ActorInfo->LastNetReplicateTime);

							// Choose an optimal time, we choose 70% of the actual rate to allow frequency to go up if needed
							PriorityActors[j]->ActorInfo->OptimalNetUpdateDelta = FMath::Clamp(DeltaBetweenReplications * 0.7f, MinOptimalDelta, MaxOptimalDelta);
							PriorityActors[j]->ActorInfo->LastNetReplicateTime = World->TimeSeconds;
						}
						ActorUpdatesThisConnection++;
						OutUpdated++;
					}

					// second check for channel saturation
					if (!InConnection->IsNetReady(0))
					{
						// We can bail out now since this connection is saturated, we'll return how far we got though
						return j;
					}
				}
			}

			// If the actor wasn't recently relevant, or if it was torn off, close the actor channel if it exists for this connection
			if ((!bIsRecentlyRelevant || Actor->GetTearOff()) && Channel != NULL)
			{
				// Non startup (map) actors have their channels closed immediately, which destroys them.
				// Startup actors get to keep their channels open.

				if (!Actor->IsNetStartupActor())
				{
					UE_LOG(LogNetTraffic, Log, TEXT("- Closing channel for no longer relevant actor %s"), *Actor->GetName());
					Channel->Close();
				}
			}
		}
	}

	return FinalSortedCount;
}
#endif

// SpatialGDK: This is a modified and simplified version of UNetDriver::ServerReplicateActors.
// In our implementation, connections on the server do not represent clients. They represent direct connections to SpatialOS.
// For this reason, things like ready checks, acks, throttling based on number of updated connections, interest management are irrelevant at this level.
int32 USpatialNetDriver::ServerReplicateActors(float DeltaSeconds)
{
#if WITH_SERVER_CODE
	if (ClientConnections.Num() == 0)
	{
		return 0;
	}

	check(World);

	int32 Updated = 0;

	// Bump the ReplicationFrame value to invalidate any properties marked as "unchanged" for this frame.
	ReplicationFrame++;

	const int32 NumClientsToTick = ServerReplicateActors_PrepConnections(DeltaSeconds);

	//SpatialGDK: This is a formality as there is at least one "perfect" Spatial connection in our design.
	if (NumClientsToTick == 0)
	{
		// No connections are ready this frame
		return 0;
	}

	AWorldSettings* WorldSettings = World->GetWorldSettings();

	bool bCPUSaturated = false;
	float ServerTickTime = GEngine->GetMaxTickRate(DeltaSeconds);
	if (ServerTickTime == 0.f)
	{
		ServerTickTime = DeltaSeconds;
	}
	else
	{
		ServerTickTime = 1.f / ServerTickTime;
		bCPUSaturated = DeltaSeconds > 1.2f * ServerTickTime;
	}

	TArray<FNetworkObjectInfo*> ConsiderList;
	ConsiderList.Reserve(GetNetworkObjectList().GetActiveObjects().Num());

	// Build the consider list (actors that are ready to replicate)
	ServerReplicateActors_BuildConsiderList(ConsiderList, ServerTickTime);
	FMemMark Mark(FMemStack::Get());

	for (int32 i = 0; i < ClientConnections.Num(); i++)
	{
		USpatialNetConnection* SpatialConnection = Cast<USpatialNetConnection>(ClientConnections[i]);
		check(SpatialConnection);

		// if this client shouldn't be ticked this frame
		if (i >= NumClientsToTick)
		{
			// SpatialGDK: This should not really happen in our case because we only replicate to SpatialOS and not to individual clients. Leaving the code here just in case.

			//UE_LOG(LogNet, Log, TEXT("skipping update to %s"),*Connection->GetName());
			// then mark each considered actor as bPendingNetUpdate so that they will be considered again the next frame when the connection is actually ticked
			for (int32 ConsiderIdx = 0; ConsiderIdx < ConsiderList.Num(); ConsiderIdx++)
			{
				AActor *Actor = ConsiderList[ConsiderIdx]->Actor;
				// if the actor hasn't already been flagged by another connection,
				if (Actor != NULL && !ConsiderList[ConsiderIdx]->bPendingNetUpdate)
				{
					// find the channel
					UActorChannel *Channel = SpatialConnection->ActorChannelMap().FindRef(Actor);
					// and if the channel last update time doesn't match the last net update time for the actor
					if (Channel != NULL && Channel->LastUpdateTime < ConsiderList[ConsiderIdx]->LastNetUpdateTime)
					{
						//UE_LOG(LogNet, Log, TEXT("flagging %s for a future update"),*Actor->GetName());
						// flag it for a pending update
						ConsiderList[ConsiderIdx]->bPendingNetUpdate = true;
					}
				}
			}
			// clear the time sensitive flag to avoid sending an extra packet to this connection
			SpatialConnection->TimeSensitive = false;
		}
		else if (SpatialConnection->bReliableSpatialConnection || SpatialConnection->ViewTarget)
		{
			// Make a list of viewers this connection should consider (this connection and children of this connection)
			TArray<FNetViewer>& ConnectionViewers = WorldSettings->ReplicationViewers;

			if (SpatialConnection->ViewTarget)
			{
				ConnectionViewers.Reset();
				new(ConnectionViewers)FNetViewer(SpatialConnection, DeltaSeconds);
				for (int32 ViewerIndex = 0; ViewerIndex < SpatialConnection->Children.Num(); ViewerIndex++)
				{
					if (SpatialConnection->Children[ViewerIndex]->ViewTarget != NULL)
					{
						new(ConnectionViewers)FNetViewer(SpatialConnection->Children[ViewerIndex], DeltaSeconds);
					}
				}
			}

			FMemMark RelevantActorMark(FMemStack::Get());

			FActorPriority* PriorityList = NULL;
			FActorPriority** PriorityActors = NULL;

			// Get a sorted list of actors for this connection
			const int32 FinalSortedCount = ServerReplicateActors_PrioritizeActors(SpatialConnection, ConnectionViewers, ConsiderList, bCPUSaturated, PriorityList, PriorityActors);

			// Process the sorted list of actors for this connection
			const int32 LastProcessedActor = ServerReplicateActors_ProcessPrioritizedActors(SpatialConnection, ConnectionViewers, PriorityActors, FinalSortedCount, Updated);

			// relevant actors that could not be processed this frame are marked to be considered for next frame
			for (int32 k = LastProcessedActor; k < FinalSortedCount; k++)
			{
				if (!PriorityActors[k]->ActorInfo)
				{
					// A deletion entry, skip it because we dont have anywhere to store a 'better give higher priority next time'
					continue;
				}

				AActor* Actor = PriorityActors[k]->ActorInfo->Actor;

				UActorChannel* Channel = PriorityActors[k]->Channel;

				UE_LOG(LogNetTraffic, Verbose, TEXT("Saturated. %s"), *Actor->GetName());
				if (Channel != NULL && Time - Channel->RelevantTime <= 1.f)
				{
					UE_LOG(LogNetTraffic, Log, TEXT(" Saturated. Mark %s NetUpdateTime to be checked for next tick"), *Actor->GetName());
					PriorityActors[k]->ActorInfo->bPendingNetUpdate = true;
				}
				else if (IsActorRelevantToConnection(Actor, ConnectionViewers))
				{
					// If this actor was relevant but didn't get processed, force another update for next frame
					UE_LOG(LogNetTraffic, Log, TEXT(" Saturated. Mark %s NetUpdateTime to be checked for next tick"), *Actor->GetName());
					PriorityActors[k]->ActorInfo->bPendingNetUpdate = true;
					if (Channel != NULL)
					{
						Channel->RelevantTime = Time + 0.5f * FMath::SRand();
					}
				}
			}
			RelevantActorMark.Pop();
			ConnectionViewers.Reset();
		}
	}

	// shuffle the list of connections if not all connections were ticked
	if (NumClientsToTick < ClientConnections.Num())
	{
		int32 NumConnectionsToMove = NumClientsToTick;
		while (NumConnectionsToMove > 0)
		{
			// move all the ticked connections to the end of the list so that the other connections are considered first for the next frame
			UNetConnection *ClientConnection = ClientConnections[0];
			ClientConnections.RemoveAt(0, 1);
			ClientConnections.Add(ClientConnection);
			NumConnectionsToMove--;
		}
	}
	Mark.Pop();

	if (DebugRelevantActors)
	{
		PrintDebugRelevantActors();
		LastPrioritizedActors.Empty();
		LastSentActors.Empty();
		LastRelevantActors.Empty();
		LastNonRelevantActors.Empty();

		DebugRelevantActors = false;
	}

	return Updated;
#else
	return 0;
#endif // WITH_SERVER_CODE
}

void USpatialNetDriver::TickDispatch(float DeltaTime)
{
	// Not calling Super:: on purpose.
	UNetDriver::TickDispatch(DeltaTime);

	if (Connection != nullptr && Connection->IsConnected())
	{
		Worker_OpList* OpList = Connection->GetOpList();

		Dispatcher->ProcessOps(OpList);

		Worker_OpList_Destroy(OpList);
	}
}

void USpatialNetDriver::ProcessRemoteFunction(
	AActor* Actor,
	UFunction* Function,
	void* Parameters,
	FOutParmRec* OutParms,
	FFrame* Stack,
	UObject* SubObject)
{
	if (Connection == nullptr || !Connection->IsConnected())
	{
		UE_LOG(LogSpatialOSNetDriver, Error, TEXT("Attempted to call ProcessRemoteFunction before connection was establised"))
		return;
	}

	USpatialNetConnection* NetConnection = ServerConnection ? Cast<USpatialNetConnection>(ServerConnection) : GetSpatialOSNetConnection();
	if (NetConnection == nullptr)
	{
		UE_LOG(LogSpatialOSNetDriver, Error, TEXT("Attempted to call ProcessRemoteFunction before connection was establised"))
		return;
	}

	// This check mimics the way Unreal natively checks whether an AActor has ownership for sending server RPCs.
	// The function GetNetConnection() goes up the AActor ownership chain until it reaches an AActor that is possesed by an AController and
	// hence a UNetConnection. Server RPCs should only be sent by AActor instances that either are possessed by a UNetConnection or are owned by
	// other AActor instances possessed by a UNetConnection. For native Unreal reference see ProcessRemoteFunction() of IpNetDriver.cpp.
	// However if we are on the server, and the RPC is a CrossServer or NetMulticast RPC, this can be invoked without an owner.
	if (!Actor->GetNetConnection() && !(Function->FunctionFlags & (FUNC_NetCrossServer | FUNC_NetMulticast) && IsServer()))
	{
		UE_LOG(LogSpatialOSNetDriver, Warning, TEXT("No owning connection for actor %s. Function %s will not be processed."), *Actor->GetName(), *Function->GetName());
		return;
	}

	// The RPC might have been called by an actor directly, or by a subobject on that actor
	UObject* CallingObject = SubObject ? SubObject : Actor;

	if (Function->FunctionFlags & FUNC_Net)
	{
		Sender->SendRPC(MakeShared<FPendingRPCParams>(CallingObject, Function, Parameters));
	}
}

void USpatialNetDriver::TickFlush(float DeltaTime)
{
	// Super::TickFlush() will not call ReplicateActors() because Spatial connections have InternalAck set to true.
	// In our case, our Spatial actor interop is triggered through ReplicateActors() so we want to call it regardless.

#if USE_SERVER_PERF_COUNTERS
	double ServerReplicateActorsTimeMs = 0.0f;
#endif // USE_SERVER_PERF_COUNTERS
	if (IsServer() && ClientConnections.Num() > 0 && Connection->IsConnected())
	{
		// Update all clients.
#if WITH_SERVER_CODE

#if USE_SERVER_PERF_COUNTERS
		double ServerReplicateActorsTimeStart = FPlatformTime::Seconds();
#endif // USE_SERVER_PERF_COUNTERS

		int32 Updated = ServerReplicateActors(DeltaTime);

#if USE_SERVER_PERF_COUNTERS
		ServerReplicateActorsTimeMs = (FPlatformTime::Seconds() - ServerReplicateActorsTimeStart) * 1000.0;
#endif // USE_SERVER_PERF_COUNTERS

		static int32 LastUpdateCount = 0;
		// Only log the zero replicated actors once after replicating an actor
		if ((LastUpdateCount && !Updated) || Updated)
		{
			UE_LOG(LogNetTraffic, Verbose, TEXT("%s replicated %d actors"), *GetDescription(), Updated);
		}
		LastUpdateCount = Updated;
#endif // WITH_SERVER_CODE
	}

	Super::TickFlush(DeltaTime);
}

USpatialNetConnection * USpatialNetDriver::GetSpatialOSNetConnection() const
{
	if (ServerConnection)
	{
		return Cast<USpatialNetConnection>(ServerConnection);
	}
	else
	{
		return Cast<USpatialNetConnection>(ClientConnections[0]);
	}
}

USpatialNetConnection* USpatialNetDriver::AcceptNewPlayer(const FURL& InUrl, bool bExistingPlayer)
{
	bool bOk = true;

	USpatialNetConnection* SpatialConnection = NewObject<USpatialNetConnection>(GetTransientPackage(), NetConnectionClass);
	check(SpatialConnection);

	// We create a "dummy" connection that corresponds to this player. This connection won't transmit any data.
	// We may not need to keep it in the future, but for now it looks like path of least resistance is to have one UPlayer (UConnection) per player.
	ISocketSubsystem* SocketSubsystem = GetSocketSubsystem();
	TSharedRef<FInternetAddr> FromAddr = SocketSubsystem->CreateInternetAddr();

	SpatialConnection->InitRemoteConnection(this, nullptr, InUrl, *FromAddr, USOCK_Open);
	Notify->NotifyAcceptedConnection(SpatialConnection);
	AddClientConnection(SpatialConnection);

	// Set up the net ID for this player.
	const TCHAR* WorkerAttributeOption = InUrl.GetOption(TEXT("workerAttribute"), nullptr);
	check(WorkerAttributeOption);
	FString WorkerAttribute = FString(WorkerAttributeOption).Mid(1); // Trim off the = at the beginning.
	FUniqueNetIdRepl WorkerAttributeId(TSharedPtr<FSpatialWorkerUniqueNetId>(new FSpatialWorkerUniqueNetId(WorkerAttribute)));

	// We will now ask GameMode/GameSession if it's ok for this user to join.
	// Note that in the initial implementation, we carry over no data about the user here (such as a unique player id, or the real IP)
	// In the future it would make sense to add metadata to the Spawn request and pass it here.
	// For example we can check whether a user is banned by checking against an OnlineSubsystem.

	// skip to the first option in the URL
	const FString UrlString = InUrl.ToString();
	const TCHAR* Tmp = *UrlString;
	for (; *Tmp && *Tmp != '?'; Tmp++);

	FString ErrorMsg;
	AGameModeBase* GameMode = GetWorld()->GetAuthGameMode();
	if (GameMode)
	{
		GameMode->PreLogin(Tmp, SpatialConnection->LowLevelGetRemoteAddress(), WorkerAttributeId, ErrorMsg);
	}

	if (!ErrorMsg.IsEmpty())
	{
		UE_LOG(LogSpatialOSNetDriver, Error, TEXT("PreLogin failure: %s"), *ErrorMsg);
		bOk = false;
	}

	if (bOk)
	{
		FString LevelName = GetWorld()->GetCurrentLevel()->GetOutermost()->GetName();
		SpatialConnection->SetClientWorldPackageName(GetWorld()->GetCurrentLevel()->GetOutermost()->GetFName());

		FString GameName;
		FString RedirectURL;
		if (GameMode)
		{
			GameName = GameMode->GetClass()->GetPathName();
			GameMode->GameWelcomePlayer(SpatialConnection, RedirectURL);
		}

		if (!bExistingPlayer)
		{
			SpatialConnection->PlayerController = World->SpawnPlayActor(SpatialConnection, ROLE_AutonomousProxy, InUrl, WorkerAttributeId, ErrorMsg);
		}
		else
		{
			// Most of this is taken from "World->SpawnPlayActor", excluding the logic to spawn a pawn which happens during
			// GameMode->PostLogin(...).
			APlayerController* NewPlayerController = GameMode->SpawnPlayerController(ROLE_AutonomousProxy, UrlString);

			// Destroy the player state (as we'll be replacing it anyway).
			NewPlayerController->CleanupPlayerState();

			// Possess the newly-spawned player.
			NewPlayerController->NetPlayerIndex = 0;
			NewPlayerController->Role = ROLE_Authority;
			NewPlayerController->SetReplicates(true);
			NewPlayerController->SetAutonomousProxy(true);
			NewPlayerController->SetPlayer(SpatialConnection);
			// We explicitly don't call GameMode->PostLogin(NewPlayerController) here, to avoid the engine restarting the player.
			// TODO: Should we call AGameSession::PostLogin? - UNR:583
			// TODO: Should we trigger to blueprints that a player has "joined" via GameMode->K2_PostLogin(Connection)? - UNR:583

			SpatialConnection->PlayerController = NewPlayerController;
		}

		if (SpatialConnection->PlayerController == NULL)
		{
			// Failed to connect.
			UE_LOG(LogSpatialOSNetDriver, Error, TEXT("Join failure: %s"), *ErrorMsg);
			SpatialConnection->FlushNet(true);
			bOk = false;
		}
	}

	if (!bOk)
	{
		// TODO: Destroy connection. UNR:584
	}

	return bOk ? SpatialConnection : nullptr;
}

bool USpatialNetDriver::Exec(UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
#if !UE_BUILD_SHIPPING
	if (FParse::Command(&Cmd, TEXT("DUMPCROSSSERVERRPC")))
	{
		return HandleNetDumpCrossServerRPCCommand(Cmd, Ar);
	}
#endif // !UE_BUILD_SHIPPING
	return UNetDriver::Exec(InWorld, Cmd, Ar);
}

// This function is literally a copy paste of UNetDriver::HandleNetDumpServerRPCCommand. Didn't want to refactor to avoid divergence from engine.
#if !UE_BUILD_SHIPPING
bool USpatialNetDriver::HandleNetDumpCrossServerRPCCommand(const TCHAR* Cmd, FOutputDevice& Ar)
{
#if WITH_SERVER_CODE
	for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
	{
		bool bHasNetFields = false;

		ensureMsgf(!ClassIt->HasAnyFlags(RF_NeedLoad | RF_NeedPostLoad), TEXT("UNetDriver::HandleNetDumpCrossServerRPCCommand: %s has flag RF_NeedPostLoad. NetFields and ClassReps will be incorrect!"), *GetFullNameSafe(*ClassIt));

		for (int32 i = 0; i < ClassIt->NetFields.Num(); i++)
		{
			UFunction * Function = Cast<UFunction>(ClassIt->NetFields[i]);

			if (Function != NULL && Function->FunctionFlags & FUNC_NetCrossServer)
			{
				bHasNetFields = true;
				break;
			}
		}

		if (!bHasNetFields)
		{
			continue;
		}

		Ar.Logf(TEXT("Class: %s"), *ClassIt->GetName());

		for (int32 i = 0; i < ClassIt->NetFields.Num(); i++)
		{
			UFunction * Function = Cast<UFunction>(ClassIt->NetFields[i]);

			if (Function != NULL && Function->FunctionFlags & FUNC_NetCrossServer)
			{
				const FClassNetCache * ClassCache = NetCache->GetClassNetCache(*ClassIt);

				const FFieldNetCache * FieldCache = ClassCache->GetFromField(Function);

				TArray< UProperty * > Parms;

				for (TFieldIterator<UProperty> It(Function); It && (It->PropertyFlags & (CPF_Parm | CPF_ReturnParm)) == CPF_Parm; ++It)
				{
					Parms.Add(*It);
				}

				if (Parms.Num() == 0)
				{
					Ar.Logf(TEXT("    [0x%03x] %s();"), FieldCache->FieldNetIndex, *Function->GetName());
					continue;
				}

				FString ParmString;

				for (int32 j = 0; j < Parms.Num(); j++)
				{
					if (Cast<UStructProperty>(Parms[j]))
					{
						ParmString += Cast<UStructProperty>(Parms[j])->Struct->GetName();
					}
					else
					{
						ParmString += Parms[j]->GetClass()->GetName();
					}

					ParmString += TEXT(" ");

					ParmString += Parms[j]->GetName();

					if (j < Parms.Num() - 1)
					{
						ParmString += TEXT(", ");
					}
				}

				Ar.Logf(TEXT("    [0x%03x] %s( %s );"), FieldCache->FieldNetIndex, *Function->GetName(), *ParmString);
			}
		}
	}
#endif
	return true;
}
#endif // !UE_BUILD_SHIPPING

USpatialPendingNetGame::USpatialPendingNetGame(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

void USpatialPendingNetGame::InitNetDriver()
{
	check(GIsClient);

	// This is a trimmed down version of UPendingNetGame::InitNetDriver(). We don't send any Unreal connection packets, just set up the net driver.
	if (!GDisallowNetworkTravel)
	{
		// Try to create network driver.
		if (GEngine->CreateNamedNetDriver(this, NAME_PendingNetDriver, NAME_GameNetDriver))
		{
			NetDriver = GEngine->FindNamedNetDriver(this, NAME_PendingNetDriver);
		}
		check(NetDriver);

		if (!NetDriver->InitConnect(this, URL, ConnectionError))
		{
			// error initializing the network stack...
			UE_LOG(LogNet, Warning, TEXT("error initializing the network stack"));
			GEngine->DestroyNamedNetDriver(this, NetDriver->NetDriverName);
			NetDriver = nullptr;

			// ConnectionError should be set by calling InitConnect...however, if we set NetDriver to NULL without setting a
			// value for ConnectionError, we'll trigger the assertion at the top of UPendingNetGame::Tick() so make sure it's set
			if (ConnectionError.Len() == 0)
			{
				ConnectionError = NSLOCTEXT("Engine", "NetworkInit", "Error initializing network layer.").ToString();
			}
		}
	}
	else
	{
		ConnectionError = NSLOCTEXT("Engine", "UsedCheatCommands", "Console commands were used which are disallowed in netplay.  You must restart the game to create a match.").ToString();
	}
}

void USpatialPendingNetGame::SendJoin()
{
	bSentJoinRequest = true;
}

void USpatialNetDriver::AddActorChannel(Worker_EntityId EntityId, USpatialActorChannel* Channel)
{
	EntityToActorChannel.Add(EntityId, Channel);
}

void USpatialNetDriver::RemoveActorChannel(Worker_EntityId EntityId)
{
	if (!EntityToActorChannel.Contains(EntityId))
	{
		UE_LOG(LogSpatialOSNetDriver, Warning, TEXT("RemoveActorChannel: Failed to find entity/channel mapping for entity %lld."), EntityId);
		return;
	}

	EntityToActorChannel.FindAndRemoveChecked(EntityId);
}

USpatialActorChannel* USpatialNetDriver::GetActorChannelByEntityId(Worker_EntityId EntityId) const
{
	return EntityToActorChannel.FindRef(EntityId);
}

void USpatialNetDriver::WipeWorld(const USpatialNetDriver::PostWorldWipeDelegate& LoadSnapshotAfterWorldWipe)
{
	if (Cast<USpatialGameInstance>(GetWorld()->GetGameInstance())->bResponsibleForSnapshotLoading)
	{
		SnapshotManager->WorldWipe(LoadSnapshotAfterWorldWipe);
	}
}
