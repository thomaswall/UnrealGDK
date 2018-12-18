// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#include "Interop/SpatialTypebindingManager.h"

#include "AssetRegistryModule.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/SCS_Node.h"
#include "GameFramework/Actor.h"
#include "Misc/MessageDialog.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include "EngineClasses/SpatialNetDriver.h"
#include "EngineClasses/SpatialPackageMapClient.h"
#include "Utils/RepLayoutUtils.h"

void USpatialTypebindingManager::Init(USpatialNetDriver* InNetDriver)
{
	NetDriver = InNetDriver;
	
	TSoftObjectPtr<USchemaDatabase> SchemaDatabasePtr(FSoftObjectPath(TEXT("/Game/Spatial/SchemaDatabase.SchemaDatabase")));
	SchemaDatabasePtr.LoadSynchronous();
	SchemaDatabase = SchemaDatabasePtr.Get();

	if (SchemaDatabase == nullptr)
	{
		FMessageDialog::Debugf(FText::FromString(TEXT("SchemaDatabase not found! No classes will be supported for SpatialOS replication.")));
		return;
	}

	FindSupportedClasses();
	CreateTypebindings();
}

void USpatialTypebindingManager::FindSupportedClasses()
{
	TArray<FString> SupportedClassPaths;
	SchemaDatabase->ClassPathToSchema.GetKeys(SupportedClassPaths);

	for (auto& ClassPath : SupportedClassPaths)
	{
		FSoftClassPath SoftClassPath(ClassPath);
		UClass* Class = SoftClassPath.TryLoadClass<UObject>();

		if (Class != nullptr)
		{
			SupportedClasses.Add(Class);
		}
	}
}

void USpatialTypebindingManager::CreateTypebindings()
{
	// This will iterate through all supported classes: both Actor and Subobject classes
	// For subobject classes, an FClassInfo will be created with RPC metadata and handover data
	// but schema components will be 0 since subobject classes don't have associated components
	// afterwards, the subobject FClassInfos will be the basis of the FClassInfo for specific subobjects
	// and have their proper schema components filled in
	for (UClass* Class : SupportedClasses)
	{
		FClassInfo Info;

		TArray<UFunction*> RelevantClassFunctions = improbable::GetClassRPCFunctions(Class);

		for (UFunction* RemoteFunction : RelevantClassFunctions)
		{
			ESchemaComponentType RPCType = SCHEMA_Invalid;
			if (RemoteFunction->FunctionFlags & FUNC_NetClient)
			{
				RPCType = SCHEMA_ClientRPC;
			}
			else if (RemoteFunction->FunctionFlags & FUNC_NetServer)
			{
				RPCType = SCHEMA_ServerRPC;
			}
			else if (RemoteFunction->FunctionFlags & FUNC_NetCrossServer)
			{
				RPCType = SCHEMA_CrossServerRPC;
			}
			else if (RemoteFunction->FunctionFlags & FUNC_NetMulticast)
			{
				RPCType = SCHEMA_NetMulticastRPC;
			}
			else
			{
				checkNoEntry();
			}

			TArray<UFunction*>& RPCArray = Info.RPCs.FindOrAdd(RPCType);

			FRPCInfo RPCInfo;
			RPCInfo.Type = RPCType;
			RPCInfo.Index = RPCArray.Num();

			RPCArray.Add(RemoteFunction);
			Info.RPCInfoMap.Add(RemoteFunction, RPCInfo);
		}

		for (TFieldIterator<UProperty> PropertyIt(Class); PropertyIt; ++PropertyIt)
		{
			UProperty* Property = *PropertyIt;

			if (Property->PropertyFlags & CPF_Handover)
			{
				for (int32 ArrayIdx = 0; ArrayIdx < PropertyIt->ArrayDim; ++ArrayIdx)
				{
					FHandoverPropertyInfo HandoverInfo;
					HandoverInfo.Handle = Info.HandoverProperties.Num() + 1; // 1-based index
					HandoverInfo.Offset = Property->GetOffset_ForGC() + Property->ElementSize * ArrayIdx;
					HandoverInfo.ArrayIdx = ArrayIdx;
					HandoverInfo.Property = Property;

					Info.HandoverProperties.Add(HandoverInfo);
				}
			}
		}

		ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
		{
			Worker_ComponentId ComponentId = SchemaDatabase->ClassPathToSchema[Class->GetPathName()].SchemaComponents[Type];
			if (ComponentId != 0)
			{
				Info.SchemaComponents[Type] = ComponentId;
				ComponentToClassMap.Add(ComponentId, Class);
				ComponentToOffsetMap.Add(ComponentId, 0);
				ComponentToCategoryMap.Add(ComponentId, (ESchemaComponentType)Type);
			}
		});

		Info.Class = Class;

		ClassInfoMap.Emplace(Class, Info);
	}

	for (UClass* Class : SupportedClasses)
	{
		for (auto& SubobjectDataPair : SchemaDatabase->ClassPathToSchema[Class->GetPathName()].SubobjectData)
		{
			int32 Offset = SubobjectDataPair.Key;
			FSubobjectSchemaData SubobjectSchemaData = SubobjectDataPair.Value;

			FClassInfo* ActorInfo = FindClassInfoByClass(Class);

			FSoftClassPath SubobjectClassPath(SubobjectSchemaData.ClassPath);
			UClass* SubobjectClass = SubobjectClassPath.TryLoadClass<UObject>();
			if (SubobjectClass == nullptr)
			{
				continue;
			}

			FClassInfo* SubobjectInfoPtr = FindClassInfoByClass(SubobjectClass);
			if (SubobjectInfoPtr == nullptr)
			{
				continue;
			}

			// Make a copy of the already made FClassInfo for this specific subobject
			FClassInfo SubobjectInfo = *SubobjectInfoPtr;

			SubobjectInfo.SubobjectName = SubobjectSchemaData.Name;

			ForAllSchemaComponentTypes([&](ESchemaComponentType Type)
			{
				Worker_ComponentId ComponentId = SubobjectSchemaData.SchemaComponents[Type];
				if (ComponentId != 0)
				{
					SubobjectInfo.SchemaComponents[Type] = ComponentId;
					ComponentToClassMap.Add(ComponentId, SubobjectClass);
					ComponentToOffsetMap.Add(ComponentId, Offset);
					ComponentToCategoryMap.Add(ComponentId, (ESchemaComponentType)Type);
				}
			});

			ActorInfo->SubobjectInfo.Add(Offset, MakeShared<FClassInfo>(SubobjectInfo));
		}
	}
}

FClassInfo* USpatialTypebindingManager::FindClassInfoByClass(UClass* Class)
{
	return ClassInfoMap.Find(Class);
}

FClassInfo* USpatialTypebindingManager::FindClassInfoByActorClassAndOffset(UClass* Class, uint32 Offset)
{
	if (FClassInfo* Info = FindClassInfoByClass(Class))
	{
		if (Offset == 0)
		{
			return Info;
		}

		if (TSharedPtr<FClassInfo>* SubobjectInfo = Info->SubobjectInfo.Find(Offset))
		{
			return SubobjectInfo->Get();
		}

		return nullptr;
	}

	return nullptr;
}

FClassInfo* USpatialTypebindingManager::FindClassInfoByComponentId(Worker_ComponentId ComponentId)
{
	UClass* Class = FindClassByComponentId(ComponentId);
	return Class != nullptr ? FindClassInfoByClass(Class) : nullptr;
}

FClassInfo* USpatialTypebindingManager::FindClassInfoByObject(UObject* Object)
{
	if (AActor* Actor = Cast<AActor>(Object))
	{
		return FindClassInfoByClass(Actor->GetClass());
	}
	else
	{
		checkSlow(Cast<AActor>(Object->GetOuter()));

		FUnrealObjectRef ObjectRef = NetDriver->PackageMap->GetUnrealObjectRefFromObject(Object);

		if (ObjectRef != SpatialConstants::NULL_OBJECT_REF && ObjectRef != SpatialConstants::UNRESOLVED_OBJECT_REF)
		{
			return FindClassInfoByActorClassAndOffset(Object->GetOuter()->GetClass(), ObjectRef.Offset);
		}
	}

	return nullptr;
}

UClass* USpatialTypebindingManager::FindClassByComponentId(Worker_ComponentId ComponentId)
{
	UClass** Class = ComponentToClassMap.Find(ComponentId);
	return Class != nullptr ? *Class : nullptr;
}

bool USpatialTypebindingManager::IsSupportedClass(UClass* Class)
{
	return SupportedClasses.Contains(Class);
}

bool USpatialTypebindingManager::FindOffsetByComponentId(Worker_ComponentId ComponentId, uint32& OutOffset)
{
	if (uint32* Offset = ComponentToOffsetMap.Find(ComponentId))
	{
		OutOffset = *Offset;
		return true;
	}

	return false;
}

ESchemaComponentType USpatialTypebindingManager::FindCategoryByComponentId(Worker_ComponentId ComponentId)
{
	if (ESchemaComponentType* Category = ComponentToCategoryMap.Find(ComponentId))
	{
		return *Category;
	}

	return ESchemaComponentType::SCHEMA_Invalid;
}
