/*
 * PowerMac Emulator - Qt window (menu bar + toolbar) hosting the SDL display.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifdef POWERMAC_QT_UI

#include "runtime.h"
#include "frontend.h"

#include <cpu/ppc/ppcemu.h>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEventLoop>
#include <QSize>
#include <QStringList>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QToolBar>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <QImage>
#include <QPainter>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cstring>\n#include <vector>

#include <loguru.hpp>
#include <QKeyEvent>
#include <QEvent>
#include <SDL.h>
#include <QResizeEvent>
#include <QTimer>
#include <QPalette>
#include <QCheckBox>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <string>

namespace {

QString toQ(const std::string& s) { return QString::fromStdString(s); }
std::string toS(const QString& s) { return s.toStdString(); }

int qtKeyToSdl(int key, Qt::KeyboardModifiers mods)
{
    const bool kp = mods.testFlag(Qt::KeypadModifier);

    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return SDLK_a + (key - Qt::Key_A);
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return (kp ? SDLK_KP_0 : SDLK_0) + (key - Qt::Key_0);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12)
        return SDLK_F1 + (key - Qt::Key_F1);

    if (kp) {
        switch (key) {
        case Qt::Key_Plus:      return SDLK_KP_PLUS;
        case Qt::Key_Minus:     return SDLK_KP_MINUS;
        case Qt::Key_Asterisk:  return SDLK_KP_MULTIPLY;
        case Qt::Key_Slash:     return SDLK_KP_DIVIDE;
        case Qt::Key_Period:    return SDLK_KP_PERIOD;
        case Qt::Key_Equal:     return SDLK_KP_EQUALS;
        case Qt::Key_Enter:     return SDLK_KP_ENTER;
        default: break;
        }
    }

    switch (key) {
    case Qt::Key_Space:      return SDLK_SPACE;
    case Qt::Key_Escape:     return SDLK_ESCAPE;
    case Qt::Key_Tab:        return SDLK_TAB;
    case Qt::Key_Backspace:  return SDLK_BACKSPACE;
    case Qt::Key_Return:     return SDLK_RETURN;
    case Qt::Key_Enter:      return SDLK_KP_ENTER;
    case Qt::Key_Delete:     return SDLK_DELETE;
    case Qt::Key_Insert:     return SDLK_INSERT;
    case Qt::Key_Home:       return SDLK_HOME;
    case Qt::Key_End:        return SDLK_END;
    case Qt::Key_PageUp:     return SDLK_PAGEUP;
    case Qt::Key_PageDown:   return SDLK_PAGEDOWN;
    case Qt::Key_Left:       return SDLK_LEFT;
    case Qt::Key_Right:      return SDLK_RIGHT;
    case Qt::Key_Up:         return SDLK_UP;
    case Qt::Key_Down:       return SDLK_DOWN;
    case Qt::Key_Shift:      return SDLK_LSHIFT;
    case Qt::Key_Control:    return SDLK_LCTRL;
    case Qt::Key_Alt:        return SDLK_LALT;
    case Qt::Key_Meta:       return SDLK_LGUI;
    case Qt::Key_CapsLock:   return SDLK_CAPSLOCK;
    case Qt::Key_NumLock:    return SDLK_NUMLOCKCLEAR;
    case Qt::Key_ScrollLock: return SDLK_SCROLLLOCK;
    case Qt::Key_Print:      return SDLK_PRINTSCREEN;
    case Qt::Key_Pause:      return SDLK_PAUSE;
    case Qt::Key_Minus:      return SDLK_MINUS;
    case Qt::Key_Equal:      return SDLK_EQUALS;
    case Qt::Key_BracketLeft:  return SDLK_LEFTBRACKET;
    case Qt::Key_BracketRight: return SDLK_RIGHTBRACKET;
    case Qt::Key_Backslash:  return SDLK_BACKSLASH;
    case Qt::Key_Semicolon:  return SDLK_SEMICOLON;
    case Qt::Key_Apostrophe: return SDLK_QUOTE;
    case Qt::Key_Comma:      return SDLK_COMMA;
    case Qt::Key_Period:     return SDLK_PERIOD;
    case Qt::Key_Slash:      return SDLK_SLASH;
    case Qt::Key_QuoteLeft:  return SDLK_BACKQUOTE;

    // Shifted symbols -> base key; the Shift key event is sent separately.
    case Qt::Key_Exclam:     return SDLK_1;
    case Qt::Key_At:         return SDLK_2;
    case Qt::Key_NumberSign: return SDLK_3;
    case Qt::Key_Dollar:     return SDLK_4;
    case Qt::Key_Percent:    return SDLK_5;
    case Qt::Key_AsciiCircum:return SDLK_6;
    case Qt::Key_Ampersand:  return SDLK_7;
    case Qt::Key_Asterisk:   return SDLK_8;
    case Qt::Key_ParenLeft:  return SDLK_9;
    case Qt::Key_ParenRight: return SDLK_0;
    case Qt::Key_Underscore: return SDLK_MINUS;
    case Qt::Key_Plus:       return SDLK_EQUALS;
    case Qt::Key_BraceLeft:  return SDLK_LEFTBRACKET;
    case Qt::Key_BraceRight: return SDLK_RIGHTBRACKET;
    case Qt::Key_Bar:        return SDLK_BACKSLASH;
    case Qt::Key_Colon:      return SDLK_SEMICOLON;
    case Qt::Key_QuoteDbl:   return SDLK_QUOTE;
    case Qt::Key_Less:       return SDLK_COMMA;
    case Qt::Key_Greater:    return SDLK_PERIOD;
    case Qt::Key_Question:   return SDLK_SLASH;
    case Qt::Key_AsciiTilde: return SDLK_BACKQUOTE;
    default: return 0;
    }
}

class SettingsDialog : public QDialog {
public:
    explicit SettingsDialog(QWidget* parent) : QDialog(parent)
    {
        setWindowTitle("PowerMac Emulator Settings");
        setMinimumWidth(560);

        auto& cfg = frontend::cfg();

        model_ = new QComboBox(this);
        model_->addItem("Power Macintosh 8500", "pm8500");
        model_->addItem("Power Macintosh 9500", "pm9500");
        model_->addItem("Power Macintosh 8600", "pm8600");
        model_->addItem("Power Macintosh 9600", "pm9600");
        model_->addItem("Power Macintosh G3 (Beige)", "pmg3dt");
        {
            int mi = model_->findData(toQ(cfg.machine));
            model_->setCurrentIndex(mi >= 0 ? mi : 0);
        }

        cpu_ = new QComboBox(this);
        cpu_->addItem("PowerPC 604", "604");
        cpu_->addItem("PowerPC 604e", "604e");
        cpu_->addItem("PowerPC 750 (G3, Mac OS 9.2)", "750");
        cpu_->setCurrentIndex(cpu_->findData(toQ(cfg.cpu)));

        ram_ = new QComboBox(this);
        for (const char* r : { "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB" })
            ram_->addItem(r);
        static const int ramVals[] = { 16, 32, 64, 128, 256, 512 };
        int ri = 3;
        for (int i = 0; i < 6; i++)
            if (ramVals[i] == cfg.ram_mb) ri = i;
        ram_->setCurrentIndex(ri);

        rom_ = new QLineEdit(toQ(cfg.rom), this);
        cd_  = new QLineEdit(toQ(cfg.scsi_cd.empty() ? std::string() : cfg.scsi_cd[0]), this);
        cd2_ = new QLineEdit(toQ(cfg.scsi_cd.size() > 1 ? cfg.scsi_cd[1] : std::string()), this);
        hdd_ = new QLineEdit(this);
        if (!cfg.scsi_hdd.empty())
            hdd_->setText(toQ(cfg.scsi_hdd[0]));

        auto* romBrowse  = new QPushButton("...", this);
        auto* cdBrowse   = new QPushButton("...", this);
        auto* cd2Browse  = new QPushButton("...", this);
        auto* hddBrowse  = new QPushButton("...", this);
        for (auto* b : { romBrowse, cdBrowse, cd2Browse, hddBrowse })
            b->setFixedWidth(32);

        auto makeRow = [this](QLineEdit* e, QPushButton* b) {
            auto* w = new QWidget(this);
            auto* l = new QHBoxLayout(w);
            l->setContentsMargins(0, 0, 0, 0);
            l->addWidget(e);
            l->addWidget(b);
            return w;
        };

        hddBus_ = new QComboBox(this);
        hddBus_->addItem("External SCSI (Curio)", "curio");
        hddBus_->addItem("Internal Fast SCSI (MESH)", "mesh");
        hddBus_->setCurrentIndex(hddBus_->findData(toQ(cfg.hdd_bus)));
        cdBus_ = new QComboBox(this);
        cdBus_->addItem("External SCSI (Curio)", "curio");
        cdBus_->addItem("Internal Fast SCSI (MESH)", "mesh");
        cdBus_->setCurrentIndex(cdBus_->findData(toQ(cfg.cd_bus)));

        gpu_ = new QComboBox(this);
        gpu_->addItem("None", "none");
        gpu_->addItem("ATI Rage GT", "AtiRageGT");
        gpu_->addItem("ATI Rage Pro", "AtiRagePro");
        gpu_->addItem("ATI Mach64 GX", "AtiMach64Gx");
        gpu_->setCurrentIndex(gpu_->findData(toQ(cfg.pci_gpu)));

        mon_ = new QComboBox(this);
        mon_->addItem("VGA / SVGA (multiple modes)", "VGA-SVGA");
        mon_->addItem("Multiple Scan 20in", "Multiscan20in");
        mon_->addItem("AppleVision 1710", "AppleVision1710");
        mon_->addItem("DDC / EDID (1280x800)", "");
        {
            int mi = mon_->findData(toQ(cfg.mon_id));
            mon_->setCurrentIndex(mi >= 0 ? mi : 0);
        }

        net_ = new QCheckBox("MACE Ethernet (built-in)", this);
        net_->setChecked(cfg.net_enabled);

        auto* form = new QFormLayout();
        form->addRow("Model", model_);
        form->addRow("CPU", cpu_);
        form->addRow("Memory", ram_);
        form->addRow("Boot ROM", makeRow(rom_, romBrowse));
        form->addRow("CD-ROM image (1)", makeRow(cd_, cdBrowse));
        form->addRow("CD-ROM image (2)", makeRow(cd2_, cd2Browse));
        form->addRow("Hard disk image(s)", makeRow(hdd_, hddBrowse));
        form->addRow("Hard disk bus", hddBus_);
        form->addRow("CD-ROM bus", cdBus_);
        form->addRow("PCI video (9500)", gpu_);
        form->addRow("Monitor", mon_);
        form->addRow("Network card", net_);

        auto* hint = new QLabel("Changes take effect after the virtual machine restarts.", this);
        hint->setStyleSheet("color: #555;");

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText("Save & Restart");

        auto* lay = new QVBoxLayout(this);
        lay->addLayout(form);
        lay->addWidget(hint);
        lay->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

        connect(romBrowse, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Boot ROM", rom_->text(),
                "ROM images (*.rom *.ROM *.bin);;All files (*)");
            if (!f.isEmpty()) rom_->setText(f);
        });
        connect(cdBrowse, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select CD Image", cd_->text(),
                "CD images (*.iso *.cue *.bin *.mdf *.mds *.img *.ccd);;All files (*)");
            if (!f.isEmpty()) cd_->setText(f);
        });
        connect(cd2Browse, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Second CD Image", cd2_->text(),
                "CD images (*.iso *.cue *.bin *.mdf *.mds *.img *.ccd);;All files (*)");
            if (!f.isEmpty()) cd2_->setText(f);
        });
        connect(hddBrowse, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Hard Disk Image", hdd_->text(),
                "Disk images (*.vhd *.img *.raw *.hdd);;All files (*)");
            if (!f.isEmpty()) hdd_->setText(f);
        });
    }

    void apply()
    {
        static const int ramVals[] = { 16, 32, 64, 128, 256, 512 };
        auto& cfg = frontend::cfg();
        cfg.machine  = toS(model_->currentData().toString());
        cfg.cpu      = toS(cpu_->currentData().toString());
        cfg.ram_mb   = ramVals[std::clamp(ram_->currentIndex(), 0, 5)];
        cfg.rom      = toS(rom_->text().trimmed());
        cfg.scsi_cd.clear();
        if (!cd_->text().trimmed().isEmpty())
            cfg.scsi_cd.push_back(toS(cd_->text().trimmed()));
        if (!cd2_->text().trimmed().isEmpty())
            cfg.scsi_cd.push_back(toS(cd2_->text().trimmed()));
        cfg.scsi_hdd.clear();
        if (!hdd_->text().trimmed().isEmpty())
            cfg.scsi_hdd.push_back(toS(hdd_->text().trimmed()));
        cfg.hdd_bus = toS(hddBus_->currentData().toString());
        cfg.cd_bus  = toS(cdBus_->currentData().toString());
        cfg.pci_gpu = toS(gpu_->currentData().toString());
        cfg.mon_id = toS(mon_->currentData().toString());
        cfg.net_enabled = net_->isChecked();
        frontend::apply_machine_defaults();
        frontend::save_config(frontend::active_config_path());
    }

private:
    QComboBox* model_;
    QComboBox* cpu_;
    QComboBox* ram_;
    QLineEdit* rom_;
    QLineEdit* cd_;
    QLineEdit* cd2_;
    QLineEdit* hdd_;
    QComboBox* hddBus_;
    QComboBox* cdBus_;
    QComboBox* gpu_;
    QComboBox* mon_;
    QCheckBox* net_;
};


class DisplayWidget : public QWidget {
public:
    DisplayWidget(SDL_Window* sdl_window, QWidget* parent = nullptr)
        : QWidget(parent), sdl_window_(sdl_window)
    {
        setAutoFillBackground(true);
        QPalette pal = palette();
        pal.setColor(QPalette::Window, Qt::black);
        setPalette(pal);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);
    }

    void presentFrame(const uint32_t* pixels, int w, int h, bool guest_cursor)
    {
        if (!pixels || w <= 0 || h <= 0)
            return;

        if (image_.width() != w || image_.height() != h)
            image_ = QImage(w, h, QImage::Format_RGB32);
        if (image_.isNull())
            return;

        std::memcpy(image_.bits(), pixels, size_t(w) * size_t(h) * 4);
        if (guest_cursor != guest_cursor_) {
            guest_cursor_ = guest_cursor;
            if (guest_cursor_)
                setCursor(Qt::BlankCursor);
            else
                unsetCursor();
        }
        update();
    }

    void presentBlank()
    {
        image_ = QImage();
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), Qt::black);
        if (image_.isNull())
            return;

        const QRect dst = imageRect();
        painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
        painter.drawImage(dst, image_);
    }

    void mouseMoveEvent(QMouseEvent* e) override
    {
        const QPoint guest = toGuest(e->position().toPoint());
        int gx = guest.x(), gy = guest.y();
        int rx = gx - last_x_;
        int ry = gy - last_y_;
        last_x_ = gx;
        last_y_ = gy;

        SDL_Event ev{};
        ev.type = SDL_MOUSEMOTION;
        ev.motion.windowID = windowId();
        ev.motion.x = gx;
        ev.motion.y = gy;
        ev.motion.xrel = rx;
        ev.motion.yrel = ry;
        ev.motion.state = 0;
        SDL_PushEvent(&ev);
    }

    void mousePressEvent(QMouseEvent* e) override
    {
        pushButton(e, SDL_MOUSEBUTTONDOWN);
        setFocus(Qt::MouseFocusReason);
    }

    void mouseReleaseEvent(QMouseEvent* e) override
    {
        pushButton(e, SDL_MOUSEBUTTONUP);
    }

    void wheelEvent(QWheelEvent* e) override
    {
        SDL_Event ev{};
        ev.type = SDL_MOUSEWHEEL;
        ev.wheel.windowID = windowId();
        ev.wheel.y = e->angleDelta().y() > 0 ? 1 : -1;
        SDL_PushEvent(&ev);
    }

private:
    QRect imageRect() const
    {
        if (image_.isNull())
            return rect();

        const double sx = double(width()) / image_.width();
        const double sy = double(height()) / image_.height();
        const double scale = std::min(sx, sy);
        const int w = std::max(1, int(std::round(image_.width() * scale)));
        const int h = std::max(1, int(std::round(image_.height() * scale)));
        return QRect((width() - w) / 2, (height() - h) / 2, w, h);
    }

    QPoint toGuest(QPoint p) const
    {
        if (image_.isNull())
            return p;

        const QRect dst = imageRect();
        if (dst.width() <= 0 || dst.height() <= 0)
            return p;

        int x = (p.x() - dst.x()) * image_.width() / dst.width();
        int y = (p.y() - dst.y()) * image_.height() / dst.height();
        x = std::clamp(x, 0, image_.width() - 1);
        y = std::clamp(y, 0, image_.height() - 1);
        return QPoint(x, y);
    }

    Uint32 windowId() const
    {
        return sdl_window_ ? SDL_GetWindowID(sdl_window_) : 0;
    }

    void pushButton(QMouseEvent* e, Uint32 type)
    {
        Uint8 button = 0;
        switch (e->button()) {
            case Qt::LeftButton:   button = SDL_BUTTON_LEFT;   break;
            case Qt::MiddleButton: button = SDL_BUTTON_MIDDLE; break;
            case Qt::RightButton:  button = SDL_BUTTON_RIGHT;  break;
            default: return;
        }

        const QPoint g = toGuest(e->position().toPoint());
        SDL_Event ev{};
        ev.type = type;
        ev.button.windowID = windowId();
        ev.button.button = button;
        ev.button.state = (type == SDL_MOUSEBUTTONDOWN) ? SDL_PRESSED : SDL_RELEASED;
        ev.button.clicks = 1;
        ev.button.x = g.x();
        ev.button.y = g.y();
        SDL_PushEvent(&ev);
    }

    SDL_Window* sdl_window_ = nullptr;
    QImage image_;
    bool guest_cursor_ = false;
    int last_x_ = 0;
    int last_y_ = 0;
};
class ActivityIcon : public QWidget {
public:
    ActivityIcon(DeviceActivity kind, QWidget* parent = nullptr)
        : QWidget(parent), kind_(kind)
    {
        setFixedSize(30, 20);
        setToolTip(kind == DeviceActivity::Hdd ? "Hard disk" :
                   kind == DeviceActivity::Cdrom ? "CD-ROM" :
                   kind == DeviceActivity::Floppy ? "Floppy" : "Network");
    }

    void poll()
    {
        uint32_t serial = frontend_activity_serial(kind_);
        if (serial != serial_) {
            serial_ = serial;
            lit_ = true;
            write_ = frontend_activity_write(kind_);
            update();
            QTimer::singleShot(180, this, [this] {
                lit_ = false;
                update();
            });
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(90, 90, 90), 1));
        p.setBrush(QColor(210, 210, 210));

        switch (kind_) {
        case DeviceActivity::Hdd:
            p.drawRoundedRect(QRectF(1, 4, 18, 12), 2, 2);
            p.drawLine(QPointF(4, 8), QPointF(16, 8));
            p.drawLine(QPointF(4, 11), QPointF(12, 11));
            break;
        case DeviceActivity::Cdrom:
            p.drawEllipse(QRectF(1, 3, 18, 18));
            p.setBrush(QColor(120, 120, 120));
            p.drawEllipse(QRectF(7, 9, 6, 6));
            break;
        case DeviceActivity::Floppy:
            p.drawRect(QRectF(1, 3, 18, 16));
            p.setBrush(QColor(130, 130, 130));
            p.drawRect(QRectF(6, 3, 8, 6));
            p.setBrush(QColor(210, 210, 210));
            p.drawRect(QRectF(5, 12, 10, 6));
            break;
        case DeviceActivity::Network:
            p.drawRect(QRectF(1, 4, 10, 8));
            p.drawRect(QRectF(9, 9, 10, 8));
            p.drawLine(QPointF(4, 7), QPointF(8, 7));
            p.drawLine(QPointF(12, 12), QPointF(16, 12));
            break;
        }

        QColor led = QColor(70, 70, 70);
        if (lit_)
            led = write_ ? QColor(220, 40, 40) : QColor(40, 210, 70);
        p.setPen(Qt::NoPen);
        p.setBrush(led);
        p.drawEllipse(QRectF(21, 6, 7, 7));
    }

private:
    DeviceActivity kind_;
    uint32_t serial_ = 0;
    bool lit_ = false;
    bool write_ = false;
};

class EmuWindow : public QMainWindow {
public:
    EmuWindow()
    {
#ifdef POWERMAC_RHAPSODY
        setWindowTitle("Rhapsody Emulator");
#else
        setWindowTitle("PowerMac Emulator");
#endif
        resize(820, 640);
        setAcceptDrops(true);
        QApplication::instance()->installEventFilter(this);

        auto* central = new QWidget(this);
        layout_ = new QVBoxLayout(central);
        layout_->setContentsMargins(0, 0, 0, 0);
        layout_->setSpacing(0);
        placeholder_ = new QLabel("Starting Power Macintosh...", this);
        placeholder_->setAlignment(Qt::AlignCenter);
        placeholder_->setStyleSheet("font-size: 18px; color: #555;");
        layout_->addWidget(placeholder_);
        setCentralWidget(central);

        auto* file = menuBar()->addMenu("&File");
        auto* vm = menuBar()->addMenu("&Virtual Machine");
        auto* view = menuBar()->addMenu("&View");
        auto* help = menuBar()->addMenu("&Help");

        auto* actSettings = file->addAction(
            style()->standardIcon(QStyle::SP_FileDialogDetailedView), "&Settings...");
        auto* actVmManager = file->addAction("Virtual Machine &Manager...");
        auto* actExit = file->addAction("E&xit");

        auto* actPause = vm->addAction(style()->standardIcon(QStyle::SP_MediaPause), "&Pause");
        auto* actResume = vm->addAction(style()->standardIcon(QStyle::SP_MediaPlay), "&Resume");
        auto* actBootRhapsody = vm->addAction("&Boot Rhapsody (Caps Lock)");
        auto* actReset = vm->addAction(style()->standardIcon(QStyle::SP_BrowserReload), "&Reset");
        auto* actChangeCd = vm->addAction(
            style()->standardIcon(QStyle::SP_DialogOpenButton), "&Change CD Image...");
        auto* actEject = vm->addAction(style()->standardIcon(QStyle::SP_DriveCDIcon), "&Eject Media");
        auto* actInsertFloppy = vm->addAction("Insert &Floppy Image...");
        auto* actEjectFloppy = vm->addAction("Eject &Floppy");

        auto* actFull = view->addAction("&Full Screen");
        auto* actAbout = help->addAction("&About");

        auto* tb = addToolBar("Operations");
        tb->setIconSize(QSize(22, 22));
        tb->addAction(actPause);
        tb->addAction(actResume);
        tb->addAction(actBootRhapsody);
        tb->addAction(actReset);
        tb->addAction(actChangeCd);
        tb->addAction(actEject);
        tb->addAction(actInsertFloppy);
        tb->addAction(actEjectFloppy);
        tb->addSeparator();
        tb->addAction(actSettings);

        connect(actPause, &QAction::triggered, this, [this] {
            g_emulator_paused = true;
            statusBar()->showMessage("Paused");
        });
        connect(actResume, &QAction::triggered, this, [this] {
            g_emulator_paused = false;
            statusBar()->showMessage("Running");
        });
        connect(actBootRhapsody, &QAction::triggered, this, [this] {
            bootRhapsody();
        });
        connect(actReset, &QAction::triggered, this, [this] {
            g_emulator_paused = false;
            statusBar()->showMessage("Resetting...");
            power_off(po_restarting);
        });
        connect(actChangeCd, &QAction::triggered, this, [this] {
            chooseCdImage();
        });
        connect(actEject, &QAction::triggered, this, [this] {
            frontend_eject_cdrom();
            statusBar()->showMessage("Ejecting media...");
        });
        connect(actInsertFloppy, &QAction::triggered, this, [this] {
            chooseFloppyImage();
        });
        connect(actEjectFloppy, &QAction::triggered, this, [this] {
            frontend_eject_floppy();
            auto& cfg = frontend::cfg();
            cfg.fdd_img.clear();
            frontend::save_config(frontend::active_config_path());
            statusBar()->showMessage("Floppy ejected");
        });
        connect(actSettings, &QAction::triggered, this, &EmuWindow::openSettings);
        connect(actVmManager, &QAction::triggered, this, &EmuWindow::launchVmManager);
        connect(actExit, &QAction::triggered, this, [this] {
            power_off(po_quit);
            close();
        });
        connect(actFull, &QAction::triggered, this, [this] {
            if (isFullScreen()) showNormal(); else showFullScreen();
        });
        connect(actAbout, &QAction::triggered, this, [this] {
            QMessageBox::about(this, "About PowerMac Emulator",
                "PowerMac Emulator - Power Macintosh 8500/9500\n\n"
                "Emulation engine based on DingusPPC (GPLv3).\n"
                "Qt window with SDL display.");
        });

        if (!frontend::cfg().scsi_cd.empty())
            lastCdPath_ = toQ(frontend::cfg().scsi_cd[0]);
        else if (!frontend::cfg().ide_cd.empty())
            lastCdPath_ = toQ(frontend::cfg().ide_cd);

        auto addActivity = [this](DeviceActivity kind, bool visible) {
            if (!visible)
                return;
            auto* icon = new ActivityIcon(kind, this);
            activity_icons_.push_back(icon);
            statusBar()->addPermanentWidget(icon);
        };
        const auto& cfg = frontend::cfg();
        addActivity(DeviceActivity::Hdd,
                    !cfg.scsi_hdd.empty() || !cfg.ide_hdd.empty());
        addActivity(DeviceActivity::Cdrom,
                    !cfg.scsi_cd.empty() || !cfg.ide_cd.empty());
        addActivity(DeviceActivity::Floppy, true);
        addActivity(DeviceActivity::Network, cfg.net_enabled);

        auto* activity_timer = new QTimer(this);
        connect(activity_timer, &QTimer::timeout, this, [this] {
            for (auto* icon : activity_icons_)
                icon->poll();
        });
        activity_timer->start(50);

        capsLed_    = new QLabel("CapsLk", this);
        numLed_     = new QLabel("NumLk", this);
        scrollLed_  = new QLabel("ScrollLk", this);
        for (auto* led : { capsLed_, numLed_, scrollLed_ }) {
            led->setAlignment(Qt::AlignCenter);
            led->setMinimumWidth(58);
            statusBar()->addPermanentWidget(led);
        }
        updateLeds();

        statusBar()->showMessage("Starting...");
    }

    void embed(void* native_handle, void* sdl_window, int w, int h)
    {
        (void)native_handle;
        cleanupDisplay();
        sdl_window_ = static_cast<SDL_Window*>(sdl_window);

        auto* display = new DisplayWidget(sdl_window_, this);
        host_ = display;
        layout_->addWidget(host_, 1);
        host_->show();

        resize(std::max(820, w + 24), std::max(640, h + 110));
        statusBar()->showMessage(QString("Running - %1x%2").arg(w).arg(h));
    }

    void presentFrame(const uint32_t* pixels, int w, int h, bool guest_cursor)
    {
        if (auto* display = dynamic_cast<DisplayWidget*>(host_))
            display->presentFrame(pixels, w, h, guest_cursor);
    }

    void presentBlank()
    {
        if (auto* display = dynamic_cast<DisplayWidget*>(host_))
            display->presentBlank();
    }

    void detach()
    {
        cleanupDisplay();
        showPlaceholder("Restarting Power Macintosh...");
    }

    void updateDisplaySize(int w, int h)
    {
        if (!host_)
            return;

        LOG_F(INFO, "Qt display request: guest %dx%d window %dx%d host %dx%d",
              w, h, width(), height(), host_->width(), host_->height());

        if (!isFullScreen() && !isMaximized()) {
            int chrome_w = width() - host_->width();
            int chrome_h = height() - host_->height();
            if (chrome_w < 0) chrome_w = 0;
            if (chrome_h < 0) chrome_h = 0;

            resize(w + chrome_w, h + chrome_h);
            if (layout_)
                layout_->activate();
            if (QApplication::instance())
                QApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        }
        // Fill the embedded child even when the host is maximized/fullscreen.
        resizeDisplayToHost();

        LOG_F(INFO, "Qt display result: window %dx%d host %dx%d",
              width(), height(), host_->width(), host_->height());
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        power_off(po_quit);
        e->accept();
    }

    void resizeEvent(QResizeEvent* e) override
    {
        QMainWindow::resizeEvent(e);
        resizeDisplayToHost();
    }

    void dragEnterEvent(QDragEnterEvent* e) override
    {
        if (e->mimeData()->hasUrls())
            e->acceptProposedAction();
    }

    void dropEvent(QDropEvent* e) override
    {
        const auto urls = e->mimeData()->urls();
        if (!urls.isEmpty() && urls.first().isLocalFile()) {
            applyCdImage(urls.first().toLocalFile());
            e->acceptProposedAction();
        }
    }

    bool eventFilter(QObject* obj, QEvent* ev) override
    {
        if (sdl_window_ && isActiveWindow() &&
            (ev->type() == QEvent::KeyPress || ev->type() == QEvent::KeyRelease) &&
            !QApplication::activeModalWidget() &&
            !QApplication::activePopupWidget()) {
            auto* ke = static_cast<QKeyEvent*>(ev);
            if (ev->type() == QEvent::KeyPress && !ke->isAutoRepeat()) {
                bool changed = false;
                if (ke->key() == Qt::Key_CapsLock) {
                    capsOn_ = !capsOn_;
                    changed = true;
                } else if (ke->key() == Qt::Key_NumLock) {
                    numOn_ = !numOn_;
                    changed = true;
                } else if (ke->key() == Qt::Key_ScrollLock) {
                    scrollOn_ = !scrollOn_;
                    changed = true;
                }
                if (changed)
                    updateLeds();
            }
            if (forwardKey(ke, ev->type() == QEvent::KeyPress))
                return true;
        }
        return QMainWindow::eventFilter(obj, ev);
    }

public:
    void releaseCaps()
    {
        if (!capsLatched_ || !sdl_window_)
            return;
        SDL_Event up{};
        up.key.windowID = SDL_GetWindowID(sdl_window_);
        up.key.repeat = 0;
        up.key.keysym.sym = SDLK_CAPSLOCK;
        up.key.keysym.scancode = SDL_SCANCODE_CAPSLOCK;
        up.type = SDL_KEYUP;
        up.key.state = SDL_RELEASED;
        up.key.keysym.mod = KMOD_NONE;
        SDL_PushEvent(&up);
        capsLatched_ = false;
        capsOn_ = false;
        updateLeds();
        LOG_F(INFO, "Boot Rhapsody: released CapsLock before restart");
    }
    void bootRhapsody()
    {
        if (!sdl_window_)
            return;
        if (!capsLatched_) {
            SDL_Event ev{};
            ev.key.windowID = SDL_GetWindowID(sdl_window_);
            ev.key.repeat = 0;
            ev.key.keysym.sym = SDLK_CAPSLOCK;
            ev.key.keysym.scancode = SDL_SCANCODE_CAPSLOCK;
            ev.key.keysym.mod = KMOD_CAPS;
            ev.type = SDL_KEYDOWN;
            ev.key.state = SDL_PRESSED;
            SDL_PushEvent(&ev);
            capsLatched_ = true;
            capsOn_ = true;
            updateLeds();
            statusBar()->showMessage("Caps Lock held for Rhapsody boot");
            LOG_F(INFO, "Boot Rhapsody: latched CapsLock");


        } else {
            statusBar()->showMessage("Caps Lock already latched");
        }
    }
    void chooseCdImage()
    {
        QString f = QFileDialog::getOpenFileName(this, "Insert CD Image",
            lastCdPath_,
            "CD images (*.iso *.bin *.cue *.mdf *.mds *.img *.ccd);;All files (*)");
        if (!f.isEmpty())
            applyCdImage(f);
    }

    void chooseFloppyImage()
    {
        QString start = toQ(frontend::cfg().fdd_img);
        QString f = QFileDialog::getOpenFileName(this, "Insert Floppy Image",
            start, "Floppy images (*.dsk *.img *.ima *.vfd *.hfs *.mfm *.raw);;All files (*)");
        if (!f.isEmpty())
            applyFloppyImage(f);
    }

    void applyFloppyImage(const QString& f)
    {
        auto& cfg = frontend::cfg();
        if (frontend_swap_floppy(toS(f), cfg.fdd_wr_prot)) {
            cfg.fdd_img = toS(f);
            frontend::save_config(frontend::active_config_path());
            statusBar()->showMessage("Inserted floppy: " + f);
        } else {
            QMessageBox::warning(this, "Floppy",
                "Could not insert the floppy image.\n\n" + f);
        }
    }

    void applyCdImage(const QString& f)
    {
        lastCdPath_ = f;
        if (frontend_swap_cdrom(toS(f))) {
            auto& cfg = frontend::cfg();
            if (cfg.scsi_cd.empty())
                cfg.scsi_cd.push_back(toS(f));
            else
                cfg.scsi_cd[0] = toS(f);
            frontend::save_config(frontend::active_config_path());
            statusBar()->showMessage("Inserted CD: " + f);
        } else {
            QMessageBox::warning(this, "CD-ROM",
                "Could not insert the CD image.\n\n" + f +
                "\n\nSupported image types: ISO/raw, BIN/CUE, CCD/IMG, MDS/MDF.");
        }
    }

    void openSettings()
    {
        SettingsDialog dlg(this);
        if (dlg.exec() != QDialog::Accepted)
            return;
        dlg.apply();
        g_emulator_paused = false;
        power_off(po_restarting);
    }

    void launchVmManager()
    {
        QString dir = QCoreApplication::applicationDirPath();
        QString exe = dir;
#ifdef _WIN32
        exe += "/PowerMacVMManager.exe";
#else
        exe += "/PowerMacVMManager";
#endif
        if (!QProcess::startDetached(exe, QStringList(), dir))
            QMessageBox::warning(this, "Virtual Machine Manager",
                "Could not start PowerMacVMManager.");
    }

    bool forwardKey(QKeyEvent* e, bool down)
    {
        if (!sdl_window_)
            return false;

        if (e->key() == Qt::Key_CapsLock) {
            // ADB Caps Lock is a latching key. The Rhapsody DR1 "press Caps
            // Lock" startup path checks that it is still held after the
            // QuickTime extension loads, so never send an immediate release.
            if (!down || e->isAutoRepeat())
                return true;

            SDL_Event ev{};
            ev.key.windowID = SDL_GetWindowID(sdl_window_);
            ev.key.repeat = 0;
            ev.key.keysym.sym = SDLK_CAPSLOCK;
            ev.key.keysym.scancode = SDL_SCANCODE_CAPSLOCK;
            ev.type = capsLatched_ ? SDL_KEYUP : SDL_KEYDOWN;
            ev.key.state = capsLatched_ ? SDL_RELEASED : SDL_PRESSED;
            ev.key.keysym.mod = capsLatched_ ? KMOD_NONE : KMOD_CAPS;
            capsLatched_ = !capsLatched_;
            SDL_PushEvent(&ev);
            LOG_F(INFO, "Qt CapsLock latch %s", capsLatched_ ? "down" : "up");
            return true;
        }

        int sym = qtKeyToSdl(e->key(), e->modifiers());
        if (!sym)
            return false;

        SDL_Event ev{};
        ev.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        ev.key.windowID = SDL_GetWindowID(sdl_window_);
        ev.key.state = down ? SDL_PRESSED : SDL_RELEASED;
        ev.key.repeat = e->isAutoRepeat() ? 1 : 0;
        ev.key.keysym.sym = SDL_Keycode(sym);
        ev.key.keysym.scancode = SDL_GetScancodeFromKey(SDL_Keycode(sym));

        Uint16 mod = KMOD_NONE;
        auto qm = e->modifiers();
        if (qm & Qt::ShiftModifier)   mod |= KMOD_LSHIFT;
        if (qm & Qt::ControlModifier) mod |= KMOD_LCTRL;
        if (qm & Qt::AltModifier)     mod |= KMOD_LALT;
        if (qm & Qt::MetaModifier)    mod |= KMOD_LGUI;
        mod |= (SDL_GetModState() & KMOD_CAPS);
        ev.key.keysym.mod = mod;

        SDL_PushEvent(&ev);
        return true;
    }

    void cleanupDisplay()
    {
        sdl_window_ = nullptr;
        if (sdl_hwnd_) {
#ifdef _WIN32
            SetParent(static_cast<HWND>(sdl_hwnd_), nullptr);
#endif
            sdl_hwnd_ = nullptr;
        }
        if (container_) {
            layout_->removeWidget(container_);
            delete container_;
            container_ = nullptr;
        }
        if (host_) {
            layout_->removeWidget(host_);
            delete host_;
            host_ = nullptr;
        }
        if (placeholder_) {
            layout_->removeWidget(placeholder_);
            delete placeholder_;
            placeholder_ = nullptr;
        }
    }

    void showPlaceholder(const QString& text)
    {
        placeholder_ = new QLabel(text, this);
        placeholder_->setAlignment(Qt::AlignCenter);
        placeholder_->setStyleSheet("font-size: 18px; color: #555;");
        layout_->addWidget(placeholder_, 1);
    }

    void resizeDisplayToHost()
    {
        // The Qt display widget follows the layout automatically.
    }

    void updateLeds()
    {
        auto style = [](bool on) {
            return on
                ? QStringLiteral("QLabel{background:#2e7d32;color:white;"
                                 "border-radius:7px;padding:1px 7px;font-weight:bold;}")
                : QStringLiteral("QLabel{background:#505050;color:#b8b8b8;"
                                 "border-radius:7px;padding:1px 7px;}");
        };
        if (capsLed_)
            capsLed_->setStyleSheet(style(capsOn_));
        if (numLed_)
            numLed_->setStyleSheet(style(numOn_));
        if (scrollLed_)
            scrollLed_->setStyleSheet(style(scrollOn_));
    }

    QVBoxLayout* layout_ = nullptr;
    QWidget* host_ = nullptr;
    QWidget* container_ = nullptr; // non-Windows fallback
    void*    sdl_hwnd_ = nullptr;
    SDL_Window* sdl_window_ = nullptr;
    QLabel*  placeholder_ = nullptr;
    QLabel*  capsLed_ = nullptr;
    QLabel*  numLed_ = nullptr;
    QLabel*  scrollLed_ = nullptr;
    bool     capsOn_ = false;
    bool     capsLatched_ = false;
    bool     numOn_ = false;
    bool     scrollOn_ = false;
    QString  lastCdPath_;
    std::vector<ActivityIcon*> activity_icons_;
};

QApplication* g_app = nullptr;
EmuWindow*   g_win = nullptr;

} // namespace

void frontend_qt_init()
{
    if (QApplication::instance())
        return;

    static int   qt_argc = 1;
    static char  qt_arg0[] = "PowerMacEmulator";
    static char* qt_argv[] = { qt_arg0, nullptr };
    g_app = new QApplication(qt_argc, qt_argv);
#ifdef POWERMAC_RHAPSODY
    QApplication::setApplicationName("Rhapsody Emulator");
#else
    QApplication::setApplicationName("PowerMac Emulator");
#endif

    g_win = new EmuWindow();
    g_win->show();
}

void frontend_qt_pump()
{
    if (QApplication::instance())
        QApplication::processEvents(QEventLoop::AllEvents, 5);
}

void frontend_qt_embed_window(void* native_handle, void* sdl_window,
                            int width, int height)
{
    if (g_win)
        g_win->embed(native_handle, sdl_window, width, height);
}

void frontend_qt_display_destroyed()
{
    if (g_win)
        g_win->detach();
}

void frontend_qt_update_display_size(int width, int height)
{
    // The engine runs on the same thread as the Qt event loop, so the
    // window and the embedded SDL child can be resized synchronously.
    if (g_win)
        g_win->updateDisplaySize(width, height);
}

void frontend_qt_present_frame(const uint32_t* pixels, int width, int height,
                               bool guest_cursor)
{
    if (g_win)
        g_win->presentFrame(pixels, width, height, guest_cursor);
}

void frontend_qt_present_blank()
{
    if (g_win)
        g_win->presentBlank();
}

void frontend_qt_release_caps()
{
    if (g_win)
        g_win->releaseCaps();
}

#endif // POWERMAC_QT_UI
