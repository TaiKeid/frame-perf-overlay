#!/usr/bin/env bash
# Frame Perf Overlay を Steam Frame に入れる・更新する・外す。本体の上で、ふつうのユーザーで実行する（sudo 不要）。
#   ./install.sh                 入れる・更新する。自動起動を有効にし、SteamVR が動いていればその場で（再）起動する
#   ./install.sh --no-autostart  入れる・更新する。自動起動は無効にする（ダッシュボードの＋から起動して使う）
#   ./install.sh --uninstall     外す（設定ファイルは残す）
# リリースの tar.gz を展開したフォルダでも、ビルドしたリポジトリのフォルダ（build/ に実行ファイル）でも動く。
set -euo pipefail

name="frame-perf-overlay"
here="$(cd "$(dirname "$0")" && pwd)"
config_home="${XDG_CONFIG_HOME:-$HOME/.config}"
data_home="${XDG_DATA_HOME:-$HOME/.local/share}"
bin="$HOME/.local/bin/$name"
unit="$name.service"
unit_dir="$config_home/systemd/user"
desktop="$data_home/applications/$name.desktop"
icons="$data_home/icons/hicolor"
icon_sizes="48 128 256"
config_dir="$config_home/$name"

usage() {
    cat <<'EOF'
Usage: ./install.sh [--no-autostart | --uninstall]
  (no option)     install or update, turn on autostart, and (re)start it if SteamVR is running
  --no-autostart  install or update with autostart off (start it from the + button in the dashboard)
  --uninstall     remove it (your settings in ~/.config/frame-perf-overlay/ are kept)
EOF
}

uninstall=0
autostart=1
for arg in "$@"; do
    case "$arg" in
        --uninstall) uninstall=1 ;;
        --no-autostart) autostart=0 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $arg" >&2; usage >&2; exit 2 ;;
    esac
done

# ＋から手で起動したもの（サービスの外で動いているもの）があれば、SIGTERM で止める。
# アプリは SIGTERM でふだんどおりの終了処理（オーバーレイを消してから VR_Shutdown）を通る。
stop_manual_instance() {
    local lock="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$name.lock"
    [[ -r "$lock" ]] || return 0
    local pid
    pid="$(head -n 1 "$lock" 2>/dev/null | tr -dc '0-9')"
    [[ -n "$pid" ]] || return 0
    # ロックファイルの PID が古くて別のプロセスに使い回されていることがあるので、名前を確かめる
    # （/proc/<pid>/comm は 15 文字で切れるので、cmdline の実行ファイル名で比べる）
    local argv0
    argv0="$(tr '\0' '\n' < "/proc/$pid/cmdline" 2>/dev/null | head -n 1)"
    [[ "$(basename -- "${argv0:-x}")" == "$name" ]] || return 0
    local service_pid
    service_pid="$(systemctl --user show -p MainPID --value "$unit" 2>/dev/null || echo 0)"
    [[ "$pid" != "$service_pid" ]] || return 0
    echo "Stopping the running $name (PID $pid) that was started outside the service..."
    kill -TERM "$pid" 2>/dev/null || return 0
    for _ in $(seq 1 50); do
        [[ -d "/proc/$pid" ]] || return 0
        sleep 0.1
    done
    echo "Warning: PID $pid is still running. Close it from the SteamVR dashboard (Perf icon > Close)." >&2
}

# .desktop とアイコンの一覧を作り直す（コマンドが無ければ何もしない）
refresh_caches() {
    if command -v update-desktop-database >/dev/null 2>&1 && [[ -d "$data_home/applications" ]]; then
        update-desktop-database "$data_home/applications" || true
    fi
    if command -v gtk-update-icon-cache >/dev/null 2>&1 && [[ -f "$icons/index.theme" ]]; then
        gtk-update-icon-cache -q "$icons" || true
    fi
}

if [[ "$uninstall" == 1 ]]; then
    # --now の停止は SIGTERM なので、アプリの終了処理がふだんどおり通る
    systemctl --user disable --now "$unit" 2>/dev/null || true
    stop_manual_instance
    rm -f "$unit_dir/$unit" "$bin" "$desktop"
    for size in $icon_sizes; do
        rm -f "$icons/${size}x${size}/apps/$name.png"
    done
    systemctl --user daemon-reload || true
    refresh_caches
    echo "Removed $name."
    if [[ -d "$config_dir" ]]; then
        echo "Your settings are kept in $config_dir. To delete them too: rm -r \"$config_dir\""
    fi
    exit 0
fi

if [[ "$(uname -m)" != "aarch64" ]]; then
    echo "This build is for the Steam Frame (aarch64), but this machine is $(uname -m)." >&2
    exit 1
fi

# 実行ファイル: tar.gz の中なら同じフォルダ、リポジトリなら build/
if [[ -f "$here/$name" ]]; then
    src_bin="$here/$name"
elif [[ -f "$here/build/$name" ]]; then
    src_bin="$here/build/$name"
else
    echo "Cannot find the $name binary next to install.sh or in build/. Build it first (see README)." >&2
    exit 1
fi
for f in "contrib/$unit" "contrib/$name.desktop"; do
    if [[ ! -f "$here/$f" ]]; then
        echo "Missing $here/$f" >&2
        exit 1
    fi
done

# 実行ファイル（一時ファイルに書いてから置き換えるので、動いているものがあっても壊さない）
install -Dm755 "$src_bin" "$bin.new"
mv -f "$bin.new" "$bin"

# アイコン（Steam は hicolor の <サイズ>/apps からアイコンを探す。256x256 だけでは見つからなかったので 3 サイズ入れる）
for size in $icon_sizes; do
    install -Dm644 "$here/contrib/icons/$name-$size.png" "$icons/${size}x${size}/apps/$name.png"
done

# .desktop（ダッシュボードの＋の一覧に出る。Exec は実行ファイルの絶対パスにする。
# Steam から起動されると PATH に ~/.local/bin が無いことがあるため）
mkdir -p "$(dirname "$desktop")"
sed "s|@BINARY@|$bin|" "$here/contrib/$name.desktop" > "$desktop.tmp"
chmod 644 "$desktop.tmp"
mv -f "$desktop.tmp" "$desktop"
if command -v desktop-file-validate >/dev/null 2>&1; then
    desktop-file-validate "$desktop" || true
fi
refresh_caches

# systemd のユーザーサービス
install -Dm644 "$here/contrib/$unit" "$unit_dir/$unit"
systemctl --user daemon-reload

steamvr_running=0
if systemctl --user is-active --quiet steamvr.service; then
    steamvr_running=1
fi

if [[ "$autostart" == 1 ]]; then
    systemctl --user enable "$unit"
    if [[ "$steamvr_running" == 1 ]]; then
        stop_manual_instance
        # restart の停止も SIGTERM なので、動いている古いものは終了処理を通ってから新しいものに替わる
        systemctl --user restart "$unit"
        sleep 2
        systemctl --user --no-pager status "$unit" | head -n 5 || true
    fi
else
    systemctl --user disable "$unit" 2>/dev/null || true
    # サービスとして動いているものがあれば、新しい実行ファイルで起動し直す（次の SteamVR 起動からは自動では起動しない）
    if systemctl --user is-active --quiet "$unit"; then
        systemctl --user restart "$unit"
    fi
fi

echo
echo "$name $("$bin" --version 2>/dev/null | awk '{print $2}') is installed."
if [[ "$autostart" == 1 ]]; then
    if [[ "$steamvr_running" == 1 ]]; then
        echo "  It is running now and starts together with SteamVR from now on."
    else
        echo "  It starts together with SteamVR from the next SteamVR start."
    fi
else
    echo "  Autostart is off. Start it from the + button in the SteamVR dashboard (Frame Perf Overlay)."
fi
cat <<EOF
  Settings: SteamVR dashboard > Perf, or $config_dir/config.json
  Logs:     journalctl --user -u $name -f
  Remove:   ./install.sh --uninstall
EOF
