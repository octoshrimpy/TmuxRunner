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
        tmuxinatorConfigs = api->fetchTmuxinatorConfigs();
        enableTmuxinator = !tmuxinatorConfigs.isEmpty();
    }
}

void TmuxRunner::match(Plasma::RunnerContext &context) {
    QString term = context.query();
    if (!context.isValid() || !term.startsWith(triggerWord)) return;
    term.remove(triggerWordRegex);
    QList <Plasma::QueryMatch> matches;
    bool exactMatch = false;
    bool tmuxinator = false;

    QMap <QString, QVariant> data;
    QString program = defaultProgram;
    QStringList attached;

    // Flags to open other terminal emulator
    QString openIn;
    if (enableFlags) {
        QString flagProgram = api->parseQueryFlags(term, openIn);
        if (!flagProgram.isEmpty()) {
            program = flagProgram;
        }
    }

    // Session with Tmuxinator
    if (enableTmuxinator && term.startsWith(tmuxinatorQuery)) {
        tmuxinator = true;
        context.addMatches(addTmuxinatorMatches(term, openIn, program, attached));
    }

    // Attach to session options
    context.addMatches(addTmuxAttachMatches(term, openIn, program, attached, &exactMatch));

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

    const QMap<QString, QVariant> data = match.data().toMap();
    QString program = data.value("program", defaultProgram).toString();
    const QString target = data.value("target").toString();

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

QList<Plasma::QueryMatch>
TmuxRunner::addTmuxinatorMatches(QString &term, const QString &openIn, const QString &program,
                                 QStringList &attached) {
    QList <Plasma::QueryMatch> matches;
    const QRegularExpressionMatch tmuxinatorMatch = tmuxinatorQueryRegex.match(term);
    term.remove(tmuxinatorClearRegex);
    const QString filter = tmuxinatorMatch.captured(1);
    for (const auto &tmuxinatorConfig: qAsConst(tmuxinatorConfigs)) {
        if (tmuxinatorConfig.startsWith(filter)) {
            if (!tmuxSessions.contains(tmuxinatorConfig)) {
                matches.append(
                        createMatch("Create Tmuxinator  " + tmuxinatorConfig + openIn,
                                    {
                                            {"action",  "tmuxinator"},
                                            {"program", program},
                                            {"args",    tmuxinatorMatch.captured(2)},
                                            {"target",  tmuxinatorConfig}
                                    }, 1)
                );
            } else {
                attached.append(tmuxinatorConfig);
                matches.append(
                        createMatch("Attach Tmuxinator  " + tmuxinatorConfig + openIn, {
                                {"action",  "attach"},
                                {"program", program},
                                {"target",  tmuxinatorConfig},
                        }, 0.99)
                );
            }
        }
    }
    return matches;
}

QList<Plasma::QueryMatch>
TmuxRunner::addTmuxAttachMatches(QString &term, const QString &openIn, const QString &program,
                                 QStringList &attached, bool *exactMatch) {
    QList<Plasma::QueryMatch> matches;
    const auto queryName = term.contains(' ') ? term.split(' ').first() : term;
    for (const auto &session:tmuxSessions) {
        if (session.startsWith(queryName)) {
            if (session == queryName) *exactMatch = true;
            if (attached.contains(session)) continue;
            matches.append(
                    createMatch("Attach to " + session + openIn,
                                {{"action",  "attach"},
                                 {"program", program},
                                 {"target",  session}},
                                (float) queryName.length() / (float) session.length())
            );
        }
    }
    return matches;
}

QList<Plasma::QueryMatch>
TmuxRunner::addTmuxNewSessionMatches(QString &term, const QString &openIn, const QString &program,
                                     QStringList &attached, bool *exactMatch) {
    return QList<Plasma::QueryMatch>();
}

K_EXPORT_PLASMA_RUNNER(tmuxrunner, TmuxRunner)

// needed for the QObject subclass declared as part of K_EXPORT_PLASMA_RUNNER
#include "tmuxrunner.moc"
