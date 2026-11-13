/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "types.hxx"
#include <tools/gen.hxx>
#include <functional>
#include <memory>

class CommandEvent;
class KeyEvent;
namespace vcl
{
class Window;
}

namespace pdffast
{
// Private, same-build ABI between the reader and the optional editor library.
// No sd private classes or new public UNO interfaces cross this boundary.
struct EditorHost
{
    std::function<void()> changed;
    std::function<void(const CommandEvent&)> command;
    std::function<void(const KeyEvent&)> key;
};

class Editor
{
public:
    virtual ~Editor() = default;
    virtual bool prepare(int sourcePage, PageSize size, OUString& error) = 0;
    virtual bool contains(int sourcePage) const = 0;
    virtual void readOnly(bool enabled, bool zoomSettling) = 0;
    virtual void place(int sourcePage, const tools::Rectangle& rect, bool visible) = 0;
    virtual void focus(int sourcePage) = 0;
    virtual void finish() = 0;
    virtual bool modified() const = 0;
    virtual void undo(bool redo) = 0;
    // Export a NEW PDF copy. Does not clear the editable models or undo stacks.
    virtual bool exportCopy(const OUString& url, OUString& error) = 0;
};

using CreateEditor = Editor* (*)(vcl::Window*, const std::shared_ptr<Source>&, const EditorHost&);
}
