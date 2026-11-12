/* This file is part of the LibreOffice project, MPL-2.0. */
#include <sal/config.h>
#include <sal/types.h>
#include "input.hxx"
#include "session.hxx"
#include <unotest/bootstrapfixturebase.hxx>
#include <cppunit/extensions/HelperMacros.h>
#include <cppunit/plugin/TestPlugIn.h>
#include <chrono>
#include <cstring>
#include <thread>

namespace
{
class RenderTest : public test::BootstrapFixtureBase
{
protected:
    OUString url() { return m_directories.getURLFromSrc(u"/vcl/qa/cppunit/data/Pangram.pdf"); }
    pdffast::Result wait(pdffast::Session& session, pdffast::Result::Kind kind)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (auto result = session.takeResult())
            {
                CPPUNIT_ASSERT_MESSAGE("PDF worker returned an error", result->kind != pdffast::Result::Kind::Error);
                if (result->kind == kind)
                    return std::move(*result);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        CPPUNIT_FAIL("Timed out waiting for PDF worker result");
        return {};
    }
};

CPPUNIT_TEST_FIXTURE(RenderTest, testPixelsAndCancellation)
{
    auto engine = vcl::pdf::PDFiumLibrary::get();
    CPPUNIT_ASSERT(engine);
    SvFileStream stream(url(), StreamMode::READ);
    BinaryDataContainer bytes(stream, stream.remainingSize());
    auto document = engine->openDocument(bytes.getData(), bytes.getSize(), OString());
    CPPUNIT_ASSERT(document);
    auto page = document->openPage(0);
    CPPUNIT_ASSERT(page);
    int width = 1024, height = 1024;
    auto full = engine->createBitmap(width, height, 0);
    CPPUNIT_ASSERT(full);
    full->fillRect(0, 0, width, height, 0xffffffff);
    full->renderPageBitmap(document.get(), page.get(), 0, 0, 1024, 1024);
    auto progressive = engine->createBitmap(width, height, 0);
    CPPUNIT_ASSERT(progressive);
    progressive->fillRect(0, 0, width, height, 0xffffffff);
    int callbacks = 0;
    CPPUNIT_ASSERT(!progressive->renderPageBitmapCancelable(document.get(), page.get(),
        0, 0, 1024, 1024, [&] { return ++callbacks >= 2; }));
    CPPUNIT_ASSERT(callbacks >= 2);
    // Reuse the SAME page after cancellation: leaked progressive state must not
    // affect a second render. The tiny fixture may finish before its first pause.
    progressive->fillRect(0, 0, width, height, 0xffffffff);
    CPPUNIT_ASSERT(progressive->renderPageBitmapCancelable(document.get(), page.get(),
        0, 0, 1024, 1024, [] { return false; }));
    CPPUNIT_ASSERT(full->getFormat() == progressive->getFormat());
    const int channels = full->getFormat() == vcl::pdf::PDFBitmapType::BGR ? 3 : 4;
    for (int y = 0; y < 1024; ++y)
        CPPUNIT_ASSERT(std::memcmp(full->getBuffer() + y * full->getStride(),
            progressive->getBuffer() + y * progressive->getStride(), 1024 * channels) == 0);
    int cropWidth = 320, cropHeight = 300;
    auto crop = engine->createBitmap(cropWidth, cropHeight, 0);
    CPPUNIT_ASSERT(crop);
    crop->fillRect(0, 0, cropWidth, cropHeight, 0xffffffff);
    CPPUNIT_ASSERT(crop->renderPageBitmapCancelable(document.get(), page.get(),
        -512, -512, 1024, 1024, [] { return false; }));
    CPPUNIT_ASSERT(full->getFormat() == crop->getFormat());
    for (int y = 0; y < cropHeight; ++y)
        CPPUNIT_ASSERT(std::memcmp(full->getBuffer() + (y + 512) * full->getStride() + 512 * channels,
            crop->getBuffer() + y * crop->getStride(), cropWidth * channels) == 0);
}

CPPUNIT_TEST_FIXTURE(RenderTest, testRandomInputOwnershipAndFreeze)
{
    auto engine = vcl::pdf::PDFiumLibrary::get();
    CPPUNIT_ASSERT(engine);
    pdffast::Source source;
    source.input = pdffast::openInput(url(), source.stop);
    std::weak_ptr<pdffast::Input> weak = source.input;
    auto document = engine->openDocument(source.input, OString());
    CPPUNIT_ASSERT(document);
    pdffast::materializeSource(source);
    CPPUNIT_ASSERT(!source.bytes.isEmpty());
    unsigned char header[5]{};
    CPPUNIT_ASSERT(source.input->read(0, header, sizeof(header)));
    CPPUNIT_ASSERT(std::memcmp(header, source.bytes.getData(), sizeof(header)) == 0);
    CPPUNIT_ASSERT(!source.input->read(source.input->size(), header, 1));
    source.input.reset(); // The document must retain the callback backing object.
    CPPUNIT_ASSERT(!weak.expired());
    auto page = document->openPage(0);
    CPPUNIT_ASSERT(page);
    page.reset();
    document.reset();
    CPPUNIT_ASSERT(weak.expired());
}

CPPUNIT_TEST_FIXTURE(RenderTest, testSessionReplaceAndClose)
{
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        pdffast::Session session(url());
        wait(session, pdffast::Result::Kind::Opened);
        std::vector<pdffast::Request> requests;
        for (int y = 0; y < 4096; y += 512)
            for (int x = 0; x < 4096; x += 512)
                requests.push_back({ 0, { 0, 4096, 4096, x, y, 512, 512 } });
        session.request(std::move(requests));
        const pdffast::TileKey replacement{ 0, 1024, 1024, 512, 512, 512, 512 };
        session.request({ { 0, replacement } });
        auto result = wait(session, pdffast::Result::Kind::Tile);
        CPPUNIT_ASSERT(result.tile == replacement);
        CPPUNIT_ASSERT_EQUAL(512, result.pixels.width);
        CPPUNIT_ASSERT_EQUAL(512, result.pixels.height);
        CPPUNIT_ASSERT(result.pixels.stride >= 512 * 3);
        CPPUNIT_ASSERT(result.pixels.data.size() >= std::size_t(result.pixels.stride) * 512);
        {
            // Check the actual worker's crop/copy/BGR result, not just geometry.
            std::lock_guard guard(session.source()->engineMutex);
            auto engine = vcl::pdf::PDFiumLibrary::get();
            CPPUNIT_ASSERT(engine);
            SvFileStream stream(url(), StreamMode::READ);
            BinaryDataContainer bytes(stream, stream.remainingSize());
            auto document = engine->openDocument(bytes.getData(), bytes.getSize(), OString());
            CPPUNIT_ASSERT(document);
            auto page = document->openPage(0);
            CPPUNIT_ASSERT(page);
            int width = 1024, height = 1024;
            auto full = engine->createBitmap(width, height, 0);
            CPPUNIT_ASSERT(full);
            full->fillRect(0, 0, width, height, 0xffffffff);
            full->renderPageBitmap(document.get(), page.get(), 0, 0, 1024, 1024);
            const int channels = full->getFormat() == vcl::pdf::PDFBitmapType::BGR ? 3 : 4;
            for (int y = 0; y < 512; ++y)
                for (int x = 0; x < 512; ++x)
                    CPPUNIT_ASSERT(std::memcmp(
                        full->getBuffer() + (y + 512) * full->getStride() + (x + 512) * channels,
                        result.pixels.data.data() + y * result.pixels.stride + x * 3, 3) == 0);
        }
        session.request({ { 0, { 0, 8192, 8192, 0, 0, 512, 512 } } });
        // Destructor cancels queued/in-flight work and joins without UI pumping.
        // Scheduling determines whether the final task has actually started.
    }
    pdffast::Session immediateClose(url());
}
}
CPPUNIT_TEST_FIXTURE(RenderTest, testNativeMemorySessionAlongsideDirectEngine)
{
    auto engine = vcl::pdf::PDFiumLibrary::get();
    CPPUNIT_ASSERT(engine);
    SvFileStream stream(url(), StreamMode::READ);
    BinaryDataContainer bytes(stream, stream.remainingSize());
    auto document = engine->openDocument(bytes.getData(), bytes.getSize(), OString());
    CPPUNIT_ASSERT(document);
    auto page = document->openPage(0);
    CPPUNIT_ASSERT(page);
    int width = 512, height = 512;
    auto reference = engine->createBitmap(width, height, 0);
    CPPUNIT_ASSERT(reference);
    reference->fillRect(0, 0, width, height, 0xffffffff);
    reference->renderPageBitmap(document.get(), page.get(), 0, 0, width, height);
    const int channels = reference->getFormat() == vcl::pdf::PDFBitmapType::BGR ? 3 : 4;
    for (int iteration = 0; iteration < 4; ++iteration)
    {
        auto session = std::make_unique<pdffast::Session>(bytes);
        CPPUNIT_ASSERT_EQUAL(1, wait(*session, pdffast::Result::Kind::Opened).pageCount);
        session->request({ { 0, { 0, 512, 512, 0, 0, 512, 512 } } });
        // Direct native-style PDFium calls overlap the background session. VCL's
        // engine boundary must protect all of them, not just Session's own lock.
        for (int n = 0; n < 4; ++n)
        {
            auto nativePage = document->openPage(0);
            int w = 256, h = 256;
            auto bitmap = engine->createBitmap(w, h, 0);
            CPPUNIT_ASSERT(nativePage && bitmap);
            bitmap->fillRect(0, 0, w, h, 0xffffffff);
            bitmap->renderPageBitmap(document.get(), nativePage.get(), 0, 0, w, h);
        }
        auto result = wait(*session, pdffast::Result::Kind::Tile);
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x)
                CPPUNIT_ASSERT(std::memcmp(reference->getBuffer() + y * reference->getStride() + x * channels,
                    result.pixels.data.data() + y * result.pixels.stride + x * 3, 3) == 0);
        session->request({ { 0, { 0, 8192, 8192, 0, 0, 512, 512 } } });
        session.reset(); // close/cancel while native document and page remain alive
        CPPUNIT_ASSERT_EQUAL(1, document->getPageCount());
    }
}

CPPUNIT_PLUGIN_IMPLEMENT();
