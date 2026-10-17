#include "relay_station.h"
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    RelayStation window;
    window.show();
    return app.exec();
}
