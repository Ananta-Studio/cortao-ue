// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// CORTAO is by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO

#include "CORTAOModule.h"

#include "CORTAORendering.h"
#include "DeferredShadingRenderer.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

void FCORTAOModule::StartupModule()
{
	// The module loads at PostConfigInit, so that the mapping exists before the global shaders compile.
	const FString ShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("CORTAO"))->GetBaseDir(), TEXT("Shaders"));
	AddShaderSourceDirectoryMapping(TEXT("/Plugin/CORTAO"), ShaderDir);

	// The hook fires once per view inside RenderDiffuseIndirectAndAmbientOcclusion, after the engine's
	// SSAO and BEFORE ScreenSpaceAO is read for the indirect composite, the sky light and reflections.
	// It only fires for a view whose Global Illumination method is Plugin
	// (r.DynamicGlobalIlluminationMethod 3, or a post-process volume that says so). Other global
	// illumination plugins use the same delegate and run beside this one.
	RenderHandle = FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().AddStatic(&CORTAO::RenderForView);
	EngineStartedHandle = FCoreDelegates::OnFEngineLoopInitComplete.AddStatic(&CORTAO::OnEngineStarted);
}

void FCORTAOModule::ShutdownModule()
{
	FGlobalIlluminationPluginDelegates::RenderDiffuseIndirectLight().Remove(RenderHandle);
	FCoreDelegates::OnFEngineLoopInitComplete.Remove(EngineStartedHandle);
}

IMPLEMENT_MODULE(FCORTAOModule, CORTAO)
