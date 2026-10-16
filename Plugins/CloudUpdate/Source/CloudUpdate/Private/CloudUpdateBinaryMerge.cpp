// ======================================================================
// CloudUpdate 二进制补丁合并实现
// ======================================================================
#include "CloudUpdateBinaryMerge.h"
#include "CloudUpdate.h"

#include "BinariesPatchFeature.h"
#include "Features/IModularFeatures.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/CriticalSection.h"
#include "Misc/Paths.h"

// 合并锁静态成员定义（见头文件 GetMergeLock 备注）
FCriticalSection FCloudBinaryMerge::MergeMapLock;
TMap<FString, TSharedPtr<FCriticalSection>> FCloudBinaryMerge::MergeLocks;

FCriticalSection& FCloudBinaryMerge::GetMergeLock(const FString& InBaseFilePath)
{
	FScopeLock MapScope(&MergeMapLock);
	if (TSharedPtr<FCriticalSection>* Found = MergeLocks.Find(InBaseFilePath))
	{
		return *Found->Get();
	}
	TSharedPtr<FCriticalSection> NewLock = MakeShared<FCriticalSection>();
	FCriticalSection& Ref = *NewLock.Get();
	MergeLocks.Add(InBaseFilePath, MoveTemp(NewLock));
	MergeLocks.Compact(); // 防止后续 rehash 导致 TSharedPtr 拷贝移动后引用失效
	return Ref;
}

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
	FScopeLock MergeScope(&GetMergeLock(InBaseFilePath));

	OutMergedFilePath = InBaseFilePath;

	// 备注（潜在问题）：
	// 1) 若当前生效的不是 HDiffPatchUE 而是其它 IBinariesDiffPatchFeature 实现，
	//    其 PatchDiffToFile 可能退化为「整文件读入内存」的默认实现（见 HotPatcher BinariesPatchFeature.cpp），
	//    对几个 GiB 的大 pak 会造成显著内存峰值。HDiffPatchUE 走真正的流式实现（约 1 MiB 峰值），不受影响。
	// 2) 本函数只依赖 PatchDiffToFile 返回的 bool 判断成功，未对重建结果做内容哈希校验。
	//    若需要端到端完整性保障，应在合并后用 InFile.Hash 校验 OutMergedFilePath（调用方负责）。
	// 3) StagedForRestart 分支的成功前提是：重启后、引擎挂载该基础文件「之前」能把它交换回去。
	//    详见 FinalizePendingMerges 的备注——项目自带 Paks 在引擎启动时已被自动挂载，
	//    因此 UEngineSubsystem::Initialize 里调用 FinalizePendingMerges 常常来不及（文件已锁）。

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
	const FString Extension = FPaths::GetExtension(InBaseFilePath).ToLower();
    const bool bIoStore = Extension == TEXT("utoc") || Extension == TEXT("ucas");
    if (!bIoStore && IFileManager::Get().Move(*InBaseFilePath, *TmpPath, /*ReplaceExisting=*/true, /*EvenIfReadOnly=*/false, /*Attributes=*/false))
	{
		return EBinaryMergeResult::Success;
	}

	// 替换失败：通常是基础文件被占用（运行中 pak 已挂载，Windows 下文件锁）。
	// 暂存为 <基础文件>.pending，待下次启动、挂载前由 FinalizePendingMerges 交换。
	const FString PendingPath = InBaseFilePath + TEXT(".pending");
	if (IFileManager::Get().Move(*PendingPath, *TmpPath, /*ReplaceExisting=*/true, /*EvenIfReadOnly=*/true, /*Attributes=*/false))
	{
		UE_LOG(LogCloudUpdate, Warning,
			TEXT("二进制补丁合并：已暂存为 %s，重启后生效"), *PendingPath);
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

	// 备注（时序相关）：
	// 本函数只有在「目标基础文件当前未被占用（未挂载、未打开）」时才能交换成功。
	// 项目的 Paks 目录（ProjectContentDir/Paks、移动端 ProjectSavedDir/Paks）下的 pak/utoc
	// 会在引擎启动极早期被 FPakPlatformFile 自动挂载，而 UEngineSubsystem::Initialize 的
	// 调用时机晚于该挂载，此时 Windows 上文件已被锁、Move 可能失败。因此：
	//   - Android：已在 JNI 早期钩子里做「挂载之前」的交换（见 BNSAndroidStorage.cpp）；
	//   - Windows：monolithic 构建在 main 之前交换（见 BNSWindowsStorageBootstrap.inl）；
	//   - 本函数保留为兜底，并负责成组交换 + 整组回滚 + 崩溃恢复。
	// 调用方应检查返回值：若返回 0 但期望 >0，说明有暂存补丁因文件占用未交换。

	// 崩溃恢复：若只有 <base>.swapbak 而 <base> 不存在，说明上次交换在"备份完成、改名完成前"
	// 中断，先把备份恢复回去；两者都在则备份已无用，清掉以免无限堆积。
	{
		TArray<FString> StaleBackups;
		IFileManager::Get().FindFilesRecursive(StaleBackups, *InDirectory, TEXT("*.swapbak"), /*Files=*/true, /*Dirs=*/false, bIncludeSubdirectories);
		for (const FString& BackupPath : StaleBackups)
		{
			FString BasePath = BackupPath;
			if (!BasePath.RemoveFromEnd(TEXT(".swapbak"))) continue;
			if (!IFileManager::Get().FileExists(*BasePath))
			{
				if (IFileManager::Get().Move(*BasePath, *BackupPath, true, true))
				{
					UE_LOG(LogCloudUpdate, Warning, TEXT("恢复上次交换中断的备份：%s"), *BasePath);
				}
			}
			else
			{
				IFileManager::Get().Delete(*BackupPath, false, true);
			}
		}
	}

	// 按容器组分组：<base>.utoc 与 <base>.ucas 必须同组交换，否则会留下
	// "新 .utoc + 旧 .ucas"（或反之），引擎按 .utoc 读容器时数据不一致 → 资源错误/崩溃。
	// 普通单文件（如修复用的 *.bin）各自成组，行为与旧版一致。
	TArray<FString> GroupKeys;
	TMap<FString, TArray<FString>> Groups;
	for (const FString& PendingPath : Pending)
	{
		FString BasePath = PendingPath;
		if (!BasePath.RemoveFromEnd(TEXT(".pending"))) continue;
		const FString Extension = FPaths::GetExtension(BasePath).ToLower();
		const FString Key = (Extension == TEXT("utoc") || Extension == TEXT("ucas"))
			? BasePath.LeftChop(Extension.Len() + 1)   // 去掉 ".utoc"/".ucas"，同组共用同一 base
			: BasePath;
		if (!Groups.Contains(Key)) GroupKeys.Add(Key);
		Groups.FindOrAdd(Key).Add(BasePath);
	}

	int32 Swapped = 0;
	for (const FString& Key : GroupKeys)
	{
		const TArray<FString>& Bases = Groups[Key];

		// 容器组必须成套：有 .utoc 就必须有 .ucas（反之亦然）。不完整时拒绝交换、保留
		// .pending 原样，让上层按"待生效/未完成"处理，绝不制造半套容器。
		bool bHasUtoc = false, bHasUcas = false;
		for (const FString& Base : Bases)
		{
			const FString Extension = FPaths::GetExtension(Base).ToLower();
			bHasUtoc |= Extension == TEXT("utoc");
			bHasUcas |= Extension == TEXT("ucas");
		}
		if (bHasUtoc != bHasUcas)
		{
			UE_LOG(LogCloudUpdate, Error, TEXT("容器组不完整，拒绝交换以免留下半套容器：%s"), *Key);
			continue;
		}

		TArray<FString> SwappedBases;
		TArray<TPair<FString, FString>> Backups;
		bool bGroupOk = true;
		for (const FString& BasePath : Bases)
		{
			// 与 ApplyPatchToFileEx 共用同一把锁，防止交换与合并并发写同一基础文件
			FScopeLock MergeScope(&GetMergeLock(BasePath));
			const FString PendingPath = BasePath + TEXT(".pending");
			const FString BackupPath = BasePath + TEXT(".swapbak");

			// 先备份现有基础文件，任何一步失败都能整组回滚到"全旧"。
			if (IFileManager::Get().FileExists(*BasePath))
			{
				IFileManager::Get().Delete(*BackupPath, false, true);
				if (!IFileManager::Get().Move(*BackupPath, *BasePath, true, true))
				{
					bGroupOk = false;
					break;
				}
				Backups.Emplace(BasePath, BackupPath);
			}
			if (!IFileManager::Get().Move(*BasePath, *PendingPath, true, true))
			{
				bGroupOk = false;
				break;
			}
			SwappedBases.Add(BasePath);
		}

		if (!bGroupOk)
		{
			// 整组回滚：先把已换上的新内容退回 .pending，再把备份恢复回去。
			for (int32 Index = SwappedBases.Num() - 1; Index >= 0; --Index)
			{
				IFileManager::Get().Move(*(SwappedBases[Index] + TEXT(".pending")), *SwappedBases[Index], true, true);
			}
			for (int32 Index = Backups.Num() - 1; Index >= 0; --Index)
			{
				IFileManager::Get().Move(*Backups[Index].Key, *Backups[Index].Value, true, true);
			}
			UE_LOG(LogCloudUpdate, Error, TEXT("容器组交换失败（文件可能仍被占用），已整组回滚：%s"), *Key);
			continue;
		}

		for (const TPair<FString, FString>& Backup : Backups)
		{
			IFileManager::Get().Delete(*Backup.Value, false, true);
		}
		Swapped += SwappedBases.Num();
		UE_LOG(LogCloudUpdate, Log, TEXT("已交换暂存补丁组 %s（%d 个文件）"), *Key, SwappedBases.Num());
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

	// 备注（潜在问题）：
	// a) 并发冲突：AutoMergeDirectory 在后台线程执行，而更新下载流程的 HandleBinaryPatchEntry
	//    也会对相同基础文件调用 ApplyPatchToFileEx（写 Foo.merged.tmp / Foo.pending）。
	//    若用户在更新进行中手动触发 AutoMergePatches，两条路径可能同时操作同一临时文件，造成结果互相覆盖。
	//    当前仅用 bAutoMergeRunning 防止「两次自动合并」并发，未与下载流程互斥——如需严格安全，
	//    应引入一个跨流程的合并锁（如 FCriticalSection 或统一的合并队列）。
	// b) Total 与最终实际处理列表来自同一次 FindPatchFiles，本函数内一致；
	//    但子系统 AutoMergePatches 在进入后台前另算了一次 Total（用于先广播一次进度），
	//    两次扫描之间若目录变化，进度总数可能和实际不符（仅 UI 显示问题，不影响正确性）。
	// c) 后台线程内调用 GetFeature -> IModularFeatures::Get()：通常安全（读取已注册列表 + 内部加锁），
	//    但 ModularFeatures 的注册发生在主线程，假设「注册已完成、仅读取」成立；若在极早期模块加载阶段并发调用需谨慎。
	// d) 若某基础文件在合并时已被占用（运行中 pak 已挂载），会得到 StagedForRestart：
	//    结果暂存为 .pending，且仅在本次自动合并里计入「成功」，但需重启后才能真正生效（见 FinalizePendingMerges 时序备注）。

	for (const FString& PatchPath : Patches)
	{
		// 补丁与基础文件同目录：X.patch -> X
        if (ProgressCb) ProgressCb(Completed, Total, PatchPath, true);
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
                Result.RestartRequiredCount += R == EBinaryMergeResult::StagedForRestart ? 1 : 0;
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
	// 备注（边界情况）：本函数只剥离「末尾一个」.patch 后缀。
	// 若补丁文件名为 Foo.patch.patch，则推导结果为 Foo.patch（而非预期的 Foo），
	// 会导致找不到基础文件而被跳过。常规 HotPatcher 产物为 Foo.pak.patch / Foo.utoc.patch，不受影响。
	return Result;
}
