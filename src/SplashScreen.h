#pragma once

#include <QSplashScreen>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QApplication>
#include <QScreen>

class AppSplashScreen : public QSplashScreen
{
    Q_OBJECT

public:
    explicit AppSplashScreen()
        : QSplashScreen()
    {
        setFixedSize(600, 360);
        setWindowFlags(Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);

        // Center on screen
        if (auto *screen = QApplication::primaryScreen()) {
            auto geo = screen->geometry();
            move((geo.width() - width()) / 2, (geo.height() - height()) / 2);
        }

        m_progress = 0;
        m_statusText = "Initializing...";
    }

    void setProgress(int value, const QString &status)
    {
        m_progress = qBound(0, value, 100);
        m_statusText = status;
        repaint();
        QApplication::processEvents();
    }

    void finishWithDelay(QWidget *mainWin, int delayMs = 500)
    {
        setProgress(100, "Ready");
        QTimer::singleShot(delayMs, this, [this, mainWin]() {
            finish(mainWin);
        });
    }

protected:
    void drawContents(QPainter *painter) override
    {
        painter->setRenderHint(QPainter::Antialiasing);

        // Background gradient
        QLinearGradient bg(0, 0, width(), height());
        bg.setColorAt(0.0, QColor(26, 26, 46));
        bg.setColorAt(1.0, QColor(22, 33, 62));
        painter->fillRect(rect(), bg);

        // Border
        painter->setPen(QPen(QColor(15, 52, 96), 2));
        painter->drawRect(rect().adjusted(1, 1, -1, -1));

        // Film strip decoration (left)
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(15, 52, 96, 100));
        painter->drawRect(0, 0, 30, height());
        for (int y = 10; y < height() - 10; y += 24) {
            painter->setBrush(QColor(26, 26, 46));
            painter->drawRoundedRect(6, y, 18, 14, 2, 2);
        }

        // Film strip (right)
        painter->setBrush(QColor(15, 52, 96, 100));
        painter->drawRect(width() - 30, 0, 30, height());
        for (int y = 10; y < height() - 10; y += 24) {
            painter->setBrush(QColor(26, 26, 46));
            painter->drawRoundedRect(width() - 24, y, 18, 14, 2, 2);
        }

        // Play triangle / V logo
        QLinearGradient accentGrad(200, 80, 340, 80);
        accentGrad.setColorAt(0.0, QColor(233, 69, 96));
        accentGrad.setColorAt(1.0, QColor(255, 107, 107));

        QPainterPath triangle;
        triangle.moveTo(240, 60);
        triangle.lineTo(340, 120);
        triangle.lineTo(240, 180);
        triangle.closeSubpath();
        painter->setBrush(accentGrad);
        painter->setPen(Qt::NoPen);
        painter->drawPath(triangle);

        // App name
        QFont titleFont("Segoe UI", 36, QFont::Bold);
        painter->setFont(titleFont);
        painter->setPen(QColor(255, 255, 255));
        painter->drawText(QRect(50, 190, width() - 100, 50), Qt::AlignCenter, "V Editor Simple");

        // Version
        QFont versionFont("Segoe UI", 12);
        painter->setFont(versionFont);
        painter->setPen(QColor(150, 150, 180));
        painter->drawText(QRect(50, 240, width() - 100, 25), Qt::AlignCenter,
            QString("Version %1 — C++ / Qt6 / FFmpeg / OpenGL").arg(APP_VERSION));

        // Progress bar background
        int barX = 60, barY = 290, barW = width() - 120, barH = 8;
        painter->setBrush(QColor(15, 52, 96));
        painter->setPen(Qt::NoPen);
        painter->drawRoundedRect(barX, barY, barW, barH, 4, 4);

        // Progress bar fill
        if (m_progress > 0) {
            int fillW = barW * m_progress / 100;
            QLinearGradient progressGrad(barX, 0, barX + barW, 0);
            progressGrad.setColorAt(0.0, QColor(233, 69, 96));
            progressGrad.setColorAt(1.0, QColor(83, 52, 131));
            painter->setBrush(progressGrad);
            painter->drawRoundedRect(barX, barY, fillW, barH, 4, 4);
        }

        // Status text
        QFont statusFont("Segoe UI", 10);
        painter->setFont(statusFont);
        painter->setPen(QColor(120, 120, 150));
        painter->drawText(QRect(barX, barY + 14, barW, 20), Qt::AlignLeft, m_statusText);

        // Copyright
        painter->drawText(QRect(barX, barY + 14, barW, 20), Qt::AlignRight, "MIT License");
    }

private:
    int m_progress;
    QString m_statusText;
};
