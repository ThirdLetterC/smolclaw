#pragma once

#include "sc/buffer.h"
#include "sc/result.h"

sc_status sc_media_wav_to_ogg_opus(sc_allocator *alloc, sc_buf wav, size_t max_output_bytes, sc_bytes *out);
