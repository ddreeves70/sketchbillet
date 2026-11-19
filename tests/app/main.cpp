// Runs the app tests inside a QApplication, without a screen; with SB_SCREEN set,
// on the screen QT_QPA_PLATFORM names, so tests tagged [gl] use the real 3D view.

#include <catch2/catch_session.hpp>

#include <QApplication>

int main(int argc, char** argv) {
    if (qEnvironmentVariableIsEmpty("SB_SCREEN")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}
