# -*- Mode: makefile-gmake; tab-width: 4; indent-tabs-mode: t -*-
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

ifneq ($(CPUNAME),LOONGARCH64)
$(error sc_arraysum_lsx requires a LoongArch64 target)
endif

$(eval $(call gb_CppunitTest_CppunitTest,sc_arraysum_lsx))
ifeq ($(DISABLE_NAN_TESTS),TRUE)
$(eval $(call gb_CppunitTest_add_defs,sc_arraysum_lsx,\
    -DDISABLE_NAN_TESTS \
))
endif

$(eval $(call gb_CppunitTest_set_include,sc_arraysum_lsx,\
    -I$(SRCDIR)/sc/inc \
    $$(INCLUDE) \
))
$(eval $(call gb_CppunitTest_add_cxxflags,sc_arraysum_lsx,-mlsx))
$(eval $(call gb_CppunitTest_add_exception_objects,sc_arraysum_lsx,\
    sc/qa/unit/arraysum_lsx \
))
$(eval $(call gb_CppunitTest_use_libraries,sc_arraysum_lsx,sal))

# vim: set noet sw=4 ts=4:
