"""Regenerate patches/xiaozhi-esp32-fork.patch from the working tree of firmware/xiaozhi-esp32.

The fork patch is the difference between "upstream ref + upstream patch" and the current working tree,
new files included. The patch marker used by fetch_repos.py is updated, so the tree is recognised as
fully patched afterwards.

Usage (from anywhere):  python firmware/tools/xiaozhi_fork_patch.py
"""

import json
import os
import subprocess
import sys
import tempfile

FIRMWARE_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.dont_write_bytecode = True  # keep firmware/__pycache__ out of the working tree
sys.path.insert(0, FIRMWARE_DIR)
from fetch_repos import PATCH_MARKER, patch_stack_id  # noqa: E402


def git(repo, args, env=None):
    return subprocess.run(["git", "-C", repo] + args, check=True, capture_output=True, env=env).stdout


def main():
    with open(os.path.join(FIRMWARE_DIR, "repos.json")) as f:
        repo = next(r for r in json.load(f) if r["path"] == "xiaozhi-esp32")

    repo_path = os.path.join(FIRMWARE_DIR, repo["path"])
    upstream_patch = os.path.join(FIRMWARE_DIR, repo["patch"])
    fork_patch = os.path.join(FIRMWARE_DIR, repo["extra_patches"][0])

    with tempfile.TemporaryDirectory() as tmp:
        # Baseline = HEAD + upstream patch, built in a throw-away index; the real index is untouched.
        env = dict(os.environ, GIT_INDEX_FILE=os.path.join(tmp, "index"))
        git(repo_path, ["read-tree", "HEAD"], env)
        git(repo_path, ["apply", "--cached", upstream_patch], env)

        in_baseline = set(git(repo_path, ["ls-files", "-z"], env).decode().split("\0"))
        untracked = git(repo_path, ["ls-files", "--others", "--exclude-standard", "-z"]).decode().split("\0")
        new_files = [p for p in untracked if p and p not in in_baseline]
        if new_files:
            git(repo_path, ["add", "--intent-to-add", "--"] + new_files, env)

        diff = git(repo_path, ["diff", "--no-color", "--no-ext-diff"], env)

    with open(fork_patch, "wb") as f:
        f.write(diff)
    with open(os.path.join(repo_path, PATCH_MARKER), "w") as f:
        f.write(patch_stack_id([upstream_patch, fork_patch]) + "\n")

    files = [line.split(" b/", 1)[-1] for line in diff.decode().splitlines() if line.startswith("diff --git ")]
    print(f"Wrote {os.path.relpath(fork_patch, FIRMWARE_DIR)}: {len(files)} file(s)")
    for name in files:
        print(f"  {name}")


if __name__ == "__main__":
    main()
