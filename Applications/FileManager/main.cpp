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

#include "DirectoryView.h"
#include "FileUtils.h"
#include "PropertiesDialog.h"
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/URL.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/MimeData.h>
#include <LibCore/StandardPaths.h>
#include <LibDesktop/Launcher.h>
#include <LibGUI/AboutDialog.h>
#include <LibGUI/Action.h>
#include <LibGUI/ActionGroup.h>
#include <LibGUI/Application.h>
#include <LibGUI/BoxLayout.h>
#include <LibGUI/Clipboard.h>
#include <LibGUI/FileSystemModel.h>
#include <LibGUI/InputBox.h>
#include <LibGUI/Label.h>
#include <LibGUI/Menu.h>
#include <LibGUI/MenuBar.h>
#include <LibGUI/MessageBox.h>
#include <LibGUI/Painter.h>
#include <LibGUI/ProgressBar.h>
#include <LibGUI/Splitter.h>
#include <LibGUI/StatusBar.h>
#include <LibGUI/TextEditor.h>
#include <LibGUI/ToolBar.h>
#include <LibGUI/ToolBarContainer.h>
#include <LibGUI/TreeView.h>
#include <LibGUI/Widget.h>
#include <LibGUI/Window.h>
#include <LibGfx/Palette.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int run_in_desktop_mode(RefPtr<Core::ConfigFile>, String initial_location);
static int run_in_windowed_mode(RefPtr<Core::ConfigFile>, String initial_location);

int main(int argc, char** argv)
{
    if (pledge("stdio thread shared_buffer accept unix cpath rpath wpath fattr proc exec sigaction", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_flags = SA_NOCLDWAIT;
    act.sa_handler = SIG_IGN;
    int rc = sigaction(SIGCHLD, &act, nullptr);
    if (rc < 0) {
        perror("sigaction");
        return 1;
    }

    RefPtr<Core::ConfigFile> config = Core::ConfigFile::get_for_app("FileManager");

    GUI::Application app(argc, argv);

    if (pledge("stdio thread shared_buffer accept cpath rpath wpath fattr proc exec unix", nullptr) < 0) {
        perror("pledge");
        return 1;
    }

    if (app.args().contains_slow("--desktop") || app.args().contains_slow("-d"))
        return run_in_desktop_mode(move(config), Core::StandardPaths::desktop_directory());

    // our initial location is defined as, in order of precedence:
    // 1. the first command-line argument (e.g. FileManager /bin)
    // 2. the user's home directory
    // 3. the root directory
    String initial_location;

    if (argc >= 2) {
        char* buffer = realpath(argv[1], nullptr);
        initial_location = buffer;
        free(buffer);
    }

    if (initial_location.is_empty())
        initial_location = Core::StandardPaths::home_directory();

    if (initial_location.is_empty())
        initial_location = "/";

    return run_in_windowed_mode(move(config), initial_location);
}

class DesktopWidget final : public GUI::Widget {
    C_OBJECT(DesktopWidget);

private:
    virtual void paint_event(GUI::PaintEvent& event) override
    {
        GUI::Painter painter(*this);
        painter.add_clip_rect(event.rect());
        painter.clear_rect(event.rect(), Color(0, 0, 0, 0));
    }

    DesktopWidget()
    {
    }
};

int run_in_desktop_mode(RefPtr<Core::ConfigFile> config, String initial_location)
{
    (void)config;
    (void)initial_location;
    auto window = GUI::Window::construct();
    window->set_title("Desktop Manager");
    window->set_window_type(GUI::WindowType::Desktop);
    window->set_has_alpha_channel(true);

    auto& desktop_widget = window->set_main_widget<DesktopWidget>();
    desktop_widget.set_layout<GUI::VerticalBoxLayout>();

    auto& icon_view = desktop_widget.add<GUI::IconView>();
    icon_view.set_frame_thickness(0);
    icon_view.set_scrollbars_enabled(false);
    icon_view.set_fill_with_background_color(false);

    auto model = GUI::FileSystemModel::create(initial_location);
    icon_view.set_model(model);
    icon_view.set_model_column(GUI::FileSystemModel::Column::Name);

    icon_view.on_activation = [&](auto& index) {
        if (!index.is_valid())
            return;
        auto& node = model->node(index);
        auto path = node.full_path(model);
        Desktop::Launcher::open(URL::create_with_file_protocol(path));
    };

    auto desktop_view_context_menu = GUI::Menu::construct("Directory View");

    auto mkdir_action = GUI::Action::create("New directory...", {}, Gfx::Bitmap::load_from_file("/res/icons/16x16/mkdir.png"), [&](const GUI::Action&) {
        auto input_box = GUI::InputBox::construct("Enter name:", "New directory", window);
        if (input_box->exec() == GUI::InputBox::ExecOK && !input_box->text_value().is_empty()) {
            auto new_dir_path = LexicalPath::canonicalized_path(
                String::format("%s/%s",
                    model->root_path().characters(),
                    input_box->text_value().characters()));
            int rc = mkdir(new_dir_path.characters(), 0777);
            if (rc < 0) {
                GUI::MessageBox::show(String::format("mkdir(\"%s\") failed: %s", new_dir_path.characters(), strerror(errno)), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
            }
        }
    });

    auto touch_action = GUI::Action::create("New file...", {}, Gfx::Bitmap::load_from_file("/res/icons/16x16/new.png"), [&](const GUI::Action&) {
        auto input_box = GUI::InputBox::construct("Enter name:", "New file", window);
        if (input_box->exec() == GUI::InputBox::ExecOK && !input_box->text_value().is_empty()) {
            auto new_file_path = LexicalPath::canonicalized_path(
                String::format("%s/%s",
                    model->root_path().characters(),
                    input_box->text_value().characters()));
            struct stat st;
            int rc = stat(new_file_path.characters(), &st);
            if ((rc < 0 && errno != ENOENT)) {
                GUI::MessageBox::show(String::format("stat(\"%s\") failed: %s", new_file_path.characters(), strerror(errno)), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
                return;
            }
            if (rc == 0) {
                GUI::MessageBox::show(String::format("%s: Already exists", new_file_path.characters()), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
                return;
            }
            int fd = creat(new_file_path.characters(), 0666);
            if (fd < 0) {
                GUI::MessageBox::show(String::format("creat(\"%s\") failed: %s", new_file_path.characters(), strerror(errno)), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
                return;
            }
            rc = close(fd);
            assert(rc >= 0);
        }
    });

    auto file_manager_action = GUI::Action::create("Show in FileManager...", {}, Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-folder.png"), [&](const GUI::Action&) {
        Desktop::Launcher::open(URL::create_with_file_protocol(model->root_path()));
    });

    auto display_properties_action = GUI::Action::create("Display settings...", {}, Gfx::Bitmap::load_from_file("/res/icons/16x16/app-display-settings.png"), [&](const GUI::Action&) {
        Desktop::Launcher::open(URL::create_with_file_protocol("/bin/DisplaySettings"));
    });

    desktop_view_context_menu->add_action(mkdir_action);
    desktop_view_context_menu->add_action(touch_action);
    desktop_view_context_menu->add_separator();
    desktop_view_context_menu->add_action(file_manager_action);
    desktop_view_context_menu->add_separator();
    desktop_view_context_menu->add_action(display_properties_action);

    icon_view.on_context_menu_request = [&](const GUI::ModelIndex& index, const GUI::ContextMenuEvent& event) {
        if (!index.is_valid())
            desktop_view_context_menu->popup(event.screen_position());
    };

    window->show();
    return GUI::Application::the().exec();
}

int run_in_windowed_mode(RefPtr<Core::ConfigFile> config, String initial_location)
{
    auto window = GUI::Window::construct();
    window->set_title("File Manager");

    auto left = config->read_num_entry("Window", "Left", 150);
    auto top = config->read_num_entry("Window", "Top", 75);
    auto width = config->read_num_entry("Window", "Width", 640);
    auto heigth = config->read_num_entry("Window", "Heigth", 480);
    window->set_rect({ left, top, width, heigth });

    auto& widget = window->set_main_widget<GUI::Widget>();
    widget.set_layout<GUI::VerticalBoxLayout>();
    widget.set_fill_with_background_color(true);
    widget.layout()->set_spacing(2);

    auto& toolbar_container = widget.add<GUI::ToolBarContainer>();

    auto& main_toolbar = toolbar_container.add<GUI::ToolBar>();
    auto& location_toolbar = toolbar_container.add<GUI::ToolBar>();
    location_toolbar.layout()->set_margins({ 6, 3, 6, 3 });

    auto& location_label = location_toolbar.add<GUI::Label>("Location: ");
    location_label.size_to_fit();

    auto& location_textbox = location_toolbar.add<GUI::TextBox>();
    location_textbox.set_size_policy(GUI::SizePolicy::Fill, GUI::SizePolicy::Fixed);
    location_textbox.set_preferred_size(0, 22);
    location_textbox.set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-folder.png"));

    auto& splitter = widget.add<GUI::HorizontalSplitter>();
    auto& tree_view = splitter.add<GUI::TreeView>();
    auto directories_model = GUI::FileSystemModel::create("/", GUI::FileSystemModel::Mode::DirectoriesOnly);
    tree_view.set_model(directories_model);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Icon, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Size, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Owner, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Group, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Permissions, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::ModificationTime, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::Inode, true);
    tree_view.set_column_hidden(GUI::FileSystemModel::Column::SymlinkTarget, true);
    tree_view.set_size_policy(GUI::SizePolicy::Fixed, GUI::SizePolicy::Fill);
    tree_view.set_preferred_size(150, 0);
    auto& directory_view = splitter.add<DirectoryView>();

    auto& statusbar = widget.add<GUI::StatusBar>();

    auto& progressbar = statusbar.add<GUI::ProgressBar>();
    progressbar.set_caption("Generating thumbnails: ");
    progressbar.set_format(GUI::ProgressBar::Format::ValueSlashMax);
    progressbar.set_visible(false);
    progressbar.set_frame_shape(Gfx::FrameShape::Panel);
    progressbar.set_frame_shadow(Gfx::FrameShadow::Sunken);
    progressbar.set_frame_thickness(1);

    location_textbox.on_return_pressed = [&] {
        directory_view.open(location_textbox.text());
    };

    auto refresh_tree_view = [&] {
        directories_model->update();

        auto current_path = directory_view.path();

        struct stat st;
        // If the directory no longer exists, we find a parent that does.
        while (stat(current_path.characters(), &st) != 0) {
            directory_view.open_parent_directory();
            current_path = directory_view.path();
            if (current_path == directories_model->root_path()) {
                break;
            }
        }

        // Reselect the existing folder in the tree.
        auto new_index = directories_model->index(current_path, GUI::FileSystemModel::Column::Name);
        tree_view.selection().set(new_index);
        tree_view.scroll_into_view(new_index, Orientation::Vertical);
        tree_view.update();

        directory_view.refresh();
    };

    auto directory_context_menu = GUI::Menu::construct("Directory View Directory");
    auto file_context_menu = GUI::Menu::construct("Directory View File");
    auto directory_view_context_menu = GUI::Menu::construct("Directory View");
    auto tree_view_directory_context_menu = GUI::Menu::construct("Tree View Directory");
    auto tree_view_context_menu = GUI::Menu::construct("Tree View");

    auto open_parent_directory_action = GUI::Action::create("Open parent directory", { Mod_Alt, Key_Up }, Gfx::Bitmap::load_from_file("/res/icons/16x16/open-parent-directory.png"), [&](const GUI::Action&) {
        directory_view.open_parent_directory();
    });

    auto mkdir_action = GUI::Action::create("New directory...", { Mod_Ctrl | Mod_Shift, Key_N }, Gfx::Bitmap::load_from_file("/res/icons/16x16/mkdir.png"), [&](const GUI::Action&) {
        auto input_box = GUI::InputBox::construct("Enter name:", "New directory", window);
        if (input_box->exec() == GUI::InputBox::ExecOK && !input_box->text_value().is_empty()) {
            auto new_dir_path = LexicalPath::canonicalized_path(
                String::format("%s/%s",
                    directory_view.path().characters(),
                    input_box->text_value().characters()));
            int rc = mkdir(new_dir_path.characters(), 0777);
            if (rc < 0) {
                GUI::MessageBox::show(String::format("mkdir(\"%s\") failed: %s", new_dir_path.characters(), strerror(errno)), "Error", GUI::MessageBox::Type::Error, GUI::MessageBox::InputType::OK, window);
            } else {
                refresh_tree_view();
            }
        }
    });

    auto open_terminal_action = GUI::Action::create("Open Terminal here...", Gfx::Bitmap::load_from_file("/res/icons/16x16/app-terminal.png"), [&](const GUI::Action&) {
        posix_spawn_file_actions_t spawn_actions;
        posix_spawn_file_actions_init(&spawn_actions);
        posix_spawn_file_actions_addchdir(&spawn_actions, directory_view.path().characters());
        pid_t pid;
        const char* argv[] = { "Terminal", nullptr };
        posix_spawn(&pid, "/bin/Terminal", &spawn_actions, nullptr, const_cast<char**>(argv), environ);
        posix_spawn_file_actions_destroy(&spawn_actions);
    });

    RefPtr<GUI::Action> view_as_table_action;
    RefPtr<GUI::Action> view_as_icons_action;
    RefPtr<GUI::Action> view_as_columns_action;

    view_as_table_action = GUI::Action::create_checkable(
        "Table view", { Mod_Ctrl, KeyCode::Key_L }, Gfx::Bitmap::load_from_file("/res/icons/16x16/table-view.png"), [&](const GUI::Action&) {
            directory_view.set_view_mode(DirectoryView::ViewMode::Table);
            config->write_entry("DirectoryView", "ViewMode", "Table");
            config->sync();
        },
        window);

    view_as_icons_action = GUI::Action::create_checkable(
        "Icon view", { Mod_Ctrl, KeyCode::Key_I }, Gfx::Bitmap::load_from_file("/res/icons/16x16/icon-view.png"), [&](const GUI::Action&) {
            directory_view.set_view_mode(DirectoryView::ViewMode::Icon);
            config->write_entry("DirectoryView", "ViewMode", "Icon");
            config->sync();
        },
        window);

    view_as_columns_action = GUI::Action::create_checkable(
        "Columns view", Gfx::Bitmap::load_from_file("/res/icons/16x16/columns-view.png"), [&](const GUI::Action&) {
            directory_view.set_view_mode(DirectoryView::ViewMode::Columns);
            config->write_entry("DirectoryView", "ViewMode", "Columns");
            config->sync();
        },
        window);

    auto view_type_action_group = make<GUI::ActionGroup>();
    view_type_action_group->set_exclusive(true);
    view_type_action_group->add_action(*view_as_table_action);
    view_type_action_group->add_action(*view_as_icons_action);
    view_type_action_group->add_action(*view_as_columns_action);

    auto selected_file_paths = [&] {
        Vector<String> paths;
        auto& view = directory_view.current_view();
        auto& model = *view.model();
        view.selection().for_each_index([&](const GUI::ModelIndex& index) {
            auto parent_index = model.parent_index(index);
            auto name_index = model.index(index.row(), GUI::FileSystemModel::Column::Name, parent_index);
            auto path = model.data(name_index, GUI::Model::Role::Custom).to_string();
            paths.append(path);
        });
        return paths;
    };

    auto tree_view_selected_file_paths = [&] {
        Vector<String> paths;
        auto& view = tree_view;
        view.selection().for_each_index([&](const GUI::ModelIndex& index) {
            paths.append(directories_model->full_path(index));
        });
        return paths;
    };

    auto select_all_action = GUI::Action::create("Select all", { Mod_Ctrl, KeyCode::Key_A }, [&](const GUI::Action&) {
        directory_view.current_view().select_all();
    });

    auto copy_action = GUI::CommonActions::make_copy_action(
        [&](const GUI::Action&) {
            Vector<String> paths = selected_file_paths();

            if (!paths.size())
                paths = tree_view_selected_file_paths();

            if (paths.is_empty())
                ASSERT_NOT_REACHED();

            StringBuilder copy_text;
            for (auto& path : paths) {
                auto url = URL::create_with_file_protocol(path);
                copy_text.appendf("%s\n", url.to_string().characters());
            }
            GUI::Clipboard::the().set_data(copy_text.build(), "text/uri-list");
        },
        window);
    copy_action->set_enabled(false);

    auto properties_action
        = GUI::Action::create(
            "Properties...", { Mod_Alt, Key_Return }, Gfx::Bitmap::load_from_file("/res/icons/16x16/properties.png"), [&](const GUI::Action& action) {
                auto& model = directory_view.model();
                String container_dir_path;
                String path;
                Vector<String> selected;
                if (action.activator() == directory_context_menu || directory_view.active_widget()->is_focused()) {
                    path = directory_view.path();
                    container_dir_path = path;
                    selected = selected_file_paths();
                } else {
                    path = directories_model->full_path(tree_view.selection().first());
                    container_dir_path = LexicalPath(path).basename();
                    selected = tree_view_selected_file_paths();
                }

                RefPtr<PropertiesDialog> properties;
                if (selected.is_empty()) {
                    properties = window->add<PropertiesDialog>(model, path, true);
                } else {
                    properties = window->add<PropertiesDialog>(model, selected.first(), access(container_dir_path.characters(), W_OK) != 0);
                }

                properties->exec();
            },
            window);

    enum class ConfirmBeforeDelete {
        No,
        Yes
    };

    auto do_paste = [&](const GUI::Action& action) {
        auto data_and_type = GUI::Clipboard::the().data_and_type();
        if (data_and_type.type != "text/uri-list") {
            dbg() << "Cannot paste clipboard type " << data_and_type.type;
            return;
        }
        auto copied_lines = data_and_type.data.split('\n');
        if (copied_lines.is_empty()) {
            dbg() << "No files to paste";
            return;
        }

        AK::String target_directory;
        if (action.activator() == directory_context_menu)
            target_directory = selected_file_paths()[0];
        else
            target_directory = directory_view.path();

        for (auto& uri_as_string : copied_lines) {
            if (uri_as_string.is_empty())
                continue;
            URL url = uri_as_string;
            if (!url.is_valid() || url.protocol() != "file") {
                dbg() << "Cannot paste URI " << uri_as_string;
                continue;
            }

            auto new_path = String::format("%s/%s", target_directory.characters(), url.basename().characters());
            if (!FileUtils::copy_file_or_directory(url.path(), new_path)) {
                auto error_message = String::format("Could not paste %s.", url.path().characters());
                GUI::MessageBox::show(error_message, "File Manager", GUI::MessageBox::Type::Error);
            } else {
                refresh_tree_view();
            }
        }
    };

    auto do_delete = [&](ConfirmBeforeDelete confirm, const GUI::Action&) {
        Vector<String> paths = selected_file_paths();

        if (!paths.size())
            paths = tree_view_selected_file_paths();

        if (paths.is_empty())
            ASSERT_NOT_REACHED();

        String message;
        if (paths.size() == 1) {
            message = String::format("Really delete %s?", LexicalPath(paths[0]).basename().characters());
        } else {
            message = String::format("Really delete %d files?", paths.size());
        }

        if (confirm == ConfirmBeforeDelete::Yes) {
            auto result = GUI::MessageBox::show(
                message,
                "Confirm deletion",
                GUI::MessageBox::Type::Warning,
                GUI::MessageBox::InputType::OKCancel,
                window);
            if (result == GUI::MessageBox::ExecCancel)
                return;
        }

        for (auto& path : paths) {
            struct stat st;
            if (lstat(path.characters(), &st)) {
                GUI::MessageBox::show(
                    String::format("lstat(%s) failed: %s", path.characters(), strerror(errno)),
                    "Delete failed",
                    GUI::MessageBox::Type::Error,
                    GUI::MessageBox::InputType::OK,
                    window);
                break;
            } else {
                refresh_tree_view();
            }

            if (S_ISDIR(st.st_mode)) {
                String error_path;
                int error = FileUtils::delete_directory(path, error_path);

                if (error) {
                    GUI::MessageBox::show(
                        String::format("Failed to delete directory \"%s\": %s", error_path.characters(), strerror(error)),
                        "Delete failed",
                        GUI::MessageBox::Type::Error,
                        GUI::MessageBox::InputType::OK,
                        window);
                    break;
                } else {
                    refresh_tree_view();
                }
            } else if (unlink(path.characters()) < 0) {
                int saved_errno = errno;
                GUI::MessageBox::show(
                    String::format("unlink(%s) failed: %s", path.characters(), strerror(saved_errno)),
                    "Delete failed",
                    GUI::MessageBox::Type::Error,
                    GUI::MessageBox::InputType::OK,
                    window);
                break;
            }
        }
    };

    auto paste_action = GUI::CommonActions::make_paste_action(
        [&](const GUI::Action& action) {
            do_paste(action);
        },
        window);

    auto folder_specific_paste_action = GUI::CommonActions::make_paste_action(
        [&](const GUI::Action& action) {
            do_paste(action);
        },
        window);

    auto force_delete_action = GUI::Action::create(
        "Delete without confirmation", { Mod_Shift, Key_Delete }, [&](const GUI::Action& action) {
            do_delete(ConfirmBeforeDelete::No, action);
        },
        window);

    auto delete_action = GUI::CommonActions::make_delete_action(
        [&](const GUI::Action& action) {
            do_delete(ConfirmBeforeDelete::Yes, action);
        },
        window);
    delete_action->set_enabled(false);

    auto go_back_action = GUI::CommonActions::make_go_back_action(
        [&](auto&) {
            directory_view.open_previous_directory();
        },
        window);

    auto go_forward_action = GUI::CommonActions::make_go_forward_action(
        [&](auto&) {
            directory_view.open_next_directory();
        },
        window);

    auto go_home_action = GUI::CommonActions::make_go_home_action(
        [&](auto&) {
            directory_view.open(Core::StandardPaths::home_directory());
        },
        window);

    GUI::Clipboard::the().on_change = [&](const String& data_type) {
        auto current_location = directory_view.path();
        paste_action->set_enabled(data_type == "text/uri-list" && access(current_location.characters(), W_OK) == 0);
    };

    auto menubar = GUI::MenuBar::construct();

    auto& app_menu = menubar->add_menu("File Manager");
    app_menu.add_action(mkdir_action);
    app_menu.add_action(copy_action);
    app_menu.add_action(paste_action);
    app_menu.add_action(delete_action);
    app_menu.add_action(open_terminal_action);
    app_menu.add_separator();
    app_menu.add_action(properties_action);
    app_menu.add_separator();
    app_menu.add_action(GUI::CommonActions::make_quit_action([](auto&) {
        GUI::Application::the().quit(0);
    }));

    auto& view_menu = menubar->add_menu("View");
    view_menu.add_action(*view_as_icons_action);
    view_menu.add_action(*view_as_table_action);
    view_menu.add_action(*view_as_columns_action);

    auto& go_menu = menubar->add_menu("Go");
    go_menu.add_action(go_back_action);
    go_menu.add_action(go_forward_action);
    go_menu.add_action(open_parent_directory_action);
    go_menu.add_action(go_home_action);

    auto& help_menu = menubar->add_menu("Help");
    help_menu.add_action(GUI::Action::create("About", [&](auto&) {
        GUI::AboutDialog::show("File Manager", Gfx::Bitmap::load_from_file("/res/icons/32x32/filetype-folder.png"), window);
    }));

    GUI::Application::the().set_menubar(move(menubar));

    main_toolbar.add_action(go_back_action);
    main_toolbar.add_action(go_forward_action);
    main_toolbar.add_action(open_parent_directory_action);
    main_toolbar.add_action(go_home_action);

    main_toolbar.add_separator();
    main_toolbar.add_action(mkdir_action);
    main_toolbar.add_action(copy_action);
    main_toolbar.add_action(paste_action);
    main_toolbar.add_action(delete_action);
    main_toolbar.add_action(open_terminal_action);

    main_toolbar.add_separator();
    main_toolbar.add_action(*view_as_icons_action);
    main_toolbar.add_action(*view_as_table_action);
    main_toolbar.add_action(*view_as_columns_action);

    directory_view.on_path_change = [&](const String& new_path) {
        window->set_title(String::format("%s - File Manager", new_path.characters()));
        location_textbox.set_text(new_path);
        auto new_index = directories_model->index(new_path, GUI::FileSystemModel::Column::Name);
        if (new_index.is_valid()) {
            tree_view.selection().set(new_index);
            tree_view.scroll_into_view(new_index, Orientation::Vertical);
            tree_view.update();
        }

        struct stat st;
        if (lstat(new_path.characters(), &st)) {
            perror("stat");
            return;
        }

        auto can_write_in_path = access(new_path.characters(), W_OK) == 0;
        mkdir_action->set_enabled(can_write_in_path);
        paste_action->set_enabled(can_write_in_path && GUI::Clipboard::the().type() == "text/uri-list");
        go_forward_action->set_enabled(directory_view.path_history_position() < directory_view.path_history_size() - 1);
        go_back_action->set_enabled(directory_view.path_history_position() > 0);
        open_parent_directory_action->set_enabled(new_path != "/");
    };

    directory_view.on_error = [&](int, const char* error_string, bool quit) {
        auto error_message = String::format("Could not read directory: %s", error_string);
        GUI::MessageBox::show(error_message, "File Manager", GUI::MessageBox::Type::Error);

        if (quit)
            exit(1);
    };

    directory_view.on_status_message = [&](const StringView& message) {
        statusbar.set_text(message);
    };

    directory_view.on_thumbnail_progress = [&](int done, int total) {
        if (done == total) {
            progressbar.set_visible(false);
            return;
        }
        progressbar.set_range(0, total);
        progressbar.set_value(done);
        progressbar.set_visible(true);
    };

    directory_view.on_selection_change = [&](GUI::AbstractView& view) {
        // FIXME: Figure out how we can enable/disable the paste action, based on clipboard contents.
        auto selection = view.selection();

        delete_action->set_enabled(!selection.is_empty() && access(directory_view.path().characters(), W_OK) == 0);
        copy_action->set_enabled(!selection.is_empty());
    };

    auto open_in_text_editor_action = GUI::Action::create("Open in TextEditor...", Gfx::Bitmap::load_from_file("/res/icons/TextEditor16.png"), [&](auto&) {
        pid_t child;
        for (auto& path : selected_file_paths()) {
            const char* argv[] = { "TextEditor", path.characters(), nullptr };
            posix_spawn(&child, "/bin/TextEditor", nullptr, nullptr, const_cast<char**>(argv), environ);
        }
    });

    directory_context_menu->add_action(copy_action);
    directory_context_menu->add_action(folder_specific_paste_action);
    directory_context_menu->add_action(delete_action);
    directory_context_menu->add_separator();
    directory_context_menu->add_action(properties_action);

    file_context_menu->add_action(copy_action);
    file_context_menu->add_action(paste_action);
    file_context_menu->add_action(delete_action);
    file_context_menu->add_separator();
    file_context_menu->add_action(open_in_text_editor_action);
    file_context_menu->add_separator();
    file_context_menu->add_action(properties_action);

    directory_view_context_menu->add_action(mkdir_action);
    directory_view_context_menu->add_action(paste_action);
    directory_view_context_menu->add_action(open_terminal_action);
    directory_view_context_menu->add_separator();
    directory_view_context_menu->add_action(properties_action);

    tree_view_directory_context_menu->add_action(copy_action);
    tree_view_directory_context_menu->add_action(paste_action);
    tree_view_directory_context_menu->add_action(delete_action);
    tree_view_directory_context_menu->add_separator();
    tree_view_directory_context_menu->add_action(properties_action);
    tree_view_directory_context_menu->add_separator();
    tree_view_directory_context_menu->add_action(mkdir_action);

    directory_view.on_context_menu_request = [&](const GUI::AbstractView&, const GUI::ModelIndex& index, const GUI::ContextMenuEvent& event) {
        if (index.is_valid()) {
            auto& node = directory_view.model().node(index);

            if (node.is_directory()) {
                auto should_get_enabled = access(node.full_path(directory_view.model()).characters(), W_OK) == 0 && GUI::Clipboard::the().type() == "text/uri-list";
                folder_specific_paste_action->set_enabled(should_get_enabled);
                directory_context_menu->popup(event.screen_position());
            } else {
                file_context_menu->popup(event.screen_position());
            }
        } else {
            directory_view_context_menu->popup(event.screen_position());
        }
    };

    directory_view.on_drop = [&](const GUI::AbstractView&, const GUI::ModelIndex& index, const GUI::DropEvent& event) {
        if (!event.mime_data().has_urls())
            return;
        auto urls = event.mime_data().urls();
        if (urls.is_empty()) {
            dbg() << "No files to drop";
            return;
        }

        auto& target_node = directory_view.model().node(index);
        if (!target_node.is_directory())
            return;

        for (auto& url_to_copy : urls) {
            if (!url_to_copy.is_valid() || url_to_copy.path() == target_node.full_path(directory_view.model()))
                continue;
            auto new_path = String::format("%s/%s",
                target_node.full_path(directory_view.model()).characters(),
                LexicalPath(url_to_copy.path()).basename().characters());

            if (url_to_copy.path() == new_path)
                continue;

            if (!FileUtils::copy_file_or_directory(url_to_copy.path(), new_path)) {
                auto error_message = String::format("Could not copy %s into %s.",
                    url_to_copy.to_string().characters(),
                    new_path.characters());
                GUI::MessageBox::show(error_message, "File Manager", GUI::MessageBox::Type::Error);
            } else {
                refresh_tree_view();
            }
        }
    };

    tree_view.on_selection_change = [&] {
        if (tree_view.selection().is_empty())
            return;
        auto path = directories_model->full_path(tree_view.selection().first());
        if (directory_view.path() == path)
            return;
        directory_view.open(path);
        copy_action->set_enabled(!tree_view.selection().is_empty());
        delete_action->set_enabled(!tree_view.selection().is_empty());
    };

    tree_view.on_context_menu_request = [&](const GUI::ModelIndex& index, const GUI::ContextMenuEvent& event) {
        if (index.is_valid()) {
            tree_view_directory_context_menu->popup(event.screen_position());
        }
    };

    directory_view.open(initial_location);
    directory_view.set_focus(true);

    paste_action->set_enabled(GUI::Clipboard::the().type() == "text/uri-list" && access(initial_location.characters(), W_OK) == 0);

    window->show();

    window->set_icon(Gfx::Bitmap::load_from_file("/res/icons/16x16/filetype-folder.png"));

    // Read direcory read mode from config.
    auto dir_view_mode = config->read_entry("DirectoryView", "ViewMode", "Icon");

    if (dir_view_mode.contains("Table")) {
        directory_view.set_view_mode(DirectoryView::ViewMode::Table);
        view_as_table_action->set_checked(true);
    } else if (dir_view_mode.contains("Columns")) {
        directory_view.set_view_mode(DirectoryView::ViewMode::Columns);
        view_as_columns_action->set_checked(true);
    } else {
        directory_view.set_view_mode(DirectoryView::ViewMode::Icon);
        view_as_icons_action->set_checked(true);
    }

    // Write window position to config file on close request.
    window->on_close_request = [&] {
        config->write_num_entry("Window", "Left", window->x());
        config->write_num_entry("Window", "Top", window->y());
        config->write_num_entry("Window", "Width", window->width());
        config->write_num_entry("Window", "Heigth", window->height());
        config->sync();

        return GUI::Window::CloseRequestDecision::Close;
    };

    return GUI::Application::the().exec();
}
