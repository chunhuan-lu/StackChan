# CLAUDE.md

Instructions for Claude Code working in this repository (fork of m5stack/StackChan, base v1.6.0, focus: AI Agent / xiaozhi mode).
Full rules: [docs/CONVENTIONS.md](docs/CONVENTIONS.md).

## Language

- Reply to the maintainer in **Chinese**.
- Write all code, comments, identifiers, log strings, Kconfig help and commit messages in **English**.
- Every doc under `docs/` exists twice: `name.md` (English) and `name.zh-CN.md` (Chinese). Update both together.

## Workflow

- Work on `feat/*` / `fix/*` branches from `main` (the fork; upstream is `upstream/main`); merge back with `--no-ff`
  only after it builds and is verified on the device, and ask before pushing. Conventional Commits.
- Keep the upstream diff small: new files, small marked hooks, Kconfig switches prefixed `SC_`.
- xiaozhi changes go into `firmware/patches/xiaozhi-esp32-fork.patch`, never into upstream's `xiaozhi-esp32.patch`.
- Build with ESP-IDF **v5.5.4** before every commit. Record on-device verification in `docs/devlog/`; never claim
  "done" for untested behaviour.
- Update `docs/backlog*.md` when an item changes state.

## Device safety

- Flash with `idf.py -p COMx flash` only (keeps NVS). No `erase-flash` / full-image flashing without a fresh backup.
- Do not suggest unbinding the device in the StackChan World app. Keep xiaozhi automatic OTA disabled.
- Pitch servo stays within 5-85 degrees.

## Secrets

- Never commit tokens, passwords, Wi-Fi credentials, flash dumps, `sdkconfig` or `sdkconfig.defaults.local`.
