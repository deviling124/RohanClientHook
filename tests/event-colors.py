"""Emulate the reference's actual chat dispatcher/widget and color lookup.

No game process is launched. Unicorn executes selected native instructions with
synthetic chat objects and a fake listbox insertion function; no network/GPU APIs.
"""
from pathlib import Path
import hashlib
import re
import struct
import unittest
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_EIP

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '00_reference/rohanclient_unpacked.exe'
REFERENCE = '622e106891adc7c80bac7e526ffa2eafe5f76c37bf6e5847b1f2406ebfb814c0'


def image_cpu():
    raw = SOURCE.read_bytes()
    assert hashlib.sha256(raw).hexdigest() == REFERENCE
    pe = struct.unpack_from('<I', raw, 60)[0]
    opt = pe + 24
    size = struct.unpack_from('<I', raw, opt + 56)[0]
    cpu = Uc(UC_ARCH_X86, UC_MODE_32)
    cpu.mem_map(0x400000, (size + 4095) & ~4095)
    cpu.mem_map(0, 4096)  # Emulated FS:[0] for the widget's native SEH frame.
    cpu.mem_map(0x2000000, 0x10000)
    cpu.mem_map(0x3000000, 0x10000)
    cpu.mem_write(0x400000, raw[:struct.unpack_from('<I', raw, opt + 60)[0]])
    table = opt + struct.unpack_from('<H', raw, pe + 20)[0]
    for i in range(struct.unpack_from('<H', raw, pe + 6)[0]):
        rva, length, offset = struct.unpack_from('<III', raw, table + i * 40 + 12)
        cpu.mem_write(0x400000 + rva, raw[offset:offset + length])
    return cpu


def word(cpu, address, value=None):
    if value is None:
        return struct.unpack('<I', cpu.mem_read(address, 4))[0]
    cpu.mem_write(address, struct.pack('<I', value))


def native_dispatch(index):
    cpu = image_cpu()
    manager, sentinel, node = 0x2000000, 0x2000100, 0x2000120
    widget, wrapper, listbox = 0x2001000, 0x2002000, 0x2003000
    text, chars, vtable, stop = 0x2004000, 0x2005000, 0x2006000, 0x200F000
    word(cpu, manager + 12, sentinel)
    word(cpu, sentinel, node)
    word(cpu, node, sentinel)
    word(cpu, node + 8, widget)
    word(cpu, widget, vtable)
    word(cpu, vtable + 0x98, 0x5F12E0)
    word(cpu, widget + 0x104, wrapper)
    word(cpu, wrapper + 4, listbox)
    message = b'[Event]: Belzac was summoned in front of Ehres Harbor'
    cpu.mem_write(chars, message + b'\0')
    word(cpu, text + 4, chars)
    word(cpu, text + 20, len(message))
    word(cpu, text + 24, 160)
    stack = 0x300F000
    cpu.mem_write(stack, struct.pack('<7I', stop, 0x0D, text, index, 0, 0, 0))
    cpu.reg_write(UC_X86_REG_ECX, manager)
    cpu.reg_write(UC_X86_REG_ESP, stack)
    captured = []

    def insert(uc, address, _size, _context):
        if address != 0x4F6620:
            return
        sp = uc.reg_read(UC_X86_REG_ESP)
        args = struct.unpack('<8I', uc.mem_read(sp, 32))
        captured.append((uc.reg_read(UC_X86_REG_ECX), args[1:]))
        # Fake only listbox allocation/copying. Continue the actual widget and
        # dispatcher epilogues to verify native stack cleanup and SEH restoration.
        uc.reg_write(UC_X86_REG_ESP, sp + 32)
        uc.reg_write(UC_X86_REG_EIP, args[0])

    cpu.hook_add(UC_HOOK_CODE, insert)
    cpu.emu_start(0x5E9DE0, stop, count=1000)
    assert cpu.reg_read(UC_X86_REG_EIP) == stop
    assert cpu.reg_read(UC_X86_REG_ESP) == stack + 28
    assert word(cpu, 0) == 0
    assert captured == [(listbox, (text, index, 0, 15, 0, 0, 2))]
    return cpu


def native_draw_color(cpu, index):
    node = 0x2007000
    word(cpu, node + 0x20, index)
    cpu.reg_write(UC_X86_REG_ECX, node)
    # These are the actual renderer's MOV EDX,[ECX+20] and indexed palette load.
    assert bytes(cpu.mem_read(0x4F3797, 10)) == bytes.fromhex('8b51208b049530e9c200')
    cpu.emu_start(0x4F3797, 0x4F37A1, count=2)
    return cpu.reg_read(UC_X86_REG_EAX)


def luminance(argb):
    channels = [((argb >> shift) & 255) / 255 for shift in (16, 8, 0)]
    linear = [c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4 for c in channels]
    return sum(c * weight for c, weight in zip(linear, (0.2126, 0.7152, 0.0722)))


class EventColors(unittest.TestCase):
    def test_native_notice_blue(self):
        self.assertEqual(native_draw_color(native_dispatch(0x22), 0x22), 0xFF4080FF)

    def test_native_event_green(self):
        source = (ROOT / 'src/CustomEvents/EventNotice.cpp').read_text()
        index = int(re.search(r'EventColorIndex\s*=\s*(0x[0-9A-Fa-f]+)', source)[1], 16)
        self.assertEqual(index, 0x19)
        self.assertEqual(native_draw_color(native_dispatch(index), index), 0xFF42C762)

    def test_native_green_is_brighter_than_notice_blue(self):
        cpu = image_cpu()
        self.assertGreater(luminance(native_draw_color(cpu, 0x19)), luminance(native_draw_color(cpu, 0x22)))

    def test_appended_index_is_not_a_stock_color(self):
        # Without rewriting the renderer, index 66 reads adjacent string bytes,
        # not an opaque color. Using the existing green removes that dependency.
        self.assertEqual(native_draw_color(image_cpu(), 66), 0x5B5C3A44)

    def test_native_find_all_skips_other_windows(self):
        cpu = image_cpu()
        manager, sentinel, party_link, all_link = (0x2000100, 0x2000400, 0x2000500, 0x2000600)
        party, all_window, stack, stop = (0x2001000, 0x2002000, 0x300F000, 0x200F000)
        for address, value in (
            (manager+0xF0, sentinel), (sentinel, party_link),
            (party_link, all_link), (party_link+8, party), (party+0x144, 1),
            (all_link, sentinel), (all_link+8, all_window), (all_window+0x144, 0),
            (stack, stop),
        ):
            word(cpu, address, value)
        cpu.reg_write(UC_X86_REG_ECX, manager)
        cpu.reg_write(UC_X86_REG_ESP, stack)
        cpu.emu_start(0x5F4670, stop, count=100)
        self.assertEqual(cpu.reg_read(UC_X86_REG_EAX), all_window)
        word(cpu, all_window+0x144, 4)
        cpu.reg_write(UC_X86_REG_ECX, manager)
        cpu.reg_write(UC_X86_REG_ESP, stack)
        cpu.emu_start(0x5F4670, stop, count=100)
        self.assertEqual(cpu.reg_read(UC_X86_REG_EAX), 0)

    def test_native_global_rows_keep_the_color_marker_to_the_font(self):
        for wrapped_style, special, expected_call in (
            (0, 0, 0x4F3655), (0, 1, 0x4F360B),
            (1, 0, 0x4F404D), (1, 1, 0x4F4003),
        ):
            with self.subTest(wrapped_style=wrapped_style, special=special):
                cpu = image_cpu()
                box, vector, row, stack, stop = (0x2000100, 0x2000700, 0x2000800, 0x300F000, 0x200F000)
                marker = 0x5B524754
                for address, value in (
                    (box+0x30, 1), (box+0x88, vector), (box+0x8C, vector+4),
                    (box+0x54, 1), (box+0x18, 400), (box+0x1C, 180),
                    (box+0x6C, 14), (vector, row), (row+0x1C, 15),
                    (row+0x20, 0x2B), (row+0xAC, marker), (stack, stop),
                ):
                    word(cpu, address, value)
                cpu.mem_write(box+0xE8, bytes([wrapped_style]))
                cpu.mem_write(row+0xBA, bytes([special]))
                cpu.mem_write(row+8, b'[Trade] test\0')
                cpu.reg_write(UC_X86_REG_ECX, box)
                cpu.reg_write(UC_X86_REG_ESP, stack)
                calls = []

                def font(uc, address, _size, _context):
                    if address not in (0x516D30, 0x516E50):
                        return
                    args = struct.unpack('<8I', uc.mem_read(uc.reg_read(UC_X86_REG_ESP), 32))
                    color_slot, marker_slot = (3, 5) if address == 0x516E50 else (4, 6)
                    calls.append((args[0], args[color_slot], args[marker_slot]))
                    uc.emu_stop()

                cpu.hook_add(UC_HOOK_CODE, font)
                cpu.emu_start(0x4F3320, stop, count=5000)
                self.assertEqual(calls, [(expected_call, 0xFF82C0FB, marker)])


if __name__ == '__main__':
    unittest.main(verbosity=2)
