#pragma once
// Two-lane double vector for stereo processing (L, R). clang/gcc vector extension.
namespace lkit {
using d2 = double __attribute__((ext_vector_type(2)));
} // namespace lkit
