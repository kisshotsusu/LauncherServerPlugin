#pragma once

#include <atomic>
#include <functional>
#include <string>

struct HttpResult {
    long status = 0;
    std::string body;
    std::string error;
    bool ok() const { return status >= 200 && status < 300; }
};

class HttpClient {
public:
    // 同步 GET，返回完整响应体
    HttpResult get(const std::string& url, int timeoutSec = 60);

    // 下载到文件，带限速（KB/s，0=不限）与进度回调、取消标志
    bool download(const std::string& url,
                  const std::string& destPath,
                  long speedLimitKBps,
                  int timeoutSec,
                  const std::function<void(int64_t done, int64_t total)>& progress,
                  const std::atomic<bool>& cancel,
                  std::string& outError);
};

std::string fileMD5(const std::string& path, std::string* err = nullptr);
std::string urlEncode(const std::string& s);
std::string httpUrlJoin(const std::string& base, const std::string& path);
