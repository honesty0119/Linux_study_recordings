// 不改变原串口状态机；文件发送与文本/自动发送互斥，取消会请求关闭串口以清空待写数据。
#include "serialextras.h"
#include <QApplication>
#include <QSerialPort>
#include <QTimer>
#include <QPushButton>
#include <QLabel>
#include <QFrame>
#include <QListWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QUuid>
#include <QEvent>
#include <QScroller>
#include <QDebug>

SerialExtras::SerialExtras(QWidget *root, QSerialPort *port, Hooks callbacks)
    : QObject(root), page(root), serial(port), hooks(callbacks), fileTimer(new QTimer(this))
{
    recordButton = root->findChild<QPushButton *>(QStringLiteral("buttonRecord"));
    saveButton = root->findChild<QPushButton *>(QStringLiteral("buttonSaveReceive"));
    fileButton = root->findChild<QPushButton *>(QStringLiteral("buttonFile"));
    cancelButton = root->findChild<QPushButton *>(QStringLiteral("buttonCancelFile"));
    progress = root->findChild<QLabel *>(QStringLiteral("labelTransfer"));
    dataDirectory = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
    page->installEventFilter(this);
    fileTimer->setInterval(20);
    connect(fileTimer, &QTimer::timeout, this, &SerialExtras::refillFile);
    connect(recordButton, &QPushButton::clicked, this, &SerialExtras::toggleRecord);
    connect(saveButton, &QPushButton::clicked, this, &SerialExtras::saveCurrent);
    connect(fileButton, &QPushButton::clicked, this, &SerialExtras::chooseFile);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        if (sending) emit stopRequested(QStringLiteral("取消文件发送，关闭串口并清空尚未写出数据"));
    });
    connect(serial, &QSerialPort::bytesWritten, this, [this](qint64 count) {
        if (sending && count > 0) fileWritten += count;
    });
    refreshControls();
}
SerialExtras::~SerialExtras()
{
    input.close();
    QString error;
    if (!log.finish(QStringLiteral("application_destroyed"), error)) qWarning() << error;
}
bool SerialExtras::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == page && event->type() == QEvent::Resize && browser) browser->setGeometry(page->rect());
    return QObject::eventFilter(watched, event);
}
void SerialExtras::refreshControls()
{
    fileButton->setEnabled(serial->isOpen() && !sending);
    cancelButton->setEnabled(sending);
    recordButton->setEnabled(serial->isOpen() || log.active());
    recordButton->setText(log.active() ? QStringLiteral("停止记录") : QStringLiteral("记录接收"));
    saveButton->setEnabled(true);
}
void SerialExtras::toggleRecord()
{
    if (log.active()) { finishRecord(QStringLiteral("user_stopped")); return; }
    if (!serial->isOpen()) { emit message(QStringLiteral("请先打开串口再记录接收数据"), true); return; }
    QJsonObject metadata;
    metadata.insert(QStringLiteral("port"), serial->portName());
    metadata.insert(QStringLiteral("baud"), serial->baudRate());
    metadata.insert(QStringLiteral("data_bits"), int(serial->dataBits()));
    metadata.insert(QStringLiteral("parity_enum"), int(serial->parity()));
    metadata.insert(QStringLiteral("stop_bits_enum"), int(serial->stopBits()));
    QString error;
    if (!log.begin(QDir(dataDirectory).filePath(QStringLiteral("received")), metadata, error)) {
        emit message(QStringLiteral("无法开始接收记录：%1").arg(error), true); return;
    }
    refreshControls();
    emit message(QStringLiteral("记录已开始（仅后续接收、单文件64 MiB上限）：%1").arg(log.path()), false);
}
void SerialExtras::finishRecord(const QString &reason)
{
    if (!log.active()) return;
    QString error;
    const bool success = log.finish(reason, error);
    refreshControls();
    emit message(success ? QStringLiteral("接收记录已停止，%1 B：%2").arg(log.size()).arg(log.path())
                         : QStringLiteral("接收记录收尾失败：%1；已写数据保留于%2").arg(error, log.path()), !success);
}
void SerialExtras::recordBytes(const QByteArray &bytes)
{
    if (!log.active()) return;
    QString error;
    if (!log.append(bytes, error)) {
        const QString cause = error;
        finishRecord(QStringLiteral("recording_error"));
        emit message(QStringLiteral("接收记录停止：%1；串口接收继续").arg(cause), true);
    }
}
void SerialExtras::saveCurrent()
{
    const QByteArray bytes = hooks.snapshot();
    if (bytes.isEmpty()) { emit message(QStringLiteral("接收视图为空，暂无数据可保存"), true); return; }
    const QString directory = QDir(dataDirectory).filePath(QStringLiteral("received"));
    if (!QDir().mkpath(directory)) { emit message(QStringLiteral("无法创建接收保存目录"), true); return; }
    const QString path = QDir(directory).filePath(QStringLiteral("snapshot-%1.bin").arg(QUuid::createUuid().toString(QUuid::Id128)));
    QString error;
    if (!FileIO::saveSnapshot(path, bytes, error)) { emit message(QStringLiteral("保存失败：%1").arg(error), true); return; }
    emit message(QStringLiteral("已保存当前保留的原始接收字节 %1 B（不含已裁剪数据）：%2").arg(bytes.size()).arg(path), false);
}
void SerialExtras::portClosing()
{
    fileTimer->stop();
    if (sending) {
        input.close(); sending = false;
        progress->setText(QStringLiteral("文件发送已中止：本机写出 %1 / %2 B").arg(fileWritten).arg(input.size()));
        emit busyChanged();
    }
    finishRecord(QStringLiteral("port_closed"));
    refreshControls();
}
void SerialExtras::startFile(const QString &path)
{
    if (!serial->isOpen() || sending) { emit message(QStringLiteral("串口未打开或文件发送仍在进行"), true); return; }
    if (hooks.outstanding() != 0) { emit message(QStringLiteral("请等待现有发送队列排空后再发送文件"), true); return; }
    QString error;
    if (!input.open(path, error)) { emit message(error, true); return; }
    hooks.stopAutomatic();
    fileWritten = 0; sending = true;
    fileTimer->start(); refreshControls(); emit busyChanged();
    emit message(QStringLiteral("开始发送文件 %1（%2 B），按原始字节发送，不追加结尾").arg(path).arg(input.size()), false);
    refillFile();
}
void SerialExtras::refillFile()
{
    if (!sending) return;
    if (!serial->isOpen()) { portClosing(); return; }
    progress->setText(QStringLiteral("文件本机写出 %1 / %2 B；对端接收须另行核对").arg(fileWritten).arg(input.size()));
    if (input.loaded() == input.size()) {
        if (hooks.outstanding() == 0 && fileWritten >= input.size()) {
            const auto digest = QString::fromLatin1(input.digest());
            input.close(); sending = false; fileTimer->stop(); refreshControls(); emit busyChanged();
            progress->setText(QStringLiteral("文件本机写出完成：%1 B").arg(fileWritten));
            emit message(QStringLiteral("文件本机写出完成，SHA256=%1；仍需对端比对").arg(digest), false);
        }
        return;
    }
    // 只有应用+驱动队列为空才读取下一块，内存占用与文件总大小无关。
    if (hooks.outstanding() > 0) return;
    QString error;
    const QByteArray bytes = input.next(4096, error);
    if (!error.isEmpty() || bytes.isEmpty()) {
        emit stopRequested(QStringLiteral("文件读取失败：%1").arg(error)); return;
    }
    if (!hooks.enqueue(bytes)) emit stopRequested(QStringLiteral("文件数据无法加入发送队列，已中止"));
}
void SerialExtras::chooseFile()
{
    if (browser || sending || !serial->isOpen()) return;
    auto *panel = new QFrame(page); browser = panel;
    panel->setObjectName(QStringLiteral("fileBrowser")); panel->setGeometry(page->rect());
    panel->setStyleSheet(QStringLiteral("QFrame#fileBrowser {background:#0b202b;} QListWidget {background:#091e29;} QListWidget::item {min-height:48px;}"));
    auto *layout = new QVBoxLayout(panel);
    auto *pathLabel = new QLabel(panel); pathLabel->setWordWrap(true); pathLabel->setTextFormat(Qt::PlainText); layout->addWidget(pathLabel);
    auto *list = new QListWidget(panel); layout->addWidget(list,1);
    QScroller::grabGesture(list->viewport(),QScroller::TouchGesture);
    auto *selection = new QLabel(QStringLiteral("选择文件后再点击发送；仅支持1 B–64 MiB普通文件"),panel);
    selection->setWordWrap(true); selection->setTextFormat(Qt::PlainText); layout->addWidget(selection);
    auto *bar = new QHBoxLayout; layout->addLayout(bar);
    auto *up = new QPushButton(QStringLiteral("上级目录"),panel); bar->addWidget(up);
    auto *cancel = new QPushButton(QStringLiteral("取消"),panel); bar->addWidget(cancel);
    auto *send = new QPushButton(QStringLiteral("发送所选文件"),panel); send->setEnabled(false); bar->addWidget(send);
    // 属性存储当前目录和选择，生命周期由面板管理，不捕获栈上引用。
    const auto populate = [panel,list,pathLabel,send,selection](const QString &path) {
        const QDir directory(path);
        panel->setProperty("directory",directory.absolutePath()); panel->setProperty("selectedFile",QString());
        pathLabel->setText(QStringLiteral("发送文件：%1").arg(directory.absolutePath()));
        list->clear(); send->setEnabled(false); selection->setText(QStringLiteral("请选择文件；目录项可以进入"));
        for (const auto &info : directory.entryInfoList(QDir::Dirs|QDir::Files|QDir::NoDotAndDotDot|QDir::Readable,QDir::DirsFirst|QDir::Name)) {
            if (!info.isDir() && !info.isFile()) continue;
            auto *item = new QListWidgetItem(info.isDir() ? QStringLiteral("[目录] %1").arg(info.fileName())
                                                        : QStringLiteral("%1  (%2 B)").arg(info.fileName()).arg(info.size()),list);
            item->setData(Qt::UserRole,info.absoluteFilePath()); item->setData(Qt::UserRole+1,info.isDir());
        }
    };
    const auto dismiss = [this,panel] { panel->hide(); panel->deleteLater(); browser=nullptr; };
    connect(cancel,&QPushButton::clicked,panel,dismiss);
    connect(up,&QPushButton::clicked,panel,[panel,populate] { QDir directory(panel->property("directory").toString()); if(directory.cdUp()) populate(directory.absolutePath()); });
    connect(list,&QListWidget::itemClicked,panel,[panel,populate,selection,send](QListWidgetItem *item) {
        const QString path=item->data(Qt::UserRole).toString();
        if(item->data(Qt::UserRole+1).toBool()) { populate(path); return; }
        panel->setProperty("selectedFile",path); selection->setText(QStringLiteral("已选：%1").arg(path)); send->setEnabled(true);
    });
    connect(send,&QPushButton::clicked,panel,[this,panel,dismiss] { const QString path=panel->property("selectedFile").toString(); dismiss(); startFile(path); });
    const QString directory=QDir(dataDirectory).filePath(QStringLiteral("send"));
    if(!QDir().mkpath(directory)) populate(QCoreApplication::applicationDirPath()); else populate(directory);
    panel->show(); panel->raise(); list->setFocus();
}