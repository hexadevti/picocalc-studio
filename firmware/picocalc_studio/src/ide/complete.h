// complete.h - name completion in the editor: a list of names that start with the word before the
// cursor, from C keywords, the SDK headers in the include folder, the project's files and the
// open files. It opens only on Ctrl-Space, so typing costs nothing; the names are read then.
#pragma once

#include "doc.h"

// The include folder, the library sources folder (where F9 finds the definitions of what the
// headers declare) and the open project ("" for none). Call it again after files are saved,
// added or deleted, so the project's names are read again.
void ac_project(const char *includeDir, const char *libDir, const char *projDir);

// Files of other projects the project includes (#include "../other/x.h"): read with the project's.
// Call it after ac_project().
void ac_extra(const char *const *paths, int n);

bool ac_active();
void ac_close();

// Ctrl-Space: opens the list for the word before the cursor (inserting the name right away if
// only one fits). False if no name fits.
bool ac_open(Doc *const *docs, int ndocs, Doc *d, int rows);

// A key while the list is open, before the editor sees it: Up/Down/PgUp/PgDn choose, Tab or
// Enter insert the name, Space inserts it and is then typed as usual, Esc closes. True if the key
// was used. Other keys close the list, except
// the ones that change the word (letters, digits, '_', Backspace), which are left to the editor
// and ac_after_key().
bool ac_key(Doc *d, int key, int rows);

// After the editor handled `key`: narrows the open list to the new word (without reading the
// files again), or closes it.
void ac_after_key(Doc *d, int key);

// Draws the list next to the cursor, over the editor drawn at y0 with `rows` lines.
void ac_draw(const Doc *d, int y0, int rows);

// For the status bar: the declaration of the chosen name while the list is open. Null if there is
// none.
const char *ac_hint();

// F9: where the name at (or just before) the cursor of d is declared. In order: a local variable
// or parameter of the function the cursor is in; a global (function, macro, type, enum constant,
// variable) of the open files, the project's files, the library sources or the headers, its
// definition rather than a prototype or extern; else (and for a member, after . or ->) the first use that looks like a
// declaration in those files. Fills path and the 0-based line and col; name gets the name ("" if
// the cursor isn't on one). False if nothing is found.
bool ac_find_decl(Doc *const *docs, int ndocs, Doc *d, char *name, int nameSize, char *path, int pathSize,
                  int *line, int *col);
