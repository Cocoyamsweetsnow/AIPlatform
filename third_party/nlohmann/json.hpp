// 让 `#include <nlohmann/json.hpp>` 命中仓库自带的 third_party/json.hpp。
//
// 背景：代码里统一写的是 <nlohmann/json.hpp>，但仓库把单头文件放在
// third_party/json.hpp。之前能编过，是因为构建机上恰好装了系统包
// nlohmann-json3-dev（版本还不一样），换台机器就编不过。
// 加这个转发头之后，`-I third_party` 就能自给自足，版本也锁定在仓库里。
#pragma once

#include "../json.hpp"
