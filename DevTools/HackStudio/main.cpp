/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "CursorTool.h"
#include "Debugger/DebugInfoWidget.h"
#include "Debugger/Debugger.h"
#include "Editor.h"
#include "EditorWrapper.h"
#include "FindInFilesWidget.h"
#include "FormEditorWidget.h"
#include "FormWidget.h"
#include "Locator.h"
#include "Project.h"
#include "TerminalWrapper.h"
#include "WidgetTool.h"
#include "WidgetTreeModel.h"
#include <AK/StringBuilder.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibDebug/DebugSession.h>
#include <LibGUI/AboutDialog.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Button.h>
#include <LibGUI/CppSyntaxHighlighter.h>
#include <LibGUI/FilePicker.h>
#include <LibGUI/INISyntaxHighlighter.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/JSSyntaxHighlighter.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MenuBar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Splitter.h>
#include <LibGUI/StackWidget.h>
#include <LibGUI/TabWidget.h>
#include <LibGUI/TableView.h>
#include <LibGUI/TextBox.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/ToolBar.h>
#include <LibGUI/ToolBarContainer.h>
#include <LibGUI/TreeView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibThread/Lock.h>
#include <LibThread/Thread.h>
#include <LibVT/TerminalWidget.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

NonnullRefPtrVector<EditorWrapper> g_all_editor_wrappers;
RefPtr<EditorWrapper> g_current_editor_wrapper;
Function<void(String)> g_open_file;

String g_currently_open_file;
OwnPtr<Project> g_project;
RefPtr<GUI::Window> g_window;
RefPtr<GUI::TreeView> g_project_tree_view;
RefPtr<GUI::StackWidget> g_right_hand_stack;
RefPtr<GUI::Splitter> g_text_inner_splitter;
RefPtr<GUI::Widget> g_form_inner_container;
RefPtr<FormEditorWidget> g_form_editor_widget;

static RefPtr<GUI::TabWidget> s_action_tab_widget;

void add_new_editor(GUI::Widget& parent)
{
    auto wrapper = EditorWrapper::construct(Debugger::on_breakpoint_change);
    if (s_action_tab_widget) {
        parent.insert_child_before(wrapper, *s_action_tab_widget);
    } else {
        parent.add_child(wrapper);
    }
    g_current_editor_wrapper = wrapper;
    g_all_editor_wrappers.append(wrapper);
    wrapper->editor().set_focus(true);
}

enum class EditMode {
    Text,
    Form,
};

void set_edit_mode(EditMode mode)
{
    if (mode == EditMode::Text) {
        g_right_hand_stack->set_active_widget(g_text_inner_splitter);
    } else if (mode == EditMode::Form) {
        g_right_hand_stack->set_active_widget(g_form_inner_container);
    }
}

EditorWrapper& current_editor_wrapper()
{
    ASSERT(g_current_editor_wrapper);
    return *g_current_editor_wrapper;
}

Editor& current_editor()
{
    return current_editor_wrapper().editor();
}

NonnullRefPtr<EditorWrapper> get_editor_of_file(const String& file)
{
    for (auto& wrapper : g_all_editor_wrappers) {
        String wrapper_file = wrapper.filename_label().text();
        if (wrapper_file == file || String::format("./%s", wrapper_file.characters()) == file) {
            return wrapper;
        }
    }
    ASSERT_NOT_REACHED();
}

String get_project_executable_path()
{
    // e.g /my/project.files => /my/project
    // TODO: Perhaps a Makefile rule for getting the value of $(PROGRAM) would be better?
    return g_project->path().substring(0, g_project->path().index_of(".").value());
}

static void build(TerminalWrapper&);
static void run(TerminalWrapper&);
void open_project(String);
void open_file(const String&);
bool make_is_available();

int main(int argc, char** argv)
{
    if (pledge("stdio tty accept rpath cpath wpath shared_buffer proc exec unix fattr thread", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    GUI::Application app(argc, argv);

    if (pledge("stdio tty accept rpath cpath wpath shared_buffer proc exec fattr thread", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    Function<void()> update_actions;

    g_window = GUI::Window::construct();
    g_window->set_rect(90, 90, 840, 600);
    g_window->set_title("HackStudio");

    auto& widget = g_window->set_main_widget<GUI::Widget>();

    widget.set_fill_with_background_color(true);
    widget.set_layout<GUI::VerticalBoxLayout>();
    widget.layout()->set_spacing(2);

    StringBuilder path;
    path.append(getenv("PATH"));
    if (path.length())
        path.append(":");
    path.append("/bin:/usr/bin:/usr/local/bin");
    setenv("PATH", path.to_string().characters(), true);

    if (!make_is_available())
        GUI::MessageBox::show("The 'make' command is not available. You probably want to install the binutils, gcc, and make ports from the root of the Serenity repository.", "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);

    open_project("/home/anon/little/little.files");

    auto& toolbar_container = widget.add<GUI::ToolBarContainer>();
    auto& toolbar = toolbar_container.add<GUI::ToolBar>();

    auto selected_file_names = [&] {
        Vector<String> files;
        g_project_tree_view->selection().for_each_index([&](const GUI::ModelIndex& index) {
            files.append(g_project->model().data(index).as_string());
        });
        return files;
    };

    auto new_action = GUI::Action::create("Add new file to project...", { Mod_Ctrl, Key_N }, Gfx::Bitmap::load_from_file("/res/icons/16x16/new.png"), [&](const GUI::Action&) {
        auto input_box = GUI::InputBox::construct("Enter name of new file:", "Add new file to project", g_window);
        if (input_box->exec() == GUI::InputBox::ExecCancel)
            return;
        auto filename = input_box->text_value();
        auto file = Core::File::construct(filename);
        if (!file->open((Core::IODevice::OpenMode)(Core::IODevice::WriteOnly | Core::IODevice::MustBeNew))) {
            GUI::MessageBox::show(String::format("Failed to create '%s'", filename.characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            return;
        }
        if (!g_project->add_file(filename)) {
            GUI::MessageBox::show(String::format("Failed to add '%s' to project", filename.characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            // FIXME: Should we unlink the file here maybe?
            return;
        }
        g_project_tree_view->toggle_index(g_project_tree_view->model()->index(0, 0));
        open_file(filename);
    });

    auto add_existing_file_action = GUI::Action::create("Add existing file to project...", Gfx::Bitmap::load_from_file("/res/icons/16x16/open.png"), [&](auto&) {
        auto result = GUI::FilePicker::get_open_filepath("Add existing file to project");
        if (!result.has_value())
            return;
        auto& filename = result.value();
        if (!g_project->add_file(filename)) {
            GUI::MessageBox::show(String::format("Failed to add '%s' to project", filename.characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            return;
        }
        g_project_tree_view->toggle_index(g_project_tree_view->model()->index(0, 0));
        open_file(filename);
    });

    auto delete_action = GUI::CommonActions::make_delete_action([&](const GUI::Action& action) {
        (void)action;

        auto files = selected_file_names();
        if (files.is_empty())
            return;

        String message;
        if (files.size() == 1) {
            message = String::format("Really remove %s from the project?", LexicalPath(files[0]).basename().characters());
        } else {
            message = String::format("Really remove %d files from the project?", files.size());
        }

        auto result = GUI::MessageBox::show(
            message,
            "Confirm deletion",
            GUI::MessageBox::Type::Warning,
            GUI::MessageBox::InputType::OKCancel,
            g_window);
        if (result == GUI::MessageBox::ExecCancel)
            return;

        for (auto& file : files) {
            if (!g_project->remove_file(file)) {
                GUI::MessageBox::show(
                    String::format("Removing file %s from the project failed.", file.characters()),
                    "Removal failed",
                    GUI::MessageBox::Type::Error,
                    GUI::MessageBox::InputType::OK,
                    g_window);
                break;
            }
        }
    });
    delete_action->set_enabled(false);

    auto project_tree_view_context_menu = GUI::Menu::construct("Project Files");
    project_tree_view_context_menu->add_action(new_action);
    project_tree_view_context_menu->add_action(add_existing_file_action);
    project_tree_view_context_menu->add_action(delete_action);

    auto& outer_splitter = widget.add<GUI::HorizontalSplitter>();
    g_project_tree_view = outer_splitter.add<GUI::TreeView>();
    g_project_tree_view->set_model(g_project->model());
    g_project_tree_view->set_size_policy(GUI::SizePolicy::Fixed, GUI::SizePolicy::Fill);
    g_project_tree_view->set_preferred_size(140, 0);
    g_project_tree_view->toggle_index(g_project_tree_view->model()->index(0, 0));

    g_project_tree_view->on_context_menu_request = [&](const GUI::ModelIndex& index, const GUI::ContextMenuEvent& event) {
        if (index.is_valid()) {
            project_tree_view_context_menu->popup(event.screen_position());
        }
    };

    g_project_tree_view->on_selection_change = [&] {
        delete_action->set_enabled(!g_project_tree_view->selection().is_empty());
    };

    g_right_hand_stack = outer_splitter.add<GUI::StackWidget>();

    g_form_inner_container = g_right_hand_stack->add<GUI::Widget>();
    g_form_inner_container->set_layout<GUI::HorizontalBoxLayout>();
    auto& form_widgets_toolbar = g_form_inner_container->add<GUI::ToolBar>(Orientation::Vertical, 26);
    form_widgets_toolbar.set_preferred_size(38, 0);

    GUI::ActionGroup tool_actions;
    tool_actions.set_exclusive(true);

    auto cursor_tool_action = GUI::Action::create_checkable("Cursor", Gfx::Bitmap::load_from_file("/res/icons/widgets/Cursor.png"), [&](auto&) {
        g_form_editor_widget->set_tool(make<CursorTool>(*g_form_editor_widget));
    });
    cursor_tool_action->set_checked(true);
    tool_actions.add_action(cursor_tool_action);

    form_widgets_toolbar.add_action(cursor_tool_action);

    GUI::WidgetClassRegistration::for_each([&](const GUI::WidgetClassRegistration& reg) {
        auto icon_path = String::format("/res/icons/widgets/G%s.png", reg.class_name().characters());
        auto action = GUI::Action::create_checkable(reg.class_name(), Gfx::Bitmap::load_from_file(icon_path), [&reg](auto&) {
            g_form_editor_widget->set_tool(make<WidgetTool>(*g_form_editor_widget, reg));
            auto widget = reg.construct();
            g_form_editor_widget->form_widget().add_child(widget);
            widget->set_relative_rect(30, 30, 30, 30);
            g_form_editor_widget->model().update();
        });
        action->set_checked(false);
        tool_actions.add_action(action);
        form_widgets_toolbar.add_action(move(action));
    });

    auto& form_editor_inner_splitter = g_form_inner_container->add<GUI::HorizontalSplitter>();

    g_form_editor_widget = form_editor_inner_splitter.add<FormEditorWidget>();

    auto& form_editing_pane_container = form_editor_inner_splitter.add<GUI::VerticalSplitter>();
    form_editing_pane_container.set_size_policy(GUI::SizePolicy::Fixed, GUI::SizePolicy::Fill);
    form_editing_pane_container.set_preferred_size(190, 0);
    form_editing_pane_container.set_layout<GUI::VerticalBoxLayout>();

    auto add_properties_pane = [&](auto& text, auto pane_widget) {
        auto& wrapper = form_editing_pane_container.add<GUI::Widget>();
        wrapper.set_layout<GUI::VerticalBoxLayout>();
        auto& label = wrapper.add<GUI::Label>(text);
        label.set_fill_with_background_color(true);
        label.set_text_alignment(Gfx::TextAlignment::CenterLeft);
        label.set_font(Gfx::Font::default_bold_font());
        label.set_size_policy(GUI::SizePolicy::Fill, GUI::SizePolicy::Fixed);
        label.set_preferred_size(0, 16);
        wrapper.add_child(pane_widget);
    };

    auto form_widget_tree_view = GUI::TreeView::construct();
    form_widget_tree_view->set_model(g_form_editor_widget->model());
    form_widget_tree_view->on_selection_change = [&] {
        g_form_editor_widget->selection().disable_hooks();
        g_form_editor_widget->selection().clear();
        form_widget_tree_view->selection().for_each_index([&](auto& index) {
            // NOTE: Make sure we don't add the FormWidget itself to the selection,
            //       since that would allow you to drag-move the FormWidget.
            if (index.internal_data() != &g_form_editor_widget->form_widget())
                g_form_editor_widget->selection().add(*(GUI::Widget*)index.internal_data());
        });
        g_form_editor_widget->update();
        g_form_editor_widget->selection().enable_hooks();
    };

    g_form_editor_widget->selection().on_add = [&](auto& widget) {
        form_widget_tree_view->selection().add(g_form_editor_widget->model().index_for_widget(widget));
    };
    g_form_editor_widget->selection().on_remove = [&](auto& widget) {
        form_widget_tree_view->selection().remove(g_form_editor_widget->model().index_for_widget(widget));
    };
    g_form_editor_widget->selection().on_clear = [&] {
        form_widget_tree_view->selection().clear();
    };

    add_properties_pane("Form widget tree:", form_widget_tree_view);
    add_properties_pane("Widget properties:", GUI::TableView::construct());

    g_text_inner_splitter = g_right_hand_stack->add<GUI::VerticalSplitter>();
    g_text_inner_splitter->layout()->set_margins({ 0, 3, 0, 0 });
    add_new_editor(*g_text_inner_splitter);

    auto switch_to_next_editor = GUI::Action::create("Switch to next editor", { Mod_Ctrl, Key_E }, [&](auto&) {
        if (g_all_editor_wrappers.size() <= 1)
            return;
        Vector<EditorWrapper*> wrappers;
        g_text_inner_splitter->for_each_child_of_type<EditorWrapper>([&](auto& child) {
            wrappers.append(&child);
            return IterationDecision::Continue;
        });
        for (size_t i = 0; i < wrappers.size(); ++i) {
            if (g_current_editor_wrapper.ptr() == wrappers[i]) {
                if (i == wrappers.size() - 1)
                    wrappers[0]->editor().set_focus(true);
                else
                    wrappers[i + 1]->editor().set_focus(true);
            }
        }
    });

    auto switch_to_previous_editor = GUI::Action::create("Switch to previous editor", { Mod_Ctrl | Mod_Shift, Key_E }, [&](auto&) {
        if (g_all_editor_wrappers.size() <= 1)
            return;
        Vector<EditorWrapper*> wrappers;
        g_text_inner_splitter->for_each_child_of_type<EditorWrapper>([&](auto& child) {
            wrappers.append(&child);
            return IterationDecision::Continue;
        });
        for (int i = wrappers.size() - 1; i >= 0; --i) {
            if (g_current_editor_wrapper.ptr() == wrappers[i]) {
                if (i == 0)
                    wrappers.last()->editor().set_focus(true);
                else
                    wrappers[i - 1]->editor().set_focus(true);
            }
        }
    });

    auto remove_current_editor_action = GUI::Action::create("Remove current editor", { Mod_Alt | Mod_Shift, Key_E }, [&](auto&) {
        if (g_all_editor_wrappers.size() <= 1)
            return;
        auto wrapper = g_current_editor_wrapper;
        switch_to_next_editor->activate();
        g_text_inner_splitter->remove_child(*wrapper);
        g_all_editor_wrappers.remove_first_matching([&](auto& entry) { return entry == wrapper.ptr(); });
        update_actions();
    });

    auto open_action = GUI::Action::create("Open project...", { Mod_Ctrl | Mod_Shift, Key_O }, Gfx::Bitmap::load_from_file("/res/icons/16x16/open.png"), [&](auto&) {
        auto open_path = GUI::FilePicker::get_open_filepath("Open project");
        if (!open_path.has_value())
            return;
        open_project(open_path.value());
        open_file(g_project->default_file());
        update_actions();
    });

    auto save_action = GUI::Action::create("Save", { Mod_Ctrl, Key_S }, Gfx::Bitmap::load_from_file("/res/icons/16x16/save.png"), [&](auto&) {
        if (g_currently_open_file.is_empty())
            return;
        current_editor().write_to_file(g_currently_open_file);
    });

    toolbar.add_action(new_action);
    toolbar.add_action(add_existing_file_action);
    toolbar.add_action(save_action);
    toolbar.add_action(delete_action);
    toolbar.add_separator();

    toolbar.add_action(GUI::CommonActions::make_cut_action([&](auto&) { current_editor().cut_action().activate(); }));
    toolbar.add_action(GUI::CommonActions::make_copy_action([&](auto&) { current_editor().copy_action().activate(); }));
    toolbar.add_action(GUI::CommonActions::make_paste_action([&](auto&) { current_editor().paste_action().activate(); }));
    toolbar.add_separator();
    toolbar.add_action(GUI::CommonActions::make_undo_action([&](auto&) { current_editor().undo_action().activate(); }));
    toolbar.add_action(GUI::CommonActions::make_redo_action([&](auto&) { current_editor().redo_action().activate(); }));
    toolbar.add_separator();

    g_project_tree_view->on_activation = [&](auto& index) {
        auto filename = g_project_tree_view->model()->data(index, GUI::Model::Role::Custom).to_string();
        open_file(filename);
    };

    s_action_tab_widget = g_text_inner_splitter->add<GUI::TabWidget>();

    s_action_tab_widget->set_size_policy(GUI::SizePolicy::Fill, GUI::SizePolicy::Fixed);
    s_action_tab_widget->set_preferred_size(0, 24);

    s_action_tab_widget->on_change = [&](auto&) { update_actions(); };

    auto reveal_action_tab = [&](auto& widget) {
        if (s_action_tab_widget->preferred_size().height() < 200)
            s_action_tab_widget->set_preferred_size(0, 200);
        s_action_tab_widget->set_active_widget(&widget);
    };

    auto hide_action_tabs = [&] {
        s_action_tab_widget->set_preferred_size(0, 24);
    };

    auto hide_action_tabs_action = GUI::Action::create("Hide action tabs", { Mod_Ctrl | Mod_Shift, Key_X }, [&](auto&) {
        hide_action_tabs();
    });

    auto add_editor_action = GUI::Action::create("Add new editor", { Mod_Ctrl | Mod_Alt, Key_E },
        Gfx::Bitmap::load_from_file("/res/icons/TextEditor16.png"),
        [&](auto&) {
            add_new_editor(*g_text_inner_splitter);
            update_actions();
        });

    auto add_terminal_action = GUI::Action::create("Add new Terminal", { Mod_Ctrl | Mod_Alt, Key_T },
        Gfx::Bitmap::load_from_file("/res/icons/16x16/app-terminal.png"),
        [&](auto&) {
            auto& terminal = s_action_tab_widget->add_tab<TerminalWrapper>("Terminal");
            reveal_action_tab(terminal);
            update_actions();
            terminal.terminal()->set_focus(true);
        });

    auto remove_current_terminal_action = GUI::Action::create("Remove current Terminal", { Mod_Alt | Mod_Shift, Key_T }, [&](auto&) {
        auto widget = s_action_tab_widget->active_widget();
        if (!widget)
            return;
        if (strcmp(widget->class_name(), "TerminalWrapper") != 0)
            return;
        auto terminal = reinterpret_cast<TerminalWrapper*>(widget);
        if (!terminal->user_spawned())
            return;

        s_action_tab_widget->remove_tab(*terminal);
        update_actions();
    });

    auto& find_in_files_widget = s_action_tab_widget->add_tab<FindInFilesWidget>("Find in files");
    auto& terminal_wrapper = s_action_tab_widget->add_tab<TerminalWrapper>("Build", false);
    auto& debug_info_widget = s_action_tab_widget->add_tab<DebugInfoWidget>("Debug");

    auto& locator = widget.add<Locator>();

    auto open_locator_action = GUI::Action::create("Open Locator...", { Mod_Ctrl, Key_K }, [&](auto&) {
        locator.open();
    });

    auto menubar = GUI::MenuBar::construct();
    auto& app_menu = menubar->add_menu("HackStudio");
    app_menu.add_action(open_action);
    app_menu.add_action(save_action);
    app_menu.add_separator();
    app_menu.add_action(GUI::CommonActions::make_quit_action([&](auto&) {
        app.quit();
    }));

    auto& project_menu = menubar->add_menu("Project");
    project_menu.add_action(new_action);
    project_menu.add_action(add_existing_file_action);

    auto& edit_menu = menubar->add_menu("Edit");
    edit_menu.add_action(GUI::Action::create("Find in files...", { Mod_Ctrl | Mod_Shift, Key_F }, Gfx::Bitmap::load_from_file("/res/icons/16x16/find.png"), [&](auto&) {
        reveal_action_tab(find_in_files_widget);
        find_in_files_widget.focus_textbox_and_select_all();
    }));

    auto stop_action = GUI::Action::create("Stop", Gfx::Bitmap::load_from_file("/res/icons/16x16/program-stop.png"), [&](auto&) {
        terminal_wrapper.kill_running_command();
    });

    stop_action->set_enabled(false);
    terminal_wrapper.on_command_exit = [&] {
        stop_action->set_enabled(false);
    };

    auto build_action = GUI::Action::create("Build", { Mod_Ctrl, Key_B }, Gfx::Bitmap::load_from_file("/res/icons/16x16/build.png"), [&](auto&) {
        reveal_action_tab(terminal_wrapper);
        build(terminal_wrapper);
        stop_action->set_enabled(true);
    });
    toolbar.add_action(build_action);
    toolbar.add_separator();

    auto run_action = GUI::Action::create("Run", { Mod_Ctrl, Key_R }, Gfx::Bitmap::load_from_file("/res/icons/16x16/program-run.png"), [&](auto&) {
        reveal_action_tab(terminal_wrapper);
        run(terminal_wrapper);
        stop_action->set_enabled(true);
    });

    RefPtr<LibThread::Thread> debugger_thread;
    auto debug_action = GUI::Action::create("Debug", Gfx::Bitmap::load_from_file("/res/icons/16x16/debug-run.png"), [&](auto&) {
        if (g_project->type() != ProjectType::Cpp) {
            GUI::MessageBox::show(String::format("Cannot debug current project type", get_project_executable_path().characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            return;
        }
        if (!GUI::FilePicker::file_exists(get_project_executable_path())) {
            GUI::MessageBox::show(String::format("Could not find file: %s. (did you build the project?)", get_project_executable_path().characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            return;
        }
        if (Debugger::the().session()) {
            GUI::MessageBox::show("Debugger is already running", "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, g_window);
            return;
        }
        Debugger::the().set_executable_path(get_project_executable_path());
        debugger_thread = adopt(*new LibThread::Thread(Debugger::start_static));
        debugger_thread->start();
    });

    auto continue_action = GUI::Action::create("Continue", Gfx::Bitmap::load_from_file("/res/icons/16x16/debug-continue.png"), [&](auto&) {
        pthread_mutex_lock(Debugger::the().continue_mutex());
        Debugger::the().set_continue_type(Debugger::ContinueType::Continue);
        pthread_cond_signal(Debugger::the().continue_cond());
        pthread_mutex_unlock(Debugger::the().continue_mutex());
    });

    auto single_step_action = GUI::Action::create("Single Step", Gfx::Bitmap::load_from_file("/res/icons/16x16/debug-single-step.png"), [&](auto&) {
        pthread_mutex_lock(Debugger::the().continue_mutex());
        Debugger::the().set_continue_type(Debugger::ContinueType::SourceSingleStep);
        pthread_cond_signal(Debugger::the().continue_cond());
        pthread_mutex_unlock(Debugger::the().continue_mutex());
    });
    continue_action->set_enabled(false);
    single_step_action->set_enabled(false);

    toolbar.add_action(run_action);
    toolbar.add_action(stop_action);

    toolbar.add_separator();
    toolbar.add_action(debug_action);
    toolbar.add_action(continue_action);
    toolbar.add_action(single_step_action);

    RefPtr<EditorWrapper> current_editor_in_execution;
    Debugger::initialize(
        [&](const PtraceRegisters& regs) {
            dbg() << "Program stopped";

            ASSERT(Debugger::the().session());
            const auto& debug_session = *Debugger::the().session();
            auto source_position = debug_session.debug_info().get_source_position(regs.eip);
            if (!source_position.has_value()) {
                dbg() << "Could not find source position for address: " << (void*)regs.eip;
                return Debugger::HasControlPassedToUser::No;
            }
            current_editor_in_execution = get_editor_of_file(source_position.value().file_path);
            current_editor_in_execution->editor().set_execution_position(source_position.value().line_number - 1);
            debug_info_widget.update_state(debug_session, regs);
            continue_action->set_enabled(true);
            single_step_action->set_enabled(true);
            reveal_action_tab(debug_info_widget);
            return Debugger::HasControlPassedToUser::Yes;
        },
        [&]() {
            dbg() << "Program continued";
            continue_action->set_enabled(false);
            single_step_action->set_enabled(false);
            if (current_editor_in_execution) {
                current_editor_in_execution->editor().clear_execution_position();
            }
        },
        [&]() {
            dbg() << "Program exited";
            debug_info_widget.program_stopped();
            hide_action_tabs();
            Core::EventLoop::main().post_event(*g_window, make<Core::DeferredInvocationEvent>([=](auto&) {
                GUI::MessageBox::show("Program Exited", "Debugger", GUI::MessageBox::Type::Information, GUI::MessageBox::InputType::OK, g_window);
            }));
            Core::EventLoop::wake();
        });

    auto& build_menu = menubar->add_menu("Build");
    build_menu.add_action(build_action);
    build_menu.add_separator();
    build_menu.add_action(run_action);
    build_menu.add_action(stop_action);
    build_menu.add_separator();
    build_menu.add_action(debug_action);

    auto& view_menu = menubar->add_menu("View");
    view_menu.add_action(hide_action_tabs_action);
    view_menu.add_action(open_locator_action);
    view_menu.add_separator();
    view_menu.add_action(add_editor_action);
    view_menu.add_action(remove_current_editor_action);
    view_menu.add_action(add_terminal_action);
    view_menu.add_action(remove_current_terminal_action);

    auto& help_menu = menubar->add_menu("Help");
    help_menu.add_action(GUI::Action::create("About", [&](auto&) {
        GUI::AboutDialog::show("HackStudio", Gfx::Bitmap::load_from_file("/res/icons/32x32/app-hack-studio.png"), g_window);
    }));

    app.set_menubar(move(menubar));

    g_window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/app-hack-studio.png"));

    g_window->show();

    update_actions = [&]() {
        auto is_remove_terminal_enabled = []() {
            auto widget = s_action_tab_widget->active_widget();
            if (!widget)
                return false;
            if (strcmp(widget->class_name(), "TerminalWrapper") != 0)
                return false;
            if (!reinterpret_cast<TerminalWrapper*>(widget)->user_spawned())
                return false;
            return true;
        };

        remove_current_editor_action->set_enabled(g_all_editor_wrappers.size() > 1);
        remove_current_terminal_action->set_enabled(is_remove_terminal_enabled());
    };

    g_open_file = open_file;

    open_file(g_project->default_file());

    update_actions();
    return app.exec();
}

void build(TerminalWrapper& wrapper)
{
    if (g_project->type() == ProjectType::JavaScript && g_currently_open_file.ends_with(".js"))
        wrapper.run_command(String::format("js -A %s", g_currently_open_file.characters()));
    else
        wrapper.run_command("make");
}

void run(TerminalWrapper& wrapper)
{
    if (g_project->type() == ProjectType::JavaScript && g_currently_open_file.ends_with(".js"))
        wrapper.run_command(String::format("js %s", g_currently_open_file.characters()));
    else
        wrapper.run_command("make run");
}

void open_project(String filename)
{
    LexicalPath lexical_path(filename);
    if (chdir(lexical_path.dirname().characters()) < 0) {
        perror("chdir");
        exit(1);
    }
    g_project = Project::load_from_file(filename);
    ASSERT(g_project);
    if (g_project_tree_view) {
        g_project_tree_view->set_model(g_project->model());
        g_project_tree_view->toggle_index(g_project_tree_view->model()->index(0, 0));
        g_project_tree_view->update();
    }
    if (Debugger::is_initialized()) {
        Debugger::the().reset_breakpoints();
    }
}

void open_file(const String& filename)
{
    auto project_file = g_project->get_file(filename);
    if (project_file) {
        current_editor().set_document(const_cast<GUI::TextDocument&>(project_file->document()));
        current_editor().set_readonly(false);
    } else {
        auto external_file = ProjectFile::construct_with_name(filename);
        current_editor().set_document(const_cast<GUI::TextDocument&>(external_file->document()));
        current_editor().set_readonly(true);
    }

    if (filename.ends_with(".cpp") || filename.ends_with(".h"))
        current_editor().set_syntax_highlighter(make<GUI::CppSyntaxHighlighter>());
    else if (filename.ends_with(".js"))
        current_editor().set_syntax_highlighter(make<GUI::JSSyntaxHighlighter>());
    else if (filename.ends_with(".ini"))
        current_editor().set_syntax_highlighter(make<GUI::IniSyntaxHighlighter>());
    else
        current_editor().set_syntax_highlighter(nullptr);

    if (filename.ends_with(".frm")) {
        set_edit_mode(EditMode::Form);
    } else {
        set_edit_mode(EditMode::Text);
    }

    g_currently_open_file = filename;
    g_window->set_title(String::format("%s - HackStudio", g_currently_open_file.characters()));
    g_project_tree_view->update();

    current_editor_wrapper().filename_label().set_text(filename);

    current_editor().set_focus(true);
}

bool make_is_available()
{
    pid_t pid;
    const char* argv[] = { "make", "--version", nullptr };
    if ((errno = posix_spawnp(&pid, "make", nullptr, nullptr, const_cast<char**>(argv), environ))) {
        perror("posix_spawn");
        return false;
    }

    int wstatus;
    waitpid(pid, &wstatus, 0);
    return WEXITSTATUS(wstatus) == 0;
}
