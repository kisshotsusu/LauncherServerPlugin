#pragma once

#include "CoreMinimal.h"

class FJsonObject;
struct FCloudDownloadFile;

// CloudUpdate 内部工具函数（由 CloudUpdateService.cpp 的匿名命名空间 helper 提取）
namespace CloudUpdatePrivate
{
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
}
