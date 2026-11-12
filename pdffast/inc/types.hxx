/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include "geometry.hxx"
#include <rtl/ustring.hxx>
#include <vcl/BinaryDataContainer.hxx>
#include <cstdint>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace pdffast
{
// One held source handle per session; detected changes fail closed. First edit
// freezes its complete bytes for both PDFium and the editing model.
// This lock coordinates users of one source (including the standalone harness's
// editor). Native Draw additionally uses the process-wide VCL PDFium guard.
class Input;
struct Source
{
    OUString url;
    BinaryDataContainer bytes; // Native import bytes, or frozen on first harness edit/export.
    std::shared_ptr<Input> input;
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    std::mutex engineMutex;
};

struct Pixels
{
    int width = 0;
    int height = 0;
    int stride = 0;
    // Opaque BGR, independent of PDFium and VCL bitmap lifetime/platform format.
    std::vector<unsigned char> data;
};

struct Result
{
    enum class Kind { Opened, Page, Tile, Password, Error };
    Kind kind = Kind::Error;
    bool requested = false;
    int pageCount = 0;
    int page = 0;
    PageSize size;
    TileKey tile;
    Pixels pixels;
    OUString message;
};
}
