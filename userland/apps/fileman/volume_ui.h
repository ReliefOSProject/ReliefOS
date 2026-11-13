#ifndef RELIEFOS_FILEMAN_VOLUME_UI_H
#define RELIEFOS_FILEMAN_VOLUME_UI_H
#include "volumes.h"
#include "../reliefapk/process.h"
#include <FL/Fl_Group.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Button.H>

namespace fm {
struct VolumeCallbacks {
    bool (*open)(const char *);
    void (*leave)(const Volume &);
    void (*status)(const char *);
    bool (*ready)();
};

class VolumePanel : public Fl_Group {
    Fl_Box *heading_;
    Fl_Hold_Browser *list_;
    Fl_Button *open_, *eject_;
    Array<Volume> volumes_;
    ra::Process process_;
    Volume target_;
    VolumeCallbacks callbacks_;
    enum Job { Idle, Scan, Mount, Eject } job_ = Idle, pending_ = Idle;
    unsigned countdown_ = 0;
    bool available_ = true, stopped_ = false;
    static void timer(void *);
    static void selected(Fl_Widget *, void *);
    static void open_clicked(Fl_Widget *, void *);
    static void eject_clicked(Fl_Widget *, void *);
    void tick();
    void request(Job);
    void start_action();
    void refresh();
    void controls();
public:
    VolumePanel(int x, int y, int w, int h, const VolumeCallbacks &);
    ~VolumePanel();
    void resize(int x, int y, int w, int h) override;
    bool busy() const { return process_.running(); }
    bool acting() const { return job_ == Mount || job_ == Eject || pending_ != Idle; }
    void stop();
};
} // namespace fm
#endif
