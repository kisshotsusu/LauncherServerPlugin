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
// 局域网回退：枚举本机网卡地址、解析服务器主机名
#include "SocketSubsystem.h"
#include "AddressInfoTypes.h"
#include "IPAddress.h"

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
		// Android's game thread has a limited pthread stack. A megabyte-sized
		// local array can overwrite an adjacent thread before hitting a guard page.
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(64 * 1024);
		for (int64 Offset = 0; Offset < FileSize; )
		{
			const int64 ToRead = FMath::Min<int64>(Buffer.Num(), FileSize - Offset);
			if (!Handle->ReadAt(Buffer.GetData(), ToRead, Offset))
			{
				return false;
			}
			Md5.Update(Buffer.GetData(), static_cast<int32>(ToRead));
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
  return Name.EndsWith(TEXT(".apk")) || Name.EndsWith(TEXT(".ipa")) || Name.EndsWith(TEXT(".exe")) || Name.EndsWith(TEXT(".dll")) || Name.EndsWith(TEXT(".so")) || Name.Contains(TEXT(".so.")) || Name.EndsWith(TEXT(".dylib"));
 }
}

namespace CloudUpdatePrivate
{
 FCloudUrlParts ParseCloudUrl(const FString& InUrl)
 {
  FCloudUrlParts Parts;
  const int32 SchemeEnd = InUrl.Find(TEXT("://"));
  if (SchemeEnd == INDEX_NONE) return Parts;
  Parts.Scheme = InUrl.Left(SchemeEnd);
  if (Parts.Scheme.IsEmpty()) return Parts;
  // 只处理 http(s)：局域网回退只作用于 HTTP 请求，其它 scheme 一律视为无效输入。
  if (!Parts.Scheme.Equals(TEXT("http"), ESearchCase::IgnoreCase)
   && !Parts.Scheme.Equals(TEXT("https"), ESearchCase::IgnoreCase)) return Parts;

  const FString Rest = InUrl.Mid(SchemeEnd + 3);
  if (Rest.IsEmpty()) return Parts;

  int32 AuthorityEnd = 0;
  if (Rest.StartsWith(TEXT("[")))
  {
   // 括号形式的 IPv6 主机：跳过 ] 之后再找端口与路径。
   const int32 Close = Rest.Find(TEXT("]"));
   if (Close == INDEX_NONE) return Parts;
   Parts.Host = Rest.Mid(1, Close - 1);
   const int32 After = Close + 1;
   const int32 Slash = Rest.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, After);
   AuthorityEnd = Slash == INDEX_NONE ? Rest.Len() : Slash;
   const FString PortText = Rest.Mid(After, AuthorityEnd - After);
   if (PortText.StartsWith(TEXT(":"))) Parts.Port = FCString::Atoi(*PortText.Mid(1));
  }
  else
  {
   const int32 Slash = Rest.Find(TEXT("/"));
   AuthorityEnd = Slash == INDEX_NONE ? Rest.Len() : Slash;
   const FString HostPort = Rest.Left(AuthorityEnd);
   // 从右往左找 ':'，避免把 IPv6 里的冒号当端口分隔符。
   const int32 Colon = HostPort.Find(TEXT(":"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
   if (Colon == INDEX_NONE)
   {
    Parts.Host = HostPort;
   }
   else
   {
    Parts.Host = HostPort.Left(Colon);
    Parts.Port = FCString::Atoi(*HostPort.Mid(Colon + 1));
   }
  }

  if (Parts.Host.IsEmpty()) return Parts;
  Parts.Path = Rest.Mid(AuthorityEnd);
  Parts.bValid = true;
  return Parts;
 }

 FString ComposeCloudUrl(const FCloudUrlParts& InParts)
 {
  if (!InParts.bValid || InParts.Host.IsEmpty()) return FString();
  FString Host = InParts.Host;
  if (Host.Contains(TEXT(":"))) Host = TEXT("[") + Host + TEXT("]");
  const bool bDefaultPort = InParts.Port <= 0
   || (InParts.Scheme.Equals(TEXT("http"), ESearchCase::IgnoreCase) && InParts.Port == 80)
   || (InParts.Scheme.Equals(TEXT("https"), ESearchCase::IgnoreCase) && InParts.Port == 443);
  return InParts.Scheme + TEXT("://") + Host
   + (bDefaultPort ? FString() : FString::Printf(TEXT(":%d"), InParts.Port))
   + InParts.Path;
 }

 bool IsIPv4Literal(const FString& InText)
 {
  TArray<FString> Segments;
  InText.ParseIntoArray(Segments, TEXT("."), false);
  if (Segments.Num() != 4) return false;
  for (const FString& Segment : Segments)
  {
   if (Segment.IsEmpty() || Segment.Len() > 3) return false;
   for (const TCHAR Ch : Segment) if (!FChar::IsDigit(Ch)) return false;
   const int32 Value = FCString::Atoi(*Segment);
   if (Value < 0 || Value > 255) return false;
  }
  return true;
 }

 bool DecideLanFallback(const FString& InOriginalUrl, const FString& InLanBaseUrl,
  const TArray<FString>& InResolvedIps, const TArray<FString>& InLocalIps,
  FString& OutUrl, FString& OutReason)
 {
  OutUrl = InOriginalUrl;
  OutReason.Reset();
  if (InOriginalUrl.IsEmpty()) { OutReason = TEXT("URL 为空"); return false; }
  if (InLanBaseUrl.IsEmpty()) { OutReason = TEXT("未配置局域网服务器地址"); return false; }

  const FCloudUrlParts Original = ParseCloudUrl(InOriginalUrl);
  const FCloudUrlParts Lan = ParseCloudUrl(InLanBaseUrl);
  if (!Original.bValid) { OutReason = TEXT("待处理地址不是 http(s) URL，跳过回退"); return false; }
  if (!Lan.bValid) { OutReason = TEXT("局域网服务器地址不是 http(s) URL，跳过回退"); return false; }
  if (Lan.Host.Equals(Original.Host, ESearchCase::IgnoreCase) && Lan.Port == Original.Port)
  { OutReason = TEXT("地址已经是局域网服务器地址"); return false; }

  FString Matched;
  for (const FString& Ip : InResolvedIps)
  {
   if (InLocalIps.Contains(Ip)) { Matched = Ip; break; }
  }
  if (Matched.IsEmpty())
  { OutReason = TEXT("服务器地址不是本机地址，无需回退"); return false; }

  FCloudUrlParts Effective = Original;
  Effective.Host = Lan.Host;
  Effective.Port = Lan.Port > 0 ? Lan.Port : Original.Port;
  OutUrl = ComposeCloudUrl(Effective);
  OutReason = FString::Printf(TEXT("服务器 IP %s 与本机相同，已回退到 %s"), *Matched, *Lan.Host);
  return true;
 }

 void GetLocalIPv4Addresses(TArray<FString>& Out)
 {
  Out.Reset();
  ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  if (!Sockets) return;
  TArray<TSharedPtr<FInternetAddr>> Adapters;
  if (!Sockets->GetLocalAdapterAddresses(Adapters)) return;
  for (const TSharedPtr<FInternetAddr>& Address : Adapters)
  {
   if (!Address.IsValid()) continue;
   const FString Text = Address->ToString(false);
   if (IsIPv4Literal(Text) && Text != TEXT("0.0.0.0")) Out.AddUnique(Text);
  }
 }

 bool ResolveHostIPv4(const FString& InHost, TArray<FString>& Out)
 {
  Out.Reset();
  if (InHost.IsEmpty()) return false;
  if (IsIPv4Literal(InHost)) { Out.Add(InHost); return true; }
  ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
  if (!Sockets) return false;
  const FAddressInfoResult Info = Sockets->GetAddressInfo(*InHost, nullptr, EAddressInfoFlags::Default, NAME_None);
  for (const FAddressInfoResultData& Data : Info.Results)
  {
   // Data.Address 是 TSharedRef，必然有效，无需判空。
   const FString Text = Data.Address->ToString(false);
   if (IsIPv4Literal(Text)) Out.AddUnique(Text);
  }
  return Out.Num() > 0;
 }

 bool IsContainerSetFile(const FString& InPath)
 {
  const FString Extension = FPaths::GetExtension(InPath).ToLower();
  return Extension == TEXT("pak") || Extension == TEXT("utoc") || Extension == TEXT("ucas");
 }

 FString GetContainerSetPakPath(const FString& InPath)
 {
  return FPaths::ChangeExtension(InPath, TEXT(".pak"));
 }
}
