#include "update.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLockFile>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <unistd.h>

namespace {
constexpr qint64 maximumBinary = 64 * 1024 * 1024;
QByteArray digest(const QByteArray &bytes) {
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}
bool read(const QString &path, QByteArray &bytes) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    bytes = file.readAll();
    return file.error() == QFile::NoError;
}
bool atomicWrite(const QString &path, const QByteArray &bytes, QFile::Permissions permissions, QString &error) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(permissions) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        error = "Could not install the update: " + file.errorString();
        return false;
    }
    return true;
}
}

Updater::Updater(QString executable, QObject *parent, QNetworkAccessManager *transport)
    : QObject(parent), m_executable(std::move(executable)), m_network(this), m_transport(transport ? transport : &m_network) {}
Updater::~Updater() {
    if (m_reply) {
        m_reply->disconnect(this);
        m_reply->abort();
    }
    for (auto *process : findChildren<QProcess *>(QString(), Qt::FindDirectChildrenOnly)) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            process->kill();
            process->waitForFinished(1000);
        }
    }
}

QString Updater::repository() {
    QByteArray bytes;
    if (!read(":/release.json", bytes)) return {};
    const QString repo = QJsonDocument::fromJson(bytes).object().value("repository").toString();
    static const QRegularExpression pattern("^[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+$");
    return pattern.match(repo).hasMatch() ? repo : QString();
}

QString Updater::architecture() {
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == "x86_64") return "x86_64";
    if (cpu == "arm64" || cpu == "aarch64") return "aarch64";
    return {};
}

bool Updater::selectRelease(const QJsonObject &json, const QString &repo, const QString &arch,
                            UpdateRelease &release, QString &error) {
    static const QRegularExpression versionPattern("^v([0-9]+\\.[0-9]+\\.[0-9]+)$");
    const auto match = versionPattern.match(json.value("tag_name").toString());
    if (!match.hasMatch() || json.value("draft").toBool(true) || json.value("prerelease").toBool(true)) {
        error = "The latest release is not a supported stable version.";
        return false;
    }
    if (arch != "x86_64" && arch != "aarch64") {
        error = "Automatic updates are not available for this architecture.";
        return false;
    }
    release = {};
    release.version = match.captured(1);
    release.architecture = arch;
    const QString name = "omadraft-" + release.version + "-linux-" + arch;
    int matches = 0;
    for (const auto &value : json.value("assets").toArray()) {
        const auto asset = value.toObject();
        if (asset.value("name").toString() != name) continue;
        ++matches;
        const QString expected = "https://github.com/" + repo + "/releases/download/v" + release.version + '/' + name;
        const QString hash = asset.value("digest").toString();
        static const QRegularExpression hashPattern("^sha256:([0-9a-f]{64})$");
        const auto hashMatch = hashPattern.match(hash);
        if (asset.value("browser_download_url").toString() != expected || !hashMatch.hasMatch() ||
            asset.value("state").toString() != "uploaded") {
            error = "The release is missing a verified download. Try again after the release is complete.";
            return false;
        }
        release.url = QUrl(expected);
        release.digest = hashMatch.captured(1).toLatin1();
        release.size = asset.value("size").toVariant().toLongLong();
    }
    if (matches != 1 || release.size < 64 || release.size > maximumBinary) {
        error = "This release has no suitable download for your computer yet.";
        return false;
    }
    return true;
}

bool Updater::validateBinary(const QByteArray &bytes, const UpdateRelease &release, QString &error) {
    if (bytes.size() != release.size || digest(bytes) != release.digest) {
        error = "The download failed its integrity check. Your installed version has been kept.";
        return false;
    }
    const int machine = bytes.size() >= 64 ? quint8(bytes[18]) | (quint8(bytes[19]) << 8) : -1;
    const int expected = release.architecture == "x86_64" ? 62 : release.architecture == "aarch64" ? 183 : -2;
    if (bytes.size() < 64 || bytes.left(4) != QByteArray("\x7f" "ELF", 4) || bytes[4] != 2 || bytes[5] != 1 || machine != expected) {
        error = "The download is not a compatible Linux executable. Your installed version has been kept.";
        return false;
    }
    return true;
}

QString Updater::installationProblem(const QString &path) {
    const QFileInfo executable(path);
    const QFileInfo directory(executable.absolutePath());
    const QString marker = QDir(executable.absolutePath()).filePath("../share/omadraft/install.json");
    QByteArray bytes;
    if (!read(marker, bytes) || QJsonDocument::fromJson(bytes).object().value("kind").toString() != "local")
        return "This installation is managed externally. Update Omadraft through your package manager or its original installer.";
    if (!executable.isFile() || executable.isSymLink() || executable.fileName() != "omadraft" ||
        executable.ownerId() != uint(getuid()) || directory.ownerId() != uint(getuid()) || !directory.isWritable())
        return "This installation cannot be updated by the current user. Use its original installer.";
    if (QFileInfo(path + ".previous").isSymLink())
        return "The update backup path is a symbolic link. Resolve it before updating.";
    return {};
}

bool Updater::replaceExecutable(const QString &path, const QByteArray &originalDigest,
                                const QByteArray &bytes, QString &error) {
    error = installationProblem(path);
    if (!error.isEmpty()) return false;
    QLockFile lock(path + ".update.lock");
    if (!lock.tryLock()) {
        error = "Another update is already running. Try again when it has finished.";
        return false;
    }
    QByteArray original;
    if (!read(path, original) || digest(original) != originalDigest) {
        error = "The installed app changed while this update was being prepared. Close and reopen Omadraft before trying again.";
        return false;
    }
    const auto permissions = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                             QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther | QFile::ExeOther;
    if (!atomicWrite(path + ".previous", original, permissions, error)) return false;
    return atomicWrite(path, bytes, permissions, error);
}

void Updater::setState(State state, const QString &message) {
    m_state = state;
    emit changed(state, message);
}

void Updater::check() {
    if (m_state == Checking || m_state == Installing || m_state == Installed) return;
    const QString pending = QCoreApplication::instance()->property("installedUpdateVersion").toString();
    if (!pending.isEmpty() && m_executable == QCoreApplication::instance()->property("installedExecutable").toString()) {
        setState(Installed, "Version " + pending + " is installed. Restart now, or reopen Omadraft later.");
        return;
    }
    setState(Checking, "Checking your installation…");
    const auto continueCheck = [this] {
        const QString problem = installationProblem(m_executable);
        if (!problem.isEmpty()) { setState(Disabled, problem); return; }
        if (repository().isEmpty()) {
            setState(Disabled, "This build has no update source configured. Updates will be available in an official release build.");
            return;
        }
        fetchRelease();
    };
    if (QStandardPaths::findExecutable("pacman").isEmpty()) { continueCheck(); return; }
    auto *process = new QProcess(this);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            setState(Idle, "Could not check package ownership. Try again.");
        }
    });
    connect(process, &QProcess::finished, this, [this, process, continueCheck](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        if (status != QProcess::NormalExit || (code != 0 && code != 1)) {
            setState(Idle, "Could not check package ownership. Try again.");
        } else if (code == 0) {
            setState(Disabled, "Omadraft was installed through pacman/AUR. Use Omarchy's normal system update to update it.");
        } else continueCheck();
    });
    process->start("pacman", {"-Qqo", m_executable});
    timeout->start(5000);
}

void Updater::fetch(const QUrl &url, qint64 maximum, std::function<void(QByteArray, QString)> done) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Omadraft/" + QCoreApplication::applicationVersion().toUtf8());
    request.setRawHeader("Accept", url.host() == "api.github.com" ? "application/vnd.github+json" : "application/octet-stream");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    auto *reply = m_transport->get(request);
    m_reply = reply;
    reply->setReadBufferSize(64 * 1024);
    auto bytes = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    auto *deadline = new QTimer(reply);
    deadline->setSingleShot(true);
    connect(deadline, &QTimer::timeout, reply, &QNetworkReply::abort);
    deadline->start(120000);
    connect(reply, &QNetworkReply::readyRead, this, [reply, bytes, tooLarge, maximum] {
        bytes->append(reply->readAll());
        if (bytes->size() > maximum) { *tooLarge = true; reply->abort(); }
    });
    connect(reply, &QNetworkReply::finished, this, [reply, bytes, tooLarge, maximum, done] {
        bytes->append(reply->readAll());
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString error;
        if (*tooLarge || bytes->size() > maximum) error = "The update download exceeded its expected size.";
        else if (status == 404) error = "No published update is available yet. Try again after the first GitHub release.";
        else if (status == 403 || status == 429) error = "GitHub's request limit was reached. Please try again later.";
        else if (reply->error() != QNetworkReply::NoError || status != 200)
            error = "Could not download the update information or file. Check your connection and try again.";
        reply->deleteLater();
        done(*bytes, error);
    });
}

void Updater::fetchRelease() {
    setState(Checking, "Checking for updates…");
    fetch(QUrl("https://api.github.com/repos/" + repository() + "/releases/latest"), 2 * 1024 * 1024,
          [this](QByteArray bytes, QString error) {
        if (!error.isEmpty()) { setState(Idle, error); return; }
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (!document.isObject()) { setState(Idle, "GitHub returned invalid update information."); return; }
        if (!selectRelease(document.object(), repository(), architecture(), m_release, error)) {
            setState(Idle, error);
            return;
        }
        const auto current = QVersionNumber::fromString(QCoreApplication::applicationVersion());
        if (QVersionNumber::fromString(m_release.version) <= current) {
            setState(Idle, "You're up to date. Version " + QCoreApplication::applicationVersion() + '.');
            return;
        }
        setState(Available, "Version " + m_release.version + " is available. Your drafts and sync settings will be kept.");
    });
}

void Updater::install() {
    if (m_state != Available) return;
    const QString problem = installationProblem(m_executable);
    if (!problem.isEmpty()) { setState(Disabled, problem); return; }
    QByteArray current;
    if (!read(m_executable, current)) { setState(Idle, "Cannot read the installed app."); return; }
    m_originalDigest = digest(current);
    setState(Installing, "Downloading version " + m_release.version + "…");
    fetch(m_release.url, m_release.size, [this](QByteArray bytes, QString error) {
        if (!error.isEmpty()) { setState(Idle, error); return; }
        if (!validateBinary(bytes, m_release, error)) { setState(Idle, error); return; }
        preflight(bytes);
    });
}

void Updater::preflight(const QByteArray &bytes) {
    setState(Installing, "Checking compatibility with your system…");
    m_stage = std::make_unique<QTemporaryDir>(QFileInfo(m_executable).absolutePath() + "/.omadraft-update-XXXXXX");
    if (!m_stage->isValid()) { setState(Idle, "Cannot create a temporary update directory."); return; }
    const QString binary = m_stage->filePath("omadraft");
    QString error;
    if (!atomicWrite(binary, bytes, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner, error)) {
        setState(Idle, error);
        return;
    }
    auto *process = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    environment.insert("QT_QPA_PLATFORMTHEME", "");
    environment.insert("QT_STYLE_OVERRIDE", "Fusion");
    process->setProcessEnvironment(environment);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            m_stage.reset();
            setState(Idle, "The update could not start on this computer. Update Omarchy and try again. Your installed version was kept.");
        }
    });
    connect(process, &QProcess::finished, this, [this, process, bytes](int code, QProcess::ExitStatus status) {
        const QByteArray output = process->readAllStandardOutput().trimmed();
        process->deleteLater();
        if (code != 0 || status != QProcess::NormalExit || output != "omadraft " + m_release.version.toUtf8()) {
            m_stage.reset();
            setState(Idle, "The update is not compatible with your current system libraries. Update Omarchy and try again. Your installed version was kept.");
            return;
        }
        QString error;
        const bool installed = replaceExecutable(m_executable, m_originalDigest, bytes, error);
        m_stage.reset();
        if (!installed) { setState(Idle, error); return; }
        if (m_executable == QCoreApplication::instance()->property("installedExecutable").toString())
            QCoreApplication::instance()->setProperty("installedUpdateVersion", m_release.version);
        setState(Installed, "Version " + m_release.version + " is installed. Restart now, or keep writing and reopen Omadraft later.");
    });
    process->start(binary, {"--version"});
    timeout->start(10000);
}

UpdateDialog::UpdateDialog(const QString &executable, const Theme &theme, QWidget *parent, QNetworkAccessManager *transport)
    : QDialog(parent), m_updater(executable, this, transport) {
    setObjectName("updateDialog");
    setWindowTitle("Omadraft updates");
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(18);
    auto *version = new QLabel("Omadraft " + QCoreApplication::applicationVersion() + " · " + Updater::architecture(), this);
    layout->addWidget(version);
    m_status = new QLabel("Check GitHub for a newer version of Omadraft.", this);
    m_status->setObjectName("updateStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    m_action = new QPushButton("Check for updates", this);
    m_action->setObjectName("updateAction");
    m_action->setAutoDefault(false);
    layout->addWidget(m_action);
    auto *close = new QPushButton("Close", this);
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(close);
    connect(&m_updater, &Updater::changed, this, [this](Updater::State state, const QString &message) {
        m_status->setText(message);
        m_action->setEnabled(state == Updater::Idle || state == Updater::Available || state == Updater::Installed);
        m_action->setText(state == Updater::Available ? "Update" : state == Updater::Installed ? "Restart now" : "Check for updates");
    });
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (m_updater.state() == Updater::Available) m_updater.install();
        else if (m_updater.state() == Updater::Installed) { accept(); emit restartRequested(); }
        else m_updater.check();
    });
    setTheme(theme);
    resize(480, sizeHint().height());
    QTimer::singleShot(0, &m_updater, &Updater::check);
}

void UpdateDialog::setTheme(const Theme &theme) {
    setStyleSheet(QString("QDialog { background: %1; } QLabel { color: %2; } QPushButton { background: transparent; color: %3; border: 1px solid transparent; border-radius: 5px; padding: 7px 14px; } QPushButton:focus, QPushButton:hover { border-color: %4; color: %4; }")
        .arg(theme.background.name(), theme.foreground.name(), theme.muted.name(), theme.accent.name()));
}
