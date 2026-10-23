// selftest.h - serial "@test": runs the compiler test suite from the SD card on the device.
#pragma once

void rt_selftest_install();   // after hal_init()
bool rt_selftest_service();   // runs a queued @test (CC_BORROW_FRAMEBUFFER); true if it did
