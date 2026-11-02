#!/var/jb/bin/sh
# Runs as an optional launchd job; never delays PID 1 or resets Data.
set -eu
export PATH=/var/jb/usr/bin:/var/jb/usr/sbin:/var/jb/bin:/var/jb/sbin:/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/var/root TMPDIR=/var/tmp
export DYLD_SHARED_CACHE_DIR=/private/preboot/System/Library/Caches/com.apple.dyld/
export DYLD_FALLBACK_LIBRARY_PATH=/var/jb/usr/lib:/private/preboot/usr/lib
prefix=/var/jb
# This job also exists in OS copies without the optional package payload.
[ -f "$prefix/.pong-sileo-staged" ] || exit 0
# Rootless Sileo prefixes Foundation's non-container mobile directories with
# /var/jb. Those directories are runtime data, not root-owned package files.
for home in /var/mobile "$prefix/var/mobile"; do
    for directory in "$home/Library" "$home/Library/Caches" "$home/Library/Sileo" \
        "$home/Library/Sileo/Database" "$home/Library/Caches/Sileo" "$home/Documents" "$home/Downloads"; do
        mkdir -p "$directory"
        chown 501:501 "$directory"
        chmod 0755 "$directory"
    done
done
# Let launchd and its original container/LaunchServices providers initialize.
sleep 30
if [ ! -e "$prefix/.pong-sileo-configured" ]; then
    "$prefix/usr/libexec/firmware"
    "$prefix/usr/sbin/pwd_mkdb" -p "$prefix/etc/master.passwd"
    for pkg in debianutils apt dash zsh bash vi; do
        script="$prefix/Library/dpkg/info/$pkg.postinst"
        if [ -f "$script" ]; then "$prefix/bin/sh" "$script" configure; fi
    done
    # Do not rebuild every stock app with the bootstrap's older uicache.
    "$prefix/usr/bin/dpkg" --no-triggers --configure org.coolstar.sileo
    touch "$prefix/.pong-sileo-configured"
fi
# Retry next boot if native LaunchServices has not become available yet.
# The older uicache returned zero even when LS rejected every application.
# Recheck the actual mobile-domain proxy on every boot, including an upgrade
# from that incorrect marker. A failed registration remains retryable.
rm -f "$prefix/.pong-sileo-registered"
/pong/sileo-register
touch "$prefix/.pong-sileo-registered"
