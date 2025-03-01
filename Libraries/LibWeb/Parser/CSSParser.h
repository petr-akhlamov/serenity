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

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibWeb/CSS/StyleSheet.h>

namespace Web {

namespace CSS {
class ParsingContext {
public:
    ParsingContext();
    explicit ParsingContext(const Document&);

    bool in_quirks_mode() const;

private:
    const Document* m_document { nullptr };
};
}

RefPtr<StyleSheet> parse_css(const CSS::ParsingContext&, const StringView&);
RefPtr<StyleDeclaration> parse_css_declaration(const CSS::ParsingContext&, const StringView&);
RefPtr<StyleValue> parse_css_value(const CSS::ParsingContext&, const StringView&);
Optional<Selector> parse_selector(const CSS::ParsingContext&, const StringView&);

RefPtr<LengthStyleValue> parse_line_width(const CSS::ParsingContext&, const StringView&);
RefPtr<ColorStyleValue> parse_color(const CSS::ParsingContext&, const StringView&);
RefPtr<StringStyleValue> parse_line_style(const CSS::ParsingContext&, const StringView&);

}
