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

#include <AK/ByteBuffer.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <ctype.h>

static String snake_name(const StringView& title_name)
{
    StringBuilder builder;
    bool first = true;
    bool last_was_uppercase = false;
    for (auto ch : title_name) {
        if (isupper(ch)) {
            if (!first && !last_was_uppercase)
                builder.append('_');
            builder.append(tolower(ch));
        } else {
            builder.append(ch);
        }
        first = false;
        last_was_uppercase = isupper(ch);
    }
    return builder.to_string();
}

namespace IDL {

struct Type {
    String name;
    bool nullable { false };
};

struct Parameter {
    Type type;
    String name;
};

struct Function {
    Type return_type;
    String name;
    Vector<Parameter> parameters;

    size_t length() const
    {
        // FIXME: Take optional arguments into account
        return parameters.size();
    }
};

struct Attribute {
    bool readonly { false };
    bool unsigned_ { false };
    Type type;
    String name;

    // Added for convenience after parsing
    String getter_callback_name;
    String setter_callback_name;
};

struct Interface {
    String name;
    String parent_name;

    Vector<Attribute> attributes;
    Vector<Function> functions;

    // Added for convenience after parsing
    String wrapper_class;
    String wrapper_base_class;
};

OwnPtr<Interface> parse_interface(const StringView& input)
{
    auto interface = make<Interface>();

    size_t index = 0;

    auto peek = [&](size_t offset = 0) -> char {
        if (index + offset > input.length())
            return 0;
        return input[index + offset];
    };

    auto consume = [&] {
        return input[index++];
    };

    auto consume_if = [&](auto ch) {
        if (peek() == ch) {
            consume();
            return true;
        }
        return false;
    };

    auto consume_specific = [&](char ch) {
        auto consumed = consume();
        if (consumed != ch) {
            dbg() << "Expected '" << ch << "' at offset " << index << " but got '" << consumed << "'";
            ASSERT_NOT_REACHED();
        }
    };

    auto consume_whitespace = [&] {
        while (isspace(peek()))
            consume();
    };

    auto consume_string = [&](const StringView& string) {
        for (size_t i = 0; i < string.length(); ++i) {
            ASSERT(consume() == string[i]);
        }
    };

    auto next_is = [&](const StringView& string) {
        for (size_t i = 0; i < string.length(); ++i) {
            if (peek(i) != string[i])
                return false;
        }
        return true;
    };

    auto consume_while = [&](auto condition) {
        StringBuilder builder;
        while (index < input.length() && condition(peek())) {
            builder.append(consume());
        }
        return builder.to_string();
    };

    consume_string("interface");
    consume_whitespace();
    interface->name = consume_while([](auto ch) { return !isspace(ch); });
    consume_whitespace();
    if (consume_if(':')) {
        consume_whitespace();
        interface->parent_name = consume_while([](auto ch) { return !isspace(ch); });
        consume_whitespace();
    }
    consume_specific('{');

    auto parse_type = [&] {
        auto name = consume_while([](auto ch) { return !isspace(ch) && ch != '?'; });
        auto nullable = peek() == '?';
        if (nullable)
            consume_specific('?');
        return Type { name, nullable };
    };

    auto parse_attribute = [&] {
        bool readonly = false;
        bool unsigned_ = false;
        if (next_is("readonly")) {
            consume_string("readonly");
            readonly = true;
            consume_whitespace();
        }
        if (next_is("attribute")) {
            consume_string("attribute");
            consume_whitespace();
        }
        if (next_is("unsigned")) {
            consume_string("unsigned");
            unsigned_ = true;
            consume_whitespace();
        }
        auto type = parse_type();
        consume_whitespace();
        auto name = consume_while([](auto ch) { return !isspace(ch) && ch != ';'; });
        consume_specific(';');
        Attribute attribute;
        attribute.readonly = readonly;
        attribute.unsigned_ = unsigned_;
        attribute.type = type;
        attribute.name = name;
        attribute.getter_callback_name = String::format("%s_getter", snake_name(attribute.name).characters());
        attribute.setter_callback_name = String::format("%s_setter", snake_name(attribute.name).characters());
        interface->attributes.append(move(attribute));
    };

    auto parse_function = [&] {
        auto return_type = parse_type();
        consume_whitespace();
        auto name = consume_while([](auto ch) { return !isspace(ch) && ch != '('; });
        consume_specific('(');

        Vector<Parameter> parameters;

        for (;;) {
            if (consume_if(')'))
                break;
            auto type = parse_type();
            consume_whitespace();
            auto name = consume_while([](auto ch) { return !isspace(ch) && ch != ',' && ch != ')'; });
            parameters.append({ move(type), move(name) });
            if (consume_if(')'))
                break;
            consume_specific(',');
            consume_whitespace();
        }

        consume_specific(';');

        interface->functions.append(Function { return_type, name, move(parameters) });
    };

    for (;;) {

        consume_whitespace();

        if (consume_if('}'))
            break;

        if (next_is("readonly") || next_is("attribute")) {
            parse_attribute();
            continue;
        }

        parse_function();
    }

    interface->wrapper_class = String::format("%sWrapper", interface->name.characters());
    interface->wrapper_base_class = String::format("%sWrapper", interface->parent_name.is_empty() ? "" : interface->parent_name.characters());

    return interface;
}

}

static void generate_header(const IDL::Interface&);
static void generate_implementation(const IDL::Interface&);

int main(int argc, char** argv)
{
    Core::ArgsParser args_parser;
    const char* path = nullptr;
    bool header_mode = false;
    bool implementation_mode = false;
    args_parser.add_option(header_mode, "Generate the wrapper .h file", "header", 'H');
    args_parser.add_option(implementation_mode, "Generate the wrapper .cpp file", "implementation", 'I');
    args_parser.add_positional_argument(path, "IDL file", "idl-file");
    args_parser.parse(argc, argv);

    auto file_or_error = Core::File::open(path, Core::IODevice::ReadOnly);
    if (file_or_error.is_error()) {
        fprintf(stderr, "Cannot open %s\n", path);
        return 1;
    }

    auto data = file_or_error.value()->read_all();
    auto interface = IDL::parse_interface(data);

    if (!interface) {
        fprintf(stderr, "Cannot parse %s\n", path);
        return 1;
    }

#if 0
    dbg() << "Attributes:";
    for (auto& attribute : interface->attributes) {
        dbg() << "  " << (attribute.readonly ? "Readonly " : "")
              << attribute.type.name << (attribute.type.nullable ? "?" : "")
              << " " << attribute.name;
    }

    dbg() << "Functions:";
    for (auto& function : interface->functions) {
        dbg() << "  " << function.return_type.name << (function.return_type.nullable ? "?" : "")
              << " " << function.name;
        for (auto& parameter : function.parameters) {
            dbg() << "    " << parameter.type.name << (parameter.type.nullable ? "?" : "") << " " << parameter.name;
        }
    }
#endif

    if (header_mode)
        generate_header(*interface);

    if (implementation_mode)
        generate_implementation(*interface);

    return 0;
}

static bool should_emit_wrapper_factory(const IDL::Interface& interface)
{
    // FIXME: This is very hackish.
    if (interface.name == "EventTarget")
        return false;
    if (interface.name == "Node")
        return false;
    if (interface.name == "Text")
        return false;
    if (interface.name == "Document")
        return false;
    if (interface.name == "DocumentType")
        return false;
    if (interface.name.ends_with("Element"))
        return false;
    if (interface.name.ends_with("Event"))
        return false;
    return true;
}

static bool is_wrappable_type(const IDL::Type& type)
{
    if (type.name == "Node")
        return true;
    if (type.name == "Document")
        return true;
    if (type.name == "Text")
        return true;
    if (type.name == "DocumentType")
        return true;
    if (type.name.ends_with("Element"))
        return true;
    if (type.name == "ImageData")
        return true;
    return false;
}

static void generate_header(const IDL::Interface& interface)
{
    auto& wrapper_class = interface.wrapper_class;
    auto& wrapper_base_class = interface.wrapper_base_class;

    out() << "#pragma once";
    out() << "#include <LibWeb/Bindings/Wrapper.h>";
    out() << "#include <LibWeb/DOM/" << interface.name << ".h>";

    if (wrapper_base_class != "Wrapper")
        out() << "#include <LibWeb/Bindings/" << wrapper_base_class << ".h>";

    out() << "namespace Web {";
    out() << "namespace Bindings {";

    out() << "class " << wrapper_class << " : public " << wrapper_base_class << " {";
    out() << "    JS_OBJECT(" << wrapper_class << ", " << wrapper_base_class << ");";
    out() << "public:";
    out() << "    " << wrapper_class << "(JS::GlobalObject&, " << interface.name << "&);";
    out() << "    virtual void initialize(JS::Interpreter&, JS::GlobalObject&) override;";
    out() << "    virtual ~" << wrapper_class << "() override;";

    if (wrapper_base_class == "Wrapper") {
        out() << "    " << interface.name << "& impl() { return *m_impl; }";
        out() << "    const " << interface.name << "& impl() const { return *m_impl; }";
    } else {
        out() << "    " << interface.name << "& impl() { return static_cast<" << interface.name << "&>(" << wrapper_base_class << "::impl()); }";
        out() << "    const " << interface.name << "& impl() const { return static_cast<const " << interface.name << "&>(" << wrapper_base_class << "::impl()); }";
    }

    auto is_foo_wrapper_name = snake_name(String::format("Is%s", wrapper_class.characters()));
    out() << "    virtual bool " << is_foo_wrapper_name << "() const final { return true; }";

    out() << "private:";

    for (auto& function : interface.functions) {
        out() << "    JS_DECLARE_NATIVE_FUNCTION(" << snake_name(function.name) << ");";
    }

    for (auto& attribute : interface.attributes) {
        out() << "    JS_DECLARE_NATIVE_GETTER(" << snake_name(attribute.name) << "_getter);";
        if (!attribute.readonly)
            out() << "    JS_DECLARE_NATIVE_SETTER(" << snake_name(attribute.name) << "_setter);";
    }

    if (wrapper_base_class == "Wrapper") {
        out() << "    NonnullRefPtr<" << interface.name << "> m_impl;";
    }

    out() << "};";

    if (should_emit_wrapper_factory(interface)) {
        out() << wrapper_class << "* wrap(JS::GlobalObject&, " << interface.name << "&);";
    }

    out() << "}";
    out() << "}";
}

void generate_implementation(const IDL::Interface& interface)
{
    auto& wrapper_class = interface.wrapper_class;
    auto& wrapper_base_class = interface.wrapper_base_class;

    out() << "#include <AK/FlyString.h>";
    out() << "#include <LibJS/Interpreter.h>";
    out() << "#include <LibJS/Runtime/Array.h>";
    out() << "#include <LibJS/Runtime/Value.h>";
    out() << "#include <LibJS/Runtime/GlobalObject.h>";
    out() << "#include <LibJS/Runtime/Error.h>";
    out() << "#include <LibJS/Runtime/Function.h>";
    out() << "#include <LibJS/Runtime/Uint8ClampedArray.h>";
    out() << "#include <LibWeb/Bindings/NodeWrapperFactory.h>";
    out() << "#include <LibWeb/Bindings/" << wrapper_class << ".h>";
    out() << "#include <LibWeb/DOM/Element.h>";
    out() << "#include <LibWeb/DOM/HTMLElement.h>";
    out() << "#include <LibWeb/DOM/EventListener.h>";
    out() << "#include <LibWeb/Bindings/HTMLCanvasElementWrapper.h>";
    out() << "#include <LibWeb/Bindings/HTMLImageElementWrapper.h>";
    out() << "#include <LibWeb/Bindings/ImageDataWrapper.h>";
    out() << "#include <LibWeb/Bindings/CanvasRenderingContext2DWrapper.h>";

    out() << "namespace Web {";
    out() << "namespace Bindings {";

    // Implementation: Wrapper constructor
    out() << wrapper_class << "::" << wrapper_class << "(JS::GlobalObject& global_object, " << interface.name << "& impl)";
    if (wrapper_base_class == "Wrapper") {
        out() << "    : Wrapper(*global_object.object_prototype())";
        out() << "    , m_impl(impl)";
    } else {
        out() << "    : " << wrapper_base_class << "(global_object, impl)";
    }
    out() << "{";
    out() << "}";

    // Implementation: Wrapper initialize()
    out() << "void " << wrapper_class << "::initialize(JS::Interpreter& interpreter, JS::GlobalObject& global_object)";
    out() << "{";
    out() << "    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable;";
    out() << "    " << wrapper_base_class << "::initialize(interpreter, global_object);";

    for (auto& attribute : interface.attributes) {
        out() << "    define_native_property(\"" << attribute.name << "\", " << attribute.getter_callback_name << ", " << (attribute.readonly ? "nullptr" : attribute.setter_callback_name) << ", default_attributes);";
    }

    for (auto& function : interface.functions) {
        out() << "    define_native_function(\"" << function.name << "\", " << snake_name(function.name) << ", " << function.length() << ", default_attributes);";
    }

    out() << "}";

    // Implementation: Wrapper destructor
    out() << wrapper_class << "::~" << wrapper_class << "()";
    out() << "{";
    out() << "}";

    // Implementation: impl_from()
    if (!interface.attributes.is_empty() || !interface.functions.is_empty()) {
        out() << "static " << interface.name << "* impl_from(JS::Interpreter& interpreter, JS::GlobalObject& global_object)";
        out() << "{";
        out() << "    auto* this_object = interpreter.this_value(global_object).to_object(interpreter, global_object);";
        out() << "    if (!this_object)";
        out() << "        return {};";
        out() << "    if (!this_object->inherits(\"" << wrapper_class << "\")) {";
        out() << "        interpreter.throw_exception<JS::TypeError>(JS::ErrorType::NotA, \"" << interface.name << "\");";
        out() << "        return nullptr;";
        out() << "    }";
        out() << "    return &static_cast<" << wrapper_class << "*>(this_object)->impl();";
        out() << "}";
    }

    auto generate_to_cpp = [&](auto& parameter, auto& js_name, auto& js_suffix, auto cpp_name, bool return_void = false) {
        auto generate_return = [&] {
            if (return_void)
                out() << "        return;";
            else
                out() << "        return {};";
        };
        if (parameter.type.name == "DOMString") {
            out() << "    auto " << cpp_name << " = " << js_name << js_suffix << ".to_string(interpreter);";
            out() << "    if (interpreter.exception())";
            generate_return();
        } else if (parameter.type.name == "EventListener") {
            out() << "    if (!" << js_name << js_suffix << ".is_function()) {";
            out() << "        interpreter.throw_exception<JS::TypeError>(JS::ErrorType::NotA, \"Function\");";
            generate_return();
            out() << "    }";
            out() << "    auto " << cpp_name << " = adopt(*new EventListener(JS::make_handle(&" << js_name << js_suffix << ".as_function())));";
        } else if (is_wrappable_type(parameter.type)) {
            out() << "    auto " << cpp_name << "_object = " << js_name << js_suffix << ".to_object(interpreter, global_object);";
            out() << "    if (interpreter.exception())";
            generate_return();
            out() << "    if (!" << cpp_name << "_object->inherits(\"" << parameter.type.name << "Wrapper\")) {";
            out() << "        interpreter.throw_exception<JS::TypeError>(JS::ErrorType::NotA, \"" << parameter.type.name << "\");";
            generate_return();
            out() << "    }";
            out() << "    auto& " << cpp_name << " = static_cast<" << parameter.type.name << "Wrapper*>(" << cpp_name << "_object)->impl();";
        } else if (parameter.type.name == "double") {
            out() << "    auto " << cpp_name << " = " << js_name << js_suffix << ".to_double(interpreter);";
            out() << "    if (interpreter.exception())";
            generate_return();
        } else {
            dbg() << "Unimplemented JS-to-C++ conversion: " << parameter.type.name;
            ASSERT_NOT_REACHED();
        }
    };

    auto generate_arguments = [&](auto& parameters, auto& arguments_builder, bool return_void = false) {
        Vector<String> parameter_names;
        size_t argument_index = 0;
        for (auto& parameter : parameters) {
            parameter_names.append(snake_name(parameter.name));
            out() << "    auto arg" << argument_index << " = interpreter.argument(" << argument_index << ");";
            generate_to_cpp(parameter, "arg", argument_index, snake_name(parameter.name), return_void);
            ++argument_index;
        }

        arguments_builder.join(", ", parameter_names);
    };

    auto generate_return_statement = [&](auto& return_type) {
        if (return_type.name == "void") {
            out() << "    return JS::js_undefined();";
            return;
        }

        if (return_type.nullable) {
            if (return_type.name == "DOMString") {
                out() << "    if (retval.is_null())";
            } else {
                out() << "    if (!retval)";
            }
            out() << "        return JS::js_null();";
        }

        if (return_type.name == "DOMString") {
            out() << "    return JS::js_string(interpreter, retval);";
        } else if (return_type.name == "ArrayFromVector") {
            // FIXME: Remove this fake type hack once it's no longer needed.
            //        Basically once we have NodeList we can throw this out.
            out() << "    auto* new_array = JS::Array::create(global_object);";
            out() << "    for (auto& element : retval) {";
            out() << "        new_array->indexed_properties().append(wrap(global_object, element));";
            out() << "    }";
            out() << "    return new_array;";
        } else if (return_type.name == "long" || return_type.name == "double") {
            out() << "    return JS::Value(retval);";
        } else if (return_type.name == "Uint8ClampedArray") {
            out() << "    return retval;";
        } else {
            out() << "    return wrap(global_object, const_cast<" << return_type.name << "&>(*retval));";
        }
    };

    // Implementation: Attributes
    for (auto& attribute : interface.attributes) {
        out() << "JS_DEFINE_NATIVE_GETTER(" << wrapper_class << "::" << attribute.getter_callback_name << ")";
        out() << "{";
        out() << "    auto* impl = impl_from(interpreter, global_object);";
        out() << "    if (!impl)";
        out() << "        return {};";
        out() << "    auto retval = impl->" << snake_name(attribute.name) << "();";
        generate_return_statement(attribute.type);
        out() << "}";

        if (!attribute.readonly) {
            out() << "JS_DEFINE_NATIVE_SETTER(" << wrapper_class << "::" << attribute.setter_callback_name << ")";
            out() << "{";
            out() << "    auto* impl = impl_from(interpreter, global_object);";
            out() << "    if (!impl)";
            out() << "        return;";

            generate_to_cpp(attribute, "value", "", "cpp_value", true);

            out() << "    impl->set_" << snake_name(attribute.name) << "(cpp_value);";
            out() << "}";
        }
    }

    // Implementation: Functions
    for (auto& function : interface.functions) {
        out() << "JS_DEFINE_NATIVE_FUNCTION(" << wrapper_class << "::" << snake_name(function.name) << ")";
        out() << "{";
        out() << "    auto* impl = impl_from(interpreter, global_object);";
        out() << "    if (!impl)";
        out() << "        return {};";
        if (function.length() > 0) {
            out() << "    if (interpreter.argument_count() < " << function.length() << ")";
            out() << "        return interpreter.throw_exception<JS::TypeError>(JS::ErrorType::BadArgCountMany, \"" << function.name << "\", \"" << function.length() << "\");";
        }

        StringBuilder arguments_builder;
        generate_arguments(function.parameters, arguments_builder);

        if (function.return_type.name != "void") {
            out() << "    auto retval = impl->" << snake_name(function.name) << "(" << arguments_builder.to_string() << ");";
        } else {
            out() << "    impl->" << snake_name(function.name) << "(" << arguments_builder.to_string() << ");";
        }

        generate_return_statement(function.return_type);
        out() << "}";
    }

    // Implementation: Wrapper factory
    if (should_emit_wrapper_factory(interface)) {
        out() << wrapper_class << "* wrap(JS::GlobalObject& global_object, " << interface.name << "& impl)";
        out() << "{";
        out() << "    return static_cast<" << wrapper_class << "*>(wrap_impl(global_object, impl));";
        out() << "}";
    }

    out() << "}";
    out() << "}";
}
