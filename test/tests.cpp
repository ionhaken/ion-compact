// Standalone test runner for ion-compact. No Catch2 dependency.
#include <ion/compact/CompactReader.h>
#include <ion/compact/CompactWriter.h>
#include <ion/compact/CompressionSystem.h>
#include <ion/compact/CompressionUtil.h>
#include <ion/compact/DecompressionSystem.h>

#include "test_helpers.h"

#include <cstring>

#define INFO(__expr) ION_LOG_INFO(__expr)

using ion::byte;

// ---------------------------------------------------------------------------
// CompactWriter / CompactReader round-trips (from CompactTest.cpp)
// ---------------------------------------------------------------------------

TEST(CompactTest_NoCompression)
{
	ion::ByteBuffer<> outBuffer(64);

	const uint64_t FirstSymbol = 0x1122334455667788ULL;
	{
		ion::compact::CompactWriter<> writer(outBuffer, 64);
		writer.Write(FirstSymbol, ion::compact::TagNoCompression);
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}
	REQUIRE(outBuffer.Size() == 9);

	uint64_t roundTrip = 0;
	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		reader.Read(roundTrip, ion::compact::TagNoCompression);
		ion::compact::DecompressDeinit(decompression);
	}
	REQUIRE(roundTrip == FirstSymbol);
}

TEST(CompactTest_Short)
{
	ion::ByteBuffer<> outBuffer(64);
	const uint64_t FirstSymbol = 12;
	{
		ion::compact::CompactWriter<> writer(outBuffer, 64);
		ion::compact::WriteVariableBytes(writer, FirstSymbol);
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}
	REQUIRE(outBuffer.Size() == 7);

	uint64_t roundTrip = 0;
	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		ion::compact::ReadVariableBytes(reader, roundTrip);
		ion::compact::DecompressDeinit(decompression);
	}
	REQUIRE(roundTrip == FirstSymbol);
}

TEST(CompactTest_SingleSymbolZero)
{
	ion::ByteBuffer<> outBuffer(64);
	const uint64_t FirstSymbol = 0;
	{
		ion::compact::CompactWriter<> writer(outBuffer, 64);
		writer.Write(FirstSymbol, ion::compact::TagSignificantByte);
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}
	REQUIRE(outBuffer.Size() == 4);

	uint64_t roundTrip = 0;
	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		reader.Read(roundTrip, ion::compact::TagSignificantByte);
		ion::compact::DecompressDeinit(decompression);
	}
	REQUIRE(roundTrip == FirstSymbol);
}

TEST(CompactTest_SingleSymbolOne)
{
	ion::ByteBuffer<> outBuffer(64);
	const uint64_t FirstSymbol = 0xFFFFFFFFFFFFFFFFULL;
	{
		ion::compact::CompactWriter<> writer(outBuffer, 64);
		writer.Write(FirstSymbol, ion::compact::TagSignificantByte);
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}
	REQUIRE(outBuffer.Size() == 5);

	uint64_t roundTrip = 0;
	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		reader.Read(roundTrip, ion::compact::TagSignificantByte);
		ion::compact::DecompressDeinit(decompression);
	}
	REQUIRE(roundTrip == FirstSymbol);
}

TEST(CompactTest_RLE)
{
	const size_t repeat = 2;
	for (size_t i = 0; i < 256; ++i)
	{
		const size_t maxSymbol = i;
		ion::ByteBuffer<> outBuffer(uint32_t(repeat * (maxSymbol + 4) * 2));
		{
			ion::compact::CompactWriter<> writer(outBuffer, 1024);
			for (size_t k = 0; k < (maxSymbol + 4) * 2; ++k)
			{
				const uint8_t val = uint8_t(k % (maxSymbol + 1));
				for (size_t j = 0; j < repeat; ++j)
				{
					writer.Write(val, ion::compact::TagSignificantByte);
				}
			}
			auto* compression = ion::compact::CompressionInit();
			writer.CompressFromSubstreams(*compression);
			ion::compact::CompressionDeinit(compression);
		}
		{
			ion::ByteReader byteReader(outBuffer);
			ion::compact::CompactReader<> reader(byteReader);
			auto* decompression = ion::compact::DecompressInit();
			reader.DecompressToSubstreams(*decompression);
			for (size_t k = 0; k < (maxSymbol + 4) * 2; ++k)
			{
				for (size_t j = 0; j < repeat; ++j)
				{
					uint8_t val = 0;
					reader.Read(val, ion::compact::TagSignificantByte);
					REQUIRE(val == uint8_t(k % (maxSymbol + 1)));
				}
			}
			ion::compact::DecompressDeinit(decompression);
		}
	}
}

TEST(CompactTest_Floats)
{
	constexpr size_t numFloats = 4096;
	ion::ByteBuffer<> outBuffer(numFloats * 8);
	const size_t UncompressedBandwidth = numFloats * sizeof(float);
	{
		ion::compact::CompactWriter<> writer(outBuffer, numFloats * 4);
		uint32_t delta = 0;
		ion::Random r(123);
		for (size_t i = 0; i < numFloats; ++i)
		{
			float f = r.GetFastFloat() * 100.0f;
			ion::compact::WriteFloat(writer, f, delta);
		}
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}
	INFO("Delta float compression ratio: " << (float(outBuffer.Size()) / float(UncompressedBandwidth)));
	REQUIRE(outBuffer.Size() < float(UncompressedBandwidth) * 0.95f);

	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader);
		auto* decompression = ion::compact::DecompressInit();
		uint32_t delta = 0;
		reader.DecompressToSubstreams(*decompression);
		ion::Random r(123);
		for (size_t i = 0; i < numFloats; ++i)
		{
			float out = 0.0f;
			REQUIRE(ion::compact::ReadFloat(reader, out, delta));
			float expected = r.GetFastFloat() * 100.0f;
			REQUIRE(expected == out);
		}
		ion::compact::DecompressDeinit(decompression);
	}
}

TEST(CompactTest_ContextAwareRoundTrip)
{
	using Ctx = ion::compact::context::PrevMagnitude4;
	using State = ion::compact::ContextState<Ctx>;
	constexpr size_t kStreams = State::NumStreams;	// 1 + 4*2 = 9

	const uint32_t kValues[] = {0u, 0u, 1u, 2u, 1u, 0u, 100u, 105u, 110u, 50000u, 50001u, 7u, 8u, 65535u};
	const size_t kCount = sizeof(kValues) / sizeof(kValues[0]);

	ion::ByteBuffer<> outBuffer(1024);
	{
		ion::compact::CompactWriter<> writer(outBuffer, 256, kStreams);
		State state;
		for (size_t i = 0; i < kCount; ++i)
		{
			ion::compact::WriteVariableBytes(writer, kValues[i], state);
		}
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}

	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader, kStreams);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		State state;
		for (size_t i = 0; i < kCount; ++i)
		{
			uint32_t out = 0;
			REQUIRE(ion::compact::ReadVariableBytes(reader, out, state));
			REQUIRE(out == kValues[i]);
		}
		ion::compact::DecompressDeinit(decompression);
	}
}

TEST(CompactTest_DeltaStrategiesRoundTrip)
{
	const uint32_t kValues[] = {0u, 1u, 100u, 99u, 105u, 7u, 1000u, 999u, 12345u, 0u, 100000u};
	const size_t kCount = sizeof(kValues) / sizeof(kValues[0]);

	auto roundTrip = [&](auto strategyTag)
	{
		using Strategy = decltype(strategyTag);
		uint32_t accu = 0;
		ion::Vector<uint32_t> encoded;
		encoded.Reserve(kCount);
		for (size_t i = 0; i < kCount; ++i)
		{
			encoded.Add(ion::compact::UpdateDeltaForward<Strategy>(kValues[i], accu));
		}
		uint32_t backAccu = 0;
		for (size_t i = 0; i < kCount; ++i)
		{
			const uint32_t decoded = ion::compact::UpdateDeltaBackward<Strategy>(encoded[i], backAccu);
			REQUIRE(decoded == kValues[i]);
		}
	};

	roundTrip(ion::compact::delta::None{});
	roundTrip(ion::compact::delta::SignedDelta{});
	roundTrip(ion::compact::delta::Xor{});
	roundTrip(ion::compact::delta::ZigZag{});
}

TEST(CompactTest_ContextAwareImprovesAutoCorrelatedRatio)
{
	ion::Vector<uint32_t> values;
	ion::Random r(42);
	while (values.Size() < 2048)
	{
		const bool small = (r.UInt32() & 1u) != 0u;
		const uint32_t runLen = 12u + (r.UInt32() % 12u);
		for (uint32_t i = 0; i < runLen && values.Size() < 2048; ++i)
		{
			values.Add(small ? (r.UInt32() & 0xFu) : (0x10000u | (r.UInt32() & 0xFFFFu)));
		}
	}

	auto measure = [&](auto stateProto, size_t numStreams)
	{
		using State = decltype(stateProto);
		ion::ByteBuffer<> outBuffer(uint32_t(values.Size() * 8));
		{
			ion::compact::CompactWriter<> writer(outBuffer, 2048, numStreams);
			State state;
			for (size_t i = 0; i < values.Size(); ++i)
			{
				ion::compact::WriteVariableBytes(writer, values[i], state);
			}
			auto* compression = ion::compact::CompressionInit();
			writer.CompressFromSubstreams(*compression);
			ion::compact::CompressionDeinit(compression);
		}
		return size_t(outBuffer.Size());
	};

	using NoneState = ion::compact::ContextState<ion::compact::context::None>;
	using PM4State = ion::compact::ContextState<ion::compact::context::PrevMagnitude4>;
	const size_t sizeNone = measure(NoneState{}, NoneState::NumStreams);
	const size_t sizePM4 = measure(PM4State{}, PM4State::NumStreams);

	INFO("ContextAware varbyte: None=" << sizeNone << "B  PrevMagnitude4=" << sizePM4 << "B  ratio="
									   << (float(sizePM4) / float(sizeNone)));
	REQUIRE(sizePM4 < sizeNone);
}

TEST(CompactTest_ManySubstreams)
{
	constexpr size_t kNumStreams = 6;	// 1 entry stream + 5 substreams
	ion::ByteBuffer<> outBuffer(1024);

	ion::Vector<uint64_t> values;
	for (size_t i = 0; i < kNumStreams - 1; ++i)
	{
		values.Add(uint64_t(0x1100000000ULL + i * 0x101010101ULL));
	}

	{
		ion::compact::CompactWriter<> writer(outBuffer, 256, kNumStreams);
		for (size_t i = 0; i < values.Size(); ++i)
		{
			writer.Write(values[i], ion::serialization::Tag{i + 1});
		}
		auto* compression = ion::compact::CompressionInit();
		writer.CompressFromSubstreams(*compression);
		ion::compact::CompressionDeinit(compression);
	}

	{
		ion::ByteReader byteReader(outBuffer);
		ion::compact::CompactReader<> reader(byteReader, kNumStreams);
		auto* decompression = ion::compact::DecompressInit();
		reader.DecompressToSubstreams(*decompression);
		for (size_t i = 0; i < values.Size(); ++i)
		{
			uint64_t out = 0;
			reader.Read(out, ion::serialization::Tag{i + 1});
			REQUIRE(out == values[i]);
		}
		ion::compact::DecompressDeinit(decompression);
	}
}

TEST(CompactTest_Stress)
{
	for (int j = 1; j < 1024; j = j + 16)
	{
		ion::ByteBuffer<> outBuffer(uint32_t(j * sizeof(uint32_t) * 8));
		{
			ion::compact::CompactWriter<> writer(outBuffer, j * sizeof(uint32_t) * 4);
			ion::Random r(123);
			for (int i = 0; i < j; ++i)
			{
				const uint32_t value = r.UInt32();
				const uint32_t loop = r.UInt32();
				for (uint32_t k = 0; k < (loop % 8u) + 1u; ++k)
				{
					ion::compact::WriteVariableBytes(writer, value);
				}
			}
			auto* compression = ion::compact::CompressionInit();
			writer.CompressFromSubstreams(*compression);
			ion::compact::CompressionDeinit(compression);
		}

		{
			ion::ByteReader byteReader(outBuffer);
			ion::compact::CompactReader<> reader(byteReader);
			auto* decompression = ion::compact::DecompressInit();
			reader.DecompressToSubstreams(*decompression);
			ion::Random r(123);
			for (int i = 0; i < j; ++i)
			{
				const uint32_t value = r.UInt32();
				const uint32_t loop = r.UInt32();
				uint32_t out = 0;
				for (uint32_t k = 0; k < (loop % 8u) + 1u; ++k)
				{
					ion::compact::ReadVariableBytes(reader, out);
				}
				REQUIRE(value == out);
			}
			ion::compact::DecompressDeinit(decompression);
		}
	}
}

// ---------------------------------------------------------------------------
// Direct Compress / Decompress round-trips (from CompressionDirectTest.cpp)
// ---------------------------------------------------------------------------
namespace
{
ion::compact::CompressedInfo CompressOnce(const ion::Vector<byte>& src, ion::Vector<byte>& compressed)
{
	compressed.Resize(ion::compact::DestCapacityZstd(src.Size()) + 256);
	auto* compression = ion::compact::CompressionInit();
	ion::ArrayView<byte, uint32_t> dstView(compressed.Data(), uint32_t(compressed.Size()));
	const ion::ArrayView<byte, uint32_t> srcView(const_cast<byte*>(src.Data()), uint32_t(src.Size()));
	auto info = ion::compact::Compress(*compression, dstView, srcView);
	ion::compact::CompressionDeinit(compression);
	return info;
}

void RoundTrip(const ion::Vector<byte>& src, ion::compact::CompressedInfo* outInfo = nullptr)
{
	REQUIRE(src.Size() > 0);
	REQUIRE(src.Size() <= 0xFFFFFF);

	ion::Vector<byte> compressed;
	auto info = CompressOnce(src, compressed);
	REQUIRE(info.mCompressedSize > 0);

	ion::ByteBuffer<> outBuffer(uint32_t(src.Size() + 64));
	{
		ion::ByteReader reader(compressed.Data(), info.mCompressedSize);
		ion::ByteWriter writer(outBuffer);
		auto* decompression = ion::compact::DecompressInit();
		const size_t produced = ion::compact::Decompress(*decompression, info, reader, writer);
		ion::compact::DecompressDeinit(decompression);
		REQUIRE(produced == src.Size());
	}
	REQUIRE(outBuffer.Size() == src.Size());
	REQUIRE(std::memcmp(outBuffer.Begin(), src.Data(), src.Size()) == 0);
	if (outInfo)
	{
		*outInfo = info;
	}
}

ion::Vector<byte> MakeRandom(size_t size, unsigned maxSymbol, uint32_t seed)
{
	ion::Random r(seed);
	ion::Vector<byte> v;
	v.Resize(size);
	for (size_t i = 0; i < size; ++i)
	{
		v[i] = byte(r.UInt32() % (maxSymbol + 1));
	}
	return v;
}

ion::Vector<byte> MakeRunLength(size_t size, unsigned maxSymbol, uint32_t seed, unsigned avgRun)
{
	ion::Random r(seed);
	ion::Vector<byte> v;
	v.Reserve(size);
	while (v.Size() < size)
	{
		const byte symbol = byte(r.UInt32() % (maxSymbol + 1));
		const size_t run = (r.UInt32() % (avgRun * 2u)) + 1u;
		for (size_t i = 0; i < run && v.Size() < size; ++i)
		{
			v.Add(symbol);
		}
	}
	return v;
}
}  // namespace

TEST(CompressDirect_SingleSymbolBuffer)
{
	for (unsigned symbol : {0u, 1u, 7u, 127u, 200u, 255u})
	{
		ion::Vector<byte> v;
		v.Resize(257);
		for (size_t i = 0; i < v.Size(); ++i)
		{
			v[i] = byte(symbol);
		}
		RoundTrip(v);
	}
}

TEST(CompressDirect_AlternatingTwoSymbols)
{
	ion::Vector<byte> v;
	v.Resize(1024);
	for (size_t i = 0; i < v.Size(); ++i)
	{
		v[i] = byte(i & 1);
	}
	RoundTrip(v);
}

TEST(CompressDirect_AllZerosVariousSizes)
{
	for (size_t size : {1u, 2u, 7u, 16u, 257u, 4096u, 65537u})
	{
		ion::Vector<byte> v;
		v.Resize(size);
		for (size_t i = 0; i < v.Size(); ++i)
		{
			v[i] = 0;
		}
		RoundTrip(v);
	}
}

TEST(CompressDirect_FuzzRandom)
{
	for (uint32_t seed : {1u, 7u, 17u, 42u, 123u, 999u})
	{
		for (unsigned maxSym : {3u, 15u, 31u, 127u, 200u, 255u})
		{
			for (size_t size : {17u, 256u, 1024u, 8192u})
			{
				auto v = MakeRandom(size, maxSym, seed * (maxSym + 1u) + uint32_t(size));
				RoundTrip(v);
			}
		}
	}
}

TEST(CompressDirect_FuzzRunLength)
{
	for (uint32_t seed : {1u, 7u, 17u, 42u})
	{
		for (unsigned maxSym : {3u, 15u, 31u, 127u})
		{
			for (unsigned run : {2u, 4u, 8u, 32u})
			{
				auto v = MakeRunLength(2048, maxSym, seed, run);
				RoundTrip(v);
			}
		}
	}
}

TEST(CompressDirect_FirstByteZero)
{
	ion::Vector<byte> v;
	v.Resize(512);
	ion::Random r(42);
	v[0] = 0;
	for (size_t i = 1; i < v.Size(); ++i)
	{
		v[i] = byte(r.UInt32() % 200u);
	}
	RoundTrip(v);
}

TEST(CompressDirect_HuffmanPathRoundTrip)
{
	for (uint32_t seed : {1u, 7u, 42u, 123u, 999u})
	{
		for (unsigned maxSym : {15u, 31u, 63u, 127u})
		{
			ion::Random r(seed * (maxSym + 1u));
			ion::Vector<byte> v;
			v.Resize(2048);
			for (size_t i = 0; i < v.Size(); ++i)
			{
				v[i] = byte(r.UInt32() % (maxSym + 1u));
			}
			RoundTrip(v);
		}
	}
}

TEST(CompressDirect_HuffmanTableReuseRoundTrip)
{
	auto* compression = ion::compact::CompressionInit();

	ion::Random r(1234);
	ion::Vector<byte> a;
	a.Resize(4096);
	for (size_t i = 0; i < a.Size(); ++i)
	{
		const uint32_t x = r.UInt32() & 0x7Fu;
		const uint32_t y = r.UInt32() & 0x7Fu;
		a[i] = byte(x & y);
	}
	ion::Vector<byte> b = a;	// identical content forces HUF reuse to fire

	ion::Vector<byte> dstA;
	dstA.Resize(ion::compact::DestCapacityZstd(a.Size()) + 256);
	ion::Vector<byte> dstB;
	dstB.Resize(ion::compact::DestCapacityZstd(b.Size()) + 256);

	ion::compact::CompressedInfo infoA;
	ion::compact::CompressedInfo infoB;
	{
		ion::ArrayView<byte, uint32_t> dstView(dstA.Data(), uint32_t(dstA.Size()));
		const ion::ArrayView<byte, uint32_t> srcView(const_cast<byte*>(a.Data()), uint32_t(a.Size()));
		infoA = ion::compact::Compress(*compression, dstView, srcView);
	}
	{
		ion::ArrayView<byte, uint32_t> dstView(dstB.Data(), uint32_t(dstB.Size()));
		const ion::ArrayView<byte, uint32_t> srcView(const_cast<byte*>(b.Data()), uint32_t(b.Size()));
		infoB = ion::compact::Compress(*compression, dstView, srcView);
	}
	ion::compact::CompressionDeinit(compression);

	REQUIRE(infoA.mCompressor == ion::compact::Compressor::Huf);
	REQUIRE(infoB.mCompressor == ion::compact::Compressor::Huf);
	REQUIRE((infoA.mOptions & 1u) == 0u);
	REQUIRE((infoB.mOptions & 1u) == 1u);
	REQUIRE(infoB.mCompressedSize < infoA.mCompressedSize);

	auto* decompression = ion::compact::DecompressInit();
	ion::ByteBuffer<> outA(uint32_t(a.Size() + 64));
	{
		ion::ByteReader reader(dstA.Data(), infoA.mCompressedSize);
		ion::ByteWriter writer(outA);
		REQUIRE(ion::compact::Decompress(*decompression, infoA, reader, writer) == a.Size());
	}
	ion::ByteBuffer<> outB(uint32_t(b.Size() + 64));
	{
		ion::ByteReader reader(dstB.Data(), infoB.mCompressedSize);
		ion::ByteWriter writer(outB);
		REQUIRE(ion::compact::Decompress(*decompression, infoB, reader, writer) == b.Size());
	}
	ion::compact::DecompressDeinit(decompression);

	REQUIRE(std::memcmp(outA.Begin(), a.Data(), a.Size()) == 0);
	REQUIRE(std::memcmp(outB.Begin(), b.Data(), b.Size()) == 0);
}

TEST(CompressDirect_HuffmanSelected)
{
	ion::Random r(1234);
	ion::Vector<byte> v;
	v.Resize(4096);
	for (size_t i = 0; i < v.Size(); ++i)
	{
		const uint32_t x = r.UInt32() & 0x7Fu;
		const uint32_t y = r.UInt32() & 0x7Fu;
		v[i] = byte(x & y);
	}
	ion::compact::CompressedInfo info;
	RoundTrip(v, &info);
	INFO("Selected compressor: " << int(info.mCompressor));
	REQUIRE(info.mCompressor == ion::compact::Compressor::Huf);
}

TEST(CompressDirect_HighRepeatLowSymbols)
{
	ion::Vector<byte> v;
	v.Reserve(4096);
	while (v.Size() < 4096)
	{
		const byte symbol = byte((v.Size() / 32) & 1);
		v.Add(symbol);
	}
	ion::compact::CompressedInfo info;
	RoundTrip(v, &info);
	REQUIRE(info.mCompressedSize < v.Size() / 8u + 16u);
}

int main()
{
	return ::test::RunAll();
}
