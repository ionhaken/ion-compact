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
#include <ion/compact/CompressedInfo.h>
#include <ion/compact/DecompressionSystem.h>

namespace ion
{
namespace compact
{

struct Decompression;

template <typename Allocator = GlobalAllocator<ion::u8>>
class CompactReader
{
public:
	CompactReader(ByteReader& reader, size_t numStreams = 3)
	{
		mReaders.Reserve(numStreams);
		mReaders.AddKeepCapacity(ByteReader(reader.Data(), reader.Available()));
		reader.SkipBytes(reader.Available());
	}

	CompactReader(byte* data, size_t len, size_t numStreams = 3)
	{
		mReaders.Reserve(numStreams);
		mReaders.AddKeepCapacity(ByteReader(data, len));
	}

	template <typename T>
	inline bool Read(T& t, serialization::Tag tag = serialization::TagDefault)
	{
		return mReaders[tag.mIndex].Process(t);
	}

	void DecompressToSubstreams(Decompression& decompression)
	{
		CopyToSubstreams([&decompression](const CompressedInfo& info, ion::ByteReader& reader, ion::ByteWriter& writer)
						 { Decompress(decompression, info, reader, writer); });
	}

private:
	template <typename Callback>
	void CopyToSubstreams(Callback&& callback)
	{
		SmallVector<CompressedInfo, 16> compressedInfo;
		Prepare(compressedInfo);

		SmallVector<ByteReader, 32> tmpReaders;
		// tmpReaders below stores ByteReaders that reference mSubBuffers entries;
		// reserve so subsequent Adds don't reallocate and dangle those references.
		mSubBuffers.Reserve(mReaders.Size());
		for (size_t i = 1; i < mReaders.Size(); ++i)
		{
			mSubBuffers.Add(ByteBuffer<0, Allocator>());
			{
				auto& nextCompressedInfo = compressedInfo[i - 1];
				ByteWriter writer(mSubBuffers.Back());
				callback(nextCompressedInfo, mReaders[i], writer);
			}
			tmpReaders.Add(ByteReader(mSubBuffers.Back()));
		}

		mReaders.Resize(1);
		for (size_t i = 0; i < tmpReaders.Size(); ++i)
		{
			mReaders.Add(std::move(tmpReaders[i]));
		}
	}

	// Wire layout produced by CompactWriter::CopyFromSubstreams, walked tail-to-head:
	//
	//   [substream 0 payload][substream 1 payload]...[per-substream size+compressor info][flag bytes]
	//                                                                                    ^---- pos = len
	//
	// Flag bytes encode 2 bits per substream (= mNumSizeBytes 0..3); written
	// high-byte-first by the writer, so the byte at pos-1 holds substreams 0..3,
	// pos-2 holds substreams 4..7, etc.
	//
	// Per-substream info block (only present when its mNumSizeBytes != 0):
	//   [mNumSizeBytes bytes of compressed-size little-endian][1 byte: compressor|options<<3]
	//
	// Prepare() walks backwards: read flag bytes -> for each substream with
	// mNumSizeBytes != 0, peel off its size+compressor info -> the remaining tail
	// is the substream payload region, sliced into ByteReaders.
	void Prepare(SmallVector<CompressedInfo, 16>& outInfo)
	{
		SmallVector<CompressedInfo, 16> compressedInfo;
		size_t len = mReaders[0].Size();
		size_t origin = len - mReaders[0].Available();
		size_t pos = len;
		mReaders[0].Rewind();
		byte* data = (byte*)mReaders[0].Data();

		size_t numStreams = mReaders.Capacity();

		while (pos >= sizeof(uint8_t) && compressedInfo.Size() < numStreams - 1)
		{
			pos--;
			uint8_t infoByte;
			memcpy(&infoByte, data + pos, sizeof(uint8_t));
			CompressedInfo singleInfo;
			size_t index = 0;
			while (compressedInfo.Size() < numStreams - 1 && index <= 6)
			{
				singleInfo.mNumSizeBytes = ((infoByte >> index) & 0x3);
				index += 2;
				compressedInfo.Add(singleInfo);
			}
		}

		while (compressedInfo.Size() < numStreams - 1)
		{
			compressedInfo.Add(CompressedInfo{});
		}

		size_t totalSubStreamSize = 0;
		for (int i = int(compressedInfo.Size()) - 1; i >= 0; --i)
		{
			if (compressedInfo[i].mNumSizeBytes != 0)
			{
				if (pos < compressedInfo[i].mNumSizeBytes + 1u)
				{
					ION_ABNORMAL("Out of compressed info data");
					break;
				}
				pos -= compressedInfo[i].mNumSizeBytes + 1u;
				uint32_t size = 0;

				memcpy(&size, data + pos, compressedInfo[i].mNumSizeBytes);
				memcpy(&compressedInfo[i].mOptions, data + pos + compressedInfo[i].mNumSizeBytes, 1);
				compressedInfo[i].mCompressor = Compressor(compressedInfo[i].mOptions & ((1 << 3) - 1));
				compressedInfo[i].mOptions = compressedInfo[i].mOptions >> 3;
				compressedInfo[i].mCompressedSize = size;
				totalSubStreamSize += size;
			}
		}

		if (totalSubStreamSize <= pos)
		{
			mReaders.Clear();
			mReaders.Add(ByteReader(data, pos - totalSubStreamSize));
			pos = 0;
			outInfo.Reserve(compressedInfo.Size());
			for (size_t i = 0; i < compressedInfo.Size(); ++i)
			{
				outInfo.Add(compressedInfo[i]);
				pos += mReaders.Back().Available();
				mReaders.Add(ByteReader(data + pos, compressedInfo[i].mCompressedSize));
			}
			mReaders[0].SkipBytes(SafeRangeCast<ByteSizeType>(origin));
		}
		else
		{
			ION_ABNORMAL("Invalid substreams");
		}
	}

	Vector<ByteBuffer<0, Allocator>> mSubBuffers;
	Vector<ByteReader> mReaders;
};
}  // namespace compact
}  // namespace ion
