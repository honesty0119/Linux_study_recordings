// 程序入口：仅本应用横屏；--rotate-left反转方向，--windowed便于窗口检查。
#include "landscapeview.h"
#include <QApplication>
#include <QFontDatabase>
#include <QDebug>
#include "serialdata.h"
#include "fileio.h"
int main(int argc, char *argv[])
{
    // 自检不创建窗口、不枚举或打开串口，便于板端核实数据规则。
    for (int i = 1; i < argc; ++i) {
        if (qstrcmp(argv[i], "--self-test") == 0) {
            QCoreApplication app(argc, argv);
            const int dataResult = SerialData::selfTest();
            const int fileResult = FileIO::selfTest();
            return (dataResult || fileResult) ? 1 : 0;
        }
    }
    QApplication::setAttribute(Qt::AA_SynthesizeMouseForUnhandledTouchEvents, true);
    QApplication app(argc, argv);
    const QStringList families = QFontDatabase().families();
    for (const QString &family : {QStringLiteral("Noto Sans SC"), QStringLiteral("Source Han Sans CN")}) {
        if (families.contains(family)) { app.setFont(QFont(family)); qInfo() << "UI font:" << family; break; }
    }
    QApplication::setApplicationName(QStringLiteral("RK3568 Serial Assistant"));
    LandscapeView window(app.arguments().contains(QStringLiteral("--rotate-left")));
    if (app.arguments().contains(QStringLiteral("--windowed")))
        window.show();
    else
        window.showFullScreen();
    return app.exec();
}