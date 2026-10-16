// 局域网回退：URL 分解 / 回退决策的纯逻辑单测（不做任何网络访问）
#if WITH_DEV_AUTOMATION_TESTS
#include "CloudUpdateUtil.h"
#include "Misc/AutomationTest.h"

using namespace CloudUpdatePrivate;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudLanFallbackTest, "CloudUpdate.LanFallback.Decision",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudLanFallbackTest::RunTest(const FString& Parameters)
{
 // ---------- URL 分解 ----------
 {
  const FCloudUrlParts Parts = ParseCloudUrl(TEXT("http://192.168.1.10:8710/files/a.pak?x=1"));
  TestTrue(TEXT("scheme parsed"), Parts.Scheme == TEXT("http"));
  TestTrue(TEXT("host parsed"), Parts.Host == TEXT("192.168.1.10"));
  TestEqual(TEXT("port parsed"), Parts.Port, 8710);
  TestTrue(TEXT("path+query preserved"), Parts.Path == TEXT("/files/a.pak?x=1"));
  TestTrue(TEXT("valid"), Parts.bValid);
 }
 {
  const FCloudUrlParts Parts = ParseCloudUrl(TEXT("https://example.com"));
  TestTrue(TEXT("default port is 0"), Parts.Port == 0);
  TestTrue(TEXT("empty path"), Parts.Path.IsEmpty());
  TestTrue(TEXT("https valid"), Parts.bValid);
 }
 TestFalse(TEXT("non-http rejected"), ParseCloudUrl(TEXT("ftp://example.com/x")).bValid);
 TestFalse(TEXT("missing host rejected"), ParseCloudUrl(TEXT("http:///x")).bValid);

 // ---------- 回填 ----------
 {
  FCloudUrlParts Parts;
  Parts.Scheme = TEXT("http");
  Parts.Host = TEXT("10.0.0.5");
  Parts.Port = 80;
  Parts.Path = TEXT("/api/manifest.json");
  Parts.bValid = true;
  TestTrue(TEXT("default http port omitted"), ComposeCloudUrl(Parts) == TEXT("http://10.0.0.5/api/manifest.json"));
  Parts.Port = 8710;
  TestTrue(TEXT("custom port kept"), ComposeCloudUrl(Parts) == TEXT("http://10.0.0.5:8710/api/manifest.json"));
 }

 // ---------- IPv4 字面量 ----------
 TestTrue(TEXT("127.0.0.1 is IPv4"), IsIPv4Literal(TEXT("127.0.0.1")));
 TestTrue(TEXT("192.168.1.255 is IPv4"), IsIPv4Literal(TEXT("192.168.1.255")));
 TestFalse(TEXT("256 out of range"), IsIPv4Literal(TEXT("256.1.1.1")));
 TestFalse(TEXT("three segments"), IsIPv4Literal(TEXT("1.2.3")));
 TestFalse(TEXT("IPv6 is not IPv4"), IsIPv4Literal(TEXT("::1")));
 TestFalse(TEXT("hostname is not IPv4"), IsIPv4Literal(TEXT("localhost")));

 // ---------- 主机解析的字面量快路径（不触发 DNS） ----------
 {
  TArray<FString> Ips;
  TestTrue(TEXT("literal host resolves without DNS"), ResolveHostIPv4(TEXT("127.0.0.1"), Ips));
  TestTrue(TEXT("literal returns itself"), Ips.Contains(TEXT("127.0.0.1")));
  TestFalse(TEXT("empty host rejected"), ResolveHostIPv4(TEXT(""), Ips));
 }

 const TArray<FString> LocalIps = { TEXT("127.0.0.1"), TEXT("192.168.1.50") };

 // ---------- 命中：解析出的 IP 就是本机 ----------
 {
  FString OutUrl, Reason;
  const bool bApplied = DecideLanFallback(
   TEXT("http://127.0.0.1:8710/files/a.pak?x=1"),
   TEXT("http://192.168.1.10:8710"),
   { TEXT("127.0.0.1") }, LocalIps, OutUrl, Reason);
  TestTrue(TEXT("fallback applied when server IP is local"), bApplied);
  TestTrue(TEXT("host replaced, path+query kept"), OutUrl == TEXT("http://192.168.1.10:8710/files/a.pak?x=1"));
  TestTrue(TEXT("reason mentions matched IP"), Reason.Contains(TEXT("127.0.0.1")));
 }
 {
  // 局域网基址没写端口 -> 沿用原 URL 端口
  FString OutUrl, Reason;
  const bool bApplied = DecideLanFallback(
   TEXT("http://192.168.1.50:9999/x"), TEXT("http://10.0.0.9"),
   { TEXT("192.168.1.50") }, LocalIps, OutUrl, Reason);
  TestTrue(TEXT("applied with inherited port"), bApplied);
  TestTrue(TEXT("original port inherited"), OutUrl == TEXT("http://10.0.0.9:9999/x"));
 }

 // ---------- 不命中：服务器在别处 ----------
 {
  FString OutUrl, Reason;
  const bool bApplied = DecideLanFallback(
   TEXT("http://203.0.113.7:8710/x"), TEXT("http://192.168.1.10:8710"),
   { TEXT("203.0.113.7") }, LocalIps, OutUrl, Reason);
  TestFalse(TEXT("no fallback for remote server"), bApplied);
  TestTrue(TEXT("url untouched"), OutUrl == TEXT("http://203.0.113.7:8710/x"));
 }

 // ---------- 幂等：已经是局域网地址 ----------
 {
  FString OutUrl, Reason;
  const bool bApplied = DecideLanFallback(
   TEXT("http://192.168.1.10:8710/x"), TEXT("http://192.168.1.10:8710"),
   { TEXT("192.168.1.10") }, LocalIps, OutUrl, Reason);
  TestFalse(TEXT("no rewrite when already on the LAN address"), bApplied);
 }

 // ---------- 关闭/空配置 ----------
 {
  FString OutUrl, Reason;
  TestFalse(TEXT("empty LAN url disables fallback"),
   DecideLanFallback(TEXT("http://127.0.0.1:8710/x"), TEXT(""), { TEXT("127.0.0.1") }, LocalIps, OutUrl, Reason));
 }
 {
  FString OutUrl, Reason;
  TestFalse(TEXT("non-http LAN url is rejected"),
   DecideLanFallback(TEXT("http://127.0.0.1:8710/x"), TEXT("192.168.1.10:8710"), { TEXT("127.0.0.1") }, LocalIps, OutUrl, Reason));
 }
 return true;
}
#endif
