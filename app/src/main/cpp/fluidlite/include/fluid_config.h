#ifndef FLUID_CONFIG_H
#define FLUID_CONFIG_H

// 1. Android 基础环境
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_MATH_H 1
#define HAVE_FLOAT_H 1
#define HAVE_STDARG_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_LIMITS_H 1

// 2. 禁用不需要的依赖 (防止报 vorbis/lash 错误)
#undef HAVE_LIBVORBIS
#undef FLUID_DA_SUPPORT_OGG
#undef HAVE_LASH
#undef FLUIDLITE_HAVE_LASH
#undef HAVE_LIBREADLINE

// 3. 核心功能开关
#define FLUID_SAMPLE_FLOAT 1  // 开启高质量浮点运算
#define WITH_FLOAT 1

// 4. 数据类型定义
#define SIZEOF_INT 4
#define SIZEOF_LONG 8
#define SIZEOF_LONG_LONG 8
#define SIZEOF_FLOAT 4
#define SIZEOF_DOUBLE 8

#define inline __inline

#endif /* FLUID_CONFIG_H */