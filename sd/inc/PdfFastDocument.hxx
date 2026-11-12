/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <memory>
#include <sddllapi.h>

class SdDrawDocument;
class SdrObject;
class SdrView;
class OutputDevice;
namespace sdr::contact { class ViewObjectContact; }
namespace drawinglayer::primitive2d { class Primitive2DDecompositionVisitor; }

namespace sd
{
// Owns raster-only state. The native document remains the authority for objects,
// annotations, undo, serialization and export. No document mutation during paint.
class SD_DLLPUBLIC PdfFastDocument final
{
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PdfFastDocument(SdDrawDocument&);

public:
    ~PdfFastDocument();
    static bool Attach(SdDrawDocument&);
    void PreparePaint(SdrView&, OutputDevice&);
    bool Paint(const sdr::contact::ViewObjectContact&, OutputDevice&,
               drawinglayer::primitive2d::Primitive2DDecompositionVisitor&);
    void PrepareCurrentPage(SdrView&);
};
}
