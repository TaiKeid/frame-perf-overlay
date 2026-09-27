#pragma once
#include <ctime>
#include <string>

std::string formatClock(const std::tm& local, int format);
std::string localClock(int format);
