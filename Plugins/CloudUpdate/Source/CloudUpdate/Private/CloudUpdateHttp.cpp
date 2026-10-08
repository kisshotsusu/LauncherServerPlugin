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

void FCloudUpdateService::DownloadFileTo(const FString& InUrl, const FString& InTargetPath, int32 InRetriesLeft,
	TFunction<void(bool)> InCallback,
	TFunction<void(int64, int64)> InProgress)
{
	if (bAbortRequested)
	{
		if (InCallback)
		{
			InCallback(false);
		}
		return;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InTargetPath), true);
	const FString TempPath = InTargetPath + TEXT(".download");
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(ResolveRemoteUrl(InUrl));
 Request->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnGameThread);
	Request->SetVerb(TEXT("GET"));
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
	Request->SetTimeout(Settings ? Settings->HttpTimeoutSeconds : 60);
 TSharedRef<int64> HeaderTotal = MakeShared<int64>(-1);
 Request->OnHeaderReceived().BindLambda([HeaderTotal](FHttpRequestPtr Req, const FString& Name, const FString& Value)
 {
  if (Name.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase)) *HeaderTotal = FCString::Atoi64(*Value);
 });
	if (InProgress)
	{
		Request->OnRequestProgress64().BindLambda([InProgress, HeaderTotal](FHttpRequestPtr Req, uint64 BytesSent, uint64 BytesReceived)
		{
            InProgress(static_cast<int64>(BytesReceived), *HeaderTotal);
		});
	}
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
 TSharedRef<bool> Completed = MakeShared<bool>(false);
 Request->OnProcessRequestComplete().BindLambda([WeakThis, InUrl, InTargetPath, TempPath, InRetriesLeft, InCallback, InProgress, Completed](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
	{
        if (*Completed) return;
        *Completed = true;
		auto Service = WeakThis.Pin();
		if (!Service)
		{
			if (InCallback)
			{
				InCallback(false);
			}
			return;
		}
        if (Service->bAbortRequested)
        { IFileManager::Get().Delete(*TempPath, false, true); if (InCallback) InCallback(false); return; }
		const bool bHttpOk = bConnectedSuccessfully && Resp.IsValid() && EHttpResponseCodes::IsOk(Resp->GetResponseCode());
		if (bHttpOk)
		{
			const TArray<uint8>& Content = Resp->GetContent();
            if (InProgress) InProgress(Content.Num(), Content.Num());
			if (FFileHelper::SaveArrayToFile(Content, *TempPath))
			{
				if (IFileManager::Get().Move(*InTargetPath, *TempPath, true, true))
				{
					if (InCallback)
					{
						InCallback(true);
					}
					return;
				}
			}
		}
		IFileManager::Get().Delete(*TempPath, false, true);
		if (InRetriesLeft > 0)
		{
			UE_LOG(LogCloudUpdate, Warning, TEXT("下载失败 %s，剩余重试 %d 次"), *InUrl, InRetriesLeft);
			Service->DownloadFileTo(InUrl, InTargetPath, InRetriesLeft - 1, InCallback, InProgress);
		}
		else if (InCallback)
		{
			InCallback(false);
		}
	});
 if (!Request->ProcessRequest() && !*Completed)
 {
  *Completed = true;
  if (InCallback) InCallback(false);
 }
}
