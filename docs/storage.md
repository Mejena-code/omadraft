# Where notes live

Each draft is an ordinary UTF-8 Markdown file with a stable ID:

```text
~/.local/share/omadraft/drafts/<id>.md
```

With `XDG_DATA_HOME` set, the base directory is `$XDG_DATA_HOME/omadraft`.
There is no folder picker: Omadraft manages this folder automatically.

`session.json` and `session.json.bak` outside the drafts folder hold local recovery
snapshots, active note, cursor, selection, scrolling, and window geometry.
Those files stay on each computer. Back up the **whole Omadraft data directory**
to preserve both drafts and local state; share only `drafts/` for synchronization.

Changes are saved at most 200 ms after an edit under normal conditions, and flushed
when the app loses focus or closes. Writes use atomic replacement. A small local
transaction file protects interrupted saves. External changes are checked every
second while Omadraft is open. Editing and startup never wait for Syncthing or a network.
A forced termination can still lose the most recent unsaved fraction of a second.

On the first launch after upgrading from session-only storage, existing drafts are
migrated automatically. `session.before-sync.json` retains the original session.
If a session is damaged, Omadraft tries the backup and preserves the damaged file;
Markdown files remain independent of that local snapshot. If saving fails, the
app shows an error and refuses a normal close until saving succeeds.

Discarding a draft writes a small `.deleted` marker, allowing other computers to
recognize the deletion even if they have been offline. Keep these markers in the
shared folder. Recovery snapshots can retain discarded text; discard is not secure
erasure. Remove notes through Omadraft, rather than manually deleting their files.


[Back to Omadraft](../README.md)
