# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_CppunitTest_CppunitTest,pdffast_core))
$(eval $(call gb_CppunitTest_set_include,pdffast_core,\
    $$(INCLUDE) -I$(SRCDIR)/pdffast/inc \
))
$(eval $(call gb_CppunitTest_use_libraries,pdffast_core,sal))
$(eval $(call gb_CppunitTest_add_exception_objects,pdffast_core,pdffast/qa/core))
# vim: set noet sw=4 ts=4:
