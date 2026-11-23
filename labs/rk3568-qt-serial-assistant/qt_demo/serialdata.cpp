// 二进制字节始终保存在QByteArray；不按NUL截断、不把readyRead分块当消息。
#include "serialdata.h"
#include <QDebug>
namespace SerialData {
QString format(const QByteArray &bytes, bool hex)
{
    static const char digits[] = "0123456789ABCDEF";
    QString out;
    out.reserve(bytes.size() * 4);
    for (int i = 0; i < bytes.size(); ++i) {
        const unsigned char byte = static_cast<unsigned char>(bytes.at(i));
        if (hex) {
            if (i) out += i % 16 == 0 ? QLatin1Char('\n') : QLatin1Char(' ');
            out += QLatin1Char(digits[byte >> 4]); out += QLatin1Char(digits[byte & 15]);
        } else if (byte == '\n') out += QLatin1Char('\n');
        else if (byte == '\r') out += QStringLiteral("\\r");
        else if (byte == '\t') out += QStringLiteral("\\t");
        else if (byte == '\\') out += QStringLiteral("\\\\");
        else if (byte >= 0x20 && byte <= 0x7e) out += QLatin1Char(char(byte));
        else {
            out += QStringLiteral("\\x");
            out += QLatin1Char(digits[byte >> 4]); out += QLatin1Char(digits[byte & 15]);
        }
    }
    return out;
}
int appendReceived(QByteArray &buffer, const QByteArray &bytes, int limit)
{
    buffer.append(bytes);
    const int excess = qMax(0, buffer.size() - qMax(0, limit));
    if (excess) buffer.remove(0, excess);
    return excess;
}
bool enqueue(QByteArray &pending, const QByteArray &bytes, qint64 driverPending, int limit)
{
    if (driverPending < 0 || qint64(pending.size()) + driverPending + bytes.size() > limit) return false;
    pending.append(bytes);
    return true;
}
int selfTest()
{
    int failed = 0, passed = 0;
    const auto check = [&failed, &passed](bool ok, const char *name) {
        qInfo() << (ok ? "PASS" : "FAIL") << name;
        if (ok) ++passed; else ++failed;
    };
    const QByteArray boundary = QByteArray::fromHex("00410a0d095c7f80ff");
    check(format(boundary, false) == QStringLiteral("\\x00A\n\\r\\t\\\\\\x7F\\x80\\xFF"), "ASCII controls, NUL, high bytes and backslash");
    check(format(boundary, true) == QStringLiteral("00 41 0A 0D 09 5C 7F 80 FF"), "HEX byte boundaries");
    QByteArray all;
    for (int value = 0; value < 256; ++value) all.append(char(value));
    check(QByteArray::fromHex(format(all, true).toLatin1()) == all, "all 256 bytes HEX round-trip");
    QByteArray split;
    for (int offset = 0; offset < all.size(); offset += 7) appendReceived(split, all.mid(offset, 7), 1024);
    check(split == all, "arbitrary readyRead chunks concatenate identically");
    check(format(split, false) == format(all, false), "chunk boundaries do not change ASCII rendering");
    QByteArray small("abc");
    check(appendReceived(small, QByteArray("defgh"), 5) == 3 && small == "defgh", "bounded RX drops oldest bytes");
    QByteArray pending("abc");
    check(!enqueue(pending, QByteArray("xy"), 4, 8) && pending == "abc", "TX cap counts driver queue without partial enqueue");
    check(enqueue(pending, QByteArray::fromHex("0001"), 3, 8) && pending.toHex() == "6162630001", "TX exact limit preserves binary bytes");
    check(format(QByteArray(), false).isEmpty() && format(QByteArray(), true).isEmpty(), "empty receive buffer");
    check(format(all, true).count(QLatin1Char('\n')) == 15, "HEX wraps at 16 bytes");
    qInfo() << "SERIAL_DATA_SELF_TEST" << "passed=" << passed << "failed=" << failed;
    return failed ? 1 : 0;
}
}