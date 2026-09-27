#include "session.h"
#include "omadraft_version.h"
#include "theme.h"
#include "window.h"
#include "restart.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QMessageBox>
#include <QProcess>
#include <QSessionManager>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QWindow>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>

namespace {
int signalWriteFd = -1;
void handleSignal(int number) {
    const int savedErrno = errno;
    const char value = char(number);
    if (signalWriteFd >= 0) {
        const auto result = ::write(signalWriteFd, &value, 1);
        (void)result;
    }
    errno = savedErrno;
}
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    const QString installedExecutable = QCoreApplication::applicationFilePath();
    app.setProperty("installedExecutable", installedExecutable);
    app.setApplicationName("omadraft");
    app.setApplicationDisplayName("Omadraft");
    app.setApplicationVersion(OMADRAFT_VERSION);
    app.setDesktopFileName("omadraft");
    app.setStyle("Fusion");
    app.setWindowIcon(QIcon(":/omadraft.svg"));
    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal Markdown notes for Omarchy. Open, jot, close.");
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption dataOption("data-dir", "Use a separate note directory.", "directory");
    const QCommandLineOption themeOption("theme-file", "Read a specific Omarchy colors.toml file.", "path");
    const QCommandLineOption syncOption("setup-sync", "Open the optional Syncthing setup.");
    parser.addOption(syncOption);
    parser.addOption(dataOption);
    parser.addOption(themeOption);
    parser.process(app);

    QString directory = parser.isSet(dataOption) ? QDir(parser.value(dataOption)).absolutePath()
        : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(directory)) {
        QMessageBox::critical(nullptr, "Cannot open notes", "Cannot create the note directory: " + directory);
        return 1;
    }
    directory = QDir(directory).canonicalPath();
    QLockFile lock(directory + "/session.lock");
    lock.setStaleLockTime(0);
    const QString serverName = "omadraft-" + QString::fromLatin1(
        QCryptographicHash::hash(directory.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
    if (!lock.tryLock()) {
        QLocalSocket client;
        QElapsedTimer wait;
        wait.start();
        // A simultaneous first launch may own the lock before its socket is ready.
        do {
            client.abort();
            client.connectToServer(serverName);
            if (client.waitForConnected(100)) {
                client.write(parser.isSet(syncOption) ? "setup-sync\n" : "activate\n");
                client.waitForBytesWritten(250);
                return 0;
            }
            QThread::msleep(25);
        } while (wait.elapsed() < 750);
        qWarning().noquote() << "Cannot contact the running instance:" << client.errorString();
        QMessageBox::warning(nullptr, "Notes already open",
                             "This note directory is already in use, or its lock could not be acquired.\n\n" + directory);
        return 1;
    }
    SessionStore store(directory);
    Session session;
    QString error, notice;
    if (!store.load(session, error, notice)) {
        QMessageBox::critical(nullptr, "Cannot open notes", error);
        return 1;
    }
    QStringList themePaths;
    if (parser.isSet(themeOption))
        themePaths << QFileInfo(parser.value(themeOption)).absoluteFilePath();
    ThemeWatcher theme(themePaths);
    Window window(store, session, theme);

    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(serverName);
    if (!server.listen(serverName)) {
        qCritical().noquote() << "Cannot start Omadraft:" << server.errorString();
        QMessageBox::critical(nullptr, "Cannot start Omadraft", server.errorString());
        return 1;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &window, [&] {
        while (auto *connection = server.nextPendingConnection()) {
            QObject::connect(connection, &QLocalSocket::disconnected, connection, &QObject::deleteLater);
            const auto receive = [connection, &window] {
                if (!connection->canReadLine()) return;
                const QByteArray command = connection->readLine().trimmed();
                connection->disconnectFromServer();
                if (command == "setup-sync")
                    QTimer::singleShot(0, &window, &Window::showSyncSetup);
            };
            QObject::connect(connection, &QLocalSocket::readyRead, &window, receive);
            receive();
        }
        if (window.isMinimized())
            window.showNormal();
        window.raise();
        window.activateWindow();
        window.focusNote();
        if (window.windowHandle())
            window.windowHandle()->requestActivate();
        // Hyprland can focus the existing window across workspaces without an activation token.
        if (!qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
            const QString hyprctl = QStandardPaths::findExecutable("hyprctl");
            if (!hyprctl.isEmpty()) {
                const QString selector = QString("pid:%1").arg(app.applicationPid());
                auto *focus = new QProcess(&window);
                focus->setStandardOutputFile(QProcess::nullDevice());
                focus->setStandardErrorFile(QProcess::nullDevice());
                QObject::connect(focus, &QProcess::finished, &window, [focus, hyprctl, selector](int status) {
                    if (status != 0)
                        QProcess::startDetached(hyprctl, {"dispatch", "focuswindow", selector});
                    focus->deleteLater();
                });
                QObject::connect(focus, &QProcess::errorOccurred, focus, &QObject::deleteLater);
                focus->start(hyprctl, {"dispatch", QString("hl.dsp.focus({ window = \"%1\" })").arg(selector)});
            }
        }
    });

    QObject::connect(&window, &Window::restartRequested, &window, [&] {
        if (!window.saveNow()) return;
        QStringList arguments = app.arguments().mid(1);
        arguments.removeAll("--setup-sync");
        QString reason;
        replaceProcess(installedExecutable, arguments, lock, server, reason);
        if (!lock.isLocked()) {
            qCritical() << "Restart failed: another Omadraft instance opened these notes.";
            app.exit(1);
            return;
        }
        QMessageBox::warning(&window, "Restart failed", "Your notes were saved. Close and reopen Omadraft to use the update.\n\n" + reason);
    });

    int signalPipe[2] = {-1, -1};
    if (::pipe2(signalPipe, O_NONBLOCK | O_CLOEXEC) == 0) {
        signalWriteFd = signalPipe[1];
        auto *notifier = new QSocketNotifier(signalPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &window, [&](QSocketDescriptor, QSocketNotifier::Type) {
            char bytes[32];
            while (::read(signalPipe[0], bytes, sizeof(bytes)) > 0) {}
            window.close();
        });
        std::signal(SIGTERM, handleSignal);
        std::signal(SIGINT, handleSignal);
        std::signal(SIGHUP, handleSignal);
    }
    QObject::connect(&app, &QGuiApplication::commitDataRequest, &window, [&](QSessionManager &manager) {
        if (!window.saveNow())
            manager.cancel();
    }, Qt::DirectConnection);
    window.show();
    if (parser.isSet(syncOption))
        QTimer::singleShot(0, &window, &Window::showSyncSetup);
    if (!notice.isEmpty())
        QTimer::singleShot(0, &window, [&] { QMessageBox::information(&window, "Notes recovered", notice); });
    const int result = app.exec();
    signalWriteFd = -1;
    if (signalPipe[0] >= 0) {
        ::close(signalPipe[0]);
        ::close(signalPipe[1]);
    }
    return result;
}
