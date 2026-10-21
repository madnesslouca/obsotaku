// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in integration fixture: actual mixer clicks, SDK VST3s, PCM, and restart.
#include <obs-module.h>
#include <obs-frontend-api.h>
#include <obs.hpp>
#include <util/platform.h>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QListWidget>
#include <QMainWindow>
#include <QPointer>
#include <QPushButton>
#include <QTabBar>
#include <QScrollArea>
#include <QStyleOptionViewItem>
#include <QStyle>
#include <QTest>
#include <QTimer>
#include <cmath>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <vector>
#ifdef __linux__
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#endif
OBS_DECLARE_MODULE()
static QString path,phase,original, gainUUID,secondUUID,paramKey,secondKey;
static QMainWindow *window;
static QPointer<QWidget> strip;
static OBSSourceAutoRelease source,scene;
static QTimer *audioTimer;
static QJsonObject facts,saveState;
static QJsonArray checks;
static QByteArray layout;
static bool finished,studioMode;
static uint64_t sampleIndex;
static std::mutex powerMutex;
static double sumPower,baseline;
static uint64_t frames;
static void check(bool value,const char *message){checks.append(QJsonObject{{"check",message},{"passed",value}});if(!value)throw std::runtime_error(message);}
static void capture(void *,obs_source_t *,const audio_data *audio,bool muted)
{
    if(muted||!audio->data[0])return;
    const auto *samples=reinterpret_cast<const float *>(audio->data[0]);double power=0;
    for(uint32_t i=0;i<audio->frames;++i)power+=samples[i]*samples[i];
    std::lock_guard lock(powerMutex);sumPower+=power;frames+=audio->frames;
}
static double rms(){std::lock_guard lock(powerMutex);check(frames>15000,"native filtered PCM frames received");return std::sqrt(sumPower/frames);}
static void resetPower(){std::lock_guard lock(powerMutex);sumPower=0;frames=0;}
static void write(const QString &file,const QJsonObject &data){QFile f(file);if(f.open(QIODevice::WriteOnly))f.write(QJsonDocument(data).toJson());}
static void finish(const QString &error={})
{
    if(finished)return;finished=true;
    if(auto *popup=QApplication::activePopupWidget())popup->close();
    if(audioTimer){audioTimer->stop();delete audioTimer;audioTimer=nullptr;}
    if(source)obs_source_remove_audio_capture_callback(source,capture,nullptr);
    if(phase=="save" && error.isEmpty()){
        facts["fixture_preserved_for_restart"]=true;
        write(path+".state.json",QJsonObject{{"original",original},{"layout",QString::fromLatin1(layout.toBase64())},{"studio_mode",studioMode},{"gain_uuid",gainUUID},{"second_uuid",secondUUID},{"param_key",paramKey},{"second_key",secondKey},{"baseline",baseline}});
    }else{
        OBSSourceAutoRelease previous=obs_get_source_by_uuid(original.toUtf8().constData());if(previous)obs_frontend_set_current_scene(previous);
        if(studioMode)obs_frontend_set_preview_program_mode(true);
        if(scene)obs_source_remove(scene);if(source)obs_source_remove(source);window->restoreState(layout);
    }
    obs_frontend_save();
    source=nullptr;scene=nullptr;
    write(path+"."+phase+".json",QJsonObject{{"passed",error.isEmpty()},{"error",error},{"checks",checks},{"facts",facts},{"scope","Actual mixer Qt clicks, SDK native VST3 effects, filtered PCM and saved collection across a real OBS restart; no output started or user audio settings changed"}});
}
static void later(int ms,std::function<void()> fn){QTimer::singleShot(ms,qApp,[fn]{if(finished)return;try{fn();}catch(const std::exception &e){finish(e.what());}});}
static void click(QWidget *widget){check(widget && widget->isVisible() && widget->isEnabled(),"pointer target visible and enabled");check(widget->visibleRegion().contains(widget->rect().center()),"pointer target not clipped");QTest::mouseClick(widget,Qt::LeftButton,Qt::NoModifier,widget->rect().center());}
static OBSSource filter(const QString &uuid){OBSSourceAutoRelease result=obs_get_source_by_uuid(uuid.toUtf8().constData());return result.Get();}
static void action(const char *name){auto *a=window->findChild<QAction *>(name);check(a&&a->isEnabled(),name);a->trigger();}
static QWidget *editor(){auto *e=window->findChild<QWidget *>("studioEffectEditor");check(e&&e->isVisible(),"adjacent editor visible");return e;}
static QString add(const QString &prefix)
{
    click(strip->findChild<QPushButton *>("studioMixerAddEffect"));
    auto *picker=window->findChild<QWidget *>("studioEffectPicker");check(picker&&picker->isVisible(),"mixer plus opens effect picker");
    auto *tabs=picker->findChild<QTabBar *>("studioEffectTabs");check(tabs&&tabs->count()==3&&tabs->tabText(2)=="VST3","real VST3 catalogue tab available");
    QTest::mouseClick(tabs,Qt::LeftButton,Qt::NoModifier,tabs->tabRect(2).center());
    auto *list=picker->findChild<QListWidget *>("studioEffectCatalog");QListWidgetItem *item=nullptr;int visible=0;
    for(int row=0;row<list->count();++row){auto *candidate=list->item(row);if(candidate->isHidden())continue;++visible;
        check(candidate->data(Qt::UserRole).toString()=="vst3_filter"&&!candidate->data(Qt::UserRole+2).toString().isEmpty(),"VST3 row refers to a real scanned class");
        if(candidate->text().startsWith(prefix))item=candidate;
    }
    facts["vst3_effects_in_catalogue"]=visible;check(item,"requested SDK effect found");
    const auto id=item->data(Qt::UserRole+2).toString();list->scrollToItem(item);picker->grab().save(path+".picker.png");
    QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualItemRect(item).center());
    struct Search{QString id,uuid;}search{id,{}};
    obs_source_enum_filters(source,[](obs_source_t *,obs_source_t *f,void *opaque){auto *s=static_cast<Search *>(opaque);OBSDataAutoRelease d=obs_source_get_settings(f);if(QString::fromUtf8(obs_data_get_string(d,"vst3_plugin"))==s->id)s->uuid=obs_source_get_uuid(f);},&search);
    check(!search.uuid.isEmpty(),"picker adds the selected VST3 to native filter chain");return search.uuid;
}
static void open(const QString &uuid)
{
    auto *list=strip->findChild<QListWidget *>("studioMixerEffectList");QListWidgetItem *item=nullptr;
    for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==uuid)item=list->item(row);
    check(item,"VST3 appears in mixer rack");QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,list->visualItemRect(item).center());
}
static QString parameter(const QString &uuid)
{
    OBSDataAutoRelease settings=obs_source_get_settings(filter(uuid));obs_data_item_t *item=obs_data_first(settings);
    QString found;
    while(item){QString key=obs_data_item_get_name(item);if(key.startsWith("vst3_param_")&&key.endsWith("_0"))found=key;obs_data_item_next(&item);}
    check(!found.isEmpty(),"real controller parameter zero exposed");return found;
}
static double value(const QString &uuid,const QString &key);
static void setSpin(const QString &key,const QString &value)
{
    auto *spin=editor()->findChild<QDoubleSpinBox *>(key);check(spin,"VST3 parameter has native property widget");
    for(auto *parent=spin->parentWidget();parent;parent=parent->parentWidget())if(auto *scroll=qobject_cast<QScrollArea *>(parent))scroll->ensureWidgetVisible(spin);
    click(spin);QTest::keyClick(spin,Qt::Key_A,Qt::ControlModifier);QTest::keyClicks(spin,spin->locale().toString(value.toDouble(),'f',3));QTest::keyClick(spin,Qt::Key_Return);
    facts["last_spin_value"]=spin->value();facts["last_spin_text"]=spin->text();facts["last_spin_key"]=key;facts["last_gain_setting_during_edit"]=::value(gainUUID,paramKey);
    editor()->grab().save(path+".editing-debug.png");
}
static double value(const QString &uuid,const QString &key){OBSDataAutoRelease settings=obs_source_get_settings(filter(uuid));return obs_data_get_double(settings,key.toUtf8().constData());}
static void bypassSecond()
{
    auto *list=strip->findChild<QListWidget *>("studioMixerEffectList");QListWidgetItem *item=nullptr;
    for(int row=0;row<list->count();++row)if(list->item(row)->data(Qt::UserRole).toString()==secondUUID)item=list->item(row);
    check(item,"second VST3 is in inline chain");QStyleOptionViewItem option;option.initFrom(list->viewport());option.rect=list->visualItemRect(item);option.features=QStyleOptionViewItem::HasCheckIndicator;
    QRect rect=list->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator,&option,list->viewport());QTest::mouseClick(list->viewport(),Qt::LeftButton,Qt::NoModifier,rect.center());
    check(!obs_source_enabled(filter(secondUUID)),"inline bypass disables second real VST3");
}
static void verifyState(const QString &uuid)
{
    auto f=filter(uuid);check(bool(f),"saved filter UUID exists");OBSDataAutoRelease data=obs_save_source(f);OBSDataAutoRelease settings=obs_data_get_obj(data,"settings");
    check(strlen(obs_data_get_string(settings,"vst3_state"))>0,"opaque VST3 processor state serialized");
    check(strlen(obs_data_get_string(settings,"vst3_path"))>0,"native module path serialized");
}
static void captureNativeEditor()
{
#ifdef __linux__
    Display *display=XOpenDisplay(nullptr);check(display,"X11/XWayland display available");
    std::function<Window(Window,int)> find=[&](Window parent,int depth)->Window {
        if(depth>4)return 0;
        char *name=nullptr;XFetchName(display,parent,&name);
        const bool match=name && QString::fromUtf8(name).contains("VST3 Plugin - AGain");if(name)XFree(name);
        if(match)return parent;
        Window root,actualParent,*children=nullptr;unsigned int count=0;Window found=0;
        if(XQueryTree(display,parent,&root,&actualParent,&children,&count))for(unsigned int i=0;i<count&&!found;++i)found=find(children[i],depth+1);
        if(children)XFree(children);return found;
    };
    Window native=find(DefaultRootWindow(display),0);check(native,"real VST3 native editor window exists");
    XWindowAttributes attributes{};XGetWindowAttributes(display,native,&attributes);
    check(attributes.map_state==IsViewable&&attributes.width>200&&attributes.height>100,"native VST3 editor mapped with real content dimensions");
    XImage *pixels=XGetImage(display,native,0,0,attributes.width,attributes.height,AllPlanes,ZPixmap);check(pixels,"native VST3 pixels available");
    QImage image(attributes.width,attributes.height,QImage::Format_RGB32);
    auto channel=[](unsigned long pixel,unsigned long mask){if(!mask)return 0ul;while(!(mask&1)){mask>>=1;pixel>>=1;}return (pixel&mask)*255/mask;};
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x){auto pixel=XGetPixel(pixels,x,y);image.setPixel(x,y,qRgb(channel(pixel,pixels->red_mask),channel(pixel,pixels->green_mask),channel(pixel,pixels->blue_mask)));}
    facts["native_red_mask"]=double(pixels->red_mask);facts["native_first_raw_pixel"]=double(XGetPixel(pixels,0,0));
    XDestroyImage(pixels);image.save(path+".native-editor.png");
    bool rendered=false;const auto first=image.pixel(0,0);for(int y=0;y<image.height()&&!rendered;y+=8)for(int x=0;x<image.width();x+=8)if(image.pixel(x,y)!=first){rendered=true;break;}
    // Compositors may redirect an embedded child into its own pixmap, leaving
    // the host parent's backing store black. Inspect the plug-in's drawable.
    QJsonArray childrenFacts;
    std::function<void(Window,int)> inspect=[&](Window parent,int depth){
        if(depth>3)return;Window root,owner,*children=nullptr;unsigned int count=0;
        if(!XQueryTree(display,parent,&root,&owner,&children,&count))return;
        for(unsigned int i=0;i<count;++i){XWindowAttributes a{};XGetWindowAttributes(display,children[i],&a);
            QJsonObject fact{{"id",double(children[i])},{"width",a.width},{"height",a.height},{"mapped",a.map_state==IsViewable}};
            if(a.map_state==IsViewable && a.depth>0 && a.width>50 && a.height>30){
                XImage *data=XGetImage(display,children[i],0,0,a.width,a.height,AllPlanes,ZPixmap);
                if(data){QImage child(a.width,a.height,QImage::Format_RGB32);bool varying=false;auto initial=XGetPixel(data,0,0);
                    for(int y=0;y<a.height;++y)for(int x=0;x<a.width;++x){auto pixel=XGetPixel(data,x,y);varying|=pixel!=initial;child.setPixel(x,y,qRgb(channel(pixel,data->red_mask),channel(pixel,data->green_mask),channel(pixel,data->blue_mask)));}
                    child.save(path+QString(".native-child-%1.png").arg(depth));fact["varying_pixels"]=varying;fact["first_raw_pixel"]=double(initial);XDestroyImage(data);
                    if(varying){rendered=true;child.save(path+".native-editor.png");}
                }
            }
            childrenFacts.append(fact);inspect(children[i],depth+1);
        }
        if(children)XFree(children);
    };inspect(native,0);facts["native_children"]=childrenFacts;
    check(rendered,"native VST3 editor renders nonempty pixels under Wayland/X11");
    XEvent message{};message.xclient.type=ClientMessage;message.xclient.window=native;message.xclient.message_type=XInternAtom(display,"WM_PROTOCOLS",False);message.xclient.format=32;message.xclient.data.l[0]=XInternAtom(display,"WM_DELETE_WINDOW",False);
    XSendEvent(display,native,False,NoEventMask,&message);XFlush(display);XCloseDisplay(display);
    facts["native_editor_rendered"]=true;
#else
    QWidget *native=nullptr;for(auto *widget:QApplication::topLevelWidgets())if(widget->isVisible()&&widget->windowTitle().contains("VST3 Plugin - AGain"))native=widget;
    check(native,"real VST3 native editor visible");native->grab().save(path+".native-editor.png");native->close();facts["native_editor_rendered"]=true;
#endif
}
static std::vector<std::function<void()>> steps;static size_t stepIndex;
static void next(){later(1000,[]{if(stepIndex==steps.size()){finish();return;}steps[stepIndex++]();next();});}
static void run()
{
    window=static_cast<QMainWindow *>(obs_frontend_get_main_window());check(!obs_frontend_recording_active()&&!obs_frontend_streaming_active()&&!obs_frontend_replay_buffer_active(),"all outputs idle");
    if(phase=="restore"){
        QFile f(path+".state.json");check(f.open(QIODevice::ReadOnly),"restart baseline exists");saveState=QJsonDocument::fromJson(f.readAll()).object();
        original=saveState["original"].toString();layout=QByteArray::fromBase64(saveState["layout"].toString().toLatin1());studioMode=saveState["studio_mode"].toBool();
        gainUUID=saveState["gain_uuid"].toString();secondUUID=saveState["second_uuid"].toString();paramKey=saveState["param_key"].toString();secondKey=saveState["second_key"].toString();baseline=saveState["baseline"].toDouble();
        source=obs_get_source_by_name("Studio VST3 QA input");scene=obs_get_source_by_name("Studio VST3 QA scene");check(source&&scene,"fixture loaded from collection after real application restart");
    }else{
        layout=window->saveState();OBSSourceAutoRelease previous=obs_frontend_get_current_scene();original=obs_source_get_uuid(previous);studioMode=obs_frontend_preview_program_mode_active();if(studioMode)obs_frontend_set_preview_program_mode(false);
        source=obs_source_create("studio_vst3_test_input","Studio VST3 QA input",nullptr,nullptr);OBSSceneAutoRelease fixture=obs_scene_create("Studio VST3 QA scene");scene=obs_source_get_ref(obs_scene_get_source(fixture));obs_scene_add(fixture,source);
    }
    obs_frontend_set_current_scene(scene);obs_source_add_audio_capture_callback(source,capture,nullptr);
    audioTimer=new QTimer(qApp);audioTimer->setInterval(20);audioTimer->setTimerType(Qt::PreciseTimer);
    QObject::connect(audioTimer,&QTimer::timeout,qApp,[]{float samples[960];for(float &sample:samples)sample=.12f*std::sin(2*3.141592653589793*440*double(sampleIndex++)/48000);obs_source_audio a{};a.data[0]=reinterpret_cast<uint8_t *>(samples);a.frames=960;a.samples_per_sec=48000;a.speakers=SPEAKERS_MONO;a.format=AUDIO_FORMAT_FLOAT;a.timestamp=os_gettime_ns();obs_source_output_audio(source,&a);});audioTimer->start();
    steps.push_back([]{for(auto *w:window->findChildren<QWidget *>())if(QByteArray(w->metaObject()->className())=="VolumeControl"&&w->objectName()=="Studio VST3 QA input")strip=w;check(strip&&strip->isVisible(),"fixture has visible native mixer strip");});
    if(phase=="restore"){
        steps.push_back([]{check(std::abs(value(gainUUID,paramKey)-.25)<.001,"gain parameter restored after application restart");check(std::abs(value(secondUUID,secondKey)-.3)<.001,"second plugin parameter restored after application restart");check(!obs_source_enabled(filter(secondUUID)),"VST3 bypass restored");verifyState(gainUUID);verifyState(secondUUID);resetPower();});
        steps.push_back([]{const double ratio=rms()/baseline;facts["restored_pcm_rms_ratio"]=ratio;check(ratio>.22&&ratio<.28,"reloaded VST3 actually applies saved gain to PCM");open(gainUUID);});
        steps.push_back([]{auto *spin=editor()->findChild<QDoubleSpinBox *>(paramKey);check(spin&&std::abs(spin->value()-.25)<.001,"reopened mixer editor displays saved parameter");editor()->grab().save(path+".restored-editor.png");click(editor()->findChild<QPushButton *>("studioEffectClose"));});
    }else{
        steps.push_back([]{resetPower();});
        steps.push_back([]{baseline=rms();check(baseline>.02,"synthetic tone audible in native PCM before filtering");gainUUID=add("AGain VST3");});
        steps.push_back([]{paramKey=parameter(gainUUID);open(gainUUID);});
        steps.push_back([]{setSpin(paramKey,"0.25");click(editor()->findChild<QPushButton *>("studioEffectClose"));});
        steps.push_back([]{facts["gain_setting_after_close"]=value(gainUUID,paramKey);check(std::abs(value(gainUUID,paramKey)-.25)<.001,"mixer gesture updates VST3 controller settings");resetPower();});
        steps.push_back([]{double ratio=rms()/baseline;facts["gain_pcm_rms_ratio"]=ratio;check(ratio>.22&&ratio<.28,"VST3 gain changes actual filtered PCM");action("actionMainUndo");});
        steps.push_back([]{check(value(gainUUID,paramKey)>.9,"parameter Undo restores initial gain");action("actionMainRedo");});
        steps.push_back([]{check(std::abs(value(gainUUID,paramKey)-.25)<.001,"parameter Redo restores gain");secondUUID=add("mda Limiter");});
        steps.push_back([]{secondKey=parameter(secondUUID);open(secondUUID);});
        steps.push_back([]{setSpin(secondKey,"0.3");click(editor()->findChild<QPushButton *>("studioEffectClose"));});
        steps.push_back([]{check(std::abs(value(secondUUID,secondKey)-.3)<.001,"second VST3 has independent parameter state");bypassSecond();verifyState(gainUUID);verifyState(secondUUID);strip->grab().save(path+".strip.png");open(gainUUID);});
        steps.push_back([]{editor()->grab().save(path+".editor.png");auto *native=editor()->findChild<QPushButton *>("vst3_open_gui");check(native&&native->isVisible(),"native VST3 editor available");click(native);});
        steps.push_back([]{check(!QApplication::activePopupWidget(),"mixer popup releases grab for native VST3 editor");facts["native_editor_requested"]=true;captureNativeEditor();});
    }
    next();
}
static const char *name(void *){return "Studio VST3 test input";}
static void *create(obs_data_t *,obs_source_t *s){return s;}static void destroy(void *){}
static void event(obs_frontend_event e,void *){if(e==OBS_FRONTEND_EVENT_FINISHED_LOADING)later(3500,run);}
bool obs_module_load(){path=qEnvironmentVariable("OBS_STUDIO_VST3_REPORT");phase=qEnvironmentVariable("OBS_STUDIO_VST3_PHASE");if(path.isEmpty())return false;obs_source_info i{};i.id="studio_vst3_test_input";i.type=OBS_SOURCE_TYPE_INPUT;i.output_flags=OBS_SOURCE_AUDIO;i.get_name=name;i.create=create;i.destroy=destroy;obs_register_source(&i);obs_frontend_add_event_callback(event,nullptr);return true;}
void obs_module_unload(){obs_frontend_remove_event_callback(event,nullptr);if(audioTimer){audioTimer->stop();delete audioTimer;audioTimer=nullptr;}if(source)obs_source_remove_audio_capture_callback(source,capture,nullptr);source=nullptr;scene=nullptr;}
