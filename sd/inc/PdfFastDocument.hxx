/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <memory>
#include <tools/link.hxx>
#include <sddllapi.h>

class SdDrawDocument;
class SdrObject;
class SdrView;
class OutputDevice;
class SdrPage;
class Size;
class Bitmap;
namespace sdr::contact { class ViewObjectContact; }
namespace vcl { class PDFMetadataReader; }
namespace drawinglayer::primitive2d { class Primitive2DDecompositionVisitor; }

namespace sd
{
// Owns deferred PDF state. The native document remains the authority for objects,
// annotations, undo, serialization and export. No document mutation during paint.
class SD_DLLPUBLIC PdfFastDocument final
{
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    explicit PdfFastDocument(SdDrawDocument&);

public:
    ~PdfFastDocument();
    static bool Attach(SdDrawDocument&);
    void SetMetadataReader(std::unique_ptr<vcl::PDFMetadataReader>);
    bool EnsureMetadata(SdrPage&);
    bool EnsureAllMetadata();
    // False for materialized pages: keep the original Draw redirector untouched.
    bool PreparePaint(SdrView&, OutputDevice&);
    bool Paint(const sdr::contact::ViewObjectContact&, OutputDevice&,
               drawinglayer::primitive2d::Primitive2DDecompositionVisitor&);
    void PrepareCurrentPage(SdrView&);
    // Page-change notifications happen while the PDF is initially being
    // constructed. They must not start native conversion until the user has
    // explicitly entered Draw's edit mode.
    void SetEditMode(bool);
    bool IsEditMode() const;
    void NoteScroll();
    enum class EditState { Ready, Preparing, Failed };
    void RequestCurrentPage(SdrView&, const Link<bool, void>& = {});
    EditState GetEditState(const SdrView&) const;
    void ForgetView(const SdrView&);
    bool PrepareForSave();
    enum class PreviewResult { NotHandled, Pending, Ready };
    PreviewResult GetPagePreview(const SdrPage&, const Size&, Bitmap&, bool visible = true);
};
}
