/* This file is part of the LibreOffice project.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once
#include "types.hxx"
#include <vcl/filter/PDFiumLibrary.hxx>
#include <tools/stream.hxx>
#include <algorithm>
#include <stdexcept>

namespace pdffast
{
class Input : public vcl::pdf::PDFiumInput
{
public:
    virtual void validate() = 0;
    virtual void freeze(const BinaryDataContainer&) = 0;
};
std::shared_ptr<Input> openInput(const OUString&, const std::shared_ptr<std::atomic<bool>>&);

// Used in both same-build binaries. Caller holds Source::engineMutex so the
// reader cannot enter the file callbacks while the backing source is frozen.
inline void materializeSource(Source& source)
{
    if (!source.bytes.isEmpty())
        return;
    source.input->validate();
    class Stream final : public SvStream
    {
        Input& m_input;
        sal_uInt64 m_position = 0;
        std::size_t GetData(void* buffer, std::size_t length) override
        {
            length = std::min<sal_uInt64>(length, m_input.size() - m_position);
            if (!m_input.read(m_position, buffer, length))
                return 0;
            m_position += length;
            return length;
        }
        sal_uInt64 SeekPos(sal_uInt64 position) override
        {
            m_position = std::min(position, m_input.size());
            return m_position;
        }
    public:
        explicit Stream(Input& input) : m_input(input) {}
    } stream(*source.input);
    BinaryDataContainer bytes(stream, source.input->size());
    source.input->validate();
    if (bytes.getSize() != source.input->size())
        throw std::runtime_error("Cannot read a consistent PDF source for editing");
    source.input->freeze(bytes);
    source.bytes = std::move(bytes);
}
}
