# SPDX-License-Identifier: GPL-3.0-or-later
"""Emit the NxEh unwind tables (nxrecomp/ctx.h) of a module: CFI rows and saved-register lists of every FDE, plus the
C++ LSDA call sites, action records and type tables. The runtime's unwinder (or oracle/e2e/rt/eh_rt.c) walks the guest
stack with these."""
from . import eh as ehmod


def emit(env, m):
    img = env.img
    rd = ehmod.Reader(img)
    fdes, cies = ehmod.all_fdes(img)
    if not fdes:
        return 'NULL'
    rows, saved, sites_out, actions_out, types_out, fde_out = [], [], [], [], [], []
    for f in fdes:
        cie = ehmod.parse_cie(rd, f.cie)
        try:
            frows = ehmod.cfi_rows(rd, f, cie)
        except Exception:                                   # an unsupported CFA program: the function gets no rows
            frows = []
        row_first = len(rows)
        for pc, cfa_reg, cfa_off, sv in frows:
            if cfa_reg is None:
                continue
            keep = sorted((r, o) for r, o in sv.items() if o is not None and (r <= 30 or 64 <= r <= 95))
            rows.append((env.base + pc, cfa_reg, cfa_off, len(keep), len(saved)))
            saved += keep
        site_first = len(sites_out)
        type_first = len(types_out)
        if f.lsda:
            lsites, lactions, ltypes = ehmod.parse_lsda(rd, img, f)
            order = sorted(lactions)                        # 1-based byte offsets -> compact indices
            index = {k: i + len(actions_out) for i, k in enumerate(order)}
            for k in order:
                filt, nxt = lactions[k]
                actions_out.append((filt, index[nxt] + 1 if nxt else 0))
            for s in lsites:
                sites_out.append((env.base + s.start, s.len, env.base + s.pad if s.pad else 0,
                                  index[s.action] + 1 if s.action else 0))
            types_out += ltypes
        fde_out.append((env.base + f.start, env.base + f.end, row_first, len(rows) - row_first, site_first,
                        len(sites_out) - site_first, type_first))
    m.append('static const NxFde eh_fdes[] = {')
    m += ['  { 0x%xULL, 0x%xULL, %d, %d, %d, %d, %d },' % x for x in fde_out]
    m.append('};')
    m.append('static const NxCfiRow eh_rows[] = {')
    m += ['  { 0x%xULL, %d, %d, %d, %d },' % (pc, cr, ns, co, sf) for pc, cr, co, ns, sf in rows] or ['  { 0, 0, 0, 0, 0 },']
    m.append('};')
    m.append('static const NxCfiSaved eh_saved[] = {')
    m += ['  { %d, %d },' % x for x in saved] or ['  { 0, 0 },']
    m.append('};')
    m.append('static const NxCallSite eh_sites[] = {')
    m += ['  { 0x%xULL, %d, 0x%xULL, %d },' % x for x in sites_out] or ['  { 0, 0, 0, 0 },']
    m.append('};')
    m.append('static const NxAction eh_actions[] = {')
    m += ['  { %d, %d },' % x for x in actions_out] or ['  { 0, 0 },']
    m.append('};')
    m.append('static const uint64_t eh_types[] = {')
    m += ['  0x%xULL,' % x for x in types_out] or ['  0,']
    m.append('};')
    m.append('static const NxEh eh = { eh_fdes, %d, eh_rows, eh_saved, eh_sites, eh_actions, eh_types };' % len(fde_out))
    return '&eh'
