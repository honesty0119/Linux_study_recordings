// 使用Qt Graphics View统一变换绘制与指针坐标，子控件和下拉弹窗随代理旋转。
#include "landscapeview.h"
#include "mainwindow.h"
#include <QGraphicsScene>
#include <QGraphicsProxyWidget>
#include <QResizeEvent>
#include <QCloseEvent>
#include <QTimer>
#include <QDebug>
#include <QFrame>
#include <QPainter>
#include <QComboBox>
#include <QListView>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPointer>
#include <QScroller>
#include <QMap>

namespace {
// 原生组合框弹窗会混用屏幕/代理坐标；在同一Widget树内提供可滚动选择面板。
class ComboOverlayFilter : public QObject
{
public:
    explicit ComboOverlayFilter(QWidget *page) : QObject(page), page(page)
    {
        page->installEventFilter(this);
        for (auto *combo : page->findChildren<QComboBox *>()) combo->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == page && event->type() == QEvent::Resize && panel)
            panel->setGeometry(page->rect());
        auto *combo = qobject_cast<QComboBox *>(watched);
        if (!combo || !combo->isEnabled()) return QObject::eventFilter(watched, event);
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) { showList(combo); return true; }
        }
        if (event->type() == QEvent::MouseButtonRelease
            && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) return true;
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            if (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return
                || key->key() == Qt::Key_Enter || key->key() == Qt::Key_F4
                || (key->key() == Qt::Key_Down && key->modifiers().testFlag(Qt::AltModifier))) {
                showList(combo); return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }
private:
    void showList(QComboBox *combo)
    {
        if (panel) return;
        auto *overlay = new QFrame(page);
        panel = overlay;
        overlay->setObjectName(QStringLiteral("comboOverlay"));
        overlay->setGeometry(page->rect());
        overlay->setStyleSheet(QStringLiteral(
            "QFrame#comboOverlay { background: #0b202b; }"
            "QListView { background: #091e29; border: 1px solid #386c83; }"
            "QListView::item { min-height: 48px; padding: 4px 12px; }"
            "QListView::item:selected { background: #087ca5; color: #ffffff; }"
            "QListView::item:disabled { color: #718a95; }"));
        auto *layout = new QVBoxLayout(overlay);
        const QMap<QString, QString> names{
            {QStringLiteral("comboSerial"), QStringLiteral("串口设备")},
            {QStringLiteral("comboBaudRate"), QStringLiteral("波特率")},
            {QStringLiteral("comboDataBits"), QStringLiteral("数据位")},
            {QStringLiteral("comboParity"), QStringLiteral("校验位")},
            {QStringLiteral("comboStopBits"), QStringLiteral("停止位")},
            {QStringLiteral("comboDisplay"), QStringLiteral("接收显示模式")},
            {QStringLiteral("comboEnding"), QStringLiteral("发送结尾")}};
        layout->addWidget(new QLabel(QStringLiteral("选择%1").arg(names.value(combo->objectName(), QStringLiteral("选项"))), overlay));
        auto *list = new QListView(overlay);
        list->setObjectName(QStringLiteral("comboOverlayList"));
        list->setModel(combo->model());
        list->setModelColumn(combo->modelColumn());
        list->setEditTriggers(QAbstractItemView::NoEditTriggers);
        list->setSelectionMode(QAbstractItemView::SingleSelection);
        list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list->setCurrentIndex(combo->model()->index(combo->currentIndex(), combo->modelColumn()));
        layout->addWidget(list, 1);
        QScroller::grabGesture(list->viewport(), QScroller::TouchGesture);
        auto *cancel = new QPushButton(QStringLiteral("取消"), overlay); layout->addWidget(cancel);
        const auto dismiss = [this, overlay] {
            overlay->hide(); overlay->deleteLater(); panel = nullptr;
        };
        const auto select = [combo, overlay, dismiss](const QModelIndex &index) {
            if (!overlay->isVisible() || !index.isValid()) return;
            if (!(index.flags() & Qt::ItemIsEnabled) || !(index.flags() & Qt::ItemIsSelectable)) return;
            combo->setCurrentIndex(index.row()); dismiss();
        };
        connect(list, &QListView::clicked, overlay, select);
        connect(list, &QListView::activated, overlay, select);
        connect(cancel, &QPushButton::clicked, overlay, dismiss);
        overlay->show(); overlay->raise(); list->setFocus();
        list->scrollTo(list->currentIndex(), QAbstractItemView::PositionAtCenter);
    }
    QWidget *page;
    QPointer<QFrame> panel;
};
}

LandscapeView::LandscapeView(bool left, QWidget *parent)
    : QGraphicsView(parent), content(new MainWindow), proxy(nullptr), rotateLeft(left)
{
    setWindowTitle(QStringLiteral("RK3568 串口调试助手"));
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setBackgroundBrush(QColor(QStringLiteral("#06151e")));
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
    setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto *canvas = new QGraphicsScene(this);
    setScene(canvas);
    // 场景拥有代理，代理拥有MainWindow，不能再把MainWindow放在栈上。
    proxy = canvas->addWidget(content);
    new ComboOverlayFilter(content);
    content->installEventFilter(this);
    resize(720, 1280);
    QTimer::singleShot(0, this, [this] { arrangeContent(); });
}

void LandscapeView::arrangeContent()
{
    if (!proxy || viewport()->width() <= 0 || viewport()->height() <= 0) return;
    const QSize physical = viewport()->size();
    const bool portraitSurface = physical.height() > physical.width();
    const QSize logical = portraitSurface ? QSize(physical.height(), physical.width()) : physical;
    // MainWindow始终接收横向尺寸，继续复用已有横屏布局和字号逻辑。
    proxy->setPos(0, 0);
    proxy->setScale(1);
    proxy->setRotation(0);
    proxy->resize(logical);
    proxy->setRotation(portraitSurface ? (rotateLeft ? -90 : 90) : 0);
    const QRectF initialBounds = proxy->sceneBoundingRect();
    if (initialBounds.isEmpty()) return;
    // 字体最小尺寸超出可用窗口时只等比缩小，不截掉发送/退出按钮。
    const qreal scale = qMin(qreal(physical.width()) / initialBounds.width(),
                            qreal(physical.height()) / initialBounds.height());
    proxy->setScale(scale);
    const QRectF bounds = proxy->sceneBoundingRect();
    proxy->setPos((physical.width() - bounds.width()) / 2 - bounds.left(),
                  (physical.height() - bounds.height()) / 2 - bounds.top());
    setSceneRect(QRectF(QPointF(0, 0), physical));
    qInfo() << "APP_ONLY_LANDSCAPE surface=" << physical << "content=" << content->size()
            << "rotation=" << proxy->rotation() << "scale=" << scale;
}

void LandscapeView::resizeEvent(QResizeEvent *event)
{
    QGraphicsView::resizeEvent(event);
    arrangeContent();
}

void LandscapeView::closeEvent(QCloseEvent *event)
{
    if (!closing) {
        closing = true;
        // 从窗口管理器关闭外层窗口也必须停止自动发送并关闭串口。
        if (content->isVisible()) content->close();
    }
    event->accept();
}
bool LandscapeView::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == content && event->type() == QEvent::Resize) {
        QTimer::singleShot(0, this, [this] { if (!closing) arrangeContent(); });
    }
    if (watched == content && event->type() == QEvent::Close && !closing) {
        // 先让MainWindow完成串口关闭，再关闭真正的Wayland顶层窗口。
        QTimer::singleShot(0, this, [this] { close(); });
    }
    return QGraphicsView::eventFilter(watched, event);
}