// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <memory>
#include <QWidget>

namespace Ui {
class ConfigureDebug;
}

class ConfigureDebug : public QWidget
{
    Q_OBJECT

public:
    explicit ConfigureDebug(QWidget *parent = nullptr);
    ~ConfigureDebug();

    void applyConfiguration();

private:
    void setConfiguration();

private:
    std::unique_ptr<Ui::ConfigureDebug> ui;
};
