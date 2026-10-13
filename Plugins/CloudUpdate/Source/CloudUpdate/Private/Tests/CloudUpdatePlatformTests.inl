#if WITH_DEV_AUTOMATION_TESTS
#include "CloudUpdateUtil.h"
#include "CloudUpdateTypes.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudPlatformFilesTest, "CloudUpdate.Platform.HotPatcherFiles",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudPlatformFilesTest::RunTest(const FString& Parameters)
{
 const FString Input = TEXT(R"({"pakFilesMap":{"Windows":{"pakFileInfos":[{"fileName":"win_P.pak","hash":"123","fileSize":17}]},"Android_ASTC":{"pakFileInfos":[{"fileName":"android_P.pak","hash":"abc","fileSize":23}]}}})");
 TSharedPtr<FJsonObject> Json;
 FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Input), Json);
 TArray<FCloudDownloadFile> Files; FString Error;
 for (const TCHAR* Desktop : {TEXT("Windows"), TEXT("Linux"), TEXT("Mac"), TEXT("IOS")})
  TestEqual(TEXT("Desktop/IOS platform uses runtime identity"), CloudUpdatePrivate::ResolveUpdatePlatform(Desktop,TEXT("_ASTC")), FString(Desktop));
 TestEqual(TEXT("Android ASTC build detected"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("_ASTC")),FString(TEXT("Android_ASTC")));
 TestEqual(TEXT("Android ETC2 client build detected"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("_ETC2Client")),FString(TEXT("Android_ETC2")));
 TestEqual(TEXT("Android DXT build detected"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("_DXT")),FString(TEXT("Android_DXT")));
 TestEqual(TEXT("Multi APK channel differs from individual GPU capability"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("_Multi")),FString(TEXT("Android_Multi")));
 TestEqual(TEXT("Legacy APK metadata with multiple flavors"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("ASTC,ETC2")),FString(TEXT("Android_Multi")));
 TestEqual(TEXT("No invented Android flavor"),CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),TEXT("")),FString(TEXT("Android")));
 TestTrue(TEXT("Legacy metadata stays compatible"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Windows")));
 Json->SetStringField(TEXT("platform"), TEXT("Android_ASTC"));
 TestFalse(TEXT("Windows rejects Android response"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Windows")));
 TestTrue(TEXT("Android flavor accepted"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Android_ASTC")));
 TestFalse(TEXT("Android flavor cannot silently substitute ETC2"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Android_ETC2")));
 Json->RemoveField(TEXT("platform"));
 TestTrue(TEXT("Windows parses"), CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test/patch"),TEXT("1.1"),false,Files,Error));
 TestEqual(TEXT("Windows URL"),Files[0].Url,FString(TEXT("https://test/patch/1.1/Windows/win_P.pak")));
 TestTrue(TEXT("Android parses"), CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Android_ASTC"),TEXT("https://test/patch"),TEXT("1.1"),false,Files,Error));
 TestEqual(TEXT("Android URL never points to Windows"),Files[0].Url,FString(TEXT("https://test/patch/1.1/Android_ASTC/android_P.pak")));
 TestEqual(TEXT("Hash metadata retained"),Files[0].Hash,FString(TEXT("abc")));
 TestEqual(TEXT("64-bit size retained"),Files[0].FileSize,int64(23));
 TestFalse(TEXT("Unavailable flavor does not fall back to another platform"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Android_ETC2"),TEXT("https://test"),TEXT("1.1"),false,Files,Error));
 TestTrue(TEXT("IoStore pairs generated"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test"),TEXT("1.1"),true,Files,Error));
 TestEqual(TEXT("Only declared pak and its two companions; no invented global containers"),Files.Num(),3);
 TestTrue(TEXT("All companion URLs use selected platform"),Files[1].Url.Contains(TEXT("/Windows/")) && Files[2].Url.Contains(TEXT("/Windows/")));
 TSharedRef<FJsonObject> BadInfo=MakeShared<FJsonObject>(); BadInfo->SetStringField(TEXT("fileName"),TEXT("../bad.pak"));
 Json->GetObjectField(TEXT("pakFilesMap"))->GetObjectField(TEXT("Windows"))->SetArrayField(TEXT("pakFileInfos"),{MakeShared<FJsonValueObject>(BadInfo)});
 TestFalse(TEXT("Unsafe filename rejected before download"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test"),TEXT("1.1"),false,Files,Error));
 return true;
}
#endif
