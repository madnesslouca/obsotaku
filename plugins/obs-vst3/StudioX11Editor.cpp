// SPDX-License-Identifier: GPL-3.0-or-later
#include "StudioX11Editor.hpp"
#include "VST3HostApp.h"
#include <QTimer>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
using namespace Steinberg;
VST3HostApp *getHostApp() noexcept;
StudioX11Editor::StudioX11Editor(IPlugView *view,QObject *parent):QObject(parent),view_(view) {}
StudioX11Editor::~StudioX11Editor()
{
    if(timer_) timer_->stop();
    auto *display=static_cast<Display *>(display_);
    if(display) { if(window_)XDestroyWindow(display,window_);XCloseDisplay(display); }
}
bool StudioX11Editor::create(int width,int height,const std::string &title)
{
    auto *display=XOpenDisplay(nullptr);
    if(!display) return false;
    display_=display;
    window_=XCreateSimpleWindow(display,DefaultRootWindow(display),100,100,width,height,0,0,0);
    XStoreName(display,window_,title.c_str());
    Atom utf8=XInternAtom(display,"UTF8_STRING",False);
    Atom name=XInternAtom(display,"_NET_WM_NAME",False);
    XChangeProperty(display,window_,name,utf8,8,PropModeReplace,reinterpret_cast<const unsigned char *>(title.data()),title.size());
    XClassHint hint{};hint.res_name=const_cast<char *>("obs-vst3-editor");hint.res_class=const_cast<char *>("OBS Studio");XSetClassHint(display,window_,&hint);
    XSelectInput(display,window_,StructureNotifyMask|SubstructureNotifyMask|FocusChangeMask);
    deleteAtom_=XInternAtom(display,"WM_DELETE_WINDOW",False);
    Atom protocol=deleteAtom_;XSetWMProtocols(display,window_,&protocol,1);
    if(view_->canResize()!=kResultTrue){XSizeHints size{};size.flags=PMinSize|PMaxSize;size.min_width=size.max_width=width;size.min_height=size.max_height=height;XSetWMNormalHints(display,window_,&size);}
    view_->setFrame(this);
    if(view_->attached(reinterpret_cast<void *>(window_),kPlatformTypeX11EmbedWindowID)!=kResultOk){view_->setFrame(nullptr);return false;}
    ViewRect rect(0,0,width,height);view_->onSize(&rect);
    Window root,parent,*children=nullptr;unsigned int count=0;
    if(XQueryTree(display,window_,&root,&parent,&children,&count))for(unsigned int i=0;i<count;++i)embed(children[i]);
    if(children)XFree(children);
    timer_=new QTimer(this);connect(timer_,&QTimer::timeout,this,[this]{poll();});timer_->start(16);
    return true;
}
void StudioX11Editor::notify(unsigned long child,long message,long detail)
{
    auto *display=static_cast<Display *>(display_);XEvent event{};
    event.xclient.type=ClientMessage;event.xclient.window=child;
    event.xclient.message_type=XInternAtom(display,"_XEMBED",False);event.xclient.format=32;
    event.xclient.data.l[0]=CurrentTime;event.xclient.data.l[1]=message;event.xclient.data.l[2]=detail;
    if(message==0)event.xclient.data.l[3]=window_;
    XSendEvent(display,child,False,NoEventMask,&event);
}
void StudioX11Editor::embed(unsigned long child)
{
    auto *display=static_cast<Display *>(display_);
    // VSTGUI waits for the embedding handshake before it maps its drawable.
    notify(child,0);XMapWindow(display,child);notify(child,1);notify(child,4);
    XFlush(display);
}
void StudioX11Editor::show(){auto *display=static_cast<Display *>(display_);if(!display||!window_)return;closed_=false;XMapRaised(display,window_);XFlush(display);}
void StudioX11Editor::close(){auto *display=static_cast<Display *>(display_);if(!display||!window_)return;closed_=true;XUnmapWindow(display,window_);XFlush(display);}
void StudioX11Editor::poll()
{
    auto *display=static_cast<Display *>(display_);
    while(XPending(display)){
        XEvent event;XNextEvent(display,&event);
        if(event.type==ClientMessage && static_cast<unsigned long>(event.xclient.data.l[0])==deleteAtom_)close();
        else if(event.type==CreateNotify && event.xcreatewindow.parent==window_)embed(event.xcreatewindow.window);
        else if(event.type==ReparentNotify && event.xreparent.parent==window_)embed(event.xreparent.window);
        else if(event.type==FocusIn || event.type==FocusOut){
            Window root,parent,*children=nullptr;unsigned int count=0;
            if(XQueryTree(display,window_,&root,&parent,&children,&count))for(unsigned int i=0;i<count;++i){notify(children[i],event.type==FocusIn?1:2);notify(children[i],event.type==FocusIn?4:5);}
            if(children)XFree(children);XFlush(display);
        }
        else if(event.type==ConfigureNotify && event.xconfigure.window==window_){ViewRect rect(0,0,event.xconfigure.width,event.xconfigure.height);view_->onSize(&rect);}
    }
}
tresult PLUGIN_API StudioX11Editor::resizeView(IPlugView *view,ViewRect *rect)
{
    if(view!=view_||!rect||rect->getWidth()<=0||rect->getHeight()<=0)return kInvalidArgument;
    view_->checkSizeConstraint(rect);XResizeWindow(static_cast<Display *>(display_),window_,rect->getWidth(),rect->getHeight());XFlush(static_cast<Display *>(display_));view_->onSize(rect);return kResultOk;
}
tresult PLUGIN_API StudioX11Editor::queryInterface(const TUID iid,void **object)
{
    if(!object)return kInvalidArgument;
    if(FUnknownPrivate::iidEqual(iid,FUnknown::iid)||FUnknownPrivate::iidEqual(iid,IPlugFrame::iid)){*object=static_cast<IPlugFrame *>(this);return kResultOk;}
    if(getHostApp() && FUnknownPrivate::iidEqual(iid,Linux::IRunLoop::iid))return getHostApp()->queryInterface(iid,object);
    *object=nullptr;return kNoInterface;
}
