# Demo GIF for the README: one sender, three receivers, one transfer.
# Runs entirely on this machine: multicast over the loopback interface
# (hence --iface 127.0.0.1; on a real LAN you would omit it).
#
# Regenerate with:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   PATH="$PWD/build:$PATH" vhs docs/demo.tape
#
# Requires: vhs (https://github.com/charmbracelet/vhs), tmux.

Output docs/demo.gif

Set Shell bash
Set FontSize 15
Set FontFamily "Menlo"
Set Width 1000
Set Height 640
Set Padding 12
Set Framerate 20
Set TypingSpeed 35ms
Set Theme "Catppuccin Mocha"

# --- hidden setup: demo dirs, a 40 MiB file, labeled tmux panes ---
Hide
Type `export DEMO="$(mktemp -d)" && mkdir -p "$DEMO/sender" "$DEMO/r1" "$DEMO/r2" "$DEMO/r3"` Enter
Type `dd if=/dev/urandom of="$DEMO/sender/photos.zip" bs=1048576 count=40 status=none` Enter
Type `tmux -L fcdemo -f /dev/null new-session -d -s demo -x 110 -y 34 "cd '$DEMO/sender' && PS1='\\[\\e[1;36m\\]sender\\[\\e[0m\\] $ ' exec bash --norc"` Enter
Type `tmux -L fcdemo set -g status off \; set -g pane-border-status top \; set -g pane-border-format " #{pane_title} " \; set -g pane-border-style fg=colour240 \; set -g pane-active-border-style fg=colour240` Enter
Type `for i in 1 2 3; do tmux -L fcdemo split-window -v -t demo "cd '$DEMO/r$i' && PS1='\\[\\e[1;33m\\]receiver\\[\\e[0m\\] $ ' exec bash --norc"; done` Enter
Type `tmux -L fcdemo select-layout -t demo even-vertical` Enter
Type `tmux -L fcdemo select-pane -t demo:0.0 -T "sender - has photos.zip"` Enter
Type `for i in 1 2 3; do tmux -L fcdemo select-pane -t demo:0.$i -T "receiver $i"; done` Enter
Type `tmux -L fcdemo select-pane -t demo:0.1` Enter
Type `tmux -L fcdemo attach -t demo` Enter
Sleep 1s
Show

# --- receivers: same command on every machine ---
Sleep 600ms
Type "filecast receive --multicast 239.1.2.3 --iface 127.0.0.1"
Enter
Ctrl+B
Type "o"
Sleep 300ms
Type "filecast receive --multicast 239.1.2.3 --iface 127.0.0.1"
Enter
Ctrl+B
Type "o"
Sleep 300ms
Type "filecast receive --multicast 239.1.2.3 --iface 127.0.0.1"
Enter
Ctrl+B
Type "o"
Sleep 500ms

# --- sender: one transfer, everyone gets the file ---
Type "filecast send photos.zip --multicast 239.1.2.3 --iface 127.0.0.1"
Sleep 400ms
Enter

# --- watch all three receivers complete simultaneously ---
Sleep 7.5s

# --- proof: the file landed, checksum verified ---
Ctrl+B
Type "o"
Sleep 600ms
Type "ls -lh photos.zip"
Sleep 300ms
Enter
Sleep 3.5s

# --- hidden teardown ---
Hide
Type `tmux -L fcdemo kill-server` Enter
