#!/usr/bin/env python3
"""
Task Queue Watcher for Antigravity AGY inter-agent communication.
Monitors /data/dev/cp/.tasks/pending/ (and resolved/)
Exits with code 0 immediately when a matching task is ready, triggering Antigravity CLI's reactive wakeup.
"""
import sys
import time
import os
import json
import argparse

BASE_DIR = "/data/dev/cp/.tasks"
PENDING_DIR = os.path.join(BASE_DIR, "pending")
RESOLVED_DIR = os.path.join(BASE_DIR, "resolved")

def check_pending_directory(task_id_filter=None):
    if not os.path.exists(PENDING_DIR):
        return None
    try:
        files = sorted([f for f in os.listdir(PENDING_DIR) if f.endswith(".json") and not f.startswith(".")])
        for fname in files:
            tid = fname[:-5]
            if task_id_filter and tid != task_id_filter:
                continue
            fpath = os.path.join(PENDING_DIR, fname)
            try:
                with open(fpath, "r", encoding="utf-8") as f:
                    task = json.load(f)
                if task.get("status") == "pending":
                    return task
            except (json.JSONDecodeError, OSError):
                # 捕获半写入或锁占用异常，跳过并等待下一次循环
                continue
    except Exception:
        pass
    return None

def check_resolved_directory(task_id_filter):
    if not os.path.exists(RESOLVED_DIR) or not task_id_filter:
        return None
    fpath = os.path.join(RESOLVED_DIR, f"{task_id_filter}.json")
    if os.path.exists(fpath):
        try:
            with open(fpath, "r", encoding="utf-8") as f:
                task = json.load(f)
            if task.get("status") == "resolved":
                return task
        except (json.JSONDecodeError, OSError):
            pass
    return None

def main():
    parser = argparse.ArgumentParser(description="Watch task queue for pending or resolved tasks")
    parser.add_argument("--status", default="pending", choices=["pending", "resolved"], help="Task status to wait for (default: pending)")
    parser.add_argument("--id", default=None, help="Specific task ID to wait for")
    parser.add_argument("--interval", type=float, default=1.0, help="Poll interval in seconds (default: 1.0)")
    parser.add_argument("--timeout", type=int, default=0, help="Timeout in seconds (0 = infinite)")
    args = parser.parse_args()

    print(f"[WATCHER] Listening for tasks (status={args.status}, id={args.id or 'ANY'})...", flush=True)

    start_time = time.time()
    while True:
        # 首轮零延迟即时检测：启动时若已有任务，立即退出唤醒，绝不空等
        if args.status == "pending":
            task = check_pending_directory(args.id)
        else:
            task = check_resolved_directory(args.id)

        if task:
            task_id = task.get("id", "unknown")
            repro = task.get("repro", "")
            print(f"[FOUND_TASK] ID: {task_id} | Repro: {repro}", flush=True)
            print(json.dumps(task, ensure_ascii=False), flush=True)
            sys.exit(0)  # 👈 立即退出，触发 Antigravity CLI Reactive Wakeup 毫秒级唤醒！

        if args.timeout > 0 and (time.time() - start_time) > args.timeout:
            print(f"[WATCHER_TIMEOUT] Timed out waiting after {args.timeout}s", file=sys.stderr)
            sys.exit(1)

        time.sleep(args.interval)

if __name__ == "__main__":
    main()
