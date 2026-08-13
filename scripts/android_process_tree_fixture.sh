#!/system/bin/sh
# Runtime fixture for scripts/test_android_process_tree.sh.
mode=${1:-fork}
base=${2:-/data/local/tmp/pm_tiny_tree_fixture}

rm -rf "$base"
mkdir -p "$base"
echo $$ > "$base/root.pid"
trap '' HUP INT TERM

/system/bin/sh -c '
    base=$1
    trap "" HUP INT TERM
    echo $$ > "$base/child.pid"
    /system/bin/sh -c '\''
        base=$1
        trap "" HUP INT TERM
        echo $$ > "$base/grandchild.pid"
        /system/bin/sleep 300
    '\'' fixture-grandchild "$base" &
    echo $! > "$base/grandchild-parent.pid"
    wait
' fixture-child "$base" &
echo $! > "$base/child-parent.pid"

case "$mode" in
    fork)
        wait
        ;;
    orphan)
        exit 0
        ;;
    *)
        echo "unknown fixture mode: $mode" >&2
        exit 2
        ;;
esac
