#!/usr/bin/env python3
"""MicroTech 模拟器开发助手：数字菜单，零参数。

用法：
    python3 sim/dev.py

所有产物固定在 build/ 下；会话端口 5002；截图与导树一步完成。
"""
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

SIM_ROOT = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(SIM_ROOT)
BUILD = os.path.join(ROOT, 'build', 'sim')
BINARY = os.path.join(BUILD, 'microtech_sim')
RES_DIR = os.path.join(BUILD, 'sim_res_fs')
NVS_DIR = os.path.join(BUILD, 'dev_nvs')
OUT_DIR = os.path.join(BUILD, 'shots')
PID_FILE = os.path.join(BUILD, 'dev.pid')
LOG_FILE = os.path.join(BUILD, 'dev_session.log')
PORT = 5002
APPS = ['home', 'menu', 'clock', 'level', 'diagnostics', 'recorder',
        'settings', 'setup', 'weather']

sys.path.insert(0, os.path.join(SIM_ROOT, 'tools'))
from simctl import rpc  # noqa: E402


# ------------------------------------------------------------- 会话工具

def _pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def session_pid():
    if not os.path.exists(PID_FILE):
        return None
    try:
        pid = int(open(PID_FILE).read().strip())
    except ValueError:
        return None
    return pid if _pid_alive(pid) else None


def agent_ready():
    try:
        sock = socket.create_connection(('127.0.0.1', PORT), timeout=1)
        sock.close()
        return True
    except OSError:
        return False


def running():
    return session_pid() is not None and agent_ready()


def build():
    """增量编译；首次自动 configure。返回 True=成功。"""
    if not os.path.exists(os.path.join(BUILD, 'build.ninja')):
        print('  首次配置 cmake ...')
        rc = subprocess.call(['cmake', '-S', os.path.join(ROOT, 'sim'),
                              '-B', BUILD, '-G', 'Ninja'],
                             cwd=ROOT)
        if rc != 0:
            print('  configure 失败')
            return False
    proc = subprocess.run(['cmake', '--build', BUILD], cwd=ROOT,
                          capture_output=True, text=True)
    if proc.returncode != 0:
        tail = (proc.stdout + proc.stderr).splitlines()[-20:]
        print('  编译失败：')
        for line in tail:
            print('   ', line)
        return False
    return True


def start_session(navigate=None, headless=False):
    """后台启动常驻会话；navigate=app 时启动后自动跳转。"""
    if running():
        print('  会话已在运行（端口 %d）。先选 8 停止，或复用当前会话。' % PORT)
        return False
    if not build():
        return False
    os.makedirs(OUT_DIR, exist_ok=True)
    common = [BINARY, '--res-dir', RES_DIR, '--nvs-dir', NVS_DIR,
              '--out-dir', OUT_DIR, '--agent-port', str(PORT)]
    if headless or not os.environ.get('DISPLAY'):
        cmd = [BINARY, '--headless', '--ci', '--res-dir', RES_DIR,
               '--nvs-dir', NVS_DIR, '--out-dir', OUT_DIR,
               '--agent-port', str(PORT)]
    else:
        cmd = common + ['--window-scale', '1']
    if '--ci' in cmd and not headless:
        print('  未检测到 DISPLAY，转为无头驻留（可用 simctl 驱动）')
    print('  启动会话：%s' % ('无头驻留' if '--ci' in cmd else '窗口 1:1'))
    log = open(LOG_FILE, 'w')
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    open(PID_FILE, 'w').write(str(proc.pid))
    if not _wait_socket(60):
        print('  会话未就绪，日志末尾：')
        _dump_log()
        return False
    mode = 'ci' if '--ci' in cmd else 'window'
    print('  会话就绪 pid=%d（模式 %s，日志 %s）' % (proc.pid, mode, LOG_FILE))
    if navigate:
        _navigate(navigate)
    return True


def _wait_socket(seconds):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if agent_ready():
            return True
        pid = session_pid()
        if pid is None:
            return False
        time.sleep(0.5)
    return False


def _dump_log():
    if os.path.exists(LOG_FILE):
        for line in open(LOG_FILE, errors='replace').read().splitlines()[-10:]:
            print('   ', line)


def stop_session():
    pid = session_pid()
    if pid is None:
        print('  没有运行中的会话')
        return
    try:
        sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
        rpc(sock, 'sim.exit')
        sock.close()
    except OSError:
        pass
    for _ in range(12):
        if not _pid_alive(pid):
            break
        time.sleep(0.5)
    if _pid_alive(pid):
        os.kill(pid, signal.SIGKILL)
    if os.path.exists(PID_FILE):
        os.remove(PID_FILE)
    print('  会话已停止')


def _connect():
    return socket.create_connection(('127.0.0.1', PORT), timeout=60)


def _navigate(app):
    sock = _connect()
    try:
        reply = rpc(sock, 'sim.navigate', {'app': app})
        rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
    finally:
        sock.close()
    if not reply.get('ok'):
        print('  导航失败：%s' % reply)


# ------------------------------------------------------------- 场景助手

def do_on_session(app, action):
    """在既有或临时无头会话上执行 action(sock)。app=None 表示不跳转。"""
    if app is not None and app not in APPS:
        print('  未知 app：%s' % app)
        return False
    temp = not running()
    if temp:
        if not start_session(headless=True):
            return False
        pid = session_pid()
    else:
        pid = None
    try:
        sock = _connect()
        try:
            if app:
                rpc(sock, 'sim.navigate', {'app': app})
            rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
            return action(sock)
        finally:
            sock.close()
    finally:
        if temp and pid and _pid_alive(pid):
            stop_session()


# ------------------------------------------------------------- 菜单项

def pick_app(default_current=False):
    if default_current:
        print('  0 当前页面')
    for idx, name in enumerate(APPS, 1):
        print('  %d %s' % (idx, name))
    raw = input('  选择页面: ').strip()
    if default_current and (raw == '' or raw == '0'):
        return None
    if not raw.isdigit() or not (1 <= int(raw) <= len(APPS)):
        print('  无效选择')
        return False
    return APPS[int(raw) - 1]


def cmd_run(navigate=None):
    start_session(navigate=navigate, headless=False)


def cmd_headless():
    if start_session(headless=True):
        print('  常用驱动：')
        print('    python3 sim/tools/simctl.py navigate clock')
        print('    python3 sim/tools/simctl.py step 33')
        print('    python3 sim/tools/simctl.py wait')
        print('    python3 sim/tools/simctl.py tree > /tmp/tree.json')
        print('    python3 sim/tools/simctl.py screenshot x.png')


def cmd_shot():
    app = pick_app(default_current=True)
    if app is False:
        return
    name = (app or 'current') + '.png'

    def action(sock):
        reply = rpc(sock, 'sim.screenshot', {'name': name, 'wait_idle': True})
        if reply.get('ok'):
            print('  截图：%s' % reply['result']['path'])
        else:
            print('  截图失败：%s' % reply)
        return reply.get('ok')

    do_on_session(app, action)


def cmd_tree():
    app = pick_app(default_current=True)
    if app is False:
        return
    holder = {}

    def action(sock):
        reply = rpc(sock, 'sim.tree')
        holder['tree'] = reply.get('result', {}).get('tree')
        return holder['tree'] is not None

    if do_on_session(app, action) and holder.get('tree'):
        path = os.path.join(BUILD, 'dev_tree.json')
        with open(path, 'w') as fh:
            json.dump(holder['tree'], fh, ensure_ascii=False, indent=1)
        _print_tree_summary(holder['tree'])
        print('  完整树已写入 %s' % path)


def _print_tree_summary(tree, depth=0, limit=18):
    count = [0]

    def walk(node):
        if count[0] >= limit:
            return
        text = node.get('text')
        box = node.get('coords', {})
        count[0] += 1
        print('    %-10s %s %s%s' % (node.get('type'),
                                      '[%s,%s %sx%s]' % (box.get('x'),
                                                          box.get('y'),
                                                          box.get('w'),
                                                          box.get('h')),
                                      repr(text)[:40] if text else '',
                                      ' (hidden)' if not node.get(
                                          'flags', {}).get('visible', True)
                                      else ''))
        for child in node.get('children', []):
            walk(child)

    print('  控件树概览（前 %d 个节点）：' % limit)
    walk(tree)


def cmd_ci(update=False):
    script = os.path.join(SIM_ROOT, 'ci', 'run_ci.sh')
    argv = [script, BUILD] + (['--update'] if update else [])
    if update:
        answer = input('  重新生成金样需要人工 review 后入库。继续? [y/N] ')
        if answer.strip().lower() != 'y':
            return
    subprocess.call(argv, cwd=ROOT)


def cmd_reset():
    stop_session()
    if os.path.isdir(NVS_DIR):
        shutil.rmtree(NVS_DIR)
        print('  设备状态已重置（dev_nvs 清空）')
    else:
        print('  无需重置')


MENU = """
──────────── MicroTech 模拟器 ────────────
 1  运行（窗口 1:1，自动增量编译）
 2  运行并直达某页面…
 3  无头驻留（simctl 驱动）
 4  截图某页面 → build/sim/shots/
 5  导出某页面控件树
 6  CI 回归
 7  重新生成金样（UI 变更后）
 8  停止当前会话
 9  重置设备状态
（PM 息屏默认关闭；测息屏用 simctl pm --off-ms 15000 / 场景 pm_lifecycle）
 0  退出
"""


def main():
    print('工作目录：%s' % ROOT)
    while True:
        print(MENU)
        state = '运行中' if running() else '未运行'
        choice = input('  请选择 [%s]: ' % state).strip()
        try:
            if choice == '1':
                cmd_run()
            elif choice == '2':
                app = pick_app()
                if app:
                    cmd_run(navigate=app)
            elif choice == '3':
                cmd_headless()
            elif choice == '4':
                cmd_shot()
            elif choice == '5':
                cmd_tree()
            elif choice == '6':
                cmd_ci()
            elif choice == '7':
                cmd_ci(update=True)
            elif choice == '8':
                stop_session()
            elif choice == '9':
                cmd_reset()
            elif choice in ('0', 'q'):
                return
            else:
                print('  无效选项')
        except KeyboardInterrupt:
            print()
        except EOFError:
            return


if __name__ == '__main__':
    main()
