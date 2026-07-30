#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include "timeman.h"

using namespace hypersion;

namespace {

int failures = 0;

void expect_eq(const std::string& label, std::int64_t actual, std::int64_t expected) {
    if (actual == expected) {
        std::cout << "[PASS] " << label << " = " << actual << '\n';
        return;
    }
    std::cerr << "[FAIL] " << label << ": expected " << expected
              << ", got " << actual << '\n';
    ++failures;
}

TimeManager make_manager(SearchLimits limits, Color us = WHITE) {
    TimeManager manager;
    manager.init(limits, us, 0);
    return manager;
}

}  // namespace

int main() {
    {
        SearchLimits limits;
        limits.movetime = 1000;
        limits.moveOverhead = 30;
        auto manager = make_manager(limits);
        expect_eq("movetime optimum", manager.optimum(), 970);
        expect_eq("movetime maximum", manager.maximum(), 970);
    }
    {
        SearchLimits limits;
        limits.movetime = 20;
        limits.moveOverhead = 50;
        auto manager = make_manager(limits);
        expect_eq("movetime minimum", manager.optimum(), 10);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 10000;
        limits.moveOverhead = 30;
        auto manager = make_manager(limits);
        expect_eq("sudden-death optimum", manager.optimum(), 249);
        expect_eq("sudden-death maximum", manager.maximum(), 1245);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 500;
        limits.moveOverhead = 500;
        auto manager = make_manager(limits);
        expect_eq("low-time overhead cap optimum", manager.optimum(), 20);
        expect_eq("low-time overhead cap maximum", manager.maximum(), 50);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 10000;
        limits.inc[WHITE] = 100;
        limits.moveOverhead = 30;
        auto manager = make_manager(limits);
        expect_eq("increment optimum", manager.optimum(), 324);
        expect_eq("increment maximum", manager.maximum(), 1620);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 10000;
        limits.moveOverhead = 30;
        limits.ponderEnabled = true;
        auto manager = make_manager(limits);
        expect_eq("ponder optimum bonus", manager.optimum(), 311);
        expect_eq("ponder maximum unchanged", manager.maximum(), 1245);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 5000;
        limits.movestogo = 10;
        limits.moveOverhead = 0;
        auto manager = make_manager(limits);
        expect_eq("explicit moves-to-go optimum", manager.optimum(), 500);
        expect_eq("explicit moves-to-go maximum", manager.maximum(), 1000);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 1;
        limits.inc[WHITE] = 1000;
        limits.moveOverhead = 0;
        auto manager = make_manager(limits);
        expect_eq("increment-only survival optimum", manager.optimum(), 10);
        expect_eq("increment-only survival maximum", manager.maximum(), 10);
    }
    for (const std::string mode : {"depth", "nodes", "infinite", "mate"}) {
        SearchLimits limits;
        if (mode == "depth") limits.depth = 1;
        if (mode == "nodes") limits.nodes = 1;
        if (mode == "infinite") limits.infinite = true;
        if (mode == "mate") limits.mate = 1;
        auto manager = make_manager(limits);
        const auto unlimited = std::numeric_limits<std::int64_t>::max() / 4;
        expect_eq(mode + " bypass optimum", manager.optimum(), unlimited);
        expect_eq(mode + " bypass maximum", manager.maximum(), unlimited);
    }
    {
        SearchLimits limits;
        limits.time[WHITE] = 10000;
        limits.goStartTime = now() - 7;
        auto manager = make_manager(limits);
        expect_eq("go arrival anchor", manager.start(), limits.goStartTime);
    }

    if (failures) {
        std::cerr << failures << " time-manager test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All deterministic time-manager tests passed.\n";
    return EXIT_SUCCESS;
}
