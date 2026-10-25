// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The B2B user interface as plain Qt widgets: a "B2B" menu, a one-line status
// strip, the Host/Join dialog and the "friend wants to take control" prompt.
// It knows nothing about the DJ software it is embedded in: the host app puts
// menu() in its menu bar and statusStrip() in its window, feeds setState(),
// and acts on the signals.

#include <QObject>
#include <QPointer>
#include <QString>

class QAction;
class QActionGroup;
class QLabel;
class QMenu;
class QMessageBox;
class QPushButton;
class QWidget;

namespace b2b::qt {

/// What the user chose in the Host/Join dialog.
struct SessionSettings {
    bool host = true;
    bool useCode = true;   // host: get a join code from the B2B server
    QString peerAddress;   // join: a join code, or "host" / "host:port"
    quint16 port = 9000;   // host: port to listen on
    QString musicFolder;   // tracks to share and to look in first
    QString password;      // by address: the host's generated one, or the one the guest typed
};

/// Handover preferences (kept by the UI, saved in its settings file).
struct HandoverPrefs {
    bool allowTake = false; // the friend may take control without asking
    int catchUpBars = 4;
    bool snap = false;      // jump back on the beat instead of easing
    bool deckEach = true;   // play together, a deck each (instead of taking turns): the default
};

/// A bug report or "send logs" the user asked for.
struct ReportRequest {
    bool logsOnly = false;  // "Send logs": no message
    QString message;        // what happened
    QString contact;        // optional: how to reply
    bool includeLog = true;
};

/// Everything the strip shows.
struct UiState {
    enum class Phase { Idle, Waiting, Connected };
    Phase phase = Phase::Idle;
    bool live = false;
    bool peerAllowsTake = false; // the live friend lets us take without asking
    bool requestPending = false; // we asked, waiting for an answer
    double listenDelayMs = 0;
    double rttMs = 0;
    QString where;               // "hosting on port 9000" / "joined 100.64.0.2"
    QString activity;            // catching up / receiving a track / ...
    QString code;                // host: the join code (or password) to share, shown with Copy
    QString codeSecret;          // host, join code: the session secret that goes with it (shared as
                                 // "CODE / SECRET"; a room hands it over by itself)
    bool codeIsPassword = false; // hosting by address: `code` is the session password
    QString checkCode;           // connected: six digits, the same on both screens
    QString problem;             // something the user should know (shown instead of detail)
    bool deckEach = false;       // playing together, a deck each
    QString myDecks;             // deckEach: "decks 1 and 3"
    double timingMs = 0;         // deckEach: how long after a move you hear it
};

class SessionUi : public QObject {
    Q_OBJECT
  public:
    /// `settingsFile`: an .ini file where the UI remembers its choices.
    explicit SessionUi(const QString& settingsFile, QObject* pParent = nullptr);
    ~SessionUi() override;

    /// Dialogs and prompts are centred on this window (the app's main window).
    void setDialogParent(QWidget* pParent);

    /// The "B2B" menu, to add to the app's menu bar. Owned by SessionUi.
    QMenu* menu() const {
        return m_pMenu;
    }
    /// A one-line status bar, to place in the app's window (the app takes
    /// ownership). Created on first call.
    QWidget* statusStrip();

    void setState(const UiState& state);
    const UiState& state() const {
        return m_state;
    }
    /// The app passes the join code on itself (a room): don't show it.
    void setCodeHidden(bool hidden) {
        m_codeHidden = hidden;
        updateWidgets();
    }
    HandoverPrefs prefs() const {
        return m_prefs;
    }
    /// The friend changed how you play together: show (and remember) it.
    void showDeckEach(bool on);
    /// The app chooses how we play (a room's host dialog, its strip): as if
    /// picked in "How we play" (saved, and the session switches).
    void chooseDeckEach(bool on);
    SessionSettings lastSettings() const {
        return m_last;
    }

    void showRequest();  // "Your friend wants to take control"
    void closeRequest();
    void showMessage(const QString& text); // briefly, in the strip

    /// Opens the Host/Join dialog (also available from the menu).
    void openSessionDialog(bool host);

    /// "Report a bug" and "Send logs" (menu and strip). The app sends the
    /// report and answers with reportFinished().
    void openReportDialog();
    void confirmSendLogs();
    void reportFinished(bool ok, const QString& reference, const QString& error);

  signals:
    void startRequested(b2b::qt::SessionSettings settings);
    void leaveRequested();
    void takeOrRequestClicked();
    void handOverClicked();
    void requestAnswered(bool allow);
    void prefsChanged(b2b::qt::HandoverPrefs prefs);
    void reportRequested(b2b::qt::ReportRequest request);

  private:
    void savePrefs();
    void updateWidgets();
    void copyCode();
    QString sharedCode() const; // the code to give the friend (with its session secret)

    QString m_settingsFile;
    QPointer<QWidget> m_pDialogParent;
    QMenu* m_pMenu;
    QAction* m_pHostAction;
    QAction* m_pJoinAction;
    QAction* m_pLeaveAction;
    QAction* m_pCopyCodeAction;
    QAction* m_pReportAction;
    QAction* m_pLogsAction;
    bool m_reportBusy = false;
    QAction* m_pAllowTakeAction;
    QAction* m_pSmoothAction;
    QAction* m_pSnapAction;
    QAction* m_pTurnsAction = nullptr;
    QAction* m_pDeckEachAction = nullptr;
    QActionGroup* m_pBarsGroup;
    QPointer<QWidget> m_pStrip;
    QPointer<QLabel> m_pDot;
    QPointer<QLabel> m_pStatus;
    QPointer<QLabel> m_pActivity;
    QPointer<QPushButton> m_pButton;
    QPointer<QPushButton> m_pJoinButton;
    QPointer<QPushButton> m_pCopyButton;
    QPointer<QPushButton> m_pReportButton;
    QPointer<QMessageBox> m_pRequestBox;
    QString m_message;
    UiState m_state;
    bool m_codeHidden = false;
    HandoverPrefs m_prefs;
    SessionSettings m_last;
};

} // namespace b2b::qt
