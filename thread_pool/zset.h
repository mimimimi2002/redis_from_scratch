#pragma once

#include <vector>
#include <string>
struct ZSetItem {
    std::string member;
    double score;
};

struct ZSet {
    std::vector<ZSetItem> items;
};