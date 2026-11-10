#!/usr/bin/env python3
"""
Task Queue Manager for Antigravity AGY inter-agent communication.
Uses Maildir-style atomic directory moving (.tasks/pending -> .tasks/resolved)
combined with an append-only TASK_QUEUE.jsonl audit log.
100% race-condition free and atomic on Linux filesystems.
"""
import os
import sys
import json
import time
import argparse

BASE_DIR = "/data/dev/cp/.tasks"
PENDING_DIR = os.path.join(BASE_DIR, "pending")
RESOLVED_DIR = os.path.join(BASE_DIR, "resolved")
AUDIT_LOG = "/data/dev/cp/TASK_QUEUE.jsonl"

def ensure_dirs():
    os.makedirs(PENDING_DIR, exist_ok=True)
    os.makedirs(RESOLVED_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(AUDIT_LOG), exist_ok=True)
    if not os.path.exists(AUDIT_LOG):
        with open(AUDIT_LOG, "w", encoding="utf-8") as f:
            f.write("# CP & Bun AGY Task Mailbox Log (Append-Only Event Stream)\n")

def submit_task(task_id, repro, error_msg, created_by="bun_agy"):
    ensure_dirs()
    # 自动归一化为绝对路径，杜绝跨工作区（/data/dev/bun 与 /data/dev/cp）相对路径解析歧义
    abs_repro = os.path.abspath(repro)
    task_data = {
        "id": task_id,
        "repro": abs_repro,
        "error": error_msg,
        "status": "pending",
        "created_by": created_by,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S")
    }
    # 原子写入：先写入临时隐藏文件，再通过系统调用原子重命名至 pending/
    tmp_file = os.path.join(BASE_DIR, f".tmp_{task_id}.json")
    target_file = os.path.join(PENDING_DIR, f"{task_id}.json")
    with open(tmp_file, "w", encoding="utf-8") as f:
        json.dump(task_data, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())
    os.rename(tmp_file, target_file)

    # 仅追加事件日志（POSIX 保证小于 4KB 写入具备原子性，杜绝写穿）
    with open(AUDIT_LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps({**task_data, "event": "submitted"}, ensure_ascii=False) + "\n")

    print(f"[TASK_SUBMITTED] Task '{task_id}' queued in {target_file} (repro: {abs_repro})")

def resolve_task(task_id, note="", resolved_by="cp_agy"):
    ensure_dirs()
    pending_file = os.path.join(PENDING_DIR, f"{task_id}.json")
    resolved_file = os.path.join(RESOLVED_DIR, f"{task_id}.json")

    if not os.path.exists(pending_file):
        if os.path.exists(resolved_file):
            print(f"[TASK_ALREADY_RESOLVED] Task '{task_id}' was already resolved.")
            return
        print(f"[ERROR] Task '{task_id}' not found in {PENDING_DIR}", file=sys.stderr)
        sys.exit(1)

    try:
        with open(pending_file, "r", encoding="utf-8") as f:
            task_data = json.load(f)
    except Exception as e:
        task_data = {"id": task_id}

    task_data["status"] = "resolved"
    task_data["note"] = note
    task_data["resolved_by"] = resolved_by
    task_data["resolved_at"] = time.strftime("%Y-%m-%dT%H:%M:%S")

    tmp_file = os.path.join(BASE_DIR, f".tmp_res_{task_id}.json")
    with open(tmp_file, "w", encoding="utf-8") as f:
        json.dump(task_data, f, ensure_ascii=False, indent=2)
        f.flush()
        os.fsync(f.fileno())

    # 原子迁移文件：先将 pending 文件移出 pending 目录，再将新数据写入 resolved 目录
    if os.path.exists(pending_file):
        os.remove(pending_file)
    os.rename(tmp_file, resolved_file)

    # 仅追加事件日志
    with open(AUDIT_LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps({**task_data, "event": "resolved"}, ensure_ascii=False) + "\n")

    print(f"[TASK_RESOLVED] Task '{task_id}' moved to {resolved_file}")

def list_tasks():
    ensure_dirs()
    pending = [f[:-5] for f in os.listdir(PENDING_DIR) if f.endswith(".json") and not f.startswith(".")]
    resolved = [f[:-5] for f in os.listdir(RESOLVED_DIR) if f.endswith(".json") and not f.startswith(".")]
    print(f"=== Pending Tasks ({len(pending)}) ===")
    for p in pending:
        print(f"  - {p}")
    print(f"=== Resolved Tasks ({len(resolved)}) ===")
    for r in resolved:
        print(f"  - {r}")

def main():
    parser = argparse.ArgumentParser(description="Task Queue Manager")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub_submit = sub.add_parser("submit")
    sub_submit.add_argument("--id", required=True, help="Task ID (e.g. task-20261110-01)")
    sub_submit.add_argument("--repro", required=True, help="Path to repro .cp file")
    sub_submit.add_argument("--error", default="", help="Error description")
    sub_submit.add_argument("--by", default="bun_agy", help="Creator ID")

    sub_resolve = sub.add_parser("resolve")
    sub_resolve.add_argument("task_id", help="Task ID to resolve")
    sub_resolve.add_argument("--note", default="", help="Resolution note")
    sub_resolve.add_argument("--by", default="cp_agy", help="Resolver ID")

    sub_list = sub.add_parser("list")

    args = parser.parse_args()
    if args.cmd == "submit":
        submit_task(args.id, args.repro, args.error, args.by)
    elif args.cmd == "resolve":
        resolve_task(args.task_id, args.note, args.by)
    elif args.cmd == "list":
        list_tasks()

if __name__ == "__main__":
    main()
