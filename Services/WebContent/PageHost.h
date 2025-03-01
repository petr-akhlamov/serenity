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

#include <LibWeb/Page.h>

namespace WebContent {

class ClientConnection;

class PageHost final : public Web::PageClient {
    AK_MAKE_NONCOPYABLE(PageHost);
    AK_MAKE_NONMOVABLE(PageHost);

public:
    static NonnullOwnPtr<PageHost> create(ClientConnection& client) { return adopt_own(*new PageHost(client)); }
    virtual ~PageHost();

    Web::Page& page() { return *m_page; }
    const Web::Page& page() const { return *m_page; }

    void paint(const Gfx::IntRect& content_rect, Gfx::Bitmap&);

    void set_palette_impl(const Gfx::PaletteImpl&);
    void set_viewport_rect(const Gfx::IntRect&);

private:
    // ^PageHost
    virtual Gfx::Palette palette() const override;
    virtual void page_did_invalidate(const Gfx::IntRect&) override;

    explicit PageHost(ClientConnection&);

    void setup_palette();

    ClientConnection& m_client;
    NonnullOwnPtr<Web::Page> m_page;
    RefPtr<Gfx::PaletteImpl> m_palette_impl;
};

}
