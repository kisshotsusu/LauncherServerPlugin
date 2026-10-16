#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"

// HTTP callbacks use this state exclusively; service/UObject callbacks stay on the game thread.
class FCloudDownloadStream : public FArchive
{
public:
 FCloudDownloadStream(const FString& InPath, const FString& InIdentity, int64 InSize);
 virtual ~FCloudDownloadStream() override { Close(); }
 virtual void Serialize(void* Data, int64 Length) override;
 virtual bool Close() override;
 void Status(int32 Code);
 void Header(const FString& Name, const FString& Value);
 void Finish(bool bConnected);
 void Snapshot(int64& Bytes, int64& Total, bool& bFinished, bool& bSuccess);
 void Discard();
 const int64 Offset;
 FString Validator;
 bool bInvalidRange = false;
private:
 bool OpenResponse();
 FString Path, Identity, ContentRange, Encoding, NewValidator;
 TUniquePtr<FArchive> Writer;
 FCriticalSection Mutex;
 int64 ExpectedSize, BytesWritten = 0, ResponseLength = -1, TotalSize = -1, ResponseEnd = -1;
 int32 ResponseCode = 0;
 bool bOpened = false, bFinished = false, bSuccess = false;
};
