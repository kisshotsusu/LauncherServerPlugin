#include "CloudUpdateDownload.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
int64 PreparePartial(const FString& Path, const FString& Identity, int64 Size)
{
 FString Saved;
 TSharedPtr<FJsonObject> Meta;
 FString StoredIdentity;
 const bool Matches = FFileHelper::LoadFileToString(Saved, *(Path + TEXT(".json")))
  && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved), Meta) && Meta.IsValid()
  && Meta->TryGetStringField(TEXT("identity"), StoredIdentity) && StoredIdentity == Identity;
 const int64 Bytes = IFileManager::Get().FileSize(*Path);
 if (!Matches || Bytes < 0 || (Size >= 0 && Bytes > Size))
 {
  IFileManager::Get().Delete(*Path, false, true);
  IFileManager::Get().Delete(*(Path + TEXT(".json")), false, true);
  return 0;
 }
 return Bytes;
}
}
FCloudDownloadStream::FCloudDownloadStream(const FString& InPath, const FString& InIdentity, int64 InSize)
 : Offset(PreparePartial(InPath, InIdentity, InSize)), Path(InPath), Identity(InIdentity), ExpectedSize(InSize), TotalSize(InSize)
{
 FString Saved; TSharedPtr<FJsonObject> Meta;
 if (FFileHelper::LoadFileToString(Saved, *(Path + TEXT(".json")))
  && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Saved), Meta) && Meta.IsValid())
 {
  Meta->TryGetStringField(TEXT("validator"), Validator);
  int64 StoredTotal = -1;
  if (Meta->TryGetNumberField(TEXT("total"), StoredTotal) && StoredTotal > 0) KnownTotal = StoredTotal;
 }
 if (KnownTotal <= 0) KnownTotal = InSize; // InSize<0 表示仍然未知
}
void FCloudDownloadStream::Status(int32 Code)
{
 FScopeLock Lock(&Mutex);
 if (Code == 416) bInvalidRange = true;
 ResponseCode = Code; ContentRange.Empty(); Encoding.Empty(); NewValidator.Empty(); ResponseLength = -1;
}
void FCloudDownloadStream::Header(const FString& Name, const FString& Value)
{
 FScopeLock Lock(&Mutex);
 if (Name.Equals(TEXT("Content-Range"), ESearchCase::IgnoreCase)) ContentRange = Value;
 else if (Name.Equals(TEXT("Content-Length"), ESearchCase::IgnoreCase)) LexTryParseString(ResponseLength, *Value);
 else if (Name.Equals(TEXT("Content-Encoding"), ESearchCase::IgnoreCase)) Encoding = Value;
 else if (Name.Equals(TEXT("ETag"), ESearchCase::IgnoreCase) && !Value.StartsWith(TEXT("W/"))) NewValidator = Value;
 else if (Name.Equals(TEXT("Last-Modified"), ESearchCase::IgnoreCase) && NewValidator.IsEmpty()) NewValidator = Value;
}
bool FCloudDownloadStream::OpenResponse()
{
 int64 Start = 0, End = -1, Total = -1;
 if (ResponseCode == 206)
 {
  FString Range, TotalText, StartText, EndText;
  if (!ContentRange.StartsWith(TEXT("bytes ")) || !ContentRange.Mid(6).Split(TEXT("/"), &Range, &TotalText)
   || !Range.Split(TEXT("-"), &StartText, &EndText)
   || !LexTryParseString(Start, *StartText) || !LexTryParseString(End, *EndText)
   || Start != Offset || End < Start
   || (ResponseLength >= 0 && ResponseLength != End - Start + 1))
  { bInvalidRange = true; return false; }
  // "bytes 0-99/*"（总长未知）是合法响应：不能当成非法区间，否则会丢掉一份好分片并整文件重下。
  const bool bTotalKnown = LexTryParseString(Total, *TotalText);
  if (bTotalKnown && (Total <= End || (ExpectedSize >= 0 && Total != ExpectedSize)))
  { bInvalidRange = true; return false; }
  if (!bTotalKnown) Total = -1;
  if (!Validator.IsEmpty() && !NewValidator.IsEmpty() && Validator != NewValidator)
  { bInvalidRange = true; return false; }
  ResponseEnd = End + 1;
 }
 else if (ResponseCode == 200)
 {
  // An origin/proxy may ignore Range or reject If-Range: replace, never append a full response.
  Start = 0; Total = ResponseLength;
  if (ExpectedSize >= 0 && Total >= 0 && ExpectedSize != Total) return false;
  ResponseEnd = Total;
 }
 else return false; // Never save an HTML/JSON error body as a resumable fragment.
 if (!Encoding.IsEmpty() && !Encoding.Equals(TEXT("identity"), ESearchCase::IgnoreCase)) return false;
 TotalSize = ExpectedSize >= 0 ? ExpectedSize : Total;
 if (TotalSize > 0) KnownTotal = TotalSize;
 Writer.Reset(IFileManager::Get().CreateFileWriter(*Path, Start > 0 ? FILEWRITE_Append : 0));
 if (!Writer) return false;
 BytesWritten = Start;
 TSharedRef<FJsonObject> Meta = MakeShared<FJsonObject>();
 Meta->SetStringField(TEXT("identity"), Identity);
 // 206 未回 ETag/Last-Modified 时不能把已存的验证标记清空，否则下次续传没有 If-Range，
 // 服务器换了内容也会被追加到旧分片后面。
 Meta->SetStringField(TEXT("validator"), NewValidator.IsEmpty() ? Validator : NewValidator);
 if (TotalSize > 0) Meta->SetNumberField(TEXT("total"), TotalSize);
 FString Saved; FJsonSerializer::Serialize(Meta, TJsonWriterFactory<>::Create(&Saved));
 if (!FFileHelper::SaveStringToFile(Saved, *(Path + TEXT(".json")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) return false;
 bOpened = true;
 return true;
}
void FCloudDownloadStream::Serialize(void* Data, int64 Length)
{
 FScopeLock Lock(&Mutex);
 if (Length < 0 || (!bOpened && !OpenResponse()) || (ResponseEnd >= 0 && Length > ResponseEnd - BytesWritten))
 { SetError(); return; }
 Writer->Serialize(Data, Length);
 if (Writer->IsError()) SetError();
 else BytesWritten += Length;
}
bool FCloudDownloadStream::Close()
{
 FScopeLock Lock(&Mutex);
 if (Writer) { if (!Writer->Close() || Writer->IsError()) SetError(); Writer.Reset(); }
 return !IsError();
}
void FCloudDownloadStream::Finish(bool bConnected)
{
 Close();
 FScopeLock Lock(&Mutex);
 // Empty bodies do not call Serialize, but still require a real empty file and metadata.
 if (!bOpened && bConnected && ResponseCode == 200 && ResponseLength == 0)
 { if (!OpenResponse()) SetError(); if (Writer) { Writer->Close(); Writer.Reset(); } }
 bSuccess = bConnected && !IsError() && bOpened
  && (ResponseEnd < 0 || BytesWritten == ResponseEnd) && (TotalSize < 0 || BytesWritten == TotalSize);
 bFinished = true;
}
void FCloudDownloadStream::Snapshot(int64& Bytes, int64& Total, bool& Finished, bool& Success)
{
 FScopeLock Lock(&Mutex);
 Bytes = bOpened ? BytesWritten : Offset; Total = TotalSize; Finished = bFinished; Success = bSuccess;
}
void FCloudDownloadStream::Discard()
{
 Close();
 IFileManager::Get().Delete(*Path, false, true);
 IFileManager::Get().Delete(*(Path + TEXT(".json")), false, true);
}
