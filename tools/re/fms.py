"""Lists the strings of a MsgSheet .fms file (FMSB).

Layout seen so far (UcScriptMsgConv.fms, 1.04): a 0x20-byte header ("FMSB", u32 file size,
u32 0x20, ..., u32 0x77a0f at 0x14, u32 3 at 0x18), then a zero-filled area (0x3c6cb8 bytes;
probably an index the game fills at load time), then NUL-separated UTF-8 strings, then "FEOC".
How a script line's ID maps to a string is NOT known yet: the string order differs between the
Japanese, Traditional Chinese and English files, so it is not the string's ordinal.

usage: fms.py FILE.fms [substring]
"""
import sys

TEXT = 0x3c6cd8


def strings(path):
    d = open(path, 'rb').read()
    end = d.rfind(b'FEOC')
    return d[TEXT:end].rstrip(b'\0').split(b'\0')


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    want = sys.argv[2].encode() if len(sys.argv) > 2 else None
    for i, s in enumerate(strings(sys.argv[1])):
        if s and (want is None or want in s):
            print(i, s.decode('utf-8', 'replace').replace('\n', '\n'))
