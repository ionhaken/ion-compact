/*
 * Copyright 2026 Markus Haikonen, Ionhaken
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once
#include <ion/CoreCommon.h>

#include <ion/compact/CompactSerialization.h>
#include <ion/compact/CompressionSystem.h>
namespace ion
{
namespace compact
{
template <typename BufferWriterType = ByteWriter, typename Allocator = GlobalAllocator<ion::u8>>
class CompactWriter
{
public:
	CompactWriter(BufferWriterType&& writer, size_t streamAllocation, size_t numStreams = 3)
	{
		mWriters.Reserve(numStreams);
		mWriters.AddKeepCapacity(std::move(writer));
		Init(streamAllocation, numStreams);
	}

	explicit CompactWriter(ByteBufferBase& buffer, size_t streamAllocation, size_t numStreams = 3)
	{
		mWriters.Reserve(numStreams);
		mWriters.AddKeepCapacity(std::move(BufferWriterType(buffer)));
		Init(streamAllocation, numStreams);
	}

	BufferWriterType ReleaseWriter()
	{
		BufferWriterType writer = std::move(mWriters[0]);
		mWriters.Clear();
		return writer;
	}

	template <typename T>
	inline bool Write(const T& t, serialization::Tag tag = serialization::TagDefault)
	{
		return mWriters[tag.mIndex].Process(t);
	}

	void EnsureCapacity(size_t capacity)
	{
		for (size_t i = 0; i < mWriters.Size(); ++i)
		{
			mWriters[i].EnsureCapacity(capacity);
		}
	}

	size_t TotalSize() const
	{
		size_t s = 0;
		for (size_t i = 0; i < mWriters.Size(); ++i)
		{
			s += mWriters[i].NumBytesUsed();
		}
		return s;
	}

	void CompressFromSubstreams(Compression& compression)
	{
		CopyFromSubstreams(
		  [&](BufferWriterType& writer, auto& subBuffer)
		  {
			  CompressedInfo info;
			  if (subBuffer.Size() != 0)
			  {
				  [[maybe_unused]] auto bytesWritten = writer.WriteDirectly(
					[&](byte* start, byte* end)
					{
						ion::ArrayView<byte, uint32_t> dstData(start, uint32_t(end - start));
						const ion::ArrayView<byte, uint32_t> srcData((byte*)subBuffer.Begin(), subBuffer.Size());
						info = Compress(compression, dstData, srcData);
						return info.mCompressedSize;
					});
			  }
			  return info;
		  });
	}

private:
	void Init(size_t streamAllocation, size_t numStreams = 2)
	{
		// mWriters store pointers into mSubBuffers; reserve here so subsequent Adds
		// don't reallocate and invalidate those pointers.
		mSubBuffers.Reserve(numStreams);
		while (mWriters.Size() < numStreams)
		{
			mSubBuffers.Add(ByteBuffer<0, Allocator>(SafeRangeCast<ByteSizeType>(streamAllocation)));
			mWriters.AddKeepCapacity(std::move(BufferWriterType(mSubBuffers.Back())));
		}
	}

	template <typename Callback>
	void CopyFromSubstreams(Callback&& callback)
	{
		SmallVector<CompressedInfo, 16> compressedInfo;
		size_t totalSize = 0;
		for (size_t i = 1; i < mWriters.Size(); ++i)
		{
			mWriters[i].Flush();
			totalSize += mWriters[i].NumBytesUsed();
		}

		mWriters[0].EnsureCapacity(SafeRangeCast<uint32_t>(totalSize * 2));
		for (size_t i = 0; i < mSubBuffers.Size(); ++i)
		{
			compressedInfo.Add(callback(mWriters[0], mSubBuffers[i]));
		}

		for (size_t i = 0; i < compressedInfo.Size(); ++i)
		{
			if (compressedInfo[i].mNumSizeBytes != 0)
			{
				ION_ASSERT(uint8_t(compressedInfo[i].mCompressor) < 8u, "Compressor enum value overflows the 3-bit wire slot");
				mWriters[0].WriteArray(reinterpret_cast<u8*>(&compressedInfo[i].mCompressedSize), compressedInfo[i].mNumSizeBytes);
				uint8_t compressor = uint8_t(compressedInfo[i].mCompressor) | uint8_t(compressedInfo[i].mOptions << 3);
				mWriters[0].Write(compressor);
			}
		}

		uint64_t flags = 0;
		for (size_t i = 0; i < compressedInfo.Size(); ++i)
		{
			flags |= uint64_t(compressedInfo[i].mNumSizeBytes & 0x3) << (2 * i);
		}
		// Each substream needs 2 bits in `flags`. Write enough bytes high-byte-first;
		// the reader walks from the buffer tail and processes the lowest byte first.
		const size_t numFlagBytes = (2 * compressedInfo.Size() + 7) / 8;
		ION_ASSERT(numFlagBytes <= sizeof(flags), "Too many substreams for flag word");
		for (int i = int(numFlagBytes) - 1; i >= 0; --i)
		{
			uint8_t b = uint8_t(flags >> (i * 8));
			mWriters[0].WriteArray(&b, 1);
		}
	}

	Vector<ByteBuffer<0, Allocator>> mSubBuffers;
	Vector<BufferWriterType> mWriters;
};
}  // namespace compact
}  // namespace ion
