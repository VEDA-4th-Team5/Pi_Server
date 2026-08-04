#pragma once

#include "database/EventDatabase.hpp"

namespace parking_timer {

// 기존 타이머 호출부의 소스 호환성을 유지하면서 DB 소유권은 단일 클래스로 통합한다.
using EventDatabase = database::EventDatabase;

}  // namespace parking_timer
