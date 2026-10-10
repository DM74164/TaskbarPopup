#ifndef TP_VERSION_H
#define TP_VERSION_H

// 默认版本号；build-mingw.sh 可以用 TP_VERSION=1.2.3 覆盖（自动构建按 v1.2.3 标签设置）
#ifndef TP_VERSION_MAJOR
#define TP_VERSION_MAJOR 1
#define TP_VERSION_MINOR 0
#define TP_VERSION_PATCH 3
#endif

#define TP_STR2(x) #x
#define TP_STR(x) TP_STR2(x)
#define TP_VERSION_STR TP_STR(TP_VERSION_MAJOR.TP_VERSION_MINOR.TP_VERSION_PATCH)

#endif
