// ide.h - the on-device IDE: projects in /sd/projects, the editor, build (F3) and run (F4).
#pragma once

// Never returns. After a crash in a program, crashWhere is its "file.c:line" (from
// rt_module_crash_where) and crashWhy the panic report: the IDE opens the file at that line.
[[noreturn]] void ide_run(const char *crashWhere, const char *crashWhy);
