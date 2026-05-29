#include "CredentialDialog.h"

#include "CredentialStore.h"
#include "CredentialVault.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

namespace {

QLineEdit *createMaskedLineEdit(QWidget *parent)
{
    auto *edit = new QLineEdit(parent);
    edit->setEchoMode(QLineEdit::Password);
    return edit;
}

QWidget *createCredentialRow(QLineEdit *edit,
                             QPushButton *saveButton,
                             QPushButton *clearButton,
                             QWidget *parent)
{
    auto *rowWidget = new QWidget(parent);
    auto *rowLayout = new QHBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(edit, 1);
    rowLayout->addWidget(saveButton);
    rowLayout->addWidget(clearButton);
    return rowWidget;
}

QString sourceStatusText(bool envSet, bool settingsSet, bool vaultSet, const QString &value)
{
    if (envSet) {
        return QStringLiteral("env=%1").arg(creds::CredentialStore::maskedDisplay(value));
    }
    if (vaultSet) {
        return QStringLiteral("vault=%1").arg(creds::CredentialStore::maskedDisplay(value));
    }
    if (settingsSet) {
        return QStringLiteral("settings=%1").arg(creds::CredentialStore::maskedDisplay(value));
    }
    return QStringLiteral("(unset)");
}

QLabel *createStatusLabel(QWidget *parent)
{
    auto *label = new QLabel(parent);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QWidget *createPlatformTab(const QString &description,
                           QFormLayout *formLayout,
                           QLabel *statusLabel,
                           QWidget *parent)
{
    auto *tab = new QWidget(parent);
    auto *layout = new QVBoxLayout(tab);
    auto *descriptionLabel = new QLabel(description, tab);
    descriptionLabel->setWordWrap(true);
    layout->addWidget(descriptionLabel);
    layout->addLayout(formLayout);
    layout->addWidget(statusLabel);
    layout->addStretch(1);
    return tab;
}

} // namespace

CredentialDialog::CredentialDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("配信認証情報"));
    resize(600, 400);

    auto *mainLayout = new QVBoxLayout(this);

    m_tabWidget = new QTabWidget(this);
    mainLayout->addWidget(m_tabWidget, 1);

    m_youtubeClientIdEdit = createMaskedLineEdit(this);
    m_youtubeClientIdSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_youtubeClientIdClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_youtubeClientSecretEdit = createMaskedLineEdit(this);
    m_youtubeClientSecretSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_youtubeClientSecretClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_youtubeStatusLabel = createStatusLabel(this);
    auto *youtubeForm = new QFormLayout;
    youtubeForm->addRow(QStringLiteral("Client ID:"),
                        createCredentialRow(m_youtubeClientIdEdit,
                                            m_youtubeClientIdSaveButton,
                                            m_youtubeClientIdClearButton,
                                            this));
    youtubeForm->addRow(QStringLiteral("Client Secret:"),
                        createCredentialRow(m_youtubeClientSecretEdit,
                                            m_youtubeClientSecretSaveButton,
                                            m_youtubeClientSecretClearButton,
                                            this));
    m_tabWidget->addTab(
        createPlatformTab(
            QStringLiteral("画面下に Google Cloud Console で発行した OAuth 2.0 Client ID/Secret を入力"),
            youtubeForm,
            m_youtubeStatusLabel,
            this),
        QStringLiteral("YouTube"));

    m_vimeoClientIdEdit = createMaskedLineEdit(this);
    m_vimeoClientIdSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_vimeoClientIdClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_vimeoClientSecretEdit = createMaskedLineEdit(this);
    m_vimeoClientSecretSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_vimeoClientSecretClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_vimeoStatusLabel = createStatusLabel(this);
    auto *vimeoForm = new QFormLayout;
    vimeoForm->addRow(QStringLiteral("Client ID:"),
                      createCredentialRow(m_vimeoClientIdEdit,
                                          m_vimeoClientIdSaveButton,
                                          m_vimeoClientIdClearButton,
                                          this));
    vimeoForm->addRow(QStringLiteral("Client Secret:"),
                      createCredentialRow(m_vimeoClientSecretEdit,
                                          m_vimeoClientSecretSaveButton,
                                          m_vimeoClientSecretClearButton,
                                          this));
    m_tabWidget->addTab(
        createPlatformTab(
            QStringLiteral("Vimeo Developer アプリで発行した Client ID/Secret を入力"),
            vimeoForm,
            m_vimeoStatusLabel,
            this),
        QStringLiteral("Vimeo"));

    m_instagramAccessTokenEdit = createMaskedLineEdit(this);
    m_instagramAccessTokenSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_instagramAccessTokenClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_instagramUserIdEdit = createMaskedLineEdit(this);
    m_instagramUserIdSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_instagramUserIdClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_instagramStatusLabel = createStatusLabel(this);
    auto *instagramForm = new QFormLayout;
    instagramForm->addRow(QStringLiteral("Access Token:"),
                          createCredentialRow(m_instagramAccessTokenEdit,
                                              m_instagramAccessTokenSaveButton,
                                              m_instagramAccessTokenClearButton,
                                              this));
    instagramForm->addRow(QStringLiteral("IG User ID:"),
                          createCredentialRow(m_instagramUserIdEdit,
                                              m_instagramUserIdSaveButton,
                                              m_instagramUserIdClearButton,
                                              this));
    m_tabWidget->addTab(
        createPlatformTab(
            QStringLiteral("Instagram Graph API の access token / IG User ID を入力"),
            instagramForm,
            m_instagramStatusLabel,
            this),
        QStringLiteral("Instagram"));

    m_xBearerTokenEdit = createMaskedLineEdit(this);
    m_xBearerTokenSaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_xBearerTokenClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_xStatusLabel = createStatusLabel(this);
    auto *xForm = new QFormLayout;
    xForm->addRow(QStringLiteral("Bearer Token:"),
                  createCredentialRow(m_xBearerTokenEdit,
                                      m_xBearerTokenSaveButton,
                                      m_xBearerTokenClearButton,
                                      this));
    m_tabWidget->addTab(
        createPlatformTab(
            QStringLiteral("X API で発行した Bearer Token を入力"),
            xForm,
            m_xStatusLabel,
            this),
        QStringLiteral("X"));

    m_twitchStreamKeyEdit = createMaskedLineEdit(this);
    m_twitchStreamKeySaveButton = new QPushButton(QStringLiteral("Save"), this);
    m_twitchStreamKeyClearButton = new QPushButton(QStringLiteral("Clear"), this);
    m_twitchStatusLabel = createStatusLabel(this);
    auto *twitchForm = new QFormLayout;
    twitchForm->addRow(QStringLiteral("Stream Key:"),
                       createCredentialRow(m_twitchStreamKeyEdit,
                                           m_twitchStreamKeySaveButton,
                                           m_twitchStreamKeyClearButton,
                                           this));
    m_tabWidget->addTab(
        createPlatformTab(
            QStringLiteral("Twitch 配信用の stream key を入力"),
            twitchForm,
            m_twitchStatusLabel,
            this),
        QStringLiteral("Twitch"));

    m_reloadAllButton = new QPushButton(QStringLiteral("すべて再読み込み"), this);
    auto *buttonBox = new QDialogButtonBox(this);
    auto *closeButton = buttonBox->addButton(QDialogButtonBox::Close);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    auto *bottomLayout = new QHBoxLayout;
    bottomLayout->addWidget(m_reloadAllButton);
    bottomLayout->addStretch(1);
    bottomLayout->addWidget(buttonBox);
    mainLayout->addLayout(bottomLayout);

    connect(m_reloadAllButton, &QPushButton::clicked,
            this, &CredentialDialog::reloadAllStatuses);

    connect(m_youtubeClientIdSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("youtube_oauth/client_id"),
                                    m_youtubeClientIdEdit->text().trimmed());
        reloadYouTubeStatus();
    });
    connect(m_youtubeClientIdClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("youtube_oauth/client_id"));
        m_youtubeClientIdEdit->clear();
        reloadYouTubeStatus();
    });
    connect(m_youtubeClientSecretSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("youtube_oauth/client_secret"),
                                    m_youtubeClientSecretEdit->text().trimmed());
        reloadYouTubeStatus();
    });
    connect(m_youtubeClientSecretClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("youtube_oauth/client_secret"));
        m_youtubeClientSecretEdit->clear();
        reloadYouTubeStatus();
    });

    connect(m_vimeoClientIdSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("vimeo_oauth/client_id"),
                                    m_vimeoClientIdEdit->text().trimmed());
        reloadVimeoStatus();
    });
    connect(m_vimeoClientIdClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("vimeo_oauth/client_id"));
        m_vimeoClientIdEdit->clear();
        reloadVimeoStatus();
    });
    connect(m_vimeoClientSecretSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("vimeo_oauth/client_secret"),
                                    m_vimeoClientSecretEdit->text().trimmed());
        reloadVimeoStatus();
    });
    connect(m_vimeoClientSecretClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("vimeo_oauth/client_secret"));
        m_vimeoClientSecretEdit->clear();
        reloadVimeoStatus();
    });

    connect(m_instagramAccessTokenSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("instagram/access_token"),
                                    m_instagramAccessTokenEdit->text().trimmed());
        reloadInstagramStatus();
    });
    connect(m_instagramAccessTokenClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("instagram/access_token"));
        m_instagramAccessTokenEdit->clear();
        reloadInstagramStatus();
    });
    connect(m_instagramUserIdSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("instagram/ig_user_id"),
                                    m_instagramUserIdEdit->text().trimmed());
        reloadInstagramStatus();
    });
    connect(m_instagramUserIdClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("instagram/ig_user_id"));
        m_instagramUserIdEdit->clear();
        reloadInstagramStatus();
    });

    connect(m_xBearerTokenSaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("x_video/bearer_token"),
                                    m_xBearerTokenEdit->text().trimmed());
        reloadXStatus();
    });
    connect(m_xBearerTokenClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("x_video/bearer_token"));
        m_xBearerTokenEdit->clear();
        reloadXStatus();
    });

    connect(m_twitchStreamKeySaveButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::set(QStringLiteral("twitch/stream_key"),
                                    m_twitchStreamKeyEdit->text().trimmed());
        reloadTwitchStatus();
    });
    connect(m_twitchStreamKeyClearButton, &QPushButton::clicked, this, [this]() {
        creds::CredentialStore::clear(QStringLiteral("twitch/stream_key"));
        m_twitchStreamKeyEdit->clear();
        reloadTwitchStatus();
    });

    // Phase 5C: 起動時に QSettings 平文 sensitive 値を Vault に自動移行。
    // Vault unsupported (non-Windows) なら no-op。
    const int migrated = creds::CredentialStore::migrateSensitiveKeysToVault();
    if (migrated > 0) {
        qInfo().noquote()
            << QStringLiteral("CredentialDialog: %1 sensitive key(s) migrated to %2")
                   .arg(migrated)
                   .arg(creds::CredentialVault::backendName());
    }

    reloadAllStatuses();
}

void CredentialDialog::reloadAllStatuses()
{
    reloadYouTubeStatus();
    reloadVimeoStatus();
    reloadInstagramStatus();
    reloadXStatus();
    reloadTwitchStatus();
}

void CredentialDialog::reloadYouTubeStatus()
{
    QSettings settings;
    const bool clientIdEnvSet = qEnvironmentVariableIsSet("VEDITOR_YOUTUBE_CLIENT_ID");
    const bool clientIdVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/youtube_oauth/client_id"));
    const bool clientIdSettingsSet = settings.contains(QStringLiteral("youtube_oauth/client_id"));
    const QString clientIdValue = creds::CredentialStore::get(
        "VEDITOR_YOUTUBE_CLIENT_ID",
        QStringLiteral("youtube_oauth/client_id"));
    const bool clientSecretEnvSet = qEnvironmentVariableIsSet("VEDITOR_YOUTUBE_CLIENT_SECRET");
    const bool clientSecretVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/youtube_oauth/client_secret"));
    const bool clientSecretSettingsSet = settings.contains(QStringLiteral("youtube_oauth/client_secret"));
    const QString clientSecretValue = creds::CredentialStore::get(
        "VEDITOR_YOUTUBE_CLIENT_SECRET",
        QStringLiteral("youtube_oauth/client_secret"));

    m_youtubeStatusLabel->setText(
        QStringLiteral("現在:\nClient ID: %1\nClient Secret: %2")
            .arg(sourceStatusText(clientIdEnvSet,
                                  clientIdSettingsSet,
                                  clientIdVaultSet,
                                  clientIdValue))
            .arg(sourceStatusText(clientSecretEnvSet,
                                  clientSecretSettingsSet,
                                  clientSecretVaultSet,
                                  clientSecretValue)));
}

void CredentialDialog::reloadVimeoStatus()
{
    QSettings settings;
    const bool clientIdEnvSet = qEnvironmentVariableIsSet("VEDITOR_VIMEO_CLIENT_ID");
    const bool clientIdVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/vimeo_oauth/client_id"));
    const bool clientIdSettingsSet = settings.contains(QStringLiteral("vimeo_oauth/client_id"));
    const QString clientIdValue = creds::CredentialStore::get(
        "VEDITOR_VIMEO_CLIENT_ID",
        QStringLiteral("vimeo_oauth/client_id"));
    const bool clientSecretEnvSet = qEnvironmentVariableIsSet("VEDITOR_VIMEO_CLIENT_SECRET");
    const bool clientSecretVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/vimeo_oauth/client_secret"));
    const bool clientSecretSettingsSet = settings.contains(QStringLiteral("vimeo_oauth/client_secret"));
    const QString clientSecretValue = creds::CredentialStore::get(
        "VEDITOR_VIMEO_CLIENT_SECRET",
        QStringLiteral("vimeo_oauth/client_secret"));

    m_vimeoStatusLabel->setText(
        QStringLiteral("現在:\nClient ID: %1\nClient Secret: %2")
            .arg(sourceStatusText(clientIdEnvSet,
                                  clientIdSettingsSet,
                                  clientIdVaultSet,
                                  clientIdValue))
            .arg(sourceStatusText(clientSecretEnvSet,
                                  clientSecretSettingsSet,
                                  clientSecretVaultSet,
                                  clientSecretValue)));
}

void CredentialDialog::reloadInstagramStatus()
{
    QSettings settings;
    const bool accessTokenEnvSet = qEnvironmentVariableIsSet("VEDITOR_IG_ACCESS_TOKEN");
    const bool accessTokenVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/instagram/access_token"));
    const bool accessTokenSettingsSet = settings.contains(QStringLiteral("instagram/access_token"));
    const QString accessTokenValue = creds::CredentialStore::get(
        "VEDITOR_IG_ACCESS_TOKEN",
        QStringLiteral("instagram/access_token"));
    const bool userIdEnvSet = qEnvironmentVariableIsSet("VEDITOR_IG_USER_ID");
    const bool userIdVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/instagram/ig_user_id"));
    const bool userIdSettingsSet = settings.contains(QStringLiteral("instagram/ig_user_id"));
    const QString userIdValue = creds::CredentialStore::get(
        "VEDITOR_IG_USER_ID",
        QStringLiteral("instagram/ig_user_id"));

    m_instagramStatusLabel->setText(
        QStringLiteral("現在:\nAccess Token: %1\nIG User ID: %2")
            .arg(sourceStatusText(accessTokenEnvSet,
                                  accessTokenSettingsSet,
                                  accessTokenVaultSet,
                                  accessTokenValue))
            .arg(sourceStatusText(userIdEnvSet,
                                  userIdSettingsSet,
                                  userIdVaultSet,
                                  userIdValue)));
}

void CredentialDialog::reloadXStatus()
{
    QSettings settings;
    const bool bearerTokenEnvSet = qEnvironmentVariableIsSet("VEDITOR_X_BEARER_TOKEN");
    const bool bearerTokenVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/x_video/bearer_token"));
    const bool bearerTokenSettingsSet = settings.contains(QStringLiteral("x_video/bearer_token"));
    const QString bearerTokenValue = creds::CredentialStore::get(
        "VEDITOR_X_BEARER_TOKEN",
        QStringLiteral("x_video/bearer_token"));

    m_xStatusLabel->setText(
        QStringLiteral("現在: %1")
            .arg(sourceStatusText(bearerTokenEnvSet,
                                  bearerTokenSettingsSet,
                                  bearerTokenVaultSet,
                                  bearerTokenValue)));
}

void CredentialDialog::reloadTwitchStatus()
{
    QSettings settings;
    const bool streamKeyEnvSet = false;
    const bool streamKeyVaultSet = creds::CredentialVault::isSupported()
        && creds::CredentialVault::exists(QStringLiteral("v-simple-editor/twitch/stream_key"));
    const bool streamKeySettingsSet = settings.contains(QStringLiteral("twitch/stream_key"));
    const QString streamKeyValue = creds::CredentialStore::get(
        nullptr,
        QStringLiteral("twitch/stream_key"));

    m_twitchStatusLabel->setText(
        QStringLiteral("現在: %1")
            .arg(sourceStatusText(streamKeyEnvSet,
                                  streamKeySettingsSet,
                                  streamKeyVaultSet,
                                  streamKeyValue)));
}
