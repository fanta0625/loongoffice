# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

$(eval $(call gb_CppunitTest_CppunitTest,sc_arraysum))
ifeq ($(DISABLE_NAN_TESTS),TRUE)
$(eval $(call gb_CppunitTest_add_defs,sc_arraysum,\
    -DDISABLE_NAN_TESTS \
))
endif

$(eval $(call gb_CppunitTest_set_include,sc_arraysum,\
    -I$(SRCDIR)/sc/inc \
    $$(INCLUDE) \
))
$(eval $(call gb_CppunitTest_add_exception_objects,sc_arraysum,\
    sc/qa/unit/arraysum \
))
$(eval $(call gb_CppunitTest_use_libraries,sc_arraysum,sal))

# vim: set noet sw=4 ts=4:
