// TuxDirStat — Linux için disk kullanım analizörü.
// Kullanım: tuxdirstat [klasör]

#include <QApplication>
#include <QIcon>

#include "mainwindow.h"
#include "report.h"
#include "theme.h"

int main(int argc, char **argv)
{
    if (int rc = runCommandLine(argc, argv); rc >= 0)
        return rc;
    QApplication::setDesktopFileName("tuxdirstat");
    QApplication app(argc, argv);
    app.setApplicationName("TuxDirStat");
    app.setWindowIcon(appIcon());
    applyTheme(Settings::load().theme);
    MainWindow w(QString::fromStdString(pathArgument(argc, argv)));
    w.show();
    return app.exec();
}
