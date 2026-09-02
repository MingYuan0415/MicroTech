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
LAST_NETWORK_ONLINE = False
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
    except (OSError, ValueError):
        try:
            os.remove(PID_FILE)
        except OSError:
            pass
        return None
    if _pid_alive(pid):
        return pid
    try:
        os.remove(PID_FILE)
    except OSError:
        pass
    return None


def agent_ready():
    try:
        sock = socket.create_connection(('127.0.0.1', PORT), timeout=1)
        sock.close()
        return True
    except OSError:
        return False


def running():
    return session_pid() is not None and agent_ready()


def session_summary():
    pid = session_pid()
    if pid is None:
        return '未运行'
    if not agent_ready():
        return '启动中 pid=%d' % pid
    try:
        sock = _connect(timeout=1)
        try:
            reply = rpc(sock, 'sim.ping')
        finally:
            sock.close()
    except OSError:
        return '连接中断 pid=%d' % pid
    if not reply.get('ok'):
        return '运行中 pid=%d（状态不可用）' % pid
    result = reply.get('result', {})
    mode = 'CI' if result.get('ci') else '自由运行'
    network = '联网' if result.get('network_ready') else '离线'
    app = result.get('active_app') or '未知页面'
    return '运行中 pid=%d（%s，%s，%s）' % (pid, mode, network, app)


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


def _pick_network():
    print('  网络模式：')
    print('    1  联网（真实 HTTP/HTTPS 请求）')
    print('    2  离线（不发起天气请求）')
    raw = input('  选择网络 [2]: ').strip()
    if raw in ('', '2'):
        return False
    if raw == '1':
        return True
    print('  无效选择')
    return None


def _terminate_process(pid):
    if not _pid_alive(pid):
        return
    try:
        os.kill(pid, signal.SIGTERM)
    except OSError:
        return
    for _ in range(20):
        if not _pid_alive(pid):
            return
        time.sleep(0.1)
    try:
        os.kill(pid, signal.SIGKILL)
    except OSError:
        pass


def _clear_pid_file():
    try:
        os.remove(PID_FILE)
    except OSError:
        pass


def start_session(navigate=None, headless=False, online=False):
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
        cmd = common + ['--headless']
        if not online:
            cmd.append('--ci')
    else:
        cmd = common + ['--window-scale', '1']
    if '--headless' in cmd and not headless:
        print('  未检测到 DISPLAY，转为无头驻留（可用 simctl 驱动）')
    if '--ci' in cmd:
        launch_mode = '无头离线驻留'
    elif '--headless' in cmd:
        launch_mode = '无头联网驻留'
    else:
        launch_mode = '固定尺寸窗口'
    print('  启动会话：%s' % launch_mode)
    log = open(LOG_FILE, 'w')
    proc = subprocess.Popen(cmd, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)
    log.close()
    open(PID_FILE, 'w').write(str(proc.pid))
    if not _wait_socket(60):
        print('  会话未就绪，日志末尾：')
        _dump_log()
        _terminate_process(proc.pid)
        _clear_pid_file()
        return False
    mode = 'ci' if '--ci' in cmd else (
        'headless' if '--headless' in cmd else 'window')
    network = '联网' if online else '离线'
    print('  会话就绪 pid=%d（模式 %s，%s，日志 %s）' %
          (proc.pid, mode, network, LOG_FILE))
    sock = _connect()
    try:
        reply = rpc(sock, 'sim.set_wifi',
                    {'state': 'connected' if online else 'disconnected'})
        if not reply.get('ok'):
            print('  网络状态设置失败：%s' % reply)
            _terminate_process(proc.pid)
            _clear_pid_file()
            return False
    finally:
        sock.close()
    if navigate:
        if not _navigate(navigate):
            return False
    global LAST_NETWORK_ONLINE
    LAST_NETWORK_ONLINE = online
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
    _terminate_process(pid)
    if os.path.exists(PID_FILE):
        _clear_pid_file()
    print('  会话已停止')


def _connect(timeout=60):
    return socket.create_connection(('127.0.0.1', PORT), timeout=timeout)


def _navigate(app):
    sock = _connect(timeout=5)
    try:
        reply = rpc(sock, 'sim.navigate', {'app': app})
        if not reply.get('ok'):
            print('  导航失败：%s' % reply)
            return False
        settled = rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
        if not settled.get('ok'):
            print('  页面未稳定：%s' % settled)
            return False
    finally:
        sock.close()
    return True


# ------------------------------------------------------------- 场景助手

def do_on_session(app, action):
    """在既有或临时无头会话上执行 action(sock)。app=None 表示不跳转。"""
    if app is not None and app not in APPS:
        print('  未知 app：%s' % app)
        return False
    temp = not running()
    if temp:
        if not start_session(headless=True, online=LAST_NETWORK_ONLINE):
            return False
        pid = session_pid()
    else:
        pid = None
    try:
        sock = _connect()
        try:
            if app:
                reply = rpc(sock, 'sim.navigate', {'app': app})
                if not reply.get('ok'):
                    print('  导航失败：%s' % reply)
                    return False
            settled = rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
            if not settled.get('ok'):
                print('  页面未稳定：%s' % settled)
                return False
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
    online = _pick_network()
    if online is not None:
        start_session(navigate=navigate, headless=False, online=online)


def cmd_headless():
    online = _pick_network()
    if online is None:
        return
    if start_session(headless=True, online=online):
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
    if running():
        answer = input('  当前会话占用端口 %d，停止后运行 CI？ [y/N] ' % PORT)
        if answer.strip().lower() != 'y':
            return
        stop_session()
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
 启动
 1  启动窗口
 2  启动窗口并进入页面
 3  启动无头会话（simctl 驱动）
 检查
 4  截图页面
 5  导出控件树
 6  运行 CI 回归
 7  更新 PNG 金样
 会话
 8  停止当前会话
 9  重置设备状态
（息屏：simctl pm --off-ms 15000；网络：simctl set-wifi connected|disconnected）
 0  退出
"""


def main():
    print('工作目录：%s' % ROOT)
    while True:
        print(MENU)
        state = session_summary()
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
