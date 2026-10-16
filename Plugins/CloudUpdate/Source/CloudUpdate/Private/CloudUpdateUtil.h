#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FCloudDownloadFile;

// CloudUpdate 内部工具函数（由 CloudUpdateService.cpp 的匿名命名空间 helper 提取）
namespace CloudUpdatePrivate
{
 void SelectStartupGameInstance();
 FString ResolveUpdatePlatform(const FString& RuntimePlatform, const FString& AndroidCookFlavor);
 FString GetRuntimeUpdatePlatform();
 FString NormalizeCookPlatform(const FString& SourcePlatform);
 FString GetUpdatePakDirectory();
 bool MatchesResponsePlatform(const TSharedPtr<FJsonObject>& Json, const FString& Platform);
	FString BytesToHex(const uint8* InBytes, int32 InCount);
	bool ComputeFileHash(const FString& InPath, int64& OutSize, FString& OutHash);
	FString NormalizeSlashes(const FString& InPath);
	bool IsSafeRelativePath(const FString& InPath);
 bool IsGameBinaryPath(const FString& InPath);
	TArray<FString> SplitVersionParts(const FString& InVersion);
	bool IsVersionNewer(const FString& InA, const FString& InB);
 bool BuildHotPatcherFiles(const TSharedPtr<FJsonObject>& Json, const FString& Platform, const FString& BaseUrl,
   const FString& Version, bool bIoStore, TArray<FCloudDownloadFile>& OutFiles, FString& OutError);

 // ---------- 局域网回退 ----------

 /** http(s) URL 的分解结果。Path 含前导 '/' 与完整查询串，可为空。 */
 struct FCloudUrlParts
 {
  FString Scheme;
  FString Host;
  int32 Port = 0;   // 0 表示 URL 未写端口
  FString Path;
  bool bValid = false;
 };

 /** 分解 http(s) URL；非 http(s) 或缺主机时 bValid=false。 */
 FCloudUrlParts ParseCloudUrl(const FString& InUrl);
 /** 由分解结果拼回 URL；80/443 与 0 端口不写出端口。 */
 FString ComposeCloudUrl(const FCloudUrlParts& InParts);
 /** 严格判断 IPv4 字面量（4 段、每段 0-255）；IPv6 返回 false。 */
 bool IsIPv4Literal(const FString& InText);

 /**
  * 纯函数决策（便于单测）：给定"待处理 URL / 局域网基址 / 服务器解析出的 IP / 本机 IP"，
  * 判断是否应回退，并在需要时给出回退后的 URL。
  * 只替换主机与端口，保留原 Path（含查询串）。
  * @return true 表示应使用 OutUrl。
  */
 bool DecideLanFallback(const FString& InOriginalUrl, const FString& InLanBaseUrl,
  const TArray<FString>& InResolvedIps, const TArray<FString>& InLocalIps,
  FString& OutUrl, FString& OutReason);

 /** 本机所有非 0.0.0.0 的 IPv4 地址（去重）。拿不到 socket 子系统时返回空。 */
 void GetLocalIPv4Addresses(TArray<FString>& Out);
 /** 把主机名解析为 IPv4 列表（字面量直接返回）。 */
 bool ResolveHostIPv4(const FString& InHost, TArray<FString>& Out);

 // ---------- 容器集（.pak / .utoc / .ucas，同 basename 为一组） ----------

 /**
  * 是否属于容器集文件。
  * 引擎把 .pak 当作 IoStore 的挂载入口：FPakPlatformFile::Mount 会用
  * ChangeExtension(.pak -> .utoc) 去找容器，找不到时（GPak_AllowMissingUToc=false）
  * 会把整个 .pak 丢弃不挂载。因此这三类文件必须成组一致地出现在同一目录。
  */
 bool IsContainerSetFile(const FString& InPath);

 /** 同组 .pak 的路径（.utoc/.ucas/.pak 都映射到同 basename 的 .pak）。 */
 FString GetContainerSetPakPath(const FString& InPath);

 // ---------- 启动安全模式（资源实例连续启动失败 → 回 bootstrap 引导包） ----------

 /** 读取"资源实例启动尝试"记录，返回与 InVersion 相同版本的连续尝试次数（0 = 无记录/换版本）。 */
 int32 ReadStartupAttempt(const FString& InVersion);
 /** 写入/覆盖"资源实例启动尝试"记录。 */
 void WriteStartupAttempt(const FString& InVersion, int32 InAttempt);
 /** 清除尝试记录（撑过稳定窗口后调用）。 */
 void ClearStartupAttempt();
 /** 本次进程是否因安全模式回退到 bootstrap 引导包。 */
 bool WasSafeModeTriggered();
}
