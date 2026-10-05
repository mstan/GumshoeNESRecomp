#!/usr/bin/env python3
"""Gumshoe widescreen probe (docs/WIDESCREEN.md).

Runs a TRACE-free cycle build headless (and a hidden window over TCP with
SDL's dummy video driver) and reads the mod's per-frame log
(--widescreen-log); nothing arms a capture. Each check prints PASS/FAIL with
its numbers; the exit code is the number of failed checks.

    python tests/widescreen_probe.py --exe build_ws/Release/GumshoeRecomp.exe \
        --baseline <untouched stock build>/GumshoeRecomp.exe --out <new directory>

Checks (--checks a,b,...; default all):
  stock     full-machine hashes: baseline, the mod build plain, --widescreen
            off, the package installed but disabled, and every aspect/camera/
            HUD choice enabled, on the attract route and a Zapper route
  terrain   attract route (stages 1-4, PRG banks 0-2) at 16:9, 21:9, 32:9 and
            Fit (three drawables): composed frames, seam vs the native
            background, uncovered view columns, lookahead tiles/palettes
            verified against the game's own writes
  play      a long played route (Zapper autopilot fixture, the game's
            invincibility timer held): deaths, game over, restarts; ring
            wraparound with a 128-column ring at 16:9 and 21:9
  camera    centered keeps the native picture centered; edges starts each
            stage at the left edge and centers once the camera has room
  hud       the timer and shots at the canvas edge or at their native place
  objects   edge packets and left-edge residents
  savestate a state saved mid-stage and loaded in a new process continues
            with identical machine hashes, pictures and mod state
  mods      a saved Mods selection (21:9, centered camera, centered HUD)
  zapper    hidden window: mouse/window coordinates map through the
            compositor's native origin (anchored and centered), margins aim
            off the screen, the crosshair is drawn where the aim is
  fitlive   hidden window: Fit follows the drawable
"""
import argparse
import json
import os
import shutil
import socket
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WIDTHS = {'16:9': 426, '21:9': 560, '32:9': 854}
PACKAGE = 'gumshoe.enhancement.widescreen'
# Title: a pull opens the letter, a second one starts the game.
START = [(200, 128, 120, 1), (206, 128, 120, 0), (600, 128, 100, 1), (604, 128, 100, 0)]
# Two shots in the first stage: Stevenson (a hit, he jumps) and the sky (a miss).
SHOTS = START + [(1300, 96, 205, 1), (1304, 96, 205, 0), (1400, 128, 60, 1), (1404, 128, 60, 0)]
AUTOPILOT = ['--dev-autofire', '24', '--dev-hold', '0xDC:1']


def geometry_width(w, h):
    """common/nes_video_geometry.h, Fit."""
    aspect = min(max(w / h, 256 / 240), 32 / 9)
    width = 2 * int((240 * aspect) / 2 + 0.5)
    return max(256, min(864, width)) & ~1


class Probe:
    def __init__(self, args):
        self.a = args
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.rom = next(p for p in ROOT.glob('*.nes'))
        self.failures = []
        self.write_schedule('start.txt', START)
        self.write_schedule('shots.txt', SHOTS)

    def write_schedule(self, name, rows):
        (self.out / name).write_text(''.join('%d %d %d %d\n' % r for r in rows))

    # ---- running ----
    def run(self, tag, args, exe=None, frames=2400, log=True):
        d = self.out / tag
        d.mkdir(parents=True, exist_ok=True)
        cmd = [str(exe or self.a.exe), str(self.rom), '--frames', str(frames)] + [str(x) for x in args]
        if log:
            cmd += ['--widescreen-log', str(d / 'log.jsonl')]
        with open(d / 'out.txt', 'w') as f:
            f.write(' '.join(cmd) + '\n')
            f.flush()
            rc = subprocess.run(cmd, cwd=ROOT, stdout=f, stderr=subprocess.STDOUT).returncode
        if rc:
            raise RuntimeError(f'{tag}: exit {rc} (see {d / "out.txt"})')
        return d

    def runs(self, jobs):
        with ThreadPoolExecutor(self.a.jobs) as ex:
            return list(ex.map(lambda j: self.run(*j), jobs))

    @staticmethod
    def log(d):
        return [json.loads(line) for line in open(d / 'log.jsonl')]

    @staticmethod
    def hashes(path):
        return [line.split(' ', 1)[1] for line in open(path)]

    def check(self, name, ok, **numbers):
        print(('PASS ' if ok else 'FAIL ') + name + ' ' + json.dumps(numbers))
        if not ok:
            self.failures.append(name)

    @staticmethod
    def lookahead_totals(log):
        """Sums of the per-stage counters (they restart with each stage)."""
        keys = ('verified_tiles', 'mismatched_tiles', 'verified_palettes', 'mismatched_palettes', 'lookahead_failures')
        tot = dict.fromkeys(keys, 0)
        prev = None
        for d in log:
            if prev and d['valid'] and prev['valid'] and d['verified_tiles'] >= prev['verified_tiles']:
                for k in keys:
                    tot[k] += d[k] - prev[k]
            prev = d
        return tot

    def terrain_numbers(self, log):
        comp = [d for d in log if d['picture'] == 'composed']
        tot = self.lookahead_totals(log)
        return dict(frames=len(log), composed=len(comp), banks=sorted({d['bank'] for d in comp}),
                    seam_errors=max((d['seam_errors'] for d in comp), default=-1),
                    seam_unknown=max((d['seam_unknown'] for d in comp), default=-1),
                    margin_unknown=max((d['margin_unknown'] for d in comp), default=-1),
                    max_camera=max(d['camera'] for d in log), **tot,
                    object_failures=log[-1]['objects']['failures'] if log else -1)

    def terrain_ok(self, n, banks=None):
        return (n['composed'] > 0 and n['seam_errors'] == 0 and n['seam_unknown'] == 0 and n['margin_unknown'] == 0 and
                n['mismatched_tiles'] == 0 and n['mismatched_palettes'] == 0 and n['lookahead_failures'] == 0 and
                n['verified_tiles'] > 0 and n['object_failures'] == 0 and (banks is None or set(banks) <= set(n['banks'])))

    # ---- checks ----
    def c_stock(self):
        if not self.a.baseline:
            raise RuntimeError('stock needs --baseline')
        disabled = self.out / 'mods_disabled'
        shutil.rmtree(disabled, ignore_errors=True)
        shutil.copytree(ROOT / 'mods' / 'preloaded', disabled)
        (disabled / 'state.toml').write_text(
            'format_version = 1\n\n[[package]]\nid = "%s"\nversion = "1.0.0"\n\n[[feature]]\npackage_id = "%s"\n'
            'id = "widescreen"\nenabled = false\n' % (PACKAGE, PACKAGE))
        variants = [('plain', []), ('off', ['--widescreen', 'off']), ('disabled', ['--mods-root', disabled]),
                    ('w16', ['--widescreen', '16:9']), ('w21c', ['--widescreen', '21:9', '--widescreen-camera', 'centered']),
                    ('w32h', ['--widescreen', '32:9', '--widescreen-hud', 'center']),
                    ('wfit', ['--widescreen', 'fit', '--present-size', '3440x1440'])]
        for route, frames, extra in (('attract', 4000, []), ('shots', 1500, ['--zapper-input', self.out / 'shots.txt'])):
            jobs = [(f'stock_{route}_base', extra + ['--hash-out', self.out / f'stock_{route}_base.txt'],
                     self.a.baseline, frames, False)]
            for tag, args in variants:
                jobs.append((f'stock_{route}_{tag}', extra + args + ['--hash-out', self.out / f'stock_{route}_{tag}.txt'],
                             None, frames, False))
            self.runs(jobs)
            base = self.hashes(self.out / f'stock_{route}_base.txt')
            for tag, _ in variants:
                h = self.hashes(self.out / f'stock_{route}_{tag}.txt')
                same = sum(a == b for a, b in zip(base, h))
                self.check('stock', len(h) == len(base) == frames and same == frames, route=route, variant=tag,
                           identical=f'{same}/{frames}')

    def c_terrain(self):
        presets = [('16:9', []), ('21:9', []), ('32:9', []), ('fit', ['--present-size', '1920x1080']),
                   ('fit', ['--present-size', '3440x1440']), ('fit', ['--present-size', '5120x1440'])]
        jobs = [(f'terrain_{i}', ['--widescreen', a] + extra, None, 12000) for i, (a, extra) in enumerate(presets)]
        for (a, extra), d in zip(presets, self.runs(jobs)):
            log = self.log(d)
            n = self.terrain_numbers(log)
            want = WIDTHS.get(a) or geometry_width(*map(int, extra[1].split('x')))
            widths = sorted({x['render_width'] for x in log})
            self.check('terrain', self.terrain_ok(n, banks=[0, 1, 2]) and widths == [want], aspect=a, extra=extra,
                       width=widths, **n)

    def c_play(self):
        presets = [('16:9', ['--widescreen-ring', '128']), ('21:9', ['--widescreen-ring', '128']), ('32:9', [])]
        jobs = [(f'play_{i}', ['--widescreen', a, '--zapper-input', self.out / 'start.txt'] + AUTOPILOT + extra, None, 25000)
                for i, (a, extra) in enumerate(presets)]
        for (a, extra), d in zip(presets, self.runs(jobs)):
            log = self.log(d)
            n = self.terrain_numbers(log)
            stages = sum(1 for p, q in zip(log, log[1:]) if q['stage_mode'] == 3 and p['stage_mode'] != 3)
            ring = log[-1]['ring'] if log else 0
            wrapped = n['max_camera'] > ring * 8 - 300
            self.check('play', self.terrain_ok(n) and stages >= 3 and (not extra or wrapped), aspect=a, extra=extra,
                       stages=stages, ring=ring, wrapped=wrapped, **n)

    def c_camera(self):
        jobs = [(f'camera_{c}', ['--widescreen', '32:9', '--widescreen-camera', c], None, 4000) for c in ('centered', 'edges')]
        cen, edg = (self.log(d) for d in self.runs(jobs))
        comp = [d for d in cen if d['picture'] == 'composed']
        self.check('camera', comp and all(d['native_x0'] == 299 for d in comp), mode='centered', composed=len(comp),
                   x0=sorted({d['native_x0'] for d in comp}))
        comp = [d for d in edg if d['picture'] == 'composed']
        starts = [d for p, d in zip(edg, edg[1:]) if d['picture'] == 'composed' and p['picture'] != 'composed']
        ok = comp and all(d['native_x0'] == min(d['render_camera'], 299) for d in comp) and starts and \
            all(d['native_x0'] == 0 for d in starts)
        self.check('camera', ok, mode='edges', composed=len(comp), stage_starts=len(starts),
                   follows=sum(d['native_x0'] == min(d['render_camera'], 299) for d in comp))

    def hud_frame(self, tag, args):
        from PIL import Image
        d = self.run(tag, ['--widescreen', '32:9', '--zapper-input', self.out / 'start.txt', '--present-out',
                           self.out / tag / 'present.png', '--screenshot', self.out / tag / 'native.png'] + args,
                     frames=1500)
        last = self.log(d)[-1]
        return Image.open(d / 'present.png').convert('RGB'), Image.open(d / 'native.png').convert('RGB'), last

    def c_hud(self):
        box = (48, 16, 80, 32)   # timer and shots, native columns
        for hud in ('edges', 'center'):
            present, native, last = self.hud_frame(f'hud_{hud}', ['--widescreen-hud', hud])
            x0 = last['native_x0']
            n = native.crop(box).tobytes()
            at_edge = present.crop(box).tobytes() == n
            at_native = present.crop((x0 + box[0], box[1], x0 + box[2], box[3])).tobytes() == n
            ok = last['picture'] == 'composed' and x0 > 0 and (at_edge and not at_native if hud == 'edges' else at_native)
            self.check('hud', ok, hud=hud, native_x0=x0, at_edge=at_edge, at_native_place=at_native)

    def c_objects(self):
        log = self.log(self.run('objects', ['--widescreen', '32:9'], frames=4000))
        o = log[-1]['objects']
        packets = sum(1 for d in log if d['objects']['packets'])
        moving = []
        for p, q in zip(log, log[1:]):
            if p['objects']['resident_x'] and q['objects']['resident_x'] and len(p['objects']['resident_x']) == len(q['objects']['resident_x']):
                moving.append(q['objects']['resident_x'][0] - p['objects']['resident_x'][0])
        self.check('objects', packets > 100 and o['residents_started'] > 0 and o['failures'] == 0 and moving and
                   sum(moving) < 0, frames_with_packets=packets, residents_started=o['residents_started'],
                   resident_steps=o['resident_steps'], resident_motion=sum(moving), failures=o['failures'])

    def c_savestate(self):
        a = self.out / 'save_a'
        b = self.out / 'save_b'
        for d in (a, b):
            shutil.rmtree(d, ignore_errors=True)
            d.mkdir(parents=True)
        common = ['--widescreen', '32:9', '--present-every', '50']
        self.run('save_a', common + ['--zapper-input', self.out / 'shots.txt', '--save-state', f'1350:{a / "s.state"}',
                                     '--hash-out', a / 'hash.txt', '--present-out', a / 'p.png'], frames=2000)
        self.run('save_b', common + ['--zapper-input', self.out / 'shots.txt', '--load-state', a / 's.state',
                                     '--hash-out', b / 'hash.txt', '--present-out', b / 'p.png'], frames=2000)
        ha = {l.split(' ', 1)[0]: l for l in open(a / 'hash.txt')}
        hb = [l for l in open(b / 'hash.txt')]
        same = sum(ha.get(l.split(' ', 1)[0]) == l for l in hb)
        pics = sorted(p.name for p in b.glob('p_*.png'))
        same_pics = sum((a / p).read_bytes() == (b / p).read_bytes() for p in pics if (a / p).exists())
        la = {d['frame']: d for d in self.log(a)}
        keys = ('picture', 'camera', 'render_camera', 'native_x0', 'head', 'stage_end', 'seam_errors', 'margin_unknown')
        lb = self.log(b)
        same_state = sum(all(la[d['frame']][k] == d[k] for k in keys) and
                         la[d['frame']]['objects']['packets'] == d['objects']['packets'] and
                         la[d['frame']]['objects']['resident_x'] == d['objects']['resident_x']
                         for d in lb if d['frame'] in la)
        self.check('savestate', hb and same == len(hb) and pics and same_pics == len(pics) and same_state == len(lb),
                   hashes=f'{same}/{len(hb)}', pictures=f'{same_pics}/{len(pics)}', mod_state=f'{same_state}/{len(lb)}')

    def c_mods(self):
        root = self.out / 'mods_saved'
        shutil.rmtree(root, ignore_errors=True)
        shutil.copytree(ROOT / 'mods' / 'preloaded', root)
        (root / 'state.toml').write_text(
            'format_version = 1\n\n[[package]]\nid = "%s"\nversion = "1.0.0"\n\n[[feature]]\npackage_id = "%s"\n'
            'id = "widescreen"\nenabled = true\n\n[feature.values]\naspect = "21-9"\ncamera = "centered"\n'
            'hud = "center"\n' % (PACKAGE, PACKAGE))
        last = self.log(self.run('mods', ['--mods-root', root], frames=1200))[-1]
        self.check('mods', last['enabled'] and last['render_width'] == 560 and last['hud_edges'] == 0 and
                   last['room_edges'] == 0 and last['wide_frames'] > 0, width=last['render_width'],
                   hud_edges=last['hud_edges'], room_edges=last['room_edges'], composed=last['wide_frames'])

    # ---- the window ----
    def window(self, tag, extra):
        port = self.a.port
        with socket.socket() as s:
            if s.connect_ex(('127.0.0.1', port)) == 0:
                raise RuntimeError(f'port {port} is in use')
        env = dict(os.environ, SDL_VIDEODRIVER='dummy', SDL_AUDIODRIVER='dummy', NESRECOMP_NO_LAUNCHER='1')
        d = self.out / tag
        d.mkdir(exist_ok=True)
        cfg = d / 'config.ini'
        cfg.write_text('')
        log = open(d / 'out.txt', 'w')
        proc = subprocess.Popen([str(self.a.exe), str(self.rom), '--hidden', '--tcp', str(port), '--config', str(cfg)] +
                                [str(x) for x in extra], cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT)
        return d, proc, log

    def client(self, proc, d):
        sock = None
        for _ in range(200):
            if proc.poll() is not None:
                raise RuntimeError(f'the window exited ({proc.returncode}, see {d / "out.txt"})')
            try:
                sock = socket.create_connection(('127.0.0.1', self.a.port), timeout=10)
                break
            except OSError:
                time.sleep(0.1)
        if sock is None:
            raise RuntimeError('no TCP server')
        f = sock.makefile('rw')
        ident = [0]

        def cmd(name, **kw):
            ident[0] += 1
            f.write(json.dumps(dict(cmd=name, id=ident[0], **kw)) + '\n')
            f.flush()
            while True:
                r = json.loads(f.readline())
                if r.get('id') == ident[0]:
                    if not r.get('ok', True):
                        raise RuntimeError(f'{name}: {r.get("err")}')
                    return r

        def wait_frames(n):
            start = cmd('ping')['frame']
            while cmd('ping')['frame'] < start + n:
                time.sleep(0.01)

        return cmd, wait_frames

    def close(self, proc, log, cmd=None):
        try:
            if cmd:
                cmd('quit')
            proc.wait(timeout=20)
        except Exception:
            proc.kill()
        log.close()

    def c_zapper(self):
        from PIL import Image
        d, proc, log = self.window('zapper', ['--widescreen', '32:9'])
        cmd = None
        try:
            cmd, wait_frames = self.client(proc, d)
            cmd('window_size', w=1708, h=480)   # the 854 x 240 picture at 2x
            wait_frames(3)
            # The title and the letter take a pull each, from native aims.
            for _ in range(2):
                while cmd('ping')['frame'] < 200:
                    time.sleep(0.01)
                cmd('zapper', x=128, y=120, trigger=True)
                wait_frames(4)
                cmd('zapper', x=128, y=120, trigger=False)
                wait_frames(400)
            while cmd('gumshoe_ws_state')['stage_mode'] != 4:
                wait_frames(10)
            for phase in ('stage start', 'centered'):
                if phase == 'centered':
                    while cmd('gumshoe_ws_state')['native_x0'] < 299 and cmd('gumshoe_ws_state')['stage_mode'] == 4:
                        wait_frames(20)
                # The window keeps running: each reply's own origin is the one
                # its mapping used.
                st = cmd('gumshoe_ws_state')
                x0 = st['native_x0']
                inside = cmd('zapper', window_x=2 * (x0 + 100) + 1, window_y=2 * 120 + 1, trigger=False)
                want_inside = [x0 + 100 - inside['native_origin'], 120]
                margin_x = x0 + 256 + 40 if x0 + 256 + 40 < 854 else x0 - 40
                outside = cmd('zapper', window_x=2 * margin_x + 1, window_y=2 * 120 + 1, trigger=False)
                back = cmd('zapper', window_x=2 * (x0 + 100) + 1, window_y=2 * 120 + 1, trigger=False)
                want_back = [x0 + 100 - back['native_origin'], 120]
                wait_frames(2)
                shot = d / f'ui_{phase.replace(" ", "_")}.png'
                cmd('screenshot', layer='ui', path=str(shot))
                for _ in range(100):
                    if shot.exists():
                        break
                    time.sleep(0.05)
                time.sleep(0.2)
                im = Image.open(shot).convert('RGB')
                cx, cy = 2 * (x0 + 100), 2 * 120
                white = sum(im.getpixel((cx + dx, cy)) == (255, 255, 255) for dx in range(-6, 7))
                ok = (st['picture'] == 'composed' and abs(inside['native_origin'] - x0) <= 2 and
                      inside['aim'] == want_inside and outside['aim'] == [-1, -1] and back['aim'] == want_back and
                      white >= 4 and inside['picture_width'] == 854)
                self.check('zapper', ok, phase=phase, native_x0=x0, aim_inside=inside['aim'], expected=want_inside,
                           aim_margin=outside['aim'], origin=inside['native_origin'], crosshair_pixels=white)
            cmd('zapper', mouse=True)
        finally:
            self.close(proc, log, cmd)

    def c_fitlive(self):
        d, proc, log = self.window('fitlive', ['--widescreen', 'fit'])
        cmd = None
        try:
            cmd, wait_frames = self.client(proc, d)
            for w, h in [(1280, 720), (1920, 800), (768, 720), (2560, 1080), (3840, 1080), (5120, 1080), (800, 800),
                         (1024, 768)]:
                cmd('window_size', w=w, h=h)
                wait_frames(3)
                v = cmd('video')
                dw, dh = v['drawable']
                want = geometry_width(dw, dh)
                self.check('fitlive', v['width'] == want, window=f'{w}x{h}', drawable=f'{dw}x{dh}', width=v['width'],
                           expected=want)
        finally:
            self.close(proc, log, cmd)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--exe', required=True)
    ap.add_argument('--baseline', help='an untouched stock build (for the stock check)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--checks', default='stock,terrain,play,camera,hud,objects,savestate,mods,zapper,fitlive')
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--port', type=int, default=4391)
    a = ap.parse_args()
    a.exe = str(Path(a.exe).resolve())
    if a.baseline:
        a.baseline = str(Path(a.baseline).resolve())
    p = Probe(a)
    for name in a.checks.split(','):
        try:
            getattr(p, 'c_' + name)()
        except Exception as e:  # a check that cannot run is a failed check
            p.check(name, False, error=str(e))
    print(f'{len(p.failures)} failed' if p.failures else 'ALL PASS')
    sys.exit(len(p.failures))


if __name__ == '__main__':
    main()
