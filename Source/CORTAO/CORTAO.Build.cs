// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// CORTAO is by Daniel Isheden (MIT): https://github.com/theagentd/CORTAO

using System.IO;
using UnrealBuildTool;

public class CORTAO : ModuleRules
{
	public CORTAO(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// FViewInfo, FSceneTextures and the global illumination plugin delegate live in the renderer's
		// private and internal headers. They change between engine versions: a compile error after an
		// engine update starts here.
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Private"));
		PrivateIncludePaths.Add(Path.Combine(EngineDirectory, "Source/Runtime/Renderer/Internal"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Projects",
			"RenderCore",
			"Renderer",
			"RHI",
		});
	}
}
