# SPDX-License-Identifier: MPL-2.0
"""Import selected-page PDF SAX directly into a live Draw page, without clones.

The caller owns the controller/undo transaction and removes only the preview
after success. This is one synchronous native call, not interruptible rendering.
"""
from edit_bridge import RAW_IMPORTER, property_value as prop

DIRECT_IMPORTER = 'org.loongoffice.comp.Draw.PDFPageContentImporter'


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
    handler = services.createInstanceWithArgumentsAndContext(DIRECT_IMPORTER, (
        prop('TargetPage', target), prop('EditLayerName', layer_name),
        prop('ResourcePrefix', resource_prefix)), context)
    handler.setTargetDocument(document)
    converter = services.createInstanceWithContext(RAW_IMPORTER, context)
    files = services.createInstanceWithContext('com.sun.star.ucb.SimpleFileAccess', context)
    stream = files.openFileRead(pdf_url)
    try:
        # Both services live in Office. SAX never crosses into Python for each
        # shape, and no intermediate Draw model is created.
        if not converter.importer((prop('URL', pdf_url), prop('InputStream', stream),
                                   prop('FilterOptions', f'Page={page}')),
                                  handler, ()):
            raise RuntimeError('Direct PDF page import returned false')
    finally:
        try:
            stream.closeInput()
        except Exception:
            pass


def discard_resources(context, document, prefix):
    for service in ('DashTable', 'BitmapTable'):
        table = document.createInstance('com.sun.star.drawing.' + service)
        for name in table.getElementNames():
            if name.startswith(prefix):
                table.removeByName(name)
