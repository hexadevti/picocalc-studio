// doc.h - a text file being edited: an array of lines in PSRAM, the view state the editor keeps
// per file, and undo/redo. Tabs are expanded to spaces and CRs dropped on load.
#pragma once

#include <stdint.h>

#define DOC_TAB 4
#define DOC_MAX_BREAKS 16

struct DocLine {
  char *s;                 // not NUL-terminated
  int len, cap;
};

struct DocUndo {           // one recorded edit
  char *s;                 // inserted or deleted text, may hold newlines
  int n;
  int y, x;                // where
  int group;               // edits undone together
  int cy, cx;              // cursor before the group
  bool ins, typing;
};

struct Doc {
  char path[192];
  DocLine *ln;
  int n, cap;              // n >= 1 always
  bool modified;
  bool readOnly;           // a library header outside the project: the editor only views it
  // view
  int cx, cy;              // cursor column and line
  int want;                // column the cursor tries to keep on Up/Down
  int top, left;           // first visible line and column
  bool sel;                // selection from (ax, ay) to the cursor
  int ax, ay;
  bool marking;            // mark mode: cursor keys extend the selection without Shift
  // syntax highlighting: whether line i starts inside a /* comment, valid for i < hlValid
  uint8_t *hl;
  int hlValid;
  // undo: u[0, nu) can be undone, u[nu, nt) redone
  DocUndo *u;
  int nu, nt, ucap, group;
  // debugger breakpoints: line indexes, unordered; edits move them with their lines
  int bp[DOC_MAX_BREAKS];
  int nbp;
  bool parked;             // doc_park_all() freed the lines
  Doc *nextOpen;           // doc.cpp's list of open docs
};

Doc *doc_open(const char *path);      // null if it can't be read (or out of memory)
Doc *doc_new(const char *path, const char *text);
bool doc_save(Doc *d);
void doc_free(Doc *d);

// Frees the lines of every open doc that matches its file (to make room for a build), and reads
// them back. Nothing may look at a doc in between. A doc that can't be read back becomes a
// read-only note saying so.
void doc_park_all();
void doc_unpark_all();

// Edits are recorded for undo, grouped: call doc_begin() once per user action (with the cursor
// still where it was). Typing (doc_begin_typing) runs into the previous typing group up to a word.
void doc_begin(Doc *d);
void doc_begin_typing(Doc *d, char c);
// Inserts text (may hold newlines) at (y, x); (*ey, *ex) is where it ends.
bool doc_insert_text(Doc *d, int y, int x, const char *s, int n, int *ey, int *ex);
// Deletes from (y0, x0) up to (y1, x1), which must come after it.
void doc_delete_range(Doc *d, int y0, int x0, int y1, int x1);
// The text of a range, malloc'ed and NUL-terminated (n gets its length). Null if out of memory.
char *doc_get_text(const Doc *d, int y0, int x0, int y1, int x1, int *n);
// Undo/redo one group; they move the cursor. False if there is nothing to undo/redo.
bool doc_undo(Doc *d);
bool doc_redo(Doc *d);

// Sets or clears a breakpoint on line y; false if it was full.
bool doc_toggle_break(Doc *d, int y);
bool doc_has_break(const Doc *d, int y);

// Leading spaces of line y.
int doc_indent(const Doc *d, int y);

// Syntax classes, one per character of a line.
enum { HL_TEXT, HL_KEYWORD, HL_TYPE, HL_NUMBER, HL_STRING, HL_COMMENT, HL_PREPROC, HL_CONST, HL_FUNC, HL_PUNCT };
// Fills cls[0..len) for line y (which must exist).
void doc_highlight(Doc *d, int y, uint8_t *cls);
// Classifies len characters of one line into cls (may be null), given whether the line starts
// inside a /* comment; returns whether it ends inside one. For text outside a Doc (the debugger).
bool hl_scan(const char *s, int len, bool inComment, uint8_t *cls);
// The editor's RGB565 color of each HL_* class.
extern const uint16_t hl_colors[];
