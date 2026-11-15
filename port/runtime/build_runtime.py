"""Build the host runtime and link GameAssembly.dll.

    python build_runtime.py            generate tables, compile port/runtime, link with @build/obj/gen_objs.rsp,
                                       copy GameAssembly.dll to game/ when that folder exists
    python build_runtime.py --test     build and run the stand-alone tests (no generated objects needed)
    python build_runtime.py --test-net build and run the stand-alone test of the online transport (netplay.c)
    python build_runtime.py --no-copy  link only

Toolchain: llvm-mingw clang/lld (x86_64-w64-windows-gnu, UCRT). Outputs go to build/obj (rt/ for generated sources,
obj/rt/ for objects).
"""
import os, subprocess, sys, shutil, concurrent.futures, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, 'port'))
import paths  # noqa: E402

BUILD = str(paths.OBJ)
GEN = os.path.join(BUILD, 'rt')
OBJ = os.path.join(BUILD, 'obj', 'rt')
CLANG = str(paths.llvm('clang'))
CLANGXX = str(paths.llvm('clang++'))
UNITY_API = str(paths.TABLES / 'il2cpp_api.tsv')
API_MAP = str(paths.TABLES / 'il2cpp_api_map.tsv')
IMAGE = str(paths.MAIN_BIN)
METADATA = str(paths.METADATA)
SOURCES = ['imports_posix.c', 'log.c', 'dispatch.c', 'thread.c', 'loader.c', 'eh.c', 'imports_libc.c', 'imports_cxx.c', 'imports_nn.c',
           'imports_fs.c', 'imports_plat.c', 'imports_hid.c', 'pad_hid.c', 'pad_ps.c', 'pad_switch.c', 'display_opt.c', 'interp.c', 'pc_shell.c', 'gfx_opt.c', 'port_text.c', 'dlc_log.c', 'chara_dump.c', 'window.c', 'ultrawide.c', 'netplay.c', 'upnp.c', 'steam_net.c', 'online.c', 'online_ui.c', 'playstyle_ui.c', 'name_ui.c', 'tutorial_skip.c', 'pc_input.c', 'pc_glyphs.c', 'test_aids.c', 'delisted.c', 'chara_style.c', 'glyph_data.S', 'heapdbg.c', 'swkbd_win.c', 'imports_audio.c', 'pinvoke_policy.c', 'unity_plugin.c', 'icall_host.c',
           'gc.c', 'icall.c', 'api_hand.c', 'thunk.cpp', 'tramp.S']
LIBS = ['-lbcrypt', '-lwinmm', '-lxinput9_1_0', '-lhid', '-lsetupapi', '-lws2_32', '-lole32', '-loleaut32', '-luuid', '-lgdi32', '-lshell32']
CFLAGS = ['-O2', '-g', '-march=x86-64-v3', '-D_WIN32_WINNT=0x0A00', '-fno-strict-aliasing', '-Wall', '-Wextra', '-Wno-unused-parameter',
          '-Wno-missing-field-initializers', f'-DGAME_NAME="{paths.NAME}"', f'-DPORT_LABEL="{paths.PORT_LABEL}"', '-I', os.path.join(HERE, 'include'), '-I', HERE,
          '-I', GEN]
env = paths.tool_env()


def run(cmd, **kw):
    r = subprocess.run(cmd, env=env, capture_output=True, text=True, **kw)
    if r.returncode:
        print(' '.join(cmd)[:400])
        print(r.stdout[-6000:], r.stderr[-6000:])
        sys.exit(1)
    return r.stdout + r.stderr


def compile_one(src, extra=()):
    path = src if os.path.isabs(src) else os.path.join(HERE, src)
    obj = os.path.join(OBJ, os.path.basename(src) + '.o')
    deps = [path] + [os.path.join(HERE, f) for f in os.listdir(HERE) if f.endswith(('.h', '.inc'))] + \
           [os.path.join(HERE, 'icons', f) for f in os.listdir(os.path.join(HERE, 'icons')) if f.endswith(('.inc', '.png'))] + \
           [os.path.join(HERE, 'include', f) for f in os.listdir(os.path.join(HERE, 'include'))] + \
           [os.path.join(GEN, f) for f in os.listdir(GEN)] + [paths.__file__]   # paths.py: PORT_LABEL, GAME_NAME
    if os.path.exists(obj) and all(os.path.getmtime(obj) > os.path.getmtime(d) for d in deps):
        return obj
    cc = CLANGXX if src.endswith('.cpp') else CLANG
    flags = list(CFLAGS) + list(extra)
    if src.endswith('.cpp'):
        flags += ['-std=c++17']
    elif src.endswith('.c'):
        flags += ['-std=gnu17']
    out = run([cc] + flags + ['-c', path, '-o', obj])
    if out.strip():
        print(out.strip()[:3000])
    return obj


def generate():
    os.makedirs(GEN, exist_ok=True)
    os.makedirs(OBJ, exist_ok=True)
    hand = ','.join(os.path.join(HERE, f) for f in ('api_hand.c', 'icall.c'))
    print(run([sys.executable, os.path.join(HERE, 'gen_api.py'), UNITY_API, API_MAP, hand, os.path.join(GEN, 'api_gen.c')]).strip())
    print(run([sys.executable, os.path.join(HERE, 'gen_icall_sigs.py'), IMAGE, METADATA,
               os.path.join(GEN, 'icall_sigs.inc')]).strip())


def main():
    t0 = time.time()
    if '--test-net' in sys.argv:      # stand-alone test of the online transport (loopback, simulated packet loss)
        os.makedirs(BUILD, exist_ok=True)
        exe = os.path.join(BUILD, 'test_netplay.exe')
        run([CLANG, '-O2', '-g', '-std=gnu17', '-Wall', '-Wextra', '-Wno-unused-parameter'] +
            [os.path.join(HERE, f) for f in ('netplay.c', 'upnp.c', 'steam_net.c', os.path.join('tests', 'test_netplay.c'))] +
            ['-o', exe, '-static', '-lws2_32', '-lbcrypt', '-lole32', '-loleaut32', '-luuid'])
        sys.exit(subprocess.run([exe], env=env).returncode)
    generate()
    srcs = SOURCES + [os.path.join(GEN, 'api_gen.c')]
    if '--test' in sys.argv:
        srcs += [os.path.join(HERE, 'tests', 'test_runtime.c')]
    with concurrent.futures.ThreadPoolExecutor(8) as ex:
        objs = list(ex.map(compile_one, srcs))
    if '--test' in sys.argv:
        exe = os.path.join(BUILD, 'test_runtime.exe')
        run([CLANGXX, '-o', exe] + objs + ['-static'] + LIBS + ['-fuse-ld=lld', '-g'])
        r = subprocess.run([exe, IMAGE], env=dict(env, BR_LOG=os.path.join(BUILD, 'test_runtime.log')))
        print('tests exit code', r.returncode)
        sys.exit(r.returncode)
    rsp = os.path.join(BUILD, 'gen_objs.rsp')
    if not os.path.exists(rsp):
        print('build/obj/gen_objs.rsp does not exist yet: runtime objects compiled, nothing linked')
        return
    dll = os.path.join(BUILD, 'GameAssembly.dll')
    rt_rsp = os.path.join(BUILD, 'rt_objs.rsp')
    open(rt_rsp, 'w').write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs) + '\n')
    run([CLANGXX, '-shared', '-o', dll, '@' + rt_rsp, '@' + rsp, '-static'] + LIBS + ['-fuse-ld=lld', '-g',
         '-Wl,--stack,8388608'])
    print(f'linked {dll} ({os.path.getsize(dll) >> 20} MiB) in {time.time() - t0:.0f}s')
    if '--no-copy' not in sys.argv and paths.GAME.is_dir():
        dst = str(paths.GAME / 'GameAssembly.dll')
        shutil.copy2(dll, dst)
        print('copied to', dst)


if __name__ == '__main__':
    main()
