#pragma once

#include "Utils/SchemaOption.h"

#include <cstdint>

using Worker_EntityId = std::int64_t;

struct FUnrealObjectRef
{
	FUnrealObjectRef() = default;

	FUnrealObjectRef(Worker_EntityId Entity, uint32 Offset)
		: Entity(Entity)
		, Offset(Offset)
	{}

	FUnrealObjectRef(Worker_EntityId Entity, uint32 Offset, FString Path, FUnrealObjectRef Outer)
		: Entity(Entity)
		, Offset(Offset)
		, Path(Path)
		, Outer(Outer)
	{}

	FUnrealObjectRef(const FUnrealObjectRef& In)
		: Entity(In.Entity)
		, Offset(In.Offset)
		, Path(In.Path)
		, Outer(In.Outer)
	{}

	FORCEINLINE FUnrealObjectRef& operator=(const FUnrealObjectRef& In)
	{
		Entity = In.Entity;
		Offset = In.Offset;
		Path = In.Path;
		Outer = In.Outer;
		return *this;
	}

	FORCEINLINE FString ToString() const
	{
		return FString::Printf(TEXT("(entity ID: %lld, offset: %u)"), Entity, Offset);
	}

	FORCEINLINE bool operator==(const FUnrealObjectRef& Other) const
	{
		return Entity == Other.Entity &&
			Offset == Other.Offset &&
			((!Path && !Other.Path) || (Path && Other.Path && Path->Equals(*Other.Path))) &&
			((!Outer && !Other.Outer) || (Outer && Other.Outer && *Outer == *Other.Outer));
	}

	FORCEINLINE bool operator!=(const FUnrealObjectRef& Other) const
	{
		return !operator==(Other);
	}

	Worker_EntityId Entity;
	uint32 Offset;
	improbable::TSchemaOption<FString> Path;
	improbable::TSchemaOption<FUnrealObjectRef> Outer;
};

inline uint32 GetTypeHash(const FUnrealObjectRef& ObjectRef)
{
	uint32 Result = 1327u;
	Result = (Result * 977u) + GetTypeHash(static_cast<int64>(ObjectRef.Entity));
	Result = (Result * 977u) + GetTypeHash(ObjectRef.Offset);
	Result = (Result * 977u) + GetTypeHash(ObjectRef.Path);
	Result = (Result * 977u) + GetTypeHash(ObjectRef.Outer);
	return Result;
}
