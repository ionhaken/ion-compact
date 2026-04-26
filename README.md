# ion-compact

**User-guided entropy coding.** The caller shapes the data - with deltas and per-channel substreams - and the library encodes the resulting bytes assuming independent and identically distributed (i.i.d.) No hidden modeling, no surprises.

Built on ZSTD's FSE / HUF / ZSTD primitives, plus an RLE pre-pass and a multi-substream framing layer (`CompactWriter` / `CompactReader`).

**Released as-is, no support, no roadmap, no API stability guarantees.** 

## Is this for you? Probably not.

This is a niche library. For most workloads, ZSTD or gzip is the right answer: easier to integrate, well-tested, fast enough, and the marginal compression win from a hand-tuned codec is dwarfed by the cost of building one. AWS egress is around $0.05-0.09/GB; saving 30% on a TB/month is $15-25, which doesn't pay for an engineer-week of integration.

The library is worth a look only if your problem looks like one of these:

- **Real-time multiplayer at MMO / battle-royale scale.** Per-snapshot bytes multiply through frequency and concurrent users: 200 B/snapshot * 30 Hz * 100k CCU is ~18 GB/s aggregate egress, which is real money at cloud prices. A 30% codec win on that is on the order of $1M/year. If you're at this scale, codec engineering is on your roadmap.
- **Mobile / cellular networks** where data caps and radio-on time (battery) are budget constraints. Sparse update protocols help more than generic compression here, but tighter snapshot encoding still moves the needle when you're shipping deltas every tick.
- **Satellite, LEO, IoT, telemetry over radio** - links where bytes have a per-byte dollar cost and CPU is fine. Niche but real (telemetry from oil rigs, swarms of small sats, low-bandwidth ground links).
- **Esports / scientific replay storage at scale.** Multi-million game archives (chess, dota, RTS replays, market-tick recordings) where storage cost compounds and ratios matter more than throughput.

Across these, the pattern is: someone has already measured, the ZSTD baseline isn't tight enough, and 20-40% additional savings would actually justify owning the encoding stack. If you can't picture that meeting at your project, the integration cost almost certainly outweighs any savings.

What this library specifically *adds* over rolling your own on top of [FiniteStateEntropy](https://github.com/Cyan4973/FiniteStateEntropy):

- A multi-substream framing layer so you can route different field types to different entropy coders cheaply.
- Caller-driven Brotli-style context modeling (`LSB6`, `MSB6`, `Signed`, magnitude buckets) as a per-call template parameter, on top of FSE / HUF directly without an LZ stage.
- Auto-selection between FSE / HUF / RLE / bitpack per substream by skew / size / alphabet heuristic.

These are useful design points if you're in one of the niches above. Outside of them, the right answer is usually ZSTD with a good dictionary.

## Design philosophy

**This is a low-level codec toolkit, not a smart compressor.**

The library's contract is: *"give me bytes, I will compress them as well as possible, treating them as i.i.d."* Modeling the structure of the data - choosing what bytes to feed in, in what order, with what pre-processing - is the **caller's** responsibility, exposed through utilities they opt into:

- **Delta encoding** - `CompressionUtil::UpdateDeltaForward` / `UpdateDeltaBackward`. Use when consecutive values are correlated.
- **Variable-byte integer encoding** - `WriteVariableBytes`, `ReadVariableBytes`. Use when integer magnitudes vary by orders of magnitude.
- **Per-channel substream split** - `CompactWriter` tags. Use when different *kinds* of data live in the same blob and have different distributions.

These transforms move the data closer to the i.i.d. assumption that FSE / HUF make. **Picking them is a modeling decision the user owns**, because only the user knows what their data represents.

### Why not auto-detect or build-in pre-processing?

Auto pre-processing would mean the library second-guessing the user's representation. If the user wrote raw bytes to one stream because the bytes really are independent, an automatic delta or context model just adds table overhead for no gain. If the user has temporal structure, the right answer is for them to delta-encode first or split into more substreams - both of which are already supported as primitives.

### What the library *does* decide automatically

The library picks between **alternatives that share the same model assumption** - choices that have no user-facing implication beyond size and speed:

- **HUF vs FSE** for entropy coding (same i.i.d. model, different bit-rounding tradeoff). See the heuristic in `CompressionSystem.cpp::CompressInternal`.
- **RLE pre-pass** when the input clearly has long runs.
- **HUF table reuse** across consecutive `CompressHuf` calls on the same `Compression` (transparent - same compressed output meaning).
- **Bitpack fallback** when the entropy coders can't compress.

That's the line. Anything that changes the *model* of the data - context models, predictors, bit-plane splits, dictionary training - belongs in caller code or in a separate higher-level library that uses these primitives.

## What's in the library

### Codecs (all i.i.d. assumption)

| API                         | Backend                          | When used                                                  |
|-----------------------------|----------------------------------|------------------------------------------------------------|
| `CompressZstd`              | full ZSTD frame (LZ + entropy)   | direct call only                                           |
| `CompressFse` / `Decompress`| FSE (tANS)                       | auto-picked for near-uniform / tiny-alphabet / tiny inputs |
| `CompressHuf` / `Decompress`| HUF (single-stream, table-reuse) | auto-picked for skewed alphabets (skew >= 8)                |
| `CompressRLE` (internal)    | run-length, optional inner FSE   | auto-picked for high-repeat inputs                         |
| `Bitpack` (internal)        | fixed-width bit packing          | fallback when entropy coding doesn't beat raw width        |

### Framing

- **`Compress` / `Decompress`** - single-buffer entry points; the auto-selector lives in `CompressInternal`.
- **`CompactWriter` / `CompactReader`** - multi-substream framing with per-stream entropy coding. Tags (`TagSignificantByte`, `TagByteCount`, `TagNoCompression`) route bytes to substreams.

### Pre-processing utilities (user-driven)

- **`CompressionUtil.h`** - variable-byte ints, deltas, float (de)serialization with delta tracking.

### Context-aware variable-byte (opt-in)

`WriteVariableBytes` / `ReadVariableBytes` / `WriteFloat` / `ReadFloat` have overloads that take a `ContextState<Ctx>&`. The caller picks a `Ctx` strategy at compile time; the byte-count and significant-byte fields are routed to per-bucket substreams chosen from the previous value, so each bucket gets its own entropy table.

This is the i.i.d.-respecting way for the caller to recover conditional information without the library second-guessing the data: you choose the conditioning function, you size the framing accordingly, and the compressed wire format follows from `Ctx::NumContexts`.

Stock contexts in `ion::compact::context`:

| Strategy           | Buckets | When to use                                                |
|--------------------|---------|------------------------------------------------------------|
| `None`             | 1       | Default; identical wire format to non-context API.         |
| `ZeroVsNonzero`    | 2       | Sparse value streams.                                      |
| `PrevMagnitude4`   | 4       | Streams where small values cluster and large values cluster (delta-encoded ints, sparse-vs-dense regions). |
| `PrevMagnitude8`   | 8       | Wider dynamic range with same intuition as PrevMagnitude4. |
| `LSB6` / `MSB6`    | 64      | Brotli-style: condition on low / high 6 bits of previous byte. |
| `Signed`           | 2       | Split positive vs negative previous values (signed deltas). |

Caller writes its own by providing `static constexpr size_t NumContexts` and `static constexpr size_t Compute(uint64_t prev)`. Use `ContextState<Ctx>::NumStreams` to size the `CompactWriter` / `CompactReader`. Same `Ctx` must be used on read.

## Status

### Working / tested

- FSE encode + decode (round-trip).
- HUF encode + decode (round-trip), including table reuse across calls.
- ZSTD encode + decode (round-trip).
- RLE encode + decode, including the RLE+FSE recursive wrapping.
- Bitpack fallback for incompressible inputs.
- `CompactWriter` / `CompactReader` framing including the multi-byte flag-byte path (`numStreams >= 5`).

Test coverage: 21 tests in `test/tests.cpp` (direct round-trip fuzz, multi-substream framing, context-aware varbyte, delta strategies, Huffman path / table reuse, RLE, stress). Build and run as described in [Build](#build).

### Known issues

- The format is host-endian (size headers written via raw `memcpy`). Compressed blobs do not round-trip across endian boundaries.

## Contributing

When in doubt about whether a change belongs here, apply the philosophy above:

**In scope:**

- Within-codec optimizations that don't change semantics.
- Selecting between codecs that share the same model assumption (the HUF/FSE heuristic).
- Faster impls of existing codecs.

**Out of scope:**

- Auto delta encoding, predictor selection, bit-plane decomposition, dictionary training - these are user-side modeling decisions. Add to `CompressionUtil` as primitives if you want to make them easier, but don't bake them into the codec auto-select.
- Order-N context modeling baked into the entropy stage - same reason; the user already has the substream-split lever for inter-channel context, and can do per-tag modeling on top of these primitives.

## Build

Self-contained CMake build. Vendored ZSTD lives under `depend/zstd`; no other external dependencies.

``` 
mkdir build && cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

This produces:

- `build/Release/ion-compact.lib` - the static library.
- `build/Release/ion-compact-tests.exe` - the standalone test runner (21 tests: direct round-trip fuzz, multi-substream framing, context-aware varbyte, delta strategies, Huffman selection / table reuse, RLE, stress). No external test-framework dependency; the runner lives in `test/test_helpers.h`.

Run it; should print `21 passed, 0 failed`. Disable with `-DION_COMPACT_BUILD_TESTS=OFF`.

## Source layout

```
src/ion/
  CoreCommon.h     # codec utilities
  compact/         # the codec sources
```

## License

Apache License 2.0. See `LICENSE`.
