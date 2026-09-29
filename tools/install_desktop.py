#!/usr/bin/env python3
"""Register this checkout in the current user's Ubuntu launcher and Open With menu."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

APP_ID = "io.github.prokyon486.qViewSR"


def desktop_string(value):
    return str(value).replace("\\", "\\\\").replace("\n", "\\n").replace("\r", "\\r")


def exec_argument(value):
    # Exec quoting is separate from Desktop Entry string escaping. No shell runs.
    quoted = "".join("\\" + c if c in '\\"$`' else c for c in str(value))
    return desktop_string('"' + quoted.replace("%", "%%") + '"')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--data-home", type=Path,
                        default=Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("Run as your desktop user, without sudo.")
    repo = Path(__file__).resolve().parents[1]
    data = args.data_home.resolve()
    applications = data / "applications"
    entry = applications / (APP_ID + ".desktop")
    icon = data / "icons/hicolor/scalable/apps" / (APP_ID + ".svg")
    defaults = {}
    if args.uninstall:
        entry.unlink(missing_ok=True)
        icon.unlink(missing_ok=True)
    else:
        binary = repo / "build/gui/qviewsr"
        if not binary.is_file():
            parser.error("Build first: tools/build_qviewsr.sh")
        env = dict(os.environ, QT_QPA_PLATFORM="offscreen")
        env.pop("LD_LIBRARY_PATH", None)
        with tempfile.TemporaryDirectory(prefix="qviewsr-desktop-") as temporary:
            # Query decoder support without touching the user's recent files/settings.
            env["XDG_CONFIG_HOME"] = temporary
            mimes = subprocess.check_output([binary, "--supported-mime-types"], env=env,
                                            text=True, timeout=30).strip()
            if not mimes or any(not mime.startswith(("image/", "application/"))
                                for mime in mimes.rstrip(";").split(";")):
                raise ValueError("Unexpected supported MIME type list")
            user_data = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")).resolve()
            if data == user_data and shutil.which("xdg-mime"):
                for mime in mimes.rstrip(";").split(";"):
                    default = subprocess.check_output(["xdg-mime", "query", "default", mime],
                                                      text=True, timeout=15).strip()
                    if default:
                        defaults[mime] = default
            launcher = repo / "tools/run_qviewsr.sh"
            content = f"""[Desktop Entry]
Version=1.0
Type=Application
Name=qViewSR
GenericName=Image Viewer
GenericName[ja]=超解像画像ビューア
Comment=View images with color management and NCS super-resolution
Comment[ja]=カラープロファイル対応・NCS超解像画像ビューア
Exec={exec_argument(launcher)} %f
TryExec={desktop_string(launcher)}
Icon={APP_ID}
Terminal=false
StartupNotify=true
StartupWMClass=qViewSR
Categories=Qt;Graphics;Viewer;Photography;
Keywords=photos;pictures;super-resolution;NCS;
Keywords[ja]=画像;写真;超解像;NCS;
MimeType={mimes}
"""
            staged = Path(temporary) / entry.name
            staged.write_text(content)
            if shutil.which("desktop-file-validate"):
                subprocess.run(["desktop-file-validate", staged], check=True)
            applications.mkdir(parents=True, exist_ok=True)
            icon.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(repo / "dist/linux/hicolor/scalable/apps/com.interversehq.qView.svg", icon)
            shutil.copyfile(staged, entry)
            entry.chmod(0o644)
    if applications.exists() and shutil.which("update-desktop-database"):
        subprocess.run(["update-desktop-database", applications], check=True)
    # A newly added handler can change GNOME's implicit default even without
    # modifying mimeapps.list. Preserve that previous effective choice as well.
    for mime, previous in defaults.items():
        current = subprocess.check_output(["xdg-mime", "query", "default", mime],
                                          text=True, timeout=15).strip()
        if current != previous:
            subprocess.run(["xdg-mime", "default", previous, mime], check=True, timeout=15)
    if shutil.which("gtk-update-icon-cache") and (data / "icons/hicolor").exists():
        subprocess.run(["gtk-update-icon-cache", "--force", "--ignore-theme-index",
                        data / "icons/hicolor"], check=True)
    print("Removed:" if args.uninstall else "Registered:", entry)
    print("Existing default image applications are unchanged.")


if __name__ == "__main__":
    main()
