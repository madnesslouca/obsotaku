// SPDX-License-Identifier: GPL-2.0-or-later
#define main retained_modal_suite_main
#include "modal-check.cpp"
#undef main
#include <QCheckBox>
void SelectArchiveRootVersion(int);
void RestoreRootModal(bool changed){
 ResetFacts();SeedCloud();Put("ClientId","qa-fixed-client");Put("AccountName","qa-fixed-name");
 auto*d=Open();SeedSelected(d);
 auto*tree=d->findChild<QTreeWidget*>();tree->currentItem()->setData(0,Qt::UserRole+2,true);
 const auto original=CloudRaw();bool reached=false;QTimer answer;
 QObject::connect(&answer,&QTimer::timeout,[&]{
  auto*modal=qobject_cast<QDialog*>(QApplication::activeModalWidget());
  if(!modal||!modal->findChild<QPlainTextEdit*>())return;
  answer.stop();reached=true;QCheckBox*credential=nullptr;
  for(auto*c:modal->findChildren<QCheckBox*>())if(c->text()=="CloudBackup.Restore.Credentials")credential=c;
  Check(credential&&!credential->isEnabled()&&!credential->isChecked()&&credential->toolTip()=="CloudBackup.CredentialsPaused","restore credentials explicitly disabled/unchecked with explanation");
  if(changed)SelectArchiveRootVersion(1);
  auto*buttons=modal->findChild<QDialogButtonBox*>();
  Check(buttons&&buttons->button(QDialogButtonBox::Ok),"actual restore options accept button present");
  if(buttons)buttons->button(QDialogButtonBox::Ok)->click();
 });
 answer.start(2);auto*button=Button(d,"CloudBackup.Restore");
 Check(button&&button->isEnabled(),"actual selected restore gesture enabled");if(button)button->click();
 Check(Pump([&]{return reached&&Responsive(d);}),"actual restore options reached and request settles");answer.stop();
 Check(stageRestores==(changed?0:1),changed?"root-only change rejects before Archive staging":"unchanged roots positive control reaches Archive staging");
 Check(Value("ClientId")=="qa-fixed-client"&&Value("AccountName")=="qa-fixed-name","root test leaves metadata unchanged");
 Check(CloudRaw()==original,"root test leaves source credential unchanged");
 Check(ioOnGui==0&&configOnWorker==0,"root capture/config remains GUI only and credential/archive I/O worker only");
 delete d;QCoreApplication::processEvents();
}
int main(int argc,char**argv){QApplication app(argc,argv);UrlSink sink;QDesktopServices::setUrlHandler("https",&sink,"Accept");RestoreRootModal(false);RestoreRootModal(true);ShutdownMultistreamTasks();QCoreApplication::processEvents();cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";return failures?1:0;}
