// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pdf_reader
{
// Plain data only: no UNO, VCL, Draw, Qt, or PDF-engine handles cross this API.
struct PageSize
{
    double width_points;
    double height_points;
};

struct PixelRect
{
    int x;
    int y;
    int width;
    int height;
};

struct RenderSpec
{
    int page;
    int page_width_pixels;
    int page_height_pixels;
    // Region in full-page pixel coordinates. A full-page request uses
    // {0, 0, page_width_pixels, page_height_pixels}.
    PixelRect region;
    int rotation_degrees; // Clockwise: 0, 90, 180, or 270.
};

struct Image
{
    int width;
    int height;
    int stride;
    // Owned, top-down RGBA8, straight alpha. No backend-owned pointers.
    std::vector<std::uint8_t> pixels;
};

class CancellationToken
{
public:
    explicit CancellationToken(std::shared_ptr<const std::atomic<bool>> state)
        : m_state(std::move(state))
    {
    }
    bool cancelled() const { return m_state && m_state->load(); }

private:
    std::shared_ptr<const std::atomic<bool>> m_state;
};

// A session owns one document. Calls for that document are serialized by the
// session worker. Implementations additionally enforce engine-wide constraints.
class DocumentBackend
{
public:
    virtual ~DocumentBackend() = default;
    virtual int page_count() const = 0;
    virtual PageSize page_size(int page) const = 0;
    // Throw on failure; a cancelled request must not be published. A backend
    // without progressive cancellation may finish before cancellation is seen.
    virtual Image render(const RenderSpec&, const CancellationToken&) = 0;
};

class BackendFactory
{
public:
    virtual ~BackendFactory() = default;
    // Local filesystem path, UTF-8; URL conversion belongs to the adapter.
    virtual std::unique_ptr<DocumentBackend> open(const std::string& path_utf8) = 0;
};
}
