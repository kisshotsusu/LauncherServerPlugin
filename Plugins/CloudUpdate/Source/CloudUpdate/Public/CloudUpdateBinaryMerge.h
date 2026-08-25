// ======================================================================
// CloudUpdate 二进制补丁合并
// 复用 HotPatcher 的 IBinariesDiffPatchFeature（由 HDiffPatchUE 在运行时注册）
// 将 .patch 增量应用到本地基础 pak/utoc 文件，重建出更新后的版本。
// 支持 pak（单文件）与 utoc/ucas（IoStore 容器）两种格式，因为差异在字节层面完成，
// 对容器格式本身透明。
// ======================================================================
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
// EBinaryMergeResult 定义于此；显式包含，避免依赖包含顺序（该头被多个翻译单元间接引用）。
#include "CloudUpdateTypes.h"

class IBinariesDiffPatchFeature;

/** 目录级自动合并的统计结果 */
struct CLOUDUPDATE_API FBinaryMergeResult
{
	/** 成功合并（含已暂存待重启）的文件数 */
	int32 Succeeded = 0;
	/** 失败的文件数 */
	int32 Failed = 0;
	/** 失败（无法合并）的补丁文件完整路径列表 */
	TArray<FString> FailedFiles;
};

class CLOUDUPDATE_API FCloudBinaryMerge
{
public:
	/** HDiffPatch（IBinariesDiffPatchFeature）是否在运行时可用 */
	static bool IsHDiffPatchAvailable();

	/**
	 * 取第一个（或指定名称的）二进制补丁特性；不存在返回 nullptr。
	 * InFeatureName 为空时返回 GetModularFeatureImplementations 的第一个。
	 */
	static IBinariesDiffPatchFeature* GetFeature(const FString& InFeatureName = TEXT(""));

	/**
	 * 返回将要使用的特性名：指定名可用则返回它；否则返回第一个可用特性名；
	 * 都没有返回空串。供蓝图查询运行时实际生效的二进制补丁实现（如 HDiffPatchUE）。
	 */
	static FString GetFeatureName(const FString& InFeatureName = TEXT(""));

	/**
	 * 将 InPatchFilePath 应用到 InBaseFilePath（本地已安装的基础文件），
	 * 重建出的新文件原子替换回 InBaseFilePath（先写 .merged.tmp 再 Move）。
	 * 成功返回 true，OutMergedFilePath 等于 InBaseFilePath。
	 */
	static bool ApplyPatchToFile(const FString& InBaseFilePath,
	                             const FString& InPatchFilePath,
	                             FString& OutMergedFilePath,
	                             const FString& InFeatureName = TEXT(""));

	/**
	 * ApplyPatchToFile 的增强版：返回结构化结果。
	 * - Success：已就地替换基础文件，立即可用。
	 * - Failed：无法合并（无 HDiffPatch / 基础或补丁缺失 / 补丁损坏）。
	 * - StagedForRestart：基础文件被占用（如运行中 pak 已挂载，Move 失败），
	 *   已把重建结果暂存为 InBaseFilePath + ".pending"，需重启后由 FinalizePendingMerges 交换生效。
	 *   注意：此分支仅在「基础文件尚未被引擎挂载」时有效（见 FinalizePendingMerges 备注）。
	 *   若基础文件在交换时已处于挂载状态（如项目自带 Paks 目录的 pak/utoc），交换会失败，
	 *   该补丁将不会生效，需改用启动前（引擎挂载前）的交换机制（见 FinalizePendingMerges）。
	 */
	static EBinaryMergeResult ApplyPatchToFileEx(const FString& InBaseFilePath,
	                                             const FString& InPatchFilePath,
	                                             FString& OutMergedFilePath,
	                                             const FString& InFeatureName = TEXT(""));

	/**
	 * 交换所有暂存补丁：扫描目录中的 "*.pending"（即 "<基础文件>.pending"），
	 * 在基础文件未被占用时将其替换回基础文件。返回成功交换的文件数。
	 * 应在引擎启动、pak 尚未挂载前调用（如子系统 Initialize）。
	 */
	static int32 FinalizePendingMerges(const FString& InDirectory, bool bIncludeSubdirectories = true);

	/** 递归查找目录中所有 .patch 文件（含子目录由 bIncludeSubdirectories 控制） */
	static TArray<FString> FindPatchFiles(const FString& InDirectory, bool bIncludeSubdirectories = true);

	/**
	 * 自动扫描目录并顺序合并所有 .patch 到其相邻基础文件（X.patch -> X）。
	 * 基础文件缺失的补丁会被跳过并计入失败。每合并完一个通过 ProgressCb 回报（游戏线程外调用）。
	 * 返回统计结果。该函数在后台线程调用以不阻塞游戏线程。
	 */
	static FBinaryMergeResult AutoMergeDirectory(
		const FString& InDirectory,
		bool bIncludeSubdirectories,
		const FString& InFeatureName,
		TFunction<void(int32 /*Completed*/, int32 /*Total*/, const FString& /*PatchPath*/, bool /*bOk*/)> ProgressCb = nullptr);

	/** 是否为二进制补丁文件名（以 .patch 结尾，大小写不敏感） */
	static bool IsPatchFile(const FString& InFileName);

	/** 由补丁文件名推导基础文件名（去掉末尾 .patch） */
	static FString GetBaseFileName(const FString& InPatchFileName);

private:
	/**
	 * 跨流程（下载流程 / 自动合并 / .pending 交换）合并锁：
	 * 以「基础文件绝对路径」为 key 的细粒度锁，保证对同一基础文件的合并/交换操作串行，
	 * 避免下载流程（HandleBinaryPatchEntry）与后台自动合并（AutoMergeDirectory）并发写
	 * "<base>.merged.tmp" / "<base>.pending" 导致结果互相覆盖的竞态。
	 * 不同基础文件使用各自独立的锁，互不阻塞，不造成全局串行瓶颈。
	 */
	static FCriticalSection& GetMergeLock(const FString& InBaseFilePath);

	/** 保护 MergeLocks 映射的全局锁 */
	static FCriticalSection MergeMapLock;
	/** 每个基础文件路径对应的临界区（进程生命周期内持续存活） */
	static TMap<FString, TSharedPtr<FCriticalSection>> MergeLocks;
};
