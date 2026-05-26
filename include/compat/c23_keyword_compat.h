#pragma once

#if defined(__clang__) && !defined(__cplusplus)
#if defined(__is_identifier)
#if __is_identifier(constexpr)
#define constexpr const
#endif
#else
#ifndef constexpr
#define constexpr const
#endif
#endif
#endif
