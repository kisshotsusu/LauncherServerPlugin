#include "CloudUpdateDownload.h"
#include "CloudUpdateService.h"
#include "CloudUpdateUtil.h"
#include "Misc/Guid.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudResumeStreamTest, "CloudUpdate.Service.ResumeStream", (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::CommandletContext) | EAutomationTestFlags::EngineFilter)
bool FCloudResumeStreamTest::RunTest(const FString&)
{
 const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/ResumeStream") / FGuid::NewGuid().ToString();
 IFileManager::Get().MakeDirectory(*Root, true);
 const FString Path = Root / TEXT("partial");
 auto Send = [](FCloudDownloadStream& Stream, const ANSICHAR* Data, int64 Size) { Stream.Serialize((void*)Data, Size); };
 {
  FCloudDownloadStream S(Path, TEXT("hash-10"), 10); S.Status(200); S.Header(TEXT("Content-Length"), TEXT("10")); S.Header(TEXT("ETag"), TEXT("\"one\""));
  Send(S,"0123",4); S.Finish(false);
 }
 {
  FCloudDownloadStream S(Path,TEXT("hash-10"),10); TestEqual(TEXT("Interrupted bytes persist across request recreation"),S.Offset,int64(4));
  TestEqual(TEXT("Validator persists"),S.Validator,FString(TEXT("\"one\"")));
  S.Status(503); Send(S,"error",5);S.Finish(false);
  TestEqual(TEXT("HTTP errors never append to partial"),IFileManager::Get().FileSize(*Path),int64(4));
 }
 {
  FCloudDownloadStream S(Path,TEXT("hash-10"),10);S.Status(206);S.Header(TEXT("Content-Range"),TEXT("bytes 5-9/10"));Send(S,"56789",5);S.Finish(false);
  TestTrue(TEXT("Incorrect start is rejected"),S.bInvalidRange);
  TestEqual(TEXT("Invalid range preserves prefix until caller discards"),IFileManager::Get().FileSize(*Path),int64(4));
 }
 {
  FCloudDownloadStream S(Path,TEXT("hash-10"),10);S.Status(206);S.Header(TEXT("Content-Range"),TEXT("bytes 4-9/10"));S.Header(TEXT("Content-Length"),TEXT("6"));Send(S,"456789",6);S.Finish(true);
  int64 Bytes,Total;bool Finished,Success;S.Snapshot(Bytes,Total,Finished,Success);
  TestTrue(TEXT("Valid 206 completes"),Finished&&Success);TestEqual(TEXT("Progress includes prefix"),Bytes,int64(10));TestEqual(TEXT("Total is full size"),Total,int64(10));
 }
 FString Text;FFileHelper::LoadFileToString(Text,*Path);TestEqual(TEXT("Resumed bytes match exactly"),Text,FString(TEXT("0123456789")));
 {
  FCloudDownloadStream S(Path,TEXT("hash-10"),10);S.Status(200);S.Header(TEXT("Content-Length"),TEXT("10"));Send(S,"abcdefghij",10);S.Finish(true);
 }
 FFileHelper::LoadFileToString(Text,*Path);TestEqual(TEXT("Ignored Range replaces instead of appending"),Text,FString(TEXT("abcdefghij")));
 {
  FCloudDownloadStream S(Path,TEXT("changed-hash"),10);TestEqual(TEXT("Changed descriptor invalidates old fragment"),S.Offset,int64(0));
  S.Status(200);S.Header(TEXT("Content-Length"),TEXT("10"));Send(S,"xx",2);S.Finish(false);
 }
 {
  FCloudDownloadStream S(Path,TEXT("changed-hash"),10);S.Status(416);S.Finish(false);TestTrue(TEXT("416 requests fresh restart"),S.bInvalidRange);S.Discard();
 }
 {
  FCloudDownloadStream S(Path,TEXT("empty"),0);S.Status(200);S.Header(TEXT("Content-Length"),TEXT("0"));S.Finish(true);
  int64 Bytes,Total;bool Finished,Success;S.Snapshot(Bytes,Total,Finished,Success);TestTrue(TEXT("Empty downloads complete"),Success);
 }
 IFileManager::Get().DeleteDirectory(*Root,false,true);
 return true;
}

class FCloudResumeHTTPCommand : public IAutomationLatentCommand
{
public:
 FCloudResumeHTTPCommand(FAutomationTestBase* InTest) : Test(InTest)
 {
  Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("Automation/ResumeHTTP")/FGuid::NewGuid().ToString());
  IFileManager::Get().MakeDirectory(*Root,true);
  TArray<uint8> Data;Data.SetNumUninitialized(1048576);for(int32 I=0;I<Data.Num();++I)Data[I]=uint8(I);
  FFileHelper::SaveArrayToFile(Data,*(Root/TEXT("expected.bin")));
  CloudUpdatePrivate::ComputeFileHash(Root/TEXT("expected.bin"),Size,Hash);
  if (!FParse::Value(FCommandLine::Get(),TEXT("CloudResumeTestUrl="),Url))
  {
#if PLATFORM_WINDOWS
   const FString Python=FPaths::ConvertRelativePathToFull(FPaths::EngineDir()/TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
   const FString Script=IPluginManager::Get().FindPlugin(TEXT("CloudUpdate"))->GetBaseDir()/TEXT("Source/CloudUpdate/Private/Tests/Fixtures/resume_download_server.py");
   PortFile=Root/TEXT("port.txt");
   const FString Args=FString::Printf(TEXT("\"%s\" \"%s\""),*Script,*PortFile);
   Server=FPlatformProcess::CreateProc(*Python,*Args,true,true,true,nullptr,0,nullptr,nullptr);
#else
   Test->AddError(TEXT("CloudResumeTestUrl required on device"));bUnavailable=true;
#endif
  }
  Service=MakeShared<FCloudUpdateService>(nullptr);Started=FPlatformTime::Seconds();
 }
 ~FCloudResumeHTTPCommand()
 {
  Service->Abort();if(Server.IsValid()){FPlatformProcess::TerminateProc(Server,true);FPlatformProcess::CloseProc(Server);}
  IFileManager::Get().DeleteDirectory(*Root,false,true);
 }
 bool Update() override
 {
  if(bUnavailable)return true;
  if(FPlatformTime::Seconds()-Started>90){Test->AddError(TEXT("Resume HTTP test timed out"));return true;}
  if(Url.IsEmpty()) {FString Port;if(!FFileHelper::LoadFileToString(Port,*PortFile))return false;Url=TEXT("http://127.0.0.1:")+Port.TrimStartAndEnd();}
  if(!bStarted){bStarted=true;Start();return false;}
  if(!bDone)return false;
  if(Stage==5&&!bRestarted)
  {
   Test->TestFalse(TEXT("Exhausted retries report failure"),Success);
   Test->TestEqual(TEXT("Exhaustion preserves prefix on disk"),IFileManager::Get().FileSize(*(Path+TEXT(".download"))),int64(131072));
   Service=MakeShared<FCloudUpdateService>(nullptr);bRestarted=true;Start();return false;
  }
  Test->TestTrue(*FString::Printf(TEXT("HTTP case %d succeeds"),Stage),Success);
  int64 ActualSize;FString ActualHash;CloudUpdatePrivate::ComputeFileHash(Path,ActualSize,ActualHash);
  Test->TestEqual(TEXT("No missing/duplicated bytes"),ActualHash,Stage==6?FString(TEXT("d41d8cd98f00b204e9800998ecf8427e")):Hash);
  if(Stage<6)Test->TestTrue(TEXT("Intermediate progress ticks continue during transfer"),Intermediate>=3);
  if(Stage<6)Test->TestTrue(TEXT("Resume progress includes prefix"),SawPrefix);
  Test->TestFalse(TEXT("Metadata removed on successful install"),IFileManager::Get().FileExists(*(Path+TEXT(".download.json"))));
  ++Stage;if(Stage==8)return true;Start();return false;
 }
 void Start()
 {
  static const TCHAR* Routes[]={TEXT("drop"),TEXT("ignore"),TEXT("bad-range"),TEXT("error"),TEXT("change"),TEXT("restart"),TEXT("empty"),TEXT("cached")};
  Path=Root/FString::Printf(TEXT("case%d.bin"),Stage);bDone=false;Intermediate=0;SawPrefix=false;
  if(Stage==7)
  {
   TArray<uint8> Data;FFileHelper::LoadFileToArray(Data,*(Root/TEXT("expected.bin")));
   FCloudDownloadStream Cached(Path+TEXT(".download"),FString::Printf(TEXT("%s\n%lld"),*Hash.ToLower(),Size),Size);
   Cached.Status(200);Cached.Header(TEXT("Content-Length"),FString::Printf(TEXT("%lld"),Size));Cached.Serialize(Data.GetData(),Data.Num());Cached.Finish(false);
  }
  FCloudUpdateMetricsTestAccess::DownloadResume(*Service,Url/TEXT("resume")/Routes[Stage],Path,Stage==5&&!bRestarted?0:3,Stage==6?FString(TEXT("d41d8cd98f00b204e9800998ecf8427e")):Hash,Stage==6?0:Size,
   [this](bool Ok){Success=Ok;bDone=true;},[this](int64 Done,int64 Total){if(Done>0&&Done<Total){++Intermediate;if(Done>=131072)SawPrefix=true;}});
 }
 FAutomationTestBase* Test;TSharedPtr<FCloudUpdateService> Service;FString Root,Path,Hash,Url,PortFile;int64 Size=0;FProcHandle Server;
 int32 Stage=0,Intermediate=0;bool bStarted=false,bDone=false,Success=false,bUnavailable=false,bRestarted=false,SawPrefix=false;double Started=0;
};
// 跨进程"再次进入"：上一轮已下完并落地的文件必须按 hash 直接确认完成，不再重下。
class FCloudInstalledReuseCommand : public IAutomationLatentCommand
{
public:
 FCloudInstalledReuseCommand(FAutomationTestBase* InTest) : Test(InTest)
 {
  Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Automation/InstalledReuse") / FGuid::NewGuid().ToString());
  IFileManager::Get().MakeDirectory(*Root, true);
  Path = Root / TEXT("already-installed.bin");
  TArray<uint8> Data; Data.SetNumUninitialized(4096);
  for (int32 Index = 0; Index < Data.Num(); ++Index) Data[Index] = static_cast<uint8>(Index * 7);
  FFileHelper::SaveArrayToFile(Data, *Path);
  CloudUpdatePrivate::ComputeFileHash(Path, Size, Hash);
  Service = MakeShared<FCloudUpdateService>(nullptr);
  // 故意给一个不可达地址：只有"已落地文件快速确认"生效时才会立即成功。
  FCloudUpdateMetricsTestAccess::DownloadResume(*Service, TEXT("http://127.0.0.1:9/never-exists"), Path, 0, Hash, Size,
   [this](bool bOk) { Success = bOk; bDone = true; }, [](int64, int64) {});
 }
 ~FCloudInstalledReuseCommand()
 {
  Service->Abort();
  IFileManager::Get().DeleteDirectory(*Root, false, true);
 }
 bool Update() override
 {
  if (!bDone) return false;
  Test->TestTrue(TEXT("Already installed file is confirmed without re-download"), Success);
  int64 ActualSize = 0; FString ActualHash;
  Test->TestTrue(TEXT("Existing installed content untouched"),
   CloudUpdatePrivate::ComputeFileHash(Path, ActualSize, ActualHash) && ActualHash == Hash && ActualSize == Size);
  return true;
 }
 FAutomationTestBase* Test; TSharedPtr<FCloudUpdateService> Service; FString Root, Path, Hash; int64 Size = 0;
 bool bDone = false, Success = false;
};
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudInstalledReuseTest, "CloudUpdate.Service.InstalledFileReuse",
 (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::CommandletContext) | EAutomationTestFlags::EngineFilter)
bool FCloudInstalledReuseTest::RunTest(const FString&)
{
 ADD_LATENT_AUTOMATION_COMMAND(FCloudInstalledReuseCommand(this)); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudResumeHTTPTest,"CloudUpdate.Service.ResumeHTTP",(EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::CommandletContext)|EAutomationTestFlags::EngineFilter)
bool FCloudResumeHTTPTest::RunTest(const FString&)
{
 ADD_LATENT_AUTOMATION_COMMAND(FCloudResumeHTTPCommand(this));return true;
}
