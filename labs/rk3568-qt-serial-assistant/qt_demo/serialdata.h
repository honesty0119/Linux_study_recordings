// 与设备无关的数据规则，供界面和--self-test共同使用。
#ifndef SERIALDATA_H
#define SERIALDATA_H
#include <QByteArray>
#include <QString>
namespace SerialData {
QString format(const QByteArray &bytes, bool hex);
int appendReceived(QByteArray &buffer, const QByteArray &bytes, int limit);
bool enqueue(QByteArray &pending, const QByteArray &bytes, qint64 driverPending, int limit);
int selfTest();
}
#endif