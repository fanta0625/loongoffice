# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_Module_Module,pdffast))
ifeq ($(ENABLE_LOONGOFFICE_PDF_FAST),TRUE)
$(eval $(call gb_Module_add_targets,pdffast,\
    StaticLibrary_pdffastcore \
    Executable_loongoffice-pdf-fast \
    Library_pdffastedit \
    UIConfig_pdffast \
))
$(eval $(call gb_Module_add_check_targets,pdffast,CppunitTest_pdffast_core CppunitTest_pdffast_render))
endif
# vim: set noet sw=4 ts=4:
