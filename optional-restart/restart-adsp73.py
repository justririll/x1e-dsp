#!/usr/bin/env python3
# Coordinated ADSP restart for the Vivobook S15 on 7.3 (qcom_x1_el2_pas, stock modules with
# the msm/0022 remove fixes). Detaches the audio stack, stops CDSP then ADSP, starts ADSP
# then CDSP, restores modules and user audio units. Logs to /root/adsp73-<time>/.
import fcntl, json, os, subprocess, sys, time
from pathlib import Path

run = Path('/root/adsp73-' + time.strftime('%Y%m%d-%H%M%S')); run.mkdir()
lock = open('/run/restart-adsp73.lock', 'w'); fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
PLAT = Path('/sys/bus/platform/devices')
UNITS = ['pipewire-pulse.socket', 'pipewire.socket', 'pipewire-pulse.service',
         'wireplumber.service', 'pipewire.service']
# unload order: consumers first
UNLOAD = ['snd_soc_x1e80100', 'snd_soc_wsa884x', 'snd_soc_wcd938x', 'snd_soc_wcd938x_sdw',
          'soundwire_qcom', 'snd_soc_lpass_wsa_macro', 'snd_soc_lpass_tx_macro',
          'snd_soc_lpass_rx_macro', 'snd_soc_lpass_va_macro', 'pinctrl_sm8550_lpass_lpi',
          'q6apm_lpass_dais', 'q6apm_dai', 'q6prm_clocks', 'q6prm', 'snd_q6apm', 'qcom_battmgr']
CODEC_CLKS = ['mclk', 'wsa2-mclk', 'lpass-tx-mclk', 'fsgen']

def log(*a):
    print(*a, flush=True)
    with open(run / 'log.txt', 'a') as f: print(*a, file=f)

def cmd(args, check=True):
    log('$', ' '.join(map(str, args)))
    return subprocess.run(args, check=check)

def userctl(uid, args):
    env = dict(os.environ, XDG_RUNTIME_DIR=f'/run/user/{uid}',
               DBUS_SESSION_BUS_ADDRESS=f'unix:path=/run/user/{uid}/bus')
    return subprocess.run(['setpriv', f'--reuid={uid}', '--regid={}'.format(Path(f'/run/user/{uid}').stat().st_gid),
                           '--init-groups', 'systemctl', '--user', *args], env=env)

def rproc(name):
    r = [p for p in Path('/sys/class/remoteproc').iterdir() if (p / 'name').read_text().strip() == name]
    assert len(r) == 1, name
    return r[0]

def state(r): return (r / 'state').read_text().strip()

def set_state(r, s, want, timeout=15):
    log('rproc', (r / 'name').read_text().strip(), '->', s)
    (r / 'state').write_text(s)
    for _ in range(timeout * 10):
        if state(r) == want: return
        time.sleep(0.1)
    raise RuntimeError(f'{r} stuck in {state(r)}, wanted {want}')

def unbind(dev):
    d = PLAT / dev / 'driver'
    if d.exists():
        log('unbind', dev); (d.resolve() / 'unbind').write_text(dev)

users, failure = [], None
adsp, cdsp = rproc('adsp'), rproc('cdsp')
log('before', state(adsp), state(cdsp))
subprocess.run(['dmesg', '-C'])
try:
    for d in Path('/run/user').iterdir():
        if d.name.isdigit() and d.name != '0' and (d / 'bus').exists():
            uid = int(d.name)
            active = [u for u in UNITS if userctl(uid, ['is-active', '--quiet', u]).returncode == 0]
            if active:
                users.append((uid, active)); userctl(uid, ['stop', *UNITS])
    (run / 'users.json').write_text(json.dumps(users))
    upower = subprocess.run(['systemctl', 'is-active', '--quiet', 'upower']).returncode == 0
    if upower: cmd(['systemctl', 'stop', 'upower'])

    unbind('sound')
    for dev in ['6ad0000.soundwire', '6b10000.soundwire', '6d30000.soundwire', '6e80000.pinctrl']:
        unbind(dev)
    for m in UNLOAD:
        if Path('/sys/module', m, 'refcnt').exists(): cmd(['rmmod', m])
    leaked = [c for c in CODEC_CLKS if Path('/sys/kernel/debug/clk', c).exists()]
    if leaked: raise RuntimeError(f'codec clocks still registered: {leaked}')

    (cdsp / 'recovery').write_text('disabled')
    set_state(cdsp, 'stop', 'offline')
    set_state(adsp, 'stop', 'offline')
    time.sleep(1)
    set_state(adsp, 'start', 'running')
    time.sleep(2)
    set_state(cdsp, 'start', 'running')
    (cdsp / 'recovery').write_text('enabled')
except BaseException as e:
    failure = e; log('RESTART ERROR', repr(e))
finally:
    for m in reversed(UNLOAD):
        cmd(['modprobe', m], check=False)
    for dev in ['6e80000.pinctrl', '6d44000.codec', '6ac0000.codec', '6ae0000.codec', '6aa0000.codec',
                '6b00000.codec', '6ad0000.soundwire', '6b10000.soundwire', '6d30000.soundwire', 'sound']:
        if (PLAT / dev).exists() and not (PLAT / dev / 'driver').exists():
            try: Path('/sys/bus/platform/drivers_probe').write_text(dev)
            except OSError as e: log('reprobe', dev, e)
    for _ in range(60):
        if 'X1E80100' in Path('/proc/asound/cards').read_text(): break
        time.sleep(0.5)
    else: failure = failure or RuntimeError('ALSA card did not come back')
    if 'upower' in dir() and upower: cmd(['systemctl', 'start', 'upower'], check=False)
    for uid, active in users: userctl(uid, ['start', *active])
    (run / 'dmesg.txt').write_text(subprocess.run(['dmesg'], capture_output=True, text=True).stdout)
    log('after', state(adsp), state(cdsp), 'card' if 'X1E80100' in Path('/proc/asound/cards').read_text() else 'NO CARD')
    if failure: log('FAILED', repr(failure)); sys.exit(1)
    log('OK')
