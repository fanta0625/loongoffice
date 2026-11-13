# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_StaticLibrary_StaticLibrary,pdffastcore))
$(eval $(call gb_StaticLibrary_add_defs,pdffastcore,-DSAL_LOG_INFO))
$(eval $(call gb_StaticLibrary_set_include,pdffastcore,\
    $$(INCLUDE) -I$(SRCDIR)/pdffast/inc \
))
$(eval $(call gb_StaticLibrary_use_api,pdffastcore,offapi udkapi))
# Consumers declare the dynamic libraries; this target only archives objects.
$(eval $(call gb_StaticLibrary_add_exception_objects,pdffastcore,\
    pdffast/source/session \
    pdffast/source/input \
    pdffast/source/nativeimport \
))
# vim: set noet sw=4 ts=4:
