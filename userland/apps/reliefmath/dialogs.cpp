#include "window.h"
#include "catalog.h"
#include "document.h"
#include "jobs.h"
#include "library.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Multiline_Output.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace rm {
namespace {

struct Category {
    const char *english;
};

const Category kCategories[] = {
    {"All categories"},
    {"Greek letters"},
    {"Binary operators"},
    {"Relations"},
    {"Arrows"},
    {"Sets"},
    {"Brackets"},
    {"Spacing"},
    {"Fractions"},
    {"Roots"},
    {"Scripts"},
    {"Delimiters"},
    {"Big operators"},
    {"Functions"},
    {"Matrices"},
    {"Accents"},
    {"Text and fonts"}
};

Fl_Box *make_label(int x, int y, int width, int height, const char *text) {
    Fl_Box *box = new Fl_Box(x, y, width, height, text);
    box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    return box;
}

void place_dialog(MathWindow &owner, Fl_Window &dialog) {
    int screen_x = 0, screen_y = 0, screen_w = 0, screen_h = 0;
    Fl::screen_work_area(screen_x, screen_y, screen_w, screen_h, owner.x(), owner.y());
    if (screen_w <= 0 || screen_h <= 0) {
        screen_x = 0;
        screen_y = 0;
        screen_w = owner.w();
        screen_h = owner.h();
    }
    int x = owner.x() + (owner.w() - dialog.w()) / 2;
    int y = owner.y() + (owner.h() - dialog.h()) / 2;
    if (dialog.w() > screen_w) x = screen_x;
    else if (x < screen_x) x = screen_x;
    else if (x + dialog.w() > screen_x + screen_w) x = screen_x + screen_w - dialog.w();
    if (dialog.h() > screen_h) y = screen_y;
    else if (y < screen_y) y = screen_y;
    else if (y + dialog.h() > screen_y + screen_h) y = screen_y + screen_h - dialog.h();
    dialog.position(x, y);
}

void run_modal(MathWindow &owner, Fl_Window &dialog) {
    place_dialog(owner, dialog);
    dialog.set_modal();
    dialog.show();
    // Fl::wait dispatches the main window's polling timeout while the modal
    // child owns input, so rendering and I/O completions continue to progress.
    while (dialog.shown()) Fl::wait();
}

const char *category_display(const char *english, char *buffer, size_t capacity) {
    snprintf(buffer, capacity, "%s", T(english));
    return buffer;
}

bool is_template(const ToolEntry &entry) {
    return entry.key && !strncmp(entry.key, "structure.", 10);
}

bool is_structure(const ToolEntry &entry) {
    switch (entry.tool) {
    case Tool::Fraction: case Tool::DisplayFraction:
    case Tool::Root: case Tool::IndexedRoot:
    case Tool::Superscript: case Tool::Subscript:
    case Tool::Integral: case Tool::DoubleIntegral: case Tool::TripleIntegral:
    case Tool::ClosedIntegral: case Tool::Sum: case Tool::Product:
    case Tool::Matrix: case Tool::ParenthesizedMatrix: case Tool::BracketedMatrix:
    case Tool::Cases: case Tool::Aligned: case Tool::Text:
        return true;
    default:
        return false;
    }
}

struct SymbolDialogState {
    MathWindow *owner = nullptr;
    Fl_Window *dialog = nullptr;
    Fl_Input *search = nullptr;
    Fl_Choice *category = nullptr;
    Fl_Browser *browser = nullptr;
    Fl_Multiline_Output *details = nullptr;
    Array<const ToolEntry *> matches;
    bool templates = false;
};

void update_symbol_details(SymbolDialogState &state) {
    const int row = state.browser->value();
    if (row <= 0 || size_t(row) > state.matches.size()) {
        state.details->value(T("Choose an item to see its command and example."));
        return;
    }
    const ToolEntry &entry = *state.matches[size_t(row - 1)];
    char category[160];
    category_display(entry.category, category, sizeof category);
    char detail[2048];
    snprintf(detail, sizeof detail, "%s\n%s: %s\n%s: %s\n%s: %s",
             T(entry.msgid), T("Category"), category,
             T("Command"), entry.command,
             T("Example"), entry.help_example);
    state.details->value(detail);
}

void refresh_symbols(SymbolDialogState &state) {
    size_t entry_count = 0;
    const ToolEntry *entries = catalog_entries(&entry_count);
    state.matches.clear();
    state.browser->clear();
    const char *query = state.search->value();
    const int selected_category = state.category->value();
    for (size_t index = 0; index < entry_count; ++index) {
        const ToolEntry &entry = entries[index];
        if (state.templates && !is_template(entry)) continue;
        if (selected_category > 0 &&
            strcmp(entry.category, kCategories[selected_category].english) != 0) continue;
        if (!catalog_entry_matches(entry, T(entry.msgid), T(entry.category), query)) continue;
        if (!state.matches.push(&entry)) {
            state.details->value(T("Not enough memory to list items."));
            state.browser->clear();
            state.matches.clear();
            return;
        }
        char category[160];
        char row[1024];
        category_display(entry.category, category, sizeof category);
        snprintf(row, sizeof row, "%s    ·    %s    ·    %s",
                 T(entry.msgid), category, entry.command);
        state.browser->add(row);
    }
    if (state.matches.size()) state.browser->value(1);
    update_symbol_details(state);
}

struct FavoriteDialogState {
    MathWindow *owner = nullptr;
    Fl_Window *dialog = nullptr;
    Fl_Browser *browser = nullptr;
    Fl_Multiline_Output *source = nullptr;
    Fl_Box *status = nullptr;
    Fl_Button *add = nullptr;
    Fl_Button *insert = nullptr;
    Fl_Button *remove = nullptr;
    Fl_Button *retry = nullptr;
    Document favorites;
    Text path;
    Text pending_insert;
    bool load_ready = false;
    bool can_modify = false;
    bool dirty = false;
    bool saving = false;
    bool save_failed = false;
    bool close_requested = false;
    bool close_confirmed = false;
    bool insert_requested = false;
};

bool favorite_path(Text &path) {
    char candidate[4096];
    const char *config = getenv("XDG_CONFIG_HOME");
    struct stat info{};
    if (config && config[0] == '/' && strlen(config) < sizeof candidate - 32 &&
        lstat(config, &info) == 0 && S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode)) {
        int count = snprintf(candidate, sizeof candidate,
                             "%s/reliefmath-favorites.rmath", config);
        return count > 0 && size_t(count) < sizeof candidate && path.set(candidate);
    }
    const char *home = getenv("HOME");
    if (!home || !*home || strlen(home) >= sizeof candidate - 40) return false;
    int count = snprintf(candidate, sizeof candidate,
                         "%s/.reliefmath-favorites.rmath", home);
    return count > 0 && size_t(count) < sizeof candidate && path.set(candidate);
}

void refresh_favorites(FavoriteDialogState &state) {
    const int selected = state.browser->value();
    state.browser->clear();
    for (size_t index = 0; index < state.favorites.formulas.size(); ++index) {
        const Formula &formula = state.favorites.formulas[index];
        state.browser->add(formula.name.c_str());
    }
    if (state.favorites.formulas.size()) {
        int row = selected > 0 && size_t(selected) <= state.favorites.formulas.size()
            ? selected : 1;
        state.browser->value(row);
    }
    const int row = state.browser->value();
    if (row > 0 && size_t(row) <= state.favorites.formulas.size()) {
        const Formula &formula = state.favorites.formulas[size_t(row - 1)];
        state.source->value(formula.source.c_str());
    } else {
        state.source->value("");
    }
    const bool has_selection = row > 0 && size_t(row) <= state.favorites.formulas.size();
    if (has_selection) state.insert->activate(); else state.insert->deactivate();
    if (has_selection && state.can_modify) state.remove->activate(); else state.remove->deactivate();
    if (state.can_modify && state.owner->current()) state.add->activate(); else state.add->deactivate();
}

void set_favorite_status(FavoriteDialogState &state, const char *text) {
    state.status->copy_label(text ? text : "");
    state.status->redraw();
}

bool start_favorite_save(FavoriteDialogState &state, IoJob &job) {
    Bytes bytes;
    Error error;
    if (!encode_document(state.favorites, bytes, error)) {
        state.save_failed = true;
        state.dirty = true;
        state.retry->show();
        set_favorite_status(state, T(error.message.c_str()));
        return false;
    }
    if (!job.write(state.path.c_str(), static_cast<Bytes &&>(bytes), true, error)) {
        state.save_failed = true;
        state.dirty = true;
        state.retry->show();
        set_favorite_status(state, T(error.message.c_str()));
        return false;
    }
    state.saving = true;
    state.dirty = false;
    state.save_failed = false;
    state.retry->hide();
    set_favorite_status(state, T("Saving favorites..."));
    return true;
}

bool add_current_favorite(FavoriteDialogState &state) {
    const Formula *current = state.owner->current();
    if (!current) return false;
    for (size_t index = 0; index < state.favorites.formulas.size(); ++index) {
        const Formula &saved = state.favorites.formulas[index];
        const bool same_style = saved.style.rgba == current->style.rgba &&
            saved.style.point_size == current->style.point_size &&
            saved.style.padding == current->style.padding &&
            saved.style.display == current->style.display;
        if (saved.name == current->name && saved.source == current->source && same_style) {
            set_favorite_status(state, T("This formula is already a favorite."));
            return false;
        }
    }
    Error error;
    FormulaId id = 0;
    if (!add_formula(state.favorites, current->name.c_str(), current->source.c_str(), id, error)) {
        set_favorite_status(state, T(error.message.c_str()));
        return false;
    }
    if (!set_formula_style(state.favorites, id, current->style, error)) {
        (void)delete_formula(state.favorites, id, error);
        set_favorite_status(state, T(error.message.c_str()));
        return false;
    }
    state.dirty = true;
    refresh_favorites(state);
    set_favorite_status(state, T("Added current formula."));
    return true;
}

void manage_favorites(MathWindow &owner) {
    FavoriteDialogState state;
    state.owner = &owner;
    state.favorites.title.set("ReliefMath Favorites");
    if (!favorite_path(state.path)) {
        fl_alert(T("Cannot find a writable user configuration path."));
        return;
    }

    Fl_Window dialog(760, 560, T("Formula favorites"));
    dialog.begin();
    make_label(16, 12, 728, 28,
               T("Saved in the .rmath format."));
    Fl_Browser browser(16, 46, 728, 258);
    browser.textsize(14);
    make_label(16, 312, 728, 23, T("Source preview"));
    Fl_Multiline_Output source(16, 337, 728, 112);
    source.textfont(FL_COURIER);
    source.textsize(14);
    Fl_Box status(16, 454, 728, 28, T("Loading favorites..."));
    status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Button add(16, 506, 152, 34, T("Add current"));
    Fl_Button insert(176, 506, 112, 34, T("Insert"));
    Fl_Button remove(296, 506, 112, 34, T("Delete"));
    Fl_Button retry(416, 506, 132, 34, T("Retry save"));
    Fl_Button close(556, 506, 112, 34, T("Close"));
    dialog.end();
    state.dialog = &dialog;
    state.browser = &browser;
    state.source = &source;
    state.status = &status;
    state.add = &add;
    state.insert = &insert;
    state.remove = &remove;
    state.retry = &retry;
    retry.hide();
    insert.deactivate();
    remove.deactivate();
    add.deactivate();
    browser.callback([](Fl_Widget *, void *data) {
        refresh_favorites(*static_cast<FavoriteDialogState *>(data));
    }, &state);
    add.callback([](Fl_Widget *, void *data) {
        FavoriteDialogState &s = *static_cast<FavoriteDialogState *>(data);
        if (add_current_favorite(s)) s.dirty = true;
    }, &state);
    remove.callback([](Fl_Widget *, void *data) {
        FavoriteDialogState &s = *static_cast<FavoriteDialogState *>(data);
        int row = s.browser->value();
        if (row <= 0 || size_t(row) > s.favorites.formulas.size()) return;
        Error error;
        if (delete_formula(s.favorites, s.favorites.formulas[size_t(row - 1)].id, error)) {
            s.dirty = true;
            refresh_favorites(s);
            set_favorite_status(s, T("Favorite deleted."));
        } else set_favorite_status(s, T(error.message.c_str()));
    }, &state);
    insert.callback([](Fl_Widget *, void *data) {
        FavoriteDialogState &s = *static_cast<FavoriteDialogState *>(data);
        int row = s.browser->value();
        if (row <= 0 || size_t(row) > s.favorites.formulas.size()) return;
        const Formula &formula = s.favorites.formulas[size_t(row - 1)];
        if (!s.pending_insert.set(formula.source.c_str())) return;
        s.insert_requested = true;
        s.close_requested = true;
    }, &state);
    retry.callback([](Fl_Widget *, void *data) {
        FavoriteDialogState &s = *static_cast<FavoriteDialogState *>(data);
        s.save_failed = false;
        s.dirty = true;
    }, &state);
    dialog.callback([](Fl_Widget *, void *data) {
        static_cast<FavoriteDialogState *>(data)->close_requested = true;
    }, &state);
    close.callback([](Fl_Widget *, void *data) {
        static_cast<FavoriteDialogState *>(data)->close_requested = true;
    }, &state);

    IoJob job;
    Error start_error;
    bool load_started = job.load(state.path.c_str(), 1, start_error);
    if (!load_started) {
        state.load_ready = true;
        set_favorite_status(state, T(start_error.message.c_str()));
    }
    place_dialog(owner, dialog);
    dialog.set_modal();
    dialog.show();
    while (dialog.shown() || job.pending()) {
        IoResult result;
        if (job.take(result)) {
            if (result.kind == IoKind::Load) {
                state.load_ready = true;
                if (result.success) {
                    state.favorites.swap(result.document);
                    state.can_modify = true;
                    set_favorite_status(state, T("Favorites loaded."));
                } else if (result.error.code == Code::NotFound) {
                    state.can_modify = true;
                    set_favorite_status(state, T("No favorites saved yet."));
                } else {
                    state.can_modify = false;
                    set_favorite_status(state, T(result.error.message.c_str()));
                }
                refresh_favorites(state);
            } else if (result.kind == IoKind::Write) {
                state.saving = false;
                if (result.success) {
                    state.save_failed = false;
                    state.retry->hide();
                    if (!state.dirty)
                        set_favorite_status(state, T("Favorites saved."));
                } else {
                    state.save_failed = true;
                    state.dirty = true;
                    state.retry->show();
                    set_favorite_status(state, T(result.error.message.c_str()));
                }
            }
        }
        if (state.close_requested && state.load_ready) {
            FavoriteCloseDecision decision = favorite_close_decision(
                true, job.pending(), state.dirty, state.save_failed);
            if (decision == FavoriteCloseDecision::Save) {
                (void)start_favorite_save(state, job);
                decision = favorite_close_decision(
                    true, job.pending(), state.dirty, state.save_failed);
            }
            if (decision == FavoriteCloseDecision::Close) {
                dialog.hide();
            } else if (decision == FavoriteCloseDecision::ConfirmDiscard &&
                       !state.close_confirmed) {
                int answer = fl_choice(
                    T("Favorites could not be saved. Close without saving?"),
                    T("Keep open"), T("Close anyway"), nullptr);
                if (answer == 1) {
                    state.close_confirmed = true;
                    dialog.hide();
                } else {
                    state.close_requested = false;
                    state.close_confirmed = false;
                    state.insert_requested = false;
                    state.pending_insert.clear();
                }
            }
        } else if (state.load_ready && state.dirty && !job.pending() &&
                   !state.save_failed) {
            (void)start_favorite_save(state, job);
        }
        if (dialog.shown() || job.pending()) Fl::wait();
    }
    if (state.insert_requested) owner.insert_source(state.pending_insert.c_str());
}

int structure_kind(Tool tool) {
    switch (tool) {
    case Tool::Fraction: case Tool::DisplayFraction: return 0;
    case Tool::Root: case Tool::IndexedRoot: return 1;
    case Tool::Superscript: return 2;
    case Tool::Subscript: return 3;
    case Tool::Integral: case Tool::DoubleIntegral: case Tool::TripleIntegral:
    case Tool::ClosedIntegral: return 4;
    case Tool::Sum: return 5;
    case Tool::Product: return 6;
    case Tool::Matrix: case Tool::ParenthesizedMatrix: case Tool::BracketedMatrix: return 7;
    case Tool::Cases: return 8;
    case Tool::Aligned: return 9;
    case Tool::Text: return 10;
    default: return 0;
    }
}

Tool structure_tool(int kind) {
    switch (kind) {
    case 1: return Tool::Root;
    case 2: return Tool::Superscript;
    case 3: return Tool::Subscript;
    case 4: return Tool::Integral;
    case 5: return Tool::Sum;
    case 6: return Tool::Product;
    case 7: return Tool::Matrix;
    case 8: return Tool::Cases;
    case 9: return Tool::Aligned;
    case 10: return Tool::Text;
    default: return Tool::Fraction;
    }
}

struct StructureDialogState {
    Fl_Window *dialog = nullptr;
    Fl_Choice *kind = nullptr;
    Fl_Choice *variant = nullptr;
    Fl_Box *variant_label = nullptr;
    Fl_Box *rows_label = nullptr;
    Fl_Box *columns_label = nullptr;
    Fl_Spinner *rows = nullptr;
    Fl_Spinner *columns = nullptr;
    Fl_Check_Button *indexed_root = nullptr;
    Fl_Multiline_Input *source = nullptr;
    Fl_Box *status = nullptr;
    Fl_Button *accept = nullptr;
    Tool initial = Tool::Fraction;
    Text selected;
    Snippet generated;
    int configured_kind = -1;
    bool manual_source = false;
    bool generated_ok = false;
    bool accepted = false;
};

void configure_structure_variant(StructureDialogState &state) {
    const int kind = state.kind->value();
    if (state.configured_kind != kind) {
        const bool initial_matches = structure_kind(state.initial) == kind;
        state.variant->clear();
        int choice = 0;
        if (kind == 0) {
            state.variant_label->copy_label(T("Fraction style"));
            state.variant->add(T("Normal fraction"));
            state.variant->add(T("Display fraction"));
            if (initial_matches && state.initial == Tool::DisplayFraction) choice = 1;
        } else if (kind == 4) {
            state.variant_label->copy_label(T("Integral type"));
            state.variant->add(T("Single integral"));
            state.variant->add(T("Double integral"));
            state.variant->add(T("Triple integral"));
            state.variant->add(T("Closed integral"));
            if (initial_matches && state.initial == Tool::DoubleIntegral) choice = 1;
            else if (initial_matches && state.initial == Tool::TripleIntegral) choice = 2;
            else if (initial_matches && state.initial == Tool::ClosedIntegral) choice = 3;
        } else if (kind == 7) {
            state.variant_label->copy_label(T("Matrix form"));
            state.variant->add(T("Plain matrix"));
            state.variant->add(T("Parentheses"));
            state.variant->add(T("Square brackets"));
            if (initial_matches && state.initial == Tool::ParenthesizedMatrix) choice = 1;
            else if (initial_matches && state.initial == Tool::BracketedMatrix) choice = 2;
        }
        state.variant->value(choice);
        if (kind == 0 || kind == 4 || kind == 7) {
            state.variant->show();
            state.variant_label->show();
        } else {
            state.variant->hide();
            state.variant_label->hide();
        }
        state.configured_kind = kind;
    }
    const int maximum_columns = int(structure_column_limit(structure_tool(kind)));
    state.columns->maximum(maximum_columns);
    if (state.columns->value() > maximum_columns)
        state.columns->value(maximum_columns);
    if (kind == 1) state.indexed_root->show(); else state.indexed_root->hide();
    const bool dimensions = kind == 7 || kind == 8 || kind == 9;
    if (dimensions) {
        state.rows_label->show();
        state.columns_label->show();
        state.rows->show();
        state.columns->show();
    } else {
        state.rows_label->hide();
        state.columns_label->hide();
        state.rows->hide();
        state.columns->hide();
    }
}

Tool selected_structure_tool(const StructureDialogState &state) {
    int kind = state.kind->value();
    if (kind == 0) return state.variant->value() == 1 ? Tool::DisplayFraction : Tool::Fraction;
    if (kind == 1) return state.indexed_root->value() ? Tool::IndexedRoot : Tool::Root;
    if (kind == 4) {
        switch (state.variant->value()) {
        case 1: return Tool::DoubleIntegral;
        case 2: return Tool::TripleIntegral;
        case 3: return Tool::ClosedIntegral;
        default: return Tool::Integral;
        }
    }
    if (kind == 7) {
        switch (state.variant->value()) {
        case 1: return Tool::ParenthesizedMatrix;
        case 2: return Tool::BracketedMatrix;
        default: return Tool::Matrix;
        }
    }
    return structure_tool(kind);
}

void update_structure_preview(StructureDialogState &state) {
    configure_structure_variant(state);
    ToolArgs args{};
    args.rows = uint16_t(state.rows->value());
    args.columns = uint16_t(state.columns->value());
    args.numbered_root = state.indexed_root->value() != 0;
    args.selected = state.selected;
    Error error;
    Snippet generated;
    if (!build_snippet(selected_structure_tool(state), args, generated, error)) {
        state.generated_ok = false;
        state.accept->deactivate();
        state.source->value("");
        state.status->copy_label(T(error.message.c_str()));
        return;
    }
    state.generated = static_cast<Snippet &&>(generated);
    state.generated_ok = true;
    state.manual_source = false;
    state.source->value(state.generated.source.c_str());
    state.accept->activate();
    state.status->copy_label(T("Editable LaTeX source; generated placeholders support Tab."));
}

} // namespace

void MathWindow::symbol_dialog(bool templates, bool favorites) {
    if (favorites) {
        manage_favorites(*this);
        return;
    }

    SymbolDialogState state;
    state.owner = this;
    state.templates = templates;
    Fl_Window dialog(880, 600, templates ? T("Templates") :
                                            T("Symbols"));
    dialog.begin();
    make_label(18, 17, 130, 30, T("Search"));
    Fl_Input search(146, 17, 450, 30);
    search.tooltip(T("Search Chinese names, English names, categories, or commands."));
    make_label(610, 17, 105, 30, T("Category"));
    Fl_Choice category(710, 17, 152, 30);
    for (const Category &item : kCategories) {
        category.add(T(item.english));
    }
    category.value(0);
    Fl_Browser browser(18, 60, 844, 350);
    browser.textsize(14);
    make_label(18, 419, 844, 24, T("Command details"));
    Fl_Multiline_Output details(18, 444, 844, 84);
    details.textsize(14);
    Fl_Button insert(18, 546, 136, 34, T("Insert"));
    Fl_Button favorites_button(164, 546, 190, 34, T("Manage favorites"));
    Fl_Button close(746, 546, 116, 34, T("Close"));
    dialog.end();
    state.dialog = &dialog;
    state.search = &search;
    state.category = &category;
    state.browser = &browser;
    state.details = &details;
    search.callback([](Fl_Widget *, void *data) {
        refresh_symbols(*static_cast<SymbolDialogState *>(data));
    }, &state);
    category.callback([](Fl_Widget *, void *data) {
        refresh_symbols(*static_cast<SymbolDialogState *>(data));
    }, &state);
    browser.callback([](Fl_Widget *, void *data) {
        update_symbol_details(*static_cast<SymbolDialogState *>(data));
    }, &state);
    insert.callback([](Fl_Widget *, void *data) {
        SymbolDialogState &s = *static_cast<SymbolDialogState *>(data);
        int row = s.browser->value();
        if (row <= 0 || size_t(row) > s.matches.size()) return;
        const ToolEntry &entry = *s.matches[size_t(row - 1)];
        s.dialog->hide();
        if (is_structure(entry)) s.owner->structure_dialog(entry.tool);
        else s.owner->insert_tool(entry.tool);
    }, &state);
    favorites_button.callback([](Fl_Widget *, void *data) {
        SymbolDialogState &s = *static_cast<SymbolDialogState *>(data);
        s.dialog->hide();
        s.owner->favorite_dialog();
    }, &state);
    close.callback([](Fl_Widget *, void *data) {
        static_cast<SymbolDialogState *>(data)->dialog->hide();
    }, &state);
    refresh_symbols(state);
    run_modal(*this, dialog);
}

void MathWindow::favorite_dialog() {
    manage_favorites(*this);
}

void MathWindow::structure_dialog(Tool tool) {
    if (!current()) return;
    int selection_begin = editor->insert_position();
    int selection_end = selection_begin;
    source_.selection_position(&selection_begin, &selection_end);
    if (selection_begin > selection_end) {
        int saved = selection_begin;
        selection_begin = selection_end;
        selection_end = saved;
    }

    StructureDialogState state;
    state.initial = tool;
    if (selection_begin != selection_end) {
        char *selected = source_.text_range(selection_begin, selection_end);
        if (selected) {
            (void)state.selected.set(selected);
            free(selected);
        }
    }

    Fl_Window dialog(820, 600, T("Structure wizard"));
    dialog.begin();
    make_label(18, 17, 130, 30, T("Structure"));
    Fl_Choice kind(148, 17, 270, 30);
    const char *kind_labels[] = {
        T("Fraction"), T("Root"), T("Superscript"),
        T("Subscript"), T("Integral"), T("Summation"),
        T("Product"), T("Matrix"), T("Cases"),
        T("Aligned equations"), T("Text")
    };
    for (const char *label : kind_labels) kind.add(label);
    kind.value(structure_kind(tool));
    make_label(434, 17, 154, 30, T("Variant"));
    Fl_Choice variant(588, 17, 214, 30);
    Fl_Box variant_label(434, 48, 368, 24, T("Variant"));
    variant_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Box rows_label(18, 82, 108, 18, T("Rows"));
    rows_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Spinner rows(18, 102, 108, 28);
    rows.minimum(1);
    rows.maximum(16);
    rows.step(1);
    rows.value(2);
    Fl_Box columns_label(148, 82, 108, 18, T("Columns"));
    columns_label.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Spinner columns(148, 102, 108, 28);
    columns.minimum(1);
    columns.maximum(16);
    columns.step(1);
    columns.value(2);
    Fl_Check_Button indexed_root(280, 102, 240, 28,
                                 T("Indexed root"));
    indexed_root.value(tool == Tool::IndexedRoot);
    make_label(18, 138, 784, 24,
               T("Generated editable LaTeX source"));
    Fl_Multiline_Input source(18, 166, 784, 330);
    source.textfont(FL_COURIER);
    source.textsize(15);
    Fl_Box status(18, 503, 784, 34, "");
    status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Button accept(540, 548, 126, 34, T("Insert"));
    Fl_Button cancel(676, 548, 126, 34, T("Cancel"));
    dialog.end();

    state.dialog = &dialog;
    state.kind = &kind;
    state.variant = &variant;
    state.variant_label = &variant_label;
    state.rows_label = &rows_label;
    state.columns_label = &columns_label;
    state.rows = &rows;
    state.columns = &columns;
    state.indexed_root = &indexed_root;
    state.source = &source;
    state.status = &status;
    state.accept = &accept;
    update_structure_preview(state);
    kind.callback([](Fl_Widget *, void *data) {
        StructureDialogState &s = *static_cast<StructureDialogState *>(data);
        s.configured_kind = -1;
        update_structure_preview(s);
    }, &state);
    variant.callback([](Fl_Widget *, void *data) {
        update_structure_preview(*static_cast<StructureDialogState *>(data));
    }, &state);
    rows.callback([](Fl_Widget *, void *data) {
        update_structure_preview(*static_cast<StructureDialogState *>(data));
    }, &state);
    columns.callback([](Fl_Widget *, void *data) {
        update_structure_preview(*static_cast<StructureDialogState *>(data));
    }, &state);
    indexed_root.callback([](Fl_Widget *, void *data) {
        update_structure_preview(*static_cast<StructureDialogState *>(data));
    }, &state);
    source.callback([](Fl_Widget *, void *data) {
        StructureDialogState &s = *static_cast<StructureDialogState *>(data);
        s.manual_source = true;
        s.accept->activate();
        s.status->copy_label(T("Custom source will be inserted as typed; generated Tab stops are cleared."));
    }, &state);
    accept.callback([](Fl_Widget *, void *data) {
        StructureDialogState &s = *static_cast<StructureDialogState *>(data);
        s.accepted = true;
        s.dialog->hide();
    }, &state);
    cancel.callback([](Fl_Widget *, void *data) {
        static_cast<StructureDialogState *>(data)->dialog->hide();
    }, &state);
    run_modal(*this, dialog);

    if (!state.accepted) return;
    Snippet insertion;
    if (state.manual_source) {
        if (!insertion.source.set(source.value())) {
            fl_alert(T("The source is invalid UTF-8 or exceeds the source limit."));
            return;
        }
    } else {
        insertion = static_cast<Snippet &&>(state.generated);
    }
    Error error;
    if (!editor_insert_snippet(core,
                               {size_t(selection_begin), size_t(selection_end)},
                               insertion, error)) {
        update_status(T(error.message.c_str()));
        return;
    }
    const int inserted_end = int(size_t(selection_begin) + insertion.source.size());
    commit_core(true);
    if (insertion.stops.empty()) {
        source_.select(inserted_end, inserted_end);
        editor->insert_position(inserted_end);
        editor->show_insert_position();
    }
}

namespace {

struct FindDialogState {
    MathWindow *owner = nullptr;
    Fl_Window *dialog = nullptr;
    Fl_Input *query = nullptr;
    Fl_Input *replacement = nullptr;
    Fl_Box *status = nullptr;
    Fl_Text_Buffer *buffer = nullptr;
    bool replace_mode = false;
};

bool find_next_match(FindDialogState &state) {
    const char *needle = state.query->value();
    if (!needle || !*needle) {
        state.status->copy_label(T("Enter text to find."));
        return false;
    }
    int selection_begin = 0, selection_end = 0;
    state.buffer->selection_position(&selection_begin, &selection_end);
    size_t start = selection_end > selection_begin ? size_t(selection_end) :
                   size_t(state.owner->editor->insert_position());
    Range match{};
    Error error;
    bool found = find_text(state.owner->core.source.c_str(), needle, start,
                           match, error);
    if (!found && start) {
        error.clear();
        found = find_text(state.owner->core.source.c_str(), needle, 0,
                          match, error);
    }
    if (!found) {
        state.status->copy_label(T("No matching text was found."));
        return false;
    }
    state.buffer->select(int(match.begin), int(match.end));
    state.owner->editor->insert_position(int(match.end));
    state.owner->editor->show_insert_position();
    state.status->copy_label(T("Match selected."));
    return true;
}

bool selected_text_equals(Fl_Text_Buffer &buffer, const char *query,
                          Range &selection) {
    int begin = 0, end = 0;
    if (!buffer.selection_position(&begin, &end) || begin == end) return false;
    if (begin > end) {
        int saved = begin;
        begin = end;
        end = saved;
    }
    char *selected = buffer.text_range(begin, end);
    bool same = selected && strcmp(selected, query) == 0;
    free(selected);
    if (same) selection = {size_t(begin), size_t(end)};
    return same;
}

} // namespace

void MathWindow::find_dialog(bool replace) {
    flush_edit();
    FindDialogState state;
    state.owner = this;
    state.replace_mode = replace;
    state.buffer = &source_;
    const int height = replace ? 370 : 300;
    Fl_Window dialog(720, height, replace ? T("Find and replace") :
                                             T("Find"));
    dialog.begin();
    make_label(18, 20, 150, 30, T("Find"));
    Fl_Input query(174, 20, 528, 30);
    query.when(FL_WHEN_ENTER_KEY_ALWAYS);
    make_label(18, 70, 150, 30, T("Replace with"));
    Fl_Input replacement(174, 70, 528, 30);
    Fl_Box status(18, replace ? 118 : 78, 684, 42, "");
    status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Button next(18, height - 54, 126, 34, T("Find next"));
    Fl_Button one(154, height - 54, 126, 34, T("Replace"));
    Fl_Button all(290, height - 54, 146, 34, T("Replace all"));
    Fl_Button close(576, height - 54, 126, 34, T("Close"));
    dialog.end();
    state.dialog = &dialog;
    state.query = &query;
    state.replacement = &replacement;
    state.status = &status;
    if (replace) {
        one.show();
        all.show();
        replacement.show();
    } else {
        one.hide();
        all.hide();
        replacement.hide();
    }
    next.callback([](Fl_Widget *, void *data) {
        find_next_match(*static_cast<FindDialogState *>(data));
    }, &state);
    query.callback([](Fl_Widget *, void *data) {
        find_next_match(*static_cast<FindDialogState *>(data));
    }, &state);
    one.callback([](Fl_Widget *, void *data) {
        FindDialogState &s = *static_cast<FindDialogState *>(data);
        const char *needle = s.query->value();
        if (!needle || !*needle) {
            s.status->copy_label(T("Enter text to find."));
            return;
        }
        Range selection{};
        if (!selected_text_equals(*s.buffer, needle, selection)) {
            (void)find_next_match(s);
            return;
        }
        Text updated;
        if (!updated.set(s.owner->core.source.c_str())) {
            s.status->copy_label(T("Could not prepare the source edit."));
            return;
        }
        Error error;
        const char *insert = s.replacement->value();
        if (!replace_text(updated, selection, insert, error)) {
            s.status->copy_label(T(error.message.c_str()));
            return;
        }
        if (!editor_set_text(s.owner->core, updated.c_str(), error)) {
            s.status->copy_label(T(error.message.c_str()));
            return;
        }
        s.owner->commit_core(false);
        size_t caret = selection.begin + strlen(insert);
        s.buffer->select(int(caret), int(caret));
        s.owner->editor->insert_position(int(caret));
        s.owner->editor->show_insert_position();
        s.query->take_focus();
        s.status->copy_label(T("One match replaced."));
    }, &state);
    all.callback([](Fl_Widget *, void *data) {
        FindDialogState &s = *static_cast<FindDialogState *>(data);
        const char *needle = s.query->value();
        if (!needle || !*needle) {
            s.status->copy_label(T("Enter text to find."));
            return;
        }
        Text updated;
        Error error;
        size_t count = 0;
        if (!replace_all_utf8(s.owner->core.source.c_str(), needle,
                              s.replacement->value(), updated, count, error)) {
            s.status->copy_label(T(error.message.c_str()));
            return;
        }
        if (!count) {
            s.status->copy_label(T("No matching text was found."));
            return;
        }
        if (!editor_set_text(s.owner->core, updated.c_str(), error)) {
            s.status->copy_label(T(error.message.c_str()));
            return;
        }
        s.owner->commit_core(false);
        const int caret = int(updated.size());
        s.buffer->select(caret, caret);
        s.owner->editor->insert_position(caret);
        s.owner->editor->show_insert_position();
        s.query->take_focus();
        char message[160];
        snprintf(message, sizeof message, T("Replaced %zu matches."), count);
        s.status->copy_label(message);
    }, &state);
    close.callback([](Fl_Widget *, void *data) {
        static_cast<FindDialogState *>(data)->dialog->hide();
    }, &state);
    query.take_focus();
    run_modal(*this, dialog);
}

namespace {

struct CompletionDialogState {
    Fl_Window *dialog = nullptr;
    Fl_Browser *browser = nullptr;
    Fl_Multiline_Output *details = nullptr;
    Fl_Button *insert = nullptr;
    const ToolEntry **entries = nullptr;
    size_t count = 0;
    bool accepted = false;
};

void update_completion_details(CompletionDialogState &state) {
    const int row = state.browser->value();
    if (row <= 0 || size_t(row) > state.count) {
        state.details->value(T("Select a command to see its insertion form and example."));
        state.insert->deactivate();
        return;
    }
    const ToolEntry &entry = *state.entries[size_t(row - 1)];
    char detail[2048];
    snprintf(detail, sizeof detail, "%s\n%s: %s\n%s: %s\n%s: %s\n%s: %s",
             T(entry.msgid), T("Category"), T(entry.category),
             T("Command"), entry.command, T("Insertion"), entry.insert,
             T("Example"), entry.help_example);
    state.details->value(detail);
    state.insert->activate();
}

void append_help_line(Fl_Text_Buffer &buffer, const char *line) {
    buffer.append(line ? line : "");
    buffer.append("\n");
}

void append_help_field(Fl_Text_Buffer &buffer, const char *label,
                       const char *value) {
    buffer.append("  ");
    buffer.append(label ? label : "");
    buffer.append(": ");
    buffer.append(value ? value : "");
    buffer.append("\n");
}

} // namespace

void MathWindow::complete() {
    flush_edit();
    Text prefix;
    Range replace_range{};
    Error error;
    if (!command_completion_prefix(core.source.c_str(),
                                   size_t(editor->insert_position()),
                                   prefix, replace_range, error)) {
        fl_alert(T("Place the caret inside a backslash command to complete it."));
        return;
    }

    size_t catalog_size = 0;
    (void)catalog_entries(&catalog_size);
    Array<const ToolEntry *> matches;
    if (!matches.resize(catalog_size)) {
        fl_alert(T("Not enough memory to show command completions."));
        return;
    }
    const size_t count = complete_command(prefix.c_str(), matches.data(), matches.size());
    if (!count) {
        update_status(T("No catalog command matches this prefix."));
        return;
    }

    CompletionDialogState state;
    state.count = count;
    state.entries = matches.data();
    Fl_Window dialog(860, 560, T("Complete command"));
    dialog.begin();
    make_label(18, 14, 150, 28, T("Command prefix"));
    Fl_Input prefix_view(174, 14, 668, 28);
    prefix_view.value(prefix.c_str());
    prefix_view.deactivate();
    make_label(18, 52, 400, 24, T("Matching commands"));
    make_label(474, 52, 368, 24, T("Command details"));
    Fl_Browser browser(18, 80, 438, 408);
    browser.textsize(14);
    Fl_Multiline_Output details(474, 80, 368, 408);
    details.textsize(14);
    Fl_Button insert(592, 510, 118, 34, T("Insert"));
    Fl_Button close(724, 510, 118, 34, T("Close"));
    dialog.end();
    state.dialog = &dialog;
    state.browser = &browser;
    state.details = &details;
    state.insert = &insert;
    for (size_t index = 0; index < count; ++index) {
        const ToolEntry &entry = *matches[index];
        char row[512];
        snprintf(row, sizeof row, "%s    %s", T(entry.msgid), entry.command);
        browser.add(row);
    }
    if (count) browser.value(1);
    update_completion_details(state);
    browser.callback([](Fl_Widget *, void *data) {
        update_completion_details(*static_cast<CompletionDialogState *>(data));
    }, &state);
    insert.callback([](Fl_Widget *, void *data) {
        CompletionDialogState &s = *static_cast<CompletionDialogState *>(data);
        if (s.browser->value() > 0 && size_t(s.browser->value()) <= s.count) {
            s.accepted = true;
            s.dialog->hide();
        }
    }, &state);
    close.callback([](Fl_Widget *, void *data) {
        static_cast<CompletionDialogState *>(data)->dialog->hide();
    }, &state);
    run_modal(*this, dialog);

    const int row = browser.value();
    if (state.accepted && row > 0 && size_t(row) <= count) {
        const ToolEntry &entry = *matches[size_t(row - 1)];
        source_.select(int(replace_range.begin), int(replace_range.end));
        editor->insert_position(int(replace_range.end));
        insert_source(entry.command);
    }
}

void MathWindow::help_dialog() {
    Fl_Text_Buffer help;
    append_help_line(help, T("ReliefMath help"));
    append_help_line(help, T("ReliefMath edits UTF-8 math source and renders a supported LaTeX-like subset."));
    append_help_line(help, "");

    append_help_line(help, T("Keyboard shortcuts"));
    append_help_line(help, T("Ctrl+N  New document"));
    append_help_line(help, T("Ctrl+O  Open document"));
    append_help_line(help, T("Ctrl+S  Save document"));
    append_help_line(help, T("Ctrl+Shift+S  Save as"));
    append_help_line(help, T("Ctrl+Z  Undo"));
    append_help_line(help, T("Ctrl+Y or Ctrl+Shift+Z  Redo"));
    append_help_line(help, T("Ctrl+A  Select all"));
    append_help_line(help, T("Ctrl+C  Copy"));
    append_help_line(help, T("Ctrl+X  Cut"));
    append_help_line(help, T("Ctrl+V  Paste"));
    append_help_line(help, T("Ctrl+F  Find"));
    append_help_line(help, T("Ctrl+H  Find and replace"));
    append_help_line(help, T("Ctrl+Space  Complete the backslash command at the caret"));
    append_help_line(help, T("Tab and Shift+Tab  Move between generated placeholders"));
    append_help_line(help, T("F1  Open this help"));
    append_help_line(help, "");

    append_help_line(help, T("Source and rendering limits"));
    append_help_line(help, T("A document can contain up to 256 formulas; a formula name is limited to 256 UTF-8 bytes."));
    append_help_line(help, T("Each formula source is limited to 64 KiB and nesting is limited to 128 levels."));
    append_help_line(help, T("Matrices must be rectangular and no larger than 16 rows by 16 columns."));
    append_help_line(help, T("A preview or export is limited to 16,384 pixels per dimension and 16,777,216 raster pixels."));
    append_help_line(help, T("ReliefMath is not a full TeX engine; arbitrary macros, packages, file input, and unknown environments are unsupported."));
    append_help_line(help, "");

    append_help_line(help, T("Built-in tools, commands, and examples"));
    size_t count = 0;
    const ToolEntry *entries = catalog_entries(&count);
    for (size_t index = 0; index < count; ++index) {
        const ToolEntry &entry = entries[index];
        append_help_line(help, T(entry.msgid));
        append_help_field(help, T("Category"), T(entry.category));
        append_help_field(help, T("Command"), entry.command);
        append_help_field(help, T("Insertion"), entry.insert);
        append_help_field(help, T("Parameters"), T(entry.parameters));
        append_help_field(help, T("Example"), entry.help_example);
        help.append("\n");
    }

    Fl_Window dialog(920, 660, T("ReliefMath help"));
    dialog.begin();
    Fl_Text_Display display(18, 16, 884, 572);
    display.buffer(&help);
    display.textsize(14);
    display.wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    Fl_Button close(774, 608, 128, 34, T("Close"));
    dialog.end();
    close.callback([](Fl_Widget *widget, void *) { widget->window()->hide(); });
    run_modal(*this, dialog);
}

} // namespace rm
