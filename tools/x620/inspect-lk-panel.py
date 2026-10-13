#!/usr/bin/env python3
"""Capture X620 LK's 378C81 panel callbacks in an isolated ARM emulator.

No phone or MMIO access. Supply the owner's bootloader backup. Utility
callbacks are replaced with recording stubs; packet bytes remain those
constructed by LK, rather than inferred from an unrelated vendor DT.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR

BASE = 0x46000000
SCRATCH = 0x47000000
STOP = SCRATCH + 0x80


def capture(raw, entry, args=()):
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.mem_map(BASE, 0x100000)
    uc.mem_write(BASE, raw[512:])
    uc.mem_map(SCRATCH, 0x20000)
    events = []
    callbacks = {SCRATCH + 0x100 + i * 4: i for i in range(29)}
    for addr in callbacks:
        uc.mem_write(addr, b'\x70\x47')  # Thumb bx lr.
    uc.mem_write(SCRATCH + 0x4000,
                 struct.pack('<29I', *(p | 1 for p in callbacks)))

    def hook(machine, address, size, data):
        if address == STOP:
            machine.emu_stop()
            return
        r0 = machine.reg_read(UC_ARM_REG_R0)
        r1 = machine.reg_read(UC_ARM_REG_R1)
        r2 = machine.reg_read(UC_ARM_REG_R2)
        if address == BASE + 0x3c048:  # LK memcpy used by set_util_funcs.
            machine.mem_write(r0, bytes(machine.mem_read(r1, r2)))
        elif address == BASE + 0x304c4:  # LK memset used by get_params.
            machine.mem_write(r0, bytes([r1 & 255]) * r2)
        elif address == BASE + 0x2fd7c:  # LK printf; no UART emulation.
            machine.reg_write(UC_ARM_REG_R0, 0)
        elif address in callbacks:
            index = callbacks[address]
            event = {'utility_index': index, 'args': [r0, r1, r2]}
            if index == 11:  # dsi_set_cmdq(u32 *, count, force_update).
                assert 0 < r1 <= 64
                packet = bytes(machine.mem_read(r0, r1 * 4))
                event['words'] = list(struct.unpack('<' + 'I' * r1, packet))
                kind = packet[0] & 3
                if kind == 2:  # long packet; payload follows header.
                    length = struct.unpack_from('<H', packet, 2)[0]
                    assert length <= len(packet) - 4
                    event['payload_hex'] = packet[4:4 + length].hex()
                elif kind == 0:  # short DCS/generic packet.
                    length = 2 if packet[1] in (0x15, 0x23) else 1
                    event['payload_hex'] = packet[2:2 + length].hex()
                event['packet_type'] = packet[1]
            events.append(event)
            return
        else:
            return
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)

    def run(address, values):
        uc.reg_write(UC_ARM_REG_SP, SCRATCH + 0x1f000)
        uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        for reg, value in zip((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2), values):
            uc.reg_write(reg, value)
        uc.emu_start(address | 1, STOP, count=100000)
        assert uc.reg_read(UC_ARM_REG_PC) == STOP, 'execution did not return'

    run(BASE + 0x2c970, [SCRATCH + 0x4000])
    run(BASE + entry, args)
    return events, bytes(uc.mem_read(SCRATCH + 0x8000, 0x368))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('backup', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    options = parser.parse_args()
    raw = options.backup.read_bytes()
    assert raw[512 + 0x5bd2c:512 + 0x5bd2c + 7] == b'378C81\x00'
    results = {'backup_sha256': hashlib.sha256(raw).hexdigest(),
               'name': '378C81', 'execution': 'Unicorn ARM utility callback capture',
               'hardware_validation': 'not performed by this tool'}
    events, params = capture(raw, 0x2c8b8, [SCRATCH + 0x8000])
    results['params_words'] = {hex(i): struct.unpack_from('<I', params, i)[0]
                              for i in range(0, len(params), 4)
                              if struct.unpack_from('<I', params, i)[0]}
    for name, entry in [('init', 0x2c2b0), ('suspend', 0x2c230)]:
        results[name], _ = capture(raw, entry)
    options.output.write_text(json.dumps(results, indent=2) + '\n')
    print('Captured', len(results['init']), 'init callbacks and',
          len(results['suspend']), 'suspend callbacks')


if __name__ == '__main__':
    main()
