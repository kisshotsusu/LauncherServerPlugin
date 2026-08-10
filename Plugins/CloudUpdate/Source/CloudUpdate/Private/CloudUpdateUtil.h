#pragma once

#include "CoreMinimal.h"

// CloudUpdate 内部工具函数（由 CloudUpdateService.cpp 的匿名命名空间 helper 提取）
namespace CloudUpdatePrivate
{
	FString BytesToHex(const uint8* InBytes, int32 InCount);
	bool ComputeFileHash(const FString& InPath, int64& OutSize, FString& OutHash);
	FString NormalizeSlashes(const FString& InPath);
	TArray<FString> SplitVersionParts(const FString& InVersion);
	bool IsVersionNewer(const FString& InA, const FString& InB);
}
