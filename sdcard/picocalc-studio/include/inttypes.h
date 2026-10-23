/* inttypes.h - PicoCalc C library (format macros match the firmware's newlib printf/scanf) */
#ifndef _INTTYPES_H
#define _INTTYPES_H

#include <stdint.h>

#define PRId8  "hhd"
#define PRIi8  "hhi"
#define PRIo8  "hho"
#define PRIu8  "hhu"
#define PRIx8  "hhx"
#define PRIX8  "hhX"
#define PRId16 "hd"
#define PRIi16 "hi"
#define PRIo16 "ho"
#define PRIu16 "hu"
#define PRIx16 "hx"
#define PRIX16 "hX"
#define PRId32 "ld"
#define PRIi32 "li"
#define PRIo32 "lo"
#define PRIu32 "lu"
#define PRIx32 "lx"
#define PRIX32 "lX"
#define PRId64 "lld"
#define PRIi64 "lli"
#define PRIo64 "llo"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIX64 "llX"

#define PRIdLEAST8  PRId8
#define PRIiLEAST8  PRIi8
#define PRIoLEAST8  PRIo8
#define PRIuLEAST8  PRIu8
#define PRIxLEAST8  PRIx8
#define PRIXLEAST8  PRIX8
#define PRIdLEAST16 PRId16
#define PRIiLEAST16 PRIi16
#define PRIoLEAST16 PRIo16
#define PRIuLEAST16 PRIu16
#define PRIxLEAST16 PRIx16
#define PRIXLEAST16 PRIX16
#define PRIdLEAST32 PRId32
#define PRIiLEAST32 PRIi32
#define PRIoLEAST32 PRIo32
#define PRIuLEAST32 PRIu32
#define PRIxLEAST32 PRIx32
#define PRIXLEAST32 PRIX32
#define PRIdLEAST64 PRId64
#define PRIiLEAST64 PRIi64
#define PRIoLEAST64 PRIo64
#define PRIuLEAST64 PRIu64
#define PRIxLEAST64 PRIx64
#define PRIXLEAST64 PRIX64

#define PRIdFAST8  "d"
#define PRIiFAST8  "i"
#define PRIoFAST8  "o"
#define PRIuFAST8  "u"
#define PRIxFAST8  "x"
#define PRIXFAST8  "X"
#define PRIdFAST16 "d"
#define PRIiFAST16 "i"
#define PRIoFAST16 "o"
#define PRIuFAST16 "u"
#define PRIxFAST16 "x"
#define PRIXFAST16 "X"
#define PRIdFAST32 "d"
#define PRIiFAST32 "i"
#define PRIoFAST32 "o"
#define PRIuFAST32 "u"
#define PRIxFAST32 "x"
#define PRIXFAST32 "X"
#define PRIdFAST64 PRId64
#define PRIiFAST64 PRIi64
#define PRIoFAST64 PRIo64
#define PRIuFAST64 PRIu64
#define PRIxFAST64 PRIx64
#define PRIXFAST64 PRIX64

#define PRIdMAX PRId64
#define PRIiMAX PRIi64
#define PRIoMAX PRIo64
#define PRIuMAX PRIu64
#define PRIxMAX PRIx64
#define PRIXMAX PRIX64

#define PRIdPTR "d"
#define PRIiPTR "i"
#define PRIoPTR "o"
#define PRIuPTR "u"
#define PRIxPTR "x"
#define PRIXPTR "X"

#define SCNd8  "hhd"
#define SCNi8  "hhi"
#define SCNo8  "hho"
#define SCNu8  "hhu"
#define SCNx8  "hhx"
#define SCNd16 "hd"
#define SCNi16 "hi"
#define SCNo16 "ho"
#define SCNu16 "hu"
#define SCNx16 "hx"
#define SCNd32 "ld"
#define SCNi32 "li"
#define SCNo32 "lo"
#define SCNu32 "lu"
#define SCNx32 "lx"
#define SCNd64 "lld"
#define SCNi64 "lli"
#define SCNo64 "llo"
#define SCNu64 "llu"
#define SCNx64 "llx"

#define SCNdLEAST8  SCNd8
#define SCNiLEAST8  SCNi8
#define SCNoLEAST8  SCNo8
#define SCNuLEAST8  SCNu8
#define SCNxLEAST8  SCNx8
#define SCNdLEAST16 SCNd16
#define SCNiLEAST16 SCNi16
#define SCNoLEAST16 SCNo16
#define SCNuLEAST16 SCNu16
#define SCNxLEAST16 SCNx16
#define SCNdLEAST32 SCNd32
#define SCNiLEAST32 SCNi32
#define SCNoLEAST32 SCNo32
#define SCNuLEAST32 SCNu32
#define SCNxLEAST32 SCNx32
#define SCNdLEAST64 SCNd64
#define SCNiLEAST64 SCNi64
#define SCNoLEAST64 SCNo64
#define SCNuLEAST64 SCNu64
#define SCNxLEAST64 SCNx64

#define SCNdFAST8  "d"
#define SCNiFAST8  "i"
#define SCNoFAST8  "o"
#define SCNuFAST8  "u"
#define SCNxFAST8  "x"
#define SCNdFAST16 "d"
#define SCNiFAST16 "i"
#define SCNoFAST16 "o"
#define SCNuFAST16 "u"
#define SCNxFAST16 "x"
#define SCNdFAST32 "d"
#define SCNiFAST32 "i"
#define SCNoFAST32 "o"
#define SCNuFAST32 "u"
#define SCNxFAST32 "x"
#define SCNdFAST64 SCNd64
#define SCNiFAST64 SCNi64
#define SCNoFAST64 SCNo64
#define SCNuFAST64 SCNu64
#define SCNxFAST64 SCNx64

#define SCNdMAX SCNd64
#define SCNiMAX SCNi64
#define SCNoMAX SCNo64
#define SCNuMAX SCNu64
#define SCNxMAX SCNx64

#define SCNdPTR "d"
#define SCNiPTR "i"
#define SCNoPTR "o"
#define SCNuPTR "u"
#define SCNxPTR "x"

typedef struct {
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

#endif
