# -*- coding: utf-8 -*-
"""存储后端抽象：本地磁盘 (local) 与 S3 兼容对象存储 (oss / cos / s3)。

客户端（启动器）拿到的下载 URL：
  - local 模式：相对路径 /files/<category>/<...>，由 HTTP 服务的 /files/* 路由读取磁盘下发。
  - remote (s3) 模式：presigned HTTPS URL，客户端直连对象存储下载（带宽走云厂商）。

对象 key 约定（与历史 /files/ 路径一一对应）：
  /files/versions/<rest>   -> versions/<rest>
  /files/packages/<rest>   -> packages/<rest>
  /files/launcher/<rest>   -> launcher/<rest>
  /files/background/<name> -> launcher/background/<name>
即：下载 URL 去掉前导 "/files/" 即为对象 key。
"""
import datetime
import hashlib
import hmac
import http.client
import os
import shutil
import time
from urllib.parse import quote

from config import get_base_dir, safe_join, launcher_bg_dir


def _hmac_sha256(key: bytes, msg: str) -> bytes:
    return hmac.new(key, msg.encode("utf-8"), hashlib.sha256).digest()


def _sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _uri_encode(s: str, safe_slash: bool = False) -> str:
    """AWS SigV4 的 URI 编码：保留 A-Za-z0-9-_.~，其余百分号编码；'/' 可选保留。"""
    out = []
    for ch in s:
        if ch.isalnum() or ch in "-_.~":
            out.append(ch)
        elif ch == "/" and safe_slash:
            out.append("/")
        else:
            out.append("%" + "%02X" % ord(ch))
    return "".join(out)


def _clean_rel(rel: str) -> str:
    """规范化相对路径：统一分隔符、丢弃 '.' 与 '..' 段、去掉前后多余分隔符。

    用于阻止对象存储 key 或本地相对路径通过 '../' 逃逸出预期前缀/目录。
    例如 'a/../b' -> 'b'，'//c/' -> 'c'，'..\\..\\etc' -> 'etc'。
    """
    if not rel:
        return ""
    parts = []
    for seg in str(rel).replace("\\", "/").split("/"):
        if seg in ("", ".", ".."):
            continue
        parts.append(seg)
    return "/".join(parts)


class StorageBackend:
    is_remote = False

    def _storage_key(self, category, *, platform="", version="", rel="", name="", **_):
        # 清洗各分段，避免 '..' 逃逸对象 key 前缀
        platform = _clean_rel(platform)
        version = _clean_rel(version)
        rel = _clean_rel(rel)
        name = _clean_rel(name)
        if category == "packages":
            return f"packages/{platform}/{version}/{rel}"
        if category == "versions":
            return f"versions/{version}/{rel}"
        if category == "launcher":
            return f"launcher/{rel}"
        if category == "background":
            return f"launcher/background/{name}"
        raise ValueError(f"未知存储分类: {category}")

    def url_for(self, category, **kw):
        """返回客户端可直接下载的 URL（local 为 /files/...；remote 为 presigned URL）。"""
        return self.url_for_key(self._storage_key(category, **kw))

    def url_for_key(self, key: str) -> str:
        raise NotImplementedError

    def local_path(self, category, **kw):
        raise NotImplementedError

    def upload_file(self, category, *, src_path, **kw):
        raise NotImplementedError

    def delete_object(self, key: str):
        raise NotImplementedError


class LocalStorage(StorageBackend):
    is_remote = False

    def __init__(self, cfg):
        self.cfg = cfg

    def url_for_key(self, key: str) -> str:
        return "/files/" + _clean_rel(key)

    def _safe(self, root, rel):
        """在 root 下安全解析 rel，返回绝对路径；越界、含 '..'、绝对路径或经符号链接逃逸均返回 None。"""
        raw = str(rel).replace("\\", "/")
        # 任何 '..' 尝试或绝对路径一律拒绝（合法文件名不会包含 '..'）
        if (raw.startswith("/") or raw.startswith("../") or raw.endswith("/..")
                or "/../" in raw or raw == ".."):
            return None
        candidate = safe_join(root, _clean_rel(rel))
        if candidate is None:
            return None
        # 防御：禁止通过符号链接 / 连接点逃逸出 root
        try:
            real_root = os.path.realpath(root)
            real_cand = os.path.realpath(candidate)
            if not (real_cand == real_root or real_cand.startswith(real_root + os.sep)):
                return None
        except OSError:
            return None
        return candidate

    def local_path(self, category, *, platform="", version="", rel="", name="", **_):
        if category == "packages":
            base = get_base_dir(self.cfg, platform, version)
            return self._safe(base, rel) if base else None
        if category == "versions":
            return self._safe(os.path.join(self.cfg["versions_dir"], version), rel)
        if category == "launcher":
            return self._safe(os.path.join(self.cfg["data_dir"], "launcher"), rel)
        if category == "background":
            return self._safe(launcher_bg_dir(self.cfg), name)
        return None

    def upload_file(self, category, *, src_path, **kw):
        dst = self.local_path(category, **kw)
        if dst is None:
            return False
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src_path, dst)
        return True

    def delete_object(self, key: str):
        p = self._safe(self.cfg["data_dir"], key)
        if p and os.path.isfile(p):
            os.remove(p)


class S3Storage(StorageBackend):
    is_remote = True

    def __init__(self, cfg):
        s = (cfg.get("storage") or {}).get("s3") or {}
        self.scheme = "http" if (s.get("endpoint") or "").strip().lower().startswith("http://") else "https"
        ep = (s.get("endpoint") or "").strip()
        if ep.startswith("https://"):
            ep = ep[len("https://"):]
        elif ep.startswith("http://"):
            ep = ep[len("http://"):]
        port = 443
        host_part = ep
        if ":" in host_part:
            hp, pp = host_part.rsplit(":", 1)
            if pp.isdigit():
                host_part = hp
                port = int(pp)
        self.endpoint_host = host_part
        self.port = port
        self.region = s.get("region") or ""
        self.service = s.get("service") or "s3"
        self.bucket = s.get("bucket") or ""
        self.ak = s.get("accessKeyId") or ""
        self.sk = s.get("secretAccessKey") or ""
        self.prefix = (s.get("prefix") or "").strip("/")
        self.public_base = (s.get("publicBaseUrl") or "").rstrip("/")
        self.addressing = s.get("addressingStyle") or "virtual"
        self.expires = int(s.get("presignExpires") or 3600)

    # ---------- 内部工具 ----------
    def _full_key(self, key: str) -> str:
        return f"{self.prefix}/{key}" if self.prefix else key

    def _host(self) -> str:
        if self.addressing == "path":
            return self.endpoint_host
        return f"{self.bucket}.{self.endpoint_host}"

    def _canonical_uri(self, key: str) -> str:
        if self.addressing == "path":
            return "/" + _uri_encode(self.bucket) + "/" + _uri_encode(key, safe_slash=True)
        return "/" + _uri_encode(key, safe_slash=True)

    def _signing_key(self, datestamp: str) -> bytes:
        k = _hmac_sha256(("AWS4" + self.sk).encode("utf-8"), datestamp)
        k = _hmac_sha256(k, self.region)
        k = _hmac_sha256(k, self.service)
        return _hmac_sha256(k, "aws4_request")

    # ---------- 对外接口 ----------
    def url_for_key(self, key: str) -> str:
        full = self._full_key(_clean_rel(key))
        if self.public_base:
            return f"{self.public_base}/{quote(full)}"
        return self._presign_get(full)

    def _presign_get(self, key: str) -> str:
        if not self.endpoint_host or not self.bucket:
            raise RuntimeError("storage.s3 未配置 endpoint / bucket")
        t = datetime.datetime.now(datetime.timezone.utc)
        amzdate = t.strftime("%Y%m%dT%H%M%SZ")
        datestamp = t.strftime("%Y%m%d")
        host = self._host()
        canon_uri = self._canonical_uri(key)
        cred = f"{self.ak}/{datestamp}/{self.region}/{self.service}/aws4_request"
        params = {
            "X-Amz-Algorithm": "AWS4-HMAC-SHA256",
            "X-Amz-Credential": cred,
            "X-Amz-Date": amzdate,
            "X-Amz-Expires": str(self.expires),
            "X-Amz-SignedHeaders": "host",
        }
        canon_query = "&".join(f"{_uri_encode(k)}={_uri_encode(v)}" for k, v in sorted(params.items()))
        canon_headers = f"host:{host}\n"
        canon_req = "\n".join(["GET", canon_uri, canon_query, canon_headers, "host", "UNSIGNED-PAYLOAD"])
        scope = f"{datestamp}/{self.region}/{self.service}/aws4_request"
        string_to_sign = "\n".join([
            "AWS4-HMAC-SHA256", amzdate, scope, _sha256_hex(canon_req.encode("utf-8"))
        ])
        sig = hmac.new(self._signing_key(datestamp), string_to_sign.encode("utf-8"), hashlib.sha256).hexdigest()
        return f"{self.scheme}://{host}{canon_uri}?{canon_query}&X-Amz-Signature={sig}"

    def _sign_v4_headers(self, method: str, key: str, data: bytes):
        t = datetime.datetime.now(datetime.timezone.utc)
        amzdate = t.strftime("%Y%m%dT%H%M%SZ")
        datestamp = t.strftime("%Y%m%d")
        host = self._host()
        payload_hash = _sha256_hex(data)
        std = {
            "host": host,
            "x-amz-date": amzdate,
            "x-amz-content-sha256": payload_hash,
        }
        keys = sorted(std)
        canon_headers = "".join(f"{k}:{std[k]}\n" for k in keys)
        signed_headers = ";".join(keys)
        canon_uri = self._canonical_uri(key)
        canon_req = "\n".join([method, canon_uri, "", canon_headers, signed_headers, payload_hash])
        scope = f"{datestamp}/{self.region}/{self.service}/aws4_request"
        string_to_sign = "\n".join([
            "AWS4-HMAC-SHA256", amzdate, scope, _sha256_hex(canon_req.encode("utf-8"))
        ])
        sig = hmac.new(self._signing_key(datestamp), string_to_sign.encode("utf-8"), hashlib.sha256).hexdigest()
        auth = (f"AWS4-HMAC-SHA256 Credential={self.ak}/{scope}, "
                f"SignedHeaders={signed_headers}, Signature={sig}")
        out = dict(std)
        out["Authorization"] = auth
        return out

    def _do_request(self, method: str, key: str, body: bytes, headers: dict):
        host = self._host()
        # 请求路径必须与签名用的 canonical URI 完全一致（均使用 _uri_encode）
        if self.addressing == "path":
            path = "/" + _uri_encode(self.bucket) + "/" + _uri_encode(key, safe_slash=True)
        else:
            path = "/" + _uri_encode(key, safe_slash=True)
        if self.scheme == "http":
            conn = http.client.HTTPConnection(host, self.port, timeout=600)
        else:
            conn = http.client.HTTPSConnection(host, self.port, timeout=600)
        try:
            conn.request(method, path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
        finally:
            conn.close()
        if resp.status >= 300:
            raise RuntimeError(f"对象存储请求失败 {resp.status}: {data[:500].decode('utf-8', 'replace')}")
        return data

    def upload_file(self, category, *, src_path, **kw):
        full = self._full_key(self._storage_key(category, **kw))
        with open(src_path, "rb") as f:
            data = f.read()
        headers = self._sign_v4_headers("PUT", full, data)
        self._do_request("PUT", full, data, headers)
        return True

    def delete_object(self, key: str):
        full = self._full_key(_clean_rel(key))
        headers = self._sign_v4_headers("DELETE", full, b"")
        try:
            self._do_request("DELETE", full, b"", headers)
        except RuntimeError as exc:
            # 404 视为已删除，忽略
            if "404" not in str(exc):
                raise

    def test_connection(self):
        """对 bucket 根路径执行 HEAD 请求，验证 endpoint / bucket / 凭据是否可用。"""
        if not self.endpoint_host or not self.bucket:
            raise RuntimeError("storage.s3 未配置 endpoint / bucket")
        if not self.ak or not self.sk:
            raise RuntimeError("storage.s3 未配置 accessKeyId / secretAccessKey")
        headers = self._sign_v4_headers("HEAD", "", b"")
        self._do_request("HEAD", "", b"", headers)
        return True


_STORAGE_CACHE = {}


def get_storage(cfg):
    """返回存储后端实例（基于 cfg['storage']['backend']，默认 local）。结果缓存到 cfg['_storage']。"""
    if cfg.get("_storage") is None:
        backend = (cfg.get("storage") or {}).get("backend") or "local"
        if backend == "s3":
            cfg["_storage"] = S3Storage(cfg)
        else:
            cfg["_storage"] = LocalStorage(cfg)
    return cfg["_storage"]


if __name__ == "__main__":
    # 简单自测：用伪造配置打印 presigned URL 结构（不发起真实请求）
    fake = {
        "storage": {
            "backend": "s3",
            "s3": {
                "endpoint": "oss-cn-hangzhou.aliyuncs.com",
                "region": "cn-hangzhou",
                "service": "oss",
                "bucket": "my-bucket",
                "accessKeyId": "AKIDEXAMPLE",
                "secretAccessKey": "SECRET",
                "prefix": "cloudupdate",
                "addressingStyle": "virtual",
                "presignExpires": 3600,
            },
        }
    }
    st = S3Storage(fake)
    url = st.url_for("versions", version="1.4", rel="Windows/1.4_Content.pak")
    print("presigned url sample:")
    print(url)
    assert url.startswith("https://my-bucket.oss-cn-hangzhou.aliyuncs.com/")
    assert "X-Amz-Signature=" in url
    print("OK: presigned URL 结构正确")
