# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_CppunitTest_CppunitTest,pdffast_render))
$(eval $(call gb_CppunitTest_set_include,pdffast_render,\
    $$(INCLUDE) -I$(SRCDIR)/pdffast/inc \
))
$(eval $(call gb_CppunitTest_add_exception_objects,pdffast_render,\
    pdffast/qa/render \
))
$(eval $(call gb_CppunitTest_use_static_libraries,pdffast_render,pdffastcore))
$(eval $(call gb_CppunitTest_use_libraries,pdffast_render,\
    basegfx comphelper cppu cppuhelper sal test unotest utl tl vcl \
))
$(eval $(call gb_CppunitTest_use_sdk_api,pdffast_render))
$(eval $(call gb_CppunitTest_use_ure,pdffast_render))
$(eval $(call gb_CppunitTest_use_vcl,pdffast_render))
$(eval $(call gb_CppunitTest_use_rdb,pdffast_render,services))
$(eval $(call gb_CppunitTest_use_configuration,pdffast_render))
