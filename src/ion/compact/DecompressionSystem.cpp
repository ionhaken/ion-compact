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
#include <ion/compact/Decompression.h>
#include <ion/compact/DecompressionSystem.h>
#include <ion/compact/CompressionUtil.h>

// ZSTD
#include "zstd/lib/decompress/zstd_decompress_internal.h"
#ifdef XXH_FALLTHROUGH
	#undef XXH_FALLTHROUGH
#endif
#include "zstd/lib/zstd.h"
#define FSE_STATIC_LINKING_ONLY
#include "zstd/lib/common/huf.h"
#include "zstd/lib/compress/hist.h"
// /ZSTD

// Per-call codec-selection tracing. Off by default; flip to ION_LOG_INFO when
// debugging compressor choices. Defined locally so it doesn't leak from a public header.
#ifndef ION_LOG_COMPRESSION
	#define ION_LOG_COMPRESSION(x) ((void)0)
#endif

namespace ion
{
namespace compact
{
namespace
{
void* zalloc(void*, size_t s) { return ion::Malloc(s); }

void zfree(void*, void* address) { ion::Free(address); }


size_t Unbitpack(ion::ArrayView<byte, uint32_t>& dstData, ion::ArrayView<byte, uint32_t>& srcData, size_t decompressedSize,
				 unsigned bitsPerSymbol)
{
	ByteReader reader(srcData.Data(), srcData.Size());
	ByteWriterUnsafe writer(dstData.Data());

	const unsigned Mask = (1u << bitsPerSymbol) - 1;
	unsigned index = 0;
	uint64_t tmp = 0;
	unsigned bitsLeft = 0;
	while (reader.Available() >= 4)
	{
		uint32_t next;
		reader.ReadAssumeAvailable(next);
		tmp |= (uint64_t(next) << bitsLeft);
		bitsLeft += 32;
		index = 0;
		while (bitsPerSymbol <= bitsLeft)
		{
			uint8_t out = uint8_t((tmp >> index) & Mask);
			if (writer.NumBytesUsed() < decompressedSize)
			{
				writer.Write(out);
			}
			index += bitsPerSymbol;
			bitsLeft -= bitsPerSymbol;
		}
		tmp = tmp >> index;
	}

	while (reader.Available() >= 1)
	{
		uint8_t next = 0;
		reader.ReadAssumeAvailable(next);
		tmp |= (uint64_t(next) << bitsLeft);
		bitsLeft += 8;
		index = 0;
		while (bitsPerSymbol <= bitsLeft)
		{
			uint8_t out = uint8_t((tmp >> index) & Mask);
			if (writer.NumBytesUsed() < decompressedSize)
			{
				writer.Write(out);
			}
			index += bitsPerSymbol;
			bitsLeft -= bitsPerSymbol;
		}
		tmp = tmp >> index;
	}

	return writer.NumBytesUsed();
}

uint32_t DecompressRLE(ion::ArrayView<byte, uint32_t>& dstData, ion::ArrayView<byte, uint32_t>& srcData, size_t decompressedSize,
					   unsigned numBitsUsed)
{
	ByteReader reader(srcData.Data(), srcData.Size());
	if (numBitsUsed <= 8)
	{
		uint8_t nextSymbol = 0;
		uint8_t prevCount = 0;

		byte* op = dstData.Data();
		byte* end = op + decompressedSize;
		if (numBitsUsed == 8)
		{
			while (reader.Available() >= 1)
			{
				reader.ReadAssumeAvailable(nextSymbol);
				if (reader.Available() >= 1)
				{
					uint8_t diff = 0;
					reader.ReadAssumeAvailable(diff);
					uint8_t count = diff + prevCount;
					if (op + (count + 1) > end)
					{
						ION_ABNORMAL("Out of space");
						return 0;
					}
					prevCount = count;
					memset(op, nextSymbol, count + 1);
					op += (count + 1);
				}
				else
				{
					memset(op, nextSymbol, size_t(end - op));
					op = end;
					return uint32_t(op - dstData.Data());
				}
			}
		}
		else
		{
			while (reader.Available() >= 1)
			{
				reader.ReadAssumeAvailable(nextSymbol);
				uint8_t diff = nextSymbol >> numBitsUsed;
				uint8_t count = (diff + prevCount) % (1 << (8 - numBitsUsed));
				nextSymbol &= (1 << (numBitsUsed)) - 1;
				if (op + (count + 1) > end)
				{
					break;
				}
				memset(op, nextSymbol, count + 1);
				op += (count + 1);
				prevCount = count;
			}
		}
		if (op != end)
		{
			memset(op, nextSymbol, size_t(end - op));
			op = end;
		}
		return uint32_t(op - dstData.Data());
	}

	uint8_t nextSymbol = 0;
	uint8_t options = 0;
	byte* op = dstData.Data();
	byte* end = op + decompressedSize;
	while (reader.Available() >= 1)
	{
		reader.ReadAssumeAvailable(nextSymbol);
		if (reader.Available() >= 1)
		{
			reader.ReadAssumeAvailable(options);
			bool is8Bit = (options & 1);
			options = options >> 1;

			if (is8Bit)
			{
				if (op + (options + 1) > end)
				{
					break;
				}
				memset(op, nextSymbol, options + 1);
				op += (options + 1);
				for (int i = 0; i < 2; ++i)
				{
					if (reader.Available() >= 1 && op < end)
					{
						reader.ReadAssumeAvailable(nextSymbol);
						*op = nextSymbol;
						++op;
					}
				}
			}
			else
			{
				uint8_t other = nextSymbol & ((1 << 4) - 1);
				nextSymbol = nextSymbol >> 4;
				if (op + (options + 1) > end)
				{
					break;
				}
				memset(op, nextSymbol, options + 1);
				op += (options + 1);
				nextSymbol = other;
				if (op < end)
				{
					*op = nextSymbol;
					++op;
				}
				for (int i = 0; i < 4; ++i)
				{
					if (reader.Available() >= 1)
					{
						reader.ReadAssumeAvailable(nextSymbol);
						other = nextSymbol >> 4;
						nextSymbol = nextSymbol & ((1 << 4) - 1);
						if (op < end)
						{
							*op = other;
							++op;
						}
						if (op < end)
						{
							*op = nextSymbol;
							++op;
						}
					}
				}
			}
		}
		else
		{
			break;
		}
	}

	if (op != end)
	{
		memset(op, nextSymbol, size_t(end - op));
		op = end;
	}

	return uint32_t(op - dstData.Data());
}


}  // namespace

Decompression* DecompressInit() { return new Decompression(); }

void DecompressDeinit(Decompression* decompression)
{
	if (decompression->mDctx)
	{
		ZSTD_freeDCtx(decompression->mDctx);
	}
	delete decompression;
}

size_t DecompressZstd(Decompression& decompression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src)
{
	if (decompression.mDctx == nullptr)
	{
		ZSTD_customMem customMem;
		customMem.customAlloc = &zalloc;
		customMem.customFree = &zfree;
		customMem.opaque = nullptr;
		decompression.mDctx = ZSTD_createDCtx_advanced(customMem);
	}

	size_t oSize = ZSTD_decompressDCtx(decompression.mDctx, dst.Data(), dst.Size(), src.Data(), src.Size());
	if (!ZSTD_isError(oSize))
	{
		return oSize;
	}
	ION_LOG_INFO("ZSTD decompression failed: " << ZSTD_getErrorName(oSize));
	return 0;
}

size_t DecompressFse(Decompression& decompression, ion::ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src)
{
	const byte* op = src.Data();
	size_t srcSize = src.Size();

	size_t oSize =
	  FSE_decompress_wksp_bmi2(dst.Data(), dst.Size(), op, srcSize, TableLogMax, &decompression.mFse, sizeof(decompression.mFse), 0);
	if (FSE_isError(oSize))
	{
		ION_DBG("FSE decompression failed: " << FSE_getErrorName(oSize));
		return 0;
	}
	return oSize;
}

size_t DecompressHuf(Decompression& decompression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src, bool useExistingTable)
{
	// Encoder writes a single-stream HUF frame via HUF_compress1X_repeat. The matching
	// decoder is HUF_decompress1X_DCtx_wksp, which reads the table from the frame header
	// and decompresses in one call. When the encoder reused a previous table, the body
	// has no header and we must use HUF_decompress1X_usingDTable with the cached DTable.
	const int flags = 0;
	if (useExistingTable)
	{
		ION_ASSERT(decompression.mHufDTableValid, "Huf-repeat block without a preceding Huf table");
		size_t s = HUF_decompress1X_usingDTable(dst.Data(), dst.Size(), src.Data(), src.Size(), decompression.hufTable, flags);
		if (HUF_isError(s))
		{
			ION_LOG_INFO("Huf-repeat decompression failed: " << HUF_getErrorName(s) << ";srcSize=" << src.Size());
			return 0;
		}
		return s;
	}
	// First U32 of the DTable encodes its maxTableLog capacity (matching HUF_CREATE_STATIC_DTABLEX2).
	decompression.hufTable[0] = U32(ZSTD_HUFFDTABLE_CAPACITY_LOG) * 0x01000001u;
	size_t s = HUF_decompress1X_DCtx_wksp(decompression.hufTable, dst.Data(), dst.Size(), src.Data(), src.Size(),
										  decompression.mHufWksp, sizeof(decompression.mHufWksp), flags);
	if (HUF_isError(s))
	{
		decompression.mHufDTableValid = false;
		ION_LOG_INFO("Huf decompression failed: " << HUF_getErrorName(s) << ";srcSize=" << src.Size());
		return 0;
	}
	decompression.mHufDTableValid = true;
	return s;
}

size_t Decompress(Decompression& decompression, const CompressedInfo& info, ion::ByteReader& reader, ion::ByteWriter& writer)
{
	size_t decompressedSize = 0;
	reader.ReadDirectly(
	  [&](byte* start, byte* end)
	  {
		  const size_t len = size_t(end - start);

		  size_t sizeBytes = info.mNumSizeBytes;

		  if ((info.mCompressor != Compressor::Bitpack || info.mOptions != 0x7) && sizeBytes <= len)
		  {
			  memcpy(&decompressedSize, start, sizeBytes);
		  }
		  else
		  {
			  // Bitpack 8-bits is always equal to compressed size
			  sizeBytes = 0;
			  decompressedSize = SafeRangeCast<uint32_t>(len);
		  }
		  if (decompressedSize == 0)
		  {
			  return size_t(0);
		  }

		  ION_LOG_COMPRESSION("Decompressing " << info.mCompressedSize << " -> " << decompressedSize << " using " << int(info.mCompressor)
											   << "(" << info.mOptions << ")");

		  if (info.mCompressor == Compressor::RleFse)
		  {
			  Vector<byte> tmp;
			  tmp.ResizeFast(decompressedSize);
			  ion::ArrayView<byte, uint32_t> dst2(tmp.Data(), SafeRangeCast<uint32_t>(tmp.Size()));
			  ion::ArrayView<byte, uint32_t> srcData(start + sizeBytes, SafeRangeCast<uint32_t>(len - sizeBytes));
			  decompressedSize = DecompressFse(decompression, dst2, srcData);
			  CompressedInfo innerInfo;
			  innerInfo.mOptions = (info.mOptions) & ((1 << 3) - 1);
			  innerInfo.mCompressedSize = SafeRangeCast<uint32_t>(tmp.Size());
			  innerInfo.mCompressor = Compressor::Rle;
			  innerInfo.mNumSizeBytes = info.mNumSizeBytes;
			  ByteReader reader2(tmp.Data(), tmp.Size());
			  decompressedSize = Decompress(decompression, innerInfo, reader2, writer);
			  return len;
		  }

		  writer.EnsureCapacity(ion::SafeRangeCast<ion::ByteSizeType>(decompressedSize));
		  writer.WriteDirectly(
			[&](byte* outStart, byte* outEnd)
			{
				ion::ArrayView<byte, uint32_t> srcData(start + sizeBytes, SafeRangeCast<uint32_t>(len - sizeBytes));
				ion::ArrayView<byte, uint32_t> dstData(outStart, SafeRangeCast<uint32_t>(outEnd - outStart));
				ION_ASSERT(dstData.Size() >= decompressedSize, "Out of destination space");

				size_t writeCount = 0;
				switch (info.mCompressor)
				{
				case Compressor::Huf:
				{
					// Huf decoder requires the EXACT decompressed size, not destination capacity.
					// mOptions bit 0 indicates the encoder reused the previous HUF table — the
					// body has no inline table header.
					const bool useExistingTable = (info.mOptions & 1u) != 0u;
					ion::ArrayView<byte, uint32_t> exactDst(outStart, SafeRangeCast<uint32_t>(decompressedSize));
					writeCount = DecompressHuf(decompression, exactDst, srcData, useExistingTable);
					break;
				}
				case Compressor::Fse:
				{
					writeCount = DecompressFse(decompression, dstData, srcData);
					break;
				}
				case Compressor::Rle:
				{
					writeCount = DecompressRLE(dstData, srcData, decompressedSize, (info.mOptions & 0x7) + 1);
					break;
				}
				case Compressor::Bitpack:
				{
					unsigned bitsPerSymbol = (info.mOptions & 0x7) + 1;
					if (bitsPerSymbol == 8)
					{
						memcpy(dstData.Data(), srcData.Data(), srcData.Size());
						writeCount = srcData.Size();
					}
					else
					{
						writeCount = Unbitpack(dstData, srcData, decompressedSize, bitsPerSymbol);
					}
					break;
				}
				default:
					break;
				}
				ION_LOG_COMPRESSION("Decompressed Block from " << srcData.Size() << " to " << writeCount << " bytes using "
															   << info.mCompressor << "(" << info.mOptions << ")");

				if (writeCount != decompressedSize)
				{
					ION_ABNORMAL("Failed to decompress");
				}

				return writeCount;
			});
		  return len;
	  });
	return decompressedSize;
}
}  // namespace compact
}  // namespace ion
