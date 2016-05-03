/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <http://www.natron.fr/>,
 * Copyright (C) 2016 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AppInstance.h"

#include <fstream>
#include <list>
#include <cassert>
#include <stdexcept>

#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5
#include <QtCore/QUrl>
#include <QtCore/QFileInfo>
#include <QtCore/QEventLoop>
#include <QtCore/QSettings>

#if !defined(SBK_RUN) && !defined(Q_MOC_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// /usr/local/include/boost/bind/arg.hpp:37:9: warning: unused typedef 'boost_static_assert_typedef_37' [-Wunused-local-typedef]
#include <boost/bind.hpp>
#include <boost/algorithm/string/predicate.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
#endif

#include "Global/QtCompat.h" // removeFileExtension

#include "Engine/BlockingBackgroundRender.h"
#include "Engine/CLArgs.h"
#include "Engine/FileDownloader.h"
#include "Engine/GroupOutput.h"
#include "Engine/KnobTypes.h"
#include "Engine/DiskCacheNode.h"
#include "Engine/Node.h"
#include "Engine/NodeSerialization.h"
#include "Engine/OfxHost.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/ProcessHandler.h"
#include "Engine/ReadNode.h"
#include "Engine/RotoLayer.h"
#include "Engine/Settings.h"
#include "Engine/ViewerInstance.h"
#include "Engine/WriteNode.h"

NATRON_NAMESPACE_ENTER;

FlagSetter::FlagSetter(bool initialValue,
                       bool* p)
    : p(p)
    , lock(0)
{
    *p = initialValue;
}

FlagSetter::FlagSetter(bool initialValue,
                       bool* p,
                       QMutex* mutex)
    : p(p)
    , lock(mutex)
{
    lock->lock();
    *p = initialValue;
    lock->unlock();
}

FlagSetter::~FlagSetter()
{
    if (lock) {
        lock->lock();
    }
    *p = !*p;
    if (lock) {
        lock->unlock();
    }
}

FlagIncrementer::FlagIncrementer(int* p)
    : p(p)
    , lock(0)
{
    *p = *p + 1;
}

FlagIncrementer::FlagIncrementer(int* p,
                                 QMutex* mutex)
    : p(p)
    , lock(mutex)
{
    lock->lock();
    *p = *p + 1;
    lock->unlock();
}

FlagIncrementer::~FlagIncrementer()
{
    if (lock) {
        lock->lock();
    }
    *p = *p - 1;
    if (lock) {
        lock->unlock();
    }
}

struct RenderQueueItem
{
    AppInstance::RenderWork work;
    QString sequenceName;
    QString savePath;
    boost::shared_ptr<ProcessHandler> process;
};

struct AppInstancePrivate
{
    AppInstance* _publicInterface;
    boost::shared_ptr<Project> _currentProject; //< ptr to the project
    int _appID; //< the unique ID of this instance (or window)
    bool _projectCreatedWithLowerCaseIDs;
    mutable QMutex creatingGroupMutex;

    //When a pyplug is created
    int _creatingGroup;

    //When a node is created, it gets appended to this list (since for a PyPlug more than 1 node can be created)
    std::list<NodePtr> _creatingNodeQueue;

    //When a node tree is created
    int _creatingTree;
    mutable QMutex renderQueueMutex;
    std::list<RenderQueueItem> renderQueue, activeRenders;
    mutable QMutex invalidExprKnobsMutex;
    std::list<KnobWPtr> invalidExprKnobs;

    AppInstancePrivate(int appID,
                       AppInstance* app)

        : _publicInterface(app)
        , _currentProject( new Project(app) )
        , _appID(appID)
        , _projectCreatedWithLowerCaseIDs(false)
        , creatingGroupMutex()
        , _creatingGroup(0)
        , _creatingNodeQueue()
        , _creatingTree(0)
        , renderQueueMutex()
        , renderQueue()
        , activeRenders()
        , invalidExprKnobsMutex()
        , invalidExprKnobs()
    {
    }

    void declareCurrentAppVariable_Python();


    void executeCommandLinePythonCommands(const CLArgs& args);

    bool validateRenderOptions(const AppInstance::RenderWork& w,
                               int* firstFrame,
                               int* lastFrame,
                               int* frameStep);

    void getSequenceNameFromWriter(const OutputEffectInstance* writer, QString* sequenceName);

    void startRenderingFullSequence(bool blocking, const RenderQueueItem& writerWork);
};

AppInstance::AppInstance(int appID)
    : QObject()
    , _imp( new AppInstancePrivate(appID, this) )
{
    appPTR->registerAppInstance(this);
    appPTR->setAsTopLevelInstance(appID);


    ///initialize the knobs of the project before loading anything else.
    _imp->_currentProject->initializeKnobsPublic();
}

AppInstance::~AppInstance()
{
    appPTR->removeInstance(_imp->_appID);
    _imp->_currentProject->clearNodes(false);
}

const std::list<NodePtr>&
AppInstance::getNodesBeingCreated() const
{
    assert( QThread::currentThread() == qApp->thread() );

    return _imp->_creatingNodeQueue;
}

bool
AppInstance::isCreatingNodeTree() const
{
    QMutexLocker k(&_imp->creatingGroupMutex);

    return _imp->_creatingTree;
}

void
AppInstance::setIsCreatingNodeTree(bool b)
{
    QMutexLocker k(&_imp->creatingGroupMutex);

    if (b) {
        ++_imp->_creatingTree;
    } else {
        if (_imp->_creatingTree >= 1) {
            --_imp->_creatingTree;
        } else {
            _imp->_creatingTree = 0;
        }
    }
}

void
AppInstance::checkForNewVersion() const
{
    FileDownloader* downloader = new FileDownloader( QUrl( QString::fromUtf8(NATRON_LAST_VERSION_URL) ), false );
    QObject::connect( downloader, SIGNAL(downloaded()), this, SLOT(newVersionCheckDownloaded()) );
    QObject::connect( downloader, SIGNAL(error()), this, SLOT(newVersionCheckError()) );

    ///make the call blocking
    QEventLoop loop;

    connect( downloader->getReply(), SIGNAL(finished()), &loop, SLOT(quit()) );
    loop.exec();
}

//return -1 if a < b, 0 if a == b and 1 if a > b
//Returns -2 if not understood
static
int
compareDevStatus(const QString& a,
                 const QString& b)
{
    if ( ( a == QString::fromUtf8(NATRON_DEVELOPMENT_DEVEL) ) || ( a == QString::fromUtf8(NATRON_DEVELOPMENT_SNAPSHOT) ) ) {
        //Do not try updates when update available is a dev build
        return -1;
    } else if ( ( b == QString::fromUtf8(NATRON_DEVELOPMENT_DEVEL) ) || ( b == QString::fromUtf8(NATRON_DEVELOPMENT_SNAPSHOT) ) ) {
        //This is a dev build, do not try updates
        return -1;
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_ALPHA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_BETA) ) {
            return 1;
        } else if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) {
            return 0;
        } else {
            return -1;
        }
    } else if ( a == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_STABLE) ) {
        if ( b == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_STABLE) ) {
            return 0;
        } else {
            return 1;
        }
    }
    assert(false);

    return -2;
}

void
AppInstance::newVersionCheckDownloaded()
{
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );

    assert(downloader);

    QString extractedFileVersionStr, extractedSoftwareVersionStr, extractedDevStatusStr, extractedBuildNumberStr;
    QString fileVersionTag( QString::fromUtf8("File version: ") );
    QString softwareVersionTag( QString::fromUtf8("Software version: ") );
    QString devStatusTag( QString::fromUtf8("Development status: ") );
    QString buildNumberTag( QString::fromUtf8("Build number: ") );
    QString data( QString::fromUtf8( downloader->downloadedData() ) );
    QTextStream ts(&data);

    while ( !ts.atEnd() ) {
        QString line = ts.readLine();
        if ( line.startsWith( QChar::fromLatin1('#') ) || line.startsWith( QChar::fromLatin1('\n') ) ) {
            continue;
        }

        if ( line.startsWith(fileVersionTag) ) {
            int i = fileVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedFileVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(softwareVersionTag) ) {
            int i = softwareVersionTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedSoftwareVersionStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(devStatusTag) ) {
            int i = devStatusTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedDevStatusStr.push_back( line.at(i) );
                ++i;
            }
        } else if ( line.startsWith(buildNumberTag) ) {
            int i = buildNumberTag.size();
            while ( i < line.size() && !line.at(i).isSpace() ) {
                extractedBuildNumberStr.push_back( line.at(i) );
                ++i;
            }
        }
    }

    downloader->deleteLater();


    if ( extractedFileVersionStr.isEmpty() || (extractedFileVersionStr.toInt() < NATRON_LAST_VERSION_FILE_VERSION) ) {
        //The file cannot be decoded here
        return;
    }


    QStringList versionDigits = extractedSoftwareVersionStr.split( QChar::fromLatin1('.') );

    ///we only understand 3 digits formed version numbers
    if (versionDigits.size() != 3) {
        return;
    }


    int buildNumber = extractedBuildNumberStr.toInt();
    int major = versionDigits[0].toInt();
    int minor = versionDigits[1].toInt();
    int revision = versionDigits[2].toInt();
    const QString currentDevStatus = QString::fromUtf8(NATRON_DEVELOPMENT_STATUS);
    int devStatCompare = compareDevStatus(extractedDevStatusStr, currentDevStatus);
    int versionEncoded = NATRON_VERSION_ENCODE(major, minor, revision);
    if ( (versionEncoded > NATRON_VERSION_ENCODED) ||
         ( ( versionEncoded == NATRON_VERSION_ENCODED) &&
           ( ( devStatCompare > 0) || ( ( devStatCompare == 0) && ( buildNumber > NATRON_BUILD_NUMBER) ) ) ) ) {
        QString text;
        if (devStatCompare == 0) {
            if ( ( buildNumber > NATRON_BUILD_NUMBER) && ( versionEncoded == NATRON_VERSION_ENCODED) &&
                 ( currentDevStatus == QString::fromUtf8(NATRON_DEVELOPMENT_RELEASE_CANDIDATE) ) ) {
                ///show build number in version
                text =  QObject::tr("<p>Updates for %1 are now available for download. "
                                    "You are currently using %1 version %2 - %3 - build %4. "
                                    "The latest version of %1 is version %5 - %6 - build %7.</p> ")
                       .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
                       .arg( QString::fromUtf8(NATRON_VERSION_STRING) )
                       .arg( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) )
                       .arg(NATRON_BUILD_NUMBER)
                       .arg(extractedSoftwareVersionStr)
                       .arg(extractedDevStatusStr)
                       .arg(extractedBuildNumberStr) +
                       QObject::tr("<p>You can download it from ") + QString::fromUtf8("<a href=\"www.natron.fr/download\">"
                                                                                       "www.natron.fr</a>. </p>");
            } else {
                //Only notify build number increments for Release candidates
                return;
            }
        } else {
            text =  QObject::tr("<p>Updates for %1 are now available for download. "
                                "You are currently using %1 version %2 - %3. "
                                "The latest version of %1 is version %4 - %5.</p> ")
                   .arg( QString::fromUtf8(NATRON_APPLICATION_NAME) )
                   .arg( QString::fromUtf8(NATRON_VERSION_STRING) )
                   .arg( QString::fromUtf8(NATRON_DEVELOPMENT_STATUS) )
                   .arg(extractedSoftwareVersionStr)
                   .arg(extractedDevStatusStr) +
                   QObject::tr("<p>You can download it from ") + QString::fromUtf8("<a href=\"www.natron.fr/download\">"
                                                                                   "www.natron.fr</a>. </p>");
        }

        Dialogs::informationDialog( "New version", text.toStdString(), true );
    }
} // AppInstance::newVersionCheckDownloaded

void
AppInstance::newVersionCheckError()
{
    ///Nothing to do,
    FileDownloader* downloader = qobject_cast<FileDownloader*>( sender() );

    assert(downloader);
    downloader->deleteLater();
}

void
AppInstance::getWritersWorkForCL(const CLArgs& cl,
                                 std::list<AppInstance::RenderWork>& requests)
{
    const std::list<CLArgs::WriterArg>& writers = cl.getWriterArgs();

    for (std::list<CLArgs::WriterArg>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        NodePtr writerNode;
        if (!it->mustCreate) {
            std::string writerName = it->name.toStdString();
            writerNode = getNodeByFullySpecifiedName(writerName);

            if (!writerNode) {
                std::string exc(writerName);
                exc.append( tr(" does not belong to the project file. Please enter a valid Write node script-name.").toStdString() );
                throw std::invalid_argument(exc);
            } else {
                if ( !writerNode->isOutputNode() ) {
                    std::string exc(writerName);
                    exc.append( tr(" is not an output node! It cannot render anything.").toStdString() );
                    throw std::invalid_argument(exc);
                }
            }

            if ( !it->filename.isEmpty() ) {
                KnobPtr fileKnob = writerNode->getKnobByName(kOfxImageEffectFileParamName);
                if (fileKnob) {
                    KnobOutputFile* outFile = dynamic_cast<KnobOutputFile*>( fileKnob.get() );
                    if (outFile) {
                        outFile->setValue( it->filename.toStdString() );
                    }
                }
            }
        } else {
            writerNode = createWriter( it->filename.toStdString(), eCreateNodeReasonInternal, getProject() );
            if (!writerNode) {
                throw std::runtime_error( std::string("Failed to create writer for ") + it->filename.toStdString() );
            }

            //Connect the writer to the corresponding Output node input
            NodePtr output = getProject()->getNodeByFullySpecifiedName( it->name.toStdString() );
            if (!output) {
                throw std::invalid_argument( it->name.toStdString() + tr(" is not the name of a valid Output node of the script").toStdString() );
            }
            GroupOutput* isGrpOutput = dynamic_cast<GroupOutput*>( output->getEffectInstance().get() );
            if (!isGrpOutput) {
                throw std::invalid_argument( it->name.toStdString() + tr(" is not the name of a valid Output node of the script").toStdString() );
            }
            NodePtr outputInput = output->getRealInput(0);
            if (outputInput) {
                writerNode->connectInput(outputInput, 0);
            }
        }

        assert(writerNode);
        OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>( writerNode->getEffectInstance().get() );

        if ( cl.hasFrameRange() ) {
            const std::list<std::pair<int, std::pair<int, int> > >& frameRanges = cl.getFrameRanges();
            for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it2 = frameRanges.begin(); it2 != frameRanges.end(); ++it2) {
                AppInstance::RenderWork r( effect, it2->second.first, it2->second.second, it2->first, cl.areRenderStatsEnabled() );
                requests.push_back(r);
            }
        } else {
            AppInstance::RenderWork r( effect, INT_MIN, INT_MAX, INT_MIN, cl.areRenderStatsEnabled() );
            requests.push_back(r);
        }
    }
} // AppInstance::getWritersWorkForCL

void
AppInstancePrivate::executeCommandLinePythonCommands(const CLArgs& args)
{
    const std::list<std::string>& commands = args.getPythonCommands();

    for (std::list<std::string>::const_iterator it = commands.begin(); it != commands.end(); ++it) {
        std::string err;
        std::string output;
        bool ok  = NATRON_PYTHON_NAMESPACE::interpretPythonScript(*it, &err, &output);
        if (!ok) {
            QString m = QObject::tr("Failed to execute given command-line Python command: ");
            m.append( QString::fromUtf8( it->c_str() ) );
            m.append( QString::fromUtf8(" Error: ") );
            m.append( QString::fromUtf8( err.c_str() ) );
            throw std::runtime_error( m.toStdString() );
        } else if ( !output.empty() ) {
            std::cout << output << std::endl;
        }
    }
}

void
AppInstance::load(const CLArgs& cl,
                  bool makeEmptyInstance)
{
    declareCurrentAppVariable_Python();

    if (makeEmptyInstance) {
        return;
    }

    const QString& extraOnProjectCreatedScript = cl.getDefaultOnProjectLoadedScript();

    _imp->executeCommandLinePythonCommands(cl);

    QString exportDocPath = cl.getExportDocsPath();
    if ( !exportDocPath.isEmpty() ) {
        exportHTMLDocs(exportDocPath);

        return;
    }

    ///if the app is a background project autorun and the project name is empty just throw an exception.
    if ( ( (appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRun) ||
           ( appPTR->getAppType() == AppManager::eAppTypeBackgroundAutoRunLaunchedFromGui) ) ) {
        const QString& scriptFilename =  cl.getScriptFilename();

        if ( scriptFilename.isEmpty() ) {
            // cannot start a background process without a file
            throw std::invalid_argument( tr("Project file name empty").toStdString() );
        }


        QFileInfo info(scriptFilename);
        if ( !info.exists() ) {
            std::stringstream ss;
            ss << scriptFilename.toStdString();
            ss << tr(": No such file").toStdString();
            throw std::invalid_argument( ss.str() );
        }

        std::list<AppInstance::RenderWork> writersWork;


        if ( info.suffix() == QString::fromUtf8(NATRON_PROJECT_FILE_EXT) ) {
            ///Load the project
            if ( !_imp->_currentProject->loadProject( info.path(), info.fileName() ) ) {
                throw std::invalid_argument( tr("Project file loading failed.").toStdString() );
            }
        } else if ( info.suffix() == QString::fromUtf8("py") ) {
            ///Load the python script
            loadPythonScript(info);
        } else {
            throw std::invalid_argument( tr(NATRON_APPLICATION_NAME " only accepts python scripts or .ntp project files").toStdString() );
        }


        ///exec the python script specified via --onload
        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }


        getWritersWorkForCL(cl, writersWork);


        ///Set reader parameters if specified from the command-line
        const std::list<CLArgs::ReaderArg>& readerArgs = cl.getReaderArgs();
        for (std::list<CLArgs::ReaderArg>::const_iterator it = readerArgs.begin(); it != readerArgs.end(); ++it) {
            std::string readerName = it->name.toStdString();
            NodePtr readNode = getNodeByFullySpecifiedName(readerName);

            if (!readNode) {
                std::string exc(readerName);
                exc.append( tr(" does not belong to the project file. Please enter a valid Read node script-name.").toStdString() );
                throw std::invalid_argument(exc);
            } else {
                if ( !readNode->getEffectInstance()->isReader() ) {
                    std::string exc(readerName);
                    exc.append( tr(" is not a Read node! It cannot render anything.").toStdString() );
                    throw std::invalid_argument(exc);
                }
            }

            if ( it->filename.isEmpty() ) {
                std::string exc(readerName);
                exc.append( tr(": Filename specified is empty but [-i] or [--reader] was passed to the command-line").toStdString() );
                throw std::invalid_argument(exc);
            }
            KnobPtr fileKnob = readNode->getKnobByName(kOfxImageEffectFileParamName);
            if (fileKnob) {
                KnobFile* outFile = dynamic_cast<KnobFile*>( fileKnob.get() );
                if (outFile) {
                    outFile->setValue( it->filename.toStdString() );
                }
            }
        }

        ///launch renders
        if ( !writersWork.empty() ) {
            startWritersRendering(false, writersWork);
        } else {
            std::list<std::string> writers;
            startWritersRenderingFromNames( cl.areRenderStatsEnabled(), false, writers, cl.getFrameRanges() );
        }
    } else if (appPTR->getAppType() == AppManager::eAppTypeInterpreter) {
        QFileInfo info( cl.getScriptFilename() );
        if ( info.exists() ) {
            if ( info.suffix() == QString::fromUtf8("py") ) {
                loadPythonScript(info);
            } else if ( info.suffix() == QString::fromUtf8(NATRON_PROJECT_FILE_EXT) ) {
                if ( !_imp->_currentProject->loadProject( info.path(), info.fileName() ) ) {
                    throw std::invalid_argument( tr("Project file loading failed.").toStdString() );
                }
            }
        }

        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }


        appPTR->launchPythonInterpreter();
    } else {
        execOnProjectCreatedCallback();

        if ( !extraOnProjectCreatedScript.isEmpty() ) {
            QFileInfo cbInfo(extraOnProjectCreatedScript);
            if ( cbInfo.exists() ) {
                loadPythonScript(cbInfo);
            }
        }
    }
} // AppInstance::load

bool
AppInstance::loadPythonScript(const QFileInfo& file)
{
    std::string addToPythonPath("sys.path.append(\"");

    addToPythonPath += file.path().toStdString();
    addToPythonPath += "\")\n";

    std::string err;
    bool ok  = NATRON_PYTHON_NAMESPACE::interpretPythonScript(addToPythonPath, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("AppInstance::loadPythonScript(" + file.path().toStdString() + "): interpretPythonScript(" + addToPythonPath + " failed!");
    }

    std::string s = "app = app1\n";
    ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(s, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("AppInstance::loadPythonScript(" + file.path().toStdString() + "): interpretPythonScript(" + s + " failed!");
    }

    QFile f( file.absoluteFilePath() );
    if ( !f.open(QIODevice::ReadOnly) ) {
        return false;
    }
    QTextStream ts(&f);
    QString content = ts.readAll();
    bool hasCreateInstance = content.contains( QString::fromUtf8("def createInstance") );
    /*
       The old way of doing it was

        QString hasCreateInstanceScript = QString("import sys\n"
        "import %1\n"
        "ret = True\n"
        "if not hasattr(%1,\"createInstance\") or not hasattr(%1.createInstance,\"__call__\"):\n"
        "    ret = False\n").arg(filename);


        ok = interpretPythonScript(hasCreateInstanceScript.toStdString(), &err, 0);


       which is wrong because it will try to import the script first.
       But we in the case of regular scripts, we allow the user to access externally declared variables such as "app", "app1" etc...
       and this would not be possible if the script was imported. Importing the module would then fail because it could not
       find the variables and the script could not be executed.
     */

    if (hasCreateInstance) {
        QString moduleName = file.fileName();
        int lastDotPos = moduleName.lastIndexOf( QChar::fromLatin1('.') );
        if (lastDotPos != -1) {
            moduleName = moduleName.left(lastDotPos);
        }

        std::stringstream ss;
        ss << "import " << moduleName.toStdString() << '\n';
        ss << moduleName.toStdString() << ".createInstance(app,app)";

        std::string output;
        FlagIncrementer flag(&_imp->_creatingGroup, &_imp->creatingGroupMutex);
        CreatingNodeTreeFlag_RAII createNodeTree(this);
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(ss.str(), &err, &output) ) {
            if ( !err.empty() ) {
                Dialogs::errorDialog(tr("Python").toStdString(), err);
            }

            return false;
        } else {
            if ( !output.empty() ) {
                if ( appPTR->isBackground() ) {
                    std::cout << output << std::endl;
                } else {
                    appendToScriptEditor(output);
                }
            }
        }

        getProject()->forceComputeInputDependentDataOnAllTrees();
    } else {
        QFile f( file.absoluteFilePath() );
        PyRun_SimpleString( content.toStdString().c_str() );

        PyObject* mainModule = NATRON_PYTHON_NAMESPACE::getMainModule();
        std::string error;
        ///Gui session, do stdout, stderr redirection
        PyObject *errCatcher = 0;

        if ( PyObject_HasAttrString(mainModule, "catchErr") ) {
            errCatcher = PyObject_GetAttrString(mainModule, "catchErr"); //get our catchOutErr created above, new ref
        }

        PyErr_Print(); //make python print any errors

        PyObject *errorObj = 0;
        if (errCatcher) {
            errorObj = PyObject_GetAttrString(errCatcher, "value"); //get the  stderr from our catchErr object, new ref
            assert(errorObj);
            error = NATRON_PYTHON_NAMESPACE::PyStringToStdString(errorObj);
            PyObject* unicode = PyUnicode_FromString("");
            PyObject_SetAttrString(errCatcher, "value", unicode);
            Py_DECREF(errorObj);
            Py_DECREF(errCatcher);
        }

        if ( !error.empty() ) {
            QString message( QString::fromUtf8("Failed to load ") );
            message.append( file.absoluteFilePath() );
            message.append( QString::fromUtf8(": ") );
            message.append( QString::fromUtf8( error.c_str() ) );
            appendToScriptEditor( message.toStdString() );
        }
    }

    return true;
} // AppInstance::loadPythonScript

class AddCreateNode_RAII
{
    AppInstancePrivate* _imp;
    NodePtr _node;

public:


    AddCreateNode_RAII(AppInstancePrivate* imp,
                       const NodePtr& node)
        : _imp(imp)
        , _node(node)
    {
        _imp->_creatingNodeQueue.push_back(node);
    }

    virtual ~AddCreateNode_RAII()
    {
        std::list<NodePtr>::iterator found = std::find(_imp->_creatingNodeQueue.begin(), _imp->_creatingNodeQueue.end(), _node);

        if ( found != _imp->_creatingNodeQueue.end() ) {
            _imp->_creatingNodeQueue.erase(found);
        }
    }
};

NodePtr
AppInstance::createNodeFromPythonModule(Plugin* plugin,
                                        const boost::shared_ptr<NodeCollection>& group,
                                        CreateNodeReason reason,
                                        const boost::shared_ptr<NodeSerialization>& serialization)

{
    /*If the plug-in is a toolset, execute the toolset script and don't actually create a node*/
    bool istoolsetScript = plugin->getToolsetScript();
    NodePtr node;

    {
        FlagIncrementer fs(&_imp->_creatingGroup, &_imp->creatingGroupMutex);
        CreatingNodeTreeFlag_RAII createNodeTree(this);
        NodePtr containerNode;
        if (!istoolsetScript) {
            CreateNodeArgs groupArgs(QString::fromUtf8(PLUGINID_NATRON_GROUP), reason, group);
            groupArgs.serialization = serialization;
            containerNode = createNode(groupArgs);
            if (!containerNode) {
                return containerNode;
            }

            if ( (reason == eCreateNodeReasonUserCreate) || (reason == eCreateNodeReasonInternal) ) {
                std::string containerName;
                try {
                    group->initNodeName(plugin->getLabelWithoutSuffix().toStdString(), &containerName);
                    containerNode->setScriptName(containerName);
                    containerNode->setLabel(containerName);
                } catch (...) {
                }
            }
        }

        AddCreateNode_RAII creatingNode_raii(_imp.get(), containerNode);
        std::string containerFullySpecifiedName;
        if (containerNode) {
            containerFullySpecifiedName = containerNode->getFullyQualifiedName();
        }


        QString moduleName;
        QString modulePath;
        plugin->getPythonModuleNameAndPath(&moduleName, &modulePath);

        int appID = getAppID() + 1;
        std::stringstream ss;
        ss << moduleName.toStdString();
        ss << ".createInstance(app" << appID;
        if (istoolsetScript) {
            ss << ",\"\"";
        } else {
            ss << ", app" << appID << "." << containerFullySpecifiedName;
        }
        ss << ")\n";
        std::string err;
        std::string output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(ss.str(), &err, &output) ) {
            Dialogs::errorDialog(tr("Group plugin creation error").toStdString(), err);
            if (containerNode) {
                containerNode->destroyNode(false);
            }

            return node;
        } else {
            if ( !output.empty() ) {
                appendToScriptEditor(output);
            }
            node = containerNode;
        }
        if (istoolsetScript) {
            return NodePtr();
        }

        if ( !moduleName.isEmpty() ) {
            setGroupLabelIDAndVersion(node, modulePath, moduleName);
        }

        // If there's a serialization, restore the serialization of the group node because the Python script probably overriden any state
        if (serialization) {
            containerNode->loadKnobs(*serialization);
        }
    } //FlagSetter fs(true,&_imp->_creatingGroup,&_imp->creatingGroupMutex);

    ///Now that the group is created and all nodes loaded, autoconnect the group like other nodes.
    onGroupCreationFinished(node, reason);

    return node;
} // AppInstance::createNodeFromPythonModule

void
AppInstance::setGroupLabelIDAndVersion(const NodePtr& node,
                                       const QString& pythonModulePath,
                                       const QString &pythonModule)
{
    std::string pluginID, pluginLabel, iconFilePath, pluginGrouping, description;
    unsigned int version;
    bool istoolset;

    if ( NATRON_PYTHON_NAMESPACE::getGroupInfos(pythonModulePath.toStdString(), pythonModule.toStdString(), &pluginID, &pluginLabel, &iconFilePath, &pluginGrouping, &description, &istoolset, &version) ) {
        node->setPluginIconFilePath(iconFilePath);
        node->setPluginDescription(description);

        QString groupingStr = QString::fromUtf8( pluginGrouping.c_str() );
        QStringList groupingSplits = groupingStr.split( QLatin1Char('/') );
        std::list<std::string> stdGrouping;
        for (QStringList::iterator it = groupingSplits.begin(); it != groupingSplits.end(); ++it) {
            stdGrouping.push_back( it->toStdString() );
        }

        node->setPluginIDAndVersionForGui(stdGrouping, pluginLabel, pluginID, version);
        node->setPluginPythonModule( QString( pythonModulePath + pythonModule + QString::fromUtf8(".py") ).toStdString() );
    }
}

NodePtr
AppInstance::createReader(const std::string& filename,
                          CreateNodeReason reason,
                          const boost::shared_ptr<NodeCollection>& group)
{
    std::string pluginID;

#ifdef NATRON_ENABLE_IO_META_NODES
    pluginID = PLUGINID_NATRON_READ;
    CreateNodeArgs args(QString::fromUtf8( pluginID.c_str() ),
                        reason,
                        group);
#else

    std::map<std::string, std::string> readersForFormat;
    appPTR->getCurrentSettings()->getFileFormatsForReadingAndReader(&readersForFormat);
    QString fileCpy = QString::fromUtf8( filename.c_str() );
    std::string ext = QtCompat::removeFileExtension(fileCpy).toLower().toStdString();
    std::map<std::string, std::string>::iterator found = readersForFormat.find(ext);
    if ( found == readersForFormat.end() ) {
        Dialogs::errorDialog( tr("Reader").toStdString(),
                              tr("No plugin capable of decoding ").toStdString() + ext + tr(" was found.").toStdString(), false );

        return NodePtr();
    }
    pluginID = found->second;
    CreateNodeArgs args(QString::fromUtf8( found->second.c_str() ), reason, group);
#endif

    args.paramValues.push_back( createDefaultValueForParam(kOfxImageEffectFileParamName, filename) );
    std::string canonicalFilename = filename;
    getProject()->canonicalizePath(canonicalFilename);

    int firstFrame, lastFrame;
    Node::getOriginalFrameRangeForReader(pluginID, canonicalFilename, &firstFrame, &lastFrame);
    args.paramValues.push_back( createDefaultValueForParam(kReaderParamNameOriginalFrameRange, firstFrame, lastFrame) );

    return createNode(args);
}

NodePtr
AppInstance::createWriter(const std::string& filename,
                          CreateNodeReason reason,
                          const boost::shared_ptr<NodeCollection>& collection,
                          int firstFrame,
                          int lastFrame)
{
#ifdef NATRON_ENABLE_IO_META_NODES
    CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_WRITE), reason, collection);
#else
    std::map<std::string, std::string> writersForFormat;
    appPTR->getCurrentSettings()->getFileFormatsForWritingAndWriter(&writersForFormat);

    QString fileCpy = QString::fromUtf8( filename.c_str() );
    std::string ext = QtCompat::removeFileExtension(fileCpy).toLower().toStdString();
    std::map<std::string, std::string>::iterator found = writersForFormat.find(ext);
    if ( found == writersForFormat.end() ) {
        Dialogs::errorDialog( tr("Writer").toStdString(),
                              tr("No plugin capable of encoding ").toStdString() + ext + tr(" was found.").toStdString(), false );

        return NodePtr();
    }


    CreateNodeArgs args(QString::fromUtf8( found->second.c_str() ), reason, collection);
#endif
    args.paramValues.push_back( createDefaultValueForParam<std::string>(kOfxImageEffectFileParamName, filename) );
    if ( (firstFrame != INT_MIN) && (lastFrame != INT_MAX) ) {
        args.paramValues.push_back( createDefaultValueForParam<int>("frameRange", 2) );
        args.paramValues.push_back( createDefaultValueForParam<int>("firstFrame", firstFrame) );
        args.paramValues.push_back( createDefaultValueForParam<int>("lastFrame", lastFrame) );
    }

    return createNode(args);
}

/**
 * @brief An inspector node is like a viewer node with hidden inputs that unfolds one after another.
 * This functions returns the number of inputs to use for inspectors or 0 for a regular node.
 **/
static bool
isEntitledForInspector(Plugin* plugin,
                       OFX::Host::ImageEffect::Descriptor* ofxDesc)
{
    if ( ( plugin->getPluginID() == QString::fromUtf8(PLUGINID_NATRON_VIEWER) ) ||
         ( plugin->getPluginID() == QString::fromUtf8(PLUGINID_NATRON_ROTOPAINT) ) ||
         ( plugin->getPluginID() == QString::fromUtf8(PLUGINID_NATRON_ROTO) ) ) {
        return true;
    }

    if (!ofxDesc) {
        return false;
    }

    const std::vector<OFX::Host::ImageEffect::ClipDescriptor*>& clips = ofxDesc->getClipsByOrder();
    int nInputs = 0;
    bool firstInput = true;
    for (std::vector<OFX::Host::ImageEffect::ClipDescriptor*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if ( !(*it)->isOutput() ) {
            if ( !(*it)->isOptional() ) {
                if (!firstInput) {                    // allow one non-optional input
                    return false;
                }
            } else {
                ++nInputs;
            }
            firstInput = false;
        }
    }
    if (nInputs > 4) {
        return true;
    }

    return false;
}

NodePtr
AppInstance::createNodeInternal(CreateNodeArgs& args)
{
    NodePtr node;
    Plugin* plugin = 0;
    QString findId;

    //Roto has moved to a built-in plugin
    if ( ( (args.reason == eCreateNodeReasonUserCreate) || (args.reason == eCreateNodeReasonInternal) || (args.reason == eCreateNodeReasonProjectLoad) ) &&
         ( ( !_imp->_projectCreatedWithLowerCaseIDs && ( args.pluginID == QString::fromUtf8(PLUGINID_OFX_ROTO) ) ) || ( _imp->_projectCreatedWithLowerCaseIDs && ( args.pluginID == QString::fromUtf8(PLUGINID_OFX_ROTO).toLower() ) ) ) ) {
        findId = QString::fromUtf8(PLUGINID_NATRON_ROTO);
    } else {
        findId = args.pluginID;
    }

#ifdef NATRON_ENABLE_IO_META_NODES
    //If it is a reader or writer, create a ReadNode or WriteNode
    if (!args.ioContainer) {
        if ( ReadNode::isBundledReader( args.pluginID.toStdString(), wasProjectCreatedWithLowerCaseIDs() ) ) {
            args.paramValues.push_back( createDefaultValueForParam( kNatronReadNodeParamDecodingPluginID, args.pluginID.toStdString() ) );
            findId = QString::fromUtf8(PLUGINID_NATRON_READ);
        } else if ( WriteNode::isBundledWriter( args.pluginID.toStdString(), wasProjectCreatedWithLowerCaseIDs() ) ) {
            args.paramValues.push_back( createDefaultValueForParam( kNatronWriteNodeParamEncodingPluginID, args.pluginID.toStdString() ) );
            findId = QString::fromUtf8(PLUGINID_NATRON_WRITE);
        }
    }
#endif

    try {
        plugin = appPTR->getPluginBinary(findId, args.majorV, args.minorV, _imp->_projectCreatedWithLowerCaseIDs && args.reason == eCreateNodeReasonProjectLoad);
    } catch (const std::exception & e1) {
        ///Ok try with the old Ids we had in Natron prior to 1.0
        try {
            plugin = appPTR->getPluginBinaryFromOldID(args.pluginID, args.majorV, args.minorV);
        } catch (const std::exception& e2) {
            Dialogs::errorDialog(tr("Plugin error").toStdString(),
                                 tr("Cannot load plugin executable").toStdString() + ": " + e2.what(), false );

            return node;
        }
    }

    if (!plugin) {
        return node;
    }

    if ( !plugin->getIsUserCreatable() && (args.reason == eCreateNodeReasonUserCreate) ) {
        //The plug-in should not be instantiable by the user
        qDebug() << "Attempt to create" << args.pluginID << "which is not user creatable";

        return node;
    }


    /*
       If the plug-in is a PyPlug create it with createNodeFromPythonModule() except in the following situations:
       - The user speicifed in the Preferences that he/she prefers loading PyPlugs from their serialization in the .ntp
       file rather than load the Python script
       - The user is copy/pasting in which case we don't want to run the Python code which could override modifications
       made by the user on the original node
     */
    const QString& pythonModule = plugin->getPythonModule();
    if ( !pythonModule.isEmpty() ) {
        if (args.reason != eCreateNodeReasonCopyPaste) {
            try {
                return createNodeFromPythonModule(plugin, args.group, args.reason, args.serialization);
            } catch (const std::exception& e) {
                Dialogs::errorDialog(tr("Plugin error").toStdString(),
                                     tr("Cannot create PyPlug:").toStdString() + e.what(), false );

                return node;
            }
        } else {
            plugin = appPTR->getPluginBinary(QString::fromUtf8(PLUGINID_NATRON_GROUP), -1, -1, false);
            assert(plugin);
        }
    }

    std::string foundPluginID = plugin->getPluginID().toStdString();
    ContextEnum ctx;
    OFX::Host::ImageEffect::Descriptor* ofxDesc = plugin->getOfxDesc(&ctx);

    if (!ofxDesc) {
        OFX::Host::ImageEffect::ImageEffectPlugin* ofxPlugin = plugin->getOfxPlugin();
        if (ofxPlugin) {
            try {
                //  Should this method be in AppManager?
                // ofxDesc = appPTR->getPluginContextAndDescribe(ofxPlugin, &ctx);
                ofxDesc = appPTR->getPluginContextAndDescribe(ofxPlugin, &ctx);
            } catch (const std::exception& e) {
                errorDialog(tr("Error while creating node").toStdString(), tr("Failed to create an instance of ").toStdString()
                            + args.pluginID.toStdString() + ": " + e.what(), false);

                return NodePtr();
            }

            assert(ofxDesc);
            plugin->setOfxDesc(ofxDesc, ctx);
        }
    }

    bool useInspector = isEntitledForInspector(plugin, ofxDesc);

    if (!useInspector) {
        node.reset( new Node(this, args.group, plugin) );
    } else {
        node.reset( new InspectorNode(this, args.group, plugin) );
    }

    AddCreateNode_RAII creatingNode_raii(_imp.get(), node);

    {
        ///Furnace plug-ins don't handle using the thread pool
        boost::shared_ptr<Settings> settings = appPTR->getCurrentSettings();
        if ( boost::starts_with(foundPluginID, "uk.co.thefoundry.furnace") &&
             ( settings->useGlobalThreadPool() || ( settings->getNumberOfParallelRenders() != 1) ) ) {
            StandardButtonEnum reply = Dialogs::questionDialog(tr("Warning").toStdString(),
                                                               tr("The settings of the application are currently set to use "
                                                                  "the global thread-pool for rendering effects. The Foundry Furnace "
                                                                  "is known not to work well when this setting is checked. "
                                                                  "Would you like to turn it off ? ").toStdString(), false);
            if (reply == eStandardButtonYes) {
                settings->setUseGlobalThreadPool(false);
                settings->setNumberOfParallelRenders(1);
            }
        }

        ///If this is a stereo plug-in, check that the project has been set for multi-view
        if (args.reason == eCreateNodeReasonUserCreate) {
            const QStringList& grouping = plugin->getGrouping();
            if ( !grouping.isEmpty() && ( grouping[0] == QString::fromUtf8(PLUGIN_GROUP_MULTIVIEW) ) ) {
                int nbViews = getProject()->getProjectViewsCount();
                if (nbViews < 2) {
                    StandardButtonEnum reply = Dialogs::questionDialog(tr("Multi-View").toStdString(),
                                                                       tr("Using a multi-view node requires the project settings to be setup "
                                                                          "for multi-view. Would you like to setup the project for stereo?").toStdString(), false);
                    if (reply == eStandardButtonYes) {
                        getProject()->setupProjectForStereo();
                    }
                }
            }
        }
    }


    //if (args.addToProject) {
    //Add the node to the project before loading it so it is present when the python script that registers a variable of the name
    //of the node works
    if (args.group) {
        args.group->addNode(node);
    }
    //}
    assert(node);
    try {
        node->load(args);
    } catch (const std::exception & e) {
        if (args.group) {
            args.group->removeNode(node);
        }
        std::string error( e.what() );
        if ( !error.empty() ) {
            std::string title("Error while creating node");
            std::string message = title + " " + foundPluginID + ": " + e.what();
            qDebug() << message.c_str();
            errorDialog(title, message, false);
        }

        return NodePtr();
    } catch (...) {
        if (args.group) {
            args.group->removeNode(node);
        }
        std::string title("Error while creating node");
        std::string message = title + " " + foundPluginID;
        qDebug() << message.c_str();
        errorDialog(title, message, false);

        return NodePtr();
    }

    NodePtr multiInstanceParent = node->getParentMultiInstance();

    if (args.createGui) {
        // createNodeGui also sets the filename parameter for reader or writers
        try {
            createNodeGui(node, multiInstanceParent, args);
        } catch (const std::exception& e) {
            node->destroyNode(false);
            std::string title("Error while creating node");
            std::string message = title + " " + foundPluginID + ": " + e.what();
            qDebug() << message.c_str();
            errorDialog(title, message, false);

            return boost::shared_ptr<Node>();
        }
    }

    boost::shared_ptr<NodeGroup> isGrp = boost::dynamic_pointer_cast<NodeGroup>( node->getEffectInstance()->shared_from_this() );

    if (isGrp) {
        if ( (args.reason == eCreateNodeReasonProjectLoad) || (args.reason == eCreateNodeReasonCopyPaste) ) {
            if ( args.serialization && !args.serialization->getPythonModule().empty() ) {
                QString pythonModulePath = QString::fromUtf8( ( args.serialization->getPythonModule().c_str() ) );
                QString moduleName;
                QString modulePath;
                int foundLastSlash = pythonModulePath.lastIndexOf( QChar::fromLatin1('/') );
                if (foundLastSlash != -1) {
                    modulePath = pythonModulePath.mid(0, foundLastSlash + 1);
                    moduleName = pythonModulePath.remove(0, foundLastSlash + 1);
                }
                QtCompat::removeFileExtension(moduleName);
                setGroupLabelIDAndVersion(node, modulePath, moduleName);
            }
            onGroupCreationFinished(node, args.reason);
        } else if ( (args.reason == eCreateNodeReasonUserCreate) && !_imp->_creatingGroup && (isGrp->getPluginID() == PLUGINID_NATRON_GROUP) ) {
            //if the node is a group and we're not loading the project, create one input and one output
            NodePtr input, output;

            {
                CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_OUTPUT), eCreateNodeReasonInternal, isGrp);
                output = createNode(args);
                try {
                    output->setScriptName("Output");
                } catch (...) {
                }

                assert(output);
            }
            {
                CreateNodeArgs args(QString::fromUtf8(PLUGINID_NATRON_INPUT), eCreateNodeReasonInternal, isGrp);
                input = createNode(args);
                assert(input);
            }
            if ( input && output && !output->getInput(0) ) {
                output->connectInput(input, 0);

                double x, y;
                output->getPosition(&x, &y);
                y -= 100;
                input->setPosition(x, y);
            }
            onGroupCreationFinished(node, args.reason);

            ///Now that the group is created and all nodes loaded, autoconnect the group like other nodes.
        }
    }

    return node;
} // createNodeInternal

NodePtr
AppInstance::createNode(CreateNodeArgs & args)
{
    return createNodeInternal(args);
}

int
AppInstance::getAppID() const
{
    return _imp->_appID;
}

void
AppInstance::exportHTMLDocs(const QString path)
{
    if ( !path.isEmpty() ) {
        QStringList categories;
        QVector<QStringList> plugins;

        // Generate a HTML for each plugin
        std::list<std::string> pluginIDs = appPTR->getPluginIDs();
        for (std::list<std::string>::iterator it = pluginIDs.begin(); it != pluginIDs.end(); ++it) {
            QString pluginID = QString::fromUtf8( it->c_str() );
            if ( !pluginID.isEmpty() ) {
                Plugin* plugin = 0;
                QString pluginID = QString::fromUtf8( it->c_str() );
                plugin = appPTR->getPluginBinary(pluginID, -1, -1, false);
                if (plugin) {
                    // blacklist pyplugs and some other nodes due to crash ...
                    if ( !plugin->getIsForInternalUseOnly() && plugin->getPythonModule().isEmpty() && ( pluginID != QString::fromUtf8("fr.inria.built-in.Tracker") ) && ( pluginID != QString::fromUtf8("net.sf.openfx.TimeBufferRead") ) && ( pluginID != QString::fromUtf8("net.sf.openfx.TimeBufferWrite") ) ) {
                        QStringList groups = plugin->getGrouping();
                        categories << groups.at(0);
                        QStringList plugList;
                        plugList << plugin->getGrouping().at(0) << pluginID << plugin->getPluginLabel();
                        plugins << plugList;
                        CreateNodeArgs args( pluginID, eCreateNodeReasonInternal, boost::shared_ptr<NodeCollection>() );
                        args.createGui = false;
                        args.addToProject = false;
                        qDebug() << pluginID;
                        NodePtr node = appPTR->getTopLevelInstance()->createNode(args);
                        if (node) {
                            QString html = node->makeHTMLDocumentation(true);
                            QDir htmlDir(path);
                            if ( !htmlDir.exists() ) {
                                htmlDir.mkdir(path);
                            }
                            QFile htmlFile( path + QString::fromUtf8("/") + pluginID + QString::fromUtf8(".html") );
                            if ( htmlFile.open(QIODevice::Text | QIODevice::WriteOnly) ) {
                                QTextStream out(&htmlFile);
                                out << parseHTMLDoc(html, path, true);
                                htmlFile.close();
                            }
                        }
                    }
                }
            }
        }

        // Generate a HTML for each parent category
        categories.removeDuplicates();
        QString categoriesHeader = QString::fromUtf8("<!DOCTYPE HTML PUBLIC \"-//W3C//DTD HTML 4.01 Transitional//EN\" \"http://www.w3.org/TR/html4/loose.dtd\"><html><head><title>Natron User Guide Menu</title><link rel=\"stylesheet\" href=\"_static/default.css\" type=\"text/css\" /><link rel=\"stylesheet\" href=\"_static/pygments.css\" type=\"text/css\" /><link rel=\"stylesheet\" href=\"_static/style.css\" type=\"text/css\" /><script type=\"text/javascript\" src=\"_static/jquery.js\"></script><script type=\"text/javascript\" src=\"_static/dropdown.js\"></script></head><body>");
        QString categoryBodyEnd = QString::fromUtf8("</ul></div></div></div></div>");
        QString categoriesFooter = QString::fromUtf8("</body></html>");
        for (int i = 0; i < categories.size(); ++i) {
            QString categoriesBodyStart = QString::fromUtf8("<div class=\"document\"><div class=\"documentwrapper\"><div class=\"body\"><h1>") + categories.at(0) + QString::fromUtf8("</h1><p>") + QObject::tr("This manual is intended as a reference for all the parameters within each node in ") + categories.at(0) + QString::fromUtf8(".</p><div class=\"toctree-wrapper compound\"><ul>");
            QString html;
            html.append(categoriesHeader);
            html.append(categoriesBodyStart);
            for (int y = 0; y < plugins.size(); ++y) {
                QStringList currPlugin = plugins.at(y);
                if (currPlugin.size() == 3) {
                    if ( categories.at(i) == currPlugin.at(0) ) {
                        html.append( QString::fromUtf8("<li class=\"toctree-l1\"><a href='") + currPlugin.at(1) + QString::fromUtf8(".html'>") + currPlugin.at(2) + QString::fromUtf8("</a></li>") );
                    }
                }
            }
            html.append(categoryBodyEnd);
            html.append(categoriesFooter);
            QFile htmlFile( path + QString::fromUtf8("/group") + categories.at(i) + QString::fromUtf8(".html") );
            if ( htmlFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate) ) {
                QTextStream out(&htmlFile);
                out << parseHTMLDoc(html, path, true);
                htmlFile.close();
            }
        }

        // Generate prefs
        boost::shared_ptr<Settings> settings = appPTR->getCurrentSettings();
        QString prefsHTML = settings->makeHTMLDocumentation(false, true);
        prefsHTML = parseHTMLDoc(prefsHTML, path, true);
        QFile prefsHTMLFile( path + QString::fromUtf8("/_prefs.html") );
        if ( prefsHTMLFile.open(QIODevice::Text | QIODevice::WriteOnly | QIODevice::Truncate) ) {
            QTextStream out(&prefsHTMLFile);
            out << parseHTMLDoc(prefsHTML, path, false);
            prefsHTMLFile.close();
        }

        // Add menu to existing sphinx html's in path
        QDir htmlDir(path);
        QFileInfoList dirList = htmlDir.entryInfoList();
        for (int x = 0; x < dirList.size(); ++x) {
            QFileInfo sphinxInfo = dirList.at(x);
            if ( sphinxInfo.exists() && ( sphinxInfo.suffix() == QString::fromUtf8("html") ) && ( sphinxInfo.fileName() != QString::fromUtf8("_group.html") ) && ( sphinxInfo.fileName() != QString::fromUtf8("_prefs.html") ) ) {
                QFile sphinxFile( sphinxInfo.absoluteFilePath() );
                QString input;
                if ( sphinxFile.open(QIODevice::ReadOnly | QIODevice::Text) ) {
                    input = QString::fromUtf8( sphinxFile.readAll() );
                    sphinxFile.close();
                }
                if ( input.contains( QString::fromUtf8("http://sphinx.pocoo.org/") ) && !input.contains( QString::fromUtf8("mainMenu") ) ) {
                    if ( sphinxFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text) ) {
                        QTextStream output(&sphinxFile);
                        output << parseHTMLDoc(input, path, false);
                        sphinxFile.close();
                    }
                }
            }
        }
    }
} // AppInstance::exportHTMLDocs

QString
AppInstance::parseHTMLDoc(const QString html,
                          const QString path,
                          bool replaceNewline) const
{
    QString result = html;

    if ( html.contains( QString::fromUtf8("mainMenu") ) ) {
        return result;
    }

    // get static menu from index.html and make a header+menu
    QFile indexFile( path + QString::fromUtf8("/index.html") );
    QString menuHTML;
    menuHTML.append( QString::fromUtf8("<body>\n") );
    menuHTML.append( QString::fromUtf8("<div id=\"header\"><a href=\"/\"><div id=\"logo\"></div></a><div id=\"mainMenu\">\n") );
    menuHTML.append( QString::fromUtf8("<ul>\n") );
    if ( indexFile.exists() ) {
        if ( indexFile.open(QIODevice::ReadOnly | QIODevice::Text) ) {
            QStringList menuResult;
            bool getMenu = false;
            while ( !indexFile.atEnd() ) {
                QString line = QString::fromUtf8( indexFile.readLine() );
                if ( line == QString::fromUtf8("<div class=\"toctree-wrapper compound\">\n") ) {
                    getMenu = true;
                }
                if (getMenu) {
                    menuResult << line;
                }
                if ( line == QString::fromUtf8("</div>\n") ) {
                    getMenu = false;
                }
            }
            if ( !menuResult.isEmpty() ) {
                int menuEnd = menuResult.size() - 2;
                for (int i = 0; i < menuEnd; ++i) {
                    QString tmp = menuResult.at(i);
                    //tmp.replace(QString::fromUtf8("href=\""),QString::fromUtf8("href=\"/"));
                    menuHTML.append(tmp);
                }
            }
            indexFile.close();
            menuHTML.append( QString::fromUtf8("</div>\n</div></div>\n") );
        }
    } else {
        menuHTML.append( QString::fromUtf8("</ul></div></div>") );
    }

    // preferences
    /*boost::shared_ptr<Settings> settings = appPTR->getCurrentSettings();
       QString prefsHTML = settings->makeHTMLDocumentation(true, true);
       menuHTML.append(prefsHTML);*/

    /*/// TODO probably a better way to get categories...
       QStringList groups;
       std::list<std::string> pluginIDs = appPTR->getPluginIDs();
       for (std::list<std::string>::iterator it=pluginIDs.begin(); it != pluginIDs.end(); ++it) {
        Plugin* plugin = 0;
        QString pluginID = QString::fromUtf8(it->c_str());
        plugin = appPTR->getPluginBinary(pluginID,-1,-1,false);
        if (plugin) {
            QStringList groupList = plugin->getGrouping();
            groups << groupList.at(0);
        }
       }
       groups.removeDuplicates();
       QString refHTML;
       refHTML.append(QString::fromUtf8("<li class=\"toctree-l1\"><a href=\"#\">Reference Guide</a>\n"));
       refHTML.append(QString::fromUtf8("<ul>\n"));
       for (int i = 0; i < groups.size(); ++i) {
        refHTML.append(QString::fromUtf8("<li class='toctree-l2'><a href='group")+groups.at(i)+QString::fromUtf8(".html'>")+groups.at(i)+QString::fromUtf8("</a></li>\n"));
       }
       refHTML.append(QString::fromUtf8("\n</ul>\n</li>\n</ul>\n"));*/

    // return result
    //menuHTML.append(refHTML);
    //menuHTML.append(QString::fromUtf8("</div>\n</div>\n"));
    if (replaceNewline) {
        result.replace( QString::fromUtf8("\n"), QString::fromUtf8("</p><p>") );
    }
    result.replace(QString::fromUtf8("<body>"), menuHTML);
    result.replace( QString::fromUtf8("Natron 2.0 documentation"), QString::fromUtf8(NATRON_APPLICATION_NAME) + QString::fromUtf8(" ") + QString::fromUtf8(NATRON_VERSION_STRING) + QString::fromUtf8(" documentation") );

    return result;
} // AppInstance::parseHTMLDoc

NodePtr
AppInstance::getNodeByFullySpecifiedName(const std::string & name) const
{
    return _imp->_currentProject->getNodeByFullySpecifiedName(name);
}

boost::shared_ptr<Project>
AppInstance::getProject() const
{
    return _imp->_currentProject;
}

boost::shared_ptr<TimeLine>
AppInstance::getTimeLine() const
{
    return _imp->_currentProject->getTimeLine();
}

void
AppInstance::errorDialog(const std::string & title,
                         const std::string & message,
                         bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;
}

void
AppInstance::errorDialog(const std::string & title,
                         const std::string & message,
                         bool* stopAsking,
                         bool /*useHtml*/) const
{
    std::cout << "ERROR: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

void
AppInstance::warningDialog(const std::string & title,
                           const std::string & message,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;
}

void
AppInstance::warningDialog(const std::string & title,
                           const std::string & message,
                           bool* stopAsking,
                           bool /*useHtml*/) const
{
    std::cout << "WARNING: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

void
AppInstance::informationDialog(const std::string & title,
                               const std::string & message,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;
}

void
AppInstance::informationDialog(const std::string & title,
                               const std::string & message,
                               bool* stopAsking,
                               bool /*useHtml*/) const
{
    std::cout << "INFO: " << title + ": " << message << std::endl;

    *stopAsking = false;
}

StandardButtonEnum
AppInstance::questionDialog(const std::string & title,
                            const std::string & message,
                            bool /*useHtml*/,
                            StandardButtons /*buttons*/,
                            StandardButtonEnum /*defaultButton*/) const
{
    std::cout << "QUESTION: " << title + ": " << message << std::endl;

    return eStandardButtonYes;
}

void
AppInstance::triggerAutoSave()
{
    _imp->_currentProject->triggerAutoSave();
}

void
AppInstance::startWritersRenderingFromNames(bool enableRenderStats,
                                            bool doBlockingRender,
                                            const std::list<std::string>& writers,
                                            const std::list<std::pair<int, std::pair<int, int> > >& frameRanges)
{
    std::list<RenderWork> renderers;

    if ( !writers.empty() ) {
        for (std::list<std::string>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
            const std::string& writerName = *it;
            NodePtr node = getNodeByFullySpecifiedName(writerName);

            if (!node) {
                std::string exc(writerName);
                exc.append( tr(" does not belong to the project file. Please enter a valid Write node script-name.").toStdString() );
                throw std::invalid_argument(exc);
            } else {
                if ( !node->isOutputNode() ) {
                    std::string exc(writerName);
                    exc.append( tr(" is not an output node! It cannot render anything.").toStdString() );
                    throw std::invalid_argument(exc);
                }
                ViewerInstance* isViewer = node->isEffectViewer();
                if (isViewer) {
                    throw std::invalid_argument("Internal issue with the project loader...viewers should have been evicted from the project.");
                }

                OutputEffectInstance* effect = dynamic_cast<OutputEffectInstance*>( node->getEffectInstance().get() );
                assert(effect);

                for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it2 = frameRanges.begin(); it2 != frameRanges.end(); ++it2) {
                    RenderWork w(effect, it2->second.first, it2->second.second, it2->first, enableRenderStats);
                    renderers.push_back(w);
                }

                if ( frameRanges.empty() ) {
                    RenderWork r(effect, INT_MIN, INT_MAX, INT_MIN, enableRenderStats);
                    renderers.push_back(r);
                }
            }
        }
    } else {
        //start rendering for all writers found in the project
        std::list<OutputEffectInstance*> writers;
        getProject()->getWriters(&writers);

        for (std::list<OutputEffectInstance*>::const_iterator it2 = writers.begin(); it2 != writers.end(); ++it2) {
            assert(*it2);
            if (*it2) {
                for (std::list<std::pair<int, std::pair<int, int> > >::const_iterator it3 = frameRanges.begin(); it3 != frameRanges.end(); ++it3) {
                    RenderWork w(*it2, it3->second.first, it3->second.second, it3->first, enableRenderStats);
                    renderers.push_back(w);
                }

                if ( frameRanges.empty() ) {
                    RenderWork r(*it2, INT_MIN, INT_MAX, INT_MIN, enableRenderStats);
                    renderers.push_back(r);
                }
            }
        }
    }


    if ( renderers.empty() ) {
        throw std::invalid_argument("Project file is missing a writer node. This project cannot render anything.");
    }

    startWritersRendering(doBlockingRender, renderers);
} // AppInstance::startWritersRenderingFromNames

void
AppInstance::startWritersRendering(bool doBlockingRender,
                                   const std::list<RenderWork>& writers)
{
    if ( writers.empty() ) {
        return;
    }


    bool renderInSeparateProcess = appPTR->getCurrentSettings()->isRenderInSeparatedProcessEnabled();
    QString savePath;
    if (renderInSeparateProcess) {
        getProject()->saveProject_imp(QString(), QString::fromUtf8("RENDER_SAVE.ntp"), true, false, &savePath);
    }

    std::list<RenderQueueItem> itemsToQueue;
    for (std::list<RenderWork>::const_iterator it = writers.begin(); it != writers.end(); ++it) {
        RenderQueueItem item;
        item.work = *it;
        if ( !_imp->validateRenderOptions(item.work, &item.work.firstFrame, &item.work.lastFrame, &item.work.frameStep) ) {
            continue;;
        }
        _imp->getSequenceNameFromWriter(it->writer, &item.sequenceName);
        item.savePath = savePath;

        if (renderInSeparateProcess) {
            item.process.reset( new ProcessHandler(savePath, item.work.writer) );
            QObject::connect( item.process.get(), SIGNAL(processFinished(int)), this, SLOT(onBackgroundRenderProcessFinished()) );
        } else {
            QObject::connect(item.work.writer->getRenderEngine(), SIGNAL(renderFinished(int)), this, SLOT(onQueuedRenderFinished(int)), Qt::UniqueConnection);
        }

        bool canPause = !item.work.writer->isVideoWriter();

        if (!it->isRestart) {
            notifyRenderStarted(item.sequenceName, item.work.firstFrame, item.work.lastFrame, item.work.frameStep, canPause, item.work.writer, item.process);
        } else {
            notifyRenderRestarted(item.work.writer, item.process);
        }
        itemsToQueue.push_back(item);
    }
    if ( itemsToQueue.empty() ) {
        return;
    }

    if (appPTR->isBackground() || doBlockingRender) {
        //blocking call, we don't want this function to return pre-maturely, in which case it would kill the app
        QtConcurrent::blockingMap( itemsToQueue, boost::bind(&AppInstancePrivate::startRenderingFullSequence, _imp.get(), true, _1) );
    } else {
        bool isQueuingEnabled = appPTR->getCurrentSettings()->isRenderQueuingEnabled();
        if (isQueuingEnabled) {
            QMutexLocker k(&_imp->renderQueueMutex);
            if ( !_imp->activeRenders.empty() ) {
                _imp->renderQueue.insert( _imp->renderQueue.end(), itemsToQueue.begin(), itemsToQueue.end() );

                return;
            } else {
                std::list<RenderQueueItem>::const_iterator it = itemsToQueue.begin();
                const RenderQueueItem& firstWork = *it;
                ++it;
                for (; it != itemsToQueue.end(); ++it) {
                    _imp->renderQueue.push_back(*it);
                }
                k.unlock();
                _imp->startRenderingFullSequence(false, firstWork);
            }
        } else {
            for (std::list<RenderQueueItem>::const_iterator it = itemsToQueue.begin(); it != itemsToQueue.end(); ++it) {
                _imp->startRenderingFullSequence(false, *it);
            }
        }
    }
} // AppInstance::startWritersRendering

void
AppInstancePrivate::getSequenceNameFromWriter(const OutputEffectInstance* writer,
                                              QString* sequenceName)
{
    ///get the output file knob to get the name of the sequence
    const DiskCacheNode* isDiskCache = dynamic_cast<const DiskCacheNode*>(writer);

    if (isDiskCache) {
        *sequenceName = QObject::tr("Caching");
    } else {
        *sequenceName = QString();
        KnobPtr fileKnob = writer->getKnobByName(kOfxImageEffectFileParamName);
        if (fileKnob) {
            Knob<std::string>* isString = dynamic_cast<Knob<std::string>*>( fileKnob.get() );
            assert(isString);
            if (isString) {
                *sequenceName = QString::fromUtf8( isString->getValue().c_str() );
            }
        }
    }
}

bool
AppInstancePrivate::validateRenderOptions(const AppInstance::RenderWork& w,
                                          int* firstFrame,
                                          int* lastFrame,
                                          int* frameStep)
{
    ///validate the frame range to render
    if ( (w.firstFrame == INT_MIN) || (w.lastFrame == INT_MAX) ) {
        double firstFrameD, lastFrameD;
        w.writer->getFrameRange_public(w.writer->getHash(), &firstFrameD, &lastFrameD, true);
        if ( (firstFrameD == INT_MIN) || (lastFrameD == INT_MAX) ) {
            _publicInterface->getFrameRange(&firstFrameD, &lastFrameD);
        }

        if (firstFrameD > lastFrameD) {
            Dialogs::errorDialog(w.writer->getNode()->getLabel_mt_safe(),
                                 QObject::tr("First frame in the sequence is greater than the last frame").toStdString(), false );

            return false;
        }
        *firstFrame = (int)firstFrameD;
        *lastFrame = (int)lastFrameD;
    } else {
        *firstFrame = w.firstFrame;
        *lastFrame = w.lastFrame;
    }

    if ( (w.frameStep == INT_MAX) || (w.frameStep == INT_MIN) ) {
        ///Get the frame step from the frame step parameter of the Writer
        *frameStep = w.writer->getNode()->getFrameStepKnobValue();
    } else {
        *frameStep = std::max(1, w.frameStep);
    }

    return true;
}

void
AppInstancePrivate::startRenderingFullSequence(bool blocking,
                                               const RenderQueueItem& w)
{
    if (blocking) {
        BlockingBackgroundRender backgroundRender(w.work.writer);
        backgroundRender.blockingRender(w.work.useRenderStats, w.work.firstFrame, w.work.lastFrame, w.work.frameStep); //< doesn't return before rendering is finished
        return;
    }


    {
        QMutexLocker k(&renderQueueMutex);
        activeRenders.push_back(w);
    }


    if (w.process) {
        w.process->startProcess();
    } else {
        w.work.writer->renderFullSequence(false, w.work.useRenderStats, NULL, w.work.firstFrame, w.work.lastFrame, w.work.frameStep);
    }
}

void
AppInstance::onQueuedRenderFinished(int /*retCode*/)
{
    RenderEngine* engine = qobject_cast<RenderEngine*>( sender() );

    if (!engine) {
        return;
    }
    boost::shared_ptr<OutputEffectInstance> effect = engine->getOutput();
    if (!effect) {
        return;
    }
    startNextQueuedRender( effect.get() );
}

void
AppInstance::removeRenderFromQueue(OutputEffectInstance* writer)
{
    QMutexLocker k(&_imp->renderQueueMutex);

    for (std::list<RenderQueueItem>::iterator it = _imp->renderQueue.begin(); it != _imp->renderQueue.end(); ++it) {
        if (it->work.writer == writer) {
            _imp->renderQueue.erase(it);
            break;
        }
    }
}

void
AppInstance::startNextQueuedRender(OutputEffectInstance* finishedWriter)
{
    RenderQueueItem nextWork;
    {
        QMutexLocker k(&_imp->renderQueueMutex);
        for (std::list<RenderQueueItem>::iterator it = _imp->activeRenders.begin(); it != _imp->activeRenders.end(); ++it) {
            if (it->work.writer == finishedWriter) {
                _imp->activeRenders.erase(it);
                break;
            }
        }
        if ( !_imp->renderQueue.empty() ) {
            nextWork = _imp->renderQueue.front();
            _imp->renderQueue.pop_front();
        } else {
            return;
        }
    }

    _imp->startRenderingFullSequence(false, nextWork);
}

void
AppInstance::onBackgroundRenderProcessFinished()
{
    ProcessHandler* proc = qobject_cast<ProcessHandler*>( sender() );
    OutputEffectInstance* effect = 0;

    if (proc) {
        effect = proc->getWriter();
    }
    if (effect) {
        startNextQueuedRender(effect);
    }
}

void
AppInstance::getFrameRange(double* first,
                           double* last) const
{
    return _imp->_currentProject->getFrameRange(first, last);
}

void
AppInstance::clearOpenFXPluginsCaches()
{
    NodesList activeNodes;

    _imp->_currentProject->getActiveNodes(&activeNodes);

    for (NodesList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->purgeAllInstancesCaches();
    }
}

void
AppInstance::clearAllLastRenderedImages()
{
    NodesList activeNodes;

    _imp->_currentProject->getActiveNodes(&activeNodes);

    for (NodesList::iterator it = activeNodes.begin(); it != activeNodes.end(); ++it) {
        (*it)->clearLastRenderedImage();
    }
}

void
AppInstance::aboutToQuit()
{
    ///Clear nodes now, not in the destructor of the project as
    ///deleting nodes might reference the project.
    _imp->_currentProject->closeProject(true);
    _imp->_currentProject->discardAppPointer();
}

void
AppInstance::quit()
{
    appPTR->quit(this);
}

ViewerColorSpaceEnum
AppInstance::getDefaultColorSpaceForBitDepth(ImageBitDepthEnum bitdepth) const
{
    return _imp->_currentProject->getDefaultColorSpaceForBitDepth(bitdepth);
}

void
AppInstance::onOCIOConfigPathChanged(const std::string& path)
{
    _imp->_currentProject->onOCIOConfigPathChanged(path, false);
}

void
AppInstance::declareCurrentAppVariable_Python()
{
    /// define the app variable
    std::stringstream ss;

    ss << "app" << _imp->_appID + 1 << " = " << NATRON_ENGINE_PYTHON_MODULE_NAME << ".natron.getInstance(" << _imp->_appID << ") \n";
    const KnobsVec& knobs = _imp->_currentProject->getKnobs();
    for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        ss << "app" << _imp->_appID + 1 << "." << (*it)->getName() << " = app" << _imp->_appID + 1 << ".getProjectParam('" <<
        (*it)->getName() << "')\n";
    }
    std::string script = ss.str();
    std::string err;
    bool ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, 0);
    assert(ok);
    if (!ok) {
        throw std::runtime_error("AppInstance::declareCurrentAppVariable_Python() failed!");
    }

    if ( appPTR->isBackground() ) {
        std::string err;
        ok = NATRON_PYTHON_NAMESPACE::interpretPythonScript("app = app1\n", &err, 0);
        assert(ok);
    }
}

double
AppInstance::getProjectFrameRate() const
{
    return _imp->_currentProject->getProjectFrameRate();
}

void
AppInstance::setProjectWasCreatedWithLowerCaseIDs(bool b)
{
    _imp->_projectCreatedWithLowerCaseIDs = b;
}

bool
AppInstance::wasProjectCreatedWithLowerCaseIDs() const
{
    return _imp->_projectCreatedWithLowerCaseIDs;
}

bool
AppInstance::isCreatingPythonGroup() const
{
    QMutexLocker k(&_imp->creatingGroupMutex);

    return _imp->_creatingGroup;
}

void
AppInstance::appendToScriptEditor(const std::string& str)
{
    std::cout << str <<  std::endl;
}

void
AppInstance::printAutoDeclaredVariable(const std::string& /*str*/)
{
}

void
AppInstance::execOnProjectCreatedCallback()
{
    std::string cb = appPTR->getCurrentSettings()->getOnProjectCreatedCB();

    if ( cb.empty() ) {
        return;
    }


    std::vector<std::string> args;
    std::string error;
    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        appendToScriptEditor( std::string("Failed to run onProjectCreated callback: ")
                              + e.what() );

        return;
    }

    if ( !error.empty() ) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The on project created callback supports the following signature(s):\n");
    signatureError.append("- callback(app)");
    if (args.size() != 1) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);

        return;
    }
    if (args[0] != "app") {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + signatureError);

        return;
    }
    std::string appID = getAppIDString();
    std::string script;
    if (appID != "app") {
        script = script + "app = " + appID;
    }
    script = script + "\n" + cb + "(" + appID + ")\n";
    std::string err;
    std::string output;
    if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(script, &err, &output) ) {
        appendToScriptEditor("Failed to run onProjectCreated callback: " + err);
    } else {
        if ( !output.empty() ) {
            appendToScriptEditor(output);
        }
    }
} // AppInstance::execOnProjectCreatedCallback

std::string
AppInstance::getAppIDString() const
{
    if ( appPTR->isBackground() ) {
        return "app";
    } else {
        QString appID =  QString( QString::fromUtf8("app%1") ).arg(getAppID() + 1);

        return appID.toStdString();
    }
}

void
AppInstance::onGroupCreationFinished(const NodePtr& node,
                                     CreateNodeReason reason)
{
    assert(node);
    if ( !_imp->_currentProject->isLoadingProject() && (reason != eCreateNodeReasonProjectLoad) && (reason != eCreateNodeReasonCopyPaste) ) {
        NodeGroup* isGrp = node->isEffectGroup();
        assert(isGrp);
        if (!isGrp) {
            return;
        }
        isGrp->forceComputeInputDependentDataOnAllTrees();
    }
}

bool
AppInstance::saveTemp(const std::string& filename)
{
    std::string outFile = filename;
    std::string path = SequenceParsing::removePath(outFile);
    boost::shared_ptr<Project> project = getProject();

    return project->saveProject_imp(QString::fromUtf8( path.c_str() ), QString::fromUtf8( outFile.c_str() ), false, false, 0);
}

bool
AppInstance::save(const std::string& filename)
{
    boost::shared_ptr<Project> project = getProject();

    if ( project->hasProjectBeenSavedByUser() ) {
        QString projectFilename = project->getProjectFilename();
        QString projectPath = project->getProjectPath();

        return project->saveProject(projectPath, projectFilename, 0);
    } else {
        return saveAs(filename);
    }
}

bool
AppInstance::saveAs(const std::string& filename)
{
    std::string outFile = filename;
    std::string path = SequenceParsing::removePath(outFile);

    return getProject()->saveProject(QString::fromUtf8( path.c_str() ), QString::fromUtf8( outFile.c_str() ), 0);
}

AppInstance*
AppInstance::loadProject(const std::string& filename)
{
    QFileInfo file( QString::fromUtf8( filename.c_str() ) );

    if ( !file.exists() ) {
        return 0;
    }
    QString fileUnPathed = file.fileName();
    QString path = file.path() + QChar::fromLatin1('/');

    //We are in background mode, there can only be 1 instance active, wipe the current project
    boost::shared_ptr<Project> project = getProject();
    project->resetProject();

    bool ok  = project->loadProject( path, fileUnPathed);
    if (ok) {
        return this;
    }

    project->resetProject();

    return 0;
}

///Close the current project but keep the window
bool
AppInstance::resetProject()
{
    getProject()->closeProject(false);

    return true;
}

///Reset + close window, quit if last window
bool
AppInstance::closeProject()
{
    getProject()->closeProject(true);
    quit();

    return true;
}

///Opens a new project
AppInstance*
AppInstance::newProject()
{
    CLArgs cl;
    AppInstance* app = appPTR->newAppInstance(cl, false);

    return app;
}

void
AppInstance::addInvalidExpressionKnob(const KnobPtr& knob)
{
    QMutexLocker k(&_imp->invalidExprKnobsMutex);

    for (std::list<KnobWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
        if ( it->lock().get() ) {
            return;
        }
    }
    _imp->invalidExprKnobs.push_back(knob);
}

void
AppInstance::removeInvalidExpressionKnob(const KnobI* knob)
{
    QMutexLocker k(&_imp->invalidExprKnobsMutex);

    for (std::list<KnobWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
        if (it->lock().get() == knob) {
            _imp->invalidExprKnobs.erase(it);
            break;
        }
    }
}

void
AppInstance::recheckInvalidExpressions()
{
    std::list<KnobPtr> knobs;
    {
        QMutexLocker k(&_imp->invalidExprKnobsMutex);
        for (std::list<KnobWPtr>::iterator it = _imp->invalidExprKnobs.begin(); it != _imp->invalidExprKnobs.end(); ++it) {
            KnobPtr k = it->lock();
            if (k) {
                knobs.push_back(k);
            }
        }
    }
    std::list<KnobWPtr> newInvalidKnobs;

    for (std::list<KnobPtr>::iterator it = knobs.begin(); it != knobs.end(); ++it) {
        if ( !(*it)->checkInvalidExpressions() ) {
            newInvalidKnobs.push_back(*it);
        }
    }
    {
        QMutexLocker k(&_imp->invalidExprKnobsMutex);
        _imp->invalidExprKnobs = newInvalidKnobs;
    }
}

NATRON_NAMESPACE_EXIT;

NATRON_NAMESPACE_USING;
#include "moc_AppInstance.cpp"
