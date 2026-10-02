#!/usr/bin/env python3
"""Mark libopenmpt's rare formats so one source tree builds both editions.

Usage: formats.py <unpacked libopenmpt source dir>
"""
import re, sys

# loaders of the common-formats edition
COMMON_LOADERS = {
    'XM', 'IT', 'S3M', 'MOD', 'STK', 'ICE', 'KRIS', 'UNIC', 'PT36',
    'STM', 'MTM', '669', 'MED', 'OKT',
}
# their file extensions
COMMON_EXTS = {
    'mod', 'm15', 'stk', 'nst', 'wow', 'ice', 'unic', 'pt36',
    's3m', 'xm', 'oxm', 'it', 'mptm', 'stm', 'mtm', '669', 'med', 'okt',
}
MACRO = 'MODJUKE95_COMMON_FORMATS'


def wrap(text, start, pattern, keep):
    """Wrap the table entries after start that keep rejects."""
    a = text.index(start)
    b = text.index('};', a)
    seen = []

    def sub(m):
        seen.append(m.group(1))
        if keep(m.group(1)):
            return m.group(0)
        return '#ifndef %s\n%s\n#endif' % (MACRO, m.group(0))

    body = re.sub(pattern, sub, text[a:b], flags=re.M)
    return text[:a] + body + text[b:], seen


def patch(path, start, pattern, keep, wanted):
    text = open(path, encoding='utf-8').read()
    if MACRO in text:
        sys.exit('%s: already patched' % path)
    text, seen = wrap(text, start, pattern, keep)
    missing = wanted - set(seen)
    if missing:  # a libopenmpt update renamed something: fail loudly
        sys.exit('%s: common entries not found: %s' % (path, ', '.join(sorted(missing))))
    open(path, 'w', encoding='utf-8').write(text)
    return sum(1 for s in seen if not keep(s))


def main(src):
    n1 = patch(src + '/soundlib/Sndfile.cpp', 'ModuleFormatLoaders[] =',
               r'^\tMPT_DECLARE_FORMAT\((\w+)\),.*$', lambda f: f in COMMON_LOADERS,
               COMMON_LOADERS)
    n2 = patch(src + '/soundlib/Tables.cpp', 'modFormatInfo[] =',
               r'^\t\{ UL_\("[^"]*"\),\s*"(\w+)"\s*\},$', lambda e: e in COMMON_EXTS,
               COMMON_EXTS)
    print('formats.py: %d loaders and %d extension entries marked as rare' % (n1, n2))


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(sys.argv[1])
