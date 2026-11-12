# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_Executable_Executable,loongoffice-pdf-fast))
$(eval $(call gb_Executable_set_targettype_gui,loongoffice-pdf-fast,YES))
$(eval $(call gb_Executable_use_api,loongoffice-pdf-fast,offapi udkapi))
$(eval $(call gb_Executable_add_defs,loongoffice-pdf-fast,\
    -DVCL_INTERNALS \
))
$(eval $(call gb_Executable_set_include,loongoffice-pdf-fast,\
    $$(INCLUDE) -I$(SRCDIR)/pdffast/inc \
))
# Editing is loaded on demand, not linked into the reading entry point.
$(eval $(call gb_Executable_use_libraries,loongoffice-pdf-fast,\
    basegfx comphelper cppu cppuhelper fwk i18nlangtag sal tl utl vcl \
))
$(eval $(call gb_Executable_add_exception_objects,loongoffice-pdf-fast,\
    pdffast/source/app \
    pdffast/source/reader \
))
$(eval $(call gb_Executable_use_static_libraries,loongoffice-pdf-fast,pdffastcore))
# vim: set noet sw=4 ts=4:
