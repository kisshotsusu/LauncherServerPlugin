// ======================================================================
// CloudUpdate 二进制补丁合并实现
// ======================================================================
#include "CloudUpdateBinaryMerge.h"
#include "CloudUpdate.h"

#include "BinariesPatchFeature.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

bool FCloudBinaryMerge::IsHDiffPatchAvailable()
{
	return GetFeature() != nullptr;
}

IBinariesDiffPatchFeature* FCloudBinaryMerge::GetFeature(const FString& InFeatureName)
{
	TArray<IBinariesDiffPatchFeature*> Features =
		IModularFeatures::Get().GetModularFeatureImplementations<IBinariesDiffPatchFeature>(BINARIES_DIFF_PATCH_FEATURE_NAME);
	if (Features.Num() == 0)
	{
		return nullptr;
	}
	if (InFeatureName.IsEmpty())
	{
		return Features[0];
	}
	for (IBinariesDiffPatchFeature* Feature : Features)
	{
		if (Feature && Feature->GetFeatureName().Equals(InFeatureName, ESearchCase::IgnoreCase))
		{
			return Feature;
		}
	}
	// 指定名称未找到时，回退到第一个可用特性，避免完全不可用
	return Features[0];
}

FString FCloudBinaryMerge::GetFeatureName(const FString& InFeatureName)
{
	IBinariesDiffPatchFeature* Feature = GetFeature(InFeatureName);
	return Feature ? Feature->GetFeatureName() : TEXT("");
}

bool FCloudBinaryMerge::ApplyPatchToFile(const FString& InBaseFilePath,
                                         const FString& InPatchFilePath,
                                         FString& OutMergedFilePath,
                                         const FString& InFeatureName)
{
	const EBinaryMergeResult Result = ApplyPatchToFileEx(InBaseFilePath, InPatchFilePath, OutMergedFilePath, InFeatureName);
	// 已替换 或 已暂存待重启 都视为「合并已发生」
	return Result == EBinaryMergeResult::Success || Result == EBinaryMergeResult::StagedForRestart;
}

EBinaryMergeResult FCloudBinaryMerge::ApplyPatchToFileEx(const FString& InBaseFilePath,
                                                        const FString& InPatchFilePath,
                                                        FString& OutMergedFilePath,
                                                        const FString& InFeatureName)
{
	OutMergedFilePath = InBaseFilePath;

	IBinariesDiffPatchFeature* Feature = GetFeature(InFeatureName);
	if (!Feature)
	{
		UE_LOG(LogCloudUpdate, Warning,
			TEXT("二进制补丁合并跳过：运行时未注册任何 IBinariesDiffPatchFeature（HDiffPatch 不可用）"));
		return EBinaryMergeResult::Failed;
	}

	// 流式应用：Feature->PatchDiffToFile 内部按需分块读写文件，避免把整个基础/补丁文件载入内存。
	// HDiffPatchUE 提供真正的流式实现（峰值内存约 1 MiB）；其它未重写的特性走默认缓冲实现。
	const FString TmpPath = InBaseFilePath + TEXT(".merged.tmp");
	if (!Feature->PatchDiffToFile(InBaseFilePath, InPatchFilePath, TmpPath))
	{
		IFileManager::Get().Delete(*TmpPath, false, true);
		UE_LOG(LogCloudUpdate, Warning, TEXT("二进制补丁合并失败：PatchDiffToFile 返回 false（%s）"), *InBaseFilePath);
		return EBinaryMergeResult::Failed;
	}

	// 原子替换：先写临时文件，再 Move 覆盖基础文件，避免写一半损坏基础文件
	if (IFileManager::Get().Move(*InBaseFilePath, *TmpPath, /*ReplaceExisting=*/true, /*EvenIfReadOnly=*/false, /*Attributes=*/false))
	{
		return EBinaryMergeResult::Success;
	}

	// 替换失败：通常是基础文件被占用（运行中 pak 已挂载，Windows 下文件锁）。
	// 暂存为 <基础文件>.pending，待下次启动、挂载前由 FinalizePendingMerges 交换。
	const FString PendingPath = InBaseFilePath + TEXT(".pending");
	if (IFileManager::Get().Move(*PendingPath, *TmpPath, /*ReplaceExisting=*/true, /*EvenIfReadOnly=*/true, /*Attributes=*/false))
	{
		UE_LOG(LogCloudUpdate, Warning,
			TEXT("二进制补丁合并：基础文件被占用，已暂存为 %s，重启后生效"), *PendingPath);
		return EBinaryMergeResult::StagedForRestart;
	}

	// 连暂存也失败（极端情况）：清理临时文件，标记失败
	IFileManager::Get().Delete(*TmpPath, false, true);
	UE_LOG(LogCloudUpdate, Error, TEXT("二进制补丁合并失败：无法替换或暂存基础文件 %s"), *InBaseFilePath);
	return EBinaryMergeResult::Failed;
}

int32 FCloudBinaryMerge::FinalizePendingMerges(const FString& InDirectory, bool bIncludeSubdirectories)
{
	TArray<FString> Pending;
	IFileManager::Get().FindFilesRecursive(Pending, *InDirectory, TEXT("*.pending"), /*Files=*/true, /*Dirs=*/false, bIncludeSubdirectories);

	int32 Swapped = 0;
	for (const FString& PendingPath : Pending)
	{
		// PendingPath == "<基础文件>.pending" -> 基础文件为去掉末尾 ".pending"
		FString BasePath = PendingPath;
		if (!BasePath.RemoveFromEnd(TEXT(".pending")))
		{
			continue;
		}
		// 基础文件一般不存在（已被 .pending 取代前已被删除/移动），直接重命名即可；
		// 若存在（极端情况），ReplaceExisting 覆盖它。
		if (IFileManager::Get().Move(*BasePath, *PendingPath, /*ReplaceExisting=*/true, /*EvenIfReadOnly=*/true, /*Attributes=*/false))
		{
			++Swapped;
			UE_LOG(LogCloudUpdate, Log, TEXT("已交换暂存补丁：%s -> %s"), *PendingPath, *BasePath);
		}
		else
		{
			UE_LOG(LogCloudUpdate, Error, TEXT("暂存补丁交换失败（文件可能仍被占用）：%s"), *PendingPath);
		}
	}
	if (Swapped > 0)
	{
		UE_LOG(LogCloudUpdate, Log, TEXT("FinalizePendingMerges：成功交换 %d 个暂存补丁"), Swapped);
	}
	return Swapped;
}

TArray<FString> FCloudBinaryMerge::FindPatchFiles(const FString& InDirectory, bool bIncludeSubdirectories)
{
	TArray<FString> Result;
	IFileManager::Get().FindFilesRecursive(Result, *InDirectory, TEXT("*.patch"), /*Files=*/true, /*Dirs=*/false, bIncludeSubdirectories);
	return Result;
}

FBinaryMergeResult FCloudBinaryMerge::AutoMergeDirectory(
	const FString& InDirectory,
	bool bIncludeSubdirectories,
	const FString& InFeatureName,
	TFunction<void(int32, int32, const FString&, bool)> ProgressCb)
{
	FBinaryMergeResult Result;
	const TArray<FString> Patches = FindPatchFiles(InDirectory, bIncludeSubdirectories);
	const int32 Total = Patches.Num();
	int32 Completed = 0;

	for (const FString& PatchPath : Patches)
	{
		// 补丁与基础文件同目录：X.patch -> X
		const FString BasePath = GetBaseFileName(PatchPath);
		bool bOk = false;
		if (FPaths::FileExists(BasePath))
		{
			FString Merged;
			const EBinaryMergeResult R = ApplyPatchToFileEx(BasePath, PatchPath, Merged, InFeatureName);
			// Success（已替换）与 StagedForRestart（已暂存待重启）都视为合并已发生
			bOk = (R == EBinaryMergeResult::Success || R == EBinaryMergeResult::StagedForRestart);
			if (R == EBinaryMergeResult::Failed)
			{
				Result.Failed += 1;
				Result.FailedFiles.Add(PatchPath);
			}
			else
			{
				Result.Succeeded += 1;
			}
		}
		else
		{
			UE_LOG(LogCloudUpdate, Warning, TEXT("自动合并跳过：基础文件缺失 %s（对应补丁 %s）"), *BasePath, *PatchPath);
			Result.Failed += 1;
			Result.FailedFiles.Add(PatchPath);
		}
		++Completed;
		if (ProgressCb)
		{
			ProgressCb(Completed, Total, PatchPath, bOk);
		}
	}
	return Result;
}

bool FCloudBinaryMerge::IsPatchFile(const FString& InFileName)
{
	return InFileName.EndsWith(TEXT(".patch"), ESearchCase::IgnoreCase);
}

FString FCloudBinaryMerge::GetBaseFileName(const FString& InPatchFileName)
{
	FString Result = InPatchFileName;
	if (Result.EndsWith(TEXT(".patch"), ESearchCase::IgnoreCase))
	{
		Result.LeftChopInline(6); // strlen(".patch") == 6
	}
	return Result;
}
