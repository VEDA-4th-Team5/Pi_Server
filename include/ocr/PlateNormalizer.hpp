#pragma once

#include <string>

namespace ocr {

std::string normalizePlateNumber(const std::string& value);
bool isPlausibleKoreanPlate(const std::string& value);

}
