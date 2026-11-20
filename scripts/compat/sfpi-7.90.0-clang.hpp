#pragma once
// Analysis only. SFPI 7.90.0's non-Tensix fallback omits this type.
#ifdef __clang__
using __xtt_vector8 = unsigned __attribute__((vector_size(sizeof(unsigned) * 32 * 8)));
#endif
