// SPDX-License-Identifier: MPL-2.0
#pragma once
class SdXMLImport;
class SvXMLStylesContext;

SvXMLStylesContext* CreatePDFPageStylesContext(SdXMLImport& rImport, bool bAutomatic);
