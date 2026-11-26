/*
 * PowerMac Emulator - Qt virtual machine manager.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "vmmanager.h"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QStyle>
#include <QTableWidget>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <string>
#include <vector>

// Required by utils/imgfile_sdl.cpp.
bool is_deterministic = false;

namespace {

QString toQ(const std::string& s) { return QString::fromStdString(s); }
std::string toS(const QString& s) { return s.toStdString(); }

QString modelName(const QString& id)
{
    if (id == "pm9600") return "Power Macintosh 9600";
    if (id == "pm8600") return "Power Macintosh 8600";
    return (id == "pm9500") ? "Power Macintosh 9500" : "Power Macintosh 8500";
}

int ramIndex(int mb)
{
    static const int vals[] = { 16, 32, 64, 128, 256, 512 };
    for (int i = 0; i < 6; i++)
        if (vals[i] == mb)
            return i;
    return 3;
}

class VmDialog : public QDialog {
public:
    VmDialog(QWidget* parent, const QString& title, bool edit)
        : QDialog(parent), edit_(edit)
    {
        setWindowTitle(title);
        setMinimumWidth(560);

        name_ = new QLineEdit(this);
        if (edit)
            name_->setReadOnly(true);

        model_ = new QComboBox(this);
        model_->addItem("Power Macintosh 8500", "pm8500");
        model_->addItem("Power Macintosh 9500", "pm9500");
        model_->addItem("Power Macintosh 8600", "pm8600");
        model_->addItem("Power Macintosh 9600", "pm9600");
        model_->addItem("Power Macintosh G3 (Beige)", "pmg3dt");

        cpu_ = new QComboBox(this);
        cpu_->addItem("PowerPC 604", "604");
        cpu_->addItem("PowerPC 604e", "604e");
        cpu_->addItem("PowerPC 750 (G3, Mac OS 9.2)", "750");

        ram_ = new QComboBox(this);
        for (const char* r : { "16 MB", "32 MB", "64 MB", "128 MB", "256 MB", "512 MB" })
            ram_->addItem(r);

        diskSize_ = new QComboBox(this);
        diskSize_->addItem("None", 0);
        diskSize_->addItem("1 GB", 1);
        diskSize_->addItem("2 GB", 2);
        diskSize_->addItem("4 GB", 4);
        diskSize_->addItem("8 GB", 8);
        diskSize_->addItem("16 GB", 16);
        diskSize_->setCurrentIndex(2);

        rom_ = new QLineEdit(this);
        cd_  = new QLineEdit(this);
        cd2_ = new QLineEdit(this);
        hdd_ = new QLineEdit(this);
        hdd2_ = new QLineEdit(this);

        romBrowse_  = new QPushButton("...", this);
        cdBrowse_   = new QPushButton("...", this);
        cd2Browse_  = new QPushButton("...", this);
        hddBrowse_  = new QPushButton("...", this);
        hdd2Browse_ = new QPushButton("...", this);
        romBrowse_->setFixedWidth(32);
        cdBrowse_->setFixedWidth(32);
        cd2Browse_->setFixedWidth(32);
        hddBrowse_->setFixedWidth(32);
        hdd2Browse_->setFixedWidth(32);

        auto* romRow = new QWidget(this);
        auto* romLay = new QHBoxLayout(romRow);
        romLay->setContentsMargins(0, 0, 0, 0);
        romLay->addWidget(rom_);
        romLay->addWidget(romBrowse_);

        auto* cdRow = new QWidget(this);
        auto* cdLay = new QHBoxLayout(cdRow);
        cdLay->setContentsMargins(0, 0, 0, 0);
        cdLay->addWidget(cd_);
        cdLay->addWidget(cdBrowse_);

        auto* cd2Row = new QWidget(this);
        auto* cd2Lay = new QHBoxLayout(cd2Row);
        cd2Lay->setContentsMargins(0, 0, 0, 0);
        cd2Lay->addWidget(cd2_);
        cd2Lay->addWidget(cd2Browse_);

        auto* hddRow = new QWidget(this);
        auto* hddLay = new QHBoxLayout(hddRow);
        hddLay->setContentsMargins(0, 0, 0, 0);
        hddLay->addWidget(hdd_);
        hddLay->addWidget(hddBrowse_);

        auto* hdd2Row = new QWidget(this);
        auto* hdd2Lay = new QHBoxLayout(hdd2Row);
        hdd2Lay->setContentsMargins(0, 0, 0, 0);
        hdd2Lay->addWidget(hdd2_);
        hdd2Lay->addWidget(hdd2Browse_);

        hddBus_ = new QComboBox(this);
        hddBus_->addItem("External SCSI (Curio)", "curio");
        hddBus_->addItem("Internal Fast SCSI (MESH)", "mesh");
        cdBus_ = new QComboBox(this);
        cdBus_->addItem("External SCSI (Curio)", "curio");
        cdBus_->addItem("Internal Fast SCSI (MESH)", "mesh");

        gpu_ = new QComboBox(this);
        gpu_->addItem("None", "none");
        gpu_->addItem("ATI Rage GT", "AtiRageGT");
        gpu_->addItem("ATI Rage Pro", "AtiRagePro");
        gpu_->addItem("ATI Mach64 GX", "AtiMach64Gx");

        net_ = new QCheckBox("MACE Ethernet (built-in)", this);
        net_->setChecked(true);
        sound_ = new QCheckBox("Screamer sound card", this);
        sound_->setChecked(true);
        rhapsody_ = new QCheckBox("Use RhapsodyEmulator (G3 / IDE profile)", this);
        rhapsody_->setToolTip(
            "Launch RhapsodyEmulator.exe for this VM. The selected hard disk and\n"
            "CD images are attached to the G3 IDE controller.");
        dr1_ = new QCheckBox("Rhapsody DR1 compatibility (PM8500 / SCSI)", this);
        dr1_->setToolTip(
            "Launch RhapsodyEmulator.exe with the old-world PM8500/Curio SCSI\n"
            "profile. Rhapsody DR1 does not support G3/IDE.");

        auto* form = new QFormLayout();
        form->addRow("Name", name_);
        form->addRow("Model", model_);
        form->addRow("CPU", cpu_);
        form->addRow("Memory", ram_);
        if (!edit_)
            form->addRow("Hard disk (dynamic VHD)", diskSize_);
        form->addRow("Boot ROM", romRow);
        form->addRow("Install CD (optional)", cdRow);
        form->addRow("Second CD-ROM (optional)", cd2Row);
        if (edit_) {
            form->addRow("Hard disk image(s)", hddRow);
            form->addRow("Hard disk bus", hddBus_);
            form->addRow("Second IDE disk (BootX)", hdd2Row);
        }
        form->addRow("CD-ROM bus", cdBus_);
        form->addRow("PCI video (9500, slot D2)", gpu_);
        form->addRow("Network card", net_);
        form->addRow("Sound card", sound_);
        form->addRow("Rhapsody mode", rhapsody_);
        form->addRow("Rhapsody DR1", dr1_);

        auto* hint = new QLabel(
            "Mac OS 9.2 requires a PowerPC 750 (G3).\n"
            "Rhapsody mode uses RhapsodyEmulator.exe, a G3/IDE profile, and the\n"
            "selected hard disk and CD images. A G3 ROM is required.", this);
        hint->setStyleSheet("color: #555;");

        auto* buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        buttons->button(QDialogButtonBox::Ok)->setText(edit_ ? "Save" : "Create");

        auto* lay = new QVBoxLayout(this);
        lay->addLayout(form);
        lay->addWidget(hint);
        lay->addWidget(buttons);

        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(rhapsody_, &QCheckBox::toggled, this, [this](bool on) {
            if (!on)
                return;
            dr1_->setChecked(false);
            model_->setCurrentIndex(model_->findData("pmg3dt"));
            cpu_->setCurrentIndex(cpu_->findData("750"));
        });
        connect(dr1_, &QCheckBox::toggled, this, [this](bool on) {
            if (!on)
                return;
            rhapsody_->setChecked(false);
            model_->setCurrentIndex(model_->findData("pm8500"));
            cpu_->setCurrentIndex(cpu_->findData("604"));
            ram_->setCurrentIndex(ramIndex(512));
            hddBus_->setCurrentIndex(hddBus_->findData("curio"));
            cdBus_->setCurrentIndex(cdBus_->findData("curio"));
        });
        connect(romBrowse_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Boot ROM",
                rom_->text(), "ROM images (*.rom *.ROM *.bin);;All files (*)");
            if (!f.isEmpty()) rom_->setText(f);
        });
        connect(cdBrowse_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select CD Image",
                cd_->text(),
                "CD images (*.iso *.cue *.bin *.mdf *.mds *.img *.ccd);;All files (*)");
            if (!f.isEmpty()) cd_->setText(f);
        });
        connect(cd2Browse_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Second CD Image",
                cd2_->text(),
                "CD images (*.iso *.cue *.bin *.mdf *.mds *.img *.ccd);;All files (*)");
            if (!f.isEmpty()) cd2_->setText(f);
        });
        connect(hddBrowse_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Hard Disk Image",
                hdd_->text(), "Disk images (*.vhd *.img *.raw *.hdd);;All files (*)");
            if (!f.isEmpty()) hdd_->setText(f);
        });
        connect(hdd2Browse_, &QPushButton::clicked, this, [this] {
            QString f = QFileDialog::getOpenFileName(this, "Select Second IDE Disk",
                hdd2_->text(), "Disk images (*.dmg *.img *.raw *.vhd);;All files (*)");
            if (!f.isEmpty()) hdd2_->setText(f);
        });
    }

    void setSettings(const vm::VmSettings& s)
    {
        model_->setCurrentIndex(model_->findData(toQ(s.machine)));
        cpu_->setCurrentIndex(cpu_->findData(toQ(s.cpu)));
        ram_->setCurrentIndex(ramIndex(s.ram_mb));
        rom_->setText(toQ(s.rom));
        QStringList cds = toQ(s.scsi_cd).split(';', Qt::SkipEmptyParts);
        cd_->setText(cds.value(0).trimmed());
        cd2_->setText(cds.value(1).trimmed());
        hdd_->setText(toQ(s.scsi_hdd));
        hdd2_->setText(toQ(s.ide_hdd2));
        hddBus_->setCurrentIndex(hddBus_->findData(toQ(s.hdd_bus)));
        cdBus_->setCurrentIndex(cdBus_->findData(toQ(s.cd_bus)));
        gpu_->setCurrentIndex(gpu_->findData(toQ(s.pci_gpu)));
        net_->setChecked(s.net_enabled);
        sound_->setChecked(s.sound_enabled);
        rhapsody_->setChecked(s.rhapsody);
        dr1_->setChecked(s.rhapsody_dr1);
    }

    void setName(const QString& n) { name_->setText(n); }
    QString name() const { return name_->text().trimmed(); }
    int diskGb() const { return diskSize_->currentData().toInt(); }

    vm::VmSettings settings() const
    {
        static const int ramVals[] = { 16, 32, 64, 128, 256, 512 };
        vm::VmSettings s;
        s.machine  = toS(model_->currentData().toString());
        s.cpu      = toS(cpu_->currentData().toString());
        s.ram_mb   = ramVals[std::clamp(ram_->currentIndex(), 0, 5)];
        s.rom      = toS(rom_->text().trimmed());
        s.scsi_cd  = toS(cd_->text().trimmed());
        QString cd2 = cd2_->text().trimmed();
        if (!cd2.isEmpty()) {
            if (!s.scsi_cd.empty())
                s.scsi_cd += ";";
            s.scsi_cd += toS(cd2);
        }
        s.scsi_hdd = toS(hdd_->text().trimmed());
        s.ide_hdd2 = toS(hdd2_->text().trimmed());
        s.hdd_bus  = toS(hddBus_->currentData().toString());
        s.cd_bus   = toS(cdBus_->currentData().toString());
        s.pci_gpu  = toS(gpu_->currentData().toString());
        s.net_enabled = net_->isChecked();
        s.sound_enabled = sound_->isChecked();
        s.rhapsody = rhapsody_->isChecked();
        s.rhapsody_dr1 = dr1_->isChecked();
        return s;
    }

private:
    bool edit_;
    QLineEdit* name_;
    QComboBox* model_;
    QComboBox* cpu_;
    QComboBox* ram_;
    QComboBox* diskSize_;
    QLineEdit* rom_;
    QLineEdit* cd_;
    QLineEdit* cd2_;
    QLineEdit* hdd_;
    QLineEdit* hdd2_;
    QPushButton* romBrowse_;
    QPushButton* cdBrowse_;
    QPushButton* cd2Browse_;
    QPushButton* hddBrowse_;
    QPushButton* hdd2Browse_;
    QComboBox* hddBus_;
    QComboBox* cdBus_;
    QComboBox* gpu_;
    QCheckBox* net_;
    QCheckBox* sound_;
    QCheckBox* rhapsody_;
    QCheckBox* dr1_;
};

class MainWindow : public QMainWindow {
public:
    MainWindow()
    {
        setWindowTitle("PowerMac Emulator - Virtual Machine Manager");
        resize(980, 620);

        emulatorExe_ = QCoreApplication::applicationDirPath() +
#ifdef _WIN32
            "/PowerMacEmulator.exe";
#else
            "/PowerMacEmulator";
#endif

        auto* file = menuBar()->addMenu("&File");
        auto* edit = menuBar()->addMenu("&Edit");
        auto* vmMenu = menuBar()->addMenu("&Virtual Machine");
        auto* help = menuBar()->addMenu("&Help");

        auto* actNew = file->addAction(style()->standardIcon(QStyle::SP_FileIcon),
                                       "&New Virtual Machine...");
        auto* actExit = file->addAction("E&xit");
        auto* actEdit = edit->addAction(style()->standardIcon(QStyle::SP_FileDialogDetailedView),
                                        "&Edit Virtual Machine...");
        auto* actDelete = edit->addAction(style()->standardIcon(QStyle::SP_TrashIcon),
                                          "&Delete Virtual Machine...");
        auto* actOpen = edit->addAction(style()->standardIcon(QStyle::SP_DirOpenIcon),
                                        "Open VM &Folder");
        auto* actStart = vmMenu->addAction(style()->standardIcon(QStyle::SP_MediaPlay),
                                           "&Start");
        auto* actRefresh = vmMenu->addAction(style()->standardIcon(QStyle::SP_BrowserReload),
                                             "&Refresh");
        auto* actAbout = help->addAction("&About");

        auto* tb = addToolBar("Operations");
        tb->setIconSize(QSize(22, 22));
        tb->addAction(actNew);
        tb->addAction(actEdit);
        tb->addAction(actStart);
        tb->addAction(actDelete);
        tb->addAction(actOpen);
        tb->addSeparator();
        tb->addAction(actRefresh);

        auto* central = new QWidget(this);
        auto* lay = new QVBoxLayout(central);
        lay->setContentsMargins(8, 8, 8, 8);

        auto* rootRow = new QHBoxLayout();
        rootRow->addWidget(new QLabel("VM root folder:", this));
        rootEdit_ = new QLineEdit(this);
        rootRow->addWidget(rootEdit_, 1);
        auto* btnRoot = new QPushButton("Browse...", this);
        auto* btnSet  = new QPushButton("Set root", this);
        rootRow->addWidget(btnRoot);
        rootRow->addWidget(btnSet);
        lay->addLayout(rootRow);

        table_ = new QTableWidget(this);
        table_->setColumnCount(5);
        QStringList headers{ "Name", "Model", "CPU", "Memory", "Boot ROM" };
        table_->setHorizontalHeaderLabels(headers);
        table_->horizontalHeader()->setStretchLastSection(true);
        table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->setSelectionMode(QAbstractItemView::SingleSelection);
        table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table_->verticalHeader()->setVisible(false);
        lay->addWidget(table_, 1);

        auto* buttons = new QHBoxLayout();
        auto* btnStart = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay),
                                         "Start", this);
        auto* btnNew = new QPushButton("New VM...", this);
        auto* btnEdit = new QPushButton("Edit...", this);
        auto* btnDel = new QPushButton("Delete...", this);
        auto* btnRefresh = new QPushButton("Refresh", this);
        buttons->addWidget(btnStart);
        buttons->addSpacing(12);
        buttons->addWidget(btnNew);
        buttons->addWidget(btnEdit);
        buttons->addWidget(btnDel);
        buttons->addStretch(1);
        buttons->addWidget(btnRefresh);
        lay->addLayout(buttons);

        setCentralWidget(central);
        statusBar()->showMessage("Ready");

        connect(btnRoot, &QPushButton::clicked, this, &MainWindow::chooseRoot);
        connect(btnSet, &QPushButton::clicked, this, [this] {
            applyRoot(rootEdit_->text());
        });
        connect(btnRefresh, &QPushButton::clicked, this, &MainWindow::refresh);
        connect(btnStart, &QPushButton::clicked, this, &MainWindow::startSelected);
        connect(btnNew, &QPushButton::clicked, this, &MainWindow::newVm);
        connect(btnEdit, &QPushButton::clicked, this, &MainWindow::editSelected);
        connect(btnDel, &QPushButton::clicked, this, &MainWindow::deleteSelected);
        connect(actNew, &QAction::triggered, this, &MainWindow::newVm);
        connect(actEdit, &QAction::triggered, this, &MainWindow::editSelected);
        connect(actDelete, &QAction::triggered, this, &MainWindow::deleteSelected);
        connect(actOpen, &QAction::triggered, this, &MainWindow::openFolderSelected);
        connect(actStart, &QAction::triggered, this, &MainWindow::startSelected);
        connect(actRefresh, &QAction::triggered, this, &MainWindow::refresh);
        connect(actExit, &QAction::triggered, this, &QWidget::close);
        connect(actAbout, &QAction::triggered, this, [this] {
            QMessageBox::about(this, "About PowerMac Emulator",
                "PowerMac Emulator - Power Macintosh 8500/9500\n\n"
                "Emulation engine based on DingusPPC (GPLv3).\n"
                "Qt front-end. Dynamic VHD storage.");
        });
        connect(table_, &QTableWidget::cellDoubleClicked, this,
                [this](int, int) { startSelected(); });

        rootEdit_->setText(toQ(vm::root_dir()));
        refresh();
    }

protected:
    void closeEvent(QCloseEvent* e) override
    {
        e->accept();
        QCoreApplication::quit();
    }

private:
    int selectedRow() const
    {
        auto rows = table_->selectionModel()->selectedRows();
        return rows.isEmpty() ? -1 : rows.first().row();
    }

    std::string selectedName() const
    {
        int r = selectedRow();
        if (r < 0 || r >= int(vms_.size()))
            return "";
        return vms_[size_t(r)].name;
    }

    void chooseRoot()
    {
        QString dir = QFileDialog::getExistingDirectory(this, "Select VM Root Folder",
                                                        rootEdit_->text());
        if (!dir.isEmpty()) {
            rootEdit_->setText(dir);
            applyRoot(dir);
        }
    }

    void applyRoot(const QString& dir)
    {
        if (dir.isEmpty())
            return;
        vm::set_root_dir(toS(dir));
        rootEdit_->setText(toQ(vm::root_dir()));
        refresh();
    }

    void refresh()
    {
        vms_ = vm::scan();
        table_->setRowCount(int(vms_.size()));
        for (int i = 0; i < int(vms_.size()); i++) {
            const auto& e = vms_[size_t(i)];
            vm::VmSettings s;
            std::string err;
            vm::load_vm_settings(e.name, s, err);
            table_->setItem(i, 0, new QTableWidgetItem(toQ(e.name)));
            table_->setItem(i, 1, new QTableWidgetItem(modelName(toQ(s.machine))));
            table_->setItem(i, 2, new QTableWidgetItem(toQ(s.cpu)));
            table_->setItem(i, 3, new QTableWidgetItem(QString::number(s.ram_mb) + " MB"));
            table_->setItem(i, 4, new QTableWidgetItem(toQ(s.rom)));
        }
        table_->resizeColumnsToContents();
        table_->horizontalHeader()->setStretchLastSection(true);
        statusBar()->showMessage(QString("%1 virtual machine(s) in %2")
            .arg(vms_.size()).arg(toQ(vm::root_dir())));
    }

    void startSelected()
    {
        std::string name = selectedName();
        if (name.empty())
            return;
        std::string err;
        if (vm::launch_vm(name, toS(emulatorExe_), err))
            statusBar()->showMessage(toQ("Started " + name));
        else
            QMessageBox::warning(this, "Start Virtual Machine", toQ(err));
    }

    void newVm()
    {
        VmDialog dlg(this, "New Virtual Machine", false);
        dlg.setName("");
        // Default ROM from <base>/roms if one exists.
        QDir roms(QDir::current().filePath("roms"));
        QStringList romFiles = roms.entryList({ "*.ROM", "*.rom", "*.bin" }, QDir::Files);
        vm::VmSettings defaults;
        if (!romFiles.isEmpty())
            defaults.rom = toS(roms.filePath(romFiles.first()));
        dlg.setSettings(defaults);
        dlg.setName("");

        if (dlg.exec() != QDialog::Accepted)
            return;
        if (dlg.name().isEmpty()) {
            QMessageBox::warning(this, "New Virtual Machine", "Please enter a name.");
            return;
        }
        vm::VmSettings s = dlg.settings();
        std::string err;
        if (!vm::create_vm(toS(dlg.name()), s.machine, s.cpu, s.ram_mb,
                           dlg.diskGb(), s.rom, s.scsi_cd, err)) {
            QMessageBox::warning(this, "New Virtual Machine", toQ(err));
            return;
        }
        {
            vm::VmSettings created;
            if (vm::load_vm_settings(toS(dlg.name()), created, err)) {
                created.hdd_bus = s.hdd_bus;
                created.cd_bus = s.cd_bus;
                created.pci_gpu = s.pci_gpu;
                created.ide_hdd2 = s.ide_hdd2;
                created.net_enabled = s.net_enabled;
                created.sound_enabled = s.sound_enabled;
                created.rhapsody = s.rhapsody;
                created.rhapsody_dr1 = s.rhapsody_dr1;
                vm::update_vm(toS(dlg.name()), created, err);
            }
        }
        refresh();
    }

    void editSelected()
    {
        std::string name = selectedName();
        if (name.empty())
            return;
        vm::VmSettings s;
        std::string err;
        if (!vm::load_vm_settings(name, s, err)) {
            QMessageBox::warning(this, "Edit Virtual Machine", toQ(err));
            return;
        }
        VmDialog dlg(this, "Edit Virtual Machine", true);
        dlg.setName(toQ(name));
        dlg.setSettings(s);
        if (dlg.exec() != QDialog::Accepted)
            return;
        s = dlg.settings();
        if (!vm::update_vm(name, s, err))
            QMessageBox::warning(this, "Edit Virtual Machine", toQ(err));
        else
            refresh();
    }

    void deleteSelected()
    {
        std::string name = selectedName();
        if (name.empty())
            return;
        if (QMessageBox::question(this, "Delete Virtual Machine",
                toQ("Delete \"" + name + "\" and all its disk images?"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        std::string err;
        if (!vm::delete_vm(name, err))
            QMessageBox::warning(this, "Delete Virtual Machine", toQ(err));
        else
            refresh();
    }

    void openFolderSelected()
    {
        std::string name = selectedName();
        if (name.empty())
            return;
        for (const auto& e : vms_) {
            if (e.name == name) {
                vm::open_folder(e.folder);
                break;
            }
        }
    }

    QString emulatorExe_;
    QLineEdit* rootEdit_ = nullptr;
    QTableWidget* table_ = nullptr;
    std::vector<vm::VmEntry> vms_;
};

} // namespace

int main(int argc, char** argv)
{
    std::string baseArg, rootArg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--base" && i + 1 < argc)
            baseArg = argv[++i];
        else if (a == "--root" && i + 1 < argc)
            rootArg = argv[++i];
    }

    QApplication app(argc, argv);
    QApplication::setApplicationName("PowerMac VM Manager");

    if (!baseArg.empty())
        QDir::setCurrent(toQ(baseArg));

    vm::set_program_dir(toS(QCoreApplication::applicationDirPath()));

    if (baseArg.empty()) {
        QDir cwd = QDir::current();
        if (!QDir(cwd.filePath("roms")).exists()) {
            QDir exeDir(QCoreApplication::applicationDirPath());
            if (exeDir.dirName() == "bin") {
                QDir proj = exeDir;
                proj.cdUp();
                proj.cdUp();
                if (QDir(proj.filePath("roms")).exists())
                    QDir::setCurrent(proj.absolutePath());
            }
        }
    }

    vm::load_launcher_settings();
    if (!rootArg.empty())
        vm::set_root_dir(rootArg);

    MainWindow w;
    w.show();
    return app.exec();
}
