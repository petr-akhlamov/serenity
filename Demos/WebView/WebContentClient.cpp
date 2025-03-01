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

#include "WebContentClient.h"
#include "WebContentView.h"
#include <AK/SharedBuffer.h>

WebContentClient::WebContentClient(WebContentView& view)
    : IPC::ServerConnection<WebContentClientEndpoint, WebContentServerEndpoint>(*this, "/tmp/portal/webcontent")
    , m_view(view)
{
    handshake();
}

void WebContentClient::handshake()
{
    auto response = send_sync<Messages::WebContentServer::Greet>(getpid());
    set_my_client_id(response->client_id());
    set_server_pid(response->server_pid());
}

void WebContentClient::handle(const Messages::WebContentClient::DidPaint& message)
{
    dbg() << "handle: WebContentClient::DidPaint! content_rect=" << message.content_rect() << ", shbuf_id=" << message.shbuf_id();
    m_view.notify_server_did_paint({}, message.shbuf_id());
}

void WebContentClient::handle(const Messages::WebContentClient::DidFinishLoad& message)
{
    dbg() << "handle: WebContentClient::DidFinishLoad! url=" << message.url();
}

void WebContentClient::handle(const Messages::WebContentClient::DidInvalidateContentRect& message)
{
    dbg() << "handle: WebContentClient::DidInvalidateContentRect! content_rect=" << message.content_rect();

    // FIXME: Figure out a way to coalesce these messages to reduce unnecessary painting
    m_view.notify_server_did_invalidate_content_rect({}, message.content_rect());
}
