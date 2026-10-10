#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>

struct ByteChunk
{
    uint8_t Bytes[128]{};
    uint16_t Length = 0;
    uint64_t DrainUs = 0, Sequence = 0;
};
class ByteHistory
{
public:
    static constexpr size_t Capacity = 16;
    void Add(const uint8_t *Data, size_t Length, uint64_t DrainUs)
    {
        while (Length)
        {
            auto &Chunk = Chunks[Next];
            size_t N = Length < sizeof(Chunk.Bytes) ? Length : sizeof(Chunk.Bytes);
            memcpy(Chunk.Bytes, Data, N);
            Chunk.Length = N;
            Chunk.DrainUs = DrainUs;
            Chunk.Sequence = ++TotalChunks;
            Next = (Next + 1) % Capacity;
            if (Count < Capacity) ++Count; else ++Evictions;
            Data += N;
            Length -= N;
        }
    }
    const ByteChunk &At(size_t Index) const
    {
        return Chunks[(Next + Capacity - Count + Index) % Capacity];
    }
    size_t Count = 0;
    uint64_t Evictions = 0, TotalChunks = 0;
private:
    ByteChunk Chunks[Capacity];
    size_t Next = 0;
};
