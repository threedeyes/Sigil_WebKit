/************************************************************************
**
**  Copyright (C) 2018, 2019  Kevin B. Hendricks, Stratford, Ontario, Canada
**  Copyright (C) 2009, 2010, 2011  Strahinja Markovic  <strahinja.markovic@gmail.com>
**
**  This file is part of Sigil.
**
**  Sigil is free software: you can redistribute it and/or modify
**  it under the terms of the GNU General Public License as published by
**  the Free Software Foundation, either version 3 of the License, or
**  (at your option) any later version.
**
**  Sigil is distributed in the hope that it will be useful,
**  but WITHOUT ANY WARRANTY; without even the implied warranty of
**  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**  GNU General Public License for more details.
**
**  You should have received a copy of the GNU General Public License
**  along with Sigil.  If not, see <http://www.gnu.org/licenses/>.
**
*************************************************************************/

#include "Misc/EmbeddedPython.h"
#include <iostream>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QLibraryInfo>
#include <QtCore/QTextCodec>
#include <QtCore/QThreadPool>
#include <QtCore/QTranslator>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>
#include <QXmlStreamReader>
#include <QFileInfo>
#include <QDebug>

#include "Misc/PluginDB.h"
#include "Misc/UILanguage.h"
#include "MainUI/MainApplication.h"
#include "MainUI/MainWindow.h"
#include "Misc/AppEventFilter.h"
#include "Misc/SettingsStore.h"
#include "Misc/TempFolder.h"
#include "Misc/UpdateChecker.h"
#include "Misc/Utility.h"
#include "sigil_constants.h"
#include "sigil_exception.h"

#ifdef Q_OS_WIN32
# include <QFile>
# include <QTextStream>
# include <QProcessEnvironment>
# include <QtWidgets/QPlainTextEdit>
# include "ViewEditors/BookViewPreview.h"
static const QString WIN_CLIPBOARD_ERROR = "QClipboard::setMimeData: Failed to set data on clipboard";
static const int RETRY_DELAY_MS = 5;
#endif

#ifdef Q_OS_MAC
# include <QFileDialog>
# include <QKeySequence>
# include <QAction>
#endif

// Creates a MainWindow instance depending
// on command line arguments
static MainWindow *GetMainWindow(const QStringList &arguments)
{
    // We use the first argument as the file to load after starting
    QString filepath;
    if (arguments.size() > 1 && Utility::IsFileReadable(arguments.at(1))) {
        filepath = arguments.at(1);
    }
    return new MainWindow(filepath);
}

#ifdef Q_OS_MAC
static void file_new()
{
    MainWindow *w = GetMainWindow(QStringList());
    w->show();
}

static void file_open()
{
    const QMap<QString, QString> load_filters = MainWindow::GetLoadFiltersMap();
    QStringList filters(load_filters.values());
    filters.removeDuplicates();
    QString filter_string = "";
    foreach(QString filter, filters) {
        filter_string += filter + ";;";
    }
    // "All Files (*.*)" is the default
    QString default_filter = load_filters.value("epub");
    QString filename = QFileDialog::getOpenFileName(0,
                       "Open File",
                       "~",
                       filter_string,
                       &default_filter
                                                   );

    if (!filename.isEmpty()) {
        MainWindow *w = GetMainWindow(QStringList() << "" << filename);
        w->show();
    }
}
#endif

#if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
// Returns a QIcon with the Sigil "S" logo in various sizes
static QIcon GetApplicationIcon()
{
    QIcon app_icon;
    // This 16x16 one looks wrong for some reason
    //app_icon.addFile( ":/icon/app_icon_16.png", QSize( 16, 16 ) );
    app_icon.addFile(":/icon/app_icon_32.png",  QSize(32, 32));
    app_icon.addFile(":/icon/app_icon_48.png",  QSize(48, 48));
    app_icon.addFile(":/icon/app_icon_128.png", QSize(128, 128));
    app_icon.addFile(":/icon/app_icon_256.png", QSize(256, 256));
    app_icon.addFile(":/icon/app_icon_512.png", QSize(512, 512));
    return app_icon;
}
#endif


// The message handler installed to handle Qt messages
void MessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    QString error_message;
    QString win_debug_message;

    switch (type) {
        // TODO: should go to a log
        case QtDebugMsg:
            win_debug_message = QString("Debug: %1").arg(message.toLatin1().constData());
            fprintf(stderr, "Debug: %s\n", message.toLatin1().constData());
            break;
#if QT_VERSION >= 0x050600
        case QtInfoMsg:
            fprintf(stderr, "Info: %s\n", message.toLatin1().constData());
            break;
#endif
        // TODO: should go to a log
        case QtWarningMsg:
            fprintf(stderr, "Warning: %s\n", message.toLatin1().constData());
            break;
        case QtCriticalMsg:
            error_message = QString(message.toLatin1().constData());
#ifdef Q_OS_WIN32
            // On Windows there is a known issue with the clipboard that results in some copy
            // operations in controls being intermittently blocked. Rather than presenting
            // the user with an error dialog, we should simply retry the operation.
            // Hopefully this will be fixed in a future Qt version (still broken as of 4.8.3).
            if (error_message.startsWith(WIN_CLIPBOARD_ERROR)) {
                QWidget *widget = QApplication::focusWidget();

                if (widget) {
                    QPlainTextEdit *textEdit = dynamic_cast<QPlainTextEdit *>(widget);

                    if (textEdit) {
                        QTimer::singleShot(RETRY_DELAY_MS, textEdit, SLOT(copy()));
                        break;
                    }

                    // BV/PV copying is a little different, in that the focus widget is set to
                    // the parent editor (unlike CodeView's QPlainTextEdit).
                    BookViewPreview *bookViewPreview = dynamic_cast<BookViewPreview *>(widget);

                    if (bookViewPreview) {
                        QTimer::singleShot(RETRY_DELAY_MS, bookViewPreview, SLOT(copy()));
                        break;
                    }

                    // Same issue can happen on a QLineEdit / QComboBox
                    QLineEdit *lineEdit = dynamic_cast<QLineEdit *>(widget);

                    if (lineEdit) {
                        QTimer::singleShot(RETRY_DELAY_MS, lineEdit, SLOT(copy()));
                        break;
                    }

                    QComboBox *comboBox = dynamic_cast<QComboBox *>(widget);

                    if (comboBox) {
                        QTimer::singleShot(RETRY_DELAY_MS, comboBox->lineEdit(), SLOT(copy()));
                        break;
                    }
                }
            }

#endif
            Utility::DisplayExceptionErrorDialog(QString("Critical: %1").arg(error_message));
            break;

        case QtFatalMsg:
            Utility::DisplayExceptionErrorDialog(QString("Fatal: %1").arg(QString(message)));
            abort();
    }
#ifdef Q_OS_WIN32
    // qDebug() prints to WINDOWS_SIGIL_DEBUG_LOGFILE environment variable on Windows.
    // User must have permissions to write to the location or no file will be created.
    if (qEnvironmentVariableIsSet("WINDOWS_SIGIL_DEBUG_LOGFILE") && !qEnvironmentVariableIsEmpty("WINDOWS_SIGIL_DEBUG_LOGFILE")) {
        QString sigil_log_file;
        sigil_log_file = QProcessEnvironment::systemEnvironment().value("WINDOWS_SIGIL_DEBUG_LOGFILE", "").trimmed();
        QFile outFile(sigil_log_file);
        outFile.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        QTextStream ts(&outFile);
        ts << win_debug_message << endl;
    }
#endif
}


void VerifyPlugins()
{
    PluginDB *pdb = PluginDB::instance();
    pdb->load_plugins_from_disk();
}


// Application entry point
int main(int argc, char *argv[])
{
    QT_REQUIRE_VERSION(argc, argv, "5.0.0");
#ifndef QT_DEBUG
    qInstallMessageHandler(MessageHandler);
#endif

    // Set application information for easier use of QSettings classes
    QCoreApplication::setOrganizationName("sigil-ebook");
    QCoreApplication::setOrganizationDomain("sigil-ebook.com");
    QCoreApplication::setApplicationName("sigil");
    QCoreApplication::setApplicationVersion(SIGIL_VERSION);
    
    // many qtbugs related to mixing 32 and 64 bit qt apps when shader disk cache is used
    // Only use if using Qt5.9.0 or higher
#if QT_VERSION >= 0x050900
    QCoreApplication::setAttribute(Qt::AA_DisableShaderDiskCache);
#endif

 
#if 0   // On recent processors with multiple cores this leads to over 40 threads at times
        // We prevent Qt from constantly creating and deleting threads.
        // Using a negative number forces the threads to stay around;
        // that way, we always have a steady number of threads ready to do work.
        QThreadPool::globalInstance()->setExpiryTimeout(-1);
#endif

    MainApplication app(argc, argv);

    // drag and drop in main tab bar is too touchy and that can cause problems.
    // default drag distance limit is much too small especially for hpi displays
    // startDragDistance default is just 10 pixels
    if (app.startDragDistance() < 50) app.setStartDragDistance(50);

    // Set up embedded python integration first thing
    EmbeddedPython* epython = EmbeddedPython::instance();
    epython->addToPythonSysPath(epython->embeddedRoot());
    epython->addToPythonSysPath(PluginDB::launcherRoot() + "/python");

    try {

        // Specify the plugin folders
        // (language codecs and image loaders)
        app.addLibraryPath("codecs");
        app.addLibraryPath("iconengines");
        app.addLibraryPath("imageformats");

        QTextCodec::setCodecForLocale(QTextCodec::codecForName("utf8"));
        SettingsStore settings;
        // Setup the qtbase_ translator and load the translation for the selected language
        QTranslator qtbaseTranslator;
        const QString qm_name_qtbase = QString("qtbase_%1").arg(settings.uiLanguage());
        // Run though all locations and stop once we find and are able to load
        // an appropriate Qt base translation.
        foreach(QString path, UILanguage::GetPossibleTranslationPaths()) {
            if (QDir(path).exists()) {
                if (qtbaseTranslator.load(qm_name_qtbase, path)) {
                    break;
                }
            }
        }
        app.installTranslator(&qtbaseTranslator);

        // Setup the Sigil translator and load the translation for the selected language
        QTranslator sigilTranslator;
        const QString qm_name = QString("sigil_%1").arg(settings.uiLanguage());
        // Run though all locations and stop once we find and are able to load
        // an appropriate translation.
        foreach(QString path, UILanguage::GetPossibleTranslationPaths()) {
            if (QDir(path).exists()) {
                if (sigilTranslator.load(qm_name, path)) {
                    break;
                }
            }
        }
        app.installTranslator(&sigilTranslator);

        // Check for existing qt_styles.qss in Prefs dir and load it if present
        QString qt_stylesheet_path = Utility::DefinePrefsDir() + "/qt_styles.qss";
        QFileInfo QtStylesheetInfo(qt_stylesheet_path);
        if (QtStylesheetInfo.exists() && QtStylesheetInfo.isFile() && QtStylesheetInfo.isReadable()) {
            QString qtstyles = Utility::ReadUnicodeTextFile(qt_stylesheet_path);
            app.setStyleSheet(qtstyles);
        }

        // Qt's setCursorFlashTime(msecs) (or the docs) are broken
        // According to the docs, setting a negative value should disable cursor blinking 
        // but instead just forces it to look for PlatformSpecific Themeable Hints to get 
        // a value which for Mac OS X is hardcoded to 1000 ms
        // This was the only way I could get Qt to disable cursor blinking on a Mac if desired
        if (qEnvironmentVariableIsSet("SIGIL_DISABLE_CURSOR_BLINK")) {
            // qDebug() << "trying to disable text cursor blinking";
            app.setCursorFlashTime(0);
            // qDebug() << "cursorFlashTime: " << app.cursorFlashTime();
        }
        // We set the window icon explicitly on Linux.
        // On Windows this is handled by the RC file,
        // and on Mac by the ICNS file.
#if !defined(Q_OS_WIN32) && !defined(Q_OS_MAC)
        app.setWindowIcon(GetApplicationIcon());
#if QT_VERSION >= 0x050700
        // Wayland needs this clarified in order to propery assign the icon 
        app.setDesktopFileName(QStringLiteral("sigil.desktop"));
#endif
#endif
        // Needs to be created on the heap so that
        // the reply has time to return.
#ifndef __HAIKU__
        UpdateChecker *checker = new UpdateChecker(&app);
        checker->CheckForUpdate();
#endif

        // Install an event filter for the application
        // so we can catch OS X's file open events
        AppEventFilter *filter = new AppEventFilter(&app);
        app.installEventFilter(filter);

        QStringList arguments = QCoreApplication::arguments();

#ifdef Q_OS_MAC
        // now process main app events so that any startup 
        // FileOpen event will be processed for macOS
        QCoreApplication::processEvents();

        QString filepath = filter->getInitialFilePath();

        // if one found append it to argv for processing as normal
        if ((arguments.size() == 1) && !filepath.isEmpty()) {
            arguments << QFileInfo(filepath).absoluteFilePath();
        }
#endif

        if (arguments.contains("-t")) {
            std::cout  << TempFolder::GetPathToSigilScratchpad().toStdString() << std::endl;
            return 1;
        } else {
            // Normal startup

#ifdef Q_OS_MAC
            // Work around QTBUG-62193 and QTBUG-65245 and others where menubar
	    // menu items are lost under File and Sigil menus and where
	    // Quit menu gets lost when deleting other windows first

	    // Now we Create and show a frameless translucent QMainWindow to hold
	    // the menubar.  Note: macOS has a single menubar attached at 
	    // the top of the screen that all mainwindows share.

            app.setQuitOnLastWindowClosed(false);

	    Qt::WindowFlags flags = Qt::Window | Qt::FramelessWindowHint;
	    QMainWindow * basemw = new QMainWindow(NULL, flags);
	    basemw->setAttribute(Qt::WA_TranslucentBackground, true);

            QMenuBar *mac_menu = new QMenuBar(0);
            QMenu *file_menu = new QMenu("File");

	    // New
            QAction * new_action = new QAction("New");
            new_action->setShortcut(QKeySequence("Ctrl+N"));
            QObject::connect(new_action, &QAction::triggered, file_new);
	    file_menu ->addAction(new_action);

            // Open
	    QAction* open_action = new QAction("Open");
            open_action->setShortcut(QKeySequence("Ctrl+O"));
            QObject::connect(open_action, &QAction::triggered, file_open);
	    file_menu ->addAction(open_action);

            // Quit - force add of a secondary quit menu to the file menu
	    QAction* quit_action = new QAction("Quit");
            quit_action->setMenuRole(QAction::NoRole);
            quit_action->setShortcut(QKeySequence("Ctrl+Q"));
            QObject::connect(quit_action, &QAction::triggered, qApp->quit);
	    file_menu ->addAction(quit_action);

            mac_menu->addMenu(file_menu);
            
	    // Application specific quit menu
	    // according to Qt docs this is the right way to add an application
	    // quit menu - but it does not work and will still sometimes get lost
	    mac_menu->addAction("quit");

	    basemw->setMenuBar(mac_menu);
            basemw->show();
#endif

            VerifyPlugins();
            MainWindow *widget = GetMainWindow(arguments);
            widget->show();
            return app.exec();
        }
    } catch (std::exception e) {
        Utility::DisplayExceptionErrorDialog(e.what());
        return 1;
    }
}
