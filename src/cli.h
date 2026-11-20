#pragma once

#include <QStringList>

struct OmarchyPaths;

// Headless commands for scripts and agents (docs/technical.md section 9).
// Each takes --json, writes errors to stderr and exits 0 on success, 1 on an
// error, 2 on a usage error, 3 when it cannot run on this machine.
namespace Cli {

// True for the words that run a command rather than open the window.
bool isCommand(const QString &word);
QString summary();
// arguments as QCoreApplication::arguments(): the program, then the command.
int run(const QStringList &arguments, const OmarchyPaths &paths);

} // namespace Cli
