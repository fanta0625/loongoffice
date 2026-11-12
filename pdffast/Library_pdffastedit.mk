# This file is part of the LibreOffice project, MPL-2.0.
$(eval $(call gb_Library_Library,pdffastedit))
$(eval $(call gb_Library_use_api,pdffastedit,offapi udkapi))
$(eval $(call gb_Library_use_externals,pdffastedit,boost_headers))
$(eval $(call gb_Library_set_include,pdffastedit,\
    $$(INCLUDE) -I$(SRCDIR)/pdffast/inc \
))
$(eval $(call gb_Library_use_libraries,pdffastedit,\
    basegfx comphelper cppu cppuhelper sal svl svxcore tl utl vcl \
))
$(eval $(call gb_Library_add_exception_objects,pdffastedit,pdffast/source/editor))
# vim: set noet sw=4 ts=4:
