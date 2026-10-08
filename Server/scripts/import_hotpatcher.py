#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
导入 HotPatcher 产物到管理服务器数据目录（命令行入口）。

逻辑已迁移到 importer.py，本文件仅作薄封装，便于单独使用：
  python scripts/import_hotpatcher.py --source <dir> --data <dir> --project CodeBuild --platform Windows
"""
import argparse
import os
import sys
from pathlib import Path

SERVER_DIR = Path(__file__).resolve().parent.parent
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))

from config import load_config, resolve_server_path
from importer import run_import


def main():
    parser = argparse.ArgumentParser(description="导入 HotPatcher 产物到管理服务器")
    parser.add_argument("--source", default=None, help="补丁包位置（HotPatcher 产物目录，默认取 Server/config.json）")
    parser.add_argument("--data", default=None, help="数据目录（默认取 Server/config.json）")
    parser.add_argument("--project", default=None)
    parser.add_argument("--platform", default=None)
    parser.add_argument("--order", default=None, help="版本顺序，逗号分隔（默认取 Server/config.json）")
    parser.add_argument("--only", default="", help="只导入指定版本，逗号分隔（可选）")
    args = parser.parse_args()

    server_cfg = load_config()
    platform = args.platform or server_cfg["default_platform"]
    if platform not in server_cfg["platforms"]:
        parser.error("项目未配置这个平台")
    options = server_cfg["platform_settings"].get(platform, {})
    source_dir = resolve_server_path(args.source or options.get("patchSourceDir", ""), Path(server_cfg["_config_path"]).parent)
    data_dir = os.path.abspath(args.data or server_cfg["data_dir"])
    server_cfg["project"] = args.project or server_cfg["project"]
    if args.data:
        server_cfg["data_dir"] = data_dir
        server_cfg["versions_dir"] = os.path.join(data_dir, "versions")
        server_cfg["manifests_dir"] = os.path.join(data_dir, "manifests")
    order = args.order if args.order is not None else options.get("hotpatcherOrder", "")
    run_import(source_dir, data_dir, server_cfg["project"], platform, order, only=args.only, config=server_cfg)



if __name__ == "__main__":
    main()
