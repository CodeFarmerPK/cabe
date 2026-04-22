/*
* Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 */

// Cabe 存储引擎公开 API 的总入口。
// 调用方只需 #include "cabe/cabe.h" 即可获得全部公开类型。
//
// 各头文件可按需单独引入：
//   cabe/status.h  — Status 错误类型
//   cabe/options.h — Options / ReadOptions / WriteOptions
//   cabe/engine.h  — Engine 存储引擎主类

#pragma once

#include "cabe/engine.h"
#include "cabe/options.h"
#include "cabe/status.h"
