#ifndef RELIEFOS_TASKMGR_STARTUP_PAGE_H
#define RELIEFOS_TASKMGR_STARTUP_PAGE_H
#include <FL/Fl_Group.H>

class StartupPage : public Fl_Group {
    struct Impl;
    Impl *impl_;
public:
    StartupPage(int x, int y, int w, int h, void (*status)(const char *));
    ~StartupPage() override;
    void refresh(bool force = false);
    void edit(bool create = false);
    void toggle();
    void remove();
    void resize(int x, int y, int w, int h) override;
};
#endif
