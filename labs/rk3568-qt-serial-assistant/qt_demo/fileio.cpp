// 日志保存原始字节，视图裁剪/HEX切换不影响文件；遇到错误不伪报保存成功。
#include "fileio.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QDateTime>
#include <QUuid>
#include <QTemporaryDir>
#include <QDebug>
namespace FileIO {
bool writeAll(QIODevice &device, const QByteArray &bytes, QString &error)
{
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 n = device.write(bytes.constData() + offset, bytes.size() - offset);
        if (n <= 0) { error = QStringLiteral("写文件失败：%1").arg(device.errorString()); return false; }
        offset += n;
    }
    return true;
}
bool saveSnapshot(const QString &path, const QByteArray &bytes, QString &error)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) { error = file.errorString(); return false; }
    if (!writeAll(file, bytes, error)) { file.cancelWriting(); return false; }
    if (!file.commit()) { error = file.errorString(); return false; }
    return true;
}
bool RawLog::saveMetadata(const QString &state, QString &error)
{
    metadata.insert(QStringLiteral("state"), state);
    metadata.insert(QStringLiteral("bytes"), double(written));
    metadata.insert(QStringLiteral("sha256"), QString::fromLatin1(hash.result().toHex()));
    metadata.insert(QStringLiteral("updated"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    return saveSnapshot(path() + QStringLiteral(".json"), QJsonDocument(metadata).toJson(), error);
}
bool RawLog::begin(const QString &directory, const QJsonObject &details, QString &error, qint64 cap)
{
    if (active()) { error = QStringLiteral("已有接收记录正在进行"); return false; }
    if (!QDir().mkpath(directory)) { error = QStringLiteral("无法创建日志目录：%1").arg(directory); return false; }
    const QString name = QStringLiteral("rx-%1-%2.bin")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")),
             QUuid::createUuid().toString(QUuid::Id128));
    file.setFileName(QDir(directory).filePath(name));
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) { error = file.errorString(); return false; }
    metadata = details; written = 0; limit = cap; hash.reset();
    metadata.insert(QStringLiteral("format"), QStringLiteral("raw serial bytes, no timestamps inserted"));
    metadata.insert(QStringLiteral("started"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    if (!saveMetadata(QStringLiteral("recording"), error)) { file.close(); return false; }
    return true;
}
bool RawLog::append(const QByteArray &bytes, QString &error)
{
    if (!active()) { error = QStringLiteral("日志未开启"); return false; }
    if (written + bytes.size() > limit) { error = QStringLiteral("接收日志已达容量上限，本次数据块未记录"); return false; }
    // QFile也允许短写，记录实际成功写入的前缀，便于磁盘异常时核对。
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 n = file.write(bytes.constData() + offset, bytes.size() - offset);
        if (n <= 0) { error = file.errorString(); return false; }
        hash.addData(bytes.constData() + offset, int(n)); written += n; offset += n;
    }
    // 每块flush可及时发现磁盘错误；不宣称等同于断电持久化fsync。
    if (!file.flush()) { error = file.errorString(); return false; }
    return true;
}
bool RawLog::finish(const QString &reason, QString &error)
{
    if (!active()) return true;
    const bool flushed = file.flush();
    const QString flushError = file.errorString();
    file.close();
    QString metadataError;
    const bool saved = saveMetadata(flushed ? reason : QStringLiteral("flush_error"), metadataError);
    if (!flushed || !saved) { error = !flushed ? flushError : metadataError; return false; }
    return true;
}
bool ChunkedFile::open(const QString &path, QString &error)
{
    close();
    const QFileInfo info(path);
    if (!info.isFile() || !info.isReadable() || info.size() <= 0 || info.size() > 64*1024*1024) {
        error = QStringLiteral("请选择可读的普通文件，大小须为1 B至64 MiB"); return false;
    }
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return false; }
    if (file.isSequential()) { error = QStringLiteral("不支持设备或管道文件"); close(); return false; }
    originalSize = file.size(); offset = 0; hash.reset();
    if (originalSize <= 0 || originalSize > 64*1024*1024) { error = QStringLiteral("文件大小已变化"); close(); return false; }
    return true;
}
QByteArray ChunkedFile::next(int maximum, QString &error)
{
    if (!file.isOpen() || maximum <= 0) { error = QStringLiteral("文件未打开或分块大小无效"); return {}; }
    if (file.size() != originalSize) { error = QStringLiteral("发送期间文件大小发生变化，已中止"); return {}; }
    const qint64 count = qMin<qint64>(maximum, originalSize - offset);
    if (count == 0) return {};
    const QByteArray bytes = file.read(count);
    if (bytes.isEmpty()) { error = QStringLiteral("文件读取失败或提前结束：%1").arg(file.errorString()); return {}; }
    offset += bytes.size(); hash.addData(bytes); return bytes;
}
int selfTest()
{
    class ShortWriter : public QIODevice {
    public:
        QByteArray bytes;
        bool refuse = false;
        ShortWriter() { open(QIODevice::WriteOnly | QIODevice::Unbuffered); }
        qint64 readData(char *, qint64) override { return -1; }
        qint64 writeData(const char *data, qint64 count) override {
            if (refuse) return 0;
            const int accepted = int(qMin<qint64>(7, count)); bytes.append(data, accepted); return accepted;
        }
    };
    int passed = 0, failed = 0;
    const auto check = [&](bool ok, const char *name) { qInfo() << (ok ? "PASS" : "FAIL") << name; ok ? ++passed : ++failed; };
    QTemporaryDir directory;
    if (!directory.isValid()) { qWarning() << "No temporary directory for file self-test"; return 1; }
    QByteArray bytes;
    for (int i = 0; i < 10003; ++i) bytes.append(char(i % 256));
    QString error;
    const QString input = directory.filePath(QStringLiteral("binary.dat"));
    check(saveSnapshot(input, bytes, error), "atomic raw snapshot write");
    ChunkedFile reader;
    check(reader.open(input, error), "open binary transfer source");
    QByteArray reconstructed;
    while (reader.loaded() < reader.size() && error.isEmpty()) reconstructed += reader.next(137, error);
    check(error.isEmpty() && reconstructed == bytes, "file chunks preserve NUL and all byte values");
    check(reader.digest() == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(), "file SHA256 matches actual chunks");
    reader.close();
    RawLog log;
    check(log.begin(directory.path(), QJsonObject(), error, bytes.size()), "start receive log with exact size cap");
    check(log.append(bytes.left(5000), error) && log.append(bytes.mid(5000), error), "receive chunks concatenate without inserted separators");
    check(!log.append(QByteArray("X"), error) && log.size() == bytes.size(), "log capacity rejects whole excess block");
    error.clear();
    check(log.finish(QStringLiteral("self_test"), error), "log closes with metadata");
    QFile saved(log.path()); saved.open(QIODevice::ReadOnly);
    check(saved.readAll() == bytes, "saved log equals original bytes"); saved.close();
    QFile sidecar(log.path() + QStringLiteral(".json")); sidecar.open(QIODevice::ReadOnly);
    const auto metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
    check(metadata.value(QStringLiteral("bytes")).toInt() == bytes.size()
        && metadata.value(QStringLiteral("sha256")).toString().toLatin1() == QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex(), "log sidecar size and SHA256"); sidecar.close();
    error.clear(); check(!reader.open(directory.path(), error), "reject directory as transfer source");
    error.clear(); check(!saveSnapshot(directory.filePath(QStringLiteral("missing/sub/file")), bytes, error), "unwritable destination reports failure");
    ShortWriter shortWriter;
    error.clear(); check(writeAll(shortWriter, bytes, error) && shortWriter.bytes == bytes, "short file writes retain suffix until complete");
    shortWriter.refuse = true;
    error.clear(); check(!writeAll(shortWriter, QByteArray("x"), error), "zero file write fails without infinite retry");
    qInfo() << "FILE_IO_SELF_TEST passed=" << passed << "failed=" << failed;
    return failed ? 1 : 0;
}
}