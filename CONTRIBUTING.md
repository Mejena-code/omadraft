# Contributing

Keep Omadraft small, quick to open, and focused on writing. Discuss new visible
features before adding them. All user-facing text, code comments, documentation,
and commit messages are in English.

## Development

- Build with `./bin/build`.
- Run `./bin/test` and `python3 tests/smoke.py` after changing behavior.
- Use `--data-dir` for manual testing so your own notes stay separate.
- Keep Markdown as plain text in storage.
- Keep file writes atomic and handle failures visibly.
- Run `python3 tests/sync_smoke.py` after changing sync behavior; never test with personal notes.
- Keep update checks manual, validate downloads, and respect package ownership.
- Keep Syncthing optional. Only change its configuration after an explicit setup action.
- Preserve keyboard focus, selection, cursor position, and undo while switching notes.
- Test both light and dark palettes. Text and keyboard hints must remain readable.
- Do not put generated binaries, personal notes, or local settings in the repository.

## Layout

- `src/session.*`: Markdown files, local sessions, reconciliation, migration, and recovery.
- `src/sync.*`: optional Syncthing setup through the local API.
- `src/update.*`: manual GitHub update checks, verification, compatibility checks, and installation.
- `src/theme.*`: Omarchy palette loading, live updates, and contrast.
- `src/markdown.*`: Markdown source styling.
- `src/window.*`: editor, numbered notes, hints, and discard dialog.
- `src/main.cpp`: startup, one-instance behavior, and shutdown handling.
- `tests/`: temporary-data UI, storage, theme, and process tests.
- `bin/`: build, test, install, uninstall, and source packaging tools.
- `packaging/`: Arch package template.

## Before a release

Run the automated checks, test the app on a real Wayland session, build the Arch
package in a clean environment, and verify a fresh installation and an upgrade.
Confirm that a theme change and launching a second instance work on the supported
Omarchy version. Update README installation examples when the public repository
and AUR package exist.
