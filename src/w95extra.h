#pragma once
/* Declarations for locale-reentrant CRT variants that libc++ v23 expects but
 * mingw-w64 v12 / Win9x-era msvcrt headers don't declare. Implementations are
 * shimmed in w95imports.cpp (locale parameter ignored). */
#include <wctype.h>
#include <wchar.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

extern "C"
{
    int _iswctype_l(wint_t, wctype_t, void *);
    int _iswalpha_l(wint_t, void *);
    int _iswupper_l(wint_t, void *);
    int _iswlower_l(wint_t, void *);
    int _iswdigit_l(wint_t, void *);
    int _iswxdigit_l(wint_t, void *);
    int _iswspace_l(wint_t, void *);
    int _iswprint_l(wint_t, void *);
    int _iswpunct_l(wint_t, void *);
    int _iswcntrl_l(wint_t, void *);
    wint_t _towlower_l(wint_t, void *);
    wint_t _towupper_l(wint_t, void *);
}
extern "C"
{
    int rand_s(unsigned int *);
}
