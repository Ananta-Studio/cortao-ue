// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// CORTAO is by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO

#pragma once

#include "CoreMinimal.h"
#include "Logging/LogMacros.h"

class FScene;
class FViewInfo;
class FRDGBuilder;
class FGlobalIlluminationPluginResources;

DECLARE_LOG_CATEGORY_EXTERN(LogCORTAO, Log, All);

namespace CORTAO
{
	/** The global illumination plugin delegate's handler: CORTAO into the view's ScreenSpaceAO, when r.CORTAO is on. */
	void RenderForView(const FScene& Scene, const FViewInfo& View, FRDGBuilder& GraphBuilder, FGlobalIlluminationPluginResources& Resources);

	/** Called once the engine has finished starting: from then on a wrong Global Illumination method is reported in the log. */
	void OnEngineStarted();
}
