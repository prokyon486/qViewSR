"""Linux launcher: an AutoTrace child cannot outlive a killed GUI worker."""
import ctypes
import os
import signal
import sys


def main():
    if sys.platform != 'linux':
        raise RuntimeError('現在のベクター変換workerはLinuxに対応しています。')
    parent = int(sys.argv[1])
    libc = ctypes.CDLL(None, use_errno=True)
    # PR_SET_PDEATHSIG remains active across exec for this unprivileged binary.
    if libc.prctl(1, signal.SIGKILL, 0, 0, 0) != 0:
        raise OSError(ctypes.get_errno(), '親プロセス終了時の停止設定に失敗しました')
    # Covers a worker killed before this launcher finished starting.
    if os.getppid() != parent:
        return 1
    os.execv(sys.argv[2], sys.argv[2:])


if __name__ == '__main__':
    try:
        sys.exit(main())
    except Exception as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
