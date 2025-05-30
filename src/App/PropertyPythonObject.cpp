/***************************************************************************
 *   Copyright (c) 2009 Werner Mayer <wmayer[at]users.sourceforge.net>     *
 *                                                                         *
 *   This file is part of the FreeCAD CAx development system.              *
 *                                                                         *
 *   This library is free software; you can redistribute it and/or         *
 *   modify it under the terms of the GNU Library General Public           *
 *   License as published by the Free Software Foundation; either          *
 *   version 2 of the License, or (at your option) any later version.      *
 *                                                                         *
 *   This library  is distributed in the hope that it will be useful,      *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU Library General Public License for more details.                  *
 *                                                                         *
 *   You should have received a copy of the GNU Library General Public     *
 *   License along with this library; see the file COPYING.LIB. If not,    *
 *   write to the Free Software Foundation, Inc., 59 Temple Place,         *
 *   Suite 330, Boston, MA  02111-1307, USA                                *
 *                                                                         *
 ***************************************************************************/


#include "PreCompiled.h"

#include <iostream>
#include <boost/regex.hpp>

#include <Base/Base64.h>
#include <Base/Console.h>
#include <Base/Interpreter.h>
#include <Base/Reader.h>
#include <Base/Writer.h>

#include "PropertyPythonObject.h"
#include "DocumentObject.h"


using namespace App;


TYPESYSTEM_SOURCE(App::PropertyPythonObject, App::Property)

PropertyPythonObject::PropertyPythonObject() = default;

PropertyPythonObject::~PropertyPythonObject()
{
    // this is needed because the release of the pickled object may need the
    // GIL. Thus, we grab the GIL and replace the pickled with an empty object
    Base::PyGILStateLocker lock;
    try {
        this->object = Py::Object();
    } catch (Py::TypeError &) {
        Base::Console().warning("Py::TypeError Exception caught while destroying PropertyPythonObject\n");
    }
}

void PropertyPythonObject::setValue(const Py::Object& py)
{
    Base::PyGILStateLocker lock;
    aboutToSetValue();
    this->object = py;
    hasSetValue();
}

Py::Object PropertyPythonObject::getValue() const
{
    return object;
}

PyObject* PropertyPythonObject::getPyObject()
{
    return Py::new_reference_to(this->object);
}

void PropertyPythonObject::setPyObject(PyObject* obj)
{
    Base::PyGILStateLocker lock;
    aboutToSetValue();
    this->object = obj;
    hasSetValue();
}

std::string PropertyPythonObject::toString() const
{
    std::string repr;
    Base::PyGILStateLocker lock;
    try {
        Py::Module pickle(PyImport_ImportModule("json"), true);
        if (pickle.isNull()) {
            throw Py::Exception();
        }
        Py::Callable method(pickle.getAttr(std::string("dumps")));
        Py::Object dump;
        if (this->object.hasAttr("dumps")) {
            Py::Tuple args;
            Py::Callable state(this->object.getAttr("dumps"));
            dump = state.apply(args);
        }
        // support add-ons that use the old method names
        else if (this->object.hasAttr("__getstate__")
#if PY_VERSION_HEX >= 0x030b0000
                 && this->object.getAttr("__getstate__").hasAttr("__func__")
#endif
        ) {
            Py::Tuple args;
            Py::Callable state(this->object.getAttr("__getstate__"));
            dump = state.apply(args);
        }
        else if (this->object.hasAttr("__dict__")) {
            dump = this->object.getAttr("__dict__");
        }
        else {
            dump = this->object;
        }

        Py::Tuple args(1);
        args.setItem(0, dump);
        Py::Object res = method.apply(args);
        Py::String str(res);
        repr = str.as_std_string("ascii");
    }
    catch (Py::Exception&) {
        Py::String typestr(this->object.type().str());
        Base::Console().error("PropertyPythonObject::toString(): failed for %s\n",
                              typestr.as_string().c_str());
        Base::PyException e;  // extract the Python error text
        e.reportException();
    }

    return repr;
}

void PropertyPythonObject::fromString(const std::string& repr)
{
    Base::PyGILStateLocker lock;
    try {
        if (repr.empty()) {
            return;
        }
        Py::Module pickle(PyImport_ImportModule("json"), true);
        if (pickle.isNull()) {
            throw Py::Exception();
        }
        Py::Callable method(pickle.getAttr(std::string("loads")));
        Py::Tuple args(1);
        args.setItem(0, Py::String(repr));
        Py::Object res = method.apply(args);

        if (this->object.hasAttr("loads")) {
            Py::Tuple args(1);
            args.setItem(0, res);
            Py::Callable state(this->object.getAttr("loads"));
            state.apply(args);
        }
        // support add-ons that use the old method names
        else if (this->object.hasAttr("__setstate__")
#if PY_VERSION_HEX >= 0x030b0000
                 && this->object.getAttr("__setstate__").hasAttr("__func__")
#endif
        ) {
            Py::Tuple args(1);
            args.setItem(0, res);
            Py::Callable state(this->object.getAttr("__setstate__"));
            state.apply(args);
        }
        else if (this->object.hasAttr("__dict__")) {
            if (!res.isNone()) {
                this->object.setAttr("__dict__", res);
            }
        }
        else {
            this->object = res;
        }
    }
    catch (Py::Exception&) {
        Base::PyException e;  // extract the Python error text
        e.reportException();
    }
}

void PropertyPythonObject::loadPickle(const std::string& str)
{
    // find the custom attributes and restore them
    Base::PyGILStateLocker lock;
    try {
        std::string buffer = str;
        boost::regex pickle(R"(S'(\w+)'.+S'(\w+)'\n)");
        boost::match_results<std::string::const_iterator> what;
        std::string::const_iterator start, end;
        start = buffer.begin();
        end = buffer.end();
        while (boost::regex_search(start, end, what, pickle)) {
            std::string key = std::string(what[1].first, what[1].second);
            std::string val = std::string(what[2].first, what[2].second);
            this->object.setAttr(key, Py::String(val));
            buffer = std::string(what[2].second, end);
            start = buffer.begin();
            end = buffer.end();
        }
    }
    catch (Py::Exception&) {
        Base::PyException e;  // extract the Python error text
        e.reportException();
    }
}

std::string PropertyPythonObject::encodeValue(const std::string& str) const
{
    std::string tmp;
    for (char it : str) {
        if (it == '<') {
            tmp += "&lt;";
        }
        else if (it == '"') {
            tmp += "&quot;";
        }
        else if (it == '&') {
            tmp += "&amp;";
        }
        else if (it == '>') {
            tmp += "&gt";
        }
        else if (it == '\n') {
            tmp += "\\n";
        }
        else {
            tmp += it;
        }
    }

    return tmp;
}

std::string PropertyPythonObject::decodeValue(const std::string& str) const
{
    std::string tmp;
    for (std::string::const_iterator it = str.begin(); it != str.end(); ++it) {
        if (*it == '\\') {
            ++it;
            if (it != str.end() && *it == 'n') {
                tmp += '\n';
            }
        }
        else {
            tmp += *it;
        }
    }

    return tmp;
}

void PropertyPythonObject::saveObject(Base::Writer& writer) const
{
    Base::PyGILStateLocker lock;
    try {
        PropertyContainer* parent = this->getContainer();
        if (parent->isDerivedFrom(Base::Type::fromName("App::DocumentObject"))) {
            if (this->object.hasAttr("__object__")) {
                writer.Stream() << " object=\"yes\"";
            }
        }
        if (parent->isDerivedFrom(Base::Type::fromName("Gui::ViewProvider"))) {
            if (this->object.hasAttr("__vobject__")) {
                writer.Stream() << " vobject=\"yes\"";
            }
        }
    }
    catch (Py::Exception& e) {
        e.clear();
    }
}

void PropertyPythonObject::restoreObject(Base::XMLReader& reader)
{
    Base::PyGILStateLocker lock;
    try {
        PropertyContainer* parent = this->getContainer();
        if (reader.hasAttribute("object")) {
            if (strcmp(reader.getAttribute<const char*>("object"), "yes") == 0) {
                Py::Object obj = Py::asObject(parent->getPyObject());
                this->object.setAttr("__object__", obj);
            }
        }
        if (reader.hasAttribute("vobject")) {
            if (strcmp(reader.getAttribute<const char*>("vobject"), "yes") == 0) {
                Py::Object obj = Py::asObject(parent->getPyObject());
                this->object.setAttr("__vobject__", obj);
            }
        }
    }
    catch (Py::Exception& e) {
        e.clear();
    }
    catch (const Base::Exception& e) {
        Base::Console().error("%s\n", e.what());
    }
    catch (...) {
        Base::Console().error("Critical error in PropertyPythonObject::restoreObject\n");
    }
}

void PropertyPythonObject::Save(Base::Writer& writer) const
{
    std::string repr = this->toString();
    repr = Base::base64_encode((const unsigned char*)repr.c_str(), repr.size());
    std::string val = /*encodeValue*/ (repr);
    writer.Stream() << writer.ind() << "<Python value=\"" << val << R"(" encoded="yes")";

    Base::PyGILStateLocker lock;
    try {
        if (this->object.hasAttr("__module__") && this->object.hasAttr("__class__")) {
            Py::String mod(this->object.getAttr("__module__"));
            Py::Object cls(this->object.getAttr("__class__"));
            if (cls.hasAttr("__name__")) {
                Py::String name(cls.getAttr("__name__"));
                writer.Stream() << " module=\"" << (std::string)mod << "\""
                                << " class=\"" << (std::string)name << "\"";
            }
        }
        else {
            writer.Stream() << " json=\"yes\"";
        }
    }
    catch (Py::Exception&) {
        Base::PyException e;  // extract the Python error text
        e.reportException();
    }

    saveObject(writer);
    writer.Stream() << "/>" << std::endl;
}

void PropertyPythonObject::Restore(Base::XMLReader& reader)
{
    reader.readElement("Python");
    if (reader.hasAttribute("file")) {
        std::string file(reader.getAttribute<const char*>("file"));
        reader.addFile(file.c_str(), this);
    }
    else {
        bool load_json = false;
        bool load_pickle = false;
        bool load_failed = false;
        std::string buffer = reader.getAttribute<const char*>("value");
        if (reader.hasAttribute("encoded") && strcmp(reader.getAttribute<const char*>("encoded"), "yes") == 0) {
            buffer = Base::base64_decode(buffer);
        }
        else {
            buffer = decodeValue(buffer);
        }

        Base::PyGILStateLocker lock;
        try {
            boost::regex pickle(R"(^\(i(\w+)\n(\w+)\n)");
            boost::match_results<std::string::const_iterator> what;
            std::string::const_iterator start, end;
            start = buffer.begin();
            end = buffer.end();
            if (reader.hasAttribute("module") && reader.hasAttribute("class")) {
                Py::Module mod(PyImport_ImportModule(reader.getAttribute<const char*>("module")), true);
                if (mod.isNull()) {
                    throw Py::Exception();
                }
                PyObject* cls = mod.getAttr(reader.getAttribute<const char*>("class")).ptr();
                if (!cls) {
                    std::stringstream s;
                    s << "Module " << reader.getAttribute<const char*>("module") << " has no class "
                      << reader.getAttribute<const char*>("class");
                    throw Py::AttributeError(s.str());
                }
                if (PyType_Check(cls)) {
                    this->object = PyType_GenericAlloc((PyTypeObject*)cls, 0);
                }
                else {
                    throw Py::TypeError("neither class nor type object");
                }
                load_json = true;
            }
            else if (boost::regex_search(start, end, what, pickle)) {
                std::string name = std::string(what[1].first, what[1].second);
                std::string type = std::string(what[2].first, what[2].second);
                Py::Module mod(PyImport_ImportModule(name.c_str()), true);
                if (mod.isNull()) {
                    throw Py::Exception();
                }
                this->object = PyObject_CallObject(mod.getAttr(type).ptr(), nullptr);
                load_pickle = true;
                buffer = std::string(what[2].second, end);
            }
            else if (reader.hasAttribute("json")) {
                load_json = true;
            }
        }
        catch (Py::Exception&) {
            Base::PyException e;  // extract the Python error text
            e.reportException();
            this->object = Py::None();
            load_failed = true;
        }

        aboutToSetValue();
        if (load_json) {
            this->fromString(buffer);
        }
        else if (load_pickle) {
            this->loadPickle(buffer);
        }
        else if (!load_failed) {
            Base::Console().warning(
                "PropertyPythonObject::Restore: unsupported serialisation: %s\n",
                buffer.c_str());
        }
        restoreObject(reader);
        hasSetValue();
    }
}

void PropertyPythonObject::SaveDocFile(Base::Writer& writer) const
{
    std::string buffer = this->toString();
    for (char it : buffer) {
        writer.Stream().put(it);
    }
}

void PropertyPythonObject::RestoreDocFile(Base::Reader& reader)
{
    aboutToSetValue();
    std::string buffer;
    char ch {};
    while (reader.get(ch)) {
        buffer.push_back(ch);
    }
    this->fromString(buffer);
    hasSetValue();
}

unsigned int PropertyPythonObject::getMemSize() const
{
    return sizeof(Py::Object);
}

Property* PropertyPythonObject::Copy() const
{
    PropertyPythonObject* p = new PropertyPythonObject();
    Base::PyGILStateLocker lock;
    p->object = this->object;
    return p;
}

void PropertyPythonObject::Paste(const Property& from)
{
    if (from.is<PropertyPythonObject>()) {
        Base::PyGILStateLocker lock;
        aboutToSetValue();
        this->object = static_cast<const PropertyPythonObject&>(from).object;
        hasSetValue();
    }
}
