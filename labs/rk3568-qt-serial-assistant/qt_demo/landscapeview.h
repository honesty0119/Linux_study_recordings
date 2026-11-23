// 仅旋转本应用的Widget场景；Wayland窗口、桌面及系统触摸配置保持原状。
#ifndef LANDSCAPEVIEW_H
#define LANDSCAPEVIEW_H
#include <QGraphicsView>
class MainWindow;
class QGraphicsProxyWidget;
class LandscapeView : public QGraphicsView
{
public:
    explicit LandscapeView(bool rotateLeft = false, QWidget *parent = nullptr);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
private:
    void arrangeContent();
    MainWindow *content;
    QGraphicsProxyWidget *proxy;
    bool rotateLeft;
    bool closing = false;
};
#endif