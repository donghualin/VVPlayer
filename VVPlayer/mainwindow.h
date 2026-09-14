#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QWidget>
#include "ui_mainwindow.h"

class MainWindow : public QWidget
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private:
    void initConnect();

private:
    Ui::MainWindow ui;
};

#endif // MAINWINDOW_H