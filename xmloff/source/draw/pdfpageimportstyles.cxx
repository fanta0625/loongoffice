// SPDX-License-Identifier: MPL-2.0
#include "pdfpageimportstyles.hxx"
#include "sdxmlimp_impl.hxx"
#include "ximpstyl.hxx"
#include "../style/FillStyleContext.hxx"
#include <sax/fastattribs.hxx>
#include <xmloff/xmlnamespace.hxx>
#include <xmloff/xmltoken.hxx>
#include <xmloff/families.hxx>
#include <xmloff/XMLShapeStyleContext.hxx>
#include <com/sun/star/container/XNameContainer.hpp>

using namespace com::sun::star;
using namespace xmloff::token;

namespace {
// Attribute parsing is deferred until startFastElement. Adapt SetAttribute,
// not the attribute list passed only to the child-context factory.
class PDFPageShapeStyleContext final : public XMLShapeStyleContext
{
    OUString maPrefix;
    bool mbAutomatic;
    void SetAttribute(sal_Int32 nElement, const OUString& rValue) override
    {
        if ((!mbAutomatic && nElement == XML_ELEMENT(STYLE, XML_NAME))
            || nElement == XML_ELEMENT(STYLE, XML_PARENT_STYLE_NAME))
        {
            if (nElement == XML_ELEMENT(STYLE, XML_NAME))
                GetImport().AddStyleDisplayName(XmlStyleFamily::SD_GRAPHICS_ID, rValue,
                                                maPrefix + rValue);
            XMLShapeStyleContext::SetAttribute(nElement, maPrefix + rValue);
        }
        else if (mbAutomatic || nElement != XML_ELEMENT(STYLE, XML_DISPLAY_NAME))
            XMLShapeStyleContext::SetAttribute(nElement, rValue);
    }
public:
    PDFPageShapeStyleContext(SdXMLImport& rImport, SvXMLStylesContext& rStyles,
                             XmlStyleFamily nFamily, bool bAutomatic)
        : XMLShapeStyleContext(rImport, rStyles, nFamily),
          maPrefix(rImport.GetDirectResourcePrefix()), mbAutomatic(bAutomatic) {}
};

// Keep the native default/property mapper and parent binding. Only incoming
// named styles/resources and their references receive a request-local name.
class PDFPageStylesContext final : public SdXMLStylesContext
{
    SdXMLImport& mrImport;
    bool mbAutomatic;
    using SdXMLStylesContext::CreateStyleChildContext;

    SvXMLStyleContext* CreateStyleChildContext(
        sal_Int32 nElement, const uno::Reference<xml::sax::XFastAttributeList>& xAttrs) override
    {
        if (nElement == XML_ELEMENT(DRAW, XML_STROKE_DASH)
            || nElement == XML_ELEMENT(DRAW, XML_FILL_IMAGE))
            return CreateResource(nElement, xAttrs);
        if (nElement == XML_ELEMENT(STYLE, XML_DEFAULT_STYLE))
            throw uno::RuntimeException(u"PDF page import cannot change document defaults"_ustr);
        return SdXMLStylesContext::CreateStyleChildContext(nElement, xAttrs);
    }

    using SdXMLStylesContext::CreateStyleStyleChildContext;
    SvXMLStyleContext* CreateStyleStyleChildContext(
        XmlStyleFamily nFamily, sal_Int32 nElement,
        const uno::Reference<xml::sax::XFastAttributeList>& xAttrs) override
    {
        if (nFamily == XmlStyleFamily::SD_GRAPHICS_ID)
            return new PDFPageShapeStyleContext(mrImport, *this, nFamily, mbAutomatic);
        return SdXMLStylesContext::CreateStyleStyleChildContext(nFamily, nElement, xAttrs);
    }

    SvXMLStyleContext* CreateResource(
        sal_Int32 nElement, const uno::Reference<xml::sax::XFastAttributeList>& xAttrs)
    {
        const bool bDash = nElement == XML_ELEMENT(DRAW, XML_STROKE_DASH);
        if (!bDash && nElement != XML_ELEMENT(DRAW, XML_FILL_IMAGE))
            throw uno::RuntimeException(u"Unsupported PDF page resource"_ustr);
        OUString aName;
        auto xTable = bDash ? GetImport().GetDashHelper() : GetImport().GetBitmapHelper();
        if (!xTable.is())
            throw uno::RuntimeException(u"Missing Draw resource table"_ustr);
        sal_Int32 nSuffix = 1;
        OUString aPrivateName;
        do
            aPrivateName = mrImport.GetDirectResourcePrefix()
                           + (bDash ? u"dash-"_ustr : u"bitmap-"_ustr) + OUString::number(nSuffix++);
        while (xTable->hasByName(aPrivateName));
        rtl::Reference<sax_fastparser::FastAttributeList> xPrivateAttrs(
            new sax_fastparser::FastAttributeList(nullptr));
        for (const auto& rAttr : sax_fastparser::castToFastAttributeList(xAttrs))
        {
            if (rAttr.getToken() == XML_ELEMENT(DRAW, XML_NAME))
            {
                aName = rAttr.toString();
                xPrivateAttrs->add(rAttr.getToken(), aPrivateName);
            }
            else if (rAttr.getToken() != XML_ELEMENT(DRAW, XML_DISPLAY_NAME))
                xPrivateAttrs->add(rAttr.getToken(), rAttr.toString());
        }
        if (aName.isEmpty())
            throw uno::RuntimeException(u"Missing PDF resource name"_ustr);
        GetImport().AddStyleDisplayName(bDash ? XmlStyleFamily::SD_STROKE_DASH_ID
                                             : XmlStyleFamily::SD_FILL_IMAGE_ID,
                                       aName, aPrivateName);
        if (bDash)
            return new XMLDashStyleContext(GetImport(), nElement, xPrivateAttrs);
        return new XMLBitmapStyleContext(GetImport(), nElement, xPrivateAttrs);
    }
public:
    PDFPageStylesContext(SdXMLImport& rImport, bool bAutomatic)
        : SdXMLStylesContext(rImport, bAutomatic), mrImport(rImport), mbAutomatic(bAutomatic) {}
};
}

SvXMLStylesContext* CreatePDFPageStylesContext(SdXMLImport& rImport, bool bAutomatic)
{
    return new PDFPageStylesContext(rImport, bAutomatic);
}
