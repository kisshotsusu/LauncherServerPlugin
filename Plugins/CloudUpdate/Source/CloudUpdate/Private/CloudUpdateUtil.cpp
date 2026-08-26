// ======================================================================
// CloudUpdate 内部工具函数实现
// ======================================================================
#include "CloudUpdateUtil.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/SecureHash.h"
#include "Misc/Paths.h"

namespace CloudUpdatePrivate
{
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
