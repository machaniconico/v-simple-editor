#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QPushButton;
class QTabWidget;

class CredentialDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CredentialDialog(QWidget *parent = nullptr);

private:
    void reloadAllStatuses();
    void reloadYouTubeStatus();
    void reloadVimeoStatus();
    void reloadInstagramStatus();
    void reloadXStatus();
    void reloadTwitchStatus();

    QTabWidget *m_tabWidget = nullptr;

    QLineEdit *m_youtubeClientIdEdit = nullptr;
    QPushButton *m_youtubeClientIdSaveButton = nullptr;
    QPushButton *m_youtubeClientIdClearButton = nullptr;
    QLineEdit *m_youtubeClientSecretEdit = nullptr;
    QPushButton *m_youtubeClientSecretSaveButton = nullptr;
    QPushButton *m_youtubeClientSecretClearButton = nullptr;
    QLabel *m_youtubeStatusLabel = nullptr;

    QLineEdit *m_vimeoClientIdEdit = nullptr;
    QPushButton *m_vimeoClientIdSaveButton = nullptr;
    QPushButton *m_vimeoClientIdClearButton = nullptr;
    QLineEdit *m_vimeoClientSecretEdit = nullptr;
    QPushButton *m_vimeoClientSecretSaveButton = nullptr;
    QPushButton *m_vimeoClientSecretClearButton = nullptr;
    QLabel *m_vimeoStatusLabel = nullptr;

    QLineEdit *m_instagramAccessTokenEdit = nullptr;
    QPushButton *m_instagramAccessTokenSaveButton = nullptr;
    QPushButton *m_instagramAccessTokenClearButton = nullptr;
    QLineEdit *m_instagramUserIdEdit = nullptr;
    QPushButton *m_instagramUserIdSaveButton = nullptr;
    QPushButton *m_instagramUserIdClearButton = nullptr;
    QLabel *m_instagramStatusLabel = nullptr;

    QLineEdit *m_xBearerTokenEdit = nullptr;
    QPushButton *m_xBearerTokenSaveButton = nullptr;
    QPushButton *m_xBearerTokenClearButton = nullptr;
    QLabel *m_xStatusLabel = nullptr;

    QLineEdit *m_twitchStreamKeyEdit = nullptr;
    QPushButton *m_twitchStreamKeySaveButton = nullptr;
    QPushButton *m_twitchStreamKeyClearButton = nullptr;
    QLabel *m_twitchStatusLabel = nullptr;

    QPushButton *m_reloadAllButton = nullptr;
};
