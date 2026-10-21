/* Opt-in native Qt + libobs VOD routing fixture. No network outputs are started. */
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>
OBS_DECLARE_MODULE()
static QString reportPath, originalUUID;
static QMainWindow *window;
static QPointer<QWidget> strip;
static OBSSourceAutoRelease music, voice, scene;
static QTimer *timer;
static QByteArray layout;
static bool finished, studioMode;
static uint64_t sampleIndex;
static uint32_t vodMask, liveMask, initialMask;
static size_t liveMix, vodMix;
static QJsonArray checks;
static QJsonObject facts;
struct Spectrum { double voice=0, music=0; uint64_t frames=0; };
static Spectrum energy[6], beforeLive, beforeVod;
static std::mutex energyMutex;
static std::vector<size_t> connected;
static void capture(void *, size_t mix, audio_data *data)
{
    if (!data->frames || !data->data[0] || mix >= 6) return;
    const float *samples = reinterpret_cast<const float *>(data->data[0]);
    double vr=0,vi=0,mr=0,mi=0;
    for (uint32_t i=0;i<data->frames;++i) {
        const double angle=2*3.141592653589793*i/48000;
        vr += samples[i]*std::cos(angle*440); vi += samples[i]*std::sin(angle*440);
        mr += samples[i]*std::cos(angle*1000); mi += samples[i]*std::sin(angle*1000);
    }
    std::lock_guard lock(energyMutex);
    energy[mix].voice += (vr*vr+vi*vi)/data->frames;
    energy[mix].music += (mr*mr+mi*mi)/data->frames;
    energy[mix].frames += data->frames;
}
static Spectrum spectrum(size_t mix)
{
    std::lock_guard lock(energyMutex); Spectrum result=energy[mix];
    if (result.frames) { result.voice/=result.frames; result.music/=result.frames; }
    return result;
}
static void clearEnergy() { std::lock_guard lock(energyMutex); for (auto &entry:energy) entry={}; }
static void check(bool ok,const char *label)
{
    checks.append(QJsonObject{{"check",label},{"passed",ok}});
    if(!ok) throw std::runtime_error(label);
}
static void finish(const QString &error={})
{
    if(finished) return;
    finished=true;
    if(timer) { timer->stop(); delete timer; timer=nullptr; }
    for(auto mix:connected) audio_output_disconnect(obs_get_audio(),mix,capture,nullptr);
    connected.clear();
    OBSSourceAutoRelease original=obs_get_source_by_uuid(originalUUID.toUtf8().constData());
    if(original) obs_frontend_set_current_scene(original);
    if(studioMode) obs_frontend_set_preview_program_mode(true);
    if(scene) obs_source_remove(scene);
    if(music) obs_source_remove(music);
    if(voice) obs_source_remove(voice);
    scene=nullptr; music=nullptr; voice=nullptr;
    window->restoreState(layout);
    obs_frontend_save();
    QFile file(reportPath);
    if(file.open(QIODevice::WriteOnly)) file.write(QJsonDocument(QJsonObject{
        {"passed",error.isEmpty()},{"error",error},{"checks",checks},{"facts",facts},
        {"scope","native Qt pointer controls and actual libobs mixed PCM; two synthetic tones, no recording or public streaming, user inputs untouched"}
    }).toJson());
}
static void later(int ms,std::function<void()> fn)
{
    QTimer::singleShot(ms,qApp,[fn]{if(finished)return;try{fn();}catch(const std::exception &e){finish(e.what());}});
}
static QPushButton *button()
{
    auto *button=window->findChild<QPushButton *>("studioAudioVodExclude");
    check(button && button->isEnabled(),"native VOD control enabled");
    for(auto *parent=button->parentWidget();parent;parent=parent->parentWidget())
        if(auto *scroll=qobject_cast<QScrollArea *>(parent)) scroll->ensureWidgetVisible(button);
    return button;
}
static void click(QWidget *widget)
{
    check(widget && widget->isVisible() && widget->visibleRegion().contains(widget->rect().center()),"pointer target visible and not clipped");
    QTest::mouseClick(widget,Qt::LeftButton,Qt::NoModifier,widget->rect().center());
}
static void action(const char *name)
{
    auto *item=window->findChild<QAction *>(name); check(item && item->isEnabled(),name); item->trigger();
}
static void unchanged()
{
    check((obs_source_get_audio_mixers(music)&liveMask)==(initialMask&liveMask),"live routing preserved");
    check(!obs_source_muted(music) && obs_source_get_volume(music)==1.0f && obs_source_get_monitoring_type(music)==OBS_MONITORING_TYPE_NONE,"VOD toggle preserves mute gain and CUE");
}
static void excluded()
{
    check((obs_source_get_audio_mixers(music)&vodMask)==0,"all intended VOD bits excluded");
    auto *badge=strip->findChild<QLabel *>("studioMixerVodBadge");
    check(badge && badge->isVisible() && badge->text()=="VOD-EXCL","native mixer shows VOD-EXCL badge");
    auto *list=strip->findChild<QListWidget *>("studioMixerEffectList");
    check(list && list->count()==4,"four real effects coexist with VOD badge");
    for(int row=0;row<4;++row) check(list->viewport()->rect().contains(list->visualItemRect(list->item(row))),"effect row remains fully visible below VOD badge");
    for(const char *id:{"studioMixerFader","studioMixerEdit","studioMixerDevice"}) {
        auto *widget=strip->findChild<QWidget *>(id);
        check(widget && widget->isVisible() && widget->visibleRegion().contains(widget->rect().center()),"mixer controls remain visible with VOD badge and effects");
    }
    unchanged();
}
static size_t stepIndex;
static std::vector<std::function<void()>> steps;
static void next()
{
    later(1100,[]{if(stepIndex==steps.size()){finish();return;}steps[stepIndex++]();next();});
}
static void run()
{
    window=static_cast<QMainWindow *>(obs_frontend_get_main_window());layout=window->saveState();
    check(!obs_frontend_recording_active()&&!obs_frontend_streaming_active()&&!obs_frontend_replay_buffer_active(),"all outputs idle");
    OBSSourceAutoRelease original=obs_frontend_get_current_scene();originalUUID=obs_source_get_uuid(original);
    studioMode=obs_frontend_preview_program_mode_active();if(studioMode)obs_frontend_set_preview_program_mode(false);
    music=obs_source_create("studio_vod_test_input","Studio VOD QA music",nullptr,nullptr);
    voice=obs_source_create("studio_vod_test_input","Studio VOD QA voice",nullptr,nullptr);
    check(music && voice,"two synthetic audio inputs created");
    // Four unity filters exercise badge height with a populated rack without
    // changing the tones used by the routing measurement.
    for(int index=0;index<4;++index) {
        const auto label=QString("QA unity gain %1").arg(index+1).toUtf8();
        OBSSourceAutoRelease filter=obs_source_create("gain_filter",label.constData(),nullptr,nullptr);
        check(bool(filter),"real native unity filter created");obs_source_filter_add(music,filter);
    }
    OBSSceneAutoRelease fixture=obs_scene_create("Studio VOD QA scene");scene=obs_source_get_ref(obs_scene_get_source(fixture));
    obs_scene_add(fixture,music);obs_scene_add(fixture,voice);
    obs_source_set_audio_mixers(music,63);obs_source_set_audio_mixers(voice,63);initialMask=obs_source_get_audio_mixers(music);
    timer=new QTimer(qApp);timer->setInterval(20);timer->setTimerType(Qt::PreciseTimer);
    QObject::connect(timer,&QTimer::timeout,qApp,[]{
        float a[960],b[960];
        for(size_t i=0;i<960;++i){a[i]=.12f*std::sin(2*3.141592653589793*1000*(sampleIndex+i)/48000);b[i]=.12f*std::sin(2*3.141592653589793*440*(sampleIndex+i)/48000);}
        obs_source_audio audio{};audio.frames=960;audio.samples_per_sec=48000;audio.speakers=SPEAKERS_MONO;audio.format=AUDIO_FORMAT_FLOAT;audio.timestamp=os_gettime_ns();
        audio.data[0]=reinterpret_cast<uint8_t *>(a);obs_source_output_audio(music,&audio);
        audio.data[0]=reinterpret_cast<uint8_t *>(b);obs_source_output_audio(voice,&audio);sampleIndex+=960;
    });
    timer->start();obs_frontend_set_current_scene(scene);
    steps={
        []{
            for(auto *widget:window->findChildren<QWidget *>())if(QByteArray(widget->metaObject()->className())=="VolumeControl"&&widget->objectName()=="Studio VOD QA music")strip=widget;
            check(strip && strip->isVisible(),"test music has native mixer strip");
            click(strip->findChild<QPushButton *>("studioMixerEdit"));
        },
        []{
            auto *b=button();vodMask=b->property("studioVodMask").toUInt();liveMask=b->property("studioLiveMask").toUInt();
            check(vodMask && liveMask && !(vodMask&liveMask),"UI uses disjoint actual live and VOD plan");
            facts["vod_mask"]=int(vodMask);facts["live_mask"]=int(liveMask);
            for(liveMix=0;!(liveMask&(1u<<liveMix));++liveMix){}
            for(vodMix=0;!(vodMask&(1u<<vodMix));++vodMix){}
            audio_convert_info convert{};convert.samples_per_sec=48000;convert.format=AUDIO_FORMAT_FLOAT_PLANAR;convert.speakers=SPEAKERS_STEREO;
            for(auto mix:{liveMix,vodMix}){check(audio_output_connect(obs_get_audio(),mix,&convert,capture,nullptr),"native mixed PCM callback connected");connected.push_back(mix);}
        },
        []{clearEnergy();},
        []{
            beforeLive=spectrum(liveMix);beforeVod=spectrum(vodMix);
            check(beforeLive.frames>20000 && beforeVod.frames>20000,"baseline contains native mixed audio frames");
            check(beforeLive.music>1e-5 && beforeLive.voice>1e-5 && beforeVod.music>1e-5 && beforeVod.voice>1e-5,"both tones present in live and VOD before exclusion");
            click(button());check(obs_source_get_audio_mixers(music)==(initialMask&~vodMask),"pointer excludes only intended VOD bits");
        },
        []{excluded();window->grab().save(reportPath+".window.png");strip->grab().save(reportPath+".strip.png");clearEnergy();},
        []{
            const auto live=spectrum(liveMix),vod=spectrum(vodMix);
            facts["clean_music_power_ratio"]=vod.music/beforeVod.music;facts["live_music_power_ratio"]=live.music/beforeLive.music;facts["clean_voice_power_ratio"]=vod.voice/beforeVod.voice;
            check(vod.frames>20000 && live.frames>20000,"excluded mix contains native audio frames");
            check(vod.music<beforeVod.music*.08,"music removed from native clean PCM mix");
            check(live.music>beforeLive.music*.2,"music still present in native live PCM mix");
            check(vod.voice>beforeVod.voice*.2,"voice remains in native clean PCM mix");
            action("actionMainUndo");check(obs_source_get_audio_mixers(music)==initialMask,"Undo restores VOD routing");
        },
        []{
            check(!strip->findChild<QLabel *>("studioMixerVodBadge")->isVisible(),"Undo removes VOD badge");
            action("actionMainRedo");
        },
        []{
            excluded();
            uint32_t unrelated=(~(vodMask|liveMask))&63;
            check(unrelated!=0,"fixture has unrelated recording track");
            const uint32_t bit=unrelated&(~unrelated+1);facts["unrelated_recording_bit"]=int(bit);
            obs_source_set_audio_mixers(music,obs_source_get_audio_mixers(music)^bit);
            action("actionMainUndo");check(obs_source_get_audio_mixers(music)==(initialMask^bit),"Undo preserves unrelated recording routing edit");
            action("actionMainRedo");check(obs_source_get_audio_mixers(music)==((initialMask^bit)&~vodMask),"Redo preserves unrelated recording routing edit");
        },
        []{excluded();click(button());check((obs_source_get_audio_mixers(music)&vodMask)==vodMask,"pointer includes VOD again");unchanged();},
        []{check(!strip->findChild<QLabel *>("studioMixerVodBadge")->isVisible(),"including VOD clears badge");}
    };next();
}
static const char *name(void *){return "Studio VOD test input";}
static void *create(obs_data_t *,obs_source_t *source){return source;}
static void destroy(void *){}
static void event(obs_frontend_event value,void *){if(value==OBS_FRONTEND_EVENT_FINISHED_LOADING)later(1800,run);}
bool obs_module_load()
{
    reportPath=qEnvironmentVariable("OBS_STUDIO_VOD_REPORT");if(reportPath.isEmpty())return false;
    obs_source_info info{};info.id="studio_vod_test_input";info.type=OBS_SOURCE_TYPE_INPUT;info.output_flags=OBS_SOURCE_AUDIO;info.get_name=name;info.create=create;info.destroy=destroy;obs_register_source(&info);
    obs_frontend_add_event_callback(event,nullptr);return true;
}
void obs_module_unload()
{
    obs_frontend_remove_event_callback(event,nullptr);
    if(timer){timer->stop();delete timer;timer=nullptr;}
    for(auto mix:connected)audio_output_disconnect(obs_get_audio(),mix,capture,nullptr);
    connected.clear();music=nullptr;voice=nullptr;scene=nullptr;
}
