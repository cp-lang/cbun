#!/usr/bin/env python3
"""
Convenience wrapper to mark a task as resolved.
Calls task_queue.py resolve with atomic Maildir-style movement.
"""
import sys
import os

script_dir = os.path.dirname(os.path.abspath(__file__))
task_queue_script = os.path.join(script_dir, "task_queue.py")

args = sys.argv[1:]
if not args:
    print("Usage: python3 scripts/resolve_task.py <task_id> [--note 'note']", file=sys.stderr)
    sys.exit(1)

cmd = [sys.executable, task_queue_script, "resolve"] + args
os.execv(sys.executable, cmd)
