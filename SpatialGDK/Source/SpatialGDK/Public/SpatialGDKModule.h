// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

#pragma once

#include "CoreMinimal.h"
#include "Developer/Settings/Public/ISettingsContainer.h"
#include "Developer/Settings/Public/ISettingsModule.h"
#include "Developer/Settings/Public/ISettingsSection.h"
#include "Modules/ModuleManager.h"

#include "SpatialGDKLoader.h"

DECLARE_LOG_CATEGORY_EXTERN(LogSpatialGDKModule, Log, All);

class SPATIALGDK_API FSpatialGDKModule : public IModuleInterface
{
public:
	void StartupModule() override;
	void ShutdownModule() override;

private:
	FSpatialGDKLoader Loader;
};
