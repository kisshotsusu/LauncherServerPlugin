// ======================================================================
// CloudUpdate 内部工具函数实现
// ======================================================================
#include "CloudUpdateUtil.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"
#include "CloudUpdateTypes.h"
#include "CloudUpdateBinaryMerge.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProperties.h"

namespace CloudUpdatePrivate
{
 FString ResolveUpdatePlatform(const FString& RuntimePlatform, const FString& AndroidCookFlavor)
 {
  for (const TCHAR* Supported : {TEXT("Windows"), TEXT("Android"), TEXT("IOS")})
   if (RuntimePlatform.Equals(Supported, ESearchCase::IgnoreCase)) return Supported;
  return TEXT("");
 }

 FString NormalizeCookPlatform(const FString& SourcePlatform)
 {
  for (const TCHAR* Flavor : {TEXT("Android_ASTC"), TEXT("Android_ETC2"), TEXT("Android_DXT"), TEXT("Android_Multi"), TEXT("Android_OpenXR")})
   if (SourcePlatform.Equals(Flavor, ESearchCase::IgnoreCase)) return TEXT("Android");
  return ResolveUpdatePlatform(SourcePlatform, TEXT(""));
 }

 FString GetRuntimeUpdatePlatform()
 {
  return ResolveUpdatePlatform(FPlatformProperties::IniPlatformName(), TEXT(""));
 }
 FString GetUpdatePakDirectory()
 {
#if PLATFORM_ANDROID || PLATFORM_IOS
  return FPaths::ProjectSavedDir() / TEXT("Paks");
#else
  return FPaths::ProjectContentDir() / TEXT("Paks");
#endif
 }
 bool MatchesResponsePlatform(const TSharedPtr<FJsonObject>& Json, const FString& Platform)
 {
  if (!Json.IsValid()) return false;
  if (!Json->HasField(TEXT("platform"))) return true; // Legacy single-platform servers.
  FString ReturnedPlatform;
  return Json->TryGetStringField(TEXT("platform"), ReturnedPlatform)
   && ReturnedPlatform.Equals(Platform, ESearchCase::IgnoreCase);
 }
 bool BuildHotPatcherFiles(const TSharedPtr<FJsonObject>& Json, const FString& Platform, const FString& BaseUrl,
   const FString& Version, bool bIoStore, TArray<FCloudDownloadFile>& OutFiles, FString& OutError)
 {
  OutFiles.Reset();
  const TSharedPtr<FJsonObject>* Map = nullptr;
  const TSharedPtr<FJsonObject>* PlatformInfo = nullptr;
  const TArray<TSharedPtr<FJsonValue>>* Infos = nullptr;
  FString SourcePlatform = Platform;
  if (Json.IsValid() && Json->TryGetObjectField(TEXT("pakFilesMap"), Map))
  {
   if (!(*Map)->HasField(Platform))
   {
    int32 Matches = 0;
    for (const auto& Pair : (*Map)->Values)
     if (NormalizeCookPlatform(FString(Pair.Key)) == Platform) { SourcePlatform = FString(Pair.Key); ++Matches; }
    if (Matches > 1) { OutError = TEXT("多个安卓 Cook 格式不能合并，请选择单一格式的补丁来源"); return false; }
   }
  }
  if (!Map || !(*Map)->TryGetObjectField(SourcePlatform, PlatformInfo)
   || !(*PlatformInfo)->TryGetArrayField(TEXT("pakFileInfos"), Infos))
  { OutError = TEXT("HotPatcher 文件清单缺少目标平台：") + Platform; return false; }
  TSet<FString> Names;
  for (const TSharedPtr<FJsonValue>& Value : *Infos)
  {
   if (!Value.IsValid() || Value->Type != EJson::Object) return false;
   const auto Info = Value->AsObject();
   FCloudDownloadFile File;
   Info->TryGetStringField(TEXT("fileName"), File.FileName);
   if (!IsSafeRelativePath(File.FileName) || File.FileName != FPaths::GetCleanFilename(File.FileName)
    || IsGameBinaryPath(File.FileName) || Names.Contains(File.FileName.ToLower()))
   { OutError = TEXT("HotPatcher 文件名无效或重复：") + File.FileName; OutFiles.Reset(); return false; }
   Names.Add(File.FileName.ToLower());
   Info->TryGetStringField(TEXT("hash"), File.Hash);
   File.bSizeKnown = Info->TryGetNumberField(TEXT("fileSize"), File.FileSize);
   if (!File.bSizeKnown) File.bSizeKnown = Info->TryGetNumberField(TEXT("size"), File.FileSize);
   if (File.FileSize < 0) { OutFiles.Reset(); return false; }
   File.Url = BaseUrl / Version / SourcePlatform / File.FileName;
   File.TargetRelativePath = File.FileName;
   File.bBinaryPatch = FCloudBinaryMerge::IsPatchFile(File.FileName);
   const FString BaseName = File.bBinaryPatch ? FCloudBinaryMerge::GetBaseFileName(File.FileName) : File.FileName;
   File.Kind = BaseName.EndsWith(TEXT(".pak")) ? ECloudDownloadKind::ContentPak :
    (BaseName.EndsWith(TEXT(".utoc")) || BaseName.EndsWith(TEXT(".ucas"))) ? ECloudDownloadKind::IoStoreContainer : ECloudDownloadKind::ExternFile;
   if (File.bBinaryPatch) File.FallbackUrl = BaseUrl / Version / SourcePlatform / BaseName;
   OutFiles.Add(MoveTemp(File));
  }
  // Legacy HotPatcher lists only pak sidecars for IoStore. Add the paired files
  // once, while respecting modern lists which already carry size/hash metadata.
  if (bIoStore)
  {
   const auto ListedFiles = OutFiles;
   for (const auto& File : ListedFiles)
   {
    if (File.bBinaryPatch || !File.FileName.EndsWith(TEXT(".pak"))) continue;
    for (const FString& Extension : {FString(TEXT(".utoc")), FString(TEXT(".ucas"))})
    {
     const FString Name = FPaths::GetBaseFilename(File.FileName) + Extension;
     if (Names.Contains(Name.ToLower())) continue;
     Names.Add(Name.ToLower());
     FCloudDownloadFile Container;
     Container.FileName = Container.TargetRelativePath = Name;
     Container.Url = BaseUrl / Version / SourcePlatform / Name;
     Container.Kind = ECloudDownloadKind::IoStoreContainer;
     OutFiles.Add(MoveTemp(Container));
    }
   }
  }
  if (OutFiles.IsEmpty()) { OutError = TEXT("目标平台没有可下载的资源文件"); return false; }
  return true;
 }
	FString BytesToHex(const uint8* InBytes, int32 InCount)
	{
		static const TCHAR* HexDigits = TEXT("0123456789abcdef");
		FString Result;
		Result.Reserve(InCount * 2);
		for (int32 i = 0; i < InCount; ++i)
		{
			Result += HexDigits[(InBytes[i] >> 4) & 0xF];
			Result += HexDigits[InBytes[i] & 0xF];
		}
		return Result;
	}

	bool ComputeFileHash(const FString& InPath, int64& OutSize, FString& OutHash)
	{
		OutSize = 0;
		OutHash.Empty();
		TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*InPath));
		if (!Handle)
		{
			return false;
		}
		const int64 FileSize = Handle->Size();
		OutSize = FileSize;
		FMD5 Md5;
		uint8 Buffer[1024 * 1024];
		for (int64 Offset = 0; Offset < FileSize; )
		{
			const int64 ToRead = FMath::Min<int64>(sizeof(Buffer), FileSize - Offset);
			if (!Handle->ReadAt(Buffer, ToRead, Offset))
			{
				return false;
			}
			Md5.Update(Buffer, static_cast<int32>(ToRead));
			Offset += ToRead;
		}
		uint8 Digest[16];
		Md5.Final(Digest);
		OutHash = BytesToHex(Digest, 16);
		return true;
	}

	FString NormalizeSlashes(const FString& InPath)
	{
		FString Result = InPath;
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Result;
	}

	bool IsSafeRelativePath(const FString& InPath)
	{
		if (InPath.IsEmpty())
		{
			return false;
		}

		const FString Normalized = NormalizeSlashes(InPath);
		if (Normalized.StartsWith(TEXT("/")) || Normalized.Contains(TEXT("..")))
		{
			return false;
		}

		TArray<FString> Segments;
		Normalized.ParseIntoArray(Segments, TEXT("/"), true);
		for (const FString& Segment : Segments)
		{
			if (Segment.IsEmpty() || Segment == TEXT(".") || Segment == TEXT(".."))
			{
				return false;
			}
		}
		return true;
	}

	TArray<FString> SplitVersionParts(const FString& InVersion)
	{
		FString Tmp = InVersion;
		Tmp.ReplaceInline(TEXT("-"), TEXT("."));
		Tmp.ReplaceInline(TEXT("_"), TEXT("."));
		TArray<FString> Parts;
		Tmp.ParseIntoArray(Parts, TEXT("."), true);
		return Parts;
	}

	bool IsVersionNewer(const FString& InA, const FString& InB)
	{
		const TArray<FString> AParts = SplitVersionParts(InA);
		const TArray<FString> BParts = SplitVersionParts(InB);
		const int32 Count = FMath::Max(AParts.Num(), BParts.Num());
		for (int32 i = 0; i < Count; ++i)
		{
			const FString A = AParts.IsValidIndex(i) ? AParts[i] : TEXT("0");
			const FString B = BParts.IsValidIndex(i) ? BParts[i] : TEXT("0");
			if (A.IsNumeric() && B.IsNumeric())
			{
				const int64 AI = FCString::Atoi64(*A);
				const int64 BI = FCString::Atoi64(*B);
				if (AI != BI)
				{
					return AI > BI;
				}
			}
			else if (!A.Equals(B, ESearchCase::IgnoreCase))
			{
				return A > B;
			}
		}
		return false;
	}
}

namespace CloudUpdatePrivate
{
 bool IsGameBinaryPath(const FString& InPath)
 {
  FString Name = InPath.ToLower();
  Name.RemoveFromEnd(TEXT(".patch"));
  return Name.EndsWith(TEXT(".exe")) || Name.EndsWith(TEXT(".dll")) || Name.EndsWith(TEXT(".so")) || Name.Contains(TEXT(".so.")) || Name.EndsWith(TEXT(".dylib"));
 }
}
