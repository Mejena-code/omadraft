#include "session.h"

#include <QDir>
#include <QDateTime>
#include <algorithm>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <utility>

Note Note::blank() {
    Note note;
    // Sortable creation IDs keep new notes after existing notes on every device.
    static qint64 lastCreated = 0;
    lastCreated = qMax(QDateTime::currentMSecsSinceEpoch(), lastCreated + 1);
    note.id = "note-" + QString::number(lastCreated) + '-' + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return note;
}

SessionStore::SessionStore(QString directory) : m_directory(std::move(directory)) {}

QByteArray SessionStore::encode(const Session &session) {
    QJsonArray notes;
    for (const Note &note : session.notes) {
        notes.append(QJsonObject{{"id", note.id}, {"text", note.text},
                                 {"cursor", note.cursor}, {"anchor", note.anchor},
                                 {"scroll", note.scroll}});
    }
    return QJsonDocument(QJsonObject{{"version", 1}, {"active", session.active},
                                     {"geometry", QString::fromLatin1(session.geometry.toBase64())},
                                     {"notes", notes}}).toJson(QJsonDocument::Compact);
}

bool SessionStore::decode(const QByteArray &bytes, Session &session, QString &error) {
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    const QJsonObject object = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        error = QStringLiteral("The saved session is not valid JSON.");
        return false;
    }
    if (object.value("version").toInt(-1) != 1) {
        error = QStringLiteral("This session uses an unsupported format. Keep it and use a compatible version of Omadraft.");
        return false;
    }
    if (!object.value("notes").isArray() || object.value("notes").toArray().isEmpty()) {
        error = QStringLiteral("The saved session has no valid note list.");
        return false;
    }
    Session decoded;
    QSet<QString> ids;
    for (const QJsonValue &value : object.value("notes").toArray()) {
        const QJsonObject item = value.toObject();
        if (!value.isObject() || !item.value("text").isString() ||
            !item.value("id").isString() || item.value("id").toString().isEmpty() ||
            ids.contains(item.value("id").toString())) {
            error = QStringLiteral("The saved session contains an invalid note.");
            return false;
        }
        Note note;
        note.id = item.value("id").toString();
        note.text = item.value("text").toString();
        note.cursor = qBound(0, item.value("cursor").toInt(), int(note.text.size()));
        note.anchor = qBound(0, item.value("anchor").toInt(note.cursor), int(note.text.size()));
        note.scroll = qMax(0, item.value("scroll").toInt());
        ids.insert(note.id);
        decoded.notes.append(note);
    }
    decoded.active = qBound(0, object.value("active").toInt(), int(decoded.notes.size()) - 1);
    decoded.geometry = QByteArray::fromBase64(object.value("geometry").toString().toLatin1());
    session = std::move(decoded);
    return true;
}

bool SessionStore::load(Session &session, QString &error, QString &notice) {
    m_canSave = false;
    m_baseline.clear();
    m_recovered = false;
    m_lastGood.clear();
    error.clear();
    notice.clear();
    if (!QDir().mkpath(m_directory)) {
        error = QStringLiteral("Cannot create the note directory: %1").arg(m_directory);
        return false;
    }
    QFile::setPermissions(m_directory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString primary = m_directory + "/session.json";
    const QString backup = primary + ".bak";
    if (!QFileInfo::exists(primary) && !QFileInfo::exists(backup)) {
        session = Session{{Note::blank()}, 0, {}};
        m_canSave = true;
        return initializeDrafts(session, error);
    }
    for (const QString &path : {primary, backup}) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            error = file.errorString();
            continue;
        }
        const QByteArray bytes = file.readAll();
        // A newer format must never be replaced with an older backup.
        const QJsonDocument json = QJsonDocument::fromJson(bytes);
        if (json.isObject() && json.object().value("version").toInt(1) != 1) {
            error = QStringLiteral("This session was created by an incompatible version of Omadraft. Your files have been kept.");
            return false;
        }
        if (!decode(bytes, session, error))
            continue;
        if (path == backup && QFileInfo::exists(primary)) {
            const QString preserved = primary + ".damaged-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
            if (!QFile::rename(primary, preserved)) {
                error = QStringLiteral("Cannot preserve the damaged session. Your files have been kept in %1.").arg(m_directory);
                return false;
            }
        }
        m_lastGood = bytes;
        m_canSave = true;
        m_recovered = path == backup;
        if (m_recovered)
            notice = QStringLiteral("Your notes were restored from the last backup. Any damaged file was kept in %1.").arg(m_directory);
        error.clear();
        return initializeDrafts(session, error);
    }
    error = QStringLiteral("Cannot open your saved notes. Your files have been kept in %1.\n\n%2")
                .arg(m_directory, error);
    return false;
}

bool SessionStore::writeAtomic(const QString &path, const QByteArray &bytes, QString &error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        error = file.errorString();
        return false;
    }
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        error = file.errorString();
        return false;
    }
    return true;
}

bool SessionStore::saveSnapshot(const Session &session, QString &error) {
    error.clear();
    if (!m_canSave) {
        error = QStringLiteral("The session has not been loaded safely.");
        return false;
    }
    const QByteArray bytes = encode(session);
    if (bytes == m_lastGood && !m_recovered)
        return true;
    const QString primary = m_directory + "/session.json";
    // Keep the previous successful snapshot, never an unvalidated file from disk.
    if (!m_lastGood.isEmpty() && !writeAtomic(primary + ".bak", m_lastGood, error))
        return false;
    if (!writeAtomic(primary, bytes, error))
        return false;
    m_lastGood = bytes;
    m_recovered = false;
    return true;
}


namespace {
bool safeId(const QString &id) {
    static const QRegularExpression pattern("^[a-zA-Z0-9][a-zA-Z0-9._-]{0,180}$");
    return pattern.match(id).hasMatch();
}
QString textHash(const QString &text) {
    return QString::fromLatin1(QCryptographicHash::hash(text.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString recoveredId(const QString &id, const QString &text) {
    return "recovered-" + textHash(id + '\n' + text);
}
}

bool SessionStore::initializeDrafts(Session &session, QString &error) {
    const QString marker = m_directory + "/drafts-version";
    const bool migrated = QFileInfo::exists(marker);
    if (migrated) {
        QFile file(marker);
        if (!file.open(QIODevice::ReadOnly) || file.readAll() != "1\n") {
            error = "Unsupported drafts format. Your files have been kept.";
            m_canSave = false;
            return false;
        }
        if (!QFileInfo(draftsDirectory()).isDir()) {
            error = "The drafts folder is missing. Restore it before opening Omadraft: " + draftsDirectory();
            m_canSave = false;
            return false;
        }
        for (const Note &note : session.notes)
            m_baseline.insert(note.id, note.text);
    } else {
        if (!QDir().mkpath(draftsDirectory())) {
            error = "Cannot create the drafts folder: " + draftsDirectory();
            return false;
        }
        if (!m_lastGood.isEmpty() && !QFileInfo::exists(m_directory + "/session.before-sync.json") &&
            !writeAtomic(m_directory + "/session.before-sync.json", m_lastGood, error))
            return false;
        // A new installation receiving existing drafts needs no extra empty note.
        if (m_lastGood.isEmpty() && !QDir(draftsDirectory()).entryList({"*.md"}, QDir::Files).isEmpty())
            session.notes.clear();
    }
    QFile pending(m_directory + "/pending-save.json");
    if (pending.exists()) {
        if (!pending.open(QIODevice::ReadOnly)) {
            error = "Cannot read the interrupted save. Your files have been kept.";
            return false;
        }
        const QJsonObject transaction = QJsonDocument::fromJson(pending.readAll()).object();
        Session interrupted;
        if (!transaction.value("baseline").isObject() ||
            !decode(QJsonDocument(transaction.value("session").toObject()).toJson(), interrupted, error)) {
            error = "Invalid interrupted save. Your files have been kept.";
            return false;
        }
        m_baseline.clear();
        const auto baseline = transaction.value("baseline").toObject();
        for (auto it = baseline.begin(); it != baseline.end(); ++it) {
            if (!safeId(it.key()) || !it.value().isString()) {
                error = "Invalid interrupted save baseline.";
                return false;
            }
            m_baseline.insert(it.key(), it.value().toString());
        }
        session = std::move(interrupted);
    }
    if (!synchronize(session, error))
        return false;
    return migrated || writeAtomic(marker, "1\n", error);
}

bool SessionStore::save(Session &session, QString &error) {
    return synchronize(session, error);
}

bool SessionStore::synchronize(Session &session, QString &error) {
    error.clear();
    if (!m_canSave) {
        error = "The session has not been loaded safely.";
        return false;
    }
    const QDir folder(draftsDirectory());
    if (!folder.exists()) {
        error = "The drafts folder is unavailable: " + folder.path();
        return false;
    }
    QMap<QString, QString> remote;
    QMap<QString, QString> deleted;
    for (const QFileInfo &info : folder.entryInfoList({"*.md", "*.deleted"}, QDir::Files | QDir::Hidden, QDir::Name)) {
        const QString id = info.completeBaseName();
        if (!safeId(id) || info.isSymLink()) {
            error = "Unsupported draft file: " + info.fileName();
            return false;
        }
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly)) {
            error = "Cannot read " + info.fileName() + ": " + file.errorString();
            return false;
        }
        const QByteArray bytes = file.readAll();
        if (file.error() != QFile::NoError) {
            error = file.errorString();
            return false;
        }
        if (info.suffix() == "deleted") {
            const QString hash = QString::fromLatin1(bytes.trimmed());
            static const QRegularExpression hashPattern("^[0-9a-f]{64}$");
            if (!hashPattern.match(hash).hasMatch()) {
                error = "Invalid deletion marker: " + info.fileName();
                return false;
            }
            deleted.insert(id, hash);
        } else {
            QStringDecoder decoder(QStringDecoder::Utf8);
            const QString text = decoder(bytes);
            if (decoder.hasError()) {
                error = "This draft is not UTF-8: " + info.fileName();
                return false;
            }
            remote.insert(id, text);
        }
    }
    QMap<QString, Note> local;
    for (const Note &note : session.notes) {
        if (!safeId(note.id)) {
            error = "Invalid draft ID. Your original session has been kept.";
            return false;
        }
        local.insert(note.id, note);
    }
    // Keep the merge base with a pending save so a crash cannot turn local edits
    // into an apparently unmodified snapshot on the next launch.
    bool textChanged = local.size() != m_baseline.size();
    for (auto it = local.cbegin(); it != local.cend(); ++it)
        if (!m_baseline.contains(it.key()) || m_baseline[it.key()] != it.value().text) textChanged = true;
    if (textChanged && !session.notes.isEmpty()) {
        QJsonObject baseline;
        for (auto it = m_baseline.cbegin(); it != m_baseline.cend(); ++it) baseline[it.key()] = it.value();
        const QJsonObject transaction{{"session", QJsonDocument::fromJson(encode(session)).object()}, {"baseline", baseline}};
        if (!writeAtomic(m_directory + "/pending-save.json", QJsonDocument(transaction).toJson(), error)) return false;
    }
    // Protect unsaved local edits before changing any shared files.
    if (!session.notes.isEmpty() && !saveSnapshot(session, error))
        return false;
    for (auto it = m_baseline.cbegin(); it != m_baseline.cend(); ++it) {
        if (!local.contains(it.key()) && !deleted.contains(it.key())) {
            const QString hash = textHash(it.value());
            if (!writeAtomic(folder.filePath(it.key() + ".deleted"), hash.toLatin1() + '\n', error))
                return false;
            deleted.insert(it.key(), hash);
        }
    }
    QMap<QString, Note> merged;
    auto recover = [&](const QString &id, const QString &text) {
        Note copy;
        copy.id = recoveredId(id, text);
        copy.text = text;
        // A discarded recovery must not be resurrected by a delayed original.
        if (!deleted.contains(copy.id))
            merged.insert(copy.id, copy);
    };
    QSet<QString> ids;
    for (auto it = local.cbegin(); it != local.cend(); ++it) ids.insert(it.key());
    for (auto it = remote.cbegin(); it != remote.cend(); ++it) ids.insert(it.key());
    for (const QString &id : ids) {
        if (deleted.contains(id)) {
            if (local.contains(id) && textHash(local[id].text) != deleted[id])
                recover(id, local[id].text);
            if (remote.contains(id) && textHash(remote[id]) != deleted[id])
                recover(id, remote[id]);
            continue;
        }
        if (!local.contains(id)) {
            Note note;
            note.id = id;
            note.text = remote[id];
            merged.insert(id, note);
        } else {
            Note note = local[id];
            const bool edited = !m_baseline.contains(id) || m_baseline[id] != note.text;
            if (remote.contains(id) && remote[id] != note.text) {
                if (edited) {
                    if (!m_baseline.contains(id) || remote[id] != m_baseline[id])
                        recover(id, remote[id]);
                } else {
                    note.text = remote[id];
                    note.cursor = qMin(note.cursor, int(note.text.size()));
                    note.anchor = qMin(note.anchor, int(note.text.size()));
                }
            }
            merged.insert(id, note);
        }
    }
    Session result;
    result.geometry = session.geometry;
    const QString activeId = session.notes.isEmpty() ? QString() : session.notes[qBound(0, session.active, int(session.notes.size()) - 1)].id;
    for (const Note &note : std::as_const(merged)) result.notes.append(note);
    const auto orderKey = [](const Note &note) {
        // Older UUID-based notes have no creation timestamp. Their IDs still
        // provide a consistent order; recovery copies follow regular notes.
        const QString group = note.id.startsWith("recovered-") ? "2" :
                              note.id.startsWith("note-") ? "1" : "0";
        return group + note.id;
    };
    std::sort(result.notes.begin(), result.notes.end(), [&](const Note &a, const Note &b) {
        return orderKey(a) < orderKey(b);
    });
    for (int i = 0; i < result.notes.size(); ++i)
        if (result.notes[i].id == activeId) result.active = i;
    if (result.notes.isEmpty()) result.notes.append(Note::blank());
    for (const Note &note : std::as_const(result.notes)) {
        if (!remote.contains(note.id) || remote[note.id] != note.text) {
            if (!writeAtomic(folder.filePath(note.id + ".md"), note.text.toUtf8(), error))
                return false;
        }
    }
    // Markers arrive independently of Markdown files; they remain to suppress stale copies.
    for (auto it = deleted.cbegin(); it != deleted.cend(); ++it) {
        const QString path = folder.filePath(it.key() + ".md");
        if (QFileInfo::exists(path) && !QFile::remove(path)) {
            error = "Cannot remove discarded draft: " + path;
            return false;
        }
    }
    if (!saveSnapshot(result, error))
        return false;
    const QString pending = m_directory + "/pending-save.json";
    if (QFileInfo::exists(pending) && !QFile::remove(pending)) {
        error = "Cannot finish the pending save: " + pending;
        return false;
    }
    m_baseline.clear();
    for (const Note &note : std::as_const(result.notes)) m_baseline.insert(note.id, note.text);
    session = std::move(result);
    return true;
}
