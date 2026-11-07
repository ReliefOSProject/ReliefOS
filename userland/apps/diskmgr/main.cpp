#include "model.h"
#include "worker.h"
#include "elevation.h"
#include "fltk_support.h"

#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Table_Row.H>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {
using dm::Action;
dm::Snapshot snapshot{};
dm::Request pending{};
dm::Worker worker;
dm::Elevation elevation;
Fl_Double_Window *shell;
Fl_Menu_Bar *menus;
Fl_Hold_Browser *disk_list;
Fl_Output *disk_summary, *status;
Fl_Multiline_Output *details;
Fl_Box *disk_label, *privilege;
Fl_Button *refresh_button, *admin_button, *buttons[5];
int menu_actions[6], admin_menu;
int selected_partition = -1;

/** @brief Copy text into the native status widget; no pointer is retained. */
void set_status(const char *text) { status->value(text ? text : ""); }
/** @brief Return a localized operation caption shared by menus, dialogs and progress. */
const char *action_name(Action action)
{
    switch (action) {
    case Action::Create: return T("New partition");
    case Action::Format: return T("Format");
    case Action::Delete: return T("Delete partition");
    case Action::Mount: return T("Mount");
    case Action::Unmount: return T("Unmount");
    default: return T("Refresh");
    }
}
/** @brief Display a recognized GPT role without confusing BIOS Boot with a filesystem. */
const char *partition_type(uint32_t type)
{
    switch (type) {
    case RELIEFOS_BLOCK_GPT_BIOS_BOOT: return "BIOS Boot";
    case RELIEFOS_BLOCK_GPT_ESP: return T("EFI System");
    case RELIEFOS_BLOCK_GPT_LINUX: return "Linux";
    case RELIEFOS_BLOCK_GPT_BASIC_DATA: return T("Data");
    default: return T("Unknown");
    }
}
/** @brief Format a bounded error while keeping the actual negative errno visible. */
void show_error(const char *operation, int error, bool created = false)
{
    char text[512];
    snprintf(text, sizeof(text), "%s: %s (%d)%s%s", operation, strerror(error < 0 ? -error : error),
             error, created ? ". " : "", created ? T("The partition was created, but formatting failed.") : "");
    set_status(text);
}
/** @brief Report whether any owned scan, disk operation or authentication is active. */
bool busy() { return worker.busy() || elevation.busy(); }
void update_controls();
void start_request(const dm::Request &request);
void perform_action(Action action);

class PartitionTable : public Fl_Table_Row {
public:
    PartitionTable(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h)
    {
        cols(6); rows(0); col_header(1); col_header_height(30);
        row_height_all(30); col_resize(1); row_resize(0);
        type(SELECT_SINGLE); when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
        const int widths[] = {140, 130, 82, 94, 112, 210};
        for (int i = 0; i < 6; ++i) col_width(i, widths[i]);
        end();
    }
    /** @brief Paint standard native table cells; disk text is plain UTF-8, never browser markup. */
    void draw_cell(TableContext context, int row, int col, int x, int y, int w, int h) override
    {
        if (context == CONTEXT_STARTPAGE) { fl_font(FL_HELVETICA, 15); return; }
        if (context != CONTEXT_COL_HEADER && context != CONTEXT_CELL) return;
        fl_push_clip(x, y, w, h);
        char text[dm::MountPathSize + 40] = "";
        if (context == CONTEXT_COL_HEADER) {
            const char *titles[] = {T("Partition"), T("Name"), T("Filesystem"), T("Size"),
                                    T("GPT type"), T("Mounted at")};
            fl_draw_box(FL_THIN_UP_BOX, x, y, w, h, FL_BACKGROUND_COLOR);
            snprintf(text, sizeof(text), "%s", titles[col]);
            fl_color(FL_FOREGROUND_COLOR);
        } else if (row >= 0 && static_cast<uint32_t>(row) < snapshot.partition_count) {
            const dm::Partition &part = snapshot.partitions[row];
            Fl_Color background = row_selected(row) ? FL_SELECTION_COLOR :
                                  row % 2 ? fl_color_average(FL_WHITE, FL_BACKGROUND_COLOR, .70f) : FL_WHITE;
            fl_color(background); fl_rectf(x, y, w, h);
            fl_color(fl_contrast(FL_FOREGROUND_COLOR, background));
            switch (col) {
            case 0: snprintf(text, sizeof(text), "%s", part.info.path); break;
            case 1: snprintf(text, sizeof(text), "%s", part.info.name[0] ? part.info.name : "—"); break;
            case 2: snprintf(text, sizeof(text), "%s", part.info.filesystem ?
                             reliefos_block_filesystem_name(part.info.filesystem) : T("Unknown")); break;
            case 3: dm::format_size(text, sizeof(text), part.info.sector_count * 512ULL); break;
            case 4: snprintf(text, sizeof(text), "%s", partition_type(part.info.gpt_type)); break;
            case 5: snprintf(text, sizeof(text), "%s", part.mount_count ? part.mount_path : "—"); break;
            }
        }
        fl_font(FL_HELVETICA, 15);
        fl_draw(text, x + 8, y, w - 12, h, FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        fl_pop_clip();
    }
};
PartitionTable *partitions;

/** @brief Describe the selected partition once, including its real mount target and identity. */
void update_details()
{
    char text[768];
    if (selected_partition < 0 || static_cast<uint32_t>(selected_partition) >= snapshot.partition_count) {
        details->value(T("Select a partition to see its identity and mount status."));
    } else {
        const dm::Partition &part = snapshot.partitions[selected_partition];
        snprintf(text, sizeof(text), "%s: %s    %s: %s\nPARTUUID: %s\n%s: %llu — %llu    %s: %s",
                 T("Partition"), part.info.path, T("Status"),
                 part.system ? T("System partition") : part.mount_count ? T("Mounted") :
                 part.read_only ? T("Read-only") : T("Not mounted"), part.uuid,
                 T("Sectors"), static_cast<unsigned long long>(part.info.first_lba),
                 static_cast<unsigned long long>(part.info.first_lba + part.info.sector_count - 1),
                 T("Mounted at"), part.mount_count ? part.mount_path : "—");
        details->value(text);
    }
    update_controls();
}

/** @brief Rebuild device rows after a completed scan and preserve a partition by device path. */
void update_snapshot(const char *preferred)
{
    disk_list->clear();
    for (uint32_t i = 0; i < snapshot.disk_count; ++i) {
        char size[64], row[160];
        dm::format_size(size, sizeof(size), snapshot.disks[i].info.sector_count * snapshot.disks[i].info.sector_size);
        snprintf(row, sizeof(row), "%s   %s%s", snapshot.disks[i].info.path, size,
                 snapshot.disks[i].read_only ? "  RO" : "");
        disk_list->add(row);
    }
    disk_list->value(snapshot.selected_disk + 1);
    char summary[256];
    if (snapshot.selected_disk >= 0) {
        const dm::Disk &disk = snapshot.disks[snapshot.selected_disk];
        char size[64]; dm::format_size(size, sizeof(size), disk.info.sector_count * disk.info.sector_size);
        snprintf(summary, sizeof(summary), "%s    %s    %s    %u %s%s", disk.info.path, size,
                 disk.info.name, disk.info.sector_size, T("bytes/sector"),
                 disk.read_only ? T("    Read-only") : disk.mounted ? T("    Contains mounted partitions") : "");
    } else snprintf(summary, sizeof(summary), "%s", T("No disk selected"));
    disk_summary->value(summary);
    selected_partition = -1;
    partitions->rows(static_cast<int>(snapshot.partition_count));
    partitions->select_all_rows(0);
    for (uint32_t i = 0; preferred && *preferred && i < snapshot.partition_count; ++i)
        if (!strcmp(preferred, snapshot.partitions[i].info.path)) {
            selected_partition = static_cast<int>(i); partitions->select_row(selected_partition, 1); break;
        }
    partitions->redraw();
    update_details();
}

/** @brief Enable only operations permitted by current selection, root identity and mount state. */
void update_controls()
{
    bool active = !busy();
    if (active) { refresh_button->activate(); disk_list->activate(); partitions->activate(); }
    else { refresh_button->deactivate(); disk_list->deactivate(); partitions->deactivate(); }
    menus->mode(menu_actions[0], active ? 0 : FL_MENU_INACTIVE);
    for (int i = 0; i < 5; ++i) {
        Action action = static_cast<Action>(i + 1);
        bool allowed = active && !dm::action_allowed(snapshot, action, selected_partition, static_cast<uint32_t>(geteuid()));
        if (allowed) buttons[i]->activate(); else buttons[i]->deactivate();
        menus->mode(menu_actions[i + 1], allowed ? 0 : FL_MENU_INACTIVE);
    }
    if (active && geteuid() != 0) admin_button->activate(); else admin_button->deactivate();
    menus->mode(admin_menu, active && geteuid() != 0 ? 0 : FL_MENU_INACTIVE);
}

/** @brief Submit a frozen request to the independent process, leaving the X11 loop responsive. */
void start_request(const dm::Request &request)
{
    if (busy()) return;
    int result = worker.start(request);
    if (result) { show_error(action_name(request.action), result); return; }
    pending = request;
    char text[160]; snprintf(text, sizeof(text), "%s…", action_name(request.action));
    set_status(text); update_controls();
}

struct ActionDialog {
    Fl_Window *window;
    Fl_Input *word, *size, *label;
    Fl_Choice *filesystem;
    Fl_Button *apply;
    Fl_Box *hint;
    dm::Request request;
    dm::Confirmation confirmation;
    bool accepted = false;
};
/** @brief A field edit always cancels the previous confirmation arm. */
void dialog_edit(Fl_Widget *, void *data)
{
    auto *dialog = static_cast<ActionDialog *>(data);
    dialog->confirmation.reset(); dialog->apply->copy_label(T("Apply")); dialog->hint->copy_label("");
}
/** @brief Validate all byte/number bounds before either of the two confirmation submissions. */
void dialog_apply(Fl_Widget *, void *data)
{
    auto *dialog = static_cast<ActionDialog *>(data);
    if (dialog->size && !dm::parse_size(dialog->size->value(), dialog->request.size_mib)) {
        dialog->hint->copy_label(T("Enter a valid size in MiB")); dialog->confirmation.reset(); return;
    }
    if (dialog->label && !reliefos_fltk::copy_input(dialog->request.label, sizeof(dialog->request.label), dialog->label->value())) {
        dialog->hint->copy_label(T("The partition name is too long.")); dialog->confirmation.reset(); return;
    }
    if (dialog->filesystem) {
        const uint32_t choices[] = {RELIEFOS_BLOCK_FILESYSTEM_EXT4, RELIEFOS_BLOCK_FILESYSTEM_FAT32,
                                    RELIEFOS_BLOCK_FILESYSTEM_EXFAT};
        int index = dialog->filesystem->value();
        if (index < 0 || index > 2) { dialog->confirmation.reset(); return; }
        dialog->request.filesystem = choices[index];
    }
    if (!dialog->confirmation.accept(dialog->request.action, dialog->word->value())) {
        dialog->hint->copy_label(dialog->confirmation.armed() ? T("Click Confirm again to write these changes.") :
                                                               T("Confirmation word does not match"));
        dialog->apply->copy_label(dialog->confirmation.armed() ? T("Confirm again") : T("Apply"));
        return;
    }
    dialog->accepted = true; dialog->window->hide();
}
/** @brief Close a modal without changing any disk. */
void dialog_cancel(Fl_Widget *, void *data) { static_cast<ActionDialog *>(data)->window->hide(); }

/** @brief Show the exact target and require a typed word plus two explicit clicks before writing. */
bool confirm_action(dm::Request &request)
{
    Fl_Window window(570, 390, action_name(request.action));
    ActionDialog dialog{};
    dialog.window = &window; dialog.request = request;
    char target[256];
    snprintf(target, sizeof(target), "%s: %s\n%s", T("Target"), request.action == Action::Create ?
             request.disk.info.path : request.partition.info.path,
             request.action == Action::Create ? T("A new partition will be created and formatted in free space.") :
             request.action == Action::Format ? T("All data on this partition will be erased.") :
                                               T("The partition entry will be removed. Its files will become inaccessible."));
    auto *description = new Fl_Box(20, 12, 530, 80);
    description->copy_label(target); description->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    int y = 106;
    if (request.action == Action::Create) {
        dialog.size = new Fl_Int_Input(150, y, 150, 30, T("Size (MiB):"));
        dialog.size->value("512"); dialog.size->callback(dialog_edit, &dialog); dialog.size->when(FL_WHEN_CHANGED);
        y += 42;
        dialog.label = new Fl_Input(150, y, 380, 30, T("Name:"));
        dialog.label->value(T("Data")); dialog.label->callback(dialog_edit, &dialog); dialog.label->when(FL_WHEN_CHANGED);
        y += 42;
    }
    if (request.action != Action::Delete) {
        dialog.filesystem = new Fl_Choice(150, y, 220, 30, T("Filesystem:"));
        dialog.filesystem->add("ext4|FAT32|exFAT");
        dialog.filesystem->value(request.filesystem == RELIEFOS_BLOCK_FILESYSTEM_FAT32 ? 1 :
                                 request.filesystem == RELIEFOS_BLOCK_FILESYSTEM_EXFAT ? 2 : 0);
        dialog.filesystem->callback(dialog_edit, &dialog); y += 42;
    }
    char prompt[128]; snprintf(prompt, sizeof(prompt), T("Type %s to confirm:"), dm::confirmation_word(request.action));
    auto *word_label = new Fl_Box(20, y, 240, 30);
    word_label->copy_label(prompt); word_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dialog.word = new Fl_Input(270, y, 260, 30);
    dialog.word->callback(dialog_edit, &dialog); dialog.word->when(FL_WHEN_CHANGED); y += 38;
    dialog.hint = new Fl_Box(20, y, 530, 32); dialog.hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    dialog.apply = new Fl_Button(332, 340, 104, 32, T("Apply")); dialog.apply->callback(dialog_apply, &dialog);
    auto *cancel = new Fl_Button(446, 340, 104, 32, T("Cancel")); cancel->callback(dialog_cancel, &dialog);
    window.end(); window.callback(dialog_cancel, &dialog); window.set_modal(); reliefos_fltk::style(&window);
    window.position(shell->x() + (shell->w() - window.w()) / 2,
                    shell->y() + (shell->h() - window.h()) / 2);
    window.show(); dialog.word->take_focus();
    while (window.shown()) Fl::wait();
    if (dialog.accepted) request = dialog.request;
    return dialog.accepted;
}

/** @brief Run an allowed command on a copied target, never whichever row happens to be selected later. */
void perform_action(Action action)
{
    if (busy()) return;
    int allowed = dm::action_allowed(snapshot, action, selected_partition, static_cast<uint32_t>(geteuid()));
    if (allowed) { show_error(action_name(action), allowed); return; }
    dm::Request request{};
    request.action = action;
    if (snapshot.selected_disk >= 0) request.disk = snapshot.disks[snapshot.selected_disk];
    if (selected_partition >= 0) request.partition = snapshot.partitions[selected_partition];
    request.filesystem = action == Action::Create ? RELIEFOS_BLOCK_FILESYSTEM_EXT4 : request.partition.info.filesystem;
    if (dm::confirmation_word(action) && !confirm_action(request)) return;
    start_request(request);
}

struct PasswordDialog { Fl_Window *window; Fl_Secret_Input *input; bool accepted = false; };
/** @brief Accept a password locally without sending it through an external input-method helper. */
void password_accept(Fl_Widget *, void *data)
{
    auto *dialog = static_cast<PasswordDialog *>(data);
    if (!*dialog->input->value()) return;
    dialog->accepted = true; dialog->window->hide();
}
/** @brief Cancel root relaunch before any authentication process has been started. */
void password_cancel(Fl_Widget *, void *data) { static_cast<PasswordDialog *>(data)->window->hide(); }
/** @brief Request standard sudo authentication and asynchronously relaunch the installed GUI. */
void authenticate(Fl_Widget *, void *)
{
    if (busy() || geteuid() == 0) return;
    Fl_Window window(500, 196, T("Administrator authentication"));
    PasswordDialog dialog{}; dialog.window = &window;
    auto *label = new Fl_Box(18, 14, 464, 48, T("Enter your sudo password to open Disk Manager as administrator."));
    label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    dialog.input = new Fl_Secret_Input(126, 78, 350, 30, T("Password:")); dialog.input->maximum_size(255);
    auto *ok = new Fl_Button(258, 142, 104, 32, T("Open")); ok->callback(password_accept, &dialog);
    auto *cancel = new Fl_Button(372, 142, 104, 32, T("Cancel")); cancel->callback(password_cancel, &dialog);
    window.end(); window.callback(password_cancel, &dialog); window.set_modal(); reliefos_fltk::style(&window);
    window.position(shell->x() + (shell->w() - window.w()) / 2,
                    shell->y() + (shell->h() - window.h()) / 2);
    window.show(); dialog.input->take_focus();
    while (window.shown()) Fl::wait();
    if (dialog.accepted) {
        int result = elevation.start(dialog.input->value());
        if (result) show_error(T("Administrator authentication"), result);
        else set_status(T("Authenticating; waiting for the administrator window…"));
    }
    dialog.input->value(""); update_controls();
}

/** @brief Translate the clicked menu/toolbar command into the common action path. */
void action_callback(Fl_Widget *, void *data) { perform_action(static_cast<Action>(reinterpret_cast<uintptr_t>(data))); }
/** @brief Rescan when a disk changes; no partition selection is carried to a different disk. */
void disk_callback(Fl_Widget *, void *)
{
    int row = disk_list->value() - 1;
    if (busy() || row < 0 || static_cast<uint32_t>(row) >= snapshot.disk_count) return;
    dm::Request request{}; request.action = Action::Refresh; request.disk = snapshot.disks[row];
    start_request(request);
}
/** @brief Update selection from the native row widget, including keyboard selection. */
void partition_callback(Fl_Widget *, void *)
{
    selected_partition = -1;
    for (int row = 0; row < partitions->rows(); ++row)
        if (partitions->row_selected(row)) { selected_partition = row; break; }
    update_details();
}
/** @brief Refuse closure during a disk transaction so a formatter cannot be interrupted accidentally. */
void close_window(Fl_Widget *, void *)
{
    if (busy()) { set_status(T("Wait for the current operation to finish before closing.")); return; }
    shell->hide();
}

/** @brief Arrange native widgets for the current window size without overlapping the action row. */
void layout(int w, int h)
{
    menus->resize(0, 0, w, 26);
    refresh_button->resize(12, 36, 110, 32);
    privilege->resize(134, 36, w - 390, 32);
    admin_button->resize(w - 248, 36, 236, 32);
    disk_label->resize(12, 80, 242, 22);
    disk_list->resize(12, 108, 242, h - 154);
    disk_summary->resize(270, 80, w - 282, 30);
    partitions->resize(270, 120, w - 282, h - 318);
    int x = 270;
    const int widths[] = {124, 104, 124, 96, 96};
    for (int i = 0; i < 5; ++i) { buttons[i]->resize(x, h - 184, widths[i], 32); x += widths[i] + 8; }
    details->resize(270, h - 138, w - 282, 92);
    status->resize(12, h - 34, w - 24, 26);
}
class DiskWindow : public Fl_Double_Window {
public:
    DiskWindow() : Fl_Double_Window(1080, 700, T("Disk Manager")) {}
    /** @brief Keep controls reachable and both tables scrollable at the minimum window size. */
    void resize(int x, int y, int w, int h) override
    {
        Fl_Double_Window::resize(x, y, w, h);
        if (status) layout(w, h);
    }
};

/** @brief Poll bounded child pipes/reaping from the GUI loop; no disk I/O runs on this path. */
void tick(void *)
{
    if (worker.poll()) {
        const dm::Reply &reply = worker.reply();
        snapshot = reply.snapshot;
        update_snapshot(reply.created && reply.result_path[0] ? reply.result_path : pending.partition.info.path);
        if (reply.status) show_error(action_name(pending.action), reply.status, reply.created);
        else if (pending.action == Action::Refresh && !snapshot.disk_count)
            set_status(T("No readable disks found. Open as administrator if access is restricted."));
        else {
            const char *messages[] = {T("Disk list refreshed"), T("Partition created and formatted"),
                                      T("Partition formatted"), T("Partition entry deleted"),
                                      T("Partition mounted"), T("Partition unmounted")};
            set_status(messages[static_cast<unsigned>(pending.action)]);
        }
        fprintf(stderr, "[diskmgr.elf] job complete action=%u status=%d disks=%u partitions=%u\n",
                static_cast<unsigned>(pending.action), reply.status, snapshot.disk_count, snapshot.partition_count);
        fflush(stderr); update_controls();
    }
    int auth = elevation.poll();
    if (auth == 2) {
        fprintf(stderr, "[diskmgr.elf] administrator window started\n"); fflush(stderr);
        shell->hide(); return;
    }
    if (auth == 1) {
        fprintf(stderr, "[diskmgr.elf] administrator authentication failed\n"); fflush(stderr);
        set_status(T("Authentication or administrator startup failed. Check your password and sudo permission."));
        update_controls();
    }
    Fl::repeat_timeout(.05, tick);
}

/** @brief Construct a compact disk/partition workspace using shared ReliefOS FLTK fonts and theme. */
void build_window()
{
    shell = new DiskWindow();
    shell->begin();
    menus = new Fl_Menu_Bar(0, 0, 1080, 26);
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", T("Disk"), T("Refresh"));
    menu_actions[0] = menus->add(path, FL_CTRL + 'r', action_callback, nullptr);
    snprintf(path, sizeof(path), "%s/%s", T("Disk"), T("Open as administrator…"));
    admin_menu = menus->add(path, 0, authenticate);
    snprintf(path, sizeof(path), "%s/%s", T("Disk"), T("Close")); menus->add(path, FL_CTRL + 'q', close_window);
    for (int i = 0; i < 5; ++i) {
        Action action = static_cast<Action>(i + 1);
        snprintf(path, sizeof(path), "%s/%s", T("Partition"), action_name(action));
        menu_actions[i + 1] = menus->add(path, 0, action_callback, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    }
    refresh_button = new Fl_Button(12, 36, 110, 32, T("Refresh")); refresh_button->callback(action_callback, nullptr);
    privilege = new Fl_Box(134, 36, 600, 32, geteuid() == 0 ? T("Administrator") : T("Read-only view · administrator required for changes"));
    privilege->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    admin_button = new Fl_Button(832, 36, 236, 32, T("Open as administrator…")); admin_button->callback(authenticate);
    disk_label = new Fl_Box(12, 80, 242, 22, T("Disks")); disk_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    disk_list = new Fl_Hold_Browser(12, 108, 242, 546); disk_list->format_char(0); disk_list->textsize(15);
    disk_list->callback(disk_callback); disk_list->when(FL_WHEN_CHANGED);
    disk_summary = new Fl_Output(270, 80, 798, 30); disk_summary->textsize(15);
    partitions = new PartitionTable(270, 120, 798, 382); partitions->callback(partition_callback);
    for (int i = 0; i < 5; ++i) {
        Action action = static_cast<Action>(i + 1);
        buttons[i] = new Fl_Button(270 + i * 120, 516, 110, 32, action_name(action));
        buttons[i]->callback(action_callback, reinterpret_cast<void *>(static_cast<uintptr_t>(action)));
    }
    buttons[0]->tooltip(T("Create and format a partition in free space on an unmounted GPT disk."));
    buttons[1]->tooltip(T("Format the selected unmounted partition. This erases its data."));
    buttons[2]->tooltip(T("Delete a partition entry on an unmounted disk."));
    buttons[3]->tooltip(T("Mount the selected filesystem below /mnt."));
    buttons[4]->tooltip(T("Unmount the selected filesystem from its actual mount point."));
    details = new Fl_Multiline_Output(270, 562, 798, 92); details->textsize(14);
    status = new Fl_Output(12, 666, 1056, 26); status->textsize(14);
    shell->end(); shell->callback(close_window); shell->size_range(920, 600);
    reliefos_fltk::style(shell); layout(shell->w(), shell->h()); update_controls();
}
}

/** @brief Run the X11/FLTK frontend; elevated relaunches must really have effective UID0. */
int main(int argc, char **argv)
{
    bool elevated = argc == 2 && !strcmp(argv[1], "--elevated");
    if ((argc != 1 && !elevated) || (elevated && geteuid() != 0)) return 126;
    signal(SIGPIPE, SIG_IGN);
    reliefos_fltk::init("diskmgr");
    snapshot.selected_disk = -1;
    build_window(); shell->show(); Fl::check();
    if (elevated) dm::elevated_ready();
    fprintf(stderr, "[diskmgr.elf] FLTK disk manager ready uid=%u\n", static_cast<unsigned>(geteuid())); fflush(stderr);
    dm::Request request{}; request.action = Action::Refresh; start_request(request);
    Fl::add_timeout(.05, tick);
    int result = Fl::run();
    Fl::remove_timeout(tick); delete shell;
    return result;
}
