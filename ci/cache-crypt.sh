#!/usr/bin/env bash
# Encrypted Actions cache for the public repository.
#
# The cached XNU work tree and objects are Apple source-derived, so the Actions cache only ever
# holds ciphertext: tar | zstd | AES-256-CTR, then HMAC-SHA256 over the ciphertext (encrypt-then-MAC).
# KERNEL_CACHE_KEY is a repository secret (hex); workflows from forks never receive it.
#
#   cache-crypt.sh pack   <blob-dir> <root> <relative paths...>
#   cache-crypt.sh unpack <blob-dir> <root>
set -euo pipefail
: "${KERNEL_CACHE_KEY:?KERNEL_CACHE_KEY secret is not set}"
OPENSSL=${OPENSSL:-openssl}
action=$1 blob=$2 root=$3
shift 3
enc_key=$(printf 'enc:%s' "$KERNEL_CACHE_KEY" | "$OPENSSL" dgst -sha256 -r | cut -d' ' -f1)
mac_key=$(printf 'mac:%s' "$KERNEL_CACHE_KEY" | "$OPENSSL" dgst -sha256 -r | cut -d' ' -f1)

mac() {
  "$OPENSSL" dgst -sha256 -mac HMAC -macopt "hexkey:$mac_key" -r "$1" | cut -d' ' -f1
}

case "$action" in
  pack)
    mkdir -p "$blob"
    rm -f "$blob"/cache.bin "$blob"/cache.mac "$blob"/cache.iv
    iv=$("$OPENSSL" rand -hex 16)
    present=()
    for path in "$@"; do
      [ -e "$root/$path" ] && present+=("$path")
    done
    [ "${#present[@]}" -gt 0 ] || { echo "cache: nothing to pack"; exit 1; }
    tar -C "$root" -cf - "${present[@]}" | zstd -q -T0 -3 \
      | "$OPENSSL" enc -aes-256-ctr -K "$enc_key" -iv "$iv" -out "$blob/cache.bin"
    printf '%s\n' "$iv" > "$blob/cache.iv"
    mac "$blob/cache.bin" > "$blob/cache.mac"
    echo "cache: packed ${#present[@]} paths, $(du -h "$blob/cache.bin" | cut -f1) ciphertext"
    ;;
  unpack)
    if [ ! -f "$blob/cache.bin" ]; then
      echo "cache: no restored blob (cold build)"
      exit 0
    fi
    if [ "$(mac "$blob/cache.bin")" != "$(cat "$blob/cache.mac")" ]; then
      echo "cache: HMAC mismatch, ignoring restored blob (cold build)"
      exit 0
    fi
    "$OPENSSL" enc -d -aes-256-ctr -K "$enc_key" -iv "$(cat "$blob/cache.iv")" -in "$blob/cache.bin" \
      | zstd -q -d | tar -C "$root" -xf -
    rm -f "$blob"/cache.bin
    echo "cache: restored and verified"
    ;;
  *)
    echo "usage: $0 pack|unpack <blob-dir> <root> [paths...]" >&2
    exit 2
    ;;
esac
