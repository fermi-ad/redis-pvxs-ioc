#!/usr/bin/env bash
# Explicit local-image use is opt-in. Cached remote tags are never evidence
# that the current registry image was tested.
ensure_test_image() {
  local image="$1"
  local policy="${2:-auto}"
  case "$policy" in
    auto)
      case "$image" in
        *@sha256:*) docker image inspect "$image" >/dev/null 2>&1 || docker pull "$image" ;;
        *) docker pull "$image" ;;
      esac
      ;;
    always) docker pull "$image" ;;
    never) docker image inspect "$image" >/dev/null ;;
    *) echo "invalid image pull policy: $policy (expected auto, always, or never)" >&2; return 1 ;;
  esac
}
