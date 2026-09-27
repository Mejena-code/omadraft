#include "sync.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkRequest>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QXmlStreamReader>

SyncthingClient::SyncthingClient(QObject *parent) : QObject(parent), m_network(this) {
    m_network.setProxy(QNetworkProxy::NoProxy);
}

void SyncthingClient::discover(std::function<void(const QString &)> done) {
    m_key.clear();
    if (QStandardPaths::findExecutable("syncthing").isEmpty()) {
        done("Install Syncthing first: omarchy pkg add syncthing. Then choose Refresh.");
        return;
    }
    auto *process = new QProcess(this);
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, &QProcess::kill);
    connect(process, &QProcess::errorOccurred, this, [process, done](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            process->deleteLater();
            done("Could not run Syncthing. Check that it is installed, then choose Refresh.");
        }
    });
    connect(process, &QProcess::finished, this, [this, process, timeout, done](int code) {
        const QString output = QString::fromUtf8(process->readAllStandardOutput());
        // Syncthing 1.x exposes this operation as a flag; 2.x uses a subcommand.
        if (code != 0 && process->exitStatus() == QProcess::NormalExit && !process->property("legacyPaths").toBool()) {
            process->setProperty("legacyPaths", true);
            process->start("syncthing", {"--paths"});
            timeout->start(5000);
            return;
        }
        process->deleteLater();
        const auto match = QRegularExpression("Configuration file:\\s*\\n\\s*([^\\n]+)").match(output);
        if (code != 0 || !match.hasMatch()) {
            done("Could not locate Syncthing's configuration. Start Syncthing, then choose Refresh.");
            return;
        }
        QFile file(match.captured(1).trimmed());
        if (!file.open(QIODevice::ReadOnly)) {
            done("Syncthing has not been initialized. Choose Start Syncthing, then try again.");
            return;
        }
        QXmlStreamReader xml(&file);
        QString address;
        bool gui = false, tls = false;
        while (!xml.atEnd()) {
            xml.readNext();
            if (xml.isStartElement() && xml.name() == u"gui") {
                gui = true;
                tls = xml.attributes().value("tls") == u"true";
            } else if (xml.isEndElement() && xml.name() == u"gui") {
                gui = false;
            } else if (gui && xml.isStartElement()) {
                if (xml.name() == u"address") address = xml.readElementText();
                else if (xml.name() == u"apikey") m_key = xml.readElementText().toUtf8();
            }
        }
        m_url = QUrl("http://" + address);
        if (m_url.host() == "0.0.0.0") m_url.setHost("127.0.0.1");
        if (m_url.host() == "::") m_url.setHost("::1");
        const bool loopback = m_url.host() == "localhost" || QHostAddress(m_url.host()).isLoopback();
        if (xml.hasError() || m_key.isEmpty() || !m_url.isValid() || !loopback || tls) {
            m_key.clear();
            done("Automatic setup needs a local HTTP Syncthing interface. For custom HTTPS or remote setups, add the drafts folder in Syncthing manually. Your current settings were kept.");
            return;
        }
        done({});
    });
    process->start("syncthing", {"paths"});
    timeout->start(5000);
}

void SyncthingClient::request(const QByteArray &method, const QString &path, const QJsonObject &body, Callback done) {
    if (m_key.isEmpty()) {
        done({}, "Syncthing is not connected. Choose Refresh.");
        return;
    }
    QUrl url = m_url;
    url.setPath("/rest/" + path);
    QNetworkRequest request(url);
    request.setRawHeader("X-API-Key", m_key);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(8000);
    const QByteArray bytes = body.isEmpty() ? QByteArray() : QJsonDocument(body).toJson(QJsonDocument::Compact);
    auto *reply = m_network.sendCustomRequest(request, method, bytes);
    connect(reply, &QNetworkReply::finished, this, [reply, done] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = reply->readAll();
        const auto networkError = reply->error();
        reply->deleteLater();
        if (networkError != QNetworkReply::NoError || status < 200 || status >= 300) {
            done({}, status ? QString("Syncthing rejected the request (HTTP %1). Check the device ID and folder settings, then try again.").arg(status)
                            : "Cannot reach Syncthing. Start it, then choose Refresh.");
            return;
        }
        QJsonParseError error;
        const QJsonDocument json = QJsonDocument::fromJson(data, &error);
        if (!data.trimmed().isEmpty() && error.error != QJsonParseError::NoError) {
            done({}, "Syncthing returned an invalid response.");
            return;
        }
        done(json, {});
    });
}

QString SyncthingClient::normalizeDeviceId(QString id) {
    id = id.trimmed().toUpper();
    id.remove('-');
    id.remove(' ');
    static const QRegularExpression pattern("^[A-Z2-7]{56}$");
    if (!pattern.match(id).hasMatch()) return {};
    QStringList groups;
    for (int i = 0; i < id.size(); i += 7) groups << id.mid(i, 7);
    return groups.join('-');
}

bool SyncthingClient::prepareFolder(const QJsonArray &folders, const QJsonObject &defaults,
                                   const QString &directory, const QString &local, const QString &peer,
                                   QJsonObject &folder, QString &error) {
    const auto resolvedPath = [](const QString &path) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        return canonical.isEmpty() ? QDir(path).absolutePath() : canonical;
    };
    const QString path = resolvedPath(directory);
    folder = defaults;
    bool existing = false;
    for (const auto &value : folders) {
        const QJsonObject item = value.toObject();
        QString itemPath = item.value("path").toString();
        if (itemPath.startsWith("~/")) itemPath = QDir::homePath() + itemPath.mid(1);
        itemPath = resolvedPath(itemPath);
        if (item.value("id").toString() == folderId) {
            if (itemPath != path) {
                error = "The Omadraft folder ID already points somewhere else. Resolve this in Syncthing before continuing.";
                return false;
            }
            folder = item;
            existing = true;
        } else if (itemPath == path || path.startsWith(itemPath + '/') || itemPath.startsWith(path + '/')) {
            error = "An existing Syncthing folder overlaps the drafts folder. Resolve this in Syncthing before continuing.";
            return false;
        }
    }
    if (existing && (folder.value("type").toString() != "sendreceive" || folder.value("paused").toBool())) {
        error = "The existing Omadraft folder is paused or not Send & Receive. Change it in Syncthing before continuing.";
        return false;
    }
    folder["id"] = folderId;
    folder["label"] = "Omadraft";
    folder["path"] = path;
    folder["type"] = "sendreceive";
    folder["paused"] = false;
    folder["fsWatcherEnabled"] = true;
    folder["fsWatcherDelayS"] = 1;
    folder["rescanIntervalS"] = 60;
    folder["maxConflicts"] = -1;
    folder["ignoreDelete"] = false;
    QJsonArray devices = existing ? folder.value("devices").toArray() : QJsonArray();
    for (const QString &id : {local, peer}) {
        bool found = false;
        for (const auto &device : devices) if (device.toObject().value("deviceID").toString() == id) found = true;
        if (!found) devices.append(QJsonObject{{"deviceID", id}});
    }
    folder["devices"] = devices;
    return true;
}

SyncDialog::SyncDialog(QString directory, const Theme &theme, QWidget *parent)
    : QDialog(parent), m_client(this), m_directory(std::move(directory)) {
    setObjectName("syncDialog");
    setWindowTitle("Set up sync");
    setModal(true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(28, 24, 28, 24);
    layout->setSpacing(14);
    auto *intro = new QLabel("Sync your drafts with another computer using Syncthing.\nRepeat this setup on both computers. Everything still works offline.", this);
    intro->setWordWrap(true);
    layout->addWidget(intro);
    auto *path = new QLabel("Drafts folder: " + m_directory, this);
    path->setTextFormat(Qt::PlainText);
    path->setWordWrap(true);
    path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(path);
    layout->addWidget(new QLabel("This computer's device ID", this));
    m_ownId = new QLineEdit(this);
    m_ownId->setReadOnly(true);
    m_ownId->setObjectName("syncOwnId");
    layout->addWidget(m_ownId);
    auto *copy = new QPushButton("Copy device ID", this);
    copy->setAutoDefault(false);
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_localId); });
    layout->addWidget(copy);
    layout->addWidget(new QLabel("Share drafts with", this));
    m_peers = new QComboBox(this);
    m_peers->setObjectName("syncPeers");
    layout->addWidget(m_peers);
    m_deviceId = new QLineEdit(this);
    m_deviceId->setObjectName("syncPeerId");
    m_deviceId->setPlaceholderText("Paste the other computer's device ID");
    layout->addWidget(m_deviceId);
    connect(m_peers, &QComboBox::currentIndexChanged, this, [this] { m_deviceId->setVisible(m_peers->currentData().toString().isEmpty()); });
    m_status = new QLabel(this);
    m_status->setObjectName("syncStatus");
    m_status->setWordWrap(true);
    m_status->setTextFormat(Qt::PlainText);
    layout->addWidget(m_status);
    m_enable = new QPushButton("Enable sync with this computer", this);
    m_enable->setObjectName("syncEnable");
    connect(m_enable, &QPushButton::clicked, this, &SyncDialog::share);
    layout->addWidget(m_enable);
    m_start = new QPushButton("Start Syncthing and enable at login", this);
    m_start->setAutoDefault(false);
    connect(m_start, &QPushButton::clicked, this, [this] {
        busy(true);
        m_status->setText("Starting Syncthing…");
        auto *process = new QProcess(this);
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, process, &QProcess::kill);
        connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) { process->deleteLater(); fail("Could not start systemctl. Start Syncthing manually, then choose Refresh."); }
        });
        connect(process, &QProcess::finished, this, [this, process](int code) {
            process->deleteLater();
            if (code != 0) fail("Could not start Syncthing. Try systemctl --user enable --now syncthing.service in a terminal.");
            else QTimer::singleShot(1000, this, &SyncDialog::load);
        });
        process->start("systemctl", {"--user", "enable", "--now", "syncthing.service"});
        timeout->start(10000);
    });
    layout->addWidget(m_start);
    m_refresh = new QPushButton("Refresh", this);
    m_refresh->setAutoDefault(false);
    connect(m_refresh, &QPushButton::clicked, this, &SyncDialog::load);
    layout->addWidget(m_refresh);
    auto *close = new QPushButton("Close", this);
    close->setAutoDefault(false);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    layout->addWidget(close);
    setTheme(theme);
    resize(580, sizeHint().height());
    QTimer::singleShot(0, this, &SyncDialog::load);
}

void SyncDialog::setTheme(const Theme &theme) {
    setStyleSheet(QString("QDialog { background: %1; } QLabel { color: %2; } QLineEdit, QComboBox { background: %1; color: %2; border: 1px solid %3; padding: 6px; } QPushButton { background: transparent; color: %2; border: 1px solid %3; padding: 7px; } QPushButton:focus, QPushButton:hover { border-color: %4; color: %4; } QPushButton:disabled { color: %3; }")
        .arg(theme.background.name(), theme.foreground.name(), theme.muted.name(), theme.accent.name()));
}

void SyncDialog::busy(bool value) {
    m_enable->setEnabled(!value && !m_localId.isEmpty());
    m_refresh->setEnabled(!value);
    m_start->setEnabled(!value);
    m_peers->setEnabled(!value);
    m_deviceId->setEnabled(!value);
}

void SyncDialog::fail(const QString &message) {
    busy(false);
    m_start->show();
    m_status->setText(message);
}

void SyncDialog::load() {
    busy(true);
    m_status->setText("Connecting to local Syncthing…");
    m_localId.clear();
    m_ownId->clear();
    m_client.discover([this](const QString &error) {
        if (!error.isEmpty()) { fail(error); return; }
        m_client.request("GET", "system/status", {}, [this](const QJsonDocument &status, const QString &error) {
            if (!error.isEmpty()) { fail(error); return; }
            m_localId = status.object().value("myID").toString();
            m_ownId->setText(m_localId);
            m_client.request("GET", "config/devices", {}, [this](const QJsonDocument &devices, const QString &error) {
                if (!error.isEmpty()) { fail(error); return; }
                m_devices = devices.array();
                m_peers->clear();
                m_peers->addItem("Choose a computer or paste a new device ID…", QString());
                for (const auto &value : m_devices) {
                    const auto device = value.toObject();
                    const QString id = device.value("deviceID").toString();
                    if (id == m_localId || device.value("untrusted").toBool()) continue;
                    const QString name = device.value("name").toString();
                    m_peers->addItem((name.isEmpty() ? "Computer" : name) + " — " + id.left(7), id);
                }
                busy(false);
                m_start->hide();
                m_status->setText("Choose the computer that should receive your drafts. If it is new, paste its device ID. Repeat on the other computer to finish pairing.");
            });
        });
    });
}

void SyncDialog::share() {
    const QString peer = SyncthingClient::normalizeDeviceId(m_peers->currentData().toString().isEmpty() ? m_deviceId->text() : m_peers->currentData().toString());
    if (peer.isEmpty() || peer == m_localId) {
        fail("Enter the other computer's complete Syncthing device ID.");
        return;
    }
    busy(true);
    m_status->setText("Setting up the Omadraft folder…");
    // Fetch fresh configuration before each update; never replace the full configuration.
    m_client.request("GET", "config/folders", {}, [this, peer](const QJsonDocument &folders, const QString &error) {
        if (!error.isEmpty()) { fail(error); return; }
        m_client.request("GET", "config/defaults/folder", {}, [this, peer, folders](const QJsonDocument &defaults, const QString &error) {
            if (!error.isEmpty()) { fail(error); return; }
            QJsonObject folder;
            QString problem;
            if (!SyncthingClient::prepareFolder(folders.array(), defaults.object(), m_directory, m_localId, peer, folder, problem)) { fail(problem); return; }
            m_client.request("GET", "config/devices", {}, [this, peer, folder](const QJsonDocument &devices, const QString &error) {
                if (!error.isEmpty()) { fail(error); return; }
                auto saveFolder = [this, folder] {
                    m_client.request("POST", "config/folders", folder, [this](const QJsonDocument &, const QString &error) {
                        if (!error.isEmpty()) { fail(error); return; }
                        busy(false);
                        m_status->setText("This computer is ready. Run Set up sync on the other computer and select this one (or paste the ID above). Syncthing will connect when both devices are online. To stop sharing later, remove this computer from the Omadraft folder in Syncthing.");
                    });
                };
                for (const auto &value : devices.array()) {
                    const auto device = value.toObject();
                    if (device.value("deviceID").toString() == peer) {
                        if (device.value("untrusted").toBool() || device.value("paused").toBool()) { fail("This device is paused or configured for encrypted storage. Use a trusted, unpaused computer for editing drafts."); return; }
                        saveFolder();
                        return;
                    }
                }
                m_client.request("GET", "config/defaults/device", {}, [this, peer, saveFolder](const QJsonDocument &defaults, const QString &error) {
                    if (!error.isEmpty()) { fail(error); return; }
                    QJsonObject device = defaults.object();
                    device["deviceID"] = peer;
                    device["name"] = "Omadraft computer";
                    device["addresses"] = QJsonArray{"dynamic"};
                    device["autoAcceptFolders"] = false;
                    device["introducer"] = false;
                    device["untrusted"] = false;
                    device["paused"] = false;
                    m_client.request("POST", "config/devices", device, [this, saveFolder](const QJsonDocument &, const QString &error) {
                        if (!error.isEmpty()) { fail(error); return; }
                        saveFolder();
                    });
                });
            });
        });
    });
}
