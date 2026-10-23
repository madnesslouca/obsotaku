/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dialogs/GoogleDriveBackupDialog.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <OBSApp.hpp>
#include "facts.hpp"
#include <QApplication>
#include <QComboBox>
#include <QInputDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QThread>
#include <QElapsedTimer>
#include <iostream>
using namespace std;
int checks=0,failures=0;
void Check(bool ok,const char *name){++checks;if(!ok){++failures;cerr<<"FAIL "<<name<<'\n';}}
template<class F>bool Pump(F ready){QElapsedTimer timer;timer.start();while(!ready()&&timer.elapsed()<3000){QCoreApplication::processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}return ready();}
QPushButton *Button(GoogleDriveBackupDialog *dialog,const char *name){for(auto *b:dialog->findChildren<QPushButton*>())if(b->text()==name)return b;return nullptr;}
void Run(bool external){
 ResetFacts();SeedCloud();auto *config=App()->GetUserConfig();config_set_string(config,"GoogleDriveBackup","ClientId","qa-client-A");config_set_int(config,"GoogleDriveBackup","Retention",10);
 auto *dialog=new GoogleDriveBackupDialog;
 Check(Pump([&]{return Button(dialog,"CloudBackup.BackupNow")->isEnabled();}),"actual dialog connected");
 QComboBox *retention=nullptr;for(auto *combo:dialog->findChildren<QComboBox*>())if(combo->findData(50)>=0&&combo->findData(5)>=0)retention=combo;
 Check(retention&&retention->currentData().toInt()==10,"retention widget loaded original10");
 if(!external&&retention)retention->setCurrentIndex(retention->findData(50));
 bool reached=false;
 QTimer::singleShot(0,[&]{auto *input=qobject_cast<QInputDialog*>(QApplication::activeModalWidget());reached=input;if(input){if(external)config_set_int(config,"GoogleDriveBackup","Retention",20);input->setTextValue("qa-client-own");input->accept();}});
 auto *configure=Button(dialog,"CloudBackup.ConfigureClient");Check(configure&&configure->isEnabled(),"actual Configure button enabled");if(configure)configure->click();
 Check(reached,"actual ConfigureClientId input reached");
 Check(string(config_get_string(config,"GoogleDriveBackup","ClientId"))=="qa-client-own","own ClientId subtransaction accepted");
 Check(retention&&retention->currentData().toInt()==(external?10:50),"own auth change preserves local draft widget");
 Check(config_get_int(config,"GoogleDriveBackup","Retention")== (external?20:10),"own ClientId write does not itself change settings");
 auto *backup=Button(dialog,"CloudBackup.BackupNow");Check(Pump([&]{return backup&&backup->isEnabled();}),"actual BackupNow action enabled after Configure connection check");if(backup)backup->click();
 Check(Pump([&]{auto *b=Button(dialog,"CloudBackup.BackupNow");return b&&b->isEnabled();}),"BackupNow returns responsive");
 Check(config_get_int(config,"GoogleDriveBackup","Retention")== (external?20:50),external?"external Retention20 survives stale draft after own auth write":"unchanged settings allow local Retention50 draft after own auth write");
 Check(external?archives==0:archives==1,external?"stale settings draft stops before archive worker":"valid local draft reaches archive worker");
 delete dialog;QCoreApplication::processEvents();
}
int main(int argc,char **argv){QApplication app(argc,argv);Run(false);Run(true);ShutdownMultistreamTasks();QCoreApplication::processEvents();Check(ioOnGui==0&&configOnWorker==0,"credential I/O worker-only and config GUI-only");cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";return failures?1:0;}
