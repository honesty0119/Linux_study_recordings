// 串口助手主窗口：界面、端口生命周期、异步有界收发与触摸输入。
#ifndef MAINWINDOW_H
#define MAINWINDOW_H
#include <QMainWindow>
#include <QByteArray>
#include <QElapsedTimer>
#include <QSerialPort>
class QTimer;
class QCloseEvent;
class QResizeEvent;
QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE
class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;
protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    void refreshPorts();
    void updateControls();
    void togglePort();
    void stopPort(const QString &reason, bool error = false);
    void report(const QString &message, bool error = false);
    void sendText(bool automatic = false);
    void pumpWrite();
    void receiveBytes();
    void renderReceive();
    void updateCounters();
    void showKeyboard();
    Ui::MainWindow *ui;
    QSerialPort *serial;
    QTimer *autoTimer;
    QTimer *writeTimer;
    QTimer *displayTimer;
    QByteArray pendingTx;              // 尚未被QSerialPort接受的字节，短写时保留。
    QByteArray received;               // 最近64 KiB原始字节；显示模式不修改它。
    QElapsedTimer txProgress;
    quint64 rxTotal = 0;
    quint64 txTotal = 0;               // bytesWritten计数，不代表对端已收到。
    quint64 viewDropped = 0;
    bool opening = false;
    bool stopping = false;
    bool renderingNeeded = false;
    int styleFontSize = 0;
};
#endif