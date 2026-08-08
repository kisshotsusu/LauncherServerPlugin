using UnrealBuildTool;

public class CloudUpdate : ModuleRules
{
	public CloudUpdate(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",
			"Json",
			"JsonUtilities",
			"PakFile",
			"Projects",
			"HotPatcherRuntime"
		});
	}
}