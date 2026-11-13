"""List the P/Invoke methods of the game's assemblies: work/unity/pinvoke_by_assembly.tsv (assembly, type, method).

    python pinvoke_list.py

A P/Invoke method is one whose metadata flags carry PinvokeImpl (0x2000). closure_build.py uses the list to say
which native functions the generated code calls through P/Invoke wrappers. Nested types are written
Outer+Inner, with the namespace of the outermost type.
"""
from _env import W  # noqa: E402
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from il2cpp_meta import Meta

METHOD_ATTRIBUTE_PINVOKE_IMPL = 0x2000


def main():
    m = Meta(W + r'\work\game\romfs\Data\Managed\Metadata\global-metadata.dat')

    # a nested type's declaring type: the nestedTypes section lists, per type, the type definitions nested in it
    import struct
    no, ns = m.sec['nestedTypes']
    tdo = m.sec['typeDefinitions'][0]
    outer = {}
    for t in m.types:
        v = struct.unpack_from('<16i8H2I', m.d, tdo + 88 * t['index'])
        for k in range(v[20]):
            outer[struct.unpack_from('<i', m.d, no + 4 * (v[12] + k))[0]] = t['index']

    def full(t):
        name = t['name']
        while t['index'] in outer:
            t = m.types[outer[t['index']]]
            name = t['name'] + '+' + name
        return (t['ns'] + '.' if t['ns'] else '') + name

    rows = []
    for t in m.types:
        for me in m.methods_of(t):
            if me['flags'] & METHOD_ATTRIBUTE_PINVOKE_IMPL:
                rows.append((t.get('image', ''), full(t), me['name']))
    out = W + r'\work\unity\pinvoke_by_assembly.tsv'
    with open(out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join('\t'.join(r) + '\n' for r in rows))
    print(len(rows), 'P/Invoke methods ->', out)


if __name__ == '__main__':
    main()
