#include <QApplication>
#include <QIcon>
#include "MainWindow.h"
#include "SplashScreen.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("V Editor Simple");
    app.setApplicationVersion(APP_VERSION);
    app.setOrganizationName("VEditorSimple");
    app.setWindowIcon(QIcon(":/icons/app-icon.svg"));

    // Splash screen
    AppSplashScreen splash;
    splash.show();

    splash.setProgress(10, "Loading core modules...");
    splash.setProgress(30, "Initializing video engine...");
    splash.setProgress(50, "Setting up timeline...");
    splash.setProgress(70, "Loading plugins & presets...");
    splash.setProgress(90, "Preparing workspace...");

    MainWindow window;
    splash.finishWithDelay(&window, 400);
    window.show();

    return app.exec();
}
