# SPDX-License-Identifier: MIT
"""Apply upstream Forza patch 0025's emitted dot-product fix to stock codegen.

The Windows SDK is pinned to the same revision. This transformation must match
our patched Linux generator exactly; do not generalize it to other versions.
"""
import re

DOT = re.compile(r'\tsimde_mm_store_ps\(([^,\n]+)\.f32, simde_mm_dp_ps\(simde_mm_load_ps\(([^,\n]+)\.f32\), simde_mm_load_ps\(([^,\n]+)\.f32\), (0xEF|0xFF)\)\);')


def replace_dot(match):
    dest, a, b, mask = match.groups()
    lines = ['\t{', f'\t\tsimde__m128 d = simde_mm_dp_ps(simde_mm_load_ps({a}.f32), simde_mm_load_ps({b}.f32), {mask});',
             '\t\tif ((uint32_t(simde_mm_cvtsi128_si32(simde_mm_castps_si128(d))) << 1) >= 0xFF000000u) [[unlikely]] {',
             f'\t\t\tdouble s = double({a}.f32[3]) * {b}.f32[3] + double({a}.f32[1]) * {b}.f32[1];']
    tail = f' + double({a}.f32[0]) * {b}.f32[0]' if mask == '0xFF' else ''
    lines += [f'\t\t\ts += double({a}.f32[2]) * {b}.f32[2]{tail};', '\t\t\tfloat f = float(s);',
              '\t\t\td = std::isinf(f) && std::isfinite(s) ? simde_mm_castsi128_ps(simde_mm_set1_epi32(0x7FC00000)) : simde_mm_set1_ps(f);',
              '\t\t}', f'\t\tsimde_mm_store_ps({dest}.f32, d);', '\t}']
    return '\n'.join(lines)


def normalize(generated):
    count = 0
    for path in generated.rglob('*.cpp'):
        text = path.read_text(encoding='utf-8')
        changed, number = DOT.subn(replace_dot, text)
        if number:
            path.write_text(changed, encoding='utf-8', newline='\n')
            count += number
    if not count:
        raise ValueError('Expected Forza dot-product instructions; stock codegen output changed')
    return count
