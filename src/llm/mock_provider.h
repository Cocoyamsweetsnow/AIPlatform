// 离线用的假模型：不依赖网络和 API Key，用来开发前端、写测试、跑 CI。
// 真实项目里它就是「集成测试的 stub」，非常值得保留。
#pragma once

#include <memory>

#include "llm/types.h"

namespace llm {

std::unique_ptr<Provider> make_mock_provider(std::string canned_prefix = "");

}  // namespace llm
