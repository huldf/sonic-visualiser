/* -*- c-basic-offset: 4 indent-tabs-mode: nil -*-  vi:set ts=8 sts=4 sw=4: */

/*
    Sonic Visualiser
    An audio file viewer and annotation editor.
    Centre for Digital Music, Queen Mary, University of London.
    This file copyright 2006-2023 Chris Cannam and QMUL.
    
    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License as
    published by the Free Software Foundation; either version 2 of the
    License, or (at your option) any later version.  See the file
    COPYING included with this distribution for more information.
*/

#include "MainWindow.h"
#include "SVSplash.h"

#include "system/System.h"
#include "system/Init.h"
#include "base/TempDirectory.h"
#include "base/PropertyContainer.h"
#include "base/Preferences.h"
#include "data/fileio/FileSource.h"
#include "widgets/InteractiveFileFinder.h"
#include "framework/TransformUserConfigurator.h"
#include "transform/TransformFactory.h"
#include "plugin/PluginScan.h"
#include "plugin/PluginPathSetter.h"

#include <QMetaType>
#include <QApplication>
#include <QScreen>
#include <QMessageBox>
#include <QTranslator>
#include <QLocale>
#include <QSettings>
#include <QIcon>
#include <QSessionManager>
#include <QDir>
#include <QTimer>
#include <QPainter>
#include <QFileOpenEvent>
#include <QCommandLineParser>
#include <QSslSocket>
#include <QFont>
#include <QFontInfo>

#include <iostream>
#include <signal.h>

#include "../version.h"

#if (defined (HAVE_FFTW3F) || defined (HAVE_FFTW3))
#include <fftw3.h>
#endif

/*! \mainpage Sonic Visualiser

\section interesting Summary of interesting classes

 - Data models: Model and subclasses, e.g. WaveFileModel

 - Graphical layers: Layer and subclasses, displayed on View and its
 subclass widgets.

 - Main window class, document class, and file parser: MainWindow,
 Document, SVFileReader

 - Turning one model (e.g. audio) into another (e.g. more audio, or a
 curve extracted from it): Transform, encapsulating the data that need
 to be stored to be able to reproduce a given transformation;
 TransformFactory, for discovering the available types of transform;
 ModelTransformerFactory, ModelTransformer and subclasses, providing
 the mechanisms for applying transforms to data models

 - Creating the plugins used by transforms: RealTimePluginFactory,
 FeatureExtractionPluginFactory.  See also the API documentation for
 Vamp feature extraction plugins at
 http://www.vamp-plugins.org/code-doc/.

 - File reading and writing code: AudioFileReader and subclasses,
 WavFileWriter, DataFileReader, SVFileReader

 - FFT calculation and cacheing: FFTModel, FFTDataServer

 - Widgets that show groups of editable properties: PropertyBox for
 layer properties (contained in a PropertyStack), PluginParameterBox
 for plugins (contained in a PluginParameterDialog)

 - Audio playback: AudioCallbackPlaySource and subclasses,
 AudioCallbackPlayTarget and subclasses, AudioGenerator

\section model Data sources: the Model hierarchy

***!!! todo: update this

   A Model is something containing, or knowing how to obtain, data.

   For example, WaveFileModel is a model that knows how to get data
   from an audio file; SparseTimeValueModel is a model containing
   editable "curve" data.

   Models typically subclass one of a number of abstract subclasses of
   Model.  For example, WaveFileModel subclasses DenseTimeValueModel,
   which describes an interface for models that have a value at each
   time point for a given sampling resolution.  (Note that
   WaveFileModel does not actually read the files itself: it uses
   AudioFileReader classes for that.  It just makes data from the
   files available in a Model.)  SparseTimeValueModel uses the
   SparseModel template class, which provides most of the
   implementation for models that contain a series of points of some
   sort -- also used by NoteModel, TextModel, and
   SparseOneDimensionalModel.

   Everything that goes on the screen originates from a model, via a
   layer (see below).  The models are contained in a Document object.
   There is no containment hierarchy or ordering of models in the
   document.  One model is the main model, which defines the sample
   rate for playback.

   A model may also be marked as a "derived" model, which means it was
   generated from another model using some transform (feature
   extraction or effect plugin, etc) -- the idea being that they can
   be re-generated using the same transform if a new source model is
   loaded.

\section layer Things that can display data: the Layer hierarchy

   A Layer is something that knows how to draw parts of a model onto a
   timeline.

   For example, WaveformLayer is a layer which draws waveforms, based
   on WaveFileModel; TimeValueLayer draws curves, based on
   SparseTimeValueModel; SpectrogramLayer draws spectrograms, based on
   WaveFileModel (via FFTModel).

   The most basic functions of a layer are: to draw itself onto a
   Pane, against a timeline on the x axis; and to permit user
   interaction.  If you were thinking of adding the capability to
   display a new sort of something, then you would want to add a new
   layer type.  (You may also need a new model type, depending on
   whether any existing model can capture the data you need.)
   Depending on the sort of data in question, there are various
   existing layers that might be appropriate to start from -- for
   example, a layer that displays images that the user has imported
   and associated with particular times might have something in common
   with the existing TextLayer which displays pieces of text that are
   associated with particular times.

   Although layers are visual objects, they are contained in the
   Document in Sonic Visualiser rather than being managed together
   with display widgets.  The Sonic Visualiser file format has
   separate data and layout sections, and the layers are defined in
   the data section and then referred to in the layout section which
   determines which layers may go on which panes (see Pane below).

   Once a layer class is defined, some basic data about it needs to be
   set up in the LayerFactory class, and then it will appear in the
   menus and so on on the main window.

\section view Widgets that are used to show layers: The View hierarchy

   A View is a widget that displays a stack of layers.  The most
   important subclass is Pane, the widget that is used to show most of
   the data in the main window of Sonic Visualiser.

   All a pane really does is contain a set of layers and get them to
   render themselves (one on top of the other, with the topmost layer
   being the one that is currently interacted with), cache the
   results, negotiate user interaction with them, and so on.  This is
   generally fiddly, if not especially interesting.  Panes are
   strictly layout objects and are not stored in the Document class;
   instead the MainWindow contains a PaneStack widget (the widget that
   takes up most of Sonic Visualiser's main window) which contains a
   set of panes stacked vertically.

   Another View subclass is Overview, which is the widget that
   contains that green waveform showing the entire file at the bottom
   of the window.

*/

static QMutex cleanupMutex;
static bool cleanedUp = false;

static void
signalHandler(int /* signal */)
{
    // Avoid this happening more than once across threads

    std::cerr << "signalHandler: cleaning up and exiting" << std::endl;

    if (cleanupMutex.tryLock(5000)) {
        if (!cleanedUp) {
            TempDirectory::getInstance()->cleanup();
            cleanedUp = true;
        }
        cleanupMutex.unlock();
    }
    
    exit(0);
}

class SVApplication : public QApplication
{
public:
    SVApplication(int &argc, char **argv) :
        QApplication(argc, argv),
        m_mainWindow(nullptr)
    {
    }
    ~SVApplication() override { }

    void setMainWindow(MainWindow *mw) {
        m_mainWindow = mw;
        for (auto f: m_pendingFilepaths) {
            handleFilepathArgument(f, nullptr);
        }
        m_pendingFilepaths.clear();
    }
    
    void releaseMainWindow() {
        m_mainWindow = nullptr;
    }

    virtual void commitData(QSessionManager &manager) {
        if (!m_mainWindow) return;
        bool mayAskUser = manager.allowsInteraction();
        bool success = m_mainWindow->commitData(mayAskUser);
        manager.release();
        if (!success) manager.cancel();
    }

    void handleFilepathArgument(QString path, SVSplash *splash);

protected:
    MainWindow *m_mainWindow;
    std::vector<QString> m_pendingFilepaths;
    bool event(QEvent *) override;
};

int
main(int argc, char **argv)
{
    if (argc == 2 && (QString(argv[1]) == "--version" ||
                      QString(argv[1]) == "-v")) {
        std::cerr << SV_VERSION << std::endl;
        exit(0);
    }
    
    svSystemSpecificInitialisation();

    SVApplication application(argc, argv);

    QApplication::setOrganizationName("sonic-visualiser");
    QApplication::setOrganizationDomain("sonicvisualiser.org");
    QApplication::setApplicationName(QApplication::tr("Sonic Visualiser"));
    QApplication::setApplicationVersion(SV_VERSION);

#if (QT_VERSION >= 0x050700)
    QApplication::setDesktopFileName("sonic-visualiser");
#endif

    SVCerr::installQtMessageHandler();

    QCommandLineParser parser;
    parser.setApplicationDescription(QApplication::tr("\nSonic Visualiser is a program for viewing and exploring audio data\nfor semantic music analysis and annotation."));
    parser.addHelpOption();
    parser.addVersionOption();

    parser.addOption(QCommandLineOption
                     ("no-audio", QApplication::tr
                      ("Do not attempt to open an audio output device.")));
    parser.addOption(QCommandLineOption
                     ("no-osc", QApplication::tr
                      ("Do not provide an Open Sound Control port for remote control.")));
    parser.addOption(QCommandLineOption
                     ("no-splash", QApplication::tr
                      ("Do not show a splash screen.")));
    parser.addOption(QCommandLineOption
                     ("osc-script", QApplication::tr
                      ("Batch run the Open Sound Control script found in the given file. Supply \"-\" as file to read from stdin. Scripts consist of /command arg1 arg2 ... OSC control lines, optionally interleaved with numbers to specify pauses in seconds."),
                      "osc.txt"));
    parser.addOption(QCommandLineOption
                     ("first-run", QApplication::tr
                      ("Clear any saved settings and reset to first-run behaviour.")));

    parser.addPositionalArgument
        ("[<file> ...]", QApplication::tr("One or more Sonic Visualiser (.sv) and audio files may be provided."));
    
    QStringList args = application.arguments();
    if (!parser.parse(args)) {
        if (parser.unknownOptionNames().contains("?")) {
            // QCommandLineParser only understands -? for help on Windows,
            // but we historically accepted it everywhere - provide this
            // backward compatibility
            parser.showHelp();
        }
    }        
        
    parser.process(args);

    if (parser.isSet("first-run")) {
        QSettings settings;
        settings.clear();
    }

    bool audioOutput = !(parser.isSet("no-audio"));
    bool oscSupport = !(parser.isSet("no-osc"));
    bool showSplash = !(parser.isSet("no-splash"));

    if (!audioOutput) {
        SVDEBUG << "Note: --no-audio flag set, will not use audio device" << endl;
    }
    if (!oscSupport) {
        SVDEBUG << "Note: --no-osc flag set, will not open OSC port" << endl;
    }
    
    args = parser.positionalArguments();

    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

#ifndef Q_OS_WIN32
    signal(SIGHUP,  signalHandler);
    signal(SIGQUIT, signalHandler);
#endif

    SVSplash *splash = nullptr;

    QSettings settings;

    QString language = QLocale::system().name();
    SVDEBUG << "System language is: " << language << endl;

    settings.beginGroup("Preferences");
    QString prefLanguage = settings.value("locale", language).toString();
    if (prefLanguage != QString()) language = prefLanguage;
    settings.endGroup();

    settings.beginGroup("Preferences");
    if (!(settings.value("always-use-default-font", false).toBool())) {
#ifdef Q_OS_WIN32
        if (!language.startsWith("ru_")) { // + any future non-Latin i18ns
            QFont font(QApplication::font());
            QString preferredFamily = "Segoe UI";
            font.setFamily(preferredFamily);
            if (QFontInfo(font).family() == preferredFamily) {
                font.setPointSize(9);
                QApplication::setFont(font);
            }
        }
#endif
#ifdef Q_OS_MAC
        QFont font(QApplication::font());
        font.setKerning(false);
        QApplication::setFont(font);
#endif
    }
    settings.endGroup();

    settings.beginGroup("Preferences");
    // Default to using Piper server; can change in preferences
    if (!settings.contains("run-vamp-plugins-in-process")) {
        settings.setValue("run-vamp-plugins-in-process", false);
    }
    settings.endGroup();

    settings.beginGroup("Preferences");
    if (showSplash) {
        if (!settings.value("show-splash", true).toBool()) {
            showSplash = false;
        }
    }
    settings.endGroup();

    if (showSplash) {
        splash = new SVSplash();
        splash->show();
        QTimer::singleShot(5000, splash, SLOT(hide()));
        application.processEvents();
    }

    settings.beginGroup("RDF");
    QStringList list;
    bool absent = !(settings.contains("rdf-indices"));
    QString plugIndex("http://www.vamp-plugins.org/rdf/plugins/index.txt");
    QString packsIndex("http://www.vamp-plugins.org/rdf/packs/index.txt");
    if (absent) {
        list << plugIndex;
        list << packsIndex;
    } else {
        list = settings.value("rdf-indices").toStringList();
        if (!settings.contains("rdf-indices-refreshed-for-4.1")) {
            // Packs introduced
            if (!list.contains(packsIndex)) {
                list << packsIndex;
            }
            settings.setValue("rdf-indices-refreshed-for-4.1", true);
        }
    }
    settings.setValue("rdf-indices", list);
    settings.endGroup();

    PluginPathSetter::initialiseEnvironmentVariables();
    
    QIcon icon;
    int sizes[] = { 16, 22, 24, 32, 48, 64, 128 };
    for (int i = 0; i < int(sizeof(sizes)/sizeof(sizes[0])); ++i) {
        icon.addFile(QString(":icons/sv-%1x%2.png").arg(sizes[i]).arg(sizes[i]));
    }
    QApplication::setWindowIcon(icon);

    if (showSplash) {
        application.processEvents();
    }

    QTranslator qtTranslator;
    QString qtTrName = QString("qt_%1").arg(language);
    SVDEBUG << "Loading " << qtTrName << "... ";
    bool success = false;
    if (!(success = qtTranslator.load(qtTrName))) {
        QString qtDir = getenv("QTDIR");
        if (qtDir != "") {
            success = qtTranslator.load
                (qtTrName, QDir(qtDir).filePath("translations"));
        }
    }
    if (!success) {
        SVDEBUG << "Failed\nFailed to load Qt translation for locale" << endl;
    } else {
        SVDEBUG << "Done" << endl;
    }
    application.installTranslator(&qtTranslator);

    QTranslator svTranslator;
    QString svTrName = QString("sonic-visualiser_%1").arg(language);
    SVDEBUG << "Loading " << svTrName << "... ";
    if (svTranslator.load(svTrName, ":i18n")) {
        SVDEBUG << "Done" << endl;
        application.installTranslator(&svTranslator);
    } else {
        SVDEBUG << "Unable to load" << endl;
    }

    StoreStartupLocale();

#if (QT_VERSION >= 0x050400)
    SVDEBUG << "Note: SSL library build version is: "
            << QSslSocket::sslLibraryBuildVersionString()
            << endl;
#endif

    if (showSplash) {
        application.processEvents();
    }
    
    // Permit these types to be used as args in queued signal calls
    qRegisterMetaType<PropertyContainer::PropertyName>("PropertyContainer::PropertyName");
    qRegisterMetaType<ZoomLevel>("ZoomLevel");

    MainWindow::AudioMode audioMode = 
        MainWindow::AUDIO_PLAYBACK_NOW_RECORD_LATER;
    MainWindow::MIDIMode midiMode =
        MainWindow::MIDI_LISTEN;

    if (!audioOutput) {
        audioMode = MainWindow::AUDIO_NONE;
        midiMode = MainWindow::MIDI_NONE;
    } 
    
    MainWindow *gui = new MainWindow(audioMode, midiMode, oscSupport);
    application.setMainWindow(gui);

    InteractiveFileFinder::setParentWidget(gui);
    TransformUserConfigurator::setParentWidget(gui);
    if (splash) {
        QObject::connect(gui, SIGNAL(hideSplash()), splash, SLOT(hide()));
        QObject::connect(gui, SIGNAL(hideSplash(QWidget *)),
                         splash, SLOT(finishSplash(QWidget *)));
    }

    QScreen *screen = QApplication::primaryScreen();
    QRect available = screen->availableGeometry();

    int width = (available.width() * 2) / 3;
    int height = available.height() / 2;
    if (height < 450) height = (available.height() * 2) / 3;
    if (width > height * 2) width = height * 2;

    settings.beginGroup("MainWindow");

    QSize size = settings.value("size", QSize(width, height)).toSize();
    gui->resizeConstrained(size);

    if (settings.contains("position")) {
        QRect prevrect(settings.value("position").toPoint(), size);
        if (!(available & prevrect).isEmpty()) {
            gui->move(prevrect.topLeft());
        }
    }

    if (settings.value("maximised", false).toBool()) {
        gui->setWindowState(Qt::WindowMaximized);
    }

    settings.endGroup();
    
    gui->show();

    // The MainWindow class seems to have trouble dealing with this if
    // it tries to adapt to this preference before the constructor is
    // complete.  As a lazy hack, apply it explicitly from here
    gui->preferenceChanged("Property Box Layout");

    QStringList filesToOpen;
    for (QString arg: args) {
        // Note QCommandLineParser has now pulled out argv[0] and all
        // the options, so in theory everything here from the very
        // first arg should be relevant. But let's reject names
        // starting with "-" just in case.
        if (arg.startsWith('-')) continue;
        filesToOpen.push_back(arg);
    }
    
    settings.beginGroup("FFTWisdom");
#ifdef HAVE_FFTW3F
    {
        QString wisdom = settings.value("wisdom").toString();
        if (wisdom != "") {
            fftwf_import_wisdom_from_string(wisdom.toLocal8Bit().data());
        }
    }
#endif
#ifdef HAVE_FFTW3
    {
        QString wisdom = settings.value("wisdom_d").toString();
        if (wisdom != "") {
            fftw_import_wisdom_from_string(wisdom.toLocal8Bit().data());
        }
    }
#endif
    settings.endGroup();

    QString scriptFile = parser.value("osc-script");
    if (scriptFile != "") {
        SVDEBUG << "Note: Cueing OSC script from filename \"" << scriptFile
                << "\"" << endl;
        gui->cueOSCScript(scriptFile);
    }

    if (!filesToOpen.empty()) {
        QTimer::singleShot(200, [&]() {
            for (QString file: filesToOpen) {
                QApplication::postEvent(&application, new QFileOpenEvent(file));
            }
        });
    }

    SVDEBUG << "Entering main event loop" << endl;
    
    int rv = application.exec();

    gui->hide();

    cleanupMutex.lock();

    if (!cleanedUp) {
        TransformFactory::deleteInstance();
        TempDirectory::getInstance()->cleanup();
        cleanedUp = true;
    }

    application.releaseMainWindow();

    settings.beginGroup("FFTWisdom");
#ifdef HAVE_FFTW3F
    {
        char *cwisdom = fftwf_export_wisdom_to_string();
        if (cwisdom) {
            settings.setValue("wisdom", QString::fromLocal8Bit(cwisdom));
            free(cwisdom);
        }
    }
#endif
#ifdef HAVE_FFTW3
    {
        char *cwisdom = fftw_export_wisdom_to_string();
        if (cwisdom) {
            settings.setValue("wisdom_d", QString::fromLocal8Bit(cwisdom));
            free(cwisdom);
        }
    }
#endif
    settings.endGroup();

    FileSource::debugReport();
    Profiles::getInstance()->dump();
    
    delete gui;

    cleanupMutex.unlock();

    return rv;
}

bool SVApplication::event(QEvent *event){

// Avoid warnings/errors with -Wextra because we aren't explicitly
// handling all event types (-Wall is OK with this because of the
// default but the stricter level insists)
#pragma GCC diagnostic ignored "-Wswitch-enum"

    QString thePath;

    switch (event->type()) {
    case QEvent::FileOpen:
        SVDEBUG << "SVApplication::event: Handling FileOpen event" << endl;
        thePath = static_cast<QFileOpenEvent *>(event)->file();
        handleFilepathArgument(thePath, nullptr);
        return true;
    default:
        return QApplication::event(event);
    }
}

/** Application-global handler for filepaths passed in, e.g. as command-line arguments or apple events */
void SVApplication::handleFilepathArgument(QString path, SVSplash *splash){
    static bool haveSession = false;
    static bool haveMainModel = false;
    static bool havePriorCommandLineModel = false;

    if (!m_mainWindow) {
        // Not attached yet
        m_pendingFilepaths.push_back(path);
        return;
    }
    
    MainWindow::FileOpenStatus status = MainWindow::FileOpenFailed;

#ifdef Q_OS_WIN32
    path.replace("\\", "/");
#endif

    if (path.endsWith("sv")) {
        if (!haveSession) {
            status = m_mainWindow->openSessionPath(path);
            if (status == MainWindow::FileOpenSucceeded) {
                haveSession = true;
                haveMainModel = true;
            }
        } else {
            SVCERR << "WARNING: Ignoring additional session file argument \"" << path << "\"" << endl;
            status = MainWindow::FileOpenSucceeded;
        }
    }
    if (status != MainWindow::FileOpenSucceeded) {
        if (!haveMainModel) {
            status = m_mainWindow->openPath(path, MainWindow::ReplaceSession);
            if (status == MainWindow::FileOpenSucceeded) {
                haveMainModel = true;
            }
        } else {
            if (haveSession && !havePriorCommandLineModel) {
                status = m_mainWindow->openPath(path, MainWindow::AskUser);
                if (status == MainWindow::FileOpenSucceeded) {
                    havePriorCommandLineModel = true;
                }
            } else {
                status = m_mainWindow->openPath(path, MainWindow::CreateAdditionalModel);
            }
        }
    }
    if (status == MainWindow::FileOpenFailed) {
        if (splash) splash->hide();
        QMessageBox::critical
            (m_mainWindow, QMessageBox::tr("Failed to open file"),
             QMessageBox::tr("File or URL \"%1\" could not be opened").arg(path));
    } else if (status == MainWindow::FileOpenWrongMode) {
        if (splash) splash->hide();
        QMessageBox::critical
            (m_mainWindow, QMessageBox::tr("Failed to open file"),
             QMessageBox::tr("<b>Audio required</b><p>Please load at least one audio file before importing annotation data"));
    }
}
