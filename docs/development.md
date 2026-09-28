# Build and test

Requires Linux, a C++17 compiler, Make, and Qt 6.4+ development libraries
(Core, Gui, Widgets, Network, and Test for the tests). Python 3 is used only by the
source packaging and process smoke-test scripts.

```bash
./bin/build
./bin/test
python3 tests/smoke.py
./build/app/omadraft
```

The tests cover persistence, damaged sessions, failed writes, keyboard handling,
discard confirmation, Markdown formatting, theme replacement, contrast, and restoration.
They use temporary note directories and do not change your real notes or desktop theme.
Update tests cover architecture selection, integrity checks, compatibility failures,
atomic replacement, backup preservation, and simulated GitHub downloads.
Storage tests also cover migration, incoming edits, conflicts, deletion markers,
missing folders, and interrupted writes. To test real pairing and two-way sync,
install Syncthing and run:

```bash
python3 tests/sync_smoke.py
```

This starts two isolated Syncthing instances on localhost with discovery and relays
disabled. It exercises the actual setup dialog without using your Syncthing settings.

To run with isolated data or a specific palette:

```bash
./build/app/omadraft --data-dir /tmp/omadraft-demo --theme-file /path/to/colors.toml
```

## Publishing

The release workflow builds and tests **x86_64 and ARM64** on native GitHub runners.
A `vX.Y.Z` tag matching `VERSION` in `omadraft.pro` creates a **draft GitHub release**
after both builds succeed. Review its assets and publish it to make it visible to
the update button. Creating a draft does not notify users of an available update.

For each release:

1. Push the reviewed source to GitHub.
2. Configure the update source with:
   ```bash
   ./bin/configure-release --repository https://github.com/Mejena-code/omadraft
   ```
   The workflow sets this automatically to its own repository when building releases.
3. Set the version in `omadraft.pro`, commit the changes, and push a matching tag.
4. Review the workflow's draft release. It should include:
   - `omadraft-X.Y.Z-linux-x86_64`
   - `omadraft-X.Y.Z-linux-aarch64`
   - Corresponding `.sha256` files
   - `omadraft-X.Y.Z.tar.gz`, `SHA256SUMS`, and `PKGBUILD`
5. Publish the draft. GitHub supplies a SHA-256 digest for each uploaded asset;
   the app requires that digest before it will install an update.

For manual release preparation, build natively on each architecture, then run
`./bin/binary-release`. Create the source archive with
`./bin/package --repository https://github.com/Mejena-code/omadraft`. Source archives
are reproducible and include the configured update source. Keep the generated
archive and PKGBUILD together: the recipe contains that archive's checksum.

The binaries dynamically link Qt and system libraries. Builds on the GitHub runners
provide a conservative library baseline; test on supported Omarchy installations
before publishing. The local compatibility check rejects a binary that cannot start.

To publish on AUR as well, test `PKGBUILD` in a clean Arch build environment, then
publish it and `makepkg --printsrcinfo` output to the AUR repository. Publishing a
GitHub release does not publish an AUR package automatically.

For a local Arch package before publication:

```bash
./bin/package
cd dist/0.3.1
makepkg -si
```

The package installs the executable, launcher entry, icon, and license. Both x86_64 and aarch64 are built and tested by the release workflow.

Omadraft is an independent community app for Omarchy. Licensed under MIT.

[Back to Omadraft](../README.md)
