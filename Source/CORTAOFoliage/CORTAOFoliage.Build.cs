// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// Marks foliage cards for CORTAO: a lighting channel the renderer side reads.

using UnrealBuildTool;

public class CORTAOFoliage : ModuleRules
{
	public CORTAOFoliage(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
		});
	}
}
