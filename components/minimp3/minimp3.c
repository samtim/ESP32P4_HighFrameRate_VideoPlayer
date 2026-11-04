/**
 ******************************************************************************
 * @file        minimp3.c
 * @brief       minimp3 的编译单元 (单头文件库, 只在这里展开实现)
 *
 * 来源: https://github.com/lieff/minimp3
 * 许可: CC0 1.0 (公有领域), 见 minimp3.h 文件头
 *
 * MINIMP3_ONLY_MP3 : 只保留 MPEG-1/2 Layer III (绝大多数 .mp3), 去掉 Layer I/II
 *                    以节省 flash
 * 输出为 int16 (未定义 MINIMP3_FLOAT_OUTPUT)
 ******************************************************************************
 */

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3

#include "minimp3.h"
