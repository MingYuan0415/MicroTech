#!/usr/bin/env python3
"""MicroTech 模拟器开发助手：数字菜单，零参数，面向快速查看页面。

用法：
    python3 sim/dev.py

所有产物固定在 build/ 下；会话端口 5002。退出（0 或 Ctrl+C）一律先停止
模拟器会话。无头驱动与 CI 回归请直接使用 sim/tools/simctl.py 与
sim/ci/run_ci.sh。
"""
import os
import shlex
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
DISPLAY_ENV_KEYS = ('DISPLAY', 'WAYLAND_DISPLAY', 'XDG_RUNTIME_DIR',
                    'SDL_VIDEODRIVER')
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


def _pid_argv(pid):
    proc_cmdline = '/proc/%d/cmdline' % pid
    try:
        with open(proc_cmdline, 'rb') as handle:
            return [item.decode() for item in handle.read().split(b'\0') if item]
    except (OSError, UnicodeDecodeError):
        try:
            result = subprocess.run(['ps', '-p', str(pid), '-o', 'command='],
                                    capture_output=True, text=True,
                                    check=False)
        except OSError:
            return []
        try:
            return shlex.split(result.stdout.strip())
        except ValueError:
            return []


def _pid_is_simulator(pid):
    """Return True only for our binary with the expected Agent port."""
    argv = _pid_argv(pid)
    if not argv or os.path.realpath(argv[0]) != os.path.realpath(BINARY):
        return False
    try:
        port_index = argv.index('--agent-port')
    except ValueError:
        return False
    return (port_index + 1 < len(argv) and
            argv[port_index + 1] == str(PORT))


def _display_environment(environment=None):
    source = os.environ if environment is None else environment
    return tuple(source.get(key, '') for key in DISPLAY_ENV_KEYS)


def _process_environment(pid):
    try:
        with open('/proc/%d/environ' % pid, 'rb') as handle:
            entries = handle.read().split(b'\0')
    except OSError:
        return None
    environment = {}
    try:
        for entry in entries:
            if entry:
                key, value = entry.split(b'=', 1)
                environment[key.decode()] = value.decode()
    except (UnicodeDecodeError, ValueError):
        return None
    return environment


def _session_display_matches(pid):
    """Return True/False, or None when the process environment is unavailable."""
    environment = _process_environment(pid)
    if environment is None:
        return None
    return _display_environment(environment) == _display_environment()


def session_pid():
    if not os.path.exists(PID_FILE):
        return None
    try:
        with open(PID_FILE) as handle:
            pid = int(handle.read().strip())
    except (OSError, ValueError):
        try:
            os.remove(PID_FILE)
        except OSError:
            pass
        return None
    if _pid_alive(pid) and _pid_is_simulator(pid):
        return pid
    try:
        os.remove(PID_FILE)
    except OSError:
        pass
    return None


def agent_ready():
    try:
        sock = _connect(timeout=1)
        try:
            reply = rpc(sock, 'sim.ping')
        finally:
            sock.close()
        return (isinstance(reply, dict) and reply.get('ok') and
                isinstance(reply.get('result'), dict))
    except (OSError, RuntimeError, ValueError, AttributeError):
        return False


def running():
    pid = session_pid()
    return (pid is not None and _session_display_matches(pid) is not False and
            agent_ready())


def session_summary():
    pid = session_pid()
    if pid is None:
        return '未运行'
    display_match = _session_display_matches(pid)
    if display_match is False:
        return '显示环境已变化 pid=%d（下次启动将重启）' % pid
    if display_match is None:
        return '运行中 pid=%d（无法确认显示环境）' % pid
    if not agent_ready():
        return '启动中 pid=%d' % pid
    try:
        sock = _connect(timeout=1)
        try:
            reply = rpc(sock, 'sim.ping')
        finally:
            sock.close()
    except (OSError, RuntimeError, ValueError, AttributeError):
        return '连接中断 pid=%d' % pid
    if (not isinstance(reply, dict) or not reply.get('ok') or
            not isinstance(reply.get('result'), dict)):
        return '运行中 pid=%d（状态不可用）' % pid
    result = reply['result']
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


def _stop_started_process(proc):
    _terminate_process(proc.pid)
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        pass
    _clear_pid_file()


def start_session(navigate=None, headless=False, online=False):
    """后台启动常驻会话；navigate=app 时启动后自动跳转。"""
    pid = session_pid()
    if pid is not None:
        display_match = _session_display_matches(pid)
        if display_match is False:
            print('  检测到显示环境变化，停止旧会话并重新启动（pid=%d）' % pid)
            stop_session()
            pid = None
        elif display_match is None:
            print('  无法确认旧会话显示环境，未自动停止（pid=%d）' % pid)
            return False
        elif agent_ready():
            print('  会话已在运行（端口 %d）。选 1 复用，或选 0 退出并停止。' % PORT)
            return False
        else:
            print('  会话进程正在启动（pid=%d），请稍后重试。' % pid)
            return False
    if agent_ready():
        print('  端口 %d 已被其他 Agent 占用，请先停止占用者。' % PORT)
        return False
    if not build():
        return False
    os.makedirs(OUT_DIR, exist_ok=True)
    common = [BINARY, '--res-dir', RES_DIR, '--nvs-dir', NVS_DIR,
              '--out-dir', OUT_DIR, '--agent-port', str(PORT)]
    if headless or not (os.environ.get('DISPLAY') or
                        os.environ.get('WAYLAND_DISPLAY')):
        cmd = common + ['--headless']
        if not online:
            cmd.append('--ci')
    else:
        cmd = common + ['--window-scale', '1']
    if '--headless' in cmd and not headless:
        print('  未检测到图形显示环境，转为无头驻留（可用 simctl 驱动）')
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
    if not _wait_socket(60, proc):
        print('  会话未就绪，日志末尾：')
        _dump_log()
        _stop_started_process(proc)
        return False
    mode = 'ci' if '--ci' in cmd else (
        'headless' if '--headless' in cmd else 'window')
    network = '联网' if online else '离线'
    print('  会话就绪 pid=%d（模式 %s，%s，日志 %s）' %
          (proc.pid, mode, network, LOG_FILE))
    try:
        sock = _connect(timeout=3)
        try:
            reply = rpc(sock, 'sim.set_wifi',
                        {'state': 'connected' if online else 'disconnected'})
        finally:
            sock.close()
    except (OSError, RuntimeError, ValueError, AttributeError) as exc:
        print('  网络状态设置失败：%s' % exc)
        _stop_started_process(proc)
        return False
    if not isinstance(reply, dict) or not reply.get('ok'):
        print('  网络状态设置失败：%s' % reply)
        _stop_started_process(proc)
        return False
    if navigate:
        if not _navigate(navigate):
            print('  启动后导航失败，停止本次异常会话')
            _stop_started_process(proc)
            return False
    global LAST_NETWORK_ONLINE
    LAST_NETWORK_ONLINE = online
    return True


def _wait_socket(seconds, proc=None):
    deadline = time.time() + seconds
    while time.time() < deadline:
        if proc is not None and proc.poll() is not None:
            return False
        if agent_ready():
            return True
        pid = session_pid()
        if pid is None:
            return False
        time.sleep(0.5)
    return False


def _dump_log():
    if os.path.exists(LOG_FILE):
        with open(LOG_FILE, errors='replace') as handle:
            lines = handle.read().splitlines()[-10:]
        for line in lines:
            print('   ', line)


def stop_session():
    pid = session_pid()
    if pid is None:
        print('  没有运行中的会话')
        return
    try:
        sock = socket.create_connection(('127.0.0.1', PORT), timeout=3)
        try:
            rpc(sock, 'sim.exit')
        finally:
            sock.close()
    except (OSError, RuntimeError, ValueError, AttributeError):
        pass
    finally:
        _terminate_process(pid)
        _clear_pid_file()
    print('  会话已停止')


def _connect(timeout=60):
    return socket.create_connection(('127.0.0.1', PORT), timeout=timeout)


def _navigate(app):
    try:
        sock = _connect(timeout=5)
        try:
            reply = rpc(sock, 'sim.navigate', {'app': app})
            if not isinstance(reply, dict) or not reply.get('ok'):
                print('  导航失败：%s' % reply)
                return False
            settled = rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
            if not isinstance(settled, dict) or not settled.get('ok'):
                print('  页面未稳定：%s' % settled)
                return False
        finally:
            sock.close()
    except (OSError, RuntimeError, ValueError, AttributeError) as exc:
        print('  导航连接失败：%s' % exc)
        return False
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
                if not isinstance(reply, dict) or not reply.get('ok'):
                    print('  导航失败：%s' % reply)
                    return False
            settled = rpc(sock, 'sim.wait_idle', {'timeout_ms': 6000})
            if not isinstance(settled, dict) or not settled.get('ok'):
                print('  页面未稳定：%s' % settled)
                return False
            return action(sock)
        finally:
            sock.close()
    except (OSError, RuntimeError, ValueError, AttributeError) as exc:
        print('  会话操作失败：%s' % exc)
        return False
    finally:
        if temp and pid and _pid_alive(pid):
            stop_session()


# ------------------------------------------------------------- 菜单项

def pick_app(default_current=False):
    if default_current:
        print('  0 当前页面')
    for idx, name in enumerate(APPS, 1):
        print('  %d %s' % (idx, name))
    suffix = ' [回车=当前]' if default_current else ''
    raw = input('  选择页面%s: ' % suffix).strip()
    if default_current and (raw == '' or raw == '0'):
        return None
    if not raw.isdigit() or not (1 <= int(raw) <= len(APPS)):
        print('  无效选择')
        return False
    return APPS[int(raw) - 1]


def cmd_run():
    app = pick_app(default_current=True)
    if app is False:
        return
    if running():
        print('  复用现有会话（%s）' % session_summary())
        if app:
            _navigate(app)
        return
    online = _pick_network()
    if online is None:
        return
    start_session(navigate=app, headless=False, online=online)


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


def cmd_reset():
    stop_session()
    if os.path.isdir(NVS_DIR):
        shutil.rmtree(NVS_DIR)
        print('  设备状态已重置（dev_nvs 清空）')
    else:
        print('  无需重置')


def cmd_build():
    restart = session_pid() is not None
    if restart:
        print('  会话运行中：先停止再编译，成功后自动重启')
        stop_session()
    print('  增量编译 ...')
    if not build():
        if restart:
            print('  编译失败，会话未重启；修复后选 1 重新启动')
        return
    print('  编译完成：%s' % BINARY)
    if restart:
        start_session(headless=False, online=LAST_NETWORK_ONLINE)


MENU = """
──────────── MicroTech 模拟器 ────────────
 1  启动窗口（可选直达页面）
 2  截图页面
 3  重置设备状态
 4  重新编译（运行中则先停后重启）
 0  退出（停止模拟器会话）

 无头驱动：sim/tools/simctl.py（网络/息屏：set-wifi、pm）
 CI 回归与金样：sh sim/ci/run_ci.sh [--update]
"""


def main():
    print('工作目录：%s' % ROOT)
    try:
        while True:
            print(MENU)
            state = session_summary()
            choice = input('  请选择 [%s]: ' % state).strip()
            if choice == '1':
                cmd_run()
            elif choice == '2':
                cmd_shot()
            elif choice == '3':
                cmd_reset()
            elif choice == '4':
                cmd_build()
            elif choice in ('0', 'q'):
                break
            else:
                print('  无效选项')
    except (KeyboardInterrupt, EOFError):
        print()
    finally:
        if session_pid() is not None:
            stop_session()


if __name__ == '__main__':
    main()
