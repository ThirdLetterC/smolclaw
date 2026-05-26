#pragma once

#define ckd_add(result, left, right) __builtin_add_overflow((left), (right), (result))
#define ckd_sub(result, left, right) __builtin_sub_overflow((left), (right), (result))
#define ckd_mul(result, left, right) __builtin_mul_overflow((left), (right), (result))
