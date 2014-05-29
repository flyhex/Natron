//  Natron
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
/*
 *Created by Alexandre GAUTHIER-FOICHAT on 6/1/2012.
 *contact: immarespond at gmail dot com
 *
 */

#include "DockablePanel.h"

#include <iostream>
#include <QLayout>
#include <QAction>
#include <QTabWidget>
#include <QStyle>
#include <QUndoStack>
#include <QFormLayout>
#include <QUndoCommand>
#include <QDebug>
#include <QToolTip>
CLANG_DIAG_OFF(unused-private-field)
// /opt/local/include/QtGui/qmime.h:119:10: warning: private field 'type' is not used [-Wunused-private-field]
#include <QPaintEvent>
CLANG_DIAG_ON(unused-private-field)
#include <QTextDocument> // for Qt::convertFromPlainText

#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/EffectInstance.h"

#include "Gui/GuiAppInstance.h"
#include "Gui/GuiApplicationManager.h"
#include "Gui/NodeGui.h"
#include "Gui/KnobGui.h"
#include "Gui/KnobGuiTypes.h" // for Group_KnobGui
#include "Gui/KnobGuiFactory.h"
#include "Gui/LineEdit.h"
#include "Gui/Button.h"
#include "Gui/NodeGraph.h"
#include "Gui/ClickableLabel.h"
#include "Gui/Gui.h"
#include "Gui/TabWidget.h"
#include "Gui/RotoPanel.h"

using std::make_pair;
using namespace Natron;

namespace {

struct Page {
    QWidget* tab;
    int currentRow;
    QTabWidget* tabWidget; //< to gather group knobs that are set as a tab
    
    Page() : tab(0), currentRow(0),tabWidget(0)
    {}
    
    Page(const Page& other) : tab(other.tab), currentRow(other.currentRow) , tabWidget(other.tabWidget) {}
};
    
typedef std::map<QString,Page> PageMap;

}

struct DockablePanelPrivate
{
    DockablePanel* _publicInterface;
    
    Gui* _gui;
    
    QVBoxLayout* _container; /*!< ptr to the layout containing this DockablePanel*/
    
    /*global layout*/
    QVBoxLayout* _mainLayout;
    
    /*Header related*/
    QFrame* _headerWidget;
    QHBoxLayout *_headerLayout;
    
    LineEdit* _nameLineEdit; /*!< if the name is editable*/
    QLabel* _nameLabel; /*!< if the name is read-only*/
    
    /*Tab related*/
    QTabWidget* _tabWidget;
    
    Button* _helpButton;
    Button* _minimize;
    Button* _floatButton;
    Button* _cross;
    
    Button* _undoButton;
    Button* _redoButton;
    Button* _restoreDefaultsButton;
    
    bool _minimized; /*!< true if the panel is minimized*/
    QUndoStack* _undoStack; /*!< undo/redo stack*/
    
    bool _floating; /*!< true if the panel is floating*/
    FloatingWidget* _floatingWidget;
    
    /*a map storing for each knob a pointer to their GUI.*/
    std::map<boost::shared_ptr<KnobI>,KnobGui*> _knobs;
    KnobHolder* _holder;
    
    /* map<tab name, pair<tab , row count> >*/
    PageMap _pages;
    
    QString _defaultPageName;
    
    bool _useScrollAreasForTabs;
    
    DockablePanel::HeaderMode _mode;
    
    bool _isClosed;
    
    DockablePanelPrivate(DockablePanel* publicI
                         ,Gui* gui
                         ,KnobHolder* holder
                         , QVBoxLayout* container
                         , DockablePanel::HeaderMode headerMode
                         ,bool useScrollAreasForTabs
                         ,const QString& defaultPageName)
    :_publicInterface(publicI)
    ,_gui(gui)
    ,_container(container)
    ,_mainLayout(NULL)
    ,_headerWidget(NULL)
    ,_headerLayout(NULL)
    ,_nameLineEdit(NULL)
    ,_nameLabel(NULL)
    ,_tabWidget(NULL)
    ,_helpButton(NULL)
    ,_minimize(NULL)
    ,_floatButton(NULL)
    ,_cross(NULL)
    ,_undoButton(NULL)
    ,_redoButton(NULL)
    ,_restoreDefaultsButton(NULL)
    ,_minimized(false)
    ,_undoStack(new QUndoStack)
    ,_floating(false)
    ,_floatingWidget(NULL)
    ,_knobs()
    ,_holder(holder)
    ,_pages()
    ,_defaultPageName(defaultPageName)
    ,_useScrollAreasForTabs(useScrollAreasForTabs)
    ,_mode(headerMode)
    ,_isClosed(false)
    {
        
    }
    
    
    /*inserts a new page to the dockable panel.*/
    PageMap::iterator addPage(const QString& name);
    
    
    void initializeKnobVector(const std::vector< boost::shared_ptr< KnobI> >& knobs,
                              bool onlyTopLevelKnobs);
    
    KnobGui* createKnobGui(const boost::shared_ptr<KnobI> &knob);
    
    /*Search an existing knob GUI in the map, otherwise creates
     the gui for the knob.*/
    KnobGui* findKnobGuiOrCreate(const boost::shared_ptr<KnobI> &knob,
                                 bool makeNewLine,
                                 QWidget* lastRowWidget,
                    const std::vector< boost::shared_ptr< KnobI > >& knobsOnSameLine = std::vector< boost::shared_ptr< KnobI > >());
};

DockablePanel::DockablePanel(Gui* gui
                             , KnobHolder* holder
                             , QVBoxLayout* container
                             , HeaderMode headerMode
                             , bool useScrollAreasForTabs
                             , const QString& initialName
                             , const QString& helpToolTip
                             , bool createDefaultPage
                             , const QString& defaultPageName
                             , QWidget *parent)
: QFrame(parent)
, _imp(new DockablePanelPrivate(this,gui,holder,container,headerMode,useScrollAreasForTabs,defaultPageName))

{
    _imp->_mainLayout = new QVBoxLayout(this);
    _imp->_mainLayout->setSpacing(0);
    _imp->_mainLayout->setContentsMargins(0, 0, 0, 0);
    setLayout(_imp->_mainLayout);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFrameShape(QFrame::Box);
    
    if(headerMode != NO_HEADER){
        
        _imp->_headerWidget = new QFrame(this);
        _imp->_headerWidget->setFrameShape(QFrame::Box);
        _imp->_headerLayout = new QHBoxLayout(_imp->_headerWidget);
        _imp->_headerLayout->setContentsMargins(0, 0, 0, 0);
        _imp->_headerLayout->setSpacing(2);
        _imp->_headerWidget->setLayout(_imp->_headerLayout);
        
        
        QPixmap pixHelp ;
        appPTR->getIcon(NATRON_PIXMAP_HELP_WIDGET,&pixHelp);
        _imp->_helpButton = new Button(QIcon(pixHelp),"",_imp->_headerWidget);
        if (!helpToolTip.isEmpty()) {
            _imp->_helpButton->setToolTip(Qt::convertFromPlainText(helpToolTip, Qt::WhiteSpaceNormal));
        }
        _imp->_helpButton->setFixedSize(15, 15);
        QObject::connect(_imp->_helpButton, SIGNAL(clicked()), this, SLOT(showHelp()));
        
        
        QPixmap pixM;
        appPTR->getIcon(NATRON_PIXMAP_MINIMIZE_WIDGET,&pixM);
        
        QPixmap pixC;
        appPTR->getIcon(NATRON_PIXMAP_CLOSE_WIDGET,&pixC);
        
        QPixmap pixF;
        appPTR->getIcon(NATRON_PIXMAP_MAXIMIZE_WIDGET, &pixF);
        
        _imp->_minimize=new Button(QIcon(pixM),"",_imp->_headerWidget);
        _imp->_minimize->setFixedSize(15,15);
        _imp->_minimize->setCheckable(true);
        QObject::connect(_imp->_minimize,SIGNAL(toggled(bool)),this,SLOT(minimizeOrMaximize(bool)));
        
        _imp->_floatButton = new Button(QIcon(pixF),"",_imp->_headerWidget);
        _imp->_floatButton->setFixedSize(15, 15);
        QObject::connect(_imp->_floatButton,SIGNAL(clicked()),this,SLOT(floatPanel()));
        
        
        _imp->_cross=new Button(QIcon(pixC),"",_imp->_headerWidget);
        _imp->_cross->setFixedSize(15,15);
        QObject::connect(_imp->_cross,SIGNAL(clicked()),this,SLOT(closePanel()));
        
        
        QPixmap pixUndo ;
        appPTR->getIcon(NATRON_PIXMAP_UNDO,&pixUndo);
        QPixmap pixUndo_gray ;
        appPTR->getIcon(NATRON_PIXMAP_UNDO_GRAYSCALE,&pixUndo_gray);
        QIcon icUndo;
        icUndo.addPixmap(pixUndo,QIcon::Normal);
        icUndo.addPixmap(pixUndo_gray,QIcon::Disabled);
        _imp->_undoButton = new Button(icUndo,"",_imp->_headerWidget);
        _imp->_undoButton->setToolTip(Qt::convertFromPlainText("Undo the last change made to this operator", Qt::WhiteSpaceNormal));
        _imp->_undoButton->setEnabled(false);
        _imp->_undoButton->setFixedSize(20, 20);
        
        QPixmap pixRedo ;
        appPTR->getIcon(NATRON_PIXMAP_REDO,&pixRedo);
        QPixmap pixRedo_gray;
        appPTR->getIcon(NATRON_PIXMAP_REDO_GRAYSCALE,&pixRedo_gray);
        QIcon icRedo;
        icRedo.addPixmap(pixRedo,QIcon::Normal);
        icRedo.addPixmap(pixRedo_gray,QIcon::Disabled);
        _imp->_redoButton = new Button(icRedo,"",_imp->_headerWidget);
        _imp->_redoButton->setToolTip(Qt::convertFromPlainText("Redo the last change undone to this operator", Qt::WhiteSpaceNormal));
        _imp->_redoButton->setEnabled(false);
        _imp->_redoButton->setFixedSize(20, 20);
        
        QPixmap pixRestore;
        appPTR->getIcon(NATRON_PIXMAP_RESTORE_DEFAULTS, &pixRestore);
        QIcon icRestore;
        icRestore.addPixmap(pixRestore);
        _imp->_restoreDefaultsButton = new Button(icRestore,"",_imp->_headerWidget);
        _imp->_restoreDefaultsButton->setToolTip(Qt::convertFromPlainText("Restore default values for this operator."
                                                                    " This cannot be undone!",Qt::WhiteSpaceNormal));
        _imp->_restoreDefaultsButton->setFixedSize(20, 20);
        QObject::connect(_imp->_restoreDefaultsButton,SIGNAL(clicked()),this,SLOT(onRestoreDefaultsButtonClicked()));
    
        
        QObject::connect(_imp->_undoButton, SIGNAL(clicked()),this, SLOT(onUndoClicked()));
        QObject::connect(_imp->_redoButton, SIGNAL(clicked()),this, SLOT(onRedoPressed()));
        
        if(headerMode != READ_ONLY_NAME){
            _imp->_nameLineEdit = new LineEdit(_imp->_headerWidget);
            _imp->_nameLineEdit->setText(initialName);
            QObject::connect(_imp->_nameLineEdit,SIGNAL(editingFinished()),this,SLOT(onLineEditNameEditingFinished()));
            _imp->_headerLayout->addWidget(_imp->_nameLineEdit);
        }else{
            _imp->_nameLabel = new QLabel(initialName,_imp->_headerWidget);
            _imp->_headerLayout->addWidget(_imp->_nameLabel);
        }
        
        _imp->_headerLayout->addStretch();
        
        _imp->_headerLayout->addWidget(_imp->_undoButton);
        _imp->_headerLayout->addWidget(_imp->_redoButton);
        _imp->_headerLayout->addWidget(_imp->_restoreDefaultsButton);
        
        _imp->_headerLayout->addStretch();
        _imp->_headerLayout->addWidget(_imp->_helpButton);
        _imp->_headerLayout->addWidget(_imp->_minimize);
        _imp->_headerLayout->addWidget(_imp->_floatButton);
        _imp->_headerLayout->addWidget(_imp->_cross);
        
        _imp->_mainLayout->addWidget(_imp->_headerWidget);
        
    }
    
    _imp->_tabWidget = new QTabWidget(this);
    _imp->_tabWidget->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    _imp->_tabWidget->setObjectName("QTabWidget");
    _imp->_mainLayout->addWidget(_imp->_tabWidget);
    
    if(createDefaultPage){
        _imp->addPage(defaultPageName);
    }
}

DockablePanel::~DockablePanel(){
    delete _imp->_undoStack;
    
    ///Delete the knob gui if they weren't before
    ///normally the onKnobDeletion() function should have cleared them
    for(std::map<boost::shared_ptr<KnobI>,KnobGui*>::const_iterator it = _imp->_knobs.begin();it!=_imp->_knobs.end();++it){
        if(it->second){
            KnobHelper* helper = dynamic_cast<KnobHelper*>(it->first.get());
            QObject::disconnect(helper->getSignalSlotHandler().get(),SIGNAL(deleted()),this,SLOT(onKnobDeletion()));
            delete it->second;
        }
    }
}

void DockablePanel::onRestoreDefaultsButtonClicked() {
    for(std::map<boost::shared_ptr<KnobI>,KnobGui*>::const_iterator it = _imp->_knobs.begin();it!=_imp->_knobs.end();++it) {
        for (int i = 0; i < it->first->getDimension(); ++i) {
            if (it->first->typeName() != Button_Knob::typeNameStatic()) {
                it->first->resetToDefaultValue(i);
            }
        }
    }
}

void DockablePanel::onLineEditNameEditingFinished() {
    
    
    Natron::EffectInstance* effect = dynamic_cast<Natron::EffectInstance*>(_imp->_holder);
    if (effect) {
        
        std::string newName = _imp->_nameLineEdit->text().toStdString();
        if (newName.empty()) {
            _imp->_nameLineEdit->blockSignals(true);
            Natron::errorDialog("Node name", "A node must have a unique name.");
            _imp->_nameLineEdit->setText(effect->getName().c_str());
            _imp->_nameLineEdit->blockSignals(false);
            return;
        }
        
        ///if the node name hasn't changed return
        if (effect->getName() == newName) {
            return;
        }
        
        std::vector<boost::shared_ptr<Natron::Node> > allNodes = _imp->_holder->getApp()->getProject()->getCurrentNodes();
        for (U32 i = 0;  i < allNodes.size(); ++i) {
            if (allNodes[i]->getName() == newName) {
                _imp->_nameLineEdit->blockSignals(true);
                Natron::errorDialog("Node name", "A node with the same name already exists in the project.");
                _imp->_nameLineEdit->setText(effect->getName().c_str());
                _imp->_nameLineEdit->blockSignals(false);
                return;
            }
        }
    }
    emit nameChanged(_imp->_nameLineEdit->text());
}

void DockablePanelPrivate::initializeKnobVector(const std::vector< boost::shared_ptr< KnobI> >& knobs,
                                         bool onlyTopLevelKnobs) {
    QWidget* lastRowWidget = 0;
    for(U32 i = 0 ; i < knobs.size(); ++i) {
        
        ///we create only top level knobs, they will in-turn create their children if they have any
        if ((!onlyTopLevelKnobs) || (onlyTopLevelKnobs && !knobs[i]->getParentKnob())) {
            bool makeNewLine = true;
            
            boost::shared_ptr<Group_Knob> isGroup = boost::dynamic_pointer_cast<Group_Knob>(knobs[i]);
            
            ////The knob  will have a vector of all other knobs on the same line.
            std::vector< boost::shared_ptr< KnobI > > knobsOnSameLine;
            
            if (!isGroup) { //< a knob with children (i.e a group) cannot have children on the same line
                if (i > 0 && knobs[i-1]->isNewLineTurnedOff()) {
                    makeNewLine = false;
                }
                ///find all knobs backward that are on the same line.
                int k = i -1;
                while (k >= 0 && knobs[k]->isNewLineTurnedOff()) {
                    knobsOnSameLine.push_back(knobs[k]);
                    --k;
                }
                
                ///find all knobs forward that are on the same line.
                k = i;
                while (k < (int)(knobs.size() - 1) && knobs[k]->isNewLineTurnedOff()) {
                    knobsOnSameLine.push_back(knobs[k + 1]);
                    ++k;
                }
            }
            
            KnobGui* newGui = findKnobGuiOrCreate(knobs[i],makeNewLine,lastRowWidget,knobsOnSameLine);
            ///childrens cannot be on the same row than their parent
            if (!isGroup && newGui) {
                lastRowWidget = newGui->getFieldContainer();
            }

        }
    }
}

void DockablePanel::initializeKnobs() {
    
    /// function called to create the gui for each knob. It can be called several times in a row
    /// without any damage
    const std::vector< boost::shared_ptr<KnobI> >& knobs = _imp->_holder->getKnobs();
    
    _imp->initializeKnobVector(knobs, false);
    
    ///add all knobs left  to the default page
        
    RotoPanel* roto = initializeRotoPanel();
    if (roto) {
        std::map<QString,Page >::iterator parentTab = _imp->_pages.find(_imp->_defaultPageName);
        assert(parentTab != _imp->_pages.end());
        QFormLayout* layout;
        if (_imp->_useScrollAreasForTabs) {
            layout = dynamic_cast<QFormLayout*>(dynamic_cast<QScrollArea*>(parentTab->second.tab)->widget()->layout());
        } else {
            layout = dynamic_cast<QFormLayout*>(parentTab->second.tab->layout());
        }
        assert(layout);
        layout->addRow(roto);
    }
}


KnobGui* DockablePanel::getKnobGui(const boost::shared_ptr<KnobI>& knob) const
{
    for (std::map<boost::shared_ptr<KnobI>,KnobGui*>::const_iterator it = _imp->_knobs.begin(); it!=_imp->_knobs.end(); ++it) {
        if(it->first == knob){
            return it->second;
        }
    }
    return NULL;
}

KnobGui* DockablePanelPrivate::createKnobGui(const boost::shared_ptr<KnobI> &knob)
{
    KnobHelper* helper = dynamic_cast<KnobHelper*>(knob.get());
    QObject::connect(helper->getSignalSlotHandler().get(),SIGNAL(deleted()),_publicInterface,SLOT(onKnobDeletion()));
    
    KnobGui* ret =  appPTR->createGuiForKnob(knob,_publicInterface);
    if (!ret) {
        qDebug() << "Failed to create Knob GUI";
        return NULL;
    }
    _knobs.insert(make_pair(knob, ret));
    return ret;
}

KnobGui* DockablePanelPrivate::findKnobGuiOrCreate(const boost::shared_ptr<KnobI>& knob,
                                                   bool makeNewLine,
                                                   QWidget* lastRowWidget,
                                            const std::vector< boost::shared_ptr< KnobI > >& knobsOnSameLine) {
    
    assert(knob);
    KnobGui* ret = 0;
    for (std::map<boost::shared_ptr<KnobI>,KnobGui*>::const_iterator it = _knobs.begin(); it!=_knobs.end(); ++it) {
        if(it->first == knob){
            return it->second;
        }
    }
    
    boost::shared_ptr<Group_Knob> isGroup = boost::dynamic_pointer_cast<Group_Knob>(knob);
    boost::shared_ptr<Page_Knob> isPage = boost::dynamic_pointer_cast<Page_Knob>(knob);
    
    
    if (isPage) {
        addPage(isPage->getDescription().c_str());
    } else {
        
        ret = createKnobGui(knob);
        
        ///if widgets for the KnobGui have already been created, don't the following
        ///For group only create the gui if it is not  a tab.
        if (!ret->hasWidgetBeenCreated() && (!isGroup || !isGroup->isTab())) {
            
            
            boost::shared_ptr<KnobI> parentKnob = knob->getParentKnob();
            boost::shared_ptr<Group_Knob> parentIsGroup = boost::dynamic_pointer_cast<Group_Knob>(parentKnob);
            
            
            /// if this knob is within a group, make sure the group is created so far
            Group_KnobGui* parentGui = 0;
            if (parentIsGroup) {
                parentGui = dynamic_cast<Group_KnobGui*>(findKnobGuiOrCreate(parentKnob,true,NULL));
            }
            
            KnobI* parentKnobTmp = parentKnob.get();
            while (parentKnobTmp) {
                boost::shared_ptr<KnobI> parent = parentKnobTmp->getParentKnob();
                if (!parent) {
                    break;
                } else {
                    parentKnobTmp = parent.get();
                }
            }
            
            ////find in which page the knob should be
            Page_Knob* isTopLevelParentAPage = dynamic_cast<Page_Knob*>(parentKnobTmp);
        
            PageMap::iterator page = _pages.end();
            
            if (isTopLevelParentAPage) {
                page = addPage(isTopLevelParentAPage->getDescription().c_str());
            } else {
                ///the top level parent is not a page, i.e the plug-in didn't specify any page
                ///for this param, put it in the first page that is not the default page.
                ///If there is still no page, put it in the default tab.
                for (PageMap::iterator it = _pages.begin(); it!=_pages.end(); ++it) {
                    if (it->first != _defaultPageName) {
                        page = it;
                        break;
                    }
                }
                if (page == _pages.end()) {
                    const std::vector< boost::shared_ptr<KnobI> >& knobs = _holder->getKnobs();
                    ///find in all knobs a page param to set this param into
                    for (U32 i = 0; i < knobs.size(); ++i) {
                        Page_Knob* p = dynamic_cast<Page_Knob*>(knobs[i].get());
                        if (p) {
                            page = addPage(p->getDescription().c_str());
                            break;
                        }
                    }
                    
                    ///Last resort: The plug-in didn't specify ANY page, just put it into the default page
                    if (page == _pages.end()) {
                        page = addPage(_defaultPageName);
                    }
                }
            }
            
            assert(page != _pages.end());
            
            ///retrieve the form layout
            QFormLayout* layout;
            if (_useScrollAreasForTabs) {
                layout = dynamic_cast<QFormLayout*>(
                                                    dynamic_cast<QScrollArea*>(page->second.tab)->widget()->layout());
            } else {
                layout = dynamic_cast<QFormLayout*>(page->second.tab->layout());
            }
            assert(layout);
            
            
            ///if the knob has specified that it didn't want to trigger a new line, decrement the current row
            /// index of the tab
            
            if (!makeNewLine) {
                --page->second.currentRow;
            }
            
            QWidget* fieldContainer = 0;
            QHBoxLayout* fieldLayout = 0;
            
            if (makeNewLine) {
                ///if new line is not turned off, create a new line
                fieldContainer = new QWidget(page->second.tab);
                fieldLayout = new QHBoxLayout(fieldContainer);
                fieldLayout->setContentsMargins(3,0,0,0);
                fieldContainer->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
            } else {
                ///otherwise re-use the last row's widget and layout
                assert(lastRowWidget);
                fieldContainer = lastRowWidget;
                fieldLayout = dynamic_cast<QHBoxLayout*>(fieldContainer->layout());
                
                ///the knobs use this value to know whether we should remove the row or not
                //fieldContainer->setObjectName("multi-line");
                
            }
            assert(fieldContainer);
            assert(fieldLayout);
            ClickableLabel* label = new ClickableLabel("",page->second.tab);
            
            
            if (ret->showDescriptionLabel() && label) {
                label->setText_overload(QString(QString(ret->getKnob()->getDescription().c_str()) + ":"));
                QObject::connect(label, SIGNAL(clicked(bool)), ret, SIGNAL(labelClicked(bool)));
            }
            
            if (parentIsGroup && parentIsGroup->isTab()) {
                ///The group is a tab, check if the tab widget is created
                if (!page->second.tabWidget) {
                    QFrame* frame = new QFrame(_publicInterface);
                    frame->setFrameShadow(QFrame::Raised);
                    frame->setFrameShape(QFrame::Box);
                    QHBoxLayout* frameLayout = new QHBoxLayout(frame);
                    page->second.tabWidget = new QTabWidget(frame);
                    frameLayout->addWidget(page->second.tabWidget);
                    layout->addRow(frame);
                }
                QString parentTabName(parentIsGroup->getDescription().c_str());
                
                ///now check if the tab exists
                QWidget* tab = 0;
                QFormLayout* tabLayout = 0;
                for (int i = 0; i < page->second.tabWidget->count(); ++i) {
                    if (page->second.tabWidget->tabText(i) == parentTabName) {
                        tab = page->second.tabWidget->widget(i);
                        tabLayout = qobject_cast<QFormLayout*>(tab->layout());
                        break;
                    }
                }
                
                if (!tab) {
                    tab = new QWidget(page->second.tabWidget);
                    tabLayout = new QFormLayout(tab);
                    tabLayout->setContentsMargins(0, 0, 0, 0);
                    tabLayout->setSpacing(3);
                    page->second.tabWidget->addTab(tab,parentTabName);
                }
                
                ret->createGUI(tabLayout,fieldContainer,label,fieldLayout,page->second.currentRow,makeNewLine,knobsOnSameLine);
            } else {
                
                ///fill the fieldLayout with the widgets
                ret->createGUI(layout,fieldContainer,label,fieldLayout,page->second.currentRow,makeNewLine,knobsOnSameLine);
                
            }
            
            ///increment the row count
            ++page->second.currentRow;
            
            /// if this knob is within a group, check that the group is visible, i.e. the toplevel group is unfolded
            if (parentIsGroup) {
                assert(parentGui);
                ///FIXME: this offsetColumn is never really used. Shall we use this anyway? It seems
                ///to work fine without it.
                int offsetColumn = knob->determineHierarchySize();
                parentGui->addKnob(ret,page->second.currentRow,offsetColumn);
                
                bool showit = !ret->getKnob()->getIsSecret();
                // see KnobGui::setSecret() for a very similar code
                while (showit && parentIsGroup) {
                    assert(parentGui);
                    // check for secretness and visibility of the group
                    if (parentKnob->getIsSecret() || (parentGui && !parentGui->isChecked())) {
                        showit = false; // one of the including groups is folded, so this item is hidden
                    }
                    // prepare for next loop iteration
                    parentKnob = parentKnob->getParentKnob();
                    parentIsGroup =  boost::dynamic_pointer_cast<Group_Knob>(parentKnob);
                    if (parentKnob) {
                        parentGui = dynamic_cast<Group_KnobGui*>(findKnobGuiOrCreate(parentKnob,true,NULL));
                    }
                }
                if (showit) {
                    ret->show();
                } else {
                    //gui->hide(); // already hidden? please comment if it's not.
                }
            }
        }
    } // !isPage
    
    ///if the knob is a group, create all the children
    if (isGroup) {
        initializeKnobVector(isGroup->getChildren(), false);
    } else if (isPage) {
        initializeKnobVector(isPage->getChildren(), false);
    }
    
    return ret;
    
}



PageMap::iterator DockablePanelPrivate::addPage(const QString& name)
{
    PageMap::iterator found = _pages.find(name);
    if (found != _pages.end()) {
        return found;
    }
    
    QWidget* newTab;
    QWidget* layoutContainer;
    if (_useScrollAreasForTabs) {
        QScrollArea* sa = new QScrollArea(_tabWidget);
        layoutContainer = new QWidget(sa);
        sa->setWidgetResizable(true);
        sa->setWidget(layoutContainer);
        newTab = sa;
    } else {
        newTab = new QWidget(_tabWidget);
        layoutContainer = newTab;
    }
    newTab->setObjectName(name);
    QFormLayout *tabLayout = new QFormLayout(layoutContainer);
    tabLayout->setObjectName("formLayout");
    layoutContainer->setLayout(tabLayout);
    //tabLayout->setVerticalSpacing(2); // unfortunately, this leaves extra space when parameters are hidden
    tabLayout->setVerticalSpacing(3);
    tabLayout->setContentsMargins(3, 0, 0, 0);
    tabLayout->setHorizontalSpacing(3);
    tabLayout->setLabelAlignment(Qt::AlignVCenter | Qt::AlignRight);
    tabLayout->setFormAlignment(Qt::AlignLeft | Qt::AlignTop);
    tabLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    _tabWidget->addTab(newTab,name);
    Page p;
    p.tab = newTab;
    p.currentRow = 0;
    return _pages.insert(make_pair(name,p)).first;
}

const QUndoCommand* DockablePanel::getLastUndoCommand() const{
    return _imp->_undoStack->command(_imp->_undoStack->index()-1);
}

void DockablePanel::pushUndoCommand(QUndoCommand* cmd){
    _imp->_undoStack->setActive();
    _imp->_undoStack->push(cmd);
    if(_imp->_undoButton && _imp->_redoButton){
        _imp->_undoButton->setEnabled(_imp->_undoStack->canUndo());
        _imp->_redoButton->setEnabled(_imp->_undoStack->canRedo());
    }
}

void DockablePanel::onUndoClicked(){
    _imp->_undoStack->undo();
    if(_imp->_undoButton && _imp->_redoButton){
        _imp->_undoButton->setEnabled(_imp->_undoStack->canUndo());
        _imp->_redoButton->setEnabled(_imp->_undoStack->canRedo());
    }
    emit undoneChange();
}

void DockablePanel::onRedoPressed(){
    _imp->_undoStack->redo();
    if(_imp->_undoButton && _imp->_redoButton){
        _imp->_undoButton->setEnabled(_imp->_undoStack->canUndo());
        _imp->_redoButton->setEnabled(_imp->_undoStack->canRedo());
    }
    emit redoneChange();
}

void DockablePanel::showHelp(){
    QToolTip::showText(QCursor::pos(), _imp->_helpButton->toolTip());
}

void DockablePanel::setClosed(bool c)
{
    setVisible(!c);
    _imp->_isClosed = c;
    emit closeChanged(c);
}

void DockablePanel::closePanel() {
    if (_imp->_floating) {
        floatPanel();
    }
    close();
    _imp->_isClosed = true;
    emit closeChanged(true);
    getGui()->getApp()->redrawAllViewers();
    
}
void DockablePanel::minimizeOrMaximize(bool toggled){
    _imp->_minimized=toggled;
    if(_imp->_minimized){
        emit minimized();
    }else{
        emit maximized();
    }
    _imp->_tabWidget->setVisible(!_imp->_minimized);
    std::vector<QWidget*> _panels;
    for(int i =0 ; i < _imp->_container->count(); ++i) {
        if (QWidget *myItem = dynamic_cast <QWidget*>(_imp->_container->itemAt(i))){
            _panels.push_back(myItem);
            _imp->_container->removeWidget(myItem);
        }
    }
    for (U32 i =0 ; i < _panels.size(); ++i) {
        _imp->_container->addWidget(_panels[i]);
    }
    update();
}

void DockablePanel::floatPanel() {
    _imp->_floating = !_imp->_floating;
    if (_imp->_floating) {
        assert(!_imp->_floatingWidget);
        _imp->_floatingWidget = new FloatingWidget(_imp->_gui);
        QObject::connect(_imp->_floatingWidget,SIGNAL(closed()),this,SLOT(closePanel()));
        _imp->_container->removeWidget(this);
        _imp->_floatingWidget->setWidget(size(),this);
    } else {
        assert(_imp->_floatingWidget);
        _imp->_floatingWidget->removeWidget();
        setParent(_imp->_container->parentWidget());
        _imp->_container->insertWidget(0, this);
        delete _imp->_floatingWidget;
        _imp->_floatingWidget = 0;
    }
}


void DockablePanel::onNameChanged(const QString& str){
    if(_imp->_nameLabel){
        _imp->_nameLabel->setText(str);
    }else if(_imp->_nameLineEdit){
        _imp->_nameLineEdit->setText(str);
    }
}


Button* DockablePanel::insertHeaderButton(int headerPosition){
    Button* ret = new Button(_imp->_headerWidget);
    _imp->_headerLayout->insertWidget(headerPosition, ret);
    return ret;
}

void DockablePanel::onKnobDeletion(){
    
    KnobSignalSlotHandler* handler = qobject_cast<KnobSignalSlotHandler*>(sender());
    if (handler) {
        for(std::map<boost::shared_ptr<KnobI>,KnobGui*>::iterator it = _imp->_knobs.begin();it!=_imp->_knobs.end();++it){
            KnobHelper* helper = dynamic_cast<KnobHelper*>(it->first.get());
            if (helper->getSignalSlotHandler().get() == handler) {
                if(it->second){
                    delete it->second;
                }
                _imp->_knobs.erase(it);
                return;
            }
        }
    }
    
}


Gui* DockablePanel::getGui() const {
    return _imp->_gui;
}

void DockablePanel::insertHeaderWidget(int index,QWidget* widget) {
    if (_imp->_mode != NO_HEADER) {
        _imp->_headerLayout->insertWidget(index, widget);
    }
}

void DockablePanel::appendHeaderWidget(QWidget* widget) {
    if (_imp->_mode != NO_HEADER) {
        _imp->_headerLayout->addWidget(widget);
    }
}

QWidget* DockablePanel::getHeaderWidget() const {
    return _imp->_headerWidget;
}

bool DockablePanel::isMinimized() const {return _imp->_minimized;}

const std::map<boost::shared_ptr<KnobI>,KnobGui*>& DockablePanel::getKnobs() const { return _imp->_knobs; }

QVBoxLayout* DockablePanel::getContainer() const {return _imp->_container;}

QUndoStack* DockablePanel::getUndoStack() const { return _imp->_undoStack; }

bool DockablePanel::isClosed() const { return _imp->_isClosed; }


NodeSettingsPanel::NodeSettingsPanel(Gui* gui,boost::shared_ptr<NodeGui> NodeUi ,QVBoxLayout* container,QWidget *parent)
:DockablePanel(gui,NodeUi->getNode()->getLiveInstance(),
               container,
               DockablePanel::FULLY_FEATURED,
               false,
               NodeUi->getNode()->getName().c_str(),
               NodeUi->getNode()->description().c_str(),
               false,
               "Settings",
               parent)
,_nodeGUI(NodeUi)
{
    QPixmap pixC;
    appPTR->getIcon(NATRON_PIXMAP_VIEWER_CENTER,&pixC);
    _centerNodeButton = new Button(QIcon(pixC),"",getHeaderWidget());
    _centerNodeButton->setToolTip("Centers the node graph on this node.");
    _centerNodeButton->setFixedSize(15, 15);
    QObject::connect(_centerNodeButton,SIGNAL(clicked()),this,SLOT(centerNode()));
    insertHeaderWidget(0, _centerNodeButton);
}

NodeSettingsPanel::~NodeSettingsPanel(){
    _nodeGUI->removeSettingsPanel();
}


void NodeSettingsPanel::setSelected(bool s){
    _selected = s;
    style()->unpolish(this);
    style()->polish(this);
}

void NodeSettingsPanel::centerNode() {
    _nodeGUI->centerGraphOnIt();
}

RotoPanel* NodeSettingsPanel::initializeRotoPanel()
{
    if (_nodeGUI->getNode()->isRotoNode()) {
        return new RotoPanel(_nodeGUI.get(),this);
    } else {
        return NULL;
    }
}
