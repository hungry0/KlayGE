#ifndef _CONFIG_HPP
#define _CONFIG_HPP

#if defined(DEBUG) | defined(_DEBUG)
#define KLAYGE_DEBUG
#endif

#if _MSC_VER >= 1400
#define _VC_8_0
#elif _MSC_VER >= 1310
#define _VC_7_1
#endif

// 定义各种编译期选项
#define _SELECT1ST2ND_SUPPORT
#define _COPYIF_SUPPORT

// 定义本地的endian方式
#define _LITTLE_ENDIAN

// 关闭windows.h的min/max
#define NOMINMAX

#endif		// _CONFIG_HPP
