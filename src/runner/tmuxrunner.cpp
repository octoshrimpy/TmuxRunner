#include "tmuxrunner.h"

// KF
#include <KLocalizedString>
#include <QtGui/QtGui>
#include <QtCore>
#include <KSharedConfig>


TmuxRunner::TmuxRunner(QObject *parent, const QVariantList &args)
        : Plasma::AbstractRunner(parent, args) {
    setObjectName(QStringLiteral("TmuxRunner"));
}

TmuxRunner::~TmuxRunner() = default;


void TmuxRunner::init() {
    const QString configFolder = QDir::homePath() + "/.config/krunnerplugins/";
    const QDir configDir(configFolder);
    if (!configDir.exists()) configDir.mkpath(configFolder);
    // Create file
    QFile configFile(configFolder + "tmuxrunnerrc");
    if (!configFile.exists()) {
        configFile.open(QIODevice::WriteOnly);
        configFile.close();
    }
    // Add file watcher for config
    watcher.addPath(configFolder + "tmuxrunnerrc");
    connect(&watcher, &QFileSystemWatcher::fileChanged, this, &TmuxRunner::reloadPluginConfiguration);
    connect(this, &TmuxRunner::prepare, this, &TmuxRunner::prepareForMatchSession);

    config = KSharedConfig::openConfig(QDir::homePath() + QStringLiteral("/.config/krunnerplugins/tmuxrunnerrc"))
            ->group("Config");

    api = new TmuxRunnerAPI(config);

    reloadPluginConfiguration();
}

void TmuxRunner::prepareForMatchSession() {
    tmuxSessions = api->fetchTmuxSessions();
}

/**
 * Call method whenever the config file changes, the normal reloadConfiguration method gets called to often
 */
void TmuxRunner::reloadPluginConfiguration(const QString &path) {
    // Method was triggered using file watcher => get new state from file
    if (!path.isEmpty()) config.config()->reparseConfiguration();
    enableTmuxinator = config.readEntry("enable_tmuxinator", true);
    enableFlags = config.readEntry("enable_flags", true);
    enableNewSessionByPartlyMatch = config.readEntry("add_new_by_part_match", false);
    defaultProgram = config.readEntry("program", "konsole");

    // If the file gets edited with a text editor, it often gets replaced by the edited version
    // https://stackoverflow.com/a/30076119/9342842
    if (!path.isEmpty()) {
        if (QFile::exists(path)) {
            watcher.addPath(path);
        }
    }
    if (enableTmuxinator) {
        // Check if tmuxinator is installed
        QProcess isTmuxinatorInstalledProcess;
        isTmuxinatorInstalledProcess.start("whereis", QStringList() << "-b" << "tmuxinator");
        isTmuxinatorInstalledProcess.waitForFinished();
        if (QString(isTmuxinatorInstalledProcess.readAll()) == "tmuxinator:\n") {
            // Disable tmuxinator until the user installs and enables it
            enableTmuxinator = false;
            config.writeEntry("enable_tmuxinator", false);
            return;
        }
        // Fetch the available configurations
        QProcess process;
        process.start("tmuxinator ls");
        process.waitForFinished();
        const QString res = process.readAll();
        if (res.split('\n').size() == 2) {
            return;
        }
        const auto _entries = res.split('\n');
        if (_entries.size() < 2) {
            return;
        }
        const auto entries = _entries.at(1).split(' ');
        tmuxinatorConfigs.clear();
        for (const auto &entry:entries) {
            if (!entry.isEmpty()) {
                tmuxinatorConfigs.append(entry);
            }
        }
    }
}

void TmuxRunner::match(Plasma::RunnerContext &context) {
    QString term = context.query();
    if (!context.isValid() || !term.startsWith(triggerWord)) return;
    term.remove(formatQueryRegex);
    QList<Plasma::QueryMatch> matches;
    bool exactMatch = false;
    bool tmuxinator = false;

    QMap<QString, QVariant> data;
    QStringList attached;

    // Flags to open other terminal emulator
    QString openIn = QString();
    if (enableFlags) {
        // Flag at end of query or just a flag with no session name
        if (term.contains(QRegExp(" -([a-z])$")) || (term.size() == 2 && term.contains(QRegExp("-([a-z])$")))) {
            QRegExp exp("-([a-z])$");
            exp.indexIn(term);
            const QString flag = exp.capturedTexts().at(1);
            QString flagValue = flags.value(flag, "");
            if (!flagValue.isEmpty()) {
                data.insert("program", flagValue);
                openIn = " in " + flagValue.remove("-session");
            } else {
                openIn = " default (invalid flag)";
            }
            term.remove(QRegExp(" ?-([a-z])$"));
        }
    }
    // Session with Tmuxinator
    if (enableTmuxinator && term.startsWith("inator")) {
        QRegExp regExp(R"(inator(?: (\w+) *(.+)?)?)");
        regExp.indexIn(term);
        QString filter = regExp.capturedTexts().at(1);
        term.replace(QRegExp("^inator *"), "");
        tmuxinator = true;
        for (const auto &tmuxinatorConfig: qAsConst(tmuxinatorConfigs)) {
            if (tmuxinatorConfig.startsWith(filter)) {
                if (!tmuxSessions.contains(tmuxinatorConfig)) {
                    matches.append(
                            createMatch("Create Tmuxinator  " + tmuxinatorConfig + openIn,
                                        {
                                                {"action",  "tmuxinator"},
                                                {"program", data.value("program", defaultProgram)},
                                                {"args",    regExp.capturedTexts().last()},
                                                {"target",  tmuxinatorConfig}
                                        }, 1)
                    );
                } else {
                    attached.append(tmuxinatorConfig);
                    data.insert("action", "attach");
                    data.insert("target", tmuxinatorConfig);
                    matches.append(
                            createMatch("Attach Tmuxinator  " + tmuxinatorConfig + openIn, data, 0.99)
                    );
                }
            }
        }
    }
    // Attach to session options
    const auto queryName = term.contains(' ') ? term.split(' ').first() : term;
    for (const auto &session:tmuxSessions) {
        if (session.startsWith(queryName)) {
            if (session == queryName) exactMatch = true;
            if (attached.contains(session)) continue;
            data.insert("action", "attach");
            data.insert("target", session);
            matches.append(
                    createMatch("Attach to " + session + openIn, data,
                                (float) queryName.length() / (float) session.length())
            );
        }
    }
    // New session
    if (!exactMatch && (matches.isEmpty() || enableNewSessionByPartlyMatch)) {
        // Name and optional path, Online tester : https://regex101.com/r/FdZcIZ/1
        QRegExp regex(R"(^([\w-]+)(?: +(.+)?)?$)");
        regex.indexIn(term);
        QStringList texts = regex.capturedTexts();
        float relevance = matches.empty() ? 1 : 0;
        data.insert("action", "new");
        data.insert("target", texts.at(1));

        // No path
        if (texts.at(2).isEmpty()) {
            if (!texts.at(1).isEmpty() || !tmuxinator) {
                matches.append(createMatch("New session " + texts.at(1) + openIn, data, relevance));
            }
            // With path
        } else {
            data.insert("path", texts.at(2));
            matches.append(
                    createMatch("New session " + texts.at(1) + " in " + texts.at(2) + openIn, data, relevance)
            );
        }
    }

    context.addMatches(matches);
}

void TmuxRunner::run(const Plasma::RunnerContext &context, const Plasma::QueryMatch &match) {
    Q_UNUSED(context)

    QMap<QString, QVariant> data = match.data().toMap();
    QString program = data.value("program", defaultProgram).toString();
    const QString target = data.value("target").toString();
    QStringList args;

    if (data.value("action") == "attach") {
        api->executeAttatchCommand(program, target);
    } else {
        api->executeCreateCommand(program, target, data);
    }
}


Plasma::QueryMatch TmuxRunner::createMatch(const QString &text, const QMap<QString, QVariant> &data, float relevance) {
    Plasma::QueryMatch match(this);
    match.setIcon(icon);
    match.setText(text);
    match.setData(data);
    match.setRelevance(relevance);
    return match;
}


K_EXPORT_PLASMA_RUNNER(tmuxrunner, TmuxRunner)

// needed for the QObject subclass declared as part of K_EXPORT_PLASMA_RUNNER
#include "tmuxrunner.moc"