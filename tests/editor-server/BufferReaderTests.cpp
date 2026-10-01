#include <gtest/gtest.h>

#include <stdexcept>

#include <editor-server/Commands.hpp>
#include <editor-server/Serialization.hpp>

using namespace N2Engine::Editor::Protocol;

TEST(BufferReaderTest, ReadsWellFormedData)
{
    BufferWriter w;
    w.WriteU8(7);
    w.WriteI32(-3);
    w.WriteString("scene");
    w.WriteF32(1.5f);

    BufferReader r(w.Data());
    EXPECT_EQ(r.ReadU8(), 7);
    EXPECT_EQ(r.ReadI32(), -3);
    EXPECT_EQ(r.ReadString(), "scene");
    EXPECT_FLOAT_EQ(r.ReadF32(), 1.5f);
    EXPECT_FALSE(r.HasData());
    EXPECT_EQ(r.Remaining(), 0u);
}

TEST(BufferReaderTest, ReadU8PastTheEndThrows)
{
    BufferReader r(std::span<const uint8_t>{});
    EXPECT_THROW(r.ReadU8(), std::out_of_range);
}

TEST(BufferReaderTest, ReadU32WithTooFewBytesThrows)
{
    const std::vector<uint8_t> bytes{1, 2, 3};
    BufferReader r(bytes);
    EXPECT_THROW(r.ReadU32(), std::out_of_range);
    // A failed read consumes nothing
    EXPECT_EQ(r.Remaining(), 3u);
}

TEST(BufferReaderTest, ReadStringLongerThanThePayloadThrows)
{
    BufferWriter w;
    w.WriteU32(1000); // claims 1000 bytes
    w.WriteU8('a');   // but only one follows

    BufferReader r(w.Data());
    EXPECT_THROW(r.ReadString(), std::out_of_range);
}

TEST(BufferReaderTest, ReadStringWithHugeLengthThrows)
{
    BufferWriter w;
    w.WriteU32(0xFFFFFFFFu);

    BufferReader r(w.Data());
    EXPECT_THROW(r.ReadString(), std::out_of_range);
}

TEST(BufferReaderTest, ReadBytesPastTheEndThrows)
{
    const std::vector<uint8_t> bytes{1, 2};
    BufferReader r(bytes);
    EXPECT_THROW(r.ReadBytes(3), std::out_of_range);
    EXPECT_EQ(r.ReadBytes(2).size(), 2u);
    EXPECT_THROW(r.ReadBytes(1), std::out_of_range);
}

TEST(BufferReaderTest, TruncatedCommandFailsToDeserialize)
{
    BufferWriter w;
    w.WriteString("entity");
    w.WriteF32(1.0f); // the rest of the transform is missing

    BufferReader r(w.Data());
    EXPECT_THROW(EntityTransformCmd::Deserialize(r), std::out_of_range);
}
