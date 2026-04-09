#pragma once

#include <QWidget>
#include <QPainter>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QFont>

class WelcomeWidget : public QWidget
{
    Q_OBJECT

public:
    explicit WelcomeWidget(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        auto *layout = new QVBoxLayout(this);
        layout->setAlignment(Qt::AlignCenter);
        layout->setSpacing(16);

        // Logo / title
        auto *titleLabel = new QLabel("V Editor Simple");
        titleLabel->setAlignment(Qt::AlignCenter);
        QFont titleFont("Segoe UI", 32, QFont::Bold);
        titleLabel->setFont(titleFont);
        titleLabel->setStyleSheet("color: #e94560;");

        auto *subtitleLabel = new QLabel("Simple UI. Full-Featured Video Editing.");
        subtitleLabel->setAlignment(Qt::AlignCenter);
        QFont subFont("Segoe UI", 14);
        subtitleLabel->setFont(subFont);
        subtitleLabel->setStyleSheet("color: #888;");

        auto *versionLabel = new QLabel(QString("Version %1").arg(APP_VERSION));
        versionLabel->setAlignment(Qt::AlignCenter);
        versionLabel->setStyleSheet("color: #555; font-size: 11px;");

        // Action buttons
        auto *buttonLayout = new QVBoxLayout();
        buttonLayout->setAlignment(Qt::AlignCenter);
        buttonLayout->setSpacing(10);

        auto *newBtn = new QPushButton("  New Project  ");
        newBtn->setFixedWidth(240);
        newBtn->setFixedHeight(40);
        newBtn->setStyleSheet(
            "QPushButton { background: #e94560; color: white; border: none; border-radius: 6px; "
            "font-size: 14px; font-weight: bold; }"
            "QPushButton:hover { background: #ff6b6b; }");
        connect(newBtn, &QPushButton::clicked, this, &WelcomeWidget::newProjectClicked);

        auto *openBtn = new QPushButton("  Open File  ");
        openBtn->setFixedWidth(240);
        openBtn->setFixedHeight(40);
        openBtn->setStyleSheet(
            "QPushButton { background: #0f3460; color: white; border: none; border-radius: 6px; "
            "font-size: 14px; }"
            "QPushButton:hover { background: #1a4a8a; }");
        connect(openBtn, &QPushButton::clicked, this, &WelcomeWidget::openFileClicked);

        auto *openProjectBtn = new QPushButton("  Open Project (.veditor)  ");
        openProjectBtn->setFixedWidth(240);
        openProjectBtn->setFixedHeight(40);
        openProjectBtn->setStyleSheet(
            "QPushButton { background: #533483; color: white; border: none; border-radius: 6px; "
            "font-size: 14px; }"
            "QPushButton:hover { background: #6a45a5; }");
        connect(openProjectBtn, &QPushButton::clicked, this, &WelcomeWidget::openProjectClicked);

        buttonLayout->addWidget(newBtn);
        buttonLayout->addWidget(openBtn);
        buttonLayout->addWidget(openProjectBtn);

        // Shortcuts hint
        auto *hintLabel = new QLabel(
            "Ctrl+N  New  |  Ctrl+O  Open  |  Ctrl+Shift+O  Open Project  |  F1  Resource Guide");
        hintLabel->setAlignment(Qt::AlignCenter);
        hintLabel->setStyleSheet("color: #555; font-size: 10px; margin-top: 20px;");

        // Recent files section
        m_recentLabel = new QLabel("");
        m_recentLabel->setAlignment(Qt::AlignCenter);
        m_recentLabel->setStyleSheet("color: #666; font-size: 11px;");
        m_recentLabel->setWordWrap(true);

        layout->addStretch(2);
        layout->addWidget(titleLabel);
        layout->addWidget(subtitleLabel);
        layout->addWidget(versionLabel);
        layout->addSpacing(30);
        layout->addLayout(buttonLayout);
        layout->addSpacing(10);
        layout->addWidget(m_recentLabel);
        layout->addSpacing(10);
        layout->addWidget(hintLabel);
        layout->addStretch(3);
    }

    void setRecentFiles(const QStringList &files)
    {
        if (files.isEmpty()) {
            m_recentLabel->setText("No recent files");
        } else {
            QStringList display;
            for (int i = 0; i < qMin(5, files.size()); ++i)
                display << QFileInfo(files[i]).fileName();
            m_recentLabel->setText("Recent: " + display.join("  |  "));
        }
    }

signals:
    void newProjectClicked();
    void openFileClicked();
    void openProjectClicked();

private:
    QLabel *m_recentLabel;
};
