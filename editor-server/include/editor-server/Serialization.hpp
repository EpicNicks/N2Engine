#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <span>
#include <string>

namespace N2Engine::Editor::Protocol
{
    class BufferWriter
    {
    public:
        void WriteU8(uint8_t v) { _buffer.push_back(v); }
        void WriteI32(int32_t v) { Write(&v, sizeof(v)); }
        void WriteU32(uint32_t v) { Write(&v, sizeof(v)); }
        void WriteF32(float v) { Write(&v, sizeof(v)); }
        void WriteBool(bool v) { WriteU8(v ? 1 : 0); }

        void WriteString(const std::string &s)
        {
            WriteU32(static_cast<uint32_t>(s.size()));
            Write(s.data(), s.size());
        }

        void WriteBytes(std::span<const uint8_t> data)
        {
            _buffer.insert(_buffer.end(), data.begin(), data.end());
        }

        std::span<const uint8_t> Data() const { return _buffer; }
        size_t Size() const { return _buffer.size(); }
        void Clear() { _buffer.clear(); }

    private:
        std::vector<uint8_t> _buffer;

        void Write(const void *data, size_t size)
        {
            auto *bytes = static_cast<const uint8_t*>(data);
            _buffer.insert(_buffer.end(), bytes, bytes + size);
        }
    };

    class BufferReader
    {
    public:
        explicit BufferReader(std::span<const uint8_t> data)
            : _data(data), _pos(0) {}

        uint8_t ReadU8() { return _data[_pos++]; }

        int32_t ReadI32()
        {
            int32_t v;
            Read(&v, sizeof(v));
            return v;
        }

        uint32_t ReadU32()
        {
            uint32_t v;
            Read(&v, sizeof(v));
            return v;
        }

        float ReadF32()
        {
            float v;
            Read(&v, sizeof(v));
            return v;
        }

        bool ReadBool() { return ReadU8() != 0; }

        std::string ReadString()
        {
            uint32_t len = ReadU32();
            std::string s(_data.begin() + _pos, _data.begin() + _pos + len);
            _pos += len;
            return s;
        }

        std::span<const uint8_t> ReadBytes(size_t count)
        {
            auto span = _data.subspan(_pos, count);
            _pos += count;
            return span;
        }

        size_t Remaining() const { return _data.size() - _pos; }
        bool HasData() const { return _pos < _data.size(); }

    private:
        std::span<const uint8_t> _data;
        size_t _pos;

        void Read(void *out, size_t size)
        {
            std::memcpy(out, _data.data() + _pos, size);
            _pos += size;
        }
    };
}
