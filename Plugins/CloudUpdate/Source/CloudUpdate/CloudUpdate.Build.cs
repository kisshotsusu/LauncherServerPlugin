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
			"DeveloperSettings",
			"UMG"
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Slate",
			"SlateCore",
			"EngineSettings",
			"HTTP",
			"Json",
			"JsonUtilities",
			"PakFile",
			"Projects",
			// 局域网回退需要枚举本机网卡地址 / 解析服务器主机名
			"Sockets",
			"HotPatcherRuntime",
			// HotPatcher 的二进制补丁特性接口（IBinariesDiffPatchFeature），由 HDiffPatchUE 在运行时注册实现
			"BinariesPatchFeature"
		});
	}
}
