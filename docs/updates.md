# Updates

Open **Alt+H → Check for updates…**. Omadraft checks the latest stable GitHub
release only when you request it; normal startup never waits for the network.

For local installations made with `bin/install`, choose **Update** to download the
correct Linux executable for your computer (`x86_64` or `aarch64`). Omadraft checks
its SHA-256 against the digest provided by GitHub over HTTPS, checks the executable's
architecture, and runs a short compatibility check before replacing anything.
This uses GitHub as the trusted distribution source; it is not an independent
publisher-signature system.

The installed executable is replaced atomically. Its previous version is retained
as `omadraft.previous` beside it. Failed downloads or compatibility checks leave
the existing app in place. If newer system libraries are needed, update Omarchy
and try again. Drafts, local session data, and Syncthing configuration are untouched
by the installer.

After installation, choose **Restart now**, or keep writing and reopen the app
later. Restart saves your current drafts first and replaces only this app process.
The updater refreshes the executable; the launcher and icon installed by `bin/install`
remain in place.

For **pacman/AUR installations**, the dialog directs you to Omarchy's normal system
update. Omadraft never replaces a package-owned executable itself. Custom or
unrecognized installations should be updated through their original installer.

Updates use the public repository configured in `assets/release.json`. Only stable, published releases are offered. Drafts and prereleases are excluded.

You can also update a local checkout with `git pull --ff-only` and `./bin/install`.
To uninstall a local installation, run `./bin/uninstall`. Saved notes are kept.
For a custom installation prefix, use `PREFIX=/your/prefix ./bin/install`.


[Back to Omadraft](../README.md)

## Repository move in 0.4.1

Versions 0.4.0 and earlier use the previous repository address and need a one-time
manual reinstall after the move to `Mejena-code/omadraft`. In your source checkout,
run `git remote set-url origin https://github.com/Mejena-code/omadraft.git`,
then `git pull --ff-only && ./bin/install`. Close and reopen the app afterward.
Your notes are kept; subsequent updates can use the Help window again.
