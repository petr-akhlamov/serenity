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

#include <AK/Function.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Interpreter.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ObjectConstructor.h>
#include <LibJS/Runtime/Shape.h>

namespace JS {

ObjectConstructor::ObjectConstructor(GlobalObject& global_object)
    : NativeFunction("Object", *global_object.function_prototype())
{
}

void ObjectConstructor::initialize(Interpreter& interpreter, GlobalObject& global_object)
{
    NativeFunction::initialize(interpreter, global_object);
    define_property("prototype", global_object.object_prototype(), 0);
    define_property("length", Value(1), Attribute::Configurable);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function("defineProperty", define_property_, 3, attr);
    define_native_function("is", is, 2, attr);
    define_native_function("getOwnPropertyDescriptor", get_own_property_descriptor, 2, attr);
    define_native_function("getOwnPropertyNames", get_own_property_names, 1, attr);
    define_native_function("getPrototypeOf", get_prototype_of, 1, attr);
    define_native_function("setPrototypeOf", set_prototype_of, 2, attr);
    define_native_function("isExtensible", is_extensible, 1, attr);
    define_native_function("preventExtensions", prevent_extensions, 1, attr);
    define_native_function("keys", keys, 1, attr);
    define_native_function("values", values, 1, attr);
    define_native_function("entries", entries, 1, attr);
}

ObjectConstructor::~ObjectConstructor()
{
}

Value ObjectConstructor::call(Interpreter& interpreter)
{
    return Object::create_empty(interpreter, global_object());
}

Value ObjectConstructor::construct(Interpreter& interpreter, Function&)
{
    return call(interpreter);
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_names)
{
    if (!interpreter.argument_count())
        return {};
    auto* object = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};
    auto* result = Array::create(global_object);
    for (auto& entry : object->indexed_properties())
        result->indexed_properties().append(js_string(interpreter, String::number(entry.index())));
    for (auto& it : object->shape().property_table_ordered())
        result->indexed_properties().append(js_string(interpreter, it.key));

    return result;
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_prototype_of)
{
    if (!interpreter.argument_count())
        return {};
    auto* object = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};
    return object->prototype();
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::set_prototype_of)
{
    if (interpreter.argument_count() < 2)
        return interpreter.throw_exception<TypeError>(ErrorType::ObjectSetPrototypeOfTwoArgs);
    auto* object = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};
    auto prototype_value = interpreter.argument(1);
    Object* prototype;
    if (prototype_value.is_null()) {
        prototype = nullptr;
    } else if (prototype_value.is_object()) {
        prototype = &prototype_value.as_object();
    } else {
        interpreter.throw_exception<TypeError>(ErrorType::ObjectPrototypeWrongType);
        return {};
    }
    if (!object->set_prototype(prototype)) {
        if (!interpreter.exception())
            interpreter.throw_exception<TypeError>(ErrorType::ObjectSetPrototypeOfReturnedFalse);
        return {};
    }
    return object;
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is_extensible)
{
    auto argument = interpreter.argument(0);
    if (!argument.is_object())
        return Value(false);
    return Value(argument.as_object().is_extensible());
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::prevent_extensions)
{
    auto argument = interpreter.argument(0);
    if (!argument.is_object())
        return argument;
    if (!argument.as_object().prevent_extensions()) {
        if (!interpreter.exception())
            interpreter.throw_exception<TypeError>(ErrorType::ObjectPreventExtensionsReturnedFalse);
        return {};
    }
    return argument;
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::get_own_property_descriptor)
{
    auto* object = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};
    auto property_key = interpreter.argument(1).to_string(interpreter);
    if (interpreter.exception())
        return {};
    return object->get_own_property_descriptor_object(property_key);
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::define_property_)
{
    if (!interpreter.argument(0).is_object())
        return interpreter.throw_exception<TypeError>(ErrorType::NotAnObject, "Object argument");
    if (!interpreter.argument(2).is_object())
        return interpreter.throw_exception<TypeError>(ErrorType::NotAnObject, "Descriptor argument");
    auto& object = interpreter.argument(0).as_object();
    auto property_key = interpreter.argument(1).to_string(interpreter);
    if (interpreter.exception())
        return {};
    auto& descriptor = interpreter.argument(2).as_object();
    if (!object.define_property(property_key, descriptor)) {
        if (!interpreter.exception()) {
            if (object.is_proxy_object()) {
                interpreter.throw_exception<TypeError>(ErrorType::ObjectDefinePropertyReturnedFalse);
            } else {
                interpreter.throw_exception<TypeError>(ErrorType::NonExtensibleDefine, property_key.characters());
            }
        }
        return {};
    }
    return &object;
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::is)
{
    return Value(same_value(interpreter, interpreter.argument(0), interpreter.argument(1)));
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::keys)
{
    if (!interpreter.argument_count())
        return interpreter.throw_exception<TypeError>(ErrorType::ConvertUndefinedToObject);

    auto* obj_arg = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};

    return obj_arg->get_own_properties(*obj_arg, GetOwnPropertyMode::Key, true);
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::values)
{
    if (!interpreter.argument_count())
        return interpreter.throw_exception<TypeError>(ErrorType::ConvertUndefinedToObject);

    auto* obj_arg = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};

    return obj_arg->get_own_properties(*obj_arg, GetOwnPropertyMode::Value, true);
}

JS_DEFINE_NATIVE_FUNCTION(ObjectConstructor::entries)
{
    if (!interpreter.argument_count())
        return interpreter.throw_exception<TypeError>(ErrorType::ConvertUndefinedToObject);

    auto* obj_arg = interpreter.argument(0).to_object(interpreter, global_object);
    if (interpreter.exception())
        return {};

    return obj_arg->get_own_properties(*obj_arg, GetOwnPropertyMode::KeyAndValue, true);
}

}
