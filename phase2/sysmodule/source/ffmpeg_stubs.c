// SPDX-License-Identifier: MIT
// Eliminate unused FFmpeg tx_double.o and tx_int32.o codelet lists to save ~16 MB of BSS
// in the background sysmodule while keeping real av_tx_init (float MDCT/FFT + ARM64 NEON)
// intact for Opus, AAC, and Vorbis audio decoding.

#include <stddef.h>
#include <stdint.h>

const void * const ff_tx_codelet_list_double_c[] = { NULL };
const void * const ff_tx_codelet_list_int32_c[] = { NULL };
