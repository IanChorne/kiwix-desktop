#ifndef TABWIDGET_H
#define TABWIDGET_H
#include <QTabBar>
#include <QStackedWidget>
#include <memory>
#include "webview.h"
#include "zimview.h"
#include "contentmanagerview.h"
#include "fullscreenwindow.h"
#include <QMouseEvent>
#include <QWebEngineFullScreenRequest>
class TabBar : public QTabBar
{
    Q_OBJECT
public:
    enum class TabType {
        LibraryTab,
        ZimViewTab,
        SettingsTab
    };
    TabBar(QWidget* parent=nullptr);
    void setStackedWidget(QStackedWidget* widget);
    void     setContentManagerView(ContentManagerView* view);
    void     setNewTabButton(QAction* newTabAction);
    ZimView* createNewTab(bool setCurrent, bool adjacentToCurrentTab);
    ZimView* currentZimView() {
        if (mp_stackedWidget && mp_stackedWidget->currentWidget()) {
            return qobject_cast<ZimView*>(mp_stackedWidget->currentWidget());
        }
        return nullptr;
    }
    WebView* currentWebView() {
        if (ZimView* zv = currentZimView())
            return zv->getWebView();
        return nullptr;
    }
    void openUrl(const QUrl &url, bool newTab);
    // Redirect call to sub-webView
    void setTitleOf(const QString& title, ZimView* tab=nullptr);
    void setIconOf(const QIcon& icon, ZimView* tab=nullptr);
    QString currentZimId();
    void triggerWebPageAction(QWebEnginePage::WebAction action, ZimView* widget=nullptr);
    QString currentArticleUrl();
    QString currentArticleTitle();
    virtual QSize tabSizeHint(int index) const override;
    void openFindInPageBar();
    void closeTabsByZimId(const QString &id);
    QStringList getTabUrls() const;
    QStringList getTabZimIds() const;
    // The "+" (new tab) button is implemented as a tab (that is always placed at the end).
    // This function returns the count of real tabs.
    int realTabCount() const;
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *) override;
    void tabRemoved(int index) override;
    void tabInserted(int index) override;
    void resizeEvent(QResizeEvent* event) override;
signals:
    void webActionEnabledChanged(QWebEnginePage::WebAction action, bool enabled);
    void tabDisplayed(TabType tabType);
    void currentTitleChanged(const QString& title);
    void tabRemovedSignal(int index);
    void tabInsertedSignal(int index);
    void sizeChanged();
public slots:
    void closeTab(int index);
    void openHomePage();
    void openOrSwitchToSettingsTab();
    void fullScreenRequested(QWebEngineFullScreenRequest request);
    void on_webview_titleChanged(const QString& title);
    void moveToNextTab();
    void moveToPreviousTab();
    void scrollNextTab();
    void scrollPreviousTab();
private:
    void setCloseTabButton(int index);
    void createUndoButton();
    struct ClosedTabInfo {
        QString url;
        QString title;
        QString zimId;
        QIcon icon;
        QTimer* expiryTimer;
    };
    QList<ClosedTabInfo> m_closedTabs;
    void storeClosedTab(int index);
    void restoreLastClosedTab();
    void positionUndoButton();
private:
    QStackedWidget*     mp_stackedWidget;
    QScopedPointer<FullScreenWindow> m_fullScreenWindow;
    //Trying to add a button for sprint 1
    QToolButton*       m_undoButton = nullptr;
private slots:
    void onTabMoved(int from, int to);
    void onCurrentChanged(int index);
    void onWebviewHistoryActionChanged(QWebEnginePage::WebAction action, bool enabled);
};
#endif // TABWIDGET_H
