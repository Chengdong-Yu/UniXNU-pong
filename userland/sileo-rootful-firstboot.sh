#!/usr/bin/dash
# Real rootful bootstrap configuration, optional and outside PID 1's path.
set -eu
export PATH=/usr/bin:/bin:/usr/sbin:/sbin
export HOME=/var/root TMPDIR=/var/tmp
export DYLD_SHARED_CACHE_DIR=/private/preboot/System/Library/Caches/com.apple.dyld/
export DYLD_FALLBACK_LIBRARY_PATH=/usr/lib:/private/preboot/usr/lib
[ -f /.pong-rootful-staged ] || exit 0
echo 'Pong rootful: requesting APFS OS read-write update; SystemOS stays read-only'
/sbin/mount -u -w / || exit 73
# Confirm a real root write before advertising a usable package environment.
probe=/.pong-rootful-write-probe
(umask 077; echo '22H374' > "$probe") || exit 73
rm -f "$probe"
echo 'Pong rootful: OS write succeeded'
if [ ! -e /var/.pong-rootful-data-seeded-v1 ]; then
    # The archive contains directories/files absent from stock Data. -k
    # preserves every existing user file; duplicate directories are harmless.
    /usr/bin/tar -xkpf /pong/rootful-data.tar -C /private/var || true
    [ -s /var/lib/apt/lists/apt.procurs.us_dists_iphoneos-arm64_1700_main_binary-iphoneos-arm_Packages ] || exit 74
    # The old bootstrap remains available for recovery, but its marker must
    # not make the rootful Sileo executable select /var/jb again.
    if [ -e /var/jb/.procursus_strapped ]; then
        mv /var/jb/.procursus_strapped /var/jb/.procursus_strapped.rootless-backup
    fi
    if [ -e /var/lib/dpkg ] && [ ! -L /var/lib/dpkg ]; then
        mv /var/lib/dpkg /var/lib/dpkg.before-pong-rootful
    fi
    [ -L /var/lib/dpkg ] || ln -s /Library/dpkg /var/lib/dpkg
    touch /var/.pong-rootful-data-seeded-v1
fi
mkdir -p /var/lib/apt/lists/partial /var/lib/apt/sileolists /var/cache/apt/archives/partial
chown 0:0 /var/lib/apt /var/lib/apt/lists
chmod 0755 /var/lib/apt /var/lib/apt/lists
chown 501:501 /var/lib/apt/sileolists
chmod 0755 /var/lib/apt/sileolists
for directory in /var/mobile/Library/Sileo /var/mobile/Library/Caches/Sileo /var/mobile/Documents /var/mobile/Downloads; do
    mkdir -p "$directory";chown 501:501 "$directory";chmod 0755 "$directory"
done
if [ ! -e /var/.pong-rootful-configured-v2 ]; then
    # firmware queries every MobileGestalt capability. An unimplemented
    # hardware query must not hold the package database/registration behind
    # it. Configure the real bootstrap first, then refresh capabilities.
    echo 'Pong rootful: configuring account database'
    /usr/sbin/pwd_mkdb -p /etc/master.passwd
    for package in debianutils apt dash zsh bash vi; do
        echo "Pong rootful: configuring $package"
        script="/Library/dpkg/info/$package.postinst"
        [ ! -f "$script" ] || /usr/bin/dash "$script" configure
    done
    echo 'Pong rootful: configuring Sileo package'
    # Do not fail later boots when this genuine package is already configured.
    state=$(/usr/bin/dpkg-query -W -f='${Status}' org.coolstar.sileo)
    [ "$state" = 'install ok installed' ] || /usr/bin/dpkg --no-triggers --configure org.coolstar.sileo
fi
# The host stages a real, signed rootful ElleKit package and its real dpkg
# record. Configure the Pong postinst without suspending/injecting PID 1.
if [ -f /Library/dpkg/info/ellekit.postinst ]; then
    state=$(/usr/bin/dpkg-query -W -f='${Status}' ellekit 2>/dev/null || true)
    if [ "$state" != 'install ok installed' ]; then
        /usr/bin/dpkg --no-triggers --configure ellekit || echo 'Pong rootful: ElleKit configuration remains retryable'
    fi
    /pong/tweak-refresh || echo 'Pong rootful: optional tweak rule unchanged'
fi
echo 'Pong rootful: native architecture follows'
/usr/bin/dpkg --print-architecture
# Sileo registration is independent of whether Wi-Fi has joined yet.
/pong/sileo-register || echo 'Pong rootful: LaunchServices registration will retry next boot'
# Bound only the optional capabilities refresh; keep its actual result and
# retry failures next boot. Never invent hardware capability packages.
if [ ! -e /var/.pong-rootful-configured-v2 ]; then
    echo 'Pong rootful: refreshing native firmware capabilities (60-second limit)'
    /usr/libexec/firmware & firmware_pid=$!
    (sleep 60; kill -TERM "$firmware_pid" 2>/dev/null || true
        sleep 5; kill -KILL "$firmware_pid" 2>/dev/null || true) & guard_pid=$!
    if wait "$firmware_pid"; then
        touch /var/.pong-rootful-configured-v2
        echo 'Pong rootful: package environment configured'
    else
        echo 'Pong rootful: capabilities refresh incomplete; Sileo remains registered, retry next boot'
    fi
    kill "$guard_pid" 2>/dev/null || true
    wait "$guard_pid" 2>/dev/null || true
fi
# Real signed repository metadata; keep the seeded index when offline.
# Wi-Fi joining happens after this job starts. Let the native resolver become
# ready, bounded outside PID1; never discard real offline seed indices.
/pong/package-network --wait 60 || true
/usr/bin/apt-get update || echo 'Pong rootful: repository refresh incomplete; retained real seeded index'
