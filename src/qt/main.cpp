// TuxDirStat — Linux için disk kullanım analizörü.
// Kullanım: tuxdirstat [klasör]

#include <QApplication>
#include <QIcon>
#include <QLocale>

#include "i18n.h"

#include "mainwindow.h"
#include "report.h"
#include "theme.h"

int main(int argc, char **argv)
{
    if (int rc = runCommandLine(argc, argv); rc >= 0)
        return rc;
    QApplication::setDesktopFileName("tuxdirstat");
    QApplication app(argc, argv);
    if (i18n::explicitChoice())  // MIME açıklamaları QLocale'e göre seçilir
        QLocale::setDefault(QLocale(QString::fromLatin1(i18n::code())));
    app.setApplicationName("TuxDirStat");
    app.setWindowIcon(appIcon());
    applyTheme(Settings::load().theme);
    MainWindow w(QString::fromStdString(pathArgument(argc, argv)));
    w.show();
    return app.exec();
}
