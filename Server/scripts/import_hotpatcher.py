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

from config import load_config
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
    source_dir = os.path.abspath(args.source or server_cfg.get("hotpatcher_source", ""))
    data_dir = os.path.abspath(args.data or server_cfg.get("data_dir", "data"))
    project = args.project or server_cfg.get("project", "CodeBuild")
    platform = args.platform or (server_cfg.get("platforms") or ["Windows"])[0]
    order = args.order if args.order is not None else server_cfg.get("hotpatcher_order", "")
    run_import(source_dir, data_dir, project, platform, order, only=args.only)


if __name__ == "__main__":
    main()
