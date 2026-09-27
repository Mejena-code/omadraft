# Optional sync with Syncthing

**Syncthing is optional.** Without it, Omadraft works entirely locally. Installing
Omadraft does not install Syncthing, start a service, or share any notes.

### Set up two computers

1. Install Omadraft and Syncthing on both computers. On Omarchy:
   ```bash
   omarchy pkg add syncthing
   ```
2. In Omadraft, press **Ctrl+H**, then choose **Set up sync…**.
3. If Syncthing is not running, choose **Start Syncthing and enable at login**.
4. Choose the other computer from the list. If it is not listed, copy its device ID
   from the same dialog on that computer and paste it here.
5. Choose **Enable sync with this computer**. Repeat on the other computer, using
   this computer's ID. Each side must agree to the pairing.

Omadraft fills in the folder path, uses the same folder ID on both computers, and
shares it with the selected device. Existing shares are retained. Repeating the
setup does not create duplicate folders. If an existing Syncthing folder conflicts
with the path or ID, the dialog explains the issue and keeps that configuration.

You can also open this dialog with:

```bash
omadraft --setup-sync
```

It opens in the existing Omadraft instance if one is already running.
The setup uses Syncthing's authenticated local API; credentials are not saved by
Omadraft. Automatic setup supports the usual local HTTP interface. For customized
HTTPS or remote interfaces, configure the folder in Syncthing manually:

| Setting | Value |
| --- | --- |
| Folder label | `Omadraft` |
| Folder ID on every device | `omadraft-drafts-v1` |
| Folder path | `~/.local/share/omadraft/drafts` (or your XDG data path) |
| Folder type | Send & Receive |
| File watcher | Enabled |
| Maximum conflicts (`maxConflicts`, advanced setting) | `-1` (keep all) |

Do not exclude `.deleted` files. Share only with devices that should have access
to your drafts. The built-in setup targets trusted computers that edit notes, not
Syncthing's encrypted storage-only devices.

### Everyday use and conflicts

Write normally. Notes save locally whether or not Syncthing is running. When
Syncthing transfers a change, open Omadraft windows pick it up automatically.
The Syncthing browser page does not need to stay open.

Tab numbers use a shared, deterministic order once both computers have received
all drafts. Newly created notes use creation IDs; older notes are ordered by their
existing IDs. Your active note stays selected when numbers change. Deletion always
identifies the draft by its unique ID, never by its tab number. Empty drafts are
also real notes, so an empty note from a second computer appears on both.

If both computers edit a draft before synchronizing, Omadraft preserves conflicting
text as separate numbered notes. Hover over a recovered note's number to see its
conflict label. Review both versions and discard the one you no longer need. An
offline edit that arrives after a discard is also recovered separately. This is
file synchronization, not collaborative character-by-character editing.

Both computers need to be online at the same time for a direct exchange. To sync
at different times, add an always-on third computer or server running ordinary
Syncthing and share the same folder with it. Syncthing remains a separate service.
To stop sharing, use its web interface (normally <http://localhost:8384>) and remove
the relevant device from the Omadraft folder's sharing settings. Local drafts stay.
Synchronization propagates deletions too; keep separate backups for long-term recovery.

Only one running instance may use a note directory. Launching Omadraft again
focuses the existing window. On Hyprland, this also brings its workspace into view.


[Back to Omadraft](../README.md)
