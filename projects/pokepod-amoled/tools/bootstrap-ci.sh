#!/bin/sh
set -eu

# One bootstrap contract for GitHub-hosted CI and local/external verification.
# Local runs are verification-only unless --install is explicit. GitHub Actions
# installs only a missing supported package, then both modes verify versions.

install=0
case "${1:-}" in
  --install) install=1 ;;
  --verify|'') ;;
  *)
    printf 'Usage: %s [--verify|--install]\n' "$0" >&2
    exit 64
    ;;
esac

missing=
for command in python3 clang++ rg git sha256sum curl file tar; do
  if ! command -v "$command" >/dev/null 2>&1; then
    missing="${missing}${missing:+ }$command"
  fi
done

if [ -n "$missing" ] && [ "$install" -eq 1 ]; then
  if [ "$(uname -s)" != Linux ] || ! command -v apt-get >/dev/null 2>&1; then
    printf 'CI bootstrap cannot install missing tools on this platform: %s\n' \
      "$missing" >&2
    exit 69
  fi
  packages=
  case " $missing " in *' rg '*) packages="${packages}${packages:+ }ripgrep" ;; esac
  case " $missing " in *' clang++ '*) packages="${packages}${packages:+ }clang" ;; esac
  case " $missing " in *' git '*) packages="${packages}${packages:+ }git" ;; esac
  case " $missing " in *' sha256sum '*) packages="${packages}${packages:+ }coreutils" ;; esac
  case " $missing " in *' curl '*) packages="${packages}${packages:+ }curl" ;; esac
  case " $missing " in *' file '*) packages="${packages}${packages:+ }file" ;; esac
  case " $missing " in *' tar '*) packages="${packages}${packages:+ }tar" ;; esac
  case " $missing " in
    *' python3 '*) packages="${packages}${packages:+ }python3" ;;
  esac
  if [ -z "$packages" ]; then
    printf 'CI bootstrap has no package mapping for: %s\n' "$missing" >&2
    exit 69
  fi
  sudo apt-get update
  # Disable weak recommendations so the bootstrap remains narrow and auditable.
  sudo apt-get install -y --no-install-recommends $packages
fi

for command in python3 clang++ rg git sha256sum curl file tar; do
  if ! command -v "$command" >/dev/null 2>&1; then
    printf 'CI bootstrap missing required tool: %s\n' "$command" >&2
    printf 'Run %s --install on supported Linux CI, or install it explicitly.\n' \
      "$0" >&2
    exit 69
  fi
done

python3 --version
clang++ --version | sed -n '1p'
rg --version | sed -n '1p'
git --version
sha256sum --version | sed -n '1p'
curl --version | sed -n '1p'
file --version | sed -n '1p'
tar --version | sed -n '1p'
printf 'PASS pokepod_ci_bootstrap\n'
