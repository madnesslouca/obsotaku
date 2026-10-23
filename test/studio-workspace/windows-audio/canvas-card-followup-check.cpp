/* Opt-in native Windows Settings/card proof. Private empty QA profile only.
 * No output starts, no captures, no accounts and no user host config access. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/config-file.h>
#include <util/bmem.h>
#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDockWidget>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QMainWindow>
#include <QPushButton>
#include <QPixmap>
#include <QSaveFile>
#include <QTimer>
#include <functional>
#include <stdexcept>
OBS_DECLARE_MODULE()
namespace {
QString root, configRoot;
QMainWindow *window = nullptr;
QJsonArray checks;
QElapsedTimer clock;
bool done = false;
void check(bool ok, const char *name)
{
 checks.append(QJsonObject{{"check",name},{"passed",ok}});
 if (!ok) throw std::runtime_error(name);
}
void finish(QString error = {})
{
 if(done) return;
 done = true;
 if(auto *dialog = qobject_cast<QDialog *>(qApp->activeModalWidget())) {
  if(auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox")) {
   if(auto *cancel = buttons->button(QDialogButtonBox::Cancel)) cancel->click();
  }
 }
 QSaveFile out(root+"/runtime.json");
 if(out.open(QIODevice::WriteOnly)) {
  out.write(QJsonDocument(QJsonObject{{"passed",error.isEmpty()},{"error",error},{"checks",checks},
   {"scope","Native Windows Settings real OBS theme; Main/Multi immediate mode with empty private scene, no active output, no DPI/editor/media certification"}}).toJson());out.commit();
 }
}
void later(int ms, std::function<void()> fn)
{
 QTimer::singleShot(ms,qApp,[fn]{if(done)return;try{fn();}catch(const std::exception &e){finish(QString::fromUtf8(e.what()));}});
}
void waitSettings()
{
 auto *d = qobject_cast<QDialog *>(qApp->activeModalWidget());
 if(!d || d->objectName()!="OBSBasicSettings" || !d->isVisible()) {
  check(clock.elapsed()<20000,"actual Settings becomes visible outside its constructor");
  later(100,waitSettings);return;
 }
 auto *list = d->findChild<QListWidget *>("listWidget");
 check(list && list->count()>0,"actual Settings categories ready");list->setCurrentRow(0);
 auto *single=d->findChild<QPushButton *>("studioCanvasSingle");
 auto *multi=d->findChild<QPushButton *>("studioCanvasMulti");
 check(single && multi,"both production Main and Multi cards exist");
 check(single->isChecked() && !multi->isChecked(),"fresh private profile opens in Main mode");
 check(d->grab().save(root+"/settings-main-before.png"),"actual Main Settings screenshot saved");
 multi->click();
 check(config_get_bool(obs_frontend_get_user_config(),"BasicWindow","StudioMultiCanvas"),"Multi preference changes synchronously before Apply");
 later(700,[d,single,multi]{
  check(multi->isChecked() && !single->isChecked(),"actual Multi card immediately checked");
  auto *dock=window->findChild<QDockWidget *>("verticalPreviewDock");
  check(dock && dock->isVisible(),"real Portrait preview visible before Apply");
  obs_frontend_canvas_list canvases{};obs_frontend_get_canvases(&canvases);bool found=false;
  for(size_t i=0;i<canvases.canvases.num;++i){
   auto *c=canvases.canvases.array[i];
   if(QString::fromUtf8(obs_canvas_get_name(c))=="Vertical 9:16"){
    obs_video_info video{};
    found=obs_canvas_get_video_info(c,&video) && video.base_width==1080 && video.base_height==1920;
   }
  }
  obs_frontend_canvas_list_free(&canvases);
  check(found,"actual Portrait canvas has native video before Apply");
  check(d->grab().save(root+"/settings-multi-immediate.png"),"actual Multi Settings screenshot saved");
  single->click();
  check(!config_get_bool(obs_frontend_get_user_config(),"BasicWindow","StudioMultiCanvas"),"Main preference changes synchronously before Apply");
  later(600,[d,single,multi]{
   check(single->isChecked() && !multi->isChecked(),"actual Main card immediately checked again");
   auto *dock=window->findChild<QDockWidget *>("verticalPreviewDock");
   check(dock && !dock->isVisible(),"actual Portrait preview hidden before Apply");
   check(!obs_frontend_streaming_active() && !obs_frontend_recording_active() && !obs_frontend_replay_buffer_active(),"all frontend outputs remain idle through gestures");
   check(d->grab().save(root+"/settings-main-restored.png"),"actual restored Main Settings screenshot saved");
   finish();
  });
 });
}
void run()
{
 window=static_cast<QMainWindow *>(obs_frontend_get_main_window());
 check(window!=nullptr,"native main window available");
 char *p=obs_frontend_get_current_profile_path();const QString actual=QFileInfo(QString::fromUtf8(p)).canonicalFilePath();bfree(p);
 const QString expected=QFileInfo(configRoot).canonicalFilePath();
 check(!expected.isEmpty() && actual.startsWith(expected+'/'),"effective profile confined to private QA root");
 check(!obs_video_active(),"all outputs idle before Settings gesture");
 check(!config_get_bool(obs_frontend_get_user_config(),"BasicWindow","StudioMultiCanvas"),"Main baseline is explicitly required");
 auto *action=window->findChild<QAction *>("action_Settings");check(action!=nullptr,"native Settings action exists");
 clock.start();later(100,waitSettings);action->trigger();
}
void event(obs_frontend_event e,void *){if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING)later(1800,run);}
}
bool obs_module_load()
{
 root=qEnvironmentVariable("OBS_STUDIO_WINDOWS_CARD_ROOT");configRoot=qEnvironmentVariable("OBS_STUDIO_WINDOWS_CARD_CONFIG_ROOT");
 if(root.isEmpty() || configRoot.isEmpty() || QFileInfo::exists(root+"/runtime.json") || !QDir().mkpath(root))return false;
 obs_frontend_add_event_callback(event,nullptr);later(45000,[]{finish("actual Windows card gesture timed out");});return true;
}
void obs_module_unload(){obs_frontend_remove_event_callback(event,nullptr);}
