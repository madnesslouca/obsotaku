/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <dialogs/GoogleDriveBackupDialog.hpp>
#include <utility/MultistreamTaskPool.hpp>
#include <OBSApp.hpp>
#include <FakeOBSBasic.hpp>
#include "facts.hpp"
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <chrono>
#include <iostream>
#include <thread>
using namespace std;
using namespace std::chrono_literals;
int checks=0,failures=0,beats=0,originalChecks=0,originalFailures=0;
bool extensionSection=false;
long long constructorMs=-1,disconnectMs=-1,schedulerMs=-1;
void Check(bool pass,const char*label){++checks;if(!extensionSection){++originalChecks;if(!pass)++originalFailures;}if(!pass){++failures;cerr<<"FAIL "<<label<<"\n";}}
template<class Condition> bool Pump(Condition ready,int timeout=3000){
 QElapsedTimer clock;clock.start();
 while(!ready()&&clock.elapsed()<timeout){QCoreApplication::processEvents(QEventLoop::AllEvents,5);QThread::msleep(1);}
 return ready();
}
template<class Task> void Worker(Task task){
 atomic_bool done{false};thread thread([&]{task();done=true;});
 Check(Pump([&]{return done.load();}),"worker completes without blocking GUI event loop");thread.join();
 QCoreApplication::processEvents();
}
QPushButton* Button(GoogleDriveBackupDialog*d,const char*key){
 for(auto*b:d->findChildren<QPushButton*>())if(b->text()==QString::fromUtf8(key))return b;
 return nullptr;
}
bool Ready(GoogleDriveBackupDialog*d){
 auto*b=Button(d,"CloudBackup.BackupNow");return b&&b->isEnabled();
}
void DialogCache(){
 ResetFacts();SeedCloud();keyringDelay=100;
 QElapsedTimer time;time.start();auto*d=new GoogleDriveBackupDialog;constructorMs=time.elapsed();
 Check(constructorMs<50,"dialog constructor never waits for keyring");
 const int before=beats;
 Check(Pump([&]{return Ready(d);}),"connection/history worker completes");
 Check(beats-before>=15,"GUI heartbeat continues during slow keyring");
 Check(ioOnGui==0&&configOnWorker==0,"credential IO worker-only; config GUI-only");
 Check(loads==2,"one connection check plus list token load");
 auto*tree=d->findChild<QTreeWidget*>();const int calls=loads;
 auto*row=new QTreeWidgetItem(tree);row->setText(0,"fixture");tree->setCurrentItem(row);
 QCoreApplication::processEvents();
 Check(loads==calls,"selection and UI update never query keyring");
 delete d;QCoreApplication::processEvents();
}
void ClosePending(){
 ResetFacts();SeedCloud();keyringDelay=200;
 auto*d=new GoogleDriveBackupDialog;
 Check(Pump([]{return loads.load()>0;}),"pending keyring call started");
 delete d;QElapsedTimer clock;clock.start();
 MultistreamTaskPool().start([]{}); // A harmless later task proves the pool remains usable.
 Check(Pump([]{return keyringActive.load()==0;}),"closed target cancels and drains pending keyring call");
 // Completion may already be queued; process it only after destroying receiver.
 while(clock.elapsed()<40){QCoreApplication::processEvents();QThread::msleep(1);}
 Check(ioOnGui==0&&configOnWorker==0,"destroyed dialog does not route work to GUI");
}
void PresenceAndRecovery(){
 ResetFacts();QString error;bool found=true;
 Worker([&]{found=GoogleDriveBackupClient::HasConnection(error);});
 Check(!found&&error.isEmpty(),"missing credential distinct from provider error");
 unavailable=true;
 Worker([&]{found=GoogleDriveBackupClient::HasConnection(error);});
 Check(!found&&!error.isEmpty(),"provider error distinct from missing");
 unavailable=false;SeedCloud();const auto old=CloudRaw();deleteCommitCancel=true;
 auto request=make_shared<SecureTokenStore::RequestContext>();bool removed=true;
 Worker([&]{removed=GoogleDriveBackupClient::Disconnect(error,request);});
 Check(!removed&&request->IsCancelled()&&!error.isEmpty(),"ambiguous delete reported as failure");
 Check(CloudRaw()==old,"fresh recovery restores exact current raw credential");
 deleteCommitCancel=true;failSave=true;
 Worker([&]{removed=GoogleDriveBackupClient::Disconnect(error);});
 Check(!removed&&!CloudRaw()&&error.contains(QStringLiteral("não foi confirmada")),"failed recovery remains explicit and unconfirmed");
 failSave=false;SeedCloud();
 Worker([&]{removed=GoogleDriveBackupClient::Disconnect(error);});
 Check(removed&&!CloudRaw()&&error.isEmpty(),"confirmed deletion succeeds");
 Check(ioOnGui==0&&configOnWorker==0,"provider has no worker config side effects");
}
void RefreshRace(){
 ResetFacts();SeedCloud(true);blockRefresh=true;
 config_set_string(App()->GetUserConfig(),"GoogleDriveBackup","ClientId","qa-client");
 const auto registration=GoogleDriveBackupClient::Registration();
 atomic_bool done{false};bool listed=false;QString listError;
 thread refresher([&]{vector<GoogleDriveBackupItem> items;listed=GoogleDriveBackupClient::List(items,listError,registration);done=true;});
 Check(Pump([]{return refreshEntered.load();}),"refresh entered under Cloud credential mutex");
 auto request=make_shared<SecureTokenStore::RequestContext>(80ms);QString error;bool removed=true;
 Worker([&]{removed=GoogleDriveBackupClient::Disconnect(error,request);});
 Check(!removed&&deletes==0&&!done&&!error.isEmpty(),"delete expires while refresh owns lock without touching credential");
 releaseRefresh=true;
 Check(Pump([&]{return done.load();}),"refresh finishes durably without expired UI scope");
 refresher.join();
 const auto rotated=CloudRaw();
 Check(listed&&refreshCalls==1&&rotated&&rotated->find("qa-cloud-rotated")!=string::npos,"rotated credential persisted");
 deleteCommitCancel=true;
 Worker([&]{removed=GoogleDriveBackupClient::Disconnect(error);});
 Check(!removed&&CloudRaw()==rotated,"deletion rollback never resurrects pre-refresh credential");
 QString name;
 Worker([&]{GoogleDriveBackupClient::AccountName(name,error,registration);});
 Check(!name.isEmpty()&&configOnWorker==0,"AccountName returns metadata without worker config writes");
}
void DisconnectUi(){
 ResetFacts();SeedCloud();keyringDelay=70;
 config_set_string(App()->GetUserConfig(),"GoogleDriveBackup","AccountName","QA before");
 auto*d=new GoogleDriveBackupDialog;
 Check(Pump([&]{return Ready(d);}),"disconnect fixture connected");
 QTimer answer;QObject::connect(&answer,&QTimer::timeout,[]{
  for(auto*w:QApplication::topLevelWidgets())if(auto*box=qobject_cast<QMessageBox*>(w))box->button(QMessageBox::Yes)->click();
 });answer.start(5);
 auto*button=Button(d,"CloudBackup.Disconnect");Check(button,"disconnect button present");
 const int before=beats;QElapsedTimer time;time.start();if(button)button->click();disconnectMs=time.elapsed();answer.stop();
 Check(disconnectMs<80,"disconnect click dispatches credential IO to worker");
 Check(Pump([&]{auto*b=Button(d,"CloudBackup.Connect");return b&&b->isEnabled();}),"disconnect completion updates cached UI");
 Check(beats-before>=15,"GUI heartbeat continues during credential deletion");
 Check(!CloudRaw()&&!config_get_string(App()->GetUserConfig(),"GoogleDriveBackup","AccountName"),"metadata cleared only after confirmed deletion GUI callback");
 Check(ioOnGui==0&&configOnWorker==0,"disconnect UI preserves thread ownership");
 delete d;
}
void Scheduler(){
 ResetFacts();OBSBasic basic;
 basic.RunAutomaticGoogleDriveBackup();Check(loads==0&&!basic.googleDriveBackupRunning,"disabled scheduler does not query keyring");
 auto*c=App()->GetUserConfig();config_set_bool(c,"GoogleDriveBackup","AutoEnabled",true);config_set_int(c,"GoogleDriveBackup","IntervalDays",1);config_set_int(c,"GoogleDriveBackup","Retention",5);
 keyringDelay=100;const int before=beats;QElapsedTimer time;time.start();basic.RunAutomaticGoogleDriveBackup();schedulerMs=time.elapsed();
 Check(schedulerMs<50&&basic.googleDriveBackupRunning,"scheduler keyring check dispatched from GUI");
 Check(Pump([&]{return !basic.googleDriveBackupRunning;}),"missing scheduler credential exits worker");
 Check(archives==0&&httpCalls==0&&beats-before>=10,"missing credentials skip archive/network while GUI ticks");
 // Establish the happy-path authorization through the real namespace API;
 // direct external RAW injection after NotFound correctly invalidates GUI pin.
 OAuthTokenSet authorized;authorized.accessToken="qa-cloud";authorized.refreshToken="qa-refresh";authorized.expiresAt=4102444800;QString authorizationError;keyringDelay=0;
 thread authorization([&]{GoogleDriveBackupClient::StoreTokens(authorized,authorizationError);});authorization.join();
 keyringDelay=25;basic.RunAutomaticGoogleDriveBackup();
 Check(Pump([&]{return !basic.googleDriveBackupRunning;}),"scheduled fake backup completes");
 Check(archives==1&&httpCalls>=3,"scheduler reaches fake upload and retention after credential check");
 Check(config_get_int(c,"GoogleDriveBackup","LastBackupUnix")>0,"scheduler commits metadata on GUI");
 Check(ioOnGui==0&&configOnWorker==0,"scheduler adapter thread ownership");
}
void HttpCloseAndRotation(){
 ResetFacts();SeedCloud();auto*d=new GoogleDriveBackupDialog;
 Check(Pump([&]{return Ready(d);}),"HTTP close fixture connected");
 httpDelay=2000;
 auto*refresh=Button(d,"CloudBackup.Refresh");Check(refresh,"refresh button present for HTTP close");
 if(refresh)refresh->click();
 Check(Pump([]{return httpActive.load()>0;}),"Drive HTTP started before closing receiver");
 QElapsedTimer clock;clock.start();delete d;
 Check(Pump([]{return httpActive.load()==0;}),"closing receiver cancels Drive HTTP");
 Check(clock.elapsed()<300&&httpCancelledCalls==1,"dialog cancellation drains HTTP promptly and reports no success");
 ResetFacts();SeedCloud(true);blockRefresh=true;
 const auto registration=GoogleDriveBackupClient::Registration();
 auto signal=make_shared<atomic_bool>(false);OAuthHttpClient::Cancellation cancel{signal,{}};
 atomic_bool done{false};bool listed=true;QString error;
 thread task([&]{vector<GoogleDriveBackupItem> items;listed=GoogleDriveBackupClient::List(items,error,registration,cancel);done=true;});
 Check(Pump([]{return refreshEntered.load();}),"cancel scope reaches already-running token rotation");
 signal->store(true);releaseRefresh=true;
 Check(Pump([&]{return done.load();}),"rotation finishes before cancelled Drive request returns");task.join();
 auto raw=CloudRaw();
 Check(!listed&&raw&&raw->find("qa-cloud-rotated")!=string::npos&&saves==1&&httpCalls==0,
       "HTTP cancellation preserves rotated credential and skips Drive request");
}
void SchedulerQueuedReplacement(){ResetFacts();SeedCloud();auto*c=App()->GetUserConfig();config_set_bool(c,"GoogleDriveBackup","AutoEnabled",true);config_set_int(c,"GoogleDriveBackup","IntervalDays",1);config_set_int(c,"GoogleDriveBackup","Retention",5);config_set_int(c,"GoogleDriveBackup","LastBackupUnix",71);config_set_string(c,"GoogleDriveBackup","AccountName","qa-A");QString error;bool found=false;thread observer([&]{found=GoogleDriveBackupClient::HasConnection(error);});observer.join();Check(found,"queued scheduler precondition observes original credential session");atomic_int entered{0};atomic_bool release{false};for(int i=0;i<3;++i)MultistreamTaskPool().start([&]{++entered;while(!release)QThread::msleep(1);});Check(Pump([&]{return entered==3;}),"all three workers occupied before scheduler dispatch");OBSBasic basic;basic.RunAutomaticGoogleDriveBackup();Check(basic.googleDriveBackupRunning,"automatic backup actually queued before replacement");OAuthTokenSet tokens;tokens.accessToken="qa-cloud-B";tokens.expiresAt=4102444800;bool stored=false;thread replacement([&]{stored=GoogleDriveBackupClient::StoreTokens(tokens,error);});replacement.join();config_set_string(c,"GoogleDriveBackup","AccountName","qa-B");Check(stored,"replacement credential stored before queued worker starts");release=true;Check(Pump([&]{return !basic.googleDriveBackupRunning;}),"queued scheduler finishes after replacement");Check(archives==0&&httpCalls==0,"queued old-context scheduler never archives or uploads using newer B credential");Check(config_get_int(c,"GoogleDriveBackup","LastBackupUnix")==71,"queued scheduler never overwrites new B timestamp");Check(ioOnGui==0&&configOnWorker==0,"scheduler queue ownership stays worker keyring and GUI config");}
int main(int argc,char**argv){
 QApplication app(argc,argv);
 if(argc>1&&string(argv[1])=="--scheduler-queue"){SchedulerQueuedReplacement();ShutdownMultistreamTasks();QCoreApplication::processEvents();cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"passed\":"<<(failures?"false":"true")<<"}\n";return failures?1:0;}
 QTimer beat;QObject::connect(&beat,&QTimer::timeout,[]{++beats;});beat.start(5);
 DialogCache();ClosePending();PresenceAndRecovery();RefreshRace();DisconnectUi();Scheduler();
 extensionSection=true;HttpCloseAndRotation();extensionSection=false;
 keyringDelay=200;const int previousLoads=loads;auto*d=new GoogleDriveBackupDialog;Check(Pump([&]{return loads.load()>previousLoads;}),"shutdown request prepared");delete d;
 httpDelay=2000;auto stop=MultistreamTaskPool().StopToken();atomic_bool httpDone{false};bool httpSuccess=true;string httpError;
 MultistreamTaskPool().start([&]{OAuthHttpResponse response;httpSuccess=OAuthHttpClient::Get("https://fixture.invalid/private",{},response,httpError,{{},stop});httpDone=true;});
 extensionSection=true;Check(Pump([]{return httpActive.load()>0;}),"HTTP active before pool shutdown");
 QElapsedTimer shutdownTime;shutdownTime.start();ShutdownMultistreamTasks();QCoreApplication::processEvents();
 Check(httpDone&&!httpSuccess&&httpError.find("unconfirmed")!=string::npos&&shutdownTime.elapsed()<300,
       "shutdown cancels HTTP before blocking drain without success");extensionSection=false;
 Check(ioOnGui==0&&configOnWorker==0,"shutdown drains without GUI credential IO");
 auto*rejected=new GoogleDriveBackupDialog;QCoreApplication::processEvents();Check(!Ready(rejected),"post-shutdown task rejected");delete rejected;
 cout<<"{\"checks\":"<<checks<<",\"failures\":"<<failures<<",\"passed\":"<<(failures==0?"true":"false")
     <<",\"original_checks\":"<<originalChecks<<",\"original_failures\":"<<originalFailures<<",\"constructor_ms\":"<<constructorMs<<",\"disconnect_dispatch_ms\":"<<disconnectMs<<",\"scheduler_dispatch_ms\":"<<schedulerMs<<",\"heartbeats\":"<<beats<<"}\n";
 return failures?1:0;
}
