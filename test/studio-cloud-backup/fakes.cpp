/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "facts.hpp"
#include <OBSApp.hpp>
#include <oauth/OAuthTokenSet.hpp>
#include <oauth/OAuthHttpClient.hpp>
#include <oauth/PlatformOAuthClient.hpp>
#include <utility/SecureTokenStore.hpp>
#include <utility/CloudBackupArchive.hpp>
#include <util/base.h>
#include <QCoreApplication>
#include <QThread>
#include <map>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstdarg>
using namespace std;
using namespace std::chrono_literals;
atomic_int keyringDelay{0}, loads{0}, saves{0}, deletes{0}, ioOnGui{0}, configOnWorker{0}, httpCalls{0}, archives{0}, refreshCalls{0};
atomic_bool unavailable{false}, failSave{false}, deleteCommitCancel{false}, blockRefresh{false}, refreshEntered{false}, releaseRefresh{false};
atomic_bool saveCommitCancel{false},allowAuthorization{false},blockExchange{false},exchangeEntered{false},releaseExchange{false};
atomic_int exchangeCalls{0},browserCalls{0};
atomic_bool nextPageOnce{false};
atomic_bool richListExtra{false};
atomic_int externalLoadReplacements{0};
atomic_int httpDownloadCalls{0};
atomic_int stageRestores{0};
atomic_bool richListResponses{false};
atomic_int configSaveFailures{0};
atomic_int keyringActive{0}, httpDelay{0}, httpActive{0}, httpCancelledCalls{0};
struct ActiveKeyring { ActiveKeyring(){++keyringActive;} ~ActiveKeyring(){--keyringActive;} };
struct config_data {map<pair<string,string>,string> fields;};
static config_data config, alternateConfig;
static bool alternateScope=false;
static FakeApp app;
static int archiveRootRevision=0;
bool configUnavailable=false,appUnavailable=false;
static mutex secretMutex;
static optional<string> cloudRaw;
static int replacementLoadNumber=0;
static optional<string> replacementLoadRaw;
static bool Gui(){return QThread::currentThread()==QCoreApplication::instance()->thread();}
void SelectArchiveRootVersion(int value){if(!Gui())++configOnWorker;archiveRootRevision=value;}
static void ConfigAccess(){if(!Gui())++configOnWorker;}
config_t *FakeApp::GetUserConfig(){ConfigAccess();return configUnavailable?nullptr:(alternateScope?&alternateConfig:&config);}
config_t *FakeApp::GetAppConfig(){ConfigAccess();return configUnavailable?nullptr:(alternateScope?&alternateConfig:&config);}
FakeApp *App(){return appUnavailable?nullptr:&app;}
extern "C" const char *config_get_string(config_t *c,const char*s,const char*k){ConfigAccess();auto i=c->fields.find({s,k});return i==c->fields.end()?nullptr:i->second.c_str();}
extern "C" bool config_has_user_value(config_t*c,const char*s,const char*k){ConfigAccess();return c->fields.count({s,k});}
extern "C" int64_t config_get_int(config_t*c,const char*s,const char*k){auto v=config_get_string(c,s,k);return v?stoll(v):0;}
extern "C" bool config_get_bool(config_t*c,const char*s,const char*k){auto v=config_get_string(c,s,k);return v&&(string(v)=="true"||string(v)=="1");}
extern "C" void config_set_string(config_t*c,const char*s,const char*k,const char*v){ConfigAccess();c->fields[{s,k}]=v;}
extern "C" void config_set_int(config_t*c,const char*s,const char*k,int64_t v){config_set_string(c,s,k,to_string(v).c_str());}
extern "C" void config_set_bool(config_t*c,const char*s,const char*k,bool v){config_set_string(c,s,k,v?"true":"false");}
extern "C" bool config_remove_value(config_t*c,const char*s,const char*k){ConfigAccess();return c->fields.erase({s,k});}
extern "C" int config_save_safe(config_t*,const char*,const char*){ConfigAccess();if(configSaveFailures>0){--configSaveFailures;return -1;}return CONFIG_SUCCESS;}
extern "C" void blog(int,const char*,...){}
void SeedCloud(bool expired){
 lock_guard lock(secretMutex);
 cloudRaw=string("{ \"access_token\":\"qa-cloud\", \"refresh_token\":\"qa-refresh\", \"expires_at\":")+(expired?"1":"4102444800")+", \"extra\":\"raw-preserved\" }";
}
optional<string> CloudRaw(){lock_guard lock(secretMutex);return cloudRaw;}
void SetCloudReplacementAtLoad(int number,string raw){lock_guard lock(secretMutex);replacementLoadNumber=number;replacementLoadRaw=std::move(raw);}
void ReplaceCloudRaw(optional<string> raw){lock_guard lock(secretMutex);cloudRaw=std::move(raw);}
void ResetFacts(){
 archiveRootRevision=0;
 nextPageOnce=false;richListExtra=false;externalLoadReplacements=0;{lock_guard lock(secretMutex);replacementLoadNumber=0;replacementLoadRaw.reset();}
 saveCommitCancel=allowAuthorization=blockExchange=exchangeEntered=releaseExchange=false;exchangeCalls=browserCalls=0;
 httpDownloadCalls=0;stageRestores=0;configSaveFailures=0;richListResponses=false;configUnavailable=appUnavailable=false;
 loads=saves=deletes=ioOnGui=configOnWorker=httpCalls=archives=refreshCalls=0;
 keyringDelay=httpDelay=httpCancelledCalls=0;unavailable=failSave=deleteCommitCancel=blockRefresh=refreshEntered=releaseRefresh=false;
 config.fields.clear();alternateConfig.fields.clear();SelectCloudConfigScope(false);{lock_guard lock(secretMutex);cloudRaw.reset();}
}
static bool Delay(const shared_ptr<SecureTokenStore::RequestContext>&ctx,string&error){
 const auto end=chrono::steady_clock::now()+chrono::milliseconds(keyringDelay.load());
 while(chrono::steady_clock::now()<end){
  if(ctx->IsCancelled()||chrono::steady_clock::now()>=ctx->GetDeadline()){error="Private fixture request interrupted.";return false;}
  this_thread::sleep_for(2ms);
 }return true;
}
bool SecureTokenStore::Save(const string&,const string&,const string&value,string&error){
 ActiveKeyring active;
 ++saves;if(Gui())++ioOnGui;auto ctx=RequestForCurrentThread();
 if(!RequestMayStart(ctx,error)||!Delay(ctx,error))return false;
 if(failSave){error="Private fixture save failed.";ctx->status=Status::Failed;return false;}
 {lock_guard lock(secretMutex);cloudRaw=value;}
 if(saveCommitCancel.exchange(false)){ctx->Cancel();ctx->status=Status::Cancelled;error="Private fixture Save reply cancelled after commit.";return false;}
 ctx->status=Status::Success;error.clear();return true;
}
optional<string> SecureTokenStore::Load(const string&,const string&,string&error){
 ActiveKeyring active;
 const int loadNumber=++loads;if(Gui())++ioOnGui;auto ctx=RequestForCurrentThread();
 if(!RequestMayStart(ctx,error)||!Delay(ctx,error))return {};
 if(unavailable){error="Private fixture provider unavailable.";ctx->status=Status::Unavailable;return {};}
 lock_guard lock(secretMutex);if(replacementLoadNumber==loadNumber&&replacementLoadRaw){cloudRaw=*replacementLoadRaw;replacementLoadRaw.reset();replacementLoadNumber=0;++externalLoadReplacements;}ctx->status=cloudRaw?Status::Success:Status::NotFound;error.clear();return cloudRaw;
}
bool SecureTokenStore::Remove(const string&,const string&,string&error){
 ActiveKeyring active;
 ++deletes;if(Gui())++ioOnGui;auto ctx=RequestForCurrentThread();
 if(!RequestMayStart(ctx,error)||!Delay(ctx,error))return false;
 {lock_guard lock(secretMutex);cloudRaw.reset();}
 if(deleteCommitCancel.exchange(false)){ctx->Cancel();ctx->status=Status::Cancelled;error="Private fixture reply cancelled after commit.";return false;}
 ctx->status=Status::Success;error.clear();return true;
}
bool PlatformOAuthClient::RefreshTokens(StreamPlatform,const OAuthClientRegistration&,const string&,const OAuthTokenSet&old,OAuthTokenSet&result,string&error){
 ++refreshCalls;refreshEntered=true;
 while(blockRefresh&&!releaseRefresh)this_thread::sleep_for(1ms);
 result=old;result.accessToken="qa-cloud-rotated";result.refreshToken="qa-refresh-rotated";result.expiresAt=4102444800;error.clear();return true;
}
bool PlatformOAuthClient::CreateAuthorizationSessionWithScopes(StreamPlatform,const OAuthClientRegistration&,const string&,const vector<string_view>&,bool,OAuthAuthorizationSession&session,string&error){if(!allowAuthorization){error="No real authorization in fixture.";return false;}session.authorizationUrl="https://fixture.invalid/authorize";session.state="qa-state";session.codeVerifier="qa-verifier";error.clear();return true;}
bool PlatformOAuthClient::ExchangeAuthorizationCode(const OAuthClientRegistration&,const OAuthAuthorizationSession&,const string&,OAuthTokenSet&tokens,string&error){++exchangeCalls;exchangeEntered=true;while(blockExchange&&!releaseExchange)this_thread::sleep_for(1ms);if(!allowAuthorization){error="No real authorization in fixture.";return false;}tokens.accessToken="qa-authorized-access";tokens.refreshToken="qa-authorized-refresh";tokens.expiresAt=4102444800;error.clear();return true;}
static bool HttpDelay(const OAuthHttpClient::Cancellation &cancel, string &error){
 ++httpActive;
 const auto end=chrono::steady_clock::now()+chrono::milliseconds(httpDelay.load());
 while(chrono::steady_clock::now()<end&&!cancel.IsCancelled())this_thread::sleep_for(2ms);
 --httpActive;
 if(cancel.IsCancelled()){
  ++httpCancelledCalls;
  error="The HTTP operation was cancelled; the remote result is unconfirmed.";
  return false;
 }
 return true;
}
bool OAuthHttpClient::Get(const string&url,const Headers&headers,OAuthHttpResponse&response,string&error,const Cancellation &cancel){
 if(url.find("?alt=media")!=string::npos)++httpDownloadCalls;
 if(!HttpDelay(cancel,error))return false;
 ++httpCalls;if(Gui())++ioOnGui;
 response.statusCode=200;
 response.body=url.find("/about?")!=string::npos?R"({"user":{"displayName":"QA Cloud"}})":R"({"files":[]})";
 if(richListResponses&&url.find("/files?")!=string::npos){const bool next=!headers.empty()&&headers.front().find("qa-cloud-B")!=string::npos;response.body=next?R"({"files":[{"id":"qa-item-B","name":"B.obscloud","appProperties":{"schema":"obs-cloud-2"}}]})":R"({"files":[{"id":"qa-item-A","name":"A.obscloud","appProperties":{"schema":"obs-cloud-2"}}]})";}
 if(richListResponses&&richListExtra&&url.find("/files?")!=string::npos)response.body.insert(response.body.size()-2,R"(,{"id":"qa-item-extra","name":"extra.obscloud","appProperties":{"schema":"obs-cloud-2"}})");
 if(richListResponses&&nextPageOnce&&url.find("/files?")!=string::npos){if(url.find("&pageToken=")==string::npos)response.body.insert(1,R"("nextPageToken":"qa-next",)");else{auto pos=response.body.find("qa-item-A");if(pos!=string::npos)response.body.replace(pos,9,"qa-page-2");}}
 error.clear();return true;
}
bool OAuthHttpClient::SendJson(const string&,const string&,const string&,const Headers&,OAuthHttpResponse&response,string&error,const Cancellation &cancel){
 if(!HttpDelay(cancel,error))return false;++httpCalls;response.statusCode=200;response.location="https://fixture.invalid/private";error.clear();return true;}
bool OAuthHttpClient::SendBytes(const string&,const string&,const string&,const string&,const Headers&,OAuthHttpResponse&response,string&error,const Cancellation &cancel){
 if(!HttpDelay(cancel,error))return false;++httpCalls;response.statusCode=200;response.body=R"({"id":"qa-item"})";error.clear();return true;}
string OAuthHttpClient::UrlEncode(const string&value){return value;}
bool CloudBackupArchive::EncryptionSupported(){return true;}
bool CloudBackupArchive::CaptureRoots(CloudBackupRoots &roots,QString &error){
 if(!Gui())++configOnWorker;
 roots.configRoot=QString::fromStdString(app.userConfigLocation.generic_string())+"/obs-studio";
 roots.globalConfigPath=roots.configRoot+"/global.ini";
 roots.profilesRoot=roots.configRoot+"/basic/profiles";
 roots.scenesRoot=roots.configRoot+"/basic/scenes-"+QString::number(archiveRootRevision);
 roots.pluginConfigRoot=roots.configRoot+"/plugin_config";
 roots.pluginManagerRoot=roots.configRoot+"/plugin_manager";
 roots.pendingRoot=roots.configRoot+"/cloud-restore";
 roots.safetyRoot=roots.configRoot+"/backups/cloud";
 roots.coordinationRoot="/fixture/shared-cache";
 roots.obsVersion="synthetic";roots.identity=roots.configRoot+"|"+roots.scenesRoot;error.clear();return true;
}
bool CloudBackupArchive::RootsCurrent(const CloudBackupRoots &roots,QString &error){
 CloudBackupRoots current;CaptureRoots(current,error);
 if(current.identity!=roots.identity){error="Synthetic archive roots changed.";return false;}error.clear();return true;
}
bool CloudBackupArchive::Create(const CloudBackupRoots&,const CloudBackupOptions&,QByteArray&archive,CloudBackupSummary&summary,QString&error,const std::function<bool()> &cancel){
 if(Gui())++ioOnGui;
 if(cancel&&cancel()){error="Archive cancelled.";return false;}
 ++archives;archive="fixture";summary.createdUtc=QDateTime::currentDateTimeUtc();error.clear();return true;
}
bool CloudBackupArchive::StageRestore(const CloudBackupRoots&,const QByteArray&,const CloudBackupOptions&,QString&,QString&error,const std::function<bool()> &cancel){
 if(Gui())++ioOnGui;
 if(cancel&&cancel()){error="Restore cancelled.";return false;}
 ++stageRestores;error="Restore outside fixture scope.";return false;
}

void SelectCloudConfigScope(bool alternate){alternateScope=alternate;app.userConfigLocation=alternate?"/fixture/B":"/fixture/A";}
