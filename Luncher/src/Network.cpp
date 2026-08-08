#include "Network.h"

#include "Config.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <chrono>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <thread>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

std::wstring u8w(const std::string& s) { return utf8ToWide(s); }

struct HttpSession {
    HINTERNET session = nullptr;
    HttpSession() {
        session = WinHttpOpen(L"CloudLauncher/1.0",
                              WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    }
    ~HttpSession() { if (session) WinHttpCloseHandle(session); }
    bool valid() const { return session != nullptr; }
};

bool parseUrl(const std::string& url, std::wstring& host, std::wstring& path, INTERNET_PORT& port, bool& https) {
    std::string u = url;
    https = u.rfind("https://", 0) == 0;
    std::string rest;
    if (https) rest = u.substr(8);
    else if (u.rfind("http://", 0) == 0) rest = u.substr(7);
    else return false;
    auto slash = rest.find('/');
    std::string hostPart = slash == std::string::npos ? rest : rest.substr(0, slash);
    std::string pathPart = slash == std::string::npos ? "/" : rest.substr(slash);
    port = https ? 443 : 80;
    auto colon = hostPart.find(':');
    if (colon != std::string::npos) {
        port = static_cast<INTERNET_PORT>(atoi(hostPart.c_str() + colon + 1));
        hostPart = hostPart.substr(0, colon);
    }
    host = u8w(hostPart);
    path = u8w(pathPart);
    return !host.empty();
}

}  // namespace

HttpResult HttpClient::get(const std::string& url, int timeoutSec) {
    HttpResult result;
    HttpSession session;
    if (!session.valid()) {
        result.error = "WinHttpOpen failed";
        return result;
    }
    std::wstring host, path;
    INTERNET_PORT port = 80;
    bool https = false;
    if (!parseUrl(url, host, path, port, https)) {
        result.error = "bad url";
        return result;
    }
    HINTERNET conn = WinHttpConnect(session.session, host.c_str(), port, 0);
    if (!conn) { result.error = "WinHttpConnect failed"; return result; }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(conn); result.error = "WinHttpOpenRequest failed"; return result; }
    WinHttpSetTimeouts(req, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000);
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        result.error = "WinHttpSendRequest failed";
    } else if (!WinHttpReceiveResponse(req, nullptr)) {
        result.error = "WinHttpReceiveResponse failed";
    } else {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        result.status = status;
        DWORD avail = 0;
        std::string body;
        char buf[65536];
        do {
            if (!WinHttpQueryDataAvailable(req, &avail)) break;
            if (!avail) {
                if (!WinHttpReceiveResponse(req, nullptr)) break;
                continue;
            }
            DWORD read = 0;
            if (!WinHttpReadData(req, buf, std::min<DWORD>(avail, sizeof(buf)), &read) || read == 0) break;
            body.append(buf, read);
        } while (avail > 0);
        result.body = std::move(body);
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return result;
}

bool HttpClient::download(const std::string& url,
                          const std::string& destPath,
                          long speedLimitKBps,
                          int timeoutSec,
                          const std::function<void(int64_t, int64_t)>& progress,
                          const std::atomic<bool>& cancel,
                          std::string& outError) {
    HttpSession session;
    if (!session.valid()) { outError = "WinHttpOpen failed"; return false; }
    std::wstring host, path;
    INTERNET_PORT port = 80;
    bool https = false;
    if (!parseUrl(url, host, path, port, https)) { outError = "bad url"; return false; }

    HINTERNET conn = WinHttpConnect(session.session, host.c_str(), port, 0);
    if (!conn) { outError = "WinHttpConnect failed"; return false; }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                       https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(conn); outError = "WinHttpOpenRequest failed"; return false; }
    WinHttpSetTimeouts(req, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000, timeoutSec * 1000);

    bool ok = false;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status < 200 || status >= 300) {
            outError = "HTTP " + std::to_string(status);
        } else {
            int64_t contentLength = -1;
            wchar_t lenBuf[64] = {0};
            DWORD lenSize = sizeof(lenBuf);
            if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH,
                                    WINHTTP_HEADER_NAME_BY_INDEX, lenBuf, &lenSize,
                                    WINHTTP_NO_HEADER_INDEX)) {
                contentLength = _wtoi64(lenBuf);
            }
            std::ofstream file(destPath, std::ios::binary | std::ios::trunc);
            if (!file) {
                outError = "cannot open " + destPath;
            } else {
                const double rateBytes = speedLimitKBps > 0 ? speedLimitKBps * 1024.0 : 0.0;
                char buf[65536];
                int64_t total = 0;
                DWORD avail = 0;
                ok = true;
                while (ok && !cancel.load()) {
                    auto chunkStart = std::chrono::steady_clock::now();
                    if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                    if (!avail) {
                        if (!WinHttpReceiveResponse(req, nullptr)) break;
                        continue;
                    }
                    DWORD read = 0;
                    if (!WinHttpReadData(req, buf, std::min<DWORD>(avail, sizeof(buf)), &read) || read == 0) break;
                    file.write(buf, read);
                    total += read;
                    if (progress) progress(total, contentLength);
                    if (rateBytes > 0) {
                        auto took = std::chrono::duration<double>(std::chrono::steady_clock::now() - chunkStart).count();
                        double want = static_cast<double>(read) / rateBytes;
                        if (want > took) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>((want - took) * 1000)));
                        }
                    }
                    if (cancel.load()) { ok = false; outError = "cancelled"; }
                }
                file.close();
                if (cancel.load() && outError.empty()) outError = "cancelled";
            }
        }
    } else {
        outError = "WinHttp request failed";
    }
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return ok;
}

std::string fileMD5(const std::string& path, std::string* err) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    std::string result;
    if (!CryptAcquireContext(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT)) {
        if (err) *err = "CryptAcquireContext failed";
        return result;
    }
    if (CryptCreateHash(prov, CALG_MD5, 0, 0, &hash)) {
        std::ifstream in(path, std::ios::binary);
        char buf[1 << 16];
        bool readOk = true;
        while (in) {
            in.read(buf, sizeof(buf));
            std::streamsize n = in.gcount();
            if (n > 0 && !CryptHashData(hash, reinterpret_cast<BYTE*>(buf), static_cast<DWORD>(n), 0)) {
                readOk = false;
                break;
            }
            if (n < static_cast<std::streamsize>(sizeof(buf))) break;
        }
        if (readOk) {
            BYTE digest[16] = {0};
            DWORD digestLen = sizeof(digest);
            if (CryptGetHashParam(hash, HP_HASHVAL, digest, &digestLen, 0)) {
                static const char* hex = "0123456789abcdef";
                for (DWORD i = 0; i < digestLen; ++i) {
                    result += hex[digest[i] >> 4];
                    result += hex[digest[i] & 0xF];
                }
            }
        } else if (err) {
            *err = "read failed";
        }
        CryptDestroyHash(hash);
    } else if (err) {
        *err = "CryptCreateHash failed";
    }
    CryptReleaseContext(prov, 0);
    return result;
}

std::string urlEncode(const std::string& s) {
    std::string out;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string httpUrlJoin(const std::string& base, const std::string& path) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    std::string p = path;
    while (!p.empty() && p.front() == '/') p.erase(p.begin());
    return b + "/" + p;
}
