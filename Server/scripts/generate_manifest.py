#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
重新生成完整性清单。

用法：
  python scripts/generate_manifest.py --platform Windows [--force]

也可以直接访问管理服务器：
  GET  /api/manifest.json?platform=Windows&refresh=1
  POST /api/manifest/generate?platform=Windows
"""

import argparse
import os
import sys
import time
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent
SERVER_DIR = SCRIPT_DIR.parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from run_server import ensure_dirs, generate_manifest, load_config  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description="重新生成完整性清单")
    parser.add_argument("--platform", default=None, help="平台，默认全部")
    parser.add_argument("--force", action="store_true", help="强制重新生成")
    args = parser.parse_args()

    cfg = load_config()
    ensure_dirs(cfg)
    platforms = [args.platform] if args.platform else cfg["platforms"]

    for platform in platforms:
        print(f"正在扫描 {platform} ...")
        t0 = time.time()
        manifest = generate_manifest(cfg, platform, force=args.force)
        cost = time.time() - t0
        print(f"  完成：{manifest['fileCount']} 个文件，耗时 {cost:.1f}s")
        path = os.path.join(cfg["manifests_dir"], f"{cfg['project']}_{platform}.json")
        print(f"  清单文件：{path}")


if __name__ == "__main__":
    main()
