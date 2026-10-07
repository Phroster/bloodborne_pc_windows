#!/usr/bin/env python3
"""bbport-windows: prepares the game image and starts the game; the Windows run.sh.

Usage: python windows/run.py [bb-probe options...]

Environment (as run.sh):
  BB_GAME_DIR      the game folder (eboot.bin, sce_module, ...); default ../CUSA03173
  BB_DATA_DIR      generated files (out/), saves (user/), bbport.ini; default the repository
  BB_PROBE         the loader; default out/bb-probe.exe (windows/build.sh)
  BB_FPS, BB_PATCHES, BB_RENDER_RES, BB_LIVE_RES, BB_CONTENT_SKU, BB_USER_DIR, BB_TIMEOUT,
  BB_MODS_DIR, BB_MODS_CONFIG, BB_MODS_ENABLED, BB_PATCHES_DIR, BB_PATCHES_CONFIG, BB_SAVE_LOG

Unlike run.sh, the merged mod folder is only ever removed when it is a mod view mods.py made
in out/: run.sh compared two spellings of the same path, which on Windows differ, and would
then have deleted the game folder itself.
"""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
# bb-probe exits with this code to be started again (the in-game "Apply and restart").
RESTART_EXIT_CODE = 75
# Where MSYS2 keeps the DLLs a development build of bb-probe.exe needs.
MSYS2_DLLS = Path(os.environ.get('MSYS2_ROOT', 'C:/msys64')) / 'clang64' / 'bin'


def fail(message):
    print(message, file=sys.stderr)
    sys.exit(1)


def script(*args, capture=False):
    """Runs one of the Python scripts with this interpreter, from the repository."""
    command = [sys.executable, *map(str, args)]
    if capture:  # stdout only: the scripts report on stderr
        return subprocess.run(command, cwd=ROOT, check=True, stdout=subprocess.PIPE, text=True).stdout
    subprocess.run(command, cwd=ROOT, check=True)
    return None


def find_dlls(probe):
    """A development build runs with MSYS2's DLLs; a packaged one has them next to it."""
    if (probe.parent / 'SDL3.dll').is_file():
        return
    if not (MSYS2_DLLS / 'SDL3.dll').is_file():
        fail(f'{probe.name} needs its DLLs: put them next to it or set MSYS2_ROOT (looked in {MSYS2_DLLS}).')
    path = os.environ.get('PATH', '')
    if str(MSYS2_DLLS).casefold() not in path.casefold():
        os.environ['PATH'] = f'{MSYS2_DLLS}{os.pathsep}{path}'


def merge_mods(game, out, data):
    """The game folder to run: a merged view of the enabled mods (scripts/mods.py), else the game."""
    if os.environ.get('BB_MODS_ENABLED', '1') != '1':
        return game, None
    try:
        merged = script('scripts/mods.py', game, '--out', out,
                        '--mods-dir', os.environ.get('BB_MODS_DIR', data / 'mods'),
                        '--config', os.environ.get('BB_MODS_CONFIG', data / 'mods.json'),
                        '--enabled', '1', capture=True).strip()
    except subprocess.CalledProcessError:
        # mods.py links the game's files: Windows allows symbolic links with Developer Mode on.
        print('Mods: not loaded (see above; linking needs Windows Developer Mode)', file=sys.stderr)
        return game, None
    merged = Path(merged)
    if merged.resolve() == game.resolve():
        return game, None
    return merged, merged


def remove_mod_view(view, out, game):
    """Removes the merged view, unlinking its links without following them."""
    view, out = view.resolve(), out.resolve()
    if view.parent != out or not view.name.startswith('mod-game-') or view == game.resolve():
        print(f'Mods: left {view} in place (not a mod view in {out})', file=sys.stderr)
        return

    def remove(path):
        if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()):
            (os.rmdir if path.is_dir() else os.unlink)(path)  # the link itself, never its target
        elif path.is_dir():
            for entry in path.iterdir():
                remove(entry)
            path.rmdir()
        else:
            path.unlink()
    remove(view)


def settings_value(config, key):
    """A key=value line of bbport.ini, or None."""
    if not config.is_file():
        return None
    for line in config.read_text(encoding='utf-8', errors='replace').splitlines():
        name, _, value = line.partition('=')
        if name.strip() == key:
            return value.strip()
    return None


def choose_resolution(config, probe):
    """Sets BB_RENDER_RES/BB_OUTPUT_RES for outputs other than 1080p, as run.sh does."""
    # Sizes chosen for the previous launch are recomputed after an in-game restart.
    if os.environ.get('BB_AUTO_RENDER_RES') == '1':
        for name in ('BB_RENDER_RES', 'BB_OUTPUT_RES', 'BB_AUTO_RENDER_RES'):
            os.environ.pop(name, None)
    if os.environ.get('BB_RENDER_RES'):
        return
    scaled = script('scripts/patches.py', '--print-scaled', '--settings', config, capture=True).split()
    if len(scaled) != 2:
        return
    render, output = scaled
    live = os.environ.get('BB_LIVE_RES') or settings_value(config, 'live_resolution') or '0'
    if live == 'auto':
        # The GPU check (tools/gpu_capabilities.c): live changes on strong discrete GPUs.
        caps = probe.parent / 'bb-gpu-capabilities.exe'
        try:
            live = subprocess.run([caps, '--live-resolution'], capture_output=True, text=True,
                                  timeout=60).stdout.strip() if caps.is_file() else '0'
        except (OSError, subprocess.SubprocessError):
            live = '0'
    if live == '1':
        print(f'Output {output}: live resolution changes (live_resolution=0: startup patch)')
        return
    os.environ.update(BB_RENDER_RES=render, BB_OUTPUT_RES=output, BB_AUTO_RENDER_RES='1')
    os.environ.setdefault('BB_DMEM_MB', '9152')
    print(f'Output {output}: scene {render}, direct memory {os.environ["BB_DMEM_MB"]} MiB '
          '(live_resolution=1: live changes)')


def prepare(game, out, data, config, probe):
    """The boot image, patches and the settings for one start (also after a restart)."""
    script('scripts/prepare.py', game, '--out', out)
    script('scripts/link_libc.py', game, '--out', out)
    script('scripts/link_modules.py', game, '--out', out)
    script('scripts/content_profile.py', game, '--out', out, '--sku', os.environ.get('BB_CONTENT_SKU', 'full'))
    script('windows/function_starts.py', game, '--out', out)
    choose_resolution(config, probe)
    fps = os.environ.get('BB_FPS', 'uncap')
    script('scripts/patches.py', '--out', out, '--fps', fps, '--extra', os.environ.get('BB_PATCHES', ''),
           '--settings', config, '--game-dir', game,
           '--render-res', os.environ.get('BB_RENDER_RES', ''), '--output-res', os.environ.get('BB_OUTPUT_RES', ''),
           '--patches-dir', os.environ.get('BB_PATCHES_DIR', data / 'patches'),
           '--patches-config', os.environ.get('BB_PATCHES_CONFIG', data / 'patches.json'))
    # The defaults run.sh documents (memory model, uploads, write tracking, command buffers).
    defaults = dict(BB_PREUPLOAD='1', BB_GUEST_IN_PLACE='0', BB_UFFD='0', BB_COPY_GPU_BUFFERS='1',
                    BB_GPU_WRITE_TWINS='1', BB_GPU_WRITE_TWINS_MAX='65536',
                    BB_VBLANK_HZ={'uncap': '480', '90': '90'}.get(fps, '60'))
    for name, value in defaults.items():
        os.environ.setdefault(name, value)


def start(probe, game, out, data, extra, log):
    """Runs bb-probe once; its exit code."""
    command = [probe, out / 'boot-linked.bin', '--content-profile', out / 'content.bin',
               '--patches', out / 'patches.bin', '--app0', game,
               '--user', os.environ.get('BB_USER_DIR', data / 'user'),
               '--timeout', os.environ.get('BB_TIMEOUT', '0'), *extra]
    if not log:
        # Explicit handles: on Windows a child only inherits redirected ones when they are passed.
        sys.stdout.flush()
        return subprocess.run(command, stdout=sys.stdout, stderr=sys.stderr).returncode
    with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as process:
        for line in process.stdout:
            sys.stdout.buffer.write(line)
            sys.stdout.flush()
            log.write(line)
            log.flush()
        return process.wait()


def main():
    sys.stdout.reconfigure(line_buffering=True)  # ordered with the scripts' and the game's output
    data = Path(os.environ.get('BB_DATA_DIR', ROOT)).resolve()
    out = data / 'out'
    out.mkdir(parents=True, exist_ok=True)
    config = Path(os.environ.setdefault('BB_CONFIG', str(data / 'bbport.ini')))
    # FSR 4.1.1 assets (tools/fsr4cap): next to the repository or in the data directory.
    if 'BB_FSR411_DIR' not in os.environ and not (ROOT / 'fsr4_411').is_dir() and (data / 'fsr4_411').is_dir():
        os.environ['BB_FSR411_DIR'] = str(data / 'fsr4_411')
    game = Path(os.environ.get('BB_GAME_DIR', ROOT.parent / 'CUSA03173'))
    if not (game / 'eboot.bin').is_file():
        fail(f'No eboot.bin in {game} (set BB_GAME_DIR).')
    probe = Path(os.environ.get('BB_PROBE', ROOT / 'out' / 'bb-probe.exe'))
    if not probe.is_file():
        fail(f'No {probe}: build it with "bash windows/build.sh" in an MSYS2 CLANG64 shell.')
    find_dlls(probe)

    log = None
    if os.environ.get('BB_SAVE_LOG') == '1':
        logs = data / 'logs'
        logs.mkdir(exist_ok=True)
        stamp = time.strftime('%Y%m%d_%H%M%S')
        os.environ.setdefault('BB_FRAME_STATS', '1')
        os.environ.setdefault('BB_FRAME_LOG', str(logs / f'{stamp}.frames.csv'))
        os.environ.setdefault('BB_READBACK_LOG', str(logs / f'{stamp}.readbacks.csv'))
        print(f'Log: {logs / stamp}.log')
        log = open(logs / f'{stamp}.log', 'wb')

    # The merged mod view lasts for this launch, restarts included.
    run_game, mod_view = merge_mods(game, out, data)
    try:
        while True:
            prepare(run_game, out, data, config, probe)
            code = start(probe, run_game, out, data, sys.argv[1:], log)
            if code != RESTART_EXIT_CODE:
                return code
            print('Runtime: restarting')
    except subprocess.CalledProcessError as error:
        print(f'{Path(str(error.cmd[1])).name if len(error.cmd) > 1 else error.cmd} failed (exit {error.returncode})',
              file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        return 130
    finally:
        if mod_view:
            remove_mod_view(mod_view, out, game)
        if log:
            log.close()


if __name__ == '__main__':
    sys.exit(main())
