// 原始接收日志、原子快照和有界文件读取；同一实现供GUI与文件自检使用。
#ifndef FILEIO_H
#define FILEIO_H
#include <QFile>
#include <QJsonObject>
#include <QCryptographicHash>
class QIODevice;
namespace FileIO {
bool writeAll(QIODevice &device, const QByteArray &bytes, QString &error);
bool saveSnapshot(const QString &path, const QByteArray &bytes, QString &error);
class RawLog {
public:
    bool begin(const QString &directory, const QJsonObject &metadata, QString &error, qint64 limit = 64*1024*1024);
    bool append(const QByteArray &bytes, QString &error);
    bool finish(const QString &reason, QString &error);
    bool active() const { return file.isOpen(); }
    QString path() const { return file.fileName(); }
    qint64 size() const { return written; }
private:
    bool saveMetadata(const QString &state, QString &error);
    QFile file;
    QJsonObject metadata;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    qint64 written = 0;
    qint64 limit = 0;
};
class ChunkedFile {
public:
    bool open(const QString &path, QString &error);
    QByteArray next(int maximum, QString &error);
    void close() { file.close(); }
    qint64 size() const { return originalSize; }
    qint64 loaded() const { return offset; }
    QByteArray digest() const { return hash.result().toHex(); }
private:
    QFile file;
    qint64 originalSize = 0, offset = 0;
    QCryptographicHash hash{QCryptographicHash::Sha256};
};
int selfTest();
}
#endif