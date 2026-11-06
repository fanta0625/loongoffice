/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include "PdfHybridViewLayout.hxx"
#include <svx/sdr/overlay/overlayobjectlist.hxx>
#include <tools/gen.hxx>

namespace sdr::overlay { class OverlayManager; }

namespace sd
{
// Opt-in P0-A experiment, owned by the view and destroyed before its SdrView.
// Holds only view resources and value geometry; never owns an editing page.
class PdfHybridViewProbe final
{
public:
    const pdfhybrid::Layout layout;
    const Point normalOrigin;
    const Size normalSize;
    const tools::Rectangle normalWorkArea;
    sdr::overlay::OverlayObjectList overlays;

    PdfHybridViewProbe(const pdfhybrid::Layout& rLayout, const Point& rOrigin,
                       const Size& rSize, const tools::Rectangle& rWorkArea)
        : layout(rLayout), normalOrigin(rOrigin), normalSize(rSize), normalWorkArea(rWorkArea)
    {
    }
};
}
