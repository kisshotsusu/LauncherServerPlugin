import threading, time, json, os, tempfile, urllib.request, urllib.error, traceback
import run_server, importer

BASE = "http://127.0.0.1:8799"
errs = []

def post(path, data=None, raw=None, ctype="application/json"):
    url = BASE + path
    if raw is not None:
        body = raw
    elif data is not None:
        body = json.dumps(data).encode("utf-8")
    else:
        body = b""
    req = urllib.request.Request(url, data=body, method="POST", headers={"Content-Type": ctype})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        return -1, "%s: %s" % (type(e).__name__, e)

def delete(path):
    req = urllib.request.Request(BASE + path, method="DELETE")
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, r.read().decode("utf-8", "replace")
    except urllib.error.HTTPError as e:
        return e.code, e.read().decode("utf-8", "replace")
    except Exception as e:
        return -1, "%s: %s" % (type(e).__name__, e)

try:
    cfg = run_server.load_config("config.json")
    run_server.UpdateHandler.cfg = cfg
    srv = run_server.ThreadingHTTPServer(("127.0.0.1", 8799), run_server.UpdateHandler)
    run_server.UpdateHandler.server = srv
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    time.sleep(0.5)

    print("reindex:", post("/api/versions/reindex"))
    print("cfg-update ok:", post("/api/config/update", {"project": "CodeBuild", "maxUploadMb": 2048}))
    print("cfg-update badport:", post("/api/config/update", {"port": "abc"}))
    print("cfg-update badstorage:", post("/api/config/update", {"storage": {"backend": "s3", "s3": {"endpoint": ""}}}))
    print("enabled:", post("/api/enabled_versions", {"platform": "Windows", "versions": ["1.0", "1.4"]}))
    print("import:", post("/api/import/hotpatcher"))
    print("add-launcher:", post("/api/launcher/versions", {"version": "9.9.9", "dir": "data/launcher"}))
    print("publish-launcher:", post("/api/launcher/publish", {"version": "9.9.9"}))
    print("del-launcher:", delete("/api/launcher/versions/9.9.9"))
    print("storage-test:", post("/api/storage/test", {"storage": {"backend": "s3", "s3": {"endpoint": "x", "bucket": "b", "accessKeyId": "a", "secretAccessKey": "s"}}}))
    print("manifest-gen:", post("/api/manifest/generate?platform=Windows"))
    mp = b"--B\r\nContent-Disposition: form-data; name=\"file\"; filename=\"t.txt\"\r\n\r\nhello\r\n--B--\r\n"
    print("upload:", post("/api/upload?target=background", raw=mp, ctype="multipart/form-data; boundary=B"))

    print("\n--- run_import config isolation test ---")
    tmp = tempfile.mkdtemp()
    bcfg = os.path.join(tmp, "cfg.json")
    with open(bcfg, "w", encoding="utf-8") as f:
        json.dump({"project": "T", "platforms": ["Windows"], "data_dir": tmp,
                   "base_packages": {"Windows": {"2.0": "E:\\nonexist_base"}}}, f)
    bc = run_server.load_config(bcfg)
    importer.run_import("D:\\notexist_src", os.path.join(tmp, "data"), "T", "Windows", "", "")
except Exception as e:
    traceback.print_exc()
    errs.append(repr(e))
print("\n=== ERRORS:", len(errs))
for e in errs:
    print(e)
