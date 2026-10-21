// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <pluginterfaces/gui/iplugview.h>
#include <QObject>
#include <string>
class QTimer;
// A separate X11 top-level lets an X11 VST3 editor run under a Wayland Qt
// application. Never pass a Wayland surface ID to an X11 plug-in.
class StudioX11Editor final : public QObject, public Steinberg::IPlugFrame {
public:
    explicit StudioX11Editor(Steinberg::IPlugView *view, QObject *parent);
    ~StudioX11Editor() override;
    bool create(int width,int height,const std::string &title);
    void show();
    void close();
    bool closed() const { return closed_; }
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView *,Steinberg::ViewRect *) override;
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID,void **) override;
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }
private:
    void poll();
    void embed(unsigned long child);
    void notify(unsigned long child, long message, long detail=0);
    Steinberg::IPlugView *view_;
    void *display_=nullptr;
    unsigned long window_=0, deleteAtom_=0;
    QTimer *timer_=nullptr;
    bool closed_=true;
};
