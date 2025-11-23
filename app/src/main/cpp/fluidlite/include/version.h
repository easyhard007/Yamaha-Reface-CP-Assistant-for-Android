#ifndef _FLUIDLITE_VERSION_H
#define _FLUIDLITE_VERSION_H

// 定义 API 宏，防止编译时出现 "expected identifier" 错误
#ifndef FLUIDSYNTH_API
#define FLUIDSYNTH_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 这里硬编码版本号，骗过编译器
#define FLUIDLITE_VERSION       "1.2.1"
#define FLUIDLITE_VERSION_MAJOR 1
#define FLUIDLITE_VERSION_MINOR 2
#define FLUIDLITE_VERSION_MICRO 1

FLUIDSYNTH_API void fluid_version(int *major, int *minor, int *micro);
FLUIDSYNTH_API char* fluid_version_str(void);

#ifdef __cplusplus
}
#endif

#endif /* _FLUIDLITE_VERSION_H */