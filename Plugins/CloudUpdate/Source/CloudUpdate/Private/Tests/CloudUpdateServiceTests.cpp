#if WITH_DEV_AUTOMATION_TESTS
#include "CloudUpdateService.h"
#include "Misc/AutomationTest.h"
#include "Dom/JsonObject.h"

struct FCloudUpdateServiceTestAccess
{
 static bool TestCancelledUpdate()
 {
  TSharedRef<FCloudUpdateService> Service = MakeShared<FCloudUpdateService>(nullptr);
  const FString Before = Service->GetLocalVersion();
  Service->PendingVersionId = TEXT("CANCELLED_TEST_MUST_NOT_BE_SAVED");
  Service->SetBusy(true);
  Service->Abort();
  Service->DownloadNextUpdateFile();
  return !Service->IsBusy() && Service->GetLocalVersion() == Before;
 }
 static bool TestBusyLatestRequest()
 {
  TSharedRef<FCloudUpdateService> Service = MakeShared<FCloudUpdateService>(nullptr);
  Service->SetBusy(true);
  Service->ApplyLatestUpdate();
  return Service->IsBusy() && !Service->bResolvingLatestUpdate;
 }
 static bool TestEmptySizeQuery()
 {
  TSharedRef<FCloudUpdateService> Service = MakeShared<FCloudUpdateService>(nullptr);
  Service->SetBusy(true);
  Service->bQueryingUpdateSize = true;
  Service->ParseVersionsIndex(MakeShared<FJsonObject>());
  return !Service->IsBusy() && !Service->bQueryingUpdateSize;
 }
 static bool TestCancelledLatestQueue()
 {
  TSharedRef<FCloudUpdateService> Service = MakeShared<FCloudUpdateService>(nullptr);
  Service->SetBusy(true);
  Service->bResolvingLatestUpdate = true;
  Service->Abort();
  Service->ParseVersionsIndex(MakeShared<FJsonObject>());
  return !Service->IsBusy() && !Service->bResolvingLatestUpdate && Service->LatestUpdateQueue.IsEmpty();
 }
};

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudUpdateServiceRegressionTest, "CloudUpdate.Service.TaskLifecycle",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudUpdateServiceRegressionTest::RunTest(const FString& Parameters)
{
 TestTrue(TEXT("Cancellation never advances local version"), FCloudUpdateServiceTestAccess::TestCancelledUpdate());
 TestTrue(TEXT("Busy task rejects latest update without changing task state"), FCloudUpdateServiceTestAccess::TestBusyLatestRequest());
 TestTrue(TEXT("Empty size query clears busy and query state"), FCloudUpdateServiceTestAccess::TestEmptySizeQuery());
 TestTrue(TEXT("Cancelled latest resolution never starts another update"), FCloudUpdateServiceTestAccess::TestCancelledLatestQueue());
 return true;
}
#endif
