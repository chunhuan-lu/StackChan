# Development Conventions

**English** | [中文](CONVENTIONS.zh-CN.md)

Scope: this fork ([chunhuan-lu/StackChan](https://github.com/chunhuan-lu/StackChan)), based on the official
[m5stack/StackChan](https://github.com/m5stack/StackChan) v1.6.0 firmware and focused on the **AI Agent (xiaozhi) mode**.

## 1. Language

- Discussion with the maintainer: Chinese.
- Code, comments, identifiers, log strings, Kconfig help texts and commit messages: **English only**.
- Docs (requirements, backlog, dev logs, design notes, these conventions): **two files per doc** —
  `name.md` (English) and `name.zh-CN.md` (Chinese). Update both in the same commit; each file links to the other at the top.
  Tool instruction files such as `CLAUDE.md` are English only.

## 2. Branches and commits

| Branch | Purpose |
|---|---|
| `main` | Mirror of upstream `m5stack/StackChan`. Never commit to it directly. |
| `dev` | Integration branch of this fork. |
| `feat/<topic>`, `fix/<topic>`, `docs/<topic>` | Work branches created from `dev`, merged back into `dev` once built and verified. |

- Commit messages follow [Conventional Commits](https://www.conventionalcommits.org/): `type(scope): summary`.
  Types: `feat`, `fix`, `docs`, `refactor`, `build`, `test`, `chore`. Scopes: `firmware`, `xiaozhi`, `docs`, `tools`, ...
- Syncing an upstream release:
  ```
  git fetch upstream
  git switch main && git merge --ff-only upstream/main && git push origin main
  git switch dev && git merge main      # then rebuild and re-check the xiaozhi patches
  ```
- Remotes: `origin` = this fork, `upstream` = official (push disabled), `artrobin` = reference only (push disabled).

## 3. Keep the diff to upstream small

- Put new features in **new files**; touch upstream files only for small, clearly marked hooks.
- Gate fork features behind Kconfig options whose symbols start with **`SC_`** (`CONFIG_SC_...`), so each feature can be switched off.
- Prefer `sdkconfig.defaults` changes over code changes when a setting is enough.

## 4. Changes to xiaozhi-esp32

- Never edit upstream's `firmware/patches/xiaozhi-esp32.patch`.
- Fork changes to xiaozhi go into a separate **`firmware/patches/xiaozhi-esp32-fork.patch`**, applied after upstream's patch
  (`fetch_repos.py` gets multi-patch support the first time this is needed).
- `fetch_repos.py` re-checks out the xiaozhi sources: edits in `firmware/xiaozhi-esp32/` that are not saved to the patch are lost.
- If the xiaozhi tree already carries an old patch, `fetch_repos.py` silently skips the new one. Reset first:
  `git -C firmware/xiaozhi-esp32 checkout -- .`

## 5. Hardware and device safety

- Pitch (Y) servo: keep within 5–85° (firmware clamps to 3–87°). Never rotate moving parts by hand while the motors may be powered.
- Keep idle servo movement infrequent (SCS0009 potentiometer life ≥ 100k cycles); let the on-screen eyes do most of the "looking".
- Flash with `idf.py -p COMx flash` only. It keeps NVS (Wi-Fi, servo calibration, xiaozhi device identity).
  No `erase-flash` or full-image flashing without a fresh full flash backup.
- Do not unbind the device in the StackChan World app: self-built firmware relies on the existing xiaozhi binding.
- Self-built firmware must keep xiaozhi automatic OTA disabled, otherwise an official release overwrites it.

## 6. Verification

- Every commit must build with **ESP-IDF v5.5.4** (`idf.py build`); v5.5.5 is known to fail (upstream issue #108).
- For anything that needs the device, the dev log records what was tested, how, and the result.
  Nothing is called "done" without on-device verification; untested parts are marked as untested.

## 7. Secrets

- Never commit tokens, passwords, keys, Wi-Fi credentials, flash dumps, `sdkconfig` or `sdkconfig.defaults.local`.
- Local secrets go into `firmware/sdkconfig.defaults.local` (git-ignored) or environment variables.
- A local (unversioned) pre-commit hook blocks common secret patterns in staged changes.

## 8. Platform notes

- Windows: use `core.autocrlf=false` for this repo; `.gitattributes` keeps `*.patch` byte-exact (upstream issue #62).

## Docs layout

```
docs/
  CONVENTIONS.md / CONVENTIONS.zh-CN.md
  backlog.md / backlog.zh-CN.md
  requirements/<id>-<name>.md / .zh-CN.md
  devlog/<YYYY-MM-DD>.md / .zh-CN.md
```
