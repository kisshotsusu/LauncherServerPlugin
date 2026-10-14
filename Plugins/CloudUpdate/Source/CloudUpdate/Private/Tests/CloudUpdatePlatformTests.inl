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
 for (const TCHAR* Platform : {TEXT("Windows"), TEXT("Android"), TEXT("IOS")})
  TestEqual(TEXT("Canonical runtime platform"), CloudUpdatePrivate::ResolveUpdatePlatform(Platform,TEXT("_ASTC")), FString(Platform));
 for (const TCHAR* Flavor : {TEXT("_ASTC"), TEXT("_ETC2Client"), TEXT("_DXT"), TEXT("_Multi"), TEXT("ASTC,ETC2"), TEXT("")})
  TestEqual(TEXT("Cook flavor never changes Android identity"), CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Android"),Flavor), FString(TEXT("Android")));
 TestEqual(TEXT("Unsupported runtime rejected"), CloudUpdatePrivate::ResolveUpdatePlatform(TEXT("Linux"),TEXT("")), FString(TEXT("")));
 TestTrue(TEXT("Legacy metadata stays compatible"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Windows")));
 Json->SetStringField(TEXT("platform"), TEXT("Android"));
 TestFalse(TEXT("Windows rejects Android response"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Windows")));
 TestTrue(TEXT("Android flavor accepted"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Android")));
 TestFalse(TEXT("Legacy flavor is not an API platform"), CloudUpdatePrivate::MatchesResponsePlatform(Json, TEXT("Android_ETC2")));
 Json->RemoveField(TEXT("platform"));
 TestTrue(TEXT("Windows parses"), CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test/patch"),TEXT("1.1"),false,Files,Error));
 TestEqual(TEXT("Windows URL"),Files[0].Url,FString(TEXT("https://test/patch/1.1/Windows/win_P.pak")));
 TestTrue(TEXT("Android parses"), CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Android"),TEXT("https://test/patch"),TEXT("1.1"),false,Files,Error));
 TestEqual(TEXT("Android URL never points to Windows"),Files[0].Url,FString(TEXT("https://test/patch/1.1/Android_ASTC/android_P.pak")));
 TestEqual(TEXT("Hash metadata retained"),Files[0].Hash,FString(TEXT("abc")));
 TestEqual(TEXT("64-bit size retained"),Files[0].FileSize,int64(23));
 Json->GetObjectField(TEXT("pakFilesMap"))->SetObjectField(TEXT("Android_ETC2"), Json->GetObjectField(TEXT("pakFilesMap"))->GetObjectField(TEXT("Android_ASTC")));
 TestFalse(TEXT("Ambiguous Android Cook formats rejected"), CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Android"),TEXT("https://test"),TEXT("1.1"),false,Files,Error));
 TestEqual(TEXT("Ambiguous source leaves no download entries"), Files.Num(), 0);
 Json->GetObjectField(TEXT("pakFilesMap"))->RemoveField(TEXT("Android_ETC2"));
 TestFalse(TEXT("IOS cannot download Android files"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("IOS"),TEXT("https://test"),TEXT("1.1"),false,Files,Error));
 TestTrue(TEXT("IoStore pairs generated"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test"),TEXT("1.1"),true,Files,Error));
 TestEqual(TEXT("Only declared pak and its two companions; no invented global containers"),Files.Num(),3);
 TestTrue(TEXT("All companion URLs use selected platform"),Files[1].Url.Contains(TEXT("/Windows/")) && Files[2].Url.Contains(TEXT("/Windows/")));
 TSharedRef<FJsonObject> BadInfo=MakeShared<FJsonObject>(); BadInfo->SetStringField(TEXT("fileName"),TEXT("../bad.pak"));
 Json->GetObjectField(TEXT("pakFilesMap"))->GetObjectField(TEXT("Windows"))->SetArrayField(TEXT("pakFileInfos"),{MakeShared<FJsonValueObject>(BadInfo)});
 TestFalse(TEXT("Unsafe filename rejected before download"),CloudUpdatePrivate::BuildHotPatcherFiles(Json,TEXT("Windows"),TEXT("https://test"),TEXT("1.1"),false,Files,Error));
 return true;
}
#endif
