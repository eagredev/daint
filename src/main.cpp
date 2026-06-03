// Daint: a simple MS-Paint-style raster paint program.
// Copyright (C) 2026  eagre.dev
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. This program is distributed WITHOUT ANY WARRANTY; see the GNU General
// Public License (LICENSE file) for details.

#include "mainwindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Daint"));
    QApplication::setOrganizationName(QStringLiteral("eagre.dev"));
    QApplication::setDesktopFileName(QStringLiteral("io.github.eagredev.Daint"));

    MainWindow window;
    window.show();
    return app.exec();
}
