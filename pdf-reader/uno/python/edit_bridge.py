# SPDX-License-Identifier: MPL-2.0
"""UNO integration only. Does not import or call the PDF reading backend."""

FILTER_NAME = "draw_pdf_import"
DRAW_SERVICE = "com.sun.star.drawing.DrawingDocument"
ADAPTOR_SERVICE = "com.sun.star.comp.Writer.XmlFilterAdaptor"
RAW_IMPORTER = "org.libreoffice.comp.documents.DrawPDFImport"
XML_IMPORTER = "com.sun.star.comp.Draw.XMLOasisImporter"


def property_value(name, value):
    import uno

    result = uno.createUnoStruct("com.sun.star.beans.PropertyValue")
    result.Name = name
    result.Value = value
    return result


def inspect_filter(context):
    factory = context.ServiceManager.createInstanceWithContext(
        "com.sun.star.document.FilterFactory", context
    )
    if factory is None or not factory.hasByName(FILTER_NAME):
        raise RuntimeError("PDF Draw import filter is not installed")
    config = {item.Name: item.Value for item in factory.getByName(FILTER_NAME)}
    user_data = tuple(config.get("UserData", ()))
    if (
        config.get("DocumentService") != DRAW_SERVICE
        or config.get("FilterService") != ADAPTOR_SERVICE
        or len(user_data) < 3
        or user_data[0] != RAW_IMPORTER
        or user_data[2] != XML_IMPORTER
    ):
        raise RuntimeError("Unexpected PDF filter pipeline: " + repr(config))
    return factory, config


def open_for_edit(context, pdf_url, *, hidden=False, page=None):
    """Import through FilterFactory explicitly, bypassing SFX PDF routing.

    Returns a new, untitled Draw document. The caller owns it and presents it on
    success. Keep the reader window alive on failure. Saving defaults to a new
    document; this routine never overwrites the original PDF.
    Call on the Office UI thread when used inside an extension.
    page is an optional 1-based source page. It requires the selected-page
    xpdfimport update; omission retains the existing whole-document behaviour.
    """
    if page is not None and (type(page) is not int or not 1 <= page <= 2147483647):
        raise ValueError("page must be a positive 1-based integer")
    factory, _ = inspect_filter(context)
    importer = factory.createInstance(FILTER_NAME)
    if importer is None:
        raise RuntimeError("Cannot instantiate PDF import adaptor")

    services = context.ServiceManager
    desktop = services.createInstanceWithContext("com.sun.star.frame.Desktop", context)
    document = desktop.loadComponentFromURL(
        "private:factory/sdraw", "_blank", 0,
        (property_value("Hidden", hidden),),
    )
    if document is None:
        raise RuntimeError("Cannot create Draw target document")

    stream = None
    try:
        files = services.createInstanceWithContext("com.sun.star.ucb.SimpleFileAccess", context)
        stream = files.openFileRead(pdf_url)
        importer.setTargetDocument(document)
        descriptor = (
            property_value("URL", pdf_url),
            property_value("InputStream", stream),
            property_value("FilterName", FILTER_NAME),
        )
        if page is not None:
            descriptor += (property_value("FilterOptions", f"Page={page}"),)
        if not importer.filter(descriptor):
            raise RuntimeError("PDF editable import returned false")
        if not document.supportsService(DRAW_SERVICE):
            raise RuntimeError("PDF import did not produce a Draw document")
        if page is not None and document.getDrawPages().getCount() != 1:
            raise RuntimeError("Expected exactly one imported page; rebuild xpdfimport with Page= support")
        return document
    except BaseException:
        document.dispose()
        raise
    finally:
        if stream is not None:
            # PDFIRawAdaptor normally closes this itself, including on success.
            try:
                stream.closeInput()
            except Exception:
                pass
