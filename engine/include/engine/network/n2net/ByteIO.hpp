#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace N2Engine::N2Net
{
    /// Little-endian, bounds-checked writer over a caller-owned buffer. A write that does not fit writes nothing and
    /// sets the overflow flag (it never writes past the end). Byte by byte, so it is endian- and alignment-independent.
    class ByteWriter
    {
    public:
        ByteWriter(std::uint8_t *data, std::size_t capacity) noexcept : data_(data), capacity_(capacity) {}

        [[nodiscard]] std::size_t Size() const noexcept { return position_; }
        [[nodiscard]] std::size_t Remaining() const noexcept { return capacity_ - position_; }
        [[nodiscard]] bool Overflowed() const noexcept { return overflow_; }

        void U8(std::uint8_t value) noexcept
        {
            if (Remaining() < 1)
            {
                overflow_ = true;
                return;
            }
            data_[position_++] = value;
        }

        void U16(std::uint16_t value) noexcept
        {
            if (Remaining() < 2)
            {
                overflow_ = true;
                return;
            }
            data_[position_++] = static_cast<std::uint8_t>(value & 0xFFu);
            data_[position_++] = static_cast<std::uint8_t>(value >> 8);
        }

        void U32(std::uint32_t value) noexcept
        {
            if (Remaining() < 4)
            {
                overflow_ = true;
                return;
            }
            for (int shift = 0; shift < 32; shift += 8)
            {
                data_[position_++] = static_cast<std::uint8_t>((value >> shift) & 0xFFu);
            }
        }

        void Bytes(std::span<const std::uint8_t> bytes) noexcept
        {
            if (Remaining() < bytes.size())
            {
                overflow_ = true;
                return;
            }
            for (std::uint8_t byte : bytes)
            {
                data_[position_++] = byte;
            }
        }

    private:
        std::uint8_t *data_;
        std::size_t capacity_;
        std::size_t position_ = 0;
        bool overflow_ = false;
    };

    /// Little-endian, bounds-checked reader over untrusted bytes. A read past the end returns 0 (or an empty span) and
    /// sets the failed flag; it never reads past the end, whatever lengths the bytes themselves claim.
    class ByteReader
    {
    public:
        explicit ByteReader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

        [[nodiscard]] std::size_t Remaining() const noexcept { return bytes_.size() - position_; }
        [[nodiscard]] bool Failed() const noexcept { return failed_; }

        std::uint8_t U8() noexcept
        {
            if (Remaining() < 1)
            {
                failed_ = true;
                return 0;
            }
            return bytes_[position_++];
        }

        std::uint16_t U16() noexcept
        {
            if (Remaining() < 2)
            {
                failed_ = true;
                return 0;
            }
            const std::uint16_t value = static_cast<std::uint16_t>(bytes_[position_] | (bytes_[position_ + 1] << 8));
            position_ += 2;
            return value;
        }

        std::uint32_t U32() noexcept
        {
            if (Remaining() < 4)
            {
                failed_ = true;
                return 0;
            }
            std::uint32_t value = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                value |= static_cast<std::uint32_t>(bytes_[position_++]) << shift;
            }
            return value;
        }

        /// The next `count` bytes as a view into the input, or an empty span (and the failed flag) if fewer remain
        std::span<const std::uint8_t> Bytes(std::size_t count) noexcept
        {
            if (Remaining() < count)
            {
                failed_ = true;
                return {};
            }
            const std::span<const std::uint8_t> view = bytes_.subspan(position_, count);
            position_ += count;
            return view;
        }

    private:
        std::span<const std::uint8_t> bytes_;
        std::size_t position_ = 0;
        bool failed_ = false;
    };
} // namespace N2Engine::N2Net
