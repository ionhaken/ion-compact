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

#ifdef XXH_FALLTHROUGH
	#undef XXH_FALLTHROUGH
#endif
#include "zstd/lib/zstd.h"
#define FSE_STATIC_LINKING_ONLY
#include "zstd/lib/common/huf.h"

namespace ion
{

namespace compact
{
constexpr unsigned TableLogMax = 11;
constexpr unsigned MaxSymbol = 255;

#define ZSTD_HUFFDTABLE_CAPACITY_LOG 12
struct Decompression
{
	U32 mFse[FSE_DECOMPRESS_WKSP_SIZE_U32(TableLogMax, 255)];
	ZSTD_DCtx_s* mDctx = nullptr;

	HUF_DTable hufTable[HUF_DTABLE_SIZE(ZSTD_HUFFDTABLE_CAPACITY_LOG)];

	// Workspace for HUF_decompress1X_DCtx_wksp — must be >= HUF_DECOMPRESS_WORKSPACE_SIZE bytes.
	U32 mHufWksp[HUF_DECOMPRESS_WORKSPACE_SIZE_U32];

	// True when hufTable holds a usable DTable from a previous Huf decompress, so a
	// "Huf-with-repeat" block (no inline table header) can reuse it.
	bool mHufDTableValid = false;
};
}  // namespace compact
}  // namespace ion
