"""
Capstone handle whose disasm() decodes on demand.

Cs.disasm(code, addr) with no count decodes the whole buffer, with detail,
before it yields the first instruction. The analysis passes hand it the rest
of a section from some entry point and stop after a few instructions, at the
first ret or jmp, so every probe paid for decoding the whole section: on
Nightfire's default.xbe that was 99% of a 20-minute disasm stage.

LazyCs decodes in batches that double from 8 up to 1024 instructions and
yields the same instructions in the same order: cs_disasm stops at the first
undecodable byte, and so does this, since a short batch that still had room
for all its instructions ends the stream exactly there.
"""

from capstone import Cs

# Longest x86 instruction: a batch window of count * 15 bytes always holds
# `count` instructions, so a short batch means an invalid byte or the end.
_MAX_INSN = 15


class LazyCs(Cs):
    def disasm(self, code, offset, count=0):
        if count:
            yield from super().disasm(code, offset, count)
            return
        pos = 0
        size = len(code)
        batch = 8
        while pos < size:
            window = code[pos:pos + batch * _MAX_INSN]
            got = 0
            last = None
            for insn in super().disasm(window, offset + pos, batch):
                got += 1
                last = insn
                yield insn
            if got < batch:
                return
            pos = last.address + last.size - offset
            batch = min(batch * 2, 1024)
