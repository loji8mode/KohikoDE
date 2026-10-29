#include "Application.h"

#include "ConfigMigration.h"
#include "Xdg.h"
#include "EventLoop.h"
#include "Logger.h"
#include "RecoveryMode.h"
#include "Version.h"
#include "WindowManager.h"

#include <csignal>
#include <cstdlib>

namespace Kohiko
{

int Application::Run(
    const std::string& configPath)
{
    // As early as physically possible - deliberately before even the
    // signal handlers just below - so this brackets the *entire* rest
    // of startup, including config loading/migration and
    // WindowManager::Initialize() itself (by far the most likely
    // place a genuinely broken config could crash something). See
    // RecoveryMode.h for the full mechanism.
    bool previousStartupCrashed = RecoveryMode::NoteStartupAttempt();

    // Reap Process::Spawn()'s launched children automatically instead
    // of letting them pile up as zombies, and don't die just because
    // a client vanished mid-write on some socket.
    std::signal(SIGCHLD, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);

    EventLoop::InstallSignalHandlers();

    // $XDG_CURRENT_DESKTOP drives OnlyShowIn=/NotShowIn= filtering for
    // both the launcher (DesktopEntry.cpp's ShouldDisplay()) and XDG
    // autostart (ShouldAutostart(), via WindowManager::
    // CollectXdgAutostartEntries()) - but nothing in this codebase ever
    // set it, leaving it entirely up to whatever launched this
    // process. A display manager that honours desktop/kohiko.desktop's
    // DesktopNames=Kohiko typically exports it already; a manual
    // `startx`/`~/.xinitrc` launch never does, since there's no
    // display manager involved to read that key from at all. Fill the
    // gap (without overwriting a value that's already there - setenv's
    // overwrite=0) so both paths behave identically instead of an
    // OnlyShowIn=/NotShowIn= entry silently working under one and not
    // the other.
    setenv("XDG_CURRENT_DESKTOP", "Kohiko", 0);

    Logger::Info(std::string("kohiko ") + VERSION + " starting");

    if (!m_connection.Connect())
    {
        Logger::Fatal(m_connection.LastError());
        return 1;
    }

    if (!m_connection.BecomeWindowManager())
    {
        Logger::Fatal(m_connection.LastError());
        return 1;
    }

    if (previousStartupCrashed)
    {
        // Don't retry the same configuration that (most likely) just
        // crashed Kohiko - leave m_config empty instead, so every
        // Config getter falls back to its own hardcoded default (see
        // Config.h). The suspect file is left completely alone beyond
        // this one backup: not migrated, not loaded, not deleted -
        // it's exactly what the user needs to go fix.
        ConfigMigration::BackupConfig(configPath);
        Logger::Warning(
            "kohiko didn't finish starting last time - starting with "
            "built-in defaults instead of " + configPath + " this time. "
            "Your previous configuration was backed up to " + configPath +
            ".bak - fix whatever's wrong, then either restore it "
            "(kohikoctl restore-config) or edit " + configPath + " directly.");
    }
    else
    {
        // Keeps an existing config in sync with whatever settings
        // this build actually knows about - see ConfigMigration.h.
        // Ahead of Load() below, so a key it just appended is picked
        // up this session too, not only after a second restart.
        std::vector<std::string> retiredKeys;

        if (ConfigMigration::MigrateIfNeeded(configPath, &retiredKeys))
            Logger::Info("updated " + configPath + " to match this version (previous contents saved to " + configPath + ".bak)");

        for (const std::string& key : retiredKeys)
            Logger::Info("removed obsolete setting " + key + " from " + configPath +
                         " - Kohiko no longer learns window placement; use Window Rules instead (kohiko-choose-window / kohiko-settings)");

        if (!m_config.Load(configPath))
            Logger::Warning("could not read " + configPath + ", using built-in defaults");
    }

    // Learned placement habits no longer exist as a feature (0.22.0).
    // Delete what the old one left on disk instead of leaving a dead
    // file behind that could be mistaken for live state - and never
    // convert it into explicit rules. Independent of the config
    // migration above on purpose: it must also run when that is
    // skipped (crash recovery) or the config can't be read.
    for (const std::string& name : ConfigMigration::RetireObsoleteData(Xdg::DataDir()))
        Logger::Info("removed obsolete data file " + (Xdg::DataDir() / name).string() +
                     " (learned window placement was removed; nothing replaces it automatically)");

    WindowManager windowManager(m_connection, m_config, configPath);
    windowManager.Initialize();

    Logger::Info("ready");

    // Startup made it all the way here without crashing - whatever
    // NoteStartupAttempt() saw (or didn't see) no longer matters,
    // clear it so the *next* launch starts from a clean slate.
    RecoveryMode::MarkStartupSucceeded();

    EventLoop loop(m_connection, windowManager);
    loop.Run();

    windowManager.Shutdown();

    Logger::Info("shutting down");

    return 0;
}

}
