// SPDX-License-Identifier: GPL-2.0-or-later
#include "b2b/qt/session_ui.hpp"

#include <algorithm>

#include <QAction>
#include <QActionGroup>
#include <QButtonGroup>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QCheckBox>
#include <QMessageBox>
#include <QMetaMethod>
#include <QPlainTextEdit>
#include <QNetworkInterface>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "b2b/session.hpp"

namespace b2b::qt {

namespace {

const char* const kStripStyle = R"(
#B2BStrip { background: #191a1c; border-top: 1px solid #2b2d31; border-bottom: 1px solid #0c0c0d; }
#B2BStrip QLabel { color: #d6d6d6; }
#B2BStrip QLabel#B2BActivity, #B2BStrip QLabel#B2BMessage { color: #8d9096; }
#B2BStrip QLabel#B2BProblem { color: #e3a857; }
#B2BStrip QPushButton { color: #e8e8e8; background: #2c2e33; border: 1px solid #44474e;
                        border-radius: 3px; padding: 2px 12px; }
#B2BStrip QPushButton:hover { background: #383b41; }
#B2BStrip QPushButton:disabled { color: #777; }
#B2BStrip QPushButton#B2BLive { background: #7a1f24; border-color: #b0333a; }
#B2BStrip QPushButton#B2BLive:hover { background: #8f262c; }
)";

// Addresses a friend could use to reach this computer, Tailscale (100.x) first.
QStringList localAddresses() {
    QStringList tailscale;
    QStringList other;
    for (const QHostAddress& a : QNetworkInterface::allAddresses()) {
        if (a.protocol() != QAbstractSocket::IPv4Protocol || a.isLoopback()) {
            continue;
        }
        const QString s = a.toString();
        (s.startsWith(QStringLiteral("100.")) ? tailscale : other).append(s);
    }
    return tailscale + other;
}

// A folder chooser row: line edit + "Browse…".
QWidget* folderRow(QLineEdit* pEdit, QWidget* pParent) {
    auto* pRow = new QWidget(pParent);
    auto* pLayout = new QHBoxLayout(pRow);
    pLayout->setContentsMargins(0, 0, 0, 0);
    pLayout->addWidget(pEdit, 1);
    auto* pBrowse = new QPushButton(QObject::tr("Browse…"), pRow);
    pLayout->addWidget(pBrowse);
    QObject::connect(pBrowse, &QPushButton::clicked, pRow, [pEdit, pRow] {
        const QString dir = QFileDialog::getExistingDirectory(pRow,
                QObject::tr("Music folder for this session"),
                pEdit->text().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::MusicLocation)
                                        : pEdit->text());
        if (!dir.isEmpty()) {
            pEdit->setText(QDir::toNativeSeparators(dir));
        }
    });
    return pRow;
}

} // namespace

SessionUi::SessionUi(const QString& settingsFile, QObject* pParent)
        : QObject(pParent),
          m_settingsFile(settingsFile) {
    QSettings s(m_settingsFile, QSettings::IniFormat);
    m_prefs.allowTake = s.value(QStringLiteral("allow_take"), false).toBool();
    m_prefs.catchUpBars = std::clamp(s.value(QStringLiteral("catch_up_bars"), 4).toInt(), 1, 32);
    m_prefs.snap = s.value(QStringLiteral("handover_style")).toString() == QStringLiteral("snap");
    // "A deck each" is how PitchRide plays unless you choose to take turns.
    // Earlier versions saved "turns" whenever any setting changed, chosen or
    // not: once, everyone starts from the new default.
    if (s.value(QStringLiteral("play_mode_default")).toInt() < 2) {
        s.setValue(QStringLiteral("play_mode"), QStringLiteral("deck_each"));
        s.setValue(QStringLiteral("play_mode_default"), 2);
    }
    m_prefs.deckEach = s.value(QStringLiteral("play_mode"), QStringLiteral("deck_each")).toString() ==
            QStringLiteral("deck_each");
    m_last.host = s.value(QStringLiteral("last_host"), true).toBool();
    m_last.useCode = s.value(QStringLiteral("use_code"), true).toBool();
    m_last.peerAddress = s.value(QStringLiteral("last_peer")).toString();
    m_last.port = static_cast<quint16>(s.value(QStringLiteral("last_port"), 9000).toUInt());
    m_last.musicFolder = s.value(QStringLiteral("music_folder"),
                                  QStandardPaths::writableLocation(QStandardPaths::MusicLocation))
                                 .toString();

    m_pMenu = new QMenu(tr("&B2B"));
    m_pHostAction = m_pMenu->addAction(tr("&Host a session…"), this, [this] { openSessionDialog(true); });
    m_pJoinAction = m_pMenu->addAction(tr("&Join a session…"), this, [this] { openSessionDialog(false); });
    m_pLeaveAction = m_pMenu->addAction(tr("&Leave session"), this, [this] { emit leaveRequested(); });
    m_pCopyCodeAction = m_pMenu->addAction(tr("&Copy join code"), this, [this] { copyCode(); });
    m_pMenu->addSeparator();

    QMenu* pMode = m_pMenu->addMenu(tr("How we &play"));
    auto* pModes = new QActionGroup(this);
    m_pTurnsAction = pMode->addAction(tr("Take turns: no delay for whoever is live"));
    m_pDeckEachAction = pMode->addAction(tr("A deck each: play at the same time"));
    for (QAction* a : {m_pTurnsAction, m_pDeckEachAction}) {
        a->setCheckable(true);
        pModes->addAction(a);
    }
    m_pTurnsAction->setToolTip(tr("One of you is live at a time and hears their moves instantly"));
    m_pDeckEachAction->setToolTip(tr("The host plays decks 1 and 3, the guest decks 2 and 4. Both of you hear "
                                     "every move at the same moment, a few milliseconds after you make it"));
    pMode->setToolTipsVisible(true);
    (m_prefs.deckEach ? m_pDeckEachAction : m_pTurnsAction)->setChecked(true);
    connect(pModes, &QActionGroup::triggered, this, [this](QAction* a) {
        m_prefs.deckEach = a == m_pDeckEachAction;
        savePrefs();
    });

    QMenu* pHandover = m_pMenu->addMenu(tr("When I hand &over"));
    auto* pStyle = new QActionGroup(this);
    m_pSmoothAction = pHandover->addAction(tr("Ease into my friend's timing"));
    m_pSnapAction = pHandover->addAction(tr("Jump back on the beat (instant)"));
    for (QAction* a : {m_pSmoothAction, m_pSnapAction}) {
        a->setCheckable(true);
        pStyle->addAction(a);
    }
    (m_prefs.snap ? m_pSnapAction : m_pSmoothAction)->setChecked(true);
    connect(pStyle, &QActionGroup::triggered, this, [this](QAction* a) {
        m_prefs.snap = a == m_pSnapAction;
        savePrefs();
    });
    QMenu* pBars = pHandover->addMenu(tr("Easing length"));
    m_pBarsGroup = new QActionGroup(this);
    for (int bars : {1, 2, 4, 8, 16}) {
        QAction* a = pBars->addAction(tr("%n bar(s)", nullptr, bars));
        a->setCheckable(true);
        a->setData(bars);
        a->setChecked(bars == m_prefs.catchUpBars);
        m_pBarsGroup->addAction(a);
    }
    connect(m_pBarsGroup, &QActionGroup::triggered, this, [this](QAction* a) {
        m_prefs.catchUpBars = a->data().toInt();
        savePrefs();
    });
    m_pAllowTakeAction = m_pMenu->addAction(tr("Let my friend take control without asking"));
    m_pAllowTakeAction->setCheckable(true);
    m_pAllowTakeAction->setChecked(m_prefs.allowTake);
    connect(m_pAllowTakeAction, &QAction::toggled, this, [this](bool on) {
        m_prefs.allowTake = on;
        savePrefs();
    });
    m_pMenu->addSeparator();
    QAction* pFriends = m_pMenu->addAction(tr("Friends and invites (coming soon)"));
    pFriends->setEnabled(false);
    m_pMenu->addSeparator();
    m_pReportAction = m_pMenu->addAction(tr("&Report a bug or feedback…"), this, [this] { openReportDialog(); });
    m_pLogsAction = m_pMenu->addAction(tr("Send &logs to the developers…"), this, [this] { confirmSendLogs(); });

    updateWidgets();
}

SessionUi::~SessionUi() {
    delete m_pMenu;
}

void SessionUi::setDialogParent(QWidget* pParent) {
    m_pDialogParent = pParent;
}

void SessionUi::savePrefs() {
    QSettings s(m_settingsFile, QSettings::IniFormat);
    s.setValue(QStringLiteral("allow_take"), m_prefs.allowTake);
    s.setValue(QStringLiteral("catch_up_bars"), m_prefs.catchUpBars);
    s.setValue(QStringLiteral("handover_style"), m_prefs.snap ? QStringLiteral("snap") : QStringLiteral("smooth"));
    s.setValue(QStringLiteral("play_mode"), m_prefs.deckEach ? QStringLiteral("deck_each") : QStringLiteral("turns"));
    emit prefsChanged(m_prefs);
}

void SessionUi::showDeckEach(bool on) {
    if (m_prefs.deckEach == on) {
        return;
    }
    m_prefs.deckEach = on;
    (on ? m_pDeckEachAction : m_pTurnsAction)->setChecked(true);
    QSettings s(m_settingsFile, QSettings::IniFormat);
    s.setValue(QStringLiteral("play_mode"), on ? QStringLiteral("deck_each") : QStringLiteral("turns"));
}

void SessionUi::chooseDeckEach(bool on) {
    showDeckEach(on);
    savePrefs(); // prefsChanged: the session switches, and tells the friend
}

QWidget* SessionUi::statusStrip() {
    if (m_pStrip) {
        return m_pStrip;
    }
    auto* pStrip = new QWidget();
    pStrip->setObjectName(QStringLiteral("B2BStrip"));
    pStrip->setAttribute(Qt::WA_StyledBackground, true);
    pStrip->setStyleSheet(QString::fromLatin1(kStripStyle));
    auto* pLayout = new QHBoxLayout(pStrip);
    pLayout->setContentsMargins(10, 3, 6, 3);
    pLayout->setSpacing(10);

    m_pDot = new QLabel(QStringLiteral("●"), pStrip);
    pLayout->addWidget(m_pDot);
    m_pStatus = new QLabel(pStrip);
    m_pStatus->setTextFormat(Qt::PlainText); // track names and errors from elsewhere are never HTML
    QFont bold = m_pStatus->font();
    bold.setBold(true);
    m_pStatus->setFont(bold);
    pLayout->addWidget(m_pStatus);
    m_pActivity = new QLabel(pStrip);
    m_pActivity->setTextFormat(Qt::PlainText);
    m_pActivity->setObjectName(QStringLiteral("B2BActivity"));
    m_pActivity->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    pLayout->addWidget(m_pActivity, 1);

    m_pCopyButton = new QPushButton(tr("Copy code"), pStrip);
    pLayout->addWidget(m_pCopyButton);
    connect(m_pCopyButton, &QPushButton::clicked, this, [this] { copyCode(); });

    m_pReportButton = new QPushButton(tr("Report a bug"), pStrip);
    m_pReportButton->setToolTip(tr("Tell us what went wrong. Your message and the app's log go to the developers"));
    pLayout->addWidget(m_pReportButton);
    connect(m_pReportButton, &QPushButton::clicked, this, [this] { openReportDialog(); });

    m_pJoinButton = new QPushButton(tr("Join…"), pStrip);
    pLayout->addWidget(m_pJoinButton);
    connect(m_pJoinButton, &QPushButton::clicked, this, [this] {
        if (m_state.phase == UiState::Phase::Idle) {
            openSessionDialog(false);
        } else {
            emit leaveRequested();
        }
    });
    m_pButton = new QPushButton(pStrip);
    m_pButton->setMinimumWidth(150);
    pLayout->addWidget(m_pButton);
    connect(m_pButton, &QPushButton::clicked, this, [this] {
        if (m_state.phase == UiState::Phase::Idle) {
            openSessionDialog(true);
        } else if (m_state.live) {
            emit handOverClicked();
        } else {
            emit takeOrRequestClicked();
        }
    });
    m_pStrip = pStrip;
    updateWidgets();
    return pStrip;
}

void SessionUi::setState(const UiState& state) {
    m_state = state;
    updateWidgets();
}

void SessionUi::updateWidgets() {
    const bool idle = m_state.phase == UiState::Phase::Idle;
    const bool connected = m_state.phase == UiState::Phase::Connected;
    m_pHostAction->setEnabled(idle);
    m_pJoinAction->setEnabled(idle);
    m_pLeaveAction->setEnabled(!idle);
    m_pCopyCodeAction->setEnabled(!idle && !m_state.code.isEmpty());
    m_pReportAction->setEnabled(!m_reportBusy);
    m_pLogsAction->setEnabled(!m_reportBusy);
    if (m_pReportButton) {
        m_pReportButton->setEnabled(!m_reportBusy);
        m_pReportButton->setText(m_reportBusy ? tr("Sending…") : tr("Report a bug"));
    }
    if (!m_pStrip) {
        return;
    }

    QString dotColor = QStringLiteral("#5c5f66");
    QString status;
    QString detail;
    if (idle) {
        status = tr("B2B: not in a session");
    } else if (!connected) {
        dotColor = QStringLiteral("#d9a441");
        if (!m_state.code.isEmpty() && m_state.codeIsPassword) {
            status = tr("Session password: %1").arg(m_state.code);
            detail = tr("Your friend joins with your address and this password");
        } else if (m_codeHidden) {
            status = tr("Linking your decks…");
            detail = tr("Your friend's app joins by itself");
        } else if (!m_state.code.isEmpty()) {
            status = tr("Your join code: %1").arg(sharedCode());
            detail = tr("Send it to your friend, then wait for them to join");
        } else {
            status = tr("Waiting for your friend…");
            detail = m_state.where;
        }
    } else if (m_state.deckEach) {
        dotColor = QStringLiteral("#3ddc84");
        status = tr("A deck each: you play %1").arg(m_state.myDecks);
        detail = tr("%1 · you both hear every move %2 ms after it's made · round trip %3 ms")
                         .arg(m_state.where)
                         .arg(m_state.timingMs, 0, 'f', 0)
                         .arg(m_state.rttMs, 0, 'f', 0);
    } else if (m_state.live) {
        dotColor = QStringLiteral("#e0383e");
        status = tr("You're live");
        detail = tr("%1 · round trip %2 ms").arg(m_state.where).arg(m_state.rttMs, 0, 'f', 0);
    } else {
        dotColor = QStringLiteral("#3d8bfd");
        status = tr("Your friend is live");
        detail = tr("%1 · you hear them %2 ms later · prepare decks that aren't playing")
                         .arg(m_state.where)
                         .arg(m_state.listenDelayMs, 0, 'f', 0);
    }
    if (!m_state.activity.isEmpty()) {
        detail = m_state.activity;
    }
    const bool problem = !idle && !m_state.problem.isEmpty();
    if (problem) {
        detail = m_state.problem;
    }
    if (!m_message.isEmpty()) {
        detail = m_message;
    }
    m_pDot->setStyleSheet(QStringLiteral("color: %1; font-size: 14px;").arg(dotColor));
    if (connected && !m_state.checkCode.isEmpty()) {
        // Read it out: the same on both screens means nobody is in between.
        status += tr("  ·  check %1").arg(m_state.checkCode);
    }
    m_pStatus->setText(status);
    m_pActivity->setText(detail);
    m_pActivity->setToolTip(detail);
    m_pActivity->setObjectName(problem && m_message.isEmpty() ? QStringLiteral("B2BProblem")
                                                              : QStringLiteral("B2BActivity"));
    m_pActivity->style()->unpolish(m_pActivity);
    m_pActivity->style()->polish(m_pActivity);
    m_pCopyButton->setVisible(!idle && !connected && !m_state.code.isEmpty() && !m_codeHidden);
    m_pCopyButton->setText(m_state.codeIsPassword ? tr("Copy password") : tr("Copy code"));

    m_pJoinButton->setVisible(true);
    m_pJoinButton->setText(idle ? tr("Join…") : tr("Leave"));
    m_pButton->setObjectName(QString());
    m_pButton->setVisible(!(connected && m_state.deckEach)); // nothing to hand over
    if (idle) {
        m_pButton->setText(tr("Host…"));
        m_pButton->setEnabled(true);
    } else if (!connected) {
        m_pButton->setText(tr("Take control"));
        m_pButton->setEnabled(false);
    } else if (m_state.live) {
        m_pButton->setText(tr("Hand over"));
        m_pButton->setObjectName(QStringLiteral("B2BLive"));
        m_pButton->setEnabled(true);
    } else if (m_state.requestPending) {
        m_pButton->setText(tr("Asking…"));
        m_pButton->setEnabled(false);
    } else {
        m_pButton->setText(m_state.peerAllowsTake ? tr("Take control") : tr("Ask to take control"));
        m_pButton->setEnabled(true);
    }
    // Re-apply the style sheet so the objectName-based rule takes effect.
    m_pButton->style()->unpolish(m_pButton);
    m_pButton->style()->polish(m_pButton);
}

namespace {

QString privacyNote() {
    return QObject::tr("The log lists what the app did: session events, the file names of tracks you "
                       "loaded, and network addresses. It's encrypted on your computer before it's sent, "
                       "and only the developers can read it.");
}

} // namespace

void SessionUi::openReportDialog() {
    if (m_reportBusy) {
        return;
    }
    QDialog dialog(m_pDialogParent);
    dialog.setWindowTitle(tr("Report a bug or feedback"));
    dialog.setMinimumWidth(480);
    auto* pLayout = new QVBoxLayout(&dialog);
    auto* pIntro = new QLabel(tr("Found a bug, or have an idea? For a bug: what happened, what were you doing just before, and what did you expect? "
                                        "For feedback: anything you liked, missed or would change."), &dialog);
    pIntro->setWordWrap(true);
    pLayout->addWidget(pIntro);
    auto* pMessage = new QPlainTextEdit(&dialog);
    pMessage->setPlaceholderText(tr("e.g. My friend's deck 2 jumped forward when I pressed cue. Or: I'd love a way to save our setlist."));
    pMessage->setMinimumHeight(140);
    pLayout->addWidget(pMessage);
    auto* pForm = new QFormLayout();
    auto* pContact = new QLineEdit(QSettings(m_settingsFile, QSettings::IniFormat)
                                           .value(QStringLiteral("report_contact"))
                                           .toString(),
            &dialog);
    pContact->setPlaceholderText(tr("optional, so we can ask you about it"));
    pForm->addRow(tr("Email or Discord:"), pContact);
    pLayout->addLayout(pForm);
    auto* pIncludeLog = new QCheckBox(tr("Include the app's log (helps us a lot)"), &dialog);
    pIncludeLog->setChecked(true);
    pLayout->addWidget(pIncludeLog);
    auto* pNote = new QLabel(privacyNote(), &dialog);
    pNote->setWordWrap(true);
    QFont small = pNote->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    pNote->setFont(small);
    pLayout->addWidget(pNote);
    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
    QPushButton* pSend = pButtons->addButton(tr("Send"), QDialogButtonBox::AcceptRole);
    pSend->setEnabled(false);
    connect(pMessage, &QPlainTextEdit::textChanged, &dialog, [pMessage, pSend] {
        pSend->setEnabled(!pMessage->toPlainText().trimmed().isEmpty());
    });
    connect(pButtons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    pLayout->addWidget(pButtons);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    ReportRequest request;
    request.message = pMessage->toPlainText().trimmed();
    request.contact = pContact->text().trimmed();
    request.includeLog = pIncludeLog->isChecked();
    QSettings(m_settingsFile, QSettings::IniFormat).setValue(QStringLiteral("report_contact"), request.contact);
    if (!isSignalConnected(QMetaMethod::fromSignal(&SessionUi::reportRequested))) {
        // The app around this UI is older than the UI (only half of it was updated).
        QMessageBox::warning(m_pDialogParent,
                tr("Couldn't send"),
                tr("This build can't send reports: part of the app is out of date. "
                   "Update both the app and b2b-core, then rebuild."));
        return;
    }
    m_reportBusy = true;
    updateWidgets();
    emit reportRequested(request);
}

void SessionUi::confirmSendLogs() {
    if (m_reportBusy) {
        return;
    }
    const auto answer = QMessageBox::question(m_pDialogParent,
            tr("Send logs"),
            tr("Send this app's log to the developers?") + QStringLiteral("\n\n") + privacyNote(),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Yes);
    if (answer != QMessageBox::Yes) {
        return;
    }
    ReportRequest request;
    request.logsOnly = true;
    request.contact = QSettings(m_settingsFile, QSettings::IniFormat).value(QStringLiteral("report_contact")).toString();
    if (!isSignalConnected(QMetaMethod::fromSignal(&SessionUi::reportRequested))) {
        // The app around this UI is older than the UI (only half of it was updated).
        QMessageBox::warning(m_pDialogParent,
                tr("Couldn't send"),
                tr("This build can't send reports: part of the app is out of date. "
                   "Update both the app and b2b-core, then rebuild."));
        return;
    }
    m_reportBusy = true;
    updateWidgets();
    emit reportRequested(request);
}

void SessionUi::reportFinished(bool ok, const QString& reference, const QString& error) {
    m_reportBusy = false;
    updateWidgets();
    if (ok) {
        QMessageBox::information(m_pDialogParent,
                tr("Sent"),
                tr("Thanks, it's with the developers.\n\nIf you talk to them about it, mention reference %1.")
                        .arg(reference));
    } else {
        QMessageBox::warning(m_pDialogParent, tr("Couldn't send"), error);
    }
}

QString SessionUi::sharedCode() const {
    // A join code goes with its session secret: the friend needs both.
    return m_state.codeIsPassword || m_state.codeSecret.isEmpty()
            ? m_state.code
            : m_state.code + QStringLiteral(" / ") + m_state.codeSecret;
}

void SessionUi::copyCode() {
    if (m_state.code.isEmpty()) {
        return;
    }
    QGuiApplication::clipboard()->setText(sharedCode());
    showMessage(tr("Copied. Paste it to your friend"));
}

void SessionUi::showMessage(const QString& text) {
    m_message = text;
    updateWidgets();
    QTimer::singleShot(5000, this, [this, text] {
        if (m_message == text) {
            m_message.clear();
            updateWidgets();
        }
    });
}

void SessionUi::showRequest() {
    if (m_pRequestBox) {
        return;
    }
    auto* pBox = new QMessageBox(QMessageBox::Question,
            tr("B2B"),
            tr("Your friend wants to take control."),
            QMessageBox::NoButton,
            m_pDialogParent);
    pBox->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton* pAllow = pBox->addButton(tr("Hand over"), QMessageBox::AcceptRole);
    pBox->addButton(tr("Not yet"), QMessageBox::RejectRole);
    pBox->setDefaultButton(pAllow);
    connect(pBox, &QMessageBox::buttonClicked, this, [this, pAllow](QAbstractButton* pButton) {
        emit requestAnswered(pButton == pAllow);
    });
    m_pRequestBox = pBox;
    pBox->open(); // window-modal, centred on the app; music keeps playing
    pBox->raise();
    pBox->activateWindow();
}

void SessionUi::closeRequest() {
    if (m_pRequestBox) {
        m_pRequestBox->close();
    }
}

void SessionUi::openSessionDialog(bool host) {
    if (m_state.phase != UiState::Phase::Idle) {
        return;
    }
    QDialog dialog(m_pDialogParent);
    dialog.setWindowTitle(host ? tr("Host a B2B session") : tr("Join a B2B session"));
    auto* pLayout = new QVBoxLayout(&dialog);
    auto* pForm = new QFormLayout();
    pLayout->addLayout(pForm);

    auto* pPort = new QSpinBox(&dialog);
    pPort->setRange(1024, 65534);
    pPort->setValue(m_last.port);
    // Empty each time: a join code is new every session (an address can
    // still be typed in).
    auto* pPeer = new QLineEdit(&dialog);
    pPeer->setPlaceholderText(tr("Your friend's join code, e.g. BASS-TIGER-42"));
    auto* pMusic = new QLineEdit(QDir::toNativeSeparators(m_last.musicFolder), &dialog);

    QRadioButton* pUseCode = nullptr;
    if (host) {
        auto* pIntro = new QLabel(tr("You'll start live. How will your friend connect?"), &dialog);
        pIntro->setWordWrap(true);
        pLayout->insertWidget(0, pIntro);
        pUseCode = new QRadioButton(tr("With a join code (works over the internet)"), &dialog);
        auto* pUseAddress = new QRadioButton(tr("With my address (same network, or Tailscale)"), &dialog);
        auto* pChoice = new QButtonGroup(&dialog);
        pChoice->addButton(pUseCode);
        pChoice->addButton(pUseAddress);
        (m_last.useCode ? pUseCode : pUseAddress)->setChecked(true);
        pLayout->insertWidget(1, pUseCode);
        pLayout->insertWidget(2, pUseAddress);
        const QStringList addresses = localAddresses();
        auto* pAddresses = new QLabel(addresses.isEmpty() ? tr("(no network found)") : addresses.join(QLatin1Char('\n')),
                &dialog);
        pAddresses->setTextInteractionFlags(Qt::TextSelectableByMouse);
        QFont mono = pAddresses->font();
        mono.setFamily(QStringLiteral("monospace"));
        mono.setStyleHint(QFont::Monospace);
        pAddresses->setFont(mono);
        pForm->addRow(tr("Your addresses:"), pAddresses);
        pForm->addRow(tr("Port:"), pPort);
        pPeer->hide();
        auto showAddresses = [pForm, pAddresses, pPort](bool useCode) {
            for (QWidget* pField : {static_cast<QWidget*>(pAddresses), static_cast<QWidget*>(pPort)}) {
                pField->setVisible(!useCode);
                if (QWidget* pLabel = pForm->labelForField(pField)) {
                    pLabel->setVisible(!useCode);
                }
            }
        };
        showAddresses(m_last.useCode);
        connect(pUseCode, &QRadioButton::toggled, &dialog, [&dialog, showAddresses](bool on) {
            showAddresses(on);
            dialog.adjustSize();
        });
    } else {
        pForm->addRow(tr("Join code or address:"), pPeer);
        pPort->hide();
    }
    // Joining by address needs the host's session password (codes don't).
    auto* pPassword = new QLineEdit(&dialog);
    pPassword->setPlaceholderText(tr("shown in your friend's B2B bar, e.g. KX7P-Q2MD-9TZA"));
    if (!host) {
        pForm->addRow(tr("Session password:"), pPassword);
        auto showPassword = [pForm, pPassword, pPeer] {
            const QString text = pPeer->text().trimmed();
            const bool address = !text.isEmpty() && !::b2b::looksLikeJoinCode(text.toStdString());
            pPassword->setVisible(address);
            if (QWidget* pLabel = pForm->labelForField(pPassword)) {
                pLabel->setVisible(address);
            }
        };
        showPassword();
        connect(pPeer, &QLineEdit::textChanged, &dialog, [&dialog, showPassword] {
            showPassword();
            dialog.adjustSize();
        });
    } else {
        pPassword->hide();
    }
    pForm->addRow(tr("Your music folder:"), folderRow(pMusic, &dialog));
    auto* pNote = new QLabel(tr("Tracks your friend plays that you don't have are borrowed for this "
                                "session only and deleted afterwards."),
            &dialog);
    pNote->setWordWrap(true);
    pLayout->addWidget(pNote);

    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    pButtons->button(QDialogButtonBox::Ok)->setText(host ? tr("Start hosting") : tr("Join"));
    pLayout->addWidget(pButtons);
    connect(pButtons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!host && pPeer->text().trimmed().isEmpty()) {
            pPeer->setFocus();
            return;
        }
        if (!host && pPassword->isVisible() && pPassword->text().trimmed().isEmpty()) {
            pPassword->setFocus(); // by address: the password is required
            return;
        }
        dialog.accept();
    });
    connect(pButtons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.setMinimumWidth(460);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    SessionSettings settings;
    settings.host = host;
    settings.useCode = host ? pUseCode->isChecked() : m_last.useCode;
    settings.port = static_cast<quint16>(pPort->value());
    settings.peerAddress = pPeer->text().trimmed();
    settings.musicFolder = QDir::fromNativeSeparators(pMusic->text().trimmed());
    settings.password = host ? (settings.useCode ? QString() : QString::fromStdString(::b2b::makeSessionPassword()))
                             : pPassword->text().trimmed();
    // Remember addresses (they stay the same), not codes (they're new every time).
    const bool isAddress = settings.peerAddress.contains(QLatin1Char('.')) ||
                           settings.peerAddress.contains(QLatin1Char(':'));
    const QString rememberedPeer = isAddress ? settings.peerAddress : m_last.peerAddress;
    m_last = settings;
    m_last.peerAddress = rememberedPeer;
    QSettings s(m_settingsFile, QSettings::IniFormat);
    s.setValue(QStringLiteral("last_host"), host);
    if (!host) {
        s.setValue(QStringLiteral("last_peer"), rememberedPeer);
    } else {
        s.setValue(QStringLiteral("last_port"), settings.port);
        s.setValue(QStringLiteral("use_code"), settings.useCode);
    }
    s.setValue(QStringLiteral("music_folder"), settings.musicFolder);
    emit startRequested(settings);
}

} // namespace b2b::qt
