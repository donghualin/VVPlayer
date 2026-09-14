#include "mainwindow.h"
#include <QFileDialog>

MainWindow::MainWindow(QWidget *parent)
    : QWidget(parent)
{
    ui.setupUi(this);

    connect(ui.btnSelectFile, &QPushButton::clicked, this, [this]() {
        QString filePath = QFileDialog::getOpenFileName(
            this,
            "Select Video File",
            QString(),
            "Video Files (*.mp4 *.avi *.mkv *.mov *.flv *.wmv);;All Files (*)"
        );
        if (!filePath.isEmpty()) {
            ui.editFilePath->setText(filePath);
        }
    });

    connect(ui.btnPlay, &QPushButton::clicked, this, [this]() {
        // TODO: implement play/pause toggle
    });

    connect(ui.btnStop, &QPushButton::clicked, this, [this]() {
        // TODO: implement stop
    });
}

MainWindow::~MainWindow()
{
}