// app_entry.h - a program compiled into the firmware by gcc (build.ps1 -Project).
#pragma once

bool rt_has_native_app();
[[noreturn]] void rt_run_native_app();
