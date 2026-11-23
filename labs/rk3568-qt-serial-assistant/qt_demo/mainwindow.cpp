// RK3568串口助手：单线程事件驱动；原始字节与显示分离，所有队列有上限。
#include "mainwindow.h"
#include "serialdata.h"
#include "serialextras.h"
#include "ui_mainwindow.h"
#include <QTimer>
#include <QCoreApplication>
#include <QList>
#include <QFile>
#include <QSerialPortInfo>
#include <QStandardItemModel>
#include <QSignalBlocker>
#include <QSet>
#include <QDateTime>
#include <QDebug>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QScrollBar>
#include <QFrame>
#include <QGridLayout>
#include <QDialogButtonBox>
#include <QAbstractItemView>
#include <QTextDocument>
#include <algorithm>

namespace {
const int RxLimit = 64 * 1024;
const int TxLimit = 64 * 1024;
const int DriverWindow = 4096;
// 控制台保护每次刷新、打开时重新读取，ttyFIQ0始终保护。
QSet<QString> consolePorts()
{
    QSet<QString> names{QStringLiteral("ttyFIQ0")};
    QFile file(QStringLiteral("/sys/class/tty/console/active"));
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        for (const auto &name : QString::fromLatin1(file.readAll()).simplified()
             .split(QLatin1Char(' '), Qt::SkipEmptyParts)) names.insert(name);
    }
    return names;
}
QString restriction(const QSerialPortInfo &port, const QSet<QString> &consoles)
{
    if (consoles.contains(port.portName())) return QStringLiteral("控制台 / 已保护");
    if (port.portName() == QStringLiteral("ttyGS0")
        && !QCoreApplication::arguments().contains(QStringLiteral("--allow-usb-gadget")))
        return QStringLiteral("USB Gadget / 待核实，默认保留");
    if (port.systemLocation().isEmpty()) return QStringLiteral("设备路径无效");
    return QString();
}
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), serial(new QSerialPort(this)),
      autoTimer(new QTimer(this)), writeTimer(new QTimer(this)), displayTimer(new QTimer(this))
{
    ui->setupUi(this);
    for (auto *combo : findChildren<QComboBox *>()) combo->setMaxVisibleItems(6);
    ui->comboSerial->setModel(new QStandardItemModel(ui->comboSerial));
    for (int baud : {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600})
        ui->comboBaudRate->addItem(QString::number(baud), baud);
    ui->comboBaudRate->setCurrentIndex(ui->comboBaudRate->findData(115200));
    for (int bits : {5, 6, 7, 8}) ui->comboDataBits->addItem(QString::number(bits), bits);
    ui->comboDataBits->setCurrentIndex(3);
    ui->comboParity->addItem(QStringLiteral("无校验"), QSerialPort::NoParity);
    ui->comboParity->addItem(QStringLiteral("偶校验"), QSerialPort::EvenParity);
    ui->comboParity->addItem(QStringLiteral("奇校验"), QSerialPort::OddParity);
    ui->comboStopBits->addItem(QStringLiteral("1"), QSerialPort::OneStop);
    ui->comboStopBits->addItem(QStringLiteral("2"), QSerialPort::TwoStop);
    ui->comboDisplay->addItems({QStringLiteral("ASCII"), QStringLiteral("HEX")});
    ui->comboEnding->addItems({QStringLiteral("无"), QStringLiteral("LF"), QStringLiteral("CRLF")});
    serial->setReadBufferSize(RxLimit);
    writeTimer->setInterval(20);
    displayTimer->setInterval(100);
    displayTimer->start();
    autoTimer->setTimerType(Qt::PreciseTimer);

    connect(ui->buttonExit, &QPushButton::clicked, this, &QWidget::close);
    connect(ui->buttonRefresh, &QPushButton::clicked, this, &MainWindow::refreshPorts);
    connect(ui->buttonOpen, &QPushButton::clicked, this, &MainWindow::togglePort);
    connect(ui->comboSerial, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        updateControls();
        const auto path = ui->comboSerial->currentData().toString();
        report(path.isEmpty() ? QStringLiteral("请选择串口，当前未打开")
                             : QStringLiteral("已选择 %1，当前未打开").arg(path));
    });
    connect(ui->buttonSend, &QPushButton::clicked, this, [this] { sendText(); });
    connect(ui->buttonIntervalMinus, &QPushButton::clicked, this, [this] { ui->spinInterval->stepDown(); });
    connect(ui->buttonIntervalPlus, &QPushButton::clicked, this, [this] { ui->spinInterval->stepUp(); });
    connect(ui->buttonKeyboard, &QPushButton::clicked, this, &MainWindow::showKeyboard);
    connect(ui->buttonClearReceive, &QPushButton::clicked, this, [this] {
        received.clear(); viewDropped = 0; renderingNeeded = true; renderReceive();
        updateCounters(); report(QStringLiteral("接收显示已清空，累计字节计数保留"));
    });
    connect(ui->comboDisplay, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        renderingNeeded = true; renderReceive();
    });
    connect(ui->checkAuto, &QCheckBox::toggled, this, [this](bool enabled) {
        if (!enabled) { autoTimer->stop(); report(QStringLiteral("定时发送已停止")); return; }
        if (!serial->isOpen() || ui->editSend->text().isEmpty()
            || (findChild<SerialExtras *>() && findChild<SerialExtras *>()->busy())) {
            QSignalBlocker block(ui->checkAuto); ui->checkAuto->setChecked(false);
            report(QStringLiteral("请先打开串口并输入发送文本"), true); return;
        }
        autoTimer->start(ui->spinInterval->value());
        report(QStringLiteral("定时发送已启动，间隔 %1 ms").arg(autoTimer->interval()));
    });
    connect(ui->spinInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int ms) {
        if (autoTimer->isActive()) autoTimer->setInterval(ms);
    });
    connect(autoTimer, &QTimer::timeout, this, [this] {
        // 上一笔还没排空就跳过本周期，不累积定时任务，不追赶发送。
        if (pendingTx.isEmpty() && serial->bytesToWrite() == 0) sendText(true);
    });
    connect(serial, &QSerialPort::readyRead, this, &MainWindow::receiveBytes);
    connect(serial, &QSerialPort::bytesWritten, this, [this](qint64 count) {
        if (count > 0) { txTotal += quint64(count); txProgress.restart(); }
        updateCounters();
        // 排队调用避免write信号重入时重复处理pendingTx。
        QTimer::singleShot(0, this, &MainWindow::pumpWrite);
    });
    connect(serial, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError || stopping) return;
        qWarning() << "serial error" << int(error) << serial->errorString();
        if (!opening) stopPort(QStringLiteral("串口异常：%1").arg(serial->errorString()), true);
    });
    connect(writeTimer, &QTimer::timeout, this, &MainWindow::pumpWrite);
    connect(displayTimer, &QTimer::timeout, this, &MainWindow::renderReceive);
    SerialExtras::Hooks hooks;
    hooks.outstanding = [this] { return qint64(pendingTx.size()) + serial->bytesToWrite(); };
    hooks.snapshot = [this] { return received; };
    hooks.stopAutomatic = [this] {
        autoTimer->stop(); QSignalBlocker block(ui->checkAuto); ui->checkAuto->setChecked(false);
    };
    hooks.enqueue = [this](const QByteArray &bytes) {
        if (!serial->isOpen()) return false;
        const bool idle = pendingTx.isEmpty() && serial->bytesToWrite() == 0;
        if (!SerialData::enqueue(pendingTx, bytes, serial->bytesToWrite(), TxLimit)) return false;
        if (idle) txProgress.restart();
        writeTimer->start(); pumpWrite(); return serial->isOpen();
    };
    auto *files = new SerialExtras(this, serial, hooks);
    connect(files, &SerialExtras::message, this, &MainWindow::report);
    connect(files, &SerialExtras::busyChanged, this, &MainWindow::updateControls);
    connect(files, &SerialExtras::stopRequested, this, [this](const QString &reason) { stopPort(reason, true); });
    refreshPorts();
    report(QStringLiteral("就绪：请核实接口后选择串口；默认 115200 / 8 / 无校验 / 1"));
}

MainWindow::~MainWindow() { delete ui; }

void MainWindow::report(const QString &message, bool error)
{
    ui->labelStatus->setText(QDateTime::currentDateTime().toString(QStringLiteral("[HH:mm:ss] ")) + message);
    ui->labelStatus->setStyleSheet(error ? QStringLiteral("color: #ffb4a9;") : QStringLiteral("color: #9dc9da;"));
    if (error) qWarning().noquote() << message;
    else qInfo().noquote() << message;
}

void MainWindow::refreshPorts()
{
    if (serial->isOpen()) return;  // 已打开时冻结端口及参数，避免显示与实际状态不一致。
    const QString previous = ui->comboSerial->currentData().toString();
    const auto consoles = consolePorts();
    auto ports = QSerialPortInfo::availablePorts();
    std::sort(ports.begin(), ports.end(), [](const QSerialPortInfo &a, const QSerialPortInfo &b) {
        return a.systemLocation() < b.systemLocation();
    });
    auto *model = qobject_cast<QStandardItemModel *>(ui->comboSerial->model());
    int restored = 0, available = 0;
    {
        QSignalBlocker block(ui->comboSerial);
        ui->comboSerial->clear(); ui->comboSerial->addItem(QStringLiteral("请选择串口"), QString());
        for (const auto &port : ports) {
            const QString reason = restriction(port, consoles);
            QString text = port.systemLocation();
            if (!reason.isEmpty()) text += QStringLiteral(" [%1]").arg(reason);
            ui->comboSerial->addItem(text, port.systemLocation());
            const int row = ui->comboSerial->count() - 1;
            if (!reason.isEmpty()) {
                model->item(row)->setEnabled(false); model->item(row)->setSelectable(false);
            } else {
                ++available;
                if (!previous.isEmpty() && previous == port.systemLocation()) restored = row;
            }
            qInfo() << "port" << port.systemLocation() << "restriction" << reason;
        }
        ui->comboSerial->setCurrentIndex(restored);
    }
    updateControls();
    report(!previous.isEmpty() && restored == 0
           ? QStringLiteral("原串口已不可用，已清空选择；请重新核实设备")
           : QStringLiteral("找到 %1 个串口，%2 个候选；当前未打开").arg(ports.size()).arg(available));
}

void MainWindow::updateControls()
{
    const bool opened = serial->isOpen();
    for (QWidget *control : QList<QWidget *>{ui->comboSerial, ui->comboBaudRate,
         ui->comboDataBits, ui->comboParity, ui->comboStopBits, static_cast<QWidget *>(ui->buttonRefresh)})
        control->setEnabled(!opened);
    ui->buttonOpen->setText(opened ? QStringLiteral("关闭串口") : QStringLiteral("打开串口"));
    ui->buttonOpen->setEnabled(opened || !ui->comboSerial->currentData().toString().isEmpty());
    auto *files = findChild<SerialExtras *>();
    const bool fileBusy = files && files->busy();
    ui->buttonSend->setEnabled(opened && !fileBusy);
    ui->checkAuto->setEnabled(opened && !fileBusy);
    if (files) files->refreshControls();
    ui->labelState->setText(opened ? QStringLiteral("串口已打开") : QStringLiteral("串口已关闭"));
    ui->labelState->setStyleSheet(opened ? QStringLiteral("color: #4ee3af;") : QStringLiteral("color: #9bb4c0;"));
}

void MainWindow::togglePort()
{
    if (serial->isOpen()) { stopPort(QStringLiteral("串口已关闭，定时发送已停止")); return; }
    const QString path = ui->comboSerial->currentData().toString();
    bool allowed = false;
    const auto consoles = consolePorts();
    for (const auto &port : QSerialPortInfo::availablePorts()) {
        if (port.systemLocation() == path && restriction(port, consoles).isEmpty()) allowed = true;
    }
    if (!allowed) { refreshPorts(); report(QStringLiteral("设备不存在或受保护，禁止打开"), true); return; }
    serial->setPortName(path);
    opening = true; // 参数设置或open可能同步触发errorOccurred，统一在这里处理失败。
    const bool configured = serial->setBaudRate(ui->comboBaudRate->currentData().toInt())
        && serial->setDataBits(static_cast<QSerialPort::DataBits>(ui->comboDataBits->currentData().toInt()))
        && serial->setParity(static_cast<QSerialPort::Parity>(ui->comboParity->currentData().toInt()))
        && serial->setStopBits(static_cast<QSerialPort::StopBits>(ui->comboStopBits->currentData().toInt()))
        && serial->setFlowControl(QSerialPort::NoFlowControl);
    const bool opened = configured && serial->open(QIODevice::ReadWrite);
    const QString failure = serial->errorString();
    opening = false;
    if (!opened) { stopPort(QStringLiteral("打开失败：%1 | %2").arg(path, failure), true); return; }
    // 重新打开永不恢复自动发送；累计统计持续到退出程序。
    updateControls();
    report(QStringLiteral("已打开 %1 | %2 / %3 / %4 / %5，流控无")
           .arg(path).arg(serial->baudRate()).arg(int(serial->dataBits()))
           .arg(ui->comboParity->currentText(), ui->comboStopBits->currentText()));
}

void MainWindow::stopPort(const QString &reason, bool error)
{
    if (stopping) return;
    stopping = true;
    if (auto *files = findChild<SerialExtras *>()) files->portClosing();
    autoTimer->stop(); writeTimer->stop();
    { QSignalBlocker block(ui->checkAuto); ui->checkAuto->setChecked(false); }
    const qint64 cancelled = pendingTx.size() + serial->bytesToWrite();
    pendingTx.clear();
    if (serial->isOpen()) serial->close();
    stopping = false;
    updateControls(); updateCounters();
    report(reason + (cancelled > 0 ? QStringLiteral("；取消待发送 %1 B").arg(cancelled) : QString()), error);
}

void MainWindow::sendText(bool automatic)
{
    if (auto *files = findChild<SerialExtras *>()) {
        if (files->busy()) { report(QStringLiteral("文件发送中，文本发送暂不可用"), true); return; }
    }
    if (!serial->isOpen()) { report(QStringLiteral("串口未打开，无法发送"), true); return; }
    if (ui->editSend->text().isEmpty()) {
        if (automatic) ui->checkAuto->setChecked(false);
        report(QStringLiteral("发送文本为空"), true); return;
    }
    QByteArray data = ui->editSend->text().toUtf8();
    if (ui->comboEnding->currentIndex() == 1) data.append('\n');
    else if (ui->comboEnding->currentIndex() == 2) data.append("\r\n");
    const qint64 outstanding = pendingTx.size() + serial->bytesToWrite();
    if (!SerialData::enqueue(pendingTx, data, serial->bytesToWrite(), TxLimit)) {
        ui->checkAuto->setChecked(false);
        report(QStringLiteral("发送队列已满（64 KiB），本次未加入；定时发送停止"), true); return;
    }
    if (outstanding == 0) txProgress.restart();
    writeTimer->start();
    if (!automatic) report(QStringLiteral("已加入发送队列 %1 B；TX计数不等于对端确认").arg(data.size()));
    pumpWrite();
}

void MainWindow::pumpWrite()
{
    if (!serial->isOpen() || stopping) return;
    if (pendingTx.isEmpty() && serial->bytesToWrite() == 0) { writeTimer->stop(); return; }
    if (txProgress.isValid() && txProgress.elapsed() > 5000) {
        stopPort(QStringLiteral("发送超过5秒无进展，已关闭串口并停止定时发送"), true); return;
    }
    const qint64 room = DriverWindow - serial->bytesToWrite();
    if (room <= 0 || pendingTx.isEmpty()) return;
    const qint64 requested = qMin<qint64>(room, pendingTx.size());
    const qint64 accepted = serial->write(pendingTx.constData(), requested);
    // 写入可能同步报错并触发stopPort；此时队列已清空，不再修改它。
    if (!serial->isOpen()) return;
    if (accepted < 0) { stopPort(QStringLiteral("发送失败：%1").arg(serial->errorString()), true); return; }
    if (accepted > 0) pendingTx.remove(0, int(accepted));
    // accepted==0或短写保留剩余字节，由bytesWritten/20ms定时器继续。
    updateCounters();
}

void MainWindow::receiveBytes()
{
    const QByteArray bytes = serial->readAll();
    if (bytes.isEmpty()) return;
    rxTotal += quint64(bytes.size());
    if (auto *files = findChild<SerialExtras *>()) files->recordBytes(bytes);
    viewDropped += quint64(SerialData::appendReceived(received, bytes, RxLimit));
    renderingNeeded = true; // readyRead块不是消息；只连续追加字节，显示以10Hz合并刷新。
    updateCounters();
}

void MainWindow::renderReceive()
{
    if (!renderingNeeded) return;
    renderingNeeded = false;
    const QString text = SerialData::format(received, ui->comboDisplay->currentIndex() == 1);
    auto *scroll = ui->textReceive->verticalScrollBar();
    const bool follow = scroll->value() >= scroll->maximum() - 2;
    const int position = scroll->value();
    ui->textReceive->setPlainText(text);
    scroll->setValue(follow ? scroll->maximum() : position);
}

void MainWindow::updateCounters()
{
    ui->labelCounters->setText(QStringLiteral("RX %1 B  |  TX %2 B  |  排队 %3 B")
        .arg(rxTotal).arg(txTotal).arg(pendingTx.size() + serial->bytesToWrite()));
    ui->labelEncoding->setText(QStringLiteral("ASCII控制/非ASCII字节转义；HEX保留字节。视图保留64 KiB，已移除 %1 B。TX为本机写出计数。").arg(viewDropped));
}

void MainWindow::showKeyboard()
{
    // 全尺寸子控件面板由同一个QGraphicsProxyWidget变换；不弹出未旋转的系统窗口。
    if (auto *existing = findChild<QFrame *>(QStringLiteral("keyboardPanel"))) { existing->raise(); return; }
    auto *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("keyboardPanel"));
    panel->setGeometry(rect());
    auto *layout = new QVBoxLayout(panel);
    auto *title = new QLabel(QStringLiteral("发送文本键盘"), panel); layout->addWidget(title);
    auto *edit = new QLineEdit(ui->editSend->text(), panel);
    edit->setObjectName(QStringLiteral("keyboardEdit"));
    edit->setMaxLength(16384); layout->addWidget(edit);
    auto *keys = new QGridLayout; layout->addLayout(keys, 1);
    auto *shift = new QPushButton(QStringLiteral("大写"), panel);
    shift->setCheckable(true); shift->setChecked(true);
    const QStringList rows{QStringLiteral("1234567890"), QStringLiteral("qwertyuiop"),
        QStringLiteral("asdfghjkl"), QStringLiteral("zxcvbnm"), QStringLiteral("-_=+.,/:?@")};
    int row = 0;
    for (const QString &letters : rows) {
        int column = 0;
        for (QChar letter : letters) {
            auto *key = new QPushButton(QString(letter).toUpper(), panel);
            key->setMinimumWidth(0); keys->addWidget(key, row, column++);
            connect(key, &QPushButton::clicked, panel, [edit, shift, letter] {
                edit->insert(shift->isChecked() ? QString(letter).toUpper() : QString(letter));
            });
            connect(shift, &QPushButton::toggled, key, [key, letter](bool upper) {
                key->setText(upper ? QString(letter).toUpper() : QString(letter));
            });
        }
        ++row;
    }
    auto *bar = new QHBoxLayout; layout->addLayout(bar); bar->addWidget(shift);
    auto *space = new QPushButton(QStringLiteral("空格"), panel); bar->addWidget(space);
    auto *back = new QPushButton(QStringLiteral("退格"), panel); bar->addWidget(back);
    auto *clear = new QPushButton(QStringLiteral("清空"), panel); bar->addWidget(clear);
    connect(space, &QPushButton::clicked, edit, [edit] { edit->insert(QStringLiteral(" ")); });
    connect(back, &QPushButton::clicked, edit, &QLineEdit::backspace);
    connect(clear, &QPushButton::clicked, edit, &QLineEdit::clear);
    auto *actions = new QHBoxLayout; layout->addLayout(actions);
    auto *cancel = new QPushButton(QStringLiteral("取消"), panel); actions->addWidget(cancel);
    auto *done = new QPushButton(QStringLiteral("完成"), panel); actions->addWidget(done);
    const auto dismiss = [this, panel] {
        panel->hide(); panel->setObjectName(QStringLiteral("keyboardClosing")); panel->deleteLater();
    };
    connect(cancel, &QPushButton::clicked, panel, dismiss);
    connect(done, &QPushButton::clicked, panel, [this, edit, dismiss] {
        ui->editSend->setText(edit->text()); dismiss();
    });
    panel->setStyleSheet(QStringLiteral("QFrame#keyboardPanel { background: #0b202b; } QPushButton { padding: 6px; }"));
    panel->show(); panel->raise(); edit->setFocus();
}
void MainWindow::closeEvent(QCloseEvent *event)
{
    stopPort(QStringLiteral("退出应用，串口已关闭"));
    event->accept();
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    if (auto *panel = findChild<QFrame *>(QStringLiteral("keyboardPanel"))) panel->setGeometry(rect());
    const bool portrait = height() >= width();
    ui->bodyLayout->setDirection(portrait ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    ui->groupConfig->setMaximumWidth(portrait ? QWIDGETSIZE_MAX : 420);
    ui->bodyLayout->setStretch(0, 0); ui->bodyLayout->setStretch(1, 1);
    const int font = qBound(16, qMin(width(), height()) / 40, 28);
    if (font == styleFontSize) return;
    styleFontSize = font;
    setStyleSheet(QStringLiteral(R"QSS(
QMainWindow, QWidget#centralwidget, QWidget#dataPanel { background: #06151e; }
QWidget { color: #e6f3fa; font-size: %1px; }
QFrame#headerFrame { background: #087caa; border: 1px solid #27b9db; border-radius: 10px; }
QLabel#labelTitle { font-size: %2px; font-weight: bold; }
QLabel#labelHint, QLabel#labelSafety, QLabel#labelEncoding, QLabel#labelFlow { color: #88adbf; font-size: %3px; }
QLabel#labelState { padding: 10px; background: #102b39; border-radius: 6px; }
QGroupBox { border: 1px solid #28566c; border-radius: 10px; margin-top: 18px; padding: 12px 10px 8px; font-weight: bold; }
QGroupBox::title { subcontrol-origin: margin; left: 16px; padding: 0 6px; color: #b8e9fa; }
QComboBox, QLineEdit, QSpinBox, QPlainTextEdit { background: #091e29; border: 1px solid #386c83; border-radius: 6px; padding: 6px; selection-background-color: #087ca5; }
QComboBox, QLineEdit, QSpinBox { min-height: %4px; }
QComboBox { padding-right: 30px; }
QComboBox::drop-down { width: 28px; border-left: 1px solid #386c83; }
QComboBox::down-arrow { image: url(:/icons/down.png); width: 18px; height: 18px; }
QComboBox QAbstractItemView { background: #102b39; color: #e6f3fa; selection-background-color: #087ca5; }
QAbstractItemView::item { min-height: %5px; }
QPushButton { background: #096f91; border: 1px solid #32bddc; border-radius: 7px; padding: 8px 12px; min-height: %4px; }
QPushButton:pressed { background: #0a9ec2; }
QPushButton#buttonSend, QPushButton#buttonOpen { background: #008bb5; font-weight: bold; }
QPushButton#buttonExit { background: #12536d; }
QPushButton#buttonSend:disabled, QPushButton#buttonOpen:disabled, QPushButton:disabled, QComboBox:disabled, QCheckBox:disabled { color: #65818e; background: #102631; border-color: #29414d; }
QCheckBox { spacing: 10px; }
QCheckBox::indicator { width: %4px; height: %4px; border: 1px solid #386c83; background: #091e29; border-radius: 4px; }
QCheckBox::indicator:checked { background: #087ca5; image: url(:/icons/checked.png); }
QScrollBar:vertical { background: #102631; width: 18px; }
QScrollBar::handle:vertical { background: #356d84; min-height: 40px; }
)QSS").arg(font).arg(font + 6).arg(qMax(14, font - 4)).arg(font + 6).arg(font + 24));
}