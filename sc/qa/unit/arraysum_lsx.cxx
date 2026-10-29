/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// A distinct object keeps the opt-in ISA flags out of the default test and sc.
#define SC_ARRAYSUM_REQUIRE_LSX 1
#include "arraysum.cxx"

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
