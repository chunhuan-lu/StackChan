import hashlib
import json
import os
import subprocess
import sys

# [StackChan fork] Patch handling: a repository may carry several patches ("patch" plus "extra_patches"
# in repos.json), applied in order. The applied stack is recorded in <repo>/.git/stackchan_patches, so
# re-running this script is idempotent and a patch that cannot be applied is reported instead of being
# silently skipped.
PATCH_MARKER = os.path.join(".git", "stackchan_patches")


def patch_stack_id(patch_paths):
    digest = hashlib.sha256()
    for patch_path in patch_paths:
        with open(patch_path, "rb") as f:
            digest.update(f.read())
    return digest.hexdigest()


def has_tracked_changes(path):
    result = subprocess.run(
        ["git", "-C", path, "status", "--porcelain", "--untracked-files=no"],
        check=True,
        capture_output=True,
        text=True,
    )
    return result.stdout.strip() != ""


def apply_patches(path, patch_paths):
    """Apply patch_paths to the repository at path. Returns False on error."""
    if not patch_paths:
        return True

    marker_path = os.path.join(path, PATCH_MARKER)
    stack_id = patch_stack_id(patch_paths)
    if os.path.exists(marker_path):
        with open(marker_path) as f:
            if f.read().strip() == stack_id:
                print(f"Patches already applied to {path}, skipped.")
                return True

    if has_tracked_changes(path):
        print(f"[ERROR] {path} has local changes that do not match the current patch set.")
        print(f"        Reset it and run this script again:")
        print(f"        git -C {path} checkout -- . && git -C {path} clean -fd")
        return False

    for patch_path in patch_paths:
        check = subprocess.run(["git", "-C", path, "apply", "--check", patch_path])
        if check.returncode != 0:
            print(f"[ERROR] Patch {patch_path} cannot be applied to {path}.")
            return False
        subprocess.run(["git", "-C", path, "apply", patch_path], check=True)
        print(f"Applied patch {patch_path} to {path}")

    with open(marker_path, "w") as f:
        f.write(stack_id + "\n")
    return True


def clone_or_update_repo(repo_url, path, ref=None, with_submodules=False, patch_paths=None):
    if not os.path.exists(path):
        subprocess.run(["git", "clone", repo_url, path], check=True)
    else:
        subprocess.run(["git", "-C", path, "fetch"], check=True)

    if ref:
        subprocess.run(["git", "-C", path, "checkout", ref], check=True)

    if with_submodules:
        subprocess.run(
            ["git", "-C", path, "submodule", "update", "--init", "--recursive"],
            check=True,
        )

    return apply_patches(path, patch_paths or [])


def fetch_dependencies():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    config_path = os.path.join(script_dir, "repos.json")

    with open(config_path) as f:
        repos = json.load(f)

    ok = True
    for repo in repos:
        repo_path = os.path.join(script_dir, repo["path"])
        branch = repo.get("branch")
        with_submodules = repo.get("with_submodules", False)
        patches = ([repo["patch"]] if repo.get("patch") else []) + repo.get("extra_patches", [])
        patches = [p if os.path.isabs(p) else os.path.join(script_dir, p) for p in patches]
        ok = clone_or_update_repo(repo["url"], repo_path, branch, with_submodules, patches) and ok

    if not ok:
        sys.exit(1)


if __name__ == "__main__":
    fetch_dependencies()
