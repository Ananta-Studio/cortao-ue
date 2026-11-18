// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// CORTAO is by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"

/**
 * Maps the plugin's shaders and hooks FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight.
 * Everything a project controls is a console variable (r.CORTAO.*); see CORTAORendering.cpp and the README.
 */
class FCORTAOModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	FDelegateHandle RenderHandle;
	FDelegateHandle EngineStartedHandle;
};
