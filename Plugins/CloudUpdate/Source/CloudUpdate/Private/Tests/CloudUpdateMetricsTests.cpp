#if WITH_DEV_AUTOMATION_TESTS
#include "CloudUpdateService.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdateUtil.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Guid.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Features/IModularFeatures.h"
#include "BinariesPatchFeature.h"
#include "Interfaces/IPluginManager.h"

struct FCloudUpdateMetricsTestAccess
{
 static void UseVersionFile(FCloudUpdateService& Service, const FString& Path) { Service.LocalVersionRecordPath = Path; }
 static FCloudMergeProgressInfo Merge(const FCloudUpdateService& Service) { return Service.MergeProgress; }
 static bool TestBytes()
 {
  FCloudUpdateService Service(nullptr);
  FCloudUpdateService::FPlannedVersion Version;
  Version.Info.VersionId = TEXT("1.1");
  FCloudDownloadFile Small; Small.FileSize = 10; Small.bSizeKnown = true; Small.TargetRelativePath = TEXT("small");
  FCloudDownloadFile Large; Large.FileSize = 90; Large.bSizeKnown = true; Large.TargetRelativePath = TEXT("large");
  Version.Files = {Small, Large};
  Service.InitializeTransferSession({Version});
  Service.ActiveTransfer = 0;
  Service.UpdateTransferProgress(Service.TransferGeneration, 10, 10);
  Service.CompleteTransfer(Service.TransferGeneration, true);
  Service.ActiveTransfer = 1;
  Service.UpdateTransferProgress(Service.TransferGeneration, 45, 90);
  const auto P = Service.GetDownloadProgress();
  const bool bWeighted = P.CompletedFiles == 1 && P.TotalFiles == 2 && P.DownloadedBytes == 55 && P.TotalBytes == 100 && FMath::IsNearlyEqual(P.FilePercent, 50.0f) && FMath::IsNearlyEqual(P.BytePercent, 55.0f);
  Service.UpdateTransferProgress(Service.TransferGeneration - 1, 90, 90);
  return bWeighted && Service.GetDownloadProgress().DownloadedBytes == 55;
 }
 static bool TestUnknownAndZero()
 {
  FCloudUpdateService Service(nullptr);
  FCloudUpdateService::FPlannedVersion Version;
  FCloudDownloadFile File; File.TargetRelativePath = TEXT("zero");
  Version.Files.Add(File); Service.InitializeTransferSession({Version});
  const bool bUnknown = !Service.GetDownloadProgress().bTotalBytesKnown && Service.GetDownloadProgress().BytePercent == -1.0f;
  Service.ActiveTransfer = 0;
  Service.UpdateTransferProgress(Service.TransferGeneration, 0, 0);
  Service.CompleteTransfer(Service.TransferGeneration, true);
  const auto P = Service.GetDownloadProgress();
  return bUnknown && P.bTotalBytesKnown && P.TotalBytes == 0 && P.BytePercent == 100.0f && P.FilePercent == 100.0f;
 }
 static bool TestDescriptor()
 {
  FCloudUpdateService Service(nullptr);
  auto Json = MakeShared<FJsonObject>();
  auto Entry = MakeShared<FJsonObject>();
  Entry->SetStringField(TEXT("fileName"), TEXT("safe.bin"));
  Entry->SetStringField(TEXT("url"), TEXT("/files/safe.bin"));
  Entry->SetNumberField(TEXT("size"), 0);
  TArray<TSharedPtr<FJsonValue>> Values = {MakeShared<FJsonValueObject>(Entry)};
  Json->SetArrayField(TEXT("files"), Values);
  TArray<FCloudDownloadFile> Files;
  const bool bValidZero = Service.ReadDescriptorFiles(Json, Files) && Files.Num() == 1 && Files[0].bSizeKnown && Files[0].FileSize == 0;
  Values.Add(MakeShared<FJsonValueObject>(Entry)); Json->SetArrayField(TEXT("files"), Values);
  return bValidZero && !Service.ReadDescriptorFiles(Json, Files);
 }
 static bool TestPendingActivation(const FString& Root)
 {
  FCloudUpdateService Service(nullptr);
  Service.LocalVersionRecordPath = Root / TEXT("versions.json");
  Service.SaveLocalVersion(TEXT("1.0"));
  const FString DataPath = Root / TEXT("resource.bin");
  FFileHelper::SaveStringToFile(TEXT("new"), *DataPath);
  FCloudDownloadFile File; File.Kind = ECloudDownloadKind::ExternFile; File.TargetRelativePath = DataPath;
  // Generate the same local record shape without changing the global install root.
  auto Json = MakeShared<FJsonObject>();
  Json->SetStringField(TEXT("resourceVersionId"), TEXT("1.0"));
  Json->SetStringField(TEXT("installedGameVersion"), Service.GetLocalGameVersion());
  Json->SetStringField(TEXT("pendingResourceVersionId"), TEXT("1.1"));
  auto Entry = MakeShared<FJsonObject>(); Entry->SetStringField(TEXT("path"), DataPath);
  Json->SetArrayField(TEXT("pendingFiles"), {MakeShared<FJsonValueObject>(Entry)});
  FString Text; FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
  FFileHelper::SaveStringToFile(Text, *Service.LocalVersionRecordPath);
  FFileHelper::SaveStringToFile(TEXT("staged"), *(DataPath + TEXT(".pending")));
  Service.PromotePendingResourceVersion();
  const bool bStaged = Service.GetLocalVersion() == TEXT("1.0") && Service.GetPendingResourceVersion() == TEXT("1.1");
  IFileManager::Get().Delete(*(DataPath + TEXT(".pending")));
  Service.PromotePendingResourceVersion();
  return bStaged && Service.GetLocalVersion() == TEXT("1.1") && Service.GetPendingResourceVersion().IsEmpty();
 }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudUpdateMetricsTest, "CloudUpdate.Service.VersionAndProgress", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudUpdateMetricsTest::RunTest(const FString& Parameters)
{
 TestTrue(TEXT("Native binary increments are excluded from resource patches"), CloudUpdatePrivate::IsGameBinaryPath(TEXT("Game.exe.patch")) && CloudUpdatePrivate::IsGameBinaryPath(TEXT("Module.dll")) && !CloudUpdatePrivate::IsGameBinaryPath(TEXT("Game.pak.patch")));
 TestTrue(TEXT("File count and byte percent have independent denominators; stale callback ignored"), FCloudUpdateMetricsTestAccess::TestBytes());
 TestTrue(TEXT("Unknown byte total distinguished from a completed zero byte file"), FCloudUpdateMetricsTestAccess::TestUnknownAndZero());
 TestTrue(TEXT("File list accepts known zero size and rejects duplicate targets"), FCloudUpdateMetricsTestAccess::TestDescriptor());
 const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/CloudUpdate") / FGuid::NewGuid().ToString());
 IFileManager::Get().MakeDirectory(*Root, true);
 TestTrue(TEXT("Pending resource release remains inactive while .pending exists"), FCloudUpdateMetricsTestAccess::TestPendingActivation(Root));
 IFileManager::Get().DeleteDirectory(*Root, false, true);
 return true;
}

struct FCloudUpdateMockPatchFeature : IBinariesDiffPatchFeature
{
 virtual FString GetFeatureName() const override { return TEXT("CloudUpdateTest"); }
 virtual bool CreateDiff(const TArray<uint8>& NewData, const TArray<uint8>& OldData, TArray<uint8>& OutPatch) override { OutPatch = NewData; return true; }
 virtual bool PatchDiff(const TArray<uint8>& OldData, const TArray<uint8>& PatchData, TArray<uint8>& OutNewData) override
 {
  FPlatformProcess::Sleep(0.05f);
  if (PatchData.Num() >= 4 && PatchData[0] == 'F' && PatchData[1] == 'A') return false;
  OutNewData = PatchData; return true;
 }
};
static FCloudUpdateMockPatchFeature CloudUpdateMockFeature;

class FCloudUpdateHTTPTestCommand : public IAutomationLatentCommand
{
public:
 explicit FCloudUpdateHTTPTestCommand(FAutomationTestBase* InTest) : Test(InTest)
 {
  Settings = GetMutableDefault<UCloudUpdateSettings>();
  OldServer = Settings->ServerUrl; OldGame = Settings->GameVersionOverride; OldRoot = Settings->LocalRootOverride; OldFeature = Settings->BinaryPatchFeatureName; OldMerge = Settings->bEnableBinaryMerge;
  Settings->GameVersionOverride = TEXT("1.0"); Settings->BinaryPatchFeatureName = TEXT("CloudUpdateTest"); Settings->bEnableBinaryMerge = true;
  Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/CloudUpdateHTTP") / FGuid::NewGuid().ToString());
  Settings->LocalRootOverride = Root; IFileManager::Get().MakeDirectory(*Root, true);
  FFileHelper::SaveStringToFile(TEXT("base"), *(Root / TEXT("base.bin")));
  FFileHelper::SaveStringToFile(TEXT("base"), *(Root / TEXT("fallback.bin")));
  IModularFeatures::Get().RegisterModularFeature(BINARIES_DIFF_PATCH_FEATURE_NAME, &CloudUpdateMockFeature);
  Service = MakeShared<FCloudUpdateService>(nullptr);
  FCloudUpdateMetricsTestAccess::UseVersionFile(*Service, Root / TEXT("version.json"));
  Service->SetLocalVersion(TEXT("0.0"));
  const FString Python = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
  const FString Script = IPluginManager::Get().FindPlugin(TEXT("CloudUpdate"))->GetBaseDir() / TEXT("Source/CloudUpdate/Private/Tests/Fixtures/mock_update_server.py");
  PortFile = Root / TEXT("port.txt");
  const FString Args = FString::Printf(TEXT("\"%s\" \"%s\""), *Script, *PortFile);
  ServerProcess = FPlatformProcess::CreateProc(*Python, *Args, true, true, true, nullptr, 0, nullptr, nullptr);
  Started = FPlatformTime::Seconds();
 }
 virtual ~FCloudUpdateHTTPTestCommand()
 {
  Service->Abort();
  if (ServerProcess.IsValid()) { FPlatformProcess::TerminateProc(ServerProcess, true); FPlatformProcess::CloseProc(ServerProcess); }
  Settings->ServerUrl = OldServer; Settings->GameVersionOverride = OldGame; Settings->LocalRootOverride = OldRoot; Settings->BinaryPatchFeatureName = OldFeature; Settings->bEnableBinaryMerge = OldMerge;
  IModularFeatures::Get().UnregisterModularFeature(BINARIES_DIFF_PATCH_FEATURE_NAME, &CloudUpdateMockFeature);
  IFileManager::Get().DeleteDirectory(*Root, false, true);
 }
 virtual bool Update() override
 {
  if (FPlatformTime::Seconds() - Started > 45.0) { Test->AddError(TEXT("Mock HTTP update timed out")); return true; }
  if (Stage == -1)
  {
   FString Port;
   if (!ServerProcess.IsValid()) { Test->AddError(TEXT("Unable to launch bundled Python HTTP fixture")); return true; }
   if (!FFileHelper::LoadFileToString(Port, *PortFile)) return false;
   Settings->ServerUrl = TEXT("http://127.0.0.1:") + Port.TrimStartAndEnd();
   Service->QueryUpdatePlan(); Stage = 0; return false;
  }
  if (Stage == 0)
  {
   if (Service->IsBusy()) return false;
   const auto Plan = Service->GetUpdatePlan();
   Test->TestTrue(TEXT("HTTP plan resolved"), Plan.bValid);
   if (!Plan.bValid) return true;
   Test->TestEqual(TEXT("Server game version"), Plan.ServerGameVersion, FString(TEXT("2.0")));
   Test->TestEqual(TEXT("Server resource version"), Plan.ServerResourceVersion, FString(TEXT("1.3")));
   Test->TestEqual(TEXT("Four compatible resource files across two versions"), Plan.RequiredFileCount, 4);
   Test->TestEqual(TEXT("Resource descriptor byte sum"), Plan.RequiredBytes, static_cast<int64>(49192));
   Test->TestEqual(TEXT("Game package count kept separate"), Plan.GamePackageFileCount, 2);
   Test->TestTrue(TEXT("Incompatible C++ release flagged"), Plan.bResourceUpdateRequiresGame);
   Stage = 1; Service->ApplyLatestUpdate(); return false;
  }
  const auto Progress = Service->GetDownloadProgress();
  bSawIntermediate |= Progress.DownloadedBytes > 0 && Progress.BytePercent > 0.0f && Progress.BytePercent < 100.0f;
  if (Service->IsBusy()) return false;
  Test->TestTrue(TEXT("Byte progress visible before completion"), bSawIntermediate);
  Test->TestEqual(TEXT("Game build unchanged by resource update"), Service->GetLocalGameVersion(), FString(TEXT("1.0")));
  Test->TestEqual(TEXT("Local resource version advanced independently"), Service->GetLocalVersion(), FString(TEXT("1.2")));
  Test->TestEqual(TEXT("Completed logical files"), Progress.CompletedFiles, 4);
  Test->TestEqual(TEXT("Fallback changes byte budget without adding logical file"), Progress.TotalBytes, static_cast<int64>(49312));
  Test->TestEqual(TEXT("Completed byte count"), Progress.DownloadedBytes, Progress.TotalBytes);
  Test->TestEqual(TEXT("File percent"), Progress.FilePercent, 100.0f);
  Test->TestEqual(TEXT("Byte percent"), Progress.BytePercent, 100.0f);
  const auto Merge = FCloudUpdateMetricsTestAccess::Merge(*Service);
  Test->TestEqual(TEXT("Two actual merge attempts"), Merge.CompletedFiles, 2);
  Test->TestEqual(TEXT("Successful merge"), Merge.SucceededFiles, 1);
  Test->TestEqual(TEXT("Failed merge with successful download fallback"), Merge.FailedFiles, 1);
  Test->TestEqual(TEXT("Merge percent"), Merge.Percent, 100.0f);
  Test->TestEqual(TEXT("Fallback output installed"), IFileManager::Get().FileSize(*(Root / TEXT("fallback.bin"))), static_cast<int64>(128));
  Test->TestFalse(TEXT("Game binary was not downloaded into running game"), IFileManager::Get().FileExists(*(Root / TEXT("game.exe"))));
  return true;
 }
private:
 FAutomationTestBase* Test;
 UCloudUpdateSettings* Settings;
 TSharedPtr<FCloudUpdateService> Service;
 FString OldServer, OldGame, OldRoot, OldFeature, Root, PortFile;
 FProcHandle ServerProcess;
 bool OldMerge = false, bSawIntermediate = false;
 int32 Stage = -1;
 double Started = 0;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudUpdateHTTPTest, "CloudUpdate.Service.MockHTTPIntegration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudUpdateHTTPTest::RunTest(const FString& Parameters)
{
 AddExpectedMessage(TEXT("PatchDiffToFile 返回 false"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
 ADD_LATENT_AUTOMATION_COMMAND(FCloudUpdateHTTPTestCommand(this));
 return true;
}
#endif
