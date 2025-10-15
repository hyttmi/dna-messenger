/*
 * DNA Messenger - Qt GUI
 * Main Window Implementation
 */

#include "MainWindow.h"
#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QMessageBox>
#include <QInputDialog>
#include <QDialog>
#include <QStringList>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMenuBar>
#include <QMenu>
#include <QAction>
#include <QProcess>
#include <QSettings>
#include <QFontDatabase>
#include <QMouseEvent>

// Platform-specific includes for identity detection
#ifdef _WIN32
#include <windows.h>
#else
#include <glob.h>
#endif

QString MainWindow::getLocalIdentity() {
    QDir homeDir = QDir::home();
    QDir dnaDir = QDir(homeDir.filePath(".dna"));

    if (!dnaDir.exists()) {
        return QString();  // No .dna directory
    }

#ifdef _WIN32
    // Windows: use FindFirstFile
    QString searchPath = dnaDir.filePath("*-dilithium.pqkey");
    WIN32_FIND_DATAA findData;
    HANDLE hFind = FindFirstFileA(searchPath.toUtf8().constData(), &findData);

    if (hFind == INVALID_HANDLE_VALUE) {
        return QString();  // No identity files found
    }

    // Extract identity from filename (remove -dilithium.pqkey suffix)
    QString filename = QString::fromUtf8(findData.cFileName);
    FindClose(hFind);

    if (filename.endsWith("-dilithium.pqkey")) {
        return filename.left(filename.length() - 16);  // Remove suffix (16 chars: "-dilithium.pqkey")
    }
    return QString();
#else
    // Unix: use glob
    QString pattern = dnaDir.filePath("*-dilithium.pqkey");
    glob_t globResult;

    if (glob(pattern.toUtf8().constData(), GLOB_NOSORT, NULL, &globResult) == 0 && globResult.gl_pathc > 0) {
        // Extract filename from path
        QString path = QString::fromUtf8(globResult.gl_pathv[0]);
        QString filename = QFileInfo(path).fileName();

        globfree(&globResult);

        if (filename.endsWith("-dilithium.pqkey")) {
            return filename.left(filename.length() - 16);  // Remove suffix (16 chars: "-dilithium.pqkey")
        }
    }

    globfree(&globResult);
    return QString();
#endif
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ctx(nullptr), lastCheckedMessageId(0), currentGroupId(-1), currentContactType(TYPE_CONTACT) {

    // Remove native window frame for custom title bar
    setWindowFlags(Qt::FramelessWindowHint);

    // Auto-detect local identity
    currentIdentity = getLocalIdentity();

    if (currentIdentity.isEmpty()) {
        // No local identity found, prompt for manual entry
        bool ok;
        currentIdentity = QInputDialog::getText(this, "DNA Messenger Login",
                                                 "No local identity found.\nEnter your identity:",
                                                 QLineEdit::Normal,
                                                 "", &ok);

        if (!ok || currentIdentity.isEmpty()) {
            QMessageBox::critical(this, "Error", "Identity required to start messenger");
            QApplication::quit();
            return;
        }
    }

    // Initialize messenger context
    ctx = messenger_init(currentIdentity.toUtf8().constData());
    if (!ctx) {
        QMessageBox::critical(this, "Error",
                              QString("Failed to initialize messenger for '%1'").arg(currentIdentity));
        QApplication::quit();
        return;
    }

    // Load futuristic font from resources
    int fontId = QFontDatabase::addApplicationFont(":/fonts/Orbitron.ttf");
    if (fontId != -1) {
        QStringList fontFamilies = QFontDatabase::applicationFontFamilies(fontId);
        if (!fontFamilies.isEmpty()) {
            QFont orbitronFont(fontFamilies.at(0));
            QApplication::setFont(orbitronFont);
            printf("Loaded Orbitron font\n");
        }
    } else {
        printf("Failed to load Orbitron font\n");
    }

    setupUI();
    loadContacts();

    // Initialize system tray icon
    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(QIcon(":/icons/dna_icon.png"));
    trayIcon->setToolTip("DNA Messenger");

    trayMenu = new QMenu(this);
    trayMenu->addAction("Show", this, &MainWindow::show);
    trayMenu->addAction("Exit", qApp, &QApplication::quit);
    trayIcon->setContextMenu(trayMenu);

    connect(trayIcon, &QSystemTrayIcon::activated,
            this, &MainWindow::onTrayIconActivated);

    trayIcon->show();

    // Initialize notification sound
    notificationSound = new QSoundEffect(this);
    notificationSound->setSource(QUrl("qrc:/sounds/message.wav"));
    notificationSound->setVolume(0.5);

    // Initialize polling timer (5 seconds)
    pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &MainWindow::checkForNewMessages);
    pollTimer->start(5000);

    // Initialize status polling timer (10 seconds)
    statusPollTimer = new QTimer(this);
    connect(statusPollTimer, &QTimer::timeout, this, &MainWindow::checkForStatusUpdates);
    statusPollTimer->start(10000);

    // Load saved preferences
    QSettings settings("DNA Messenger", "GUI");
    QString savedTheme = settings.value("theme", "io").toString();  // Default to "io" theme
    double savedFontScale = settings.value("fontScale", 3.0).toDouble();  // Default to 3x (Large)

    applyTheme(savedTheme);
    applyFontScale(savedFontScale);

    // Scale window to 80% of screen size
    QScreen *screen = QGuiApplication::primaryScreen();
    QRect screenGeometry = screen->availableGeometry();
    int width = screenGeometry.width() * 0.8;
    int height = screenGeometry.height() * 0.8;
    resize(width, height);

    // Center window on screen
    move(screenGeometry.center() - rect().center());

    // Print debug info on startup
    printf("DNA Messenger GUI v%s (commit %s)\n", PQSIGNUM_VERSION, BUILD_HASH);
    printf("Build date: %s\n", BUILD_TS);
    printf("Identity: %s\n", currentIdentity.toUtf8().constData());
}

MainWindow::~MainWindow() {
    if (ctx) {
        messenger_free(ctx);
    }
}

void MainWindow::setupUI() {
    // Create custom title bar
    titleBar = new QWidget(this);
    titleBar->setFixedHeight(60);
    titleBar->setStyleSheet(
        "QWidget {"
        "   background: #0D3438;"
        "   border-bottom: 2px solid #00D9FF;"
        "}"
    );

    QHBoxLayout *titleLayout = new QHBoxLayout(titleBar);
    titleLayout->setContentsMargins(20, 0, 0, 0);
    titleLayout->setSpacing(10);

    // Title label (version + username)
    titleLabel = new QLabel(QString("DNA Messenger v%1 - %2").arg(PQSIGNUM_VERSION).arg(currentIdentity), titleBar);
    titleLabel->setStyleSheet(
        "font-family: 'Orbitron';"
        "font-size: 48px;"
        "font-weight: bold;"
        "color: #00D9FF;"
        "background: transparent;"
        "border: none;"
    );
    titleLayout->addWidget(titleLabel);

    titleLayout->addStretch();

    // Minimize button
    minimizeButton = new QPushButton(QString::fromUtf8("➖"), titleBar);
    minimizeButton->setFixedSize(50, 50);
    minimizeButton->setStyleSheet(
        "QPushButton {"
        "   background: rgba(0, 217, 255, 0.2);"
        "   color: #00D9FF;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 10px;"
        "   font-family: 'Orbitron';"
        "   font-size: 24px;"
        "   font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "   background: rgba(0, 217, 255, 0.3);"
        "}"
        "QPushButton:pressed {"
        "   background: rgba(0, 217, 255, 0.4);"
        "}"
    );
    connect(minimizeButton, &QPushButton::clicked, this, &MainWindow::onMinimizeWindow);
    titleLayout->addWidget(minimizeButton);

    // Close button
    closeButton = new QPushButton(QString::fromUtf8("✖"), titleBar);
    closeButton->setFixedSize(50, 50);
    closeButton->setStyleSheet(
        "QPushButton {"
        "   background: rgba(255, 107, 53, 0.3);"
        "   color: #FF6B35;"
        "   border: 2px solid #FF6B35;"
        "   border-radius: 10px;"
        "   font-family: 'Orbitron';"
        "   font-size: 24px;"
        "   font-weight: bold;"
        "}"
        "QPushButton:hover {"
        "   background: rgba(255, 107, 53, 0.5);"
        "}"
        "QPushButton:pressed {"
        "   background: rgba(255, 107, 53, 0.7);"
        "}"
    );
    connect(closeButton, &QPushButton::clicked, this, &MainWindow::onCloseWindow);
    titleLayout->addWidget(closeButton);

    titleBar->setLayout(titleLayout);

    // Set cpunk.io theme - dark teal with cyan accents
    setStyleSheet(
        "QMainWindow {"
        "   background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
        "       stop:0 #0A2A2E, stop:1 #0D3438);"
        "}"
        "QMenuBar {"
        "   background: #0D3438;"
        "   color: #00D9FF;"
        "   padding: 8px;"
        "   font-weight: bold;"
        "   font-family: 'Orbitron'; font-size: 48px;"
        "   border-bottom: 2px solid #00D9FF;"
        "}"
        "QMenuBar::item {"
        "   padding: 8px 15px;"
        "   color: #00D9FF;"
        "}"
        "QMenuBar::item:selected {"
        "   background: rgba(0, 217, 255, 0.2);"
        "   border-radius: 5px;"
        "}"
        "QMenu {"
        "   background: #0D3438;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 10px;"
        "   padding: 8px;"
        "   font-family: 'Orbitron'; font-size: 48px;"
        "   color: #00D9FF;"
        "}"
        "QMenu::item {"
        "   padding: 10px 20px;"
        "   color: #00D9FF;"
        "}"
        "QMenu::item:selected {"
        "   background: rgba(0, 217, 255, 0.3);"
        "   border-radius: 5px;"
        "}"
        "QStatusBar {"
        "   background: #0D3438;"
        "   color: #00D9FF;"
        "   font-weight: bold;"
        "   font-family: 'Orbitron'; font-size: 48px;"
        "   padding: 8px;"
        "   border-top: 2px solid #00D9FF;"
        "}"
    );

    // Create menu bar (not using setMenuBar to keep it below title bar)
    QMenuBar *menuBar = new QMenuBar();

    // Settings menu
    QMenu *settingsMenu = menuBar->addMenu(QString::fromUtf8("⚙️ Settings"));

    // Theme submenu
    QMenu *themeMenu = settingsMenu->addMenu(QString::fromUtf8("🎨 Theme"));
    QAction *themeIOAction = themeMenu->addAction(QString::fromUtf8("🌊 cpunk.io (Cyan)"));
    QAction *themeClubAction = themeMenu->addAction(QString::fromUtf8("🔥 cpunk.club (Orange)"));
    connect(themeIOAction, &QAction::triggered, this, &MainWindow::onThemeIO);
    connect(themeClubAction, &QAction::triggered, this, &MainWindow::onThemeClub);

    // Font Scale submenu
    QMenu *fontScaleMenu = settingsMenu->addMenu(QString::fromUtf8("📏 Font Scale"));
    QAction *fontSmallAction = fontScaleMenu->addAction(QString::fromUtf8("🔤 Small (1x)"));
    QAction *fontMediumAction = fontScaleMenu->addAction(QString::fromUtf8("🔡 Medium (2x)"));
    QAction *fontLargeAction = fontScaleMenu->addAction(QString::fromUtf8("🔠 Large (3x)"));
    QAction *fontExtraLargeAction = fontScaleMenu->addAction(QString::fromUtf8("🅰️ Extra Large (4x)"));
    connect(fontSmallAction, &QAction::triggered, this, &MainWindow::onFontScaleSmall);
    connect(fontMediumAction, &QAction::triggered, this, &MainWindow::onFontScaleMedium);
    connect(fontLargeAction, &QAction::triggered, this, &MainWindow::onFontScaleLarge);
    connect(fontExtraLargeAction, &QAction::triggered, this, &MainWindow::onFontScaleExtraLarge);

    // Help menu
    QMenu *helpMenu = menuBar->addMenu(QString::fromUtf8("💝 Help"));
    QAction *updateAction = helpMenu->addAction(QString::fromUtf8("✨ Check for Updates"));
    connect(updateAction, &QAction::triggered, this, &MainWindow::onCheckForUpdates);

    // Central widget with vertical layout for title bar + content
    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *mainVerticalLayout = new QVBoxLayout(centralWidget);
    mainVerticalLayout->setContentsMargins(0, 0, 0, 0);
    mainVerticalLayout->setSpacing(0);

    // Add title bar at the top
    mainVerticalLayout->addWidget(titleBar);

    // Add menu bar below title bar
    mainVerticalLayout->addWidget(menuBar);

    // Content widget for the splitter
    QWidget *contentWidget = new QWidget;
    QHBoxLayout *mainLayout = new QHBoxLayout(contentWidget);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(10);

    // Left side: Contact list
    QWidget *leftPanel = new QWidget;
    leftPanel->setStyleSheet(
        "QWidget {"
        "   background: #0A2A2E;"
        "   border-radius: 15px;"
        "   padding: 10px;"
        "}"
    );
    QVBoxLayout *leftLayout = new QVBoxLayout(leftPanel);

    QLabel *contactsLabel = new QLabel(QString::fromUtf8("👥 Contacts"));
    contactsLabel->setStyleSheet(
        "font-weight: bold; "
        "font-family: 'Orbitron'; font-size: 72px; "
        "color: #00D9FF; "
        "background: transparent; "
        "padding: 10px;"
    );
    leftLayout->addWidget(contactsLabel);

    contactList = new QListWidget;
    contactList->setStyleSheet(
        "QListWidget {"
        "   background: #0D3438;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 10px;"
        "   padding: 8px;"
        "   font-family: 'Orbitron'; font-size: 54px;"
        "   color: #00D9FF;"
        "}"
        "QListWidget::item {"
        "   background: rgba(0, 217, 255, 0.1);"
        "   border: 1px solid rgba(0, 217, 255, 0.3);"
        "   border-radius: 10px;"
        "   padding: 15px;"
        "   margin: 5px;"
        "   color: #00D9FF;"
        "}"
        "QListWidget::item:hover {"
        "   background: rgba(0, 217, 255, 0.2);"
        "   border: 2px solid #00D9FF;"
        "}"
        "QListWidget::item:selected {"
        "   background: rgba(0, 217, 255, 0.3);"
        "   color: #FFFFFF;"
        "   font-weight: bold;"
        "   border: 2px solid #00D9FF;"
        "}"
    );
    connect(contactList, &QListWidget::itemClicked, this, &MainWindow::onContactSelected);
    leftLayout->addWidget(contactList);

    refreshButton = new QPushButton(QString::fromUtf8("🔄 Refresh"));
    refreshButton->setStyleSheet(
        "QPushButton {"
        "   background: rgba(0, 217, 255, 0.2);"
        "   color: #00D9FF;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 15px;"
        "   padding: 15px;"
        "   font-weight: bold;"
        "   font-family: 'Orbitron'; font-size: 54px;"
        "}"
        "QPushButton:hover {"
        "   background: rgba(0, 217, 255, 0.3);"
        "   border: 2px solid #33E6FF;"
        "}"
        "QPushButton:pressed {"
        "   background: rgba(0, 217, 255, 0.4);"
        "   border: 2px solid #00D9FF;"
        "}"
    );
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::onRefreshMessages);
    leftLayout->addWidget(refreshButton);

    leftPanel->setLayout(leftLayout);

    // Right side: Chat area
    QWidget *rightPanel = new QWidget;
    rightPanel->setStyleSheet(
        "QWidget {"
        "   background: #0A2A2E;"
        "   border-radius: 15px;"
        "   padding: 10px;"
        "}"
    );
    QVBoxLayout *rightLayout = new QVBoxLayout(rightPanel);

    QLabel *chatLabel = new QLabel(QString::fromUtf8("💬 Conversation"));
    chatLabel->setStyleSheet(
        "font-weight: bold; "
        "font-family: 'Orbitron'; font-size: 72px; "
        "color: #00D9FF; "
        "background: transparent; "
        "padding: 10px;"
    );
    rightLayout->addWidget(chatLabel);

    messageDisplay = new QTextEdit;
    messageDisplay->setReadOnly(true);
    messageDisplay->setStyleSheet(
        "QTextEdit {"
        "   background: #0D3438;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 10px;"
        "   padding: 15px;"
        "   font-family: 'Orbitron'; font-size: 48px;"
        "   color: #00D9FF;"
        "}"
    );
    rightLayout->addWidget(messageDisplay);

    // Recipients label
    recipientsLabel = new QLabel(QString::fromUtf8("📨 To: ..."));
    recipientsLabel->setStyleSheet(
        "QLabel {"
        "   background: rgba(0, 217, 255, 0.1);"
        "   color: #00D9FF;"
        "   border: 2px solid rgba(0, 217, 255, 0.3);"
        "   border-radius: 10px;"
        "   padding: 10px 15px;"
        "   font-family: 'Orbitron'; font-size: 42px;"
        "}"
    );
    rightLayout->addWidget(recipientsLabel);

    // Recipients button row
    QHBoxLayout *recipientsButtonLayout = new QHBoxLayout;
    recipientsButtonLayout->addStretch();

    addRecipientsButton = new QPushButton(QString::fromUtf8("➕ Add Recipients"));
    addRecipientsButton->setStyleSheet(
        "QPushButton {"
        "   background: rgba(0, 217, 255, 0.2);"
        "   color: #00D9FF;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 12px;"
        "   padding: 10px 20px;"
        "   font-weight: bold;"
        "   font-family: 'Orbitron'; font-size: 42px;"
        "}"
        "QPushButton:hover {"
        "   background: rgba(0, 217, 255, 0.3);"
        "   border: 2px solid #33E6FF;"
        "}"
        "QPushButton:pressed {"
        "   background: rgba(0, 217, 255, 0.4);"
        "   border: 2px solid #00D9FF;"
        "}"
    );
    connect(addRecipientsButton, &QPushButton::clicked, this, &MainWindow::onAddRecipients);
    recipientsButtonLayout->addWidget(addRecipientsButton);

    rightLayout->addLayout(recipientsButtonLayout);

    // Message input area
    QHBoxLayout *inputLayout = new QHBoxLayout;
    messageInput = new QLineEdit;
    messageInput->setPlaceholderText(QString::fromUtf8("✏️ Type a message..."));
    messageInput->setStyleSheet(
        "QLineEdit {"
        "   background: #0D3438;"
        "   border: 2px solid #00D9FF;"
        "   border-radius: 15px;"
        "   padding: 15px 20px;"
        "   font-family: 'Orbitron'; font-size: 54px;"
        "   color: #00D9FF;"
        "}"
        "QLineEdit:focus {"
        "   border: 2px solid #33E6FF;"
        "   background: rgba(0, 217, 255, 0.1);"
        "}"
    );
    connect(messageInput, &QLineEdit::returnPressed, this, &MainWindow::onSendMessage);
    inputLayout->addWidget(messageInput);

    sendButton = new QPushButton(QString::fromUtf8("💌 Send"));
    sendButton->setStyleSheet(
        "QPushButton {"
        "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "       stop:0 #FF6B35, stop:1 #FF8C42);"
        "   color: white;"
        "   border: 2px solid #FF6B35;"
        "   border-radius: 15px;"
        "   padding: 15px 30px;"
        "   font-weight: bold;"
        "   font-family: 'Orbitron'; font-size: 54px;"
        "}"
        "QPushButton:hover {"
        "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
        "       stop:0 #FF8C42, stop:1 #FFA55C);"
        "   border: 2px solid #FF8C42;"
        "}"
        "QPushButton:pressed {"
        "   background: #FF5722;"
        "   border: 2px solid #E64A19;"
        "}"
    );
    connect(sendButton, &QPushButton::clicked, this, &MainWindow::onSendMessage);
    inputLayout->addWidget(sendButton);

    rightLayout->addLayout(inputLayout);
    rightPanel->setLayout(rightLayout);

    // Add panels to splitter
    QSplitter *splitter = new QSplitter(Qt::Horizontal);
    splitter->setStyleSheet(
        "QSplitter::handle {"
        "   background: #00D9FF;"
        "   width: 3px;"
        "}"
    );
    splitter->addWidget(leftPanel);
    splitter->addWidget(rightPanel);
    splitter->setStretchFactor(0, 1);  // Left panel: 25%
    splitter->setStretchFactor(1, 3);  // Right panel: 75%

    mainLayout->addWidget(splitter);
    contentWidget->setLayout(mainLayout);

    // Add content to main vertical layout with stretch factor to fill remaining space
    mainVerticalLayout->addWidget(contentWidget, 1);

    // Status bar
    statusLabel = new QLabel(QString::fromUtf8("✨ Ready"));
    statusBar()->addWidget(statusLabel);
}

void MainWindow::loadContacts() {
    contactList->clear();
    contactItems.clear();

    int totalItems = 0;

    // Load contacts from keyserver
    char **identities = NULL;
    int contactCount = 0;

    if (messenger_get_contact_list(ctx, &identities, &contactCount) == 0) {
        for (int i = 0; i < contactCount; i++) {
            QString identity = QString::fromUtf8(identities[i]);
            QString displayText = QString::fromUtf8("👤 ") + identity;
            contactList->addItem(displayText);

            // Store contact metadata
            ContactItem item;
            item.type = TYPE_CONTACT;
            item.name = identity;
            item.groupId = -1;
            contactItems[displayText] = item;

            free(identities[i]);
            totalItems++;
        }
        free(identities);
    }

    // Load groups
    group_info_t *groups = NULL;
    int groupCount = 0;

    if (messenger_get_groups(ctx, &groups, &groupCount) == 0) {
        for (int i = 0; i < groupCount; i++) {
            QString groupName = QString::fromUtf8(groups[i].name);
            QString displayText = QString::fromUtf8("👥 ") + groupName;
            contactList->addItem(displayText);

            // Store group metadata
            ContactItem item;
            item.type = TYPE_GROUP;
            item.name = groupName;
            item.groupId = groups[i].id;
            contactItems[displayText] = item;

            totalItems++;
        }
        messenger_free_groups(groups, groupCount);
    }

    if (totalItems > 0) {
        statusLabel->setText(QString::fromUtf8("✨ %1 contact(s) and %2 group(s) loaded")
                             .arg(contactCount).arg(groupCount));
    } else {
        statusLabel->setText(QString::fromUtf8("❌ No contacts or groups found"));
    }
}

void MainWindow::onContactSelected(QListWidgetItem *item) {
    if (!item) return;

    QString itemText = item->text();

    // Look up item metadata
    if (!contactItems.contains(itemText)) {
        return;
    }

    ContactItem contactItem = contactItems[itemText];
    currentContactType = contactItem.type;

    // Clear additional recipients when selecting a new item
    additionalRecipients.clear();

    if (contactItem.type == TYPE_CONTACT) {
        // Handle contact selection
        currentContact = contactItem.name;
        currentGroupId = -1;

        // Update recipients label
        recipientsLabel->setText(QString::fromUtf8("📨 To: ") + currentContact);

        // Mark all messages from this contact as read
        messenger_mark_conversation_read(ctx, currentContact.toUtf8().constData());

        loadConversation(currentContact);
    } else if (contactItem.type == TYPE_GROUP) {
        // Handle group selection
        currentContact.clear();
        currentGroupId = contactItem.groupId;

        // Update recipients label
        recipientsLabel->setText(QString::fromUtf8("📨 To: Group - ") + contactItem.name);

        loadGroupConversation(currentGroupId);
    }
}

void MainWindow::loadConversation(const QString &contact) {
    messageDisplay->clear();

    if (contact.isEmpty()) {
        return;
    }

    // Calculate font sizes for message bubbles
    int headerFontSize = static_cast<int>(24 * fontScale);
    int metaFontSize = static_cast<int>(13 * fontScale);
    int messageFontSize = static_cast<int>(18 * fontScale);

    // Cute header with emoji - cpunk.io theme
    messageDisplay->setHtml(QString(
        "<div style='text-align: center; background: rgba(0, 217, 255, 0.2); "
        "padding: 15px; border-radius: 15px; margin-bottom: 15px; border: 2px solid #00D9FF;'>"
        "<span style='font-family: 'Orbitron'; font-size: %1px; font-weight: bold; color: #00D9FF;'>%2 Conversation with %3 %4</span>"
        "</div>"
    ).arg(headerFontSize + 18).arg(QString::fromUtf8("💬"), contact, QString::fromUtf8("✨")));

    // Load messages from database
    message_info_t *messages = NULL;
    int count = 0;

    if (messenger_get_conversation(ctx, contact.toUtf8().constData(), &messages, &count) == 0) {
        if (count == 0) {
            messageDisplay->append(QString(
                "<div style='text-align: center; color: rgba(0, 217, 255, 0.6); padding: 30px; font-style: italic; font-family: 'Orbitron'; font-size: %1px;'>"
                "%2"
                "</div>"
            ).arg(messageFontSize).arg(QString::fromUtf8("💭 No messages yet. Start the conversation!")));
        } else {
            for (int i = 0; i < count; i++) {
                QString sender = QString::fromUtf8(messages[i].sender);
                QString recipient = QString::fromUtf8(messages[i].recipient);
                QString timestamp = QString::fromUtf8(messages[i].timestamp);

                // Format timestamp (extract time from "YYYY-MM-DD HH:MM:SS")
                QString timeOnly = timestamp.mid(11, 5);  // Extract "HH:MM"

                // Decrypt message if current user can decrypt it
                // This includes: received messages (recipient == currentIdentity)
                // AND sent messages (sender == currentIdentity, thanks to sender-as-first-recipient)
                QString messageText = "[encrypted]";
                if (recipient == currentIdentity || sender == currentIdentity) {
                    char *plaintext = NULL;
                    size_t plaintext_len = 0;

                    if (messenger_decrypt_message(ctx, messages[i].id, &plaintext, &plaintext_len) == 0) {
                        messageText = QString::fromUtf8(plaintext, plaintext_len);
                        free(plaintext);
                    } else {
                        messageText = QString::fromUtf8("🔒 [decryption failed]");
                    }
                }

                if (sender == currentIdentity) {
                    // Generate status checkmark
                    QString statusCheckmark;
                    QString status = messages[i].status ? QString::fromUtf8(messages[i].status) : "sent";

                    if (status == "read") {
                        // Double checkmark colored (theme-aware)
                        if (currentTheme == "club") {
                            statusCheckmark = QString::fromUtf8("<span style='color: #FF8C42;'>✓✓</span>");
                        } else {
                            statusCheckmark = QString::fromUtf8("<span style='color: #00D9FF;'>✓✓</span>");
                        }
                    } else if (status == "delivered") {
                        // Double checkmark gray
                        statusCheckmark = QString::fromUtf8("<span style='color: #888888;'>✓✓</span>");
                    } else {
                        // Single checkmark gray (sent)
                        statusCheckmark = QString::fromUtf8("<span style='color: #888888;'>✓</span>");
                    }

                    // Sent messages - theme-aware bubble aligned right
                    QString sentBubble;
                    if (currentTheme == "club") {
                        // cpunk.club: orange gradient
                        sentBubble = QString(
                            "<div style='text-align: right; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF8C42, stop:1 #FFB380); "
                            "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #FF8C42;'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timeOnly, statusCheckmark).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    } else {
                        // cpunk.io: cyan gradient (default)
                        sentBubble = QString(
                            "<div style='text-align: right; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00D9FF, stop:1 #0D8B9C); "
                            "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #00D9FF;'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timeOnly, statusCheckmark).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    }
                    messageDisplay->append(sentBubble);
                } else {
                    // Received messages - theme-aware bubble aligned left
                    QString receivedBubble;
                    if (currentTheme == "club") {
                        // cpunk.club: darker brown/orange
                        receivedBubble = QString(
                            "<div style='text-align: left; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2B1F16, stop:1 #3D2B1F); "
                            "color: #FFB380; padding: 15px 20px; border-radius: 20px 20px 20px 5px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid rgba(255, 140, 66, 0.5);'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("👤"), sender, QString::fromUtf8("•"), timeOnly).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    } else {
                        // cpunk.io: darker teal (default)
                        receivedBubble = QString(
                            "<div style='text-align: left; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0D3438, stop:1 #0A5A62); "
                            "color: #00D9FF; padding: 15px 20px; border-radius: 20px 20px 20px 5px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid rgba(0, 217, 255, 0.5);'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("👤"), sender, QString::fromUtf8("•"), timeOnly).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    }
                    messageDisplay->append(receivedBubble);
                }
            }
        }

        messenger_free_messages(messages, count);
        statusLabel->setText(QString::fromUtf8("✨ Loaded %1 messages with %2").arg(count).arg(contact));
    } else {
        messageDisplay->append(QString(
            "<div style='text-align: center; color: #FF6B35; padding: 20px; font-family: 'Orbitron'; font-size: %1px; font-weight: bold;'>"
            "%2"
            "</div>"
        ).arg(messageFontSize).arg(QString::fromUtf8("❌ Failed to load conversation")));
        statusLabel->setText(QString::fromUtf8("❌ Error loading conversation"));
    }
}

void MainWindow::loadGroupConversation(int groupId) {
    messageDisplay->clear();

    if (groupId < 0) {
        return;
    }

    // Calculate font sizes for message bubbles
    int headerFontSize = static_cast<int>(24 * fontScale);
    int metaFontSize = static_cast<int>(13 * fontScale);
    int messageFontSize = static_cast<int>(18 * fontScale);

    // Get group info for header
    group_info_t groupInfo;
    if (messenger_get_group_info(ctx, groupId, &groupInfo) == 0) {
        // Display group header
        messageDisplay->setHtml(QString(
            "<div style='text-align: center; background: rgba(0, 217, 255, 0.2); "
            "padding: 15px; border-radius: 15px; margin-bottom: 15px; border: 2px solid #00D9FF;'>"
            "<span style='font-family: 'Orbitron'; font-size: %1px; font-weight: bold; color: #00D9FF;'>%2 Group: %3 %4</span>"
            "</div>"
        ).arg(headerFontSize + 18).arg(QString::fromUtf8("👥"), QString::fromUtf8(groupInfo.name), QString::fromUtf8("✨")));

        // Free group info strings
        free(groupInfo.name);
        if (groupInfo.description) free(groupInfo.description);
        if (groupInfo.creator) free(groupInfo.creator);
        if (groupInfo.created_at) free(groupInfo.created_at);
    } else {
        messageDisplay->setHtml(QString(
            "<div style='text-align: center; background: rgba(0, 217, 255, 0.2); "
            "padding: 15px; border-radius: 15px; margin-bottom: 15px; border: 2px solid #00D9FF;'>"
            "<span style='font-family: 'Orbitron'; font-size: %1px; font-weight: bold; color: #00D9FF;'>%2 Group Conversation %3</span>"
            "</div>"
        ).arg(headerFontSize + 18).arg(QString::fromUtf8("👥"), QString::fromUtf8("✨")));
    }

    // Load messages from database
    message_info_t *messages = NULL;
    int count = 0;

    if (messenger_get_group_conversation(ctx, groupId, &messages, &count) == 0) {
        if (count == 0) {
            messageDisplay->append(QString(
                "<div style='text-align: center; color: rgba(0, 217, 255, 0.6); padding: 30px; font-style: italic; font-family: 'Orbitron'; font-size: %1px;'>"
                "%2"
                "</div>"
            ).arg(messageFontSize).arg(QString::fromUtf8("💭 No messages yet. Start the conversation!")));
        } else {
            for (int i = 0; i < count; i++) {
                QString sender = QString::fromUtf8(messages[i].sender);
                QString timestamp = QString::fromUtf8(messages[i].timestamp);

                // Format timestamp (extract time from "YYYY-MM-DD HH:MM:SS")
                QString timeOnly = timestamp.mid(11, 5);  // Extract "HH:MM"

                // Decrypt message
                QString messageText = "[encrypted]";
                char *plaintext = NULL;
                size_t plaintext_len = 0;

                if (messenger_decrypt_message(ctx, messages[i].id, &plaintext, &plaintext_len) == 0) {
                    messageText = QString::fromUtf8(plaintext, plaintext_len);
                    free(plaintext);
                } else {
                    messageText = QString::fromUtf8("🔒 [decryption failed]");
                }

                if (sender == currentIdentity) {
                    // Sent messages by current user
                    QString statusCheckmark = QString::fromUtf8("<span style='color: #888888;'>✓</span>");

                    QString sentBubble;
                    if (currentTheme == "club") {
                        sentBubble = QString(
                            "<div style='text-align: right; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF8C42, stop:1 #FFB380); "
                            "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #FF8C42;'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timeOnly, statusCheckmark).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    } else {
                        sentBubble = QString(
                            "<div style='text-align: right; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00D9FF, stop:1 #0D8B9C); "
                            "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #00D9FF;'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timeOnly, statusCheckmark).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    }
                    messageDisplay->append(sentBubble);
                } else {
                    // Messages from other group members
                    QString receivedBubble;
                    if (currentTheme == "club") {
                        receivedBubble = QString(
                            "<div style='text-align: left; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2B1F16, stop:1 #3D2B1F); "
                            "color: #FFB380; padding: 15px 20px; border-radius: 20px 20px 20px 5px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid rgba(255, 140, 66, 0.5);'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("👤"), sender, QString::fromUtf8("•"), timeOnly).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    } else {
                        receivedBubble = QString(
                            "<div style='text-align: left; margin: 8px 0;'>"
                            "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #0D3438, stop:1 #0A5A62); "
                            "color: #00D9FF; padding: 15px 20px; border-radius: 20px 20px 20px 5px; "
                            "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid rgba(0, 217, 255, 0.5);'>"
                            "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 %3 %4 %5</div>"
                            "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                            "</div>"
                            "</div>"
                        ).arg(metaFontSize).arg(QString::fromUtf8("👤"), sender, QString::fromUtf8("•"), timeOnly).arg(messageFontSize).arg(messageText.toHtmlEscaped());
                    }
                    messageDisplay->append(receivedBubble);
                }
            }
        }

        messenger_free_messages(messages, count);
        statusLabel->setText(QString::fromUtf8("✨ Loaded %1 group messages").arg(count));
    } else {
        messageDisplay->append(QString(
            "<div style='text-align: center; color: #FF6B35; padding: 20px; font-family: 'Orbitron'; font-size: %1px; font-weight: bold;'>"
            "%2"
            "</div>"
        ).arg(messageFontSize).arg(QString::fromUtf8("❌ Failed to load group conversation")));
        statusLabel->setText(QString::fromUtf8("❌ Error loading group conversation"));
    }
}

void MainWindow::onSendMessage() {
    QString message = messageInput->text().trimmed();
    if (message.isEmpty()) {
        return;
    }

    int result = -1;

    // Check if we're sending to a group or contact
    if (currentContactType == TYPE_GROUP && currentGroupId >= 0) {
        // Send to group
        QByteArray messageBytes = message.toUtf8();
        result = messenger_send_group_message(ctx, currentGroupId, messageBytes.constData());
    } else if (currentContactType == TYPE_CONTACT && !currentContact.isEmpty()) {
        // Send to contact(s)
        // Build recipient list: currentContact + additionalRecipients
        QVector<QByteArray> recipientBytes;  // Store QByteArray to keep data alive
        QVector<const char*> recipients;

        // Add primary recipient
        recipientBytes.append(currentContact.toUtf8());
        recipients.append(recipientBytes.last().constData());

        // Add additional recipients
        for (const QString &recipient : additionalRecipients) {
            recipientBytes.append(recipient.toUtf8());
            recipients.append(recipientBytes.last().constData());
        }

        QByteArray messageBytes = message.toUtf8();
        result = messenger_send_message(ctx,
                                         recipients.data(),
                                         recipients.size(),
                                         messageBytes.constData());
    } else {
        QMessageBox::warning(this, "No Selection",
                             "Please select a contact or group from the list first");
        return;
    }

    if (result == 0) {
        // Success - add message bubble to display with theme-aware colors
        QString timestamp = QDateTime::currentDateTime().toString("HH:mm");

        // Calculate font sizes for message bubbles
        int metaFontSize = static_cast<int>(13 * fontScale);
        int messageFontSize = static_cast<int>(18 * fontScale);

        // New message starts with "sent" status (single gray checkmark)
        QString statusCheckmark = QString::fromUtf8("<span style='color: #888888;'>✓</span>");

        QString sentBubble;
        if (currentTheme == "club") {
            // cpunk.club: orange gradient
            sentBubble = QString(
                "<div style='text-align: right; margin: 8px 0;'>"
                "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #FF8C42, stop:1 #FFB380); "
                "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #FF8C42;'>"
                "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                "</div>"
                "</div>"
            ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timestamp, statusCheckmark).arg(messageFontSize).arg(message.toHtmlEscaped());
        } else {
            // cpunk.io: cyan gradient (default)
            sentBubble = QString(
                "<div style='text-align: right; margin: 8px 0;'>"
                "<div style='display: inline-block; background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00D9FF, stop:1 #0D8B9C); "
                "color: white; padding: 15px 20px; border-radius: 20px 20px 5px 20px; "
                "max-width: 70%; text-align: left; box-shadow: 2px 2px 8px rgba(0,0,0,0.3); border: 2px solid #00D9FF;'>"
                "<div style='font-family: 'Orbitron'; font-size: %1px; opacity: 0.9; margin-bottom: 5px;'>%2 You %3 %4 %5</div>"
                "<div style='font-family: 'Orbitron'; font-size: %6px; line-height: 1.4;'>%7</div>"
                "</div>"
                "</div>"
            ).arg(metaFontSize).arg(QString::fromUtf8("💌"), QString::fromUtf8("•"), timestamp, statusCheckmark).arg(messageFontSize).arg(message.toHtmlEscaped());
        }
        messageDisplay->append(sentBubble);
        messageInput->clear();
        statusLabel->setText(QString::fromUtf8("✨ Message sent"));
    } else {
        QMessageBox::critical(this, QString::fromUtf8("❌ Send Failed"),
                              QString::fromUtf8("Failed to send message. Check console for details."));
        statusLabel->setText(QString::fromUtf8("❌ Message send failed"));
    }
}

void MainWindow::onRefreshMessages() {
    if (currentContactType == TYPE_CONTACT && !currentContact.isEmpty()) {
        loadConversation(currentContact);
    } else if (currentContactType == TYPE_GROUP && currentGroupId >= 0) {
        loadGroupConversation(currentGroupId);
    }
    statusLabel->setText(QString::fromUtf8("✨ Messages refreshed"));
}

void MainWindow::checkForNewMessages() {
    if (!ctx || currentIdentity.isEmpty()) {
        return;
    }

    // Query for new messages since lastCheckedMessageId
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", lastCheckedMessageId);

    const char *query =
        "SELECT id, sender, created_at "
        "FROM messages "
        "WHERE recipient = $1 AND id > $2 "
        "ORDER BY id ASC";

    const char *params[2] = {
        currentIdentity.toUtf8().constData(),
        id_str
    };

    PGresult *res = PQexecParams(ctx->pg_conn, query, 2, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return;
    }

    int count = PQntuples(res);

    for (int i = 0; i < count; i++) {
        int msgId = atoi(PQgetvalue(res, i, 0));
        QString sender = QString::fromUtf8(PQgetvalue(res, i, 1));
        QString timestamp = QString::fromUtf8(PQgetvalue(res, i, 2));

        if (msgId > lastCheckedMessageId) {
            lastCheckedMessageId = msgId;
        }

        // Mark message as delivered (recipient has fetched it)
        messenger_mark_delivered(ctx, msgId);

        // Play notification sound
        notificationSound->play();

        // Show desktop notification
        QString notificationTitle = QString::fromUtf8("💌 New Message");
        QString notificationBody = QString("From: %1\n%2")
            .arg(sender)
            .arg(timestamp);

        trayIcon->showMessage(notificationTitle, notificationBody,
                              QSystemTrayIcon::Information, 5000);

        // If viewing this contact, refresh conversation
        if (currentContact == sender) {
            loadConversation(currentContact);
        }
    }

    PQclear(res);
}

void MainWindow::checkForStatusUpdates() {
    // Only check if we have an active conversation open
    if (!ctx || currentContact.isEmpty()) {
        return;
    }

    // Query for status updates on sent messages in current conversation
    // This allows the sender to see when their messages are delivered/read
    const char *query =
        "SELECT COUNT(*) FROM messages "
        "WHERE sender = $1 AND recipient = $2 "
        "AND status IN ('delivered', 'read')";

    const char *params[2] = {
        currentIdentity.toUtf8().constData(),
        currentContact.toUtf8().constData()
    };

    PGresult *res = PQexecParams(ctx->pg_conn, query, 2, NULL, params, NULL, NULL, 0);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        PQclear(res);
        return;
    }

    // If any sent messages have been delivered or read, refresh the conversation
    // to update the checkmarks
    int statusCount = atoi(PQgetvalue(res, 0, 0));
    if (statusCount > 0) {
        // Silently refresh conversation to update checkmarks
        loadConversation(currentContact);
    }

    PQclear(res);
}

void MainWindow::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) {
        show();
        raise();
        activateWindow();
    }
}

void MainWindow::onCheckForUpdates() {
    QString currentVersion = QString(PQSIGNUM_VERSION);

    printf("\n========== DEBUG: Update Check ==========\n");
    printf("Current version: %s (commit %s, built %s)\n",
           PQSIGNUM_VERSION, BUILD_HASH, BUILD_TS);

    QMessageBox::information(this, "Check for Updates",
                             QString("Current version: %1\n\nChecking latest version on GitHub...").arg(currentVersion));

    // Step 1: Fetch MAJOR.MINOR from README
    QProcess readmeProcess;
    QString majorMinor = "unknown";

#ifdef _WIN32
    QString readmeCommand = "try { "
                           "$readme = Invoke-RestMethod -Uri 'https://raw.githubusercontent.com/nocdem/dna-messenger/main/README.md' "
                           "-Headers @{'User-Agent'='DNA-Messenger'} -ErrorAction Stop; "
                           "if ($readme -match '- \\*\\*Major:\\*\\*\\s+(\\d+)') { $major = $matches[1] }; "
                           "if ($readme -match '- \\*\\*Minor:\\*\\*\\s+(\\d+)') { $minor = $matches[1] }; "
                           "Write-Output \"$major.$minor\" "
                           "} catch { Write-Output 'unknown' }";
    printf("Step 1: Fetching MAJOR.MINOR from README...\n");
    readmeProcess.start("powershell", QStringList() << "-Command" << readmeCommand);
#else
    QString readmeCommand = "curl -s -H 'User-Agent: DNA-Messenger' "
                           "'https://raw.githubusercontent.com/nocdem/dna-messenger/main/README.md' 2>/dev/null | "
                           "awk '/- \\*\\*Major:\\*\\*/ {major=$3} /- \\*\\*Minor:\\*\\*/ {minor=$3} END {print major\".\"minor}' || echo 'unknown'";
    printf("Step 1: Fetching MAJOR.MINOR from README...\n");
    readmeProcess.start("sh", QStringList() << "-c" << readmeCommand);
#endif

    if (readmeProcess.waitForFinished(10000)) {
        majorMinor = QString::fromUtf8(readmeProcess.readAllStandardOutput()).trimmed();
        printf("  Result: %s\n", majorMinor.toUtf8().constData());
    } else {
        printf("  Timed out!\n");
    }

    if (majorMinor == "unknown" || majorMinor.isEmpty() || !majorMinor.contains('.')) {
        printf("ERROR: Failed to fetch MAJOR.MINOR from README\n");
        printf("=========================================\n\n");
        QMessageBox::warning(this, "Update Check Failed",
                             "Could not fetch version info from GitHub README.");
        return;
    }

    // Step 2: Fetch PATCH (commit count) from GitHub
    QProcess commitProcess;
    QString patch = "unknown";

#ifdef _WIN32
    QString commitCommand = "try { "
                           "$commits = Invoke-RestMethod -Uri 'https://api.github.com/repos/nocdem/dna-messenger/commits?per_page=100' "
                           "-Headers @{'User-Agent'='DNA-Messenger'} -ErrorAction Stop; "
                           "Write-Output $commits.Count "
                           "} catch { Write-Output 'unknown' }";
    printf("Step 2: Fetching PATCH (commit count) from GitHub API...\n");
    commitProcess.start("powershell", QStringList() << "-Command" << commitCommand);
#else
    QString commitCommand = "curl -s -H 'User-Agent: DNA-Messenger' "
                           "'https://api.github.com/repos/nocdem/dna-messenger/commits?per_page=100' 2>/dev/null | "
                           "grep -c '\"sha\"' || echo 'unknown'";
    printf("Step 2: Fetching PATCH (commit count) from GitHub API...\n");
    commitProcess.start("sh", QStringList() << "-c" << commitCommand);
#endif

    if (commitProcess.waitForFinished(10000)) {
        patch = QString::fromUtf8(commitProcess.readAllStandardOutput()).trimmed();
        printf("  Result: %s\n", patch.toUtf8().constData());
    } else {
        printf("  Timed out!\n");
    }

    if (patch == "unknown" || patch.isEmpty()) {
        printf("ERROR: Failed to fetch commit count\n");
        printf("=========================================\n\n");
        QMessageBox::warning(this, "Update Check Failed",
                             "Could not fetch commit count from GitHub.");
        return;
    }

    // Step 3: Combine into full version
    QString latestVersion = majorMinor + "." + patch;
    printf("Step 3: Combined latest version: %s\n", latestVersion.toUtf8().constData());

    // Compare versions (semantic versioning: major.minor.patch)
    QStringList currentParts = currentVersion.split('.');
    QStringList latestParts = latestVersion.split('.');

    int currentMajor = 0, currentMinor = 0, currentPatch = 0;
    int latestMajor = 0, latestMinor = 0, latestPatch = 0;

    if (currentParts.size() >= 3) {
        currentMajor = currentParts[0].toInt();
        currentMinor = currentParts[1].toInt();
        currentPatch = currentParts[2].toInt();
    }

    if (latestParts.size() >= 3) {
        latestMajor = latestParts[0].toInt();
        latestMinor = latestParts[1].toInt();
        latestPatch = latestParts[2].toInt();
    }

    printf("Version comparison:\n");
    printf("  Current: %d.%d.%d (%s)\n", currentMajor, currentMinor, currentPatch,
           currentVersion.toUtf8().constData());
    printf("  Latest:  %d.%d.%d (%s)\n", latestMajor, latestMinor, latestPatch,
           latestVersion.toUtf8().constData());

    // Compare: major first, then minor, then patch
    bool updateAvailable = false;
    if (latestMajor > currentMajor) {
        updateAvailable = true;
    } else if (latestMajor == currentMajor && latestMinor > currentMinor) {
        updateAvailable = true;
    } else if (latestMajor == currentMajor && latestMinor == currentMinor && latestPatch > currentPatch) {
        updateAvailable = true;
    }

    if (updateAvailable) {
        printf("Result: UPDATE AVAILABLE\n");
        printf("=========================================\n\n");
        // Update available
        QMessageBox::StandardButton reply = QMessageBox::question(this, "Update Available",
                                            QString("New version available!\n\n"
                                                    "Current version: %1\n"
                                                    "Latest version:  %2\n\n"
                                                    "Do you want to update now?").arg(currentVersion, latestVersion),
                                            QMessageBox::Yes | QMessageBox::No);

        if (reply == QMessageBox::Yes) {
#ifdef _WIN32
            // Windows: Launch updater script from the repository root
            // Find the repository root by looking for .git directory
            QString exePath = QCoreApplication::applicationDirPath();
            QDir dir(exePath);

            printf("\n========== DEBUG: Windows Update ==========\n");
            printf("Executable path: %s\n", exePath.toUtf8().constData());
            printf("Looking for .git directory...\n");

            // Navigate up to find repository root (where .git exists)
            QString repoRoot;
            while (dir.cdUp()) {
                QString currentPath = dir.absolutePath();
                printf("  Checking: %s\n", currentPath.toUtf8().constData());
                if (dir.exists(".git")) {
                    repoRoot = currentPath;
                    printf("  Found .git at: %s\n", repoRoot.toUtf8().constData());
                    break;
                }
            }

            if (repoRoot.isEmpty()) {
                printf("ERROR: Could not find repository root\n");
                printf("==========================================\n\n");
                QMessageBox::critical(this, QString::fromUtf8("❌ Update Failed"),
                                     QString::fromUtf8("Could not find repository root.\n\n"
                                     "Searched from: %1\n\n"
                                     "Make sure you are running from a git repository.").arg(exePath));
                return;
            }

            QString updateScript = repoRoot + "\\update_windows.bat";
            printf("Update script path: %s\n", updateScript.toUtf8().constData());

            if (!QFileInfo::exists(updateScript)) {
                printf("ERROR: Update script not found at: %s\n", updateScript.toUtf8().constData());
                printf("==========================================\n\n");
                QMessageBox::critical(this, QString::fromUtf8("❌ Update Failed"),
                                     QString::fromUtf8("Update script not found:\n%1\n\n"
                                     "Please update manually using:\n"
                                     "git pull && cmake --build build --config Release").arg(updateScript));
                return;
            }

            printf("Launching update script...\n");

            // Build command
            QString nativeUpdateScript = QDir::toNativeSeparators(updateScript);
            QString nativeRepoRoot = QDir::toNativeSeparators(repoRoot);

            printf("Native script path: %s\n", nativeUpdateScript.toUtf8().constData());
            printf("Native repo root: %s\n", nativeRepoRoot.toUtf8().constData());
            printf("==========================================\n\n");

            // Launch updater script in new window - use cmd /k to keep window open
            // The script will change to the correct directory itself
            QProcess::startDetached("cmd", QStringList() << "/k" << nativeUpdateScript);

            // Quit immediately so updater can replace the executable
            QApplication::quit();
#else
            // Linux: Update in place
            QProcess updateProcess;
            updateProcess.start("sh", QStringList()
                              << "-c"
                              << "REPO=$(git rev-parse --show-toplevel 2>/dev/null); "
                                 "if [ -n \"$REPO\" ]; then "
                                 "cd \"$REPO\" && git pull origin main && "
                                 "cd build && cmake .. && make -j$(nproc); "
                                 "else echo 'Not a git repository'; fi");

            if (updateProcess.waitForFinished(60000)) {  // 60 second timeout
                QMessageBox::information(this, "Update Complete",
                                         "Update complete!\n\n"
                                         "Please restart DNA Messenger to use the new version.");
                QApplication::quit();
            } else {
                QMessageBox::critical(this, "Update Failed",
                                      "Update failed!\n"
                                      "Make sure you're running from the git repository.");
            }
#endif
        }
    } else {
        printf("Result: UP TO DATE\n");
        printf("=========================================\n\n");
        QMessageBox::information(this, "Up to Date",
                                 QString("You are running the latest version: %1").arg(currentVersion));
    }
}

void MainWindow::onThemeIO() {
    applyTheme("io");
}

void MainWindow::onThemeClub() {
    applyTheme("club");
}

void MainWindow::applyTheme(const QString &themeName) {
    currentTheme = themeName;

    // Save theme preference
    QSettings settings("DNA Messenger", "GUI");
    settings.setValue("theme", themeName);

    // Calculate font sizes based on scale (base sizes: 16px menu, 18px list, 24px headers, 13px meta, 18px message)
    int menuFontSize = static_cast<int>(16 * fontScale);
    int listFontSize = static_cast<int>(18 * fontScale);
    int headerFontSize = static_cast<int>(24 * fontScale);
    int metaFontSize = static_cast<int>(13 * fontScale);
    int messageFontSize = static_cast<int>(18 * fontScale);

    // Apply theme colors based on selection
    if (themeName == "io") {
        // cpunk.io theme - dark teal with cyan accents
        setStyleSheet(QString(
            "QMainWindow {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
            "       stop:0 #0A2A2E, stop:1 #0D3438);"
            "}"
            "QMenuBar {"
            "   background: #0D3438;"
            "   color: #00D9FF;"
            "   padding: 8px;"
            "   font-family: 'Orbitron';"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   border-bottom: 2px solid #00D9FF;"
            "}"
            "QMenuBar::item {"
            "   padding: 8px 15px;"
            "   color: #00D9FF;"
            "}"
            "QMenuBar::item:selected {"
            "   background: rgba(0, 217, 255, 0.2);"
            "   border-radius: 5px;"
            "}"
            "QMenu {"
            "   background: #0D3438;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 10px;"
            "   padding: 8px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #00D9FF;"
            "}"
            "QMenu::item {"
            "   padding: 10px 20px;"
            "   color: #00D9FF;"
            "}"
            "QMenu::item:selected {"
            "   background: rgba(0, 217, 255, 0.3);"
            "   border-radius: 5px;"
            "}"
            "QStatusBar {"
            "   background: #0D3438;"
            "   color: #00D9FF;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   padding: 8px;"
            "   border-top: 2px solid #00D9FF;"
            "}"
        ).arg(menuFontSize));

        contactList->parentWidget()->setStyleSheet(
            "QWidget {"
            "   background: #0A2A2E;"
            "   border-radius: 15px;"
            "   padding: 10px;"
            "}"
        );

        contactList->parentWidget()->findChild<QLabel*>()->setStyleSheet(QString(
            "font-weight: bold; "
            "font-family: 'Orbitron'; font-size: %1px; "
            "color: #00D9FF; "
            "background: transparent; "
            "padding: 10px;"
        ).arg(headerFontSize));

        contactList->setStyleSheet(QString(
            "QListWidget {"
            "   background: #0D3438;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 10px;"
            "   padding: 8px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #00D9FF;"
            "}"
            "QListWidget::item {"
            "   background: rgba(0, 217, 255, 0.1);"
            "   border: 1px solid rgba(0, 217, 255, 0.3);"
            "   border-radius: 10px;"
            "   padding: 15px;"
            "   margin: 5px;"
            "   color: #00D9FF;"
            "}"
            "QListWidget::item:hover {"
            "   background: rgba(0, 217, 255, 0.2);"
            "   border: 2px solid #00D9FF;"
            "}"
            "QListWidget::item:selected {"
            "   background: rgba(0, 217, 255, 0.3);"
            "   color: #FFFFFF;"
            "   font-weight: bold;"
            "   border: 2px solid #00D9FF;"
            "}"
        ).arg(listFontSize));

        refreshButton->setStyleSheet(QString(
            "QPushButton {"
            "   background: rgba(0, 217, 255, 0.2);"
            "   color: #00D9FF;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 15px;"
            "   padding: 15px;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "}"
            "QPushButton:hover {"
            "   background: rgba(0, 217, 255, 0.3);"
            "   border: 2px solid #33E6FF;"
            "}"
            "QPushButton:pressed {"
            "   background: rgba(0, 217, 255, 0.4);"
            "   border: 2px solid #00D9FF;"
            "}"
        ).arg(listFontSize));

        messageDisplay->parentWidget()->setStyleSheet(
            "QWidget {"
            "   background: #0A2A2E;"
            "   border-radius: 15px;"
            "   padding: 10px;"
            "}"
        );

        messageDisplay->parentWidget()->findChildren<QLabel*>()[0]->setStyleSheet(QString(
            "font-weight: bold; "
            "font-family: 'Orbitron'; font-size: %1px; "
            "color: #00D9FF; "
            "background: transparent; "
            "padding: 10px;"
        ).arg(headerFontSize));

        messageDisplay->setStyleSheet(QString(
            "QTextEdit {"
            "   background: #0D3438;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 10px;"
            "   padding: 15px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #00D9FF;"
            "}"
        ).arg(menuFontSize));

        messageInput->setStyleSheet(QString(
            "QLineEdit {"
            "   background: #0D3438;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 15px;"
            "   padding: 15px 20px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #00D9FF;"
            "}"
            "QLineEdit:focus {"
            "   border: 2px solid #33E6FF;"
            "   background: rgba(0, 217, 255, 0.1);"
            "}"
        ).arg(listFontSize));

        sendButton->setStyleSheet(QString(
            "QPushButton {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "       stop:0 #FF6B35, stop:1 #FF8C42);"
            "   color: white;"
            "   border: 2px solid #FF6B35;"
            "   border-radius: 15px;"
            "   padding: 15px 30px;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "}"
            "QPushButton:hover {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "       stop:0 #FF8C42, stop:1 #FFA55C);"
            "   border: 2px solid #FF8C42;"
            "}"
            "QPushButton:pressed {"
            "   background: #FF5722;"
            "   border: 2px solid #E64A19;"
            "}"
        ).arg(listFontSize));

        statusLabel->setText(QString::fromUtf8("🌊 Theme: cpunk.io (Cyan)"));

    } else if (themeName == "club") {
        // cpunk.club theme - dark brown with orange accents
        setStyleSheet(QString(
            "QMainWindow {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:1, "
            "       stop:0 #1A1410, stop:1 #2B1F16);"
            "}"
            "QMenuBar {"
            "   background: #2B1F16;"
            "   color: #FF8C42;"
            "   padding: 8px;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   border-bottom: 2px solid #FF8C42;"
            "}"
            "QMenuBar::item {"
            "   padding: 8px 15px;"
            "   color: #FF8C42;"
            "}"
            "QMenuBar::item:selected {"
            "   background: rgba(255, 140, 66, 0.2);"
            "   border-radius: 5px;"
            "}"
            "QMenu {"
            "   background: #2B1F16;"
            "   border: 2px solid #FF8C42;"
            "   border-radius: 10px;"
            "   padding: 8px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #FF8C42;"
            "}"
            "QMenu::item {"
            "   padding: 10px 20px;"
            "   color: #FF8C42;"
            "}"
            "QMenu::item:selected {"
            "   background: rgba(255, 140, 66, 0.3);"
            "   border-radius: 5px;"
            "}"
            "QStatusBar {"
            "   background: #2B1F16;"
            "   color: #FF8C42;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   padding: 8px;"
            "   border-top: 2px solid #FF8C42;"
            "}"
        ).arg(menuFontSize));

        contactList->parentWidget()->setStyleSheet(
            "QWidget {"
            "   background: #1A1410;"
            "   border-radius: 15px;"
            "   padding: 10px;"
            "}"
        );

        contactList->parentWidget()->findChild<QLabel*>()->setStyleSheet(QString(
            "font-weight: bold; "
            "font-family: 'Orbitron'; font-size: %1px; "
            "color: #FF8C42; "
            "background: transparent; "
            "padding: 10px;"
        ).arg(headerFontSize));

        contactList->setStyleSheet(QString(
            "QListWidget {"
            "   background: #2B1F16;"
            "   border: 2px solid #FF8C42;"
            "   border-radius: 10px;"
            "   padding: 8px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #FFB380;"
            "}"
            "QListWidget::item {"
            "   background: rgba(255, 140, 66, 0.1);"
            "   border: 1px solid rgba(255, 140, 66, 0.3);"
            "   border-radius: 10px;"
            "   padding: 15px;"
            "   margin: 5px;"
            "   color: #FFB380;"
            "}"
            "QListWidget::item:hover {"
            "   background: rgba(255, 140, 66, 0.2);"
            "   border: 2px solid #FF8C42;"
            "}"
            "QListWidget::item:selected {"
            "   background: rgba(255, 140, 66, 0.3);"
            "   color: #FFFFFF;"
            "   font-weight: bold;"
            "   border: 2px solid #FF8C42;"
            "}"
        ).arg(listFontSize));

        refreshButton->setStyleSheet(QString(
            "QPushButton {"
            "   background: rgba(255, 140, 66, 0.2);"
            "   color: #FF8C42;"
            "   border: 2px solid #FF8C42;"
            "   border-radius: 15px;"
            "   padding: 15px;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "}"
            "QPushButton:hover {"
            "   background: rgba(255, 140, 66, 0.3);"
            "   border: 2px solid #FFB380;"
            "}"
            "QPushButton:pressed {"
            "   background: rgba(255, 140, 66, 0.4);"
            "   border: 2px solid #FF8C42;"
            "}"
        ).arg(listFontSize));

        messageDisplay->parentWidget()->setStyleSheet(
            "QWidget {"
            "   background: #1A1410;"
            "   border-radius: 15px;"
            "   padding: 10px;"
            "}"
        );

        messageDisplay->parentWidget()->findChildren<QLabel*>()[0]->setStyleSheet(QString(
            "font-weight: bold; "
            "font-family: 'Orbitron'; font-size: %1px; "
            "   color: #FF8C42; "
            "background: transparent; "
            "padding: 10px;"
        ).arg(headerFontSize));

        messageDisplay->setStyleSheet(QString(
            "QTextEdit {"
            "   background: #2B1F16;"
            "   border: 2px solid #FF8C42;"
            "   border-radius: 10px;"
            "   padding: 15px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #FFB380;"
            "}"
        ).arg(menuFontSize));

        messageInput->setStyleSheet(QString(
            "QLineEdit {"
            "   background: #2B1F16;"
            "   border: 2px solid #FF8C42;"
            "   border-radius: 15px;"
            "   padding: 15px 20px;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "   color: #FFB380;"
            "}"
            "QLineEdit:focus {"
            "   border: 2px solid #FFB380;"
            "   background: rgba(255, 140, 66, 0.1);"
            "}"
        ).arg(listFontSize));

        sendButton->setStyleSheet(QString(
            "QPushButton {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "       stop:0 #00D9FF, stop:1 #00B8CC);"
            "   color: white;"
            "   border: 2px solid #00D9FF;"
            "   border-radius: 15px;"
            "   padding: 15px 30px;"
            "   font-weight: bold;"
            "   font-family: 'Orbitron'; font-size: %1px;"
            "}"
            "QPushButton:hover {"
            "   background: qlineargradient(x1:0, y1:0, x2:1, y2:0, "
            "       stop:0 #00E6FF, stop:1 #00D9FF);"
            "   border: 2px solid #00E6FF;"
            "}"
            "QPushButton:pressed {"
            "   background: #00B8CC;"
            "   border: 2px solid #009AA8;"
            "}"
        ).arg(listFontSize));

        statusLabel->setText(QString::fromUtf8("🔥 Theme: cpunk.club (Orange)"));
    }
    
    // Reload conversation to apply new message bubble colors
    if (!currentContact.isEmpty()) {
        loadConversation(currentContact);
    }
}

void MainWindow::onFontScaleSmall() {
    applyFontScale(1.0);
}

void MainWindow::onFontScaleMedium() {
    applyFontScale(2.0);
}

void MainWindow::onFontScaleLarge() {
    applyFontScale(3.0);
}

void MainWindow::onFontScaleExtraLarge() {
    applyFontScale(4.0);
}

void MainWindow::applyFontScale(double scale) {
    fontScale = scale;
    
    // Save font scale preference
    QSettings settings("DNA Messenger", "GUI");
    settings.setValue("fontScale", scale);
    
    // Re-apply the current theme with new font sizes
    applyTheme(currentTheme);
    
    // Update status bar
    QString scaleText;
    if (scale == 1.0) scaleText = "Small (1x)";
    else if (scale == 2.0) scaleText = "Medium (2x)";
    else if (scale == 3.0) scaleText = "Large (3x)";
    else if (scale == 4.0) scaleText = "Extra Large (4x)";
    else scaleText = QString::number(scale) + "x";
    
    statusLabel->setText(QString::fromUtf8("📏 Font Scale: %1").arg(scaleText));
}

void MainWindow::onAddRecipients() {
    if (currentContact.isEmpty()) {
        QMessageBox::warning(this, "No Contact Selected",
                             "Please select a primary contact first");
        return;
    }

    // Create dialog for multi-selection
    QDialog dialog(this);
    dialog.setWindowTitle(QString::fromUtf8("➕ Add Recipients"));
    dialog.setModal(true);

    QVBoxLayout *layout = new QVBoxLayout(&dialog);

    QLabel *label = new QLabel(QString::fromUtf8("Select additional recipients:"));
    layout->addWidget(label);

    // List widget with multi-selection
    QListWidget *listWidget = new QListWidget(&dialog);
    listWidget->setSelectionMode(QAbstractItemView::MultiSelection);

    // Load all contacts except current contact
    char **identities = NULL;
    int count = 0;

    if (messenger_get_contact_list(ctx, &identities, &count) == 0) {
        for (int i = 0; i < count; i++) {
            QString contact = QString::fromUtf8(identities[i]);
            // Don't include current contact or sender
            if (contact != currentContact && contact != currentIdentity) {
                QListWidgetItem *item = new QListWidgetItem(QString::fromUtf8("👤 ") + contact);
                listWidget->addItem(item);

                // Pre-select if already in additionalRecipients
                if (additionalRecipients.contains(contact)) {
                    item->setSelected(true);
                }
            }
            free(identities[i]);
        }
        free(identities);
    }

    layout->addWidget(listWidget);

    // Buttons
    QHBoxLayout *buttonLayout = new QHBoxLayout;
    QPushButton *okButton = new QPushButton(QString::fromUtf8("✅ OK"));
    QPushButton *cancelButton = new QPushButton(QString::fromUtf8("❌ Cancel"));

    connect(okButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    buttonLayout->addWidget(okButton);
    buttonLayout->addWidget(cancelButton);
    layout->addLayout(buttonLayout);

    if (dialog.exec() == QDialog::Accepted) {
        // Update additionalRecipients list
        additionalRecipients.clear();

        for (int i = 0; i < listWidget->count(); i++) {
            QListWidgetItem *item = listWidget->item(i);
            if (item->isSelected()) {
                // Strip emoji prefix "👤 "
                QString contact = item->text();
                if (contact.startsWith(QString::fromUtf8("👤 "))) {
                    contact = contact.mid(3);
                }
                additionalRecipients.append(contact);
            }
        }

        // Update recipients label
        QString recipientsText = QString::fromUtf8("📨 To: ") + currentContact;
        if (!additionalRecipients.isEmpty()) {
            recipientsText += ", " + additionalRecipients.join(", ");
        }
        recipientsLabel->setText(recipientsText);

        statusLabel->setText(QString::fromUtf8("✨ %1 additional recipient(s) added").arg(additionalRecipients.count()));
    }
}

// Window dragging
void MainWindow::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        // Map click position to titleBar's coordinate system
        QPoint titleBarPos = titleBar->mapFromGlobal(event->globalPos());
        if (titleBar->rect().contains(titleBarPos)) {
            dragPosition = event->globalPos() - frameGeometry().topLeft();
            event->accept();
        }
    }
}

void MainWindow::mouseMoveEvent(QMouseEvent *event) {
    if (event->buttons() & Qt::LeftButton && !dragPosition.isNull()) {
        move(event->globalPos() - dragPosition);
        event->accept();
    }
}

// Window control buttons
void MainWindow::onMinimizeWindow() {
    showMinimized();
}

void MainWindow::onCloseWindow() {
    QApplication::quit();
}

// Group management functions
void MainWindow::onCreateGroup() {
    QMessageBox::information(this, "Create Group", "Group creation dialog - Coming soon!");
    // TODO: Implement group creation dialog
}

void MainWindow::onGroupSettings() {
    if (currentContactType != TYPE_GROUP || currentGroupId < 0) {
        QMessageBox::warning(this, "No Group Selected", "Please select a group first");
        return;
    }
    QMessageBox::information(this, "Group Settings", "Group settings dialog - Coming soon!");
    // TODO: Implement group settings dialog (rename, change description)
}

void MainWindow::onManageGroupMembers() {
    if (currentContactType != TYPE_GROUP || currentGroupId < 0) {
        QMessageBox::warning(this, "No Group Selected", "Please select a group first");
        return;
    }
    QMessageBox::information(this, "Manage Members", "Member management dialog - Coming soon!");
    // TODO: Implement member management dialog (add/remove members)
}
