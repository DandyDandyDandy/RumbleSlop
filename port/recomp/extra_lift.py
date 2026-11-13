"""Write port/recomp/extra_lift.tsv: functions lifted in addition to the managed closure of
the lift set of tools/closure_build.py (shipped as tables/lift_functions.tsv.gz).

    python extra_lift.py

Every FDE function of these NSO ranges is taken whole (the plugins are driven through P/Invoke entry points and
internal function-pointer tables, so reachability over direct branches would miss callbacks):
  criware   0x477650-0x57fe40   the CRIWARE native library (CRI Atom/Mana/FileSystem + the CriWare Unity plug-in glue).
                                Ends before the fifteen maria-upload thunks at 0x57fe40, which are host no-ops.
  nn_glue   0x594800-0x594820, 0x594f40-0x595790
                                P/Invoke targets of nn.dll / nnUserPlugin.Runtime.dll: thin wrappers over nn::*
                                imports. The nnUser vibration player between the two pieces (static
                                nn::hid::VibrationPlayer objects and a timer thread) is left out: its four natives
                                are host no-ops (port/runtime/imports_plat.c).
gen_il2cpp.py merges this file into the lift set.
"""
from pathlib import Path
import numpy as np

HERE = Path(__file__).resolve().parent
import sys
sys.path.insert(0, str(HERE.parent))
import paths  # noqa: E402

CLOSURE = paths.UNITY / 'closure'      # closure.npz of tools/closure_build.py (a developer output)
RANGES = [('criware', 0x477650, 0x57fe40), ('nn_glue', 0x594800, 0x594820), ('nn_glue', 0x594f40, 0x595790)]


def main():
    z = np.load(CLOSURE / 'closure.npz', allow_pickle=True)
    fs, fe, lift, nofde = z['fs'], z['fe'], z['lift'], z['nofde']
    lines = ['start\tend\tsize\tregion\n']
    for name, lo, hi in RANGES:
        m = (fs >= lo) & (fs < hi) & ~lift
        if (m & nofde).any():
            raise SystemExit(f'{name}: code without unwind info in the range; decide how to split it')
        for a, b in zip(fs[m].tolist(), fe[m].tolist()):
            lines.append(f'{a:#x}\t{b:#x}\t{b - a}\t{name}\n')
        print(f'{name}: {int(m.sum())} functions, {int((fe[m] - fs[m]).sum())} bytes')
    (HERE / 'tables' / 'extra_lift.tsv').write_text(''.join(lines), encoding='utf-8', newline='\n')


if __name__ == '__main__':
    main()
