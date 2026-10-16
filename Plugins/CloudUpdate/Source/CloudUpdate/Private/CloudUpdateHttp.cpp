// ======================================================================
// CloudUpdate HTTP 层：JSON 拉取 / 文件下载
// (由 CloudUpdateService.cpp 按职责拆分，逻辑与原文一致)
// ======================================================================
#include "CloudUpdateService.h"
#include "CloudUpdateSubsystem.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdate.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Async/Async.h"
#include "Misc/App.h"
#include "FlibPakHelper.h"

#include "CloudUpdateUtil.h"
#include "CloudUpdateDownload.h"
#include "Containers/Ticker.h"
using namespace CloudUpdatePrivate;

void FCloudUpdateService::FetchJson(const FString& InUrl, TFunction<void(bool, const TSharedPtr<FJsonObject>&)> InCallback)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ResolveRemoteUrl(InUrl));
 Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	Request->SetTimeout(Settings ? Settings->HttpTimeoutSeconds : 60);
	if (Settings)
	{
		if (!Settings->ServerToken.IsEmpty())
		{
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ServerToken));
		}
		for (const FCloudUpdateHttpHeader& H : Settings->CustomHeaders)
		{
			if (!H.Key.IsEmpty())
			{
				Request->SetHeader(H.Key, H.Value);
			}
		}
	}
 TSharedRef<bool> Completed = MakeShared<bool>(false);
 Request->OnProcessRequestComplete().BindLambda([InCallback, Completed](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
	{
        if (*Completed) return;
        *Completed = true;
		TSharedPtr<FJsonObject> Json;
		bool bOk = false;
		if (bConnectedSuccessfully && Resp.IsValid() && EHttpResponseCodes::IsOk(Resp->GetResponseCode()))
		{
			const FString ResponseStr = Resp->GetContentAsString();
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
			bOk = FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid();
		}
		if (InCallback)
		{
			InCallback(bOk, Json);
		}
	});
 if (!Request->ProcessRequest() && !*Completed)
 {
  *Completed = true;
  if (InCallback) InCallback(false, nullptr);
 }
}

namespace
{
class FCloudResponseArchive : public FArchive
{
public:
 explicit FCloudResponseArchive(TSharedRef<FCloudDownloadStream, ESPMode::ThreadSafe> InState) : State(InState) {}
 virtual void Serialize(void* Data, int64 Length) override
 { State->Serialize(Data, Length); if (State->IsError()) SetError(); }
private:
 TSharedRef<FCloudDownloadStream, ESPMode::ThreadSafe> State;
};
}

void FCloudUpdateService::DownloadFileTo(const FString& InUrl, const FString& InTargetPath, int32 InRetriesLeft,
 TFunction<void(bool)> InCallback, TFunction<void(int64, int64)> InProgress, const FString& ExpectedHash, int64 ExpectedSize)
{
 if (bAbortRequested) { if (InCallback) InCallback(false); return; }
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(InTargetPath), true);
 const FString TempPath = InTargetPath + TEXT(".download");
 // Hash-based identity also survives signed URL renewal. Without a hash, require If-Range validation.
 const FString Identity = FString::Printf(TEXT("%s\n%lld"),
  ExpectedHash.IsEmpty() ? *ResolveRemoteUrl(InUrl) : *ExpectedHash.ToLower(), ExpectedSize);
 auto Stream = MakeShared<FCloudDownloadStream, ESPMode::ThreadSafe>(TempPath, Identity, ExpectedSize);
 if (ExpectedHash.IsEmpty() && Stream->Offset > 0 && Stream->Validator.IsEmpty())
 { Stream->Discard(); Stream = MakeShared<FCloudDownloadStream, ESPMode::ThreadSafe>(TempPath, Identity, ExpectedSize); }
 const bool bCached = !ExpectedHash.IsEmpty() && ExpectedSize >= 0 && Stream->Offset == ExpectedSize && IFileManager::Get().FileSize(*TempPath) == ExpectedSize;
 auto Request = FHttpModule::Get().CreateRequest();
 Request->SetURL(ResolveRemoteUrl(InUrl));
 Request->SetVerb(TEXT("GET"));
 // Headers must be validated on the HTTP thread before a single response byte is appended.
 Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
 const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
 if (Settings)
 {
  if (!Settings->ServerToken.IsEmpty()) Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Settings->ServerToken));
  for (const FCloudUpdateHttpHeader& H : Settings->CustomHeaders)
   if (!H.Key.IsEmpty()) Request->SetHeader(H.Key, H.Value);
 }
 Request->SetHeader(TEXT("Accept-Encoding"), TEXT("identity"));
 if (Stream->Offset > 0)
 {
  Request->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-"), Stream->Offset));
  if (!Stream->Validator.IsEmpty()) Request->SetHeader(TEXT("If-Range"), Stream->Validator);
  UE_LOG(LogCloudUpdate, Display, TEXT("断点续传 %s，从 %lld 字节继续"), *InTargetPath, Stream->Offset);
 }
 Request->SetTimeout(Settings ? Settings->HttpTimeoutSeconds : 60);
 Request->OnStatusCodeReceived().BindLambda([Stream](FHttpRequestPtr, int32 Code) { Stream->Status(Code); });
 Request->OnHeaderReceived().BindLambda([Stream](FHttpRequestPtr, const FString& Name, const FString& Value) { Stream->Header(Name, Value); });
 Request->OnProcessRequestComplete().BindLambda([Stream](FHttpRequestPtr, FHttpResponsePtr, bool Connected) { Stream->Finish(Connected); });
 const bool bVerifyHash = !Settings || Settings->bVerifyDownloadHash;
 TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
 auto Finish = [WeakThis, InUrl, InTargetPath, TempPath, InRetriesLeft, InCallback, InProgress, ExpectedHash, ExpectedSize, Stream](bool bOk, bool bDiscard)
 {
  const auto Service = WeakThis.Pin();
  if (!Service || Service->bAbortRequested) { if (InCallback) InCallback(false); return; }
  if (bDiscard) { Stream->Discard(); if (InProgress) InProgress(0, ExpectedSize); }
  const int64 Size = IFileManager::Get().FileSize(*TempPath);
  if (bOk)
  {
   if (InProgress) InProgress(Size, Size);
   const bool bIoStore = InTargetPath.EndsWith(TEXT(".utoc")) || InTargetPath.EndsWith(TEXT(".ucas"));
   bool bPending = bIoStore;
   bool bMoved = IFileManager::Get().Move(*(bPending ? InTargetPath + TEXT(".pending") : InTargetPath), *TempPath, true, true);
   if (!bMoved && !bPending)
   { bPending = true; bMoved = IFileManager::Get().Move(*(InTargetPath + TEXT(".pending")), *TempPath, true, true); }
   if (bMoved)
   {
    IFileManager::Get().Delete(*(TempPath + TEXT(".json")), false, true);
    Service->bRestartRequired |= bPending;
    if (InCallback) InCallback(true);
    return;
   }
  }
  if (InRetriesLeft > 0)
  {
   UE_LOG(LogCloudUpdate, Warning, TEXT("下载未完成 %s，保留 %lld 字节，剩余重试 %d 次"), *InUrl,
    FMath::Max<int64>(IFileManager::Get().FileSize(*TempPath), 0), InRetriesLeft);
   Service->DownloadFileTo(InUrl, InTargetPath, InRetriesLeft - 1, InCallback, InProgress, ExpectedHash, ExpectedSize);
  }
  else if (InCallback) InCallback(false);
 };
 // Poll actual stream writes, not platform-specific HTTP progress notifications (which may stall).
 // Holding the request here also keeps its delegates alive until completion has been observed.
 FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
  [Stream, Request, WeakThis, InProgress, ExpectedHash, ExpectedSize, bVerifyHash, bCached, TempPath, Finish = MoveTemp(Finish)](float) mutable
 {
  int64 Done, Total; bool Finished, Success;
  Stream->Snapshot(Done, Total, Finished, Success);
  const auto Service = WeakThis.Pin();
  if (!Service || Service->bAbortRequested) Request->CancelRequest();
  else if (InProgress) InProgress(Done, Total);
  if (!Finished && !bCached) return true;
  const bool bTransportOk = bCached || Success;
  const bool bBadRange = Stream->bInvalidRange;
  // Disk reads and MD5 of large containers must never block game-thread UI/progress updates.
  Async(EAsyncExecution::ThreadPool, [Stream, ExpectedHash, ExpectedSize, bVerifyHash, bTransportOk, bBadRange,
   TempPath, Finish = MoveTemp(Finish)]() mutable
  {
   bool Ok = bTransportOk;
   bool Discard = bBadRange;
   if (Ok)
   {
    const int64 Size = IFileManager::Get().FileSize(*TempPath);
    Ok = Size >= 0 && (ExpectedSize < 0 || Size == ExpectedSize);
    if (Ok && bVerifyHash && !ExpectedHash.IsEmpty())
    { int64 HashSize = 0; FString Hash; Ok = ComputeFileHash(TempPath, HashSize, Hash) && Hash.Equals(ExpectedHash, ESearchCase::IgnoreCase); }
    Discard |= !Ok;
   }
   AsyncTask(ENamedThreads::GameThread, [Finish = MoveTemp(Finish), Ok, Discard]() mutable { Finish(Ok, Discard); });
  });
  return false;
 }), 0.1f);
 if (!bCached)
 {
  if (!Request->SetResponseBodyReceiveStream(MakeShared<FCloudResponseArchive>(Stream)) || !Request->ProcessRequest())
   Stream->Finish(false);
 }
}
