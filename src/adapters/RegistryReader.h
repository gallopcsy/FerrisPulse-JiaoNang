// RegistryReader.h — 注册表只读适配层
#pragma once
#include "core/CapsuleCore.h"

namespace capsule {
namespace adapters {

// 只读遍历 FerrisPulse 全部电量键族，返回快照（readings + hostRefreshIntervalMs；hostPresent 由调用方填充）
Snapshot ReadRegistrySnapshot();

} // namespace adapters
} // namespace capsule
