#include "ThemeManager.h"

ThemeManager &ThemeManager::instance()
{
    static ThemeManager mgr;
    return mgr;
}

void ThemeManager::applyTheme(ThemeType type, QWidget *root)
{
    m_current = type;
    QString qss;
    switch (type) {
    case ThemeType::Dark:     qss = darkTheme(); break;
    case ThemeType::Light:    qss = lightTheme(); break;
    case ThemeType::Midnight: qss = midnightTheme(); break;
    case ThemeType::Ocean:    qss = oceanTheme(); break;
    default: return;
    }
    root->setStyleSheet(qss);
}

void ThemeManager::applyCustomTheme(const QString &qss, QWidget *root)
{
    m_current = ThemeType::Custom;
    root->setStyleSheet(qss);
}

QVector<Theme> ThemeManager::availableThemes() const
{
    return {
        { ThemeType::Dark,     "Dark",     darkTheme() },
        { ThemeType::Light,    "Light",    lightTheme() },
        { ThemeType::Midnight, "Midnight", midnightTheme() },
        { ThemeType::Ocean,    "Ocean",    oceanTheme() },
    };
}

QString ThemeManager::themeName(ThemeType type)
{
    switch (type) {
    case ThemeType::Dark:     return "Dark";
    case ThemeType::Light:    return "Light";
    case ThemeType::Midnight: return "Midnight";
    case ThemeType::Ocean:    return "Ocean";
    case ThemeType::Custom:   return "Custom";
    }
    return "Unknown";
}

QString ThemeManager::darkTheme()
{
    // Color palette:
    //   Base bg:     #1a1a2e  (60% - deep navy-tinted dark)
    //   Surface 1:   #1e1e35  (panels)
    //   Surface 2:   #252540  (cards/raised)
    //   Surface 3:   #2d2d4a  (hover/active)
    //   Border:      #363655
    //   Text primary:   #e8e8f0
    //   Text secondary: #a0a0b8
    //   Text disabled:  #606078
    //   Accent:      #e94560  (10% - brand red)
    //   Accent hover:#ff6b6b
    //   Button:      #0f3460  (30% - deep blue)
    return R"(
        /* ── Base ── */
        QMainWindow, QWidget {
            background-color: #1a1a2e;
            color: #e8e8f0;
        }

        /* ── Menu bar ── */
        QMenuBar {
            background-color: #1e1e35;
            color: #e8e8f0;
            border-bottom: 1px solid #363655;
        }
        QMenuBar::item:selected {
            background-color: #2d2d4a;
            color: #e8e8f0;
        }
        QMenuBar::item:pressed {
            background-color: #e94560;
            color: #e8e8f0;
        }

        /* ── Menu ── */
        QMenu {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
        }
        QMenu::item {
            padding: 4px 24px 4px 12px;
        }
        QMenu::item:selected {
            background-color: rgba(233, 69, 96, 0.2);
            color: #e8e8f0;
        }
        QMenu::separator {
            height: 1px;
            background: #363655;
            margin: 3px 6px;
        }

        /* ── Toolbar ── */
        QToolBar {
            background-color: #1e1e35;
            border-bottom: 1px solid #363655;
            spacing: 4px;
            padding: 2px;
        }
        QToolButton {
            background-color: transparent;
            color: #e8e8f0;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QToolButton:hover {
            background-color: #2d2d4a;
            border-color: #363655;
        }
        QToolButton:pressed {
            background-color: #e94560;
            border-color: #e94560;
        }
        QToolButton:checked {
            background-color: rgba(233, 69, 96, 0.2);
            border-color: #e94560;
        }

        /* ── Push button ── */
        QPushButton {
            background-color: #0f3460;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 5px 14px;
            border-radius: 4px;
            min-height: 22px;
        }
        QPushButton:hover {
            background-color: #1a4a8a;
            border-color: #e94560;
        }
        QPushButton:pressed {
            background-color: #c93850;
            border-color: #c93850;
        }
        QPushButton:disabled {
            background-color: #252540;
            color: #606078;
            border-color: #363655;
        }
        QPushButton:focus {
            border-color: #e94560;
            outline: none;
        }

        /* ── Splitter ── */
        QSplitter::handle {
            background-color: #363655;
        }
        QSplitter::handle:hover {
            background-color: #e94560;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }

        /* ── Tables / Trees ── */
        QTableWidget, QTableView, QTreeWidget, QTreeView, QListWidget, QListView {
            background-color: #1e1e35;
            color: #e8e8f0;
            border: 1px solid #363655;
            gridline-color: #363655;
            selection-background-color: rgba(233, 69, 96, 0.2);
            selection-color: #e8e8f0;
            alternate-background-color: #252540;
        }
        QTableWidget::item:selected, QTableView::item:selected,
        QTreeWidget::item:selected, QTreeView::item:selected,
        QListWidget::item:selected, QListView::item:selected {
            background-color: rgba(233, 69, 96, 0.25);
            color: #e8e8f0;
        }
        QTableWidget::item:hover, QTableView::item:hover,
        QTreeWidget::item:hover, QTreeView::item:hover,
        QListWidget::item:hover, QListView::item:hover {
            background-color: #2d2d4a;
        }
        QHeaderView::section {
            background-color: #252540;
            color: #a0a0b8;
            border: none;
            border-right: 1px solid #363655;
            border-bottom: 1px solid #363655;
            padding: 4px 8px;
        }
        QHeaderView::section:hover {
            background-color: #2d2d4a;
            color: #e8e8f0;
        }

        /* ── Scrollbars ── */
        QScrollBar:vertical {
            background: #1a1a2e;
            width: 14px;
            margin: 0;
            border-left: 1px solid #2a2a3e;
        }
        QScrollBar::handle:vertical {
            background: #5a5a8c;
            min-height: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #e94560;
        }
        QScrollBar::handle:vertical:pressed {
            background: #c93550;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: #1a1a2e;
            height: 14px;
            margin: 0;
            border-top: 1px solid #2a2a3e;
        }
        QScrollBar::handle:horizontal {
            background: #5a5a8c;
            min-width: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #e94560;
        }
        QScrollBar::handle:horizontal:pressed {
            background: #c93550;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }

        /* ── Input fields ── */
        QComboBox {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QComboBox:hover {
            border-color: #a0a0b8;
        }
        QComboBox:focus {
            border-color: #e94560;
        }
        QComboBox::drop-down {
            border: none;
            padding-right: 6px;
        }
        QComboBox QAbstractItemView {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
            selection-background-color: rgba(233, 69, 96, 0.25);
        }
        QLineEdit {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QLineEdit:hover {
            border-color: #a0a0b8;
        }
        QLineEdit:focus {
            border-color: #e94560;
            outline: none;
        }
        QLineEdit:disabled {
            background-color: #1e1e35;
            color: #606078;
        }
        QSpinBox, QDoubleSpinBox {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 4px 6px;
            border-radius: 4px;
            min-height: 22px;
        }
        QSpinBox:hover, QDoubleSpinBox:hover {
            border-color: #a0a0b8;
        }
        QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #e94560;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background-color: #2d2d4a;
            border: none;
            width: 16px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background-color: #363655;
        }

        /* ── Text editors ── */
        QTextEdit, QPlainTextEdit {
            background-color: #1e1e35;
            color: #e8e8f0;
            border: 1px solid #363655;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: rgba(233, 69, 96, 0.3);
        }
        QTextEdit:focus, QPlainTextEdit:focus {
            border-color: #e94560;
        }

        /* ── Slider ── */
        QSlider::groove:horizontal {
            background: #363655;
            height: 6px;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #e94560;
            width: 14px;
            height: 14px;
            margin: -4px 0;
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover {
            background: #ff6b6b;
        }
        QSlider::sub-page:horizontal {
            background: #e94560;
            border-radius: 3px;
        }
        QSlider::groove:vertical {
            background: #363655;
            width: 6px;
            border-radius: 3px;
        }
        QSlider::handle:vertical {
            background: #e94560;
            width: 14px;
            height: 14px;
            margin: 0 -4px;
            border-radius: 7px;
        }
        QSlider::sub-page:vertical {
            background: #e94560;
            border-radius: 3px;
        }

        /* ── Progress bar ── */
        QProgressBar {
            background-color: #252540;
            border: 1px solid #363655;
            border-radius: 4px;
            text-align: center;
            color: #e8e8f0;
            min-height: 10px;
        }
        QProgressBar::chunk {
            background-color: #e94560;
            border-radius: 3px;
        }

        /* ── Tabs ── */
        QTabWidget::pane {
            background-color: #1e1e35;
            border: 1px solid #363655;
            border-radius: 0 4px 4px 4px;
        }
        QTabBar::tab {
            background-color: #252540;
            color: #a0a0b8;
            border: 1px solid #363655;
            border-bottom: none;
            padding: 5px 14px;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:hover {
            background-color: #2d2d4a;
            color: #e8e8f0;
        }
        QTabBar::tab:selected {
            background-color: #1e1e35;
            color: #e8e8f0;
            border-bottom-color: #1e1e35;
        }

        /* ── Group box ── */
        QGroupBox {
            border: 1px solid #363655;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 10px;
            color: #a0a0b8;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            color: #a0a0b8;
        }

        /* ── Status bar ── */
        QStatusBar {
            background-color: #1e1e35;
            color: #a0a0b8;
            border-top: 1px solid #363655;
        }
        QStatusBar::item {
            border: none;
        }

        /* ── Labels ── */
        QLabel {
            color: #e8e8f0;
        }
        QLabel:disabled {
            color: #606078;
        }

        /* ── Dialogs ── */
        QDialog {
            background-color: #1a1a2e;
            color: #e8e8f0;
        }

        /* ── Dock widgets ── */
        QDockWidget {
            color: #e8e8f0;
            titlebar-close-icon: none;
        }
        QDockWidget::title {
            background-color: #252540;
            color: #e8e8f0;
            padding: 4px 8px;
            border-bottom: 1px solid #363655;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background-color: transparent;
            border: none;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background-color: #e94560;
            border-radius: 3px;
        }

        /* ── Tooltip ── */
        QToolTip {
            background-color: #252540;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 4px 8px;
            border-radius: 4px;
        }

        /* ── Check / Radio ── */
        QCheckBox {
            color: #e8e8f0;
            spacing: 6px;
        }
        QCheckBox:disabled {
            color: #606078;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #363655;
            border-radius: 3px;
            background-color: #252540;
        }
        QCheckBox::indicator:checked {
            background-color: #e94560;
            border-color: #e94560;
        }
        QCheckBox::indicator:hover {
            border-color: #e94560;
        }
        QRadioButton {
            color: #e8e8f0;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #363655;
            border-radius: 7px;
            background-color: #252540;
        }
        QRadioButton::indicator:checked {
            background-color: #e94560;
            border-color: #e94560;
        }

        /* ── Scroll area ── */
        QScrollArea {
            background-color: #1a1a2e;
            border: none;
        }
    )";
}

QString ThemeManager::lightTheme()
{
    // Color palette (warm whites, 4.5:1+ contrast on body text):
    //   Base bg:     #f8f8fc  (warm off-white, slight blue-violet tint)
    //   Surface 1:   #ffffff  (panels)
    //   Surface 2:   #f0f0f8  (cards)
    //   Border:      #d0d0e0
    //   Text primary:   #1a1a2e  (dark navy, high contrast)
    //   Text secondary: #4a4a6a
    //   Accent:      #e94560
    return R"(
        /* ── Base ── */
        QMainWindow, QWidget {
            background-color: #f8f8fc;
            color: #1a1a2e;
        }

        /* ── Menu bar ── */
        QMenuBar {
            background-color: #ffffff;
            color: #1a1a2e;
            border-bottom: 1px solid #d0d0e0;
        }
        QMenuBar::item:selected {
            background-color: #f0f0f8;
            color: #1a1a2e;
        }
        QMenuBar::item:pressed {
            background-color: #e94560;
            color: #ffffff;
        }

        /* ── Menu ── */
        QMenu {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
        }
        QMenu::item {
            padding: 4px 24px 4px 12px;
        }
        QMenu::item:selected {
            background-color: rgba(233, 69, 96, 0.12);
            color: #1a1a2e;
        }
        QMenu::separator {
            height: 1px;
            background: #d0d0e0;
            margin: 3px 6px;
        }

        /* ── Toolbar ── */
        QToolBar {
            background-color: #ffffff;
            border-bottom: 1px solid #d0d0e0;
            spacing: 4px;
            padding: 2px;
        }
        QToolButton {
            background-color: transparent;
            color: #1a1a2e;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QToolButton:hover {
            background-color: #f0f0f8;
            border-color: #d0d0e0;
        }
        QToolButton:pressed {
            background-color: #e94560;
            color: #ffffff;
            border-color: #e94560;
        }
        QToolButton:checked {
            background-color: rgba(233, 69, 96, 0.12);
            border-color: #e94560;
        }

        /* ── Push button ── */
        QPushButton {
            background-color: #f0f0f8;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            padding: 5px 14px;
            border-radius: 4px;
            min-height: 22px;
        }
        QPushButton:hover {
            background-color: #e4e4f0;
            border-color: #b0b0cc;
        }
        QPushButton:pressed {
            background-color: #e94560;
            color: #ffffff;
            border-color: #e94560;
        }
        QPushButton:disabled {
            background-color: #f0f0f8;
            color: #a0a0b8;
            border-color: #d0d0e0;
        }
        QPushButton:focus {
            border-color: #e94560;
            outline: none;
        }

        /* ── Splitter ── */
        QSplitter::handle {
            background-color: #d0d0e0;
        }
        QSplitter::handle:hover {
            background-color: #e94560;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }

        /* ── Tables / Trees ── */
        QTableWidget, QTableView, QTreeWidget, QTreeView, QListWidget, QListView {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            gridline-color: #e8e8f4;
            selection-background-color: rgba(233, 69, 96, 0.12);
            selection-color: #1a1a2e;
            alternate-background-color: #f8f8fc;
        }
        QTableWidget::item:selected, QTableView::item:selected,
        QTreeWidget::item:selected, QTreeView::item:selected,
        QListWidget::item:selected, QListView::item:selected {
            background-color: rgba(233, 69, 96, 0.15);
            color: #1a1a2e;
        }
        QTableWidget::item:hover, QTableView::item:hover,
        QTreeWidget::item:hover, QTreeView::item:hover,
        QListWidget::item:hover, QListView::item:hover {
            background-color: #f0f0f8;
        }
        QHeaderView::section {
            background-color: #f0f0f8;
            color: #4a4a6a;
            border: none;
            border-right: 1px solid #d0d0e0;
            border-bottom: 1px solid #d0d0e0;
            padding: 4px 8px;
        }
        QHeaderView::section:hover {
            background-color: #e4e4f0;
            color: #1a1a2e;
        }

        /* ── Scrollbars ── */
        QScrollBar:vertical {
            background: #ececf5;
            width: 14px;
            margin: 0;
            border-left: 1px solid #d0d0e0;
        }
        QScrollBar::handle:vertical {
            background: #8888a8;
            min-height: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #e94560;
        }
        QScrollBar::handle:vertical:pressed {
            background: #c93550;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: #ececf5;
            height: 14px;
            margin: 0;
            border-top: 1px solid #d0d0e0;
        }
        QScrollBar::handle:horizontal {
            background: #8888a8;
            min-width: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #e94560;
        }
        QScrollBar::handle:horizontal:pressed {
            background: #c93550;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }

        /* ── Input fields ── */
        QComboBox {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QComboBox:hover {
            border-color: #a0a0c0;
        }
        QComboBox:focus {
            border-color: #e94560;
        }
        QComboBox::drop-down {
            border: none;
            padding-right: 6px;
        }
        QComboBox QAbstractItemView {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            selection-background-color: rgba(233, 69, 96, 0.15);
        }
        QLineEdit {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QLineEdit:hover {
            border-color: #a0a0c0;
        }
        QLineEdit:focus {
            border-color: #e94560;
            outline: none;
        }
        QLineEdit:disabled {
            background-color: #f0f0f8;
            color: #a0a0b8;
        }
        QSpinBox, QDoubleSpinBox {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            padding: 4px 6px;
            border-radius: 4px;
            min-height: 22px;
        }
        QSpinBox:hover, QDoubleSpinBox:hover {
            border-color: #a0a0c0;
        }
        QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #e94560;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background-color: #f0f0f8;
            border: none;
            width: 16px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background-color: #d0d0e0;
        }

        /* ── Text editors ── */
        QTextEdit, QPlainTextEdit {
            background-color: #ffffff;
            color: #1a1a2e;
            border: 1px solid #d0d0e0;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: rgba(233, 69, 96, 0.2);
        }
        QTextEdit:focus, QPlainTextEdit:focus {
            border-color: #e94560;
        }

        /* ── Slider ── */
        QSlider::groove:horizontal {
            background: #d0d0e0;
            height: 6px;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #e94560;
            width: 14px;
            height: 14px;
            margin: -4px 0;
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover {
            background: #c93850;
        }
        QSlider::sub-page:horizontal {
            background: #e94560;
            border-radius: 3px;
        }
        QSlider::groove:vertical {
            background: #d0d0e0;
            width: 6px;
            border-radius: 3px;
        }
        QSlider::handle:vertical {
            background: #e94560;
            width: 14px;
            height: 14px;
            margin: 0 -4px;
            border-radius: 7px;
        }
        QSlider::sub-page:vertical {
            background: #e94560;
            border-radius: 3px;
        }

        /* ── Progress bar ── */
        QProgressBar {
            background-color: #f0f0f8;
            border: 1px solid #d0d0e0;
            border-radius: 4px;
            text-align: center;
            color: #1a1a2e;
            min-height: 10px;
        }
        QProgressBar::chunk {
            background-color: #e94560;
            border-radius: 3px;
        }

        /* ── Tabs ── */
        QTabWidget::pane {
            background-color: #ffffff;
            border: 1px solid #d0d0e0;
            border-radius: 0 4px 4px 4px;
        }
        QTabBar::tab {
            background-color: #f0f0f8;
            color: #4a4a6a;
            border: 1px solid #d0d0e0;
            border-bottom: none;
            padding: 5px 14px;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:hover {
            background-color: #e4e4f0;
            color: #1a1a2e;
        }
        QTabBar::tab:selected {
            background-color: #ffffff;
            color: #1a1a2e;
            border-bottom-color: #ffffff;
        }

        /* ── Group box ── */
        QGroupBox {
            border: 1px solid #d0d0e0;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 10px;
            color: #4a4a6a;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            color: #4a4a6a;
        }

        /* ── Status bar ── */
        QStatusBar {
            background-color: #ffffff;
            color: #4a4a6a;
            border-top: 1px solid #d0d0e0;
        }
        QStatusBar::item {
            border: none;
        }

        /* ── Labels ── */
        QLabel {
            color: #1a1a2e;
        }
        QLabel:disabled {
            color: #a0a0b8;
        }

        /* ── Dialogs ── */
        QDialog {
            background-color: #f8f8fc;
            color: #1a1a2e;
        }

        /* ── Dock widgets ── */
        QDockWidget {
            color: #1a1a2e;
        }
        QDockWidget::title {
            background-color: #f0f0f8;
            color: #1a1a2e;
            padding: 4px 8px;
            border-bottom: 1px solid #d0d0e0;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background-color: transparent;
            border: none;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background-color: rgba(233, 69, 96, 0.15);
            border-radius: 3px;
        }

        /* ── Tooltip ── */
        QToolTip {
            background-color: #1a1a2e;
            color: #e8e8f0;
            border: 1px solid #363655;
            padding: 4px 8px;
            border-radius: 4px;
        }

        /* ── Check / Radio ── */
        QCheckBox {
            color: #1a1a2e;
            spacing: 6px;
        }
        QCheckBox:disabled {
            color: #a0a0b8;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #d0d0e0;
            border-radius: 3px;
            background-color: #ffffff;
        }
        QCheckBox::indicator:checked {
            background-color: #e94560;
            border-color: #e94560;
        }
        QCheckBox::indicator:hover {
            border-color: #e94560;
        }
        QRadioButton {
            color: #1a1a2e;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #d0d0e0;
            border-radius: 7px;
            background-color: #ffffff;
        }
        QRadioButton::indicator:checked {
            background-color: #e94560;
            border-color: #e94560;
        }

        /* ── Scroll area ── */
        QScrollArea {
            background-color: #f8f8fc;
            border: none;
        }
    )";
}

QString ThemeManager::midnightTheme()
{
    // Color palette (indigo accent, cooler surfaces):
    //   Base bg:     #0d0d1a  (deep cool dark)
    //   Surface 1:   #111128  (panels)
    //   Surface 2:   #181830  (cards/raised)
    //   Surface 3:   #20203c  (hover/active)
    //   Border:      #2a2a50
    //   Text primary:   #e0e0f8
    //   Text secondary: #8888c0
    //   Text disabled:  #484870
    //   Accent:      #6366f1  (indigo)
    //   Accent hover:#818cf8
    //   Button:      #1e1e4a
    return R"(
        /* ── Base ── */
        QMainWindow, QWidget {
            background-color: #0d0d1a;
            color: #e0e0f8;
        }

        /* ── Menu bar ── */
        QMenuBar {
            background-color: #111128;
            color: #e0e0f8;
            border-bottom: 1px solid #2a2a50;
        }
        QMenuBar::item:selected {
            background-color: #20203c;
            color: #e0e0f8;
        }
        QMenuBar::item:pressed {
            background-color: #6366f1;
            color: #ffffff;
        }

        /* ── Menu ── */
        QMenu {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
        }
        QMenu::item {
            padding: 4px 24px 4px 12px;
        }
        QMenu::item:selected {
            background-color: rgba(99, 102, 241, 0.25);
            color: #e0e0f8;
        }
        QMenu::separator {
            height: 1px;
            background: #2a2a50;
            margin: 3px 6px;
        }

        /* ── Toolbar ── */
        QToolBar {
            background-color: #111128;
            border-bottom: 1px solid #2a2a50;
            spacing: 4px;
            padding: 2px;
        }
        QToolButton {
            background-color: transparent;
            color: #e0e0f8;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QToolButton:hover {
            background-color: #20203c;
            border-color: #2a2a50;
        }
        QToolButton:pressed {
            background-color: #6366f1;
            border-color: #6366f1;
        }
        QToolButton:checked {
            background-color: rgba(99, 102, 241, 0.2);
            border-color: #6366f1;
        }

        /* ── Push button ── */
        QPushButton {
            background-color: #1e1e4a;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            padding: 5px 14px;
            border-radius: 4px;
            min-height: 22px;
        }
        QPushButton:hover {
            background-color: #28286a;
            border-color: #6366f1;
        }
        QPushButton:pressed {
            background-color: #4f52c8;
            border-color: #4f52c8;
        }
        QPushButton:disabled {
            background-color: #181830;
            color: #484870;
            border-color: #2a2a50;
        }
        QPushButton:focus {
            border-color: #6366f1;
            outline: none;
        }

        /* ── Splitter ── */
        QSplitter::handle {
            background-color: #2a2a50;
        }
        QSplitter::handle:hover {
            background-color: #6366f1;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }

        /* ── Tables / Trees ── */
        QTableWidget, QTableView, QTreeWidget, QTreeView, QListWidget, QListView {
            background-color: #111128;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            gridline-color: #2a2a50;
            selection-background-color: rgba(99, 102, 241, 0.2);
            selection-color: #e0e0f8;
            alternate-background-color: #181830;
        }
        QTableWidget::item:selected, QTableView::item:selected,
        QTreeWidget::item:selected, QTreeView::item:selected,
        QListWidget::item:selected, QListView::item:selected {
            background-color: rgba(99, 102, 241, 0.25);
            color: #e0e0f8;
        }
        QTableWidget::item:hover, QTableView::item:hover,
        QTreeWidget::item:hover, QTreeView::item:hover,
        QListWidget::item:hover, QListView::item:hover {
            background-color: #20203c;
        }
        QHeaderView::section {
            background-color: #181830;
            color: #8888c0;
            border: none;
            border-right: 1px solid #2a2a50;
            border-bottom: 1px solid #2a2a50;
            padding: 4px 8px;
        }
        QHeaderView::section:hover {
            background-color: #20203c;
            color: #e0e0f8;
        }

        /* ── Scrollbars ── */
        QScrollBar:vertical {
            background: #0d0d1a;
            width: 14px;
            margin: 0;
            border-left: 1px solid #1a1a30;
        }
        QScrollBar::handle:vertical {
            background: #4a4a80;
            min-height: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #6366f1;
        }
        QScrollBar::handle:vertical:pressed {
            background: #4f52d8;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: #0d0d1a;
            height: 14px;
            margin: 0;
            border-top: 1px solid #1a1a30;
        }
        QScrollBar::handle:horizontal {
            background: #4a4a80;
            min-width: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #6366f1;
        }
        QScrollBar::handle:horizontal:pressed {
            background: #4f52d8;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }

        /* ── Input fields ── */
        QComboBox {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QComboBox:hover {
            border-color: #8888c0;
        }
        QComboBox:focus {
            border-color: #6366f1;
        }
        QComboBox::drop-down {
            border: none;
            padding-right: 6px;
        }
        QComboBox QAbstractItemView {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            selection-background-color: rgba(99, 102, 241, 0.25);
        }
        QLineEdit {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QLineEdit:hover {
            border-color: #8888c0;
        }
        QLineEdit:focus {
            border-color: #6366f1;
            outline: none;
        }
        QLineEdit:disabled {
            background-color: #111128;
            color: #484870;
        }
        QSpinBox, QDoubleSpinBox {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            padding: 4px 6px;
            border-radius: 4px;
            min-height: 22px;
        }
        QSpinBox:hover, QDoubleSpinBox:hover {
            border-color: #8888c0;
        }
        QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #6366f1;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background-color: #20203c;
            border: none;
            width: 16px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background-color: #2a2a50;
        }

        /* ── Text editors ── */
        QTextEdit, QPlainTextEdit {
            background-color: #111128;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: rgba(99, 102, 241, 0.3);
        }
        QTextEdit:focus, QPlainTextEdit:focus {
            border-color: #6366f1;
        }

        /* ── Slider ── */
        QSlider::groove:horizontal {
            background: #2a2a50;
            height: 6px;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #6366f1;
            width: 14px;
            height: 14px;
            margin: -4px 0;
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover {
            background: #818cf8;
        }
        QSlider::sub-page:horizontal {
            background: #6366f1;
            border-radius: 3px;
        }
        QSlider::groove:vertical {
            background: #2a2a50;
            width: 6px;
            border-radius: 3px;
        }
        QSlider::handle:vertical {
            background: #6366f1;
            width: 14px;
            height: 14px;
            margin: 0 -4px;
            border-radius: 7px;
        }
        QSlider::sub-page:vertical {
            background: #6366f1;
            border-radius: 3px;
        }

        /* ── Progress bar ── */
        QProgressBar {
            background-color: #181830;
            border: 1px solid #2a2a50;
            border-radius: 4px;
            text-align: center;
            color: #e0e0f8;
            min-height: 10px;
        }
        QProgressBar::chunk {
            background-color: #6366f1;
            border-radius: 3px;
        }

        /* ── Tabs ── */
        QTabWidget::pane {
            background-color: #111128;
            border: 1px solid #2a2a50;
            border-radius: 0 4px 4px 4px;
        }
        QTabBar::tab {
            background-color: #181830;
            color: #8888c0;
            border: 1px solid #2a2a50;
            border-bottom: none;
            padding: 5px 14px;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:hover {
            background-color: #20203c;
            color: #e0e0f8;
        }
        QTabBar::tab:selected {
            background-color: #111128;
            color: #e0e0f8;
            border-bottom-color: #111128;
        }

        /* ── Group box ── */
        QGroupBox {
            border: 1px solid #2a2a50;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 10px;
            color: #8888c0;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            color: #8888c0;
        }

        /* ── Status bar ── */
        QStatusBar {
            background-color: #111128;
            color: #8888c0;
            border-top: 1px solid #2a2a50;
        }
        QStatusBar::item {
            border: none;
        }

        /* ── Labels ── */
        QLabel {
            color: #e0e0f8;
        }
        QLabel:disabled {
            color: #484870;
        }

        /* ── Dialogs ── */
        QDialog {
            background-color: #0d0d1a;
            color: #e0e0f8;
        }

        /* ── Dock widgets ── */
        QDockWidget {
            color: #e0e0f8;
        }
        QDockWidget::title {
            background-color: #181830;
            color: #e0e0f8;
            padding: 4px 8px;
            border-bottom: 1px solid #2a2a50;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background-color: transparent;
            border: none;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background-color: #6366f1;
            border-radius: 3px;
        }

        /* ── Tooltip ── */
        QToolTip {
            background-color: #181830;
            color: #e0e0f8;
            border: 1px solid #2a2a50;
            padding: 4px 8px;
            border-radius: 4px;
        }

        /* ── Check / Radio ── */
        QCheckBox {
            color: #e0e0f8;
            spacing: 6px;
        }
        QCheckBox:disabled {
            color: #484870;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #2a2a50;
            border-radius: 3px;
            background-color: #181830;
        }
        QCheckBox::indicator:checked {
            background-color: #6366f1;
            border-color: #6366f1;
        }
        QCheckBox::indicator:hover {
            border-color: #6366f1;
        }
        QRadioButton {
            color: #e0e0f8;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #2a2a50;
            border-radius: 7px;
            background-color: #181830;
        }
        QRadioButton::indicator:checked {
            background-color: #6366f1;
            border-color: #6366f1;
        }

        /* ── Scroll area ── */
        QScrollArea {
            background-color: #0d0d1a;
            border: none;
        }
    )";
}

QString ThemeManager::oceanTheme()
{
    // Color palette (cyan accent, blue-green tinted surfaces):
    //   Base bg:     #0d1a1f  (deep ocean dark)
    //   Surface 1:   #112028  (panels)
    //   Surface 2:   #162a35  (cards/raised)
    //   Surface 3:   #1e3540  (hover/active)
    //   Border:      #264554
    //   Text primary:   #d0eaf5
    //   Text secondary: #6ab0cc
    //   Text disabled:  #3a6070
    //   Accent:      #06b6d4  (cyan)
    //   Accent hover:#22d3ee
    //   Button:      #0e3a50
    return R"(
        /* ── Base ── */
        QMainWindow, QWidget {
            background-color: #0d1a1f;
            color: #d0eaf5;
        }

        /* ── Menu bar ── */
        QMenuBar {
            background-color: #112028;
            color: #d0eaf5;
            border-bottom: 1px solid #264554;
        }
        QMenuBar::item:selected {
            background-color: #1e3540;
            color: #d0eaf5;
        }
        QMenuBar::item:pressed {
            background-color: #06b6d4;
            color: #0d1a1f;
        }

        /* ── Menu ── */
        QMenu {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
        }
        QMenu::item {
            padding: 4px 24px 4px 12px;
        }
        QMenu::item:selected {
            background-color: rgba(6, 182, 212, 0.22);
            color: #d0eaf5;
        }
        QMenu::separator {
            height: 1px;
            background: #264554;
            margin: 3px 6px;
        }

        /* ── Toolbar ── */
        QToolBar {
            background-color: #112028;
            border-bottom: 1px solid #264554;
            spacing: 4px;
            padding: 2px;
        }
        QToolButton {
            background-color: transparent;
            color: #d0eaf5;
            border: 1px solid transparent;
            border-radius: 4px;
            padding: 3px 6px;
        }
        QToolButton:hover {
            background-color: #1e3540;
            border-color: #264554;
        }
        QToolButton:pressed {
            background-color: #06b6d4;
            color: #0d1a1f;
            border-color: #06b6d4;
        }
        QToolButton:checked {
            background-color: rgba(6, 182, 212, 0.2);
            border-color: #06b6d4;
        }

        /* ── Push button ── */
        QPushButton {
            background-color: #0e3a50;
            color: #d0eaf5;
            border: 1px solid #264554;
            padding: 5px 14px;
            border-radius: 4px;
            min-height: 22px;
        }
        QPushButton:hover {
            background-color: #155070;
            border-color: #06b6d4;
        }
        QPushButton:pressed {
            background-color: #0592a8;
            border-color: #0592a8;
        }
        QPushButton:disabled {
            background-color: #162a35;
            color: #3a6070;
            border-color: #264554;
        }
        QPushButton:focus {
            border-color: #06b6d4;
            outline: none;
        }

        /* ── Splitter ── */
        QSplitter::handle {
            background-color: #264554;
        }
        QSplitter::handle:hover {
            background-color: #06b6d4;
        }
        QSplitter::handle:horizontal {
            width: 3px;
        }
        QSplitter::handle:vertical {
            height: 3px;
        }

        /* ── Tables / Trees ── */
        QTableWidget, QTableView, QTreeWidget, QTreeView, QListWidget, QListView {
            background-color: #112028;
            color: #d0eaf5;
            border: 1px solid #264554;
            gridline-color: #264554;
            selection-background-color: rgba(6, 182, 212, 0.2);
            selection-color: #d0eaf5;
            alternate-background-color: #162a35;
        }
        QTableWidget::item:selected, QTableView::item:selected,
        QTreeWidget::item:selected, QTreeView::item:selected,
        QListWidget::item:selected, QListView::item:selected {
            background-color: rgba(6, 182, 212, 0.25);
            color: #d0eaf5;
        }
        QTableWidget::item:hover, QTableView::item:hover,
        QTreeWidget::item:hover, QTreeView::item:hover,
        QListWidget::item:hover, QListView::item:hover {
            background-color: #1e3540;
        }
        QHeaderView::section {
            background-color: #162a35;
            color: #6ab0cc;
            border: none;
            border-right: 1px solid #264554;
            border-bottom: 1px solid #264554;
            padding: 4px 8px;
        }
        QHeaderView::section:hover {
            background-color: #1e3540;
            color: #d0eaf5;
        }

        /* ── Scrollbars ── */
        QScrollBar:vertical {
            background: #0d1a1f;
            width: 14px;
            margin: 0;
            border-left: 1px solid #1a2a32;
        }
        QScrollBar::handle:vertical {
            background: #3d6e84;
            min-height: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:vertical:hover {
            background: #06b6d4;
        }
        QScrollBar::handle:vertical:pressed {
            background: #0891a8;
        }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
            height: 0;
        }
        QScrollBar:horizontal {
            background: #0d1a1f;
            height: 14px;
            margin: 0;
            border-top: 1px solid #1a2a32;
        }
        QScrollBar::handle:horizontal {
            background: #3d6e84;
            min-width: 32px;
            border-radius: 6px;
            margin: 2px;
        }
        QScrollBar::handle:horizontal:hover {
            background: #06b6d4;
        }
        QScrollBar::handle:horizontal:pressed {
            background: #0891a8;
        }
        QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal {
            width: 0;
        }

        /* ── Input fields ── */
        QComboBox {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QComboBox:hover {
            border-color: #6ab0cc;
        }
        QComboBox:focus {
            border-color: #06b6d4;
        }
        QComboBox::drop-down {
            border: none;
            padding-right: 6px;
        }
        QComboBox QAbstractItemView {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
            selection-background-color: rgba(6, 182, 212, 0.25);
        }
        QLineEdit {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
            padding: 4px 8px;
            border-radius: 4px;
            min-height: 22px;
        }
        QLineEdit:hover {
            border-color: #6ab0cc;
        }
        QLineEdit:focus {
            border-color: #06b6d4;
            outline: none;
        }
        QLineEdit:disabled {
            background-color: #112028;
            color: #3a6070;
        }
        QSpinBox, QDoubleSpinBox {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
            padding: 4px 6px;
            border-radius: 4px;
            min-height: 22px;
        }
        QSpinBox:hover, QDoubleSpinBox:hover {
            border-color: #6ab0cc;
        }
        QSpinBox:focus, QDoubleSpinBox:focus {
            border-color: #06b6d4;
        }
        QSpinBox::up-button, QDoubleSpinBox::up-button,
        QSpinBox::down-button, QDoubleSpinBox::down-button {
            background-color: #1e3540;
            border: none;
            width: 16px;
        }
        QSpinBox::up-button:hover, QDoubleSpinBox::up-button:hover,
        QSpinBox::down-button:hover, QDoubleSpinBox::down-button:hover {
            background-color: #264554;
        }

        /* ── Text editors ── */
        QTextEdit, QPlainTextEdit {
            background-color: #112028;
            color: #d0eaf5;
            border: 1px solid #264554;
            border-radius: 4px;
            padding: 4px;
            selection-background-color: rgba(6, 182, 212, 0.3);
        }
        QTextEdit:focus, QPlainTextEdit:focus {
            border-color: #06b6d4;
        }

        /* ── Slider ── */
        QSlider::groove:horizontal {
            background: #264554;
            height: 6px;
            border-radius: 3px;
        }
        QSlider::handle:horizontal {
            background: #06b6d4;
            width: 14px;
            height: 14px;
            margin: -4px 0;
            border-radius: 7px;
        }
        QSlider::handle:horizontal:hover {
            background: #22d3ee;
        }
        QSlider::sub-page:horizontal {
            background: #06b6d4;
            border-radius: 3px;
        }
        QSlider::groove:vertical {
            background: #264554;
            width: 6px;
            border-radius: 3px;
        }
        QSlider::handle:vertical {
            background: #06b6d4;
            width: 14px;
            height: 14px;
            margin: 0 -4px;
            border-radius: 7px;
        }
        QSlider::sub-page:vertical {
            background: #06b6d4;
            border-radius: 3px;
        }

        /* ── Progress bar ── */
        QProgressBar {
            background-color: #162a35;
            border: 1px solid #264554;
            border-radius: 4px;
            text-align: center;
            color: #d0eaf5;
            min-height: 10px;
        }
        QProgressBar::chunk {
            background-color: #06b6d4;
            border-radius: 3px;
        }

        /* ── Tabs ── */
        QTabWidget::pane {
            background-color: #112028;
            border: 1px solid #264554;
            border-radius: 0 4px 4px 4px;
        }
        QTabBar::tab {
            background-color: #162a35;
            color: #6ab0cc;
            border: 1px solid #264554;
            border-bottom: none;
            padding: 5px 14px;
            border-top-left-radius: 4px;
            border-top-right-radius: 4px;
            margin-right: 2px;
        }
        QTabBar::tab:hover {
            background-color: #1e3540;
            color: #d0eaf5;
        }
        QTabBar::tab:selected {
            background-color: #112028;
            color: #d0eaf5;
            border-bottom-color: #112028;
        }

        /* ── Group box ── */
        QGroupBox {
            border: 1px solid #264554;
            border-radius: 4px;
            margin-top: 10px;
            padding-top: 10px;
            color: #6ab0cc;
        }
        QGroupBox::title {
            subcontrol-origin: margin;
            left: 10px;
            color: #6ab0cc;
        }

        /* ── Status bar ── */
        QStatusBar {
            background-color: #112028;
            color: #6ab0cc;
            border-top: 1px solid #264554;
        }
        QStatusBar::item {
            border: none;
        }

        /* ── Labels ── */
        QLabel {
            color: #d0eaf5;
        }
        QLabel:disabled {
            color: #3a6070;
        }

        /* ── Dialogs ── */
        QDialog {
            background-color: #0d1a1f;
            color: #d0eaf5;
        }

        /* ── Dock widgets ── */
        QDockWidget {
            color: #d0eaf5;
        }
        QDockWidget::title {
            background-color: #162a35;
            color: #d0eaf5;
            padding: 4px 8px;
            border-bottom: 1px solid #264554;
        }
        QDockWidget::close-button, QDockWidget::float-button {
            background-color: transparent;
            border: none;
        }
        QDockWidget::close-button:hover, QDockWidget::float-button:hover {
            background-color: #06b6d4;
            border-radius: 3px;
        }

        /* ── Tooltip ── */
        QToolTip {
            background-color: #162a35;
            color: #d0eaf5;
            border: 1px solid #264554;
            padding: 4px 8px;
            border-radius: 4px;
        }

        /* ── Check / Radio ── */
        QCheckBox {
            color: #d0eaf5;
            spacing: 6px;
        }
        QCheckBox:disabled {
            color: #3a6070;
        }
        QCheckBox::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #264554;
            border-radius: 3px;
            background-color: #162a35;
        }
        QCheckBox::indicator:checked {
            background-color: #06b6d4;
            border-color: #06b6d4;
        }
        QCheckBox::indicator:hover {
            border-color: #06b6d4;
        }
        QRadioButton {
            color: #d0eaf5;
            spacing: 6px;
        }
        QRadioButton::indicator {
            width: 14px;
            height: 14px;
            border: 1px solid #264554;
            border-radius: 7px;
            background-color: #162a35;
        }
        QRadioButton::indicator:checked {
            background-color: #06b6d4;
            border-color: #06b6d4;
        }

        /* ── Scroll area ── */
        QScrollArea {
            background-color: #0d1a1f;
            border: none;
        }
    )";
}
