/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
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

#pragma once

#include "History.h"
#include <AK/URL.h>
#include <LibGUI/Widget.h>
#include <LibHTTP/HttpJob.h>
#include <LibWeb/Forward.h>

namespace Browser {

class Tab final : public GUI::Widget {
    C_OBJECT(Tab);

public:
    virtual ~Tab() override;

    void load(const URL&);

    void did_become_active();
    void context_menu_requested(const Gfx::IntPoint& screen_position);

    Function<void(String)> on_title_change;
    Function<void(const URL&)> on_tab_open_request;
    Function<void(Tab&)> on_tab_close_request;
    Function<void(const Gfx::Bitmap&)> on_favicon_change;

    const String& title() const { return m_title; }
    const Gfx::Bitmap* icon() const { return m_icon; }

private:
    Tab();

    void update_actions();
    void update_bookmark_button(const String& url);

    History<URL> m_history;
    RefPtr<Web::PageView> m_page_view;
    RefPtr<GUI::Action> m_go_back_action;
    RefPtr<GUI::Action> m_go_forward_action;
    RefPtr<GUI::Action> m_reload_action;
    RefPtr<GUI::TextBox> m_location_box;
    RefPtr<GUI::Button> m_bookmark_button;
    RefPtr<GUI::Window> m_dom_inspector_window;
    RefPtr<GUI::Window> m_console_window;
    RefPtr<GUI::StatusBar> m_statusbar;
    RefPtr<GUI::MenuBar> m_menubar;
    RefPtr<GUI::ToolBarContainer> m_toolbar_container;

    RefPtr<GUI::Menu> m_link_context_menu;
    String m_link_context_menu_href;

    RefPtr<GUI::Menu> m_tab_context_menu;
    RefPtr<GUI::Menu> m_page_context_menu;

    String m_title;
    RefPtr<const Gfx::Bitmap> m_icon;

    bool m_should_push_loads_to_history { true };
};

}
