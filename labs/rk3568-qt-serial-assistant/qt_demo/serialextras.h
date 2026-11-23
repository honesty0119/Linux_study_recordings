// 选做功能控制器：接收落盘、当前视图原始字节快照、分块文件发送与内嵌文件选择。
#ifndef SERIALEXTRAS_H
#define SERIALEXTRAS_H
#include <QObject>
#include <QPointer>
#include <functional>
#include "fileio.h"
class QWidget;
class QSerialPort;
class QTimer;
class QPushButton;
class QLabel;
class SerialExtras : public QObject
{
    Q_OBJECT
public:
    struct Hooks {
        std::function<qint64()> outstanding;
        std::function<bool(const QByteArray &)> enqueue;
        std::function<QByteArray()> snapshot;
        std::function<void()> stopAutomatic;
    };
    SerialExtras(QWidget *page, QSerialPort *serial, Hooks hooks);
    ~SerialExtras() override;
    bool busy() const { return sending; }
    void refreshControls();
    void recordBytes(const QByteArray &bytes);
    void portClosing();
signals:
    void message(const QString &text, bool error);
    void busyChanged();
    void stopRequested(const QString &reason);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void toggleRecord();
    void saveCurrent();
    void chooseFile();
    void startFile(const QString &path);
    void refillFile();
    void finishRecord(const QString &reason);
    QWidget *page;
    QSerialPort *serial;
    Hooks hooks;
    QTimer *fileTimer;
    QPushButton *recordButton, *saveButton, *fileButton, *cancelButton;
    QLabel *progress;
    QPointer<QWidget> browser;
    FileIO::RawLog log;
    FileIO::ChunkedFile input;
    bool sending = false;
    qint64 fileWritten = 0;
    QString dataDirectory;
};
#endif