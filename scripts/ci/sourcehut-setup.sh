#!/usr/bin/env bash
set -euo pipefail

if [ -z "${SOURCEHUT_SSH_PRIVATE_KEY:-}" ]; then
  echo "ERROR: Missing required secret: SOURCEHUT_SSH_PRIVATE_KEY" >&2
  exit 1
fi

# ------------------------------------------------------------
# Prepare ~/.ssh
# ------------------------------------------------------------
install -d -m 700 "$HOME/.ssh"

printf '%s\n' "$SOURCEHUT_SSH_PRIVATE_KEY" \
  > "$HOME/.ssh/sourcehut_sync_key"

chmod 600 "$HOME/.ssh/sourcehut_sync_key"

known_hosts="$HOME/.ssh/known_hosts"

# Do not use the runner's existing known_hosts.
# This workflow uses an explicit trust store.
: > "$known_hosts"

# ------------------------------------------------------------
# Pinned git.sr.ht host keys.
# ------------------------------------------------------------
cat >> "$known_hosts" <<'EOF'
git.sr.ht ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIMZvRd4EtM7R+IHVMWmDkVU3VLQTSwQDSAvW0t2Tkj60
git.sr.ht ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQDZ+l/lvYmaeOAPeijHL8d4794Am0MOvmXPyvHTtrqvgmvCJB8pen/qkQX2S1fgl9VkMGSNxbp7NF7HmKgs5ajTGV9mB5A5zq+161lcp5+f1qmn3Dp1MWKp/AzejWXKW+dwPBd3kkudDBA1fa3uK6g1gK5nLw3qcuv/V4emX9zv3P2ZNlq9XRvBxGY2KzaCyCXVkL48RVTTJJnYbVdRuq8/jQkDRA8lHvGvKI+jqnljmZi2aIrK9OGT2gkCtfyTw2GvNDV6aZ0bEza7nDLU/I+xmByAOO79R1Uk4EYCvSc1WXDZqhiuO2sZRmVxa0pQSBDn1DB3rpvqPYW+UvKB3SOz
git.sr.ht ecdsa-sha2-nistp256 AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBCj6y+cJlqK3BHZRLZuM+KP2zGPrh4H66DacfliU1E2DHAd1GGwF4g1jwu3L8gOZUTIvUptqWTkmglpYhFp4Iy4=
EOF

# ------------------------------------------------------------
# Pinned self-hosted SourceHut host key.
#
# Obtained from:
#
#   ssh-keyscan -p 2222 -t ed25519 git.hgspace.work
#
# Host key:
#
#   SHA256 fingerprint:
#   (verify against the server operator if possible)
# ------------------------------------------------------------
cat >> "$known_hosts" <<'EOF'
[git.hgspace.work]:2222 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIF9LJrIFHQVA3MelHwmsk9pksVw8E/XNmONvBI3HGU0R
EOF

chmod 600 "$known_hosts"

# ------------------------------------------------------------
# Force SSH to:
#
#   - use only our dedicated private key
#   - reject unknown host keys
#   - use only our pinned known_hosts file
# ------------------------------------------------------------
git config --local core.sshCommand \
  "ssh \
    -i $HOME/.ssh/sourcehut_sync_key \
    -o IdentitiesOnly=yes \
    -o StrictHostKeyChecking=yes \
    -o UserKnownHostsFile=$known_hosts"

# ------------------------------------------------------------
# Parse comma-separated SourceHut repositories.
# ------------------------------------------------------------
repos=()

while IFS= read -r repo; do
  # Trim leading whitespace.
  repo="${repo#"${repo%%[![:space:]]*}"}"

  # Trim trailing whitespace.
  repo="${repo%"${repo##*[![:space:]]}"}"

  if [ -n "$repo" ]; then
    repos+=("$repo")
  fi
done < <(
  printf '%s\n' "$SOURCEHUT_REPOSITORIES" |
    tr ',' '\n'
)

if [ "${#repos[@]}" -eq 0 ]; then
  echo "ERROR: SOURCEHUT_REPOSITORIES contains no repositories" >&2
  exit 1
fi

# ------------------------------------------------------------
# Parse repository SSH URL.
#
# This validates the supported formats before registering
# remotes.
# ------------------------------------------------------------
repo_host_port() {
  local url="$1"
  local rest
  local hostpart
  local host
  local port

  case "$url" in
    ssh://*@*/*)
      rest="${url#ssh://}"
      rest="${rest#*@}"
      hostpart="${rest%%/*}"

      case "$hostpart" in
        \[*\]:*)
          # IPv6 with explicit port:
          #
          # [2001:db8::1]:2222
          host="${hostpart#\[}"
          host="${host%%\]:*}"
          port="${hostpart##*\]:}"
          ;;

        *:*)
          # Normal host with explicit port:
          #
          # example.org:2222
          host="${hostpart%%:*}"
          port="${hostpart##*:}"
          ;;

        *)
          host="$hostpart"
          port="22"
          ;;
      esac
      ;;

    *@*:*)
      # SCP-style:
      #
      # git@host:path
      #
      # SCP-style Git URLs use the default SSH port.
      rest="${url#*@}"
      host="${rest%%:*}"
      port="22"
      ;;

    *)
      echo "ERROR: unsupported SSH URL format: $url" >&2
      return 1
      ;;
  esac

  if [ -z "$host" ]; then
    echo "ERROR: empty SSH host in URL: $url" >&2
    return 1
  fi

  if [ -z "$port" ]; then
    port="22"
  fi

  case "$port" in
    ''|*[!0-9]*)
      echo "ERROR: invalid SSH port in URL: $url" >&2
      return 1
      ;;
  esac

  if [ "$port" -lt 1 ] || [ "$port" -gt 65535 ]; then
    echo "ERROR: SSH port out of range in URL: $url" >&2
    return 1
  fi

  printf '%s\n%s\n' "$host" "$port"
}

# ------------------------------------------------------------
# Register one Git remote for every SourceHut repository.
# ------------------------------------------------------------
for i in "${!repos[@]}"; do
  repo="${repos[$i]}"
  remote="sourcehut-$i"

  # Validate URL before registering it.
  mapfile -t hp < <(
    repo_host_port "$repo"
  ) || {
    echo "ERROR: invalid SourceHut repository #$((i + 1)): $repo" >&2
    exit 1
  }

  if [ "${#hp[@]}" -lt 2 ]; then
    echo "ERROR: failed to parse repository #$((i + 1)): $repo" >&2
    exit 1
  fi

  host="${hp[0]}"
  port="${hp[1]}"

  # known_hosts lookup format.
  if [ "$port" = "22" ]; then
    key="$host"
  else
    key="[$host]:$port"
  fi

  # The host must already have a pinned key.
  if ! ssh-keygen -F "$key" -f "$known_hosts" >/dev/null 2>&1; then
    echo "ERROR: no pinned SSH host key for $key" >&2
    echo "Refusing to connect to an untrusted host." >&2
    exit 1
  fi

  echo "Verified pinned SSH host key for $key"

  git remote remove "$remote" 2>/dev/null || true
  git remote add "$remote" "$repo"

  echo "Registered $remote -> $repo"
done

# Expose repository count to subsequent steps.
echo "SOURCEHUT_SYNC_COUNT=${#repos[@]}" >> "$GITHUB_ENV"
