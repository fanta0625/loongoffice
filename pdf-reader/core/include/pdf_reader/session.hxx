// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "backend.hxx"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pdf_reader
{
using ViewId = std::uint64_t;
using Generation = std::uint64_t;

enum class Priority { Visible, Prefetch };

struct Request
{
    RenderSpec spec;
    Priority priority;
};

struct Completion
{
    ViewId view;
    Generation generation;
    RenderSpec spec;
    std::shared_ptr<const Image> image;
};

struct Limits
{
    std::size_t cache_bytes = 100 * 1024 * 1024;
    std::size_t in_flight_bytes = 32 * 1024 * 1024;
    std::size_t queued_requests = 64;
};

// Interface contract, not a scheduler implementation yet.
// Cache, queue, and cancellation belong behind this boundary. The UI never
// calls DocumentBackend directly and never mutates core state from a callback.
class ReaderSession
{
public:
    virtual ~ReaderSession() = default;
    // Replace the demand of one view. Other views retain their requests.
    // Equivalent render specs can share work and immutable cached images.
    virtual void update(ViewId, Generation, const std::vector<Request>&) = 0;
    virtual std::shared_ptr<const Image> lookup(const RenderSpec&) const = 0;
    // UI thread polls/drains results and checks view generation before painting.
    // No raw UI pointer or UNO callback is retained by the worker.
    virtual std::vector<Completion> take_completed() = 0;
    virtual void detach(ViewId) = 0;
    // Stop accepting work, cancel pending demands, join workers. No publications
    // after return; destroying the session must perform the same shutdown.
    virtual void close() = 0;
};
}
