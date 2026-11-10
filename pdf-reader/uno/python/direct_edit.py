# SPDX-License-Identifier: MPL-2.0
"""Import selected-page PDF SAX directly into a live Draw page, without clones.

The caller owns the controller/undo transaction and removes only the preview
after success. This is one synchronous native call, not interruptible rendering.
"""
from edit_bridge import property_value as prop

DIRECT_IMPORTER = 'org.loongoffice.comp.Draw.PDFPageImportFilter'


class DirectImportUnavailable(RuntimeError):
    pass


def import_into_page(context, document, target, pdf_url, page, layer_name, resource_prefix):
    if type(page) is not int or page < 1:
        raise ValueError('Expected a positive source page')
    services = context.ServiceManager
    # Check before opening the PDF or touching the target. No silent fallback
    # to the old per-object copier on an unpatched Office.
    if DIRECT_IMPORTER not in services.getAvailableServiceNames():
        raise DirectImportUnavailable('Rebuild Office: native PDF target-page importer is unavailable')
    importer = services.createInstanceWithArgumentsAndContext(DIRECT_IMPORTER, (
        prop('TargetPage', target), prop('EditLayerName', layer_name),
        prop('ResourcePrefix', resource_prefix)), context)
    importer.setTargetDocument(document)
    # One native operation owns stream creation, conversion, SAX and the
    # outer SolarMutex execution boundary. Do not call the raw converter here:
    # individual object locks allowed the GUI to paint partial imports.
    if not importer.filter((prop('URL', pdf_url), prop('SourcePage', page))):
        raise RuntimeError('Native PDF page import returned false or was cancelled')


def discard_resources(context, document, prefix):
    for service in ('DashTable', 'BitmapTable'):
        table = document.createInstance('com.sun.star.drawing.' + service)
        for name in table.getElementNames():
            if name.startswith(prefix):
                table.removeByName(name)
    # Native named-style import uses the same request prefix. Remove styles
    # after their objects on rollback; never remove or reset the host standard.
    graphics = document.getStyleFamilies().getByName('graphics')
    for name in graphics.getElementNames():
        if name.startswith(prefix):
            graphics.removeByName(name)
