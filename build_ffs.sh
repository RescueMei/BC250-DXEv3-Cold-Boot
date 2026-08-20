#!/usr/bin/env bash
set -euo pipefail

# ONLY PODMAN HAS BEEN TESTED!!!
# May work for docker, but I don't run docker.
# Containerized build helper for generating both the raw DXE PE32 image (.efi)
# and a firmware-file-system driver (.ffs) suitable for insertion into an AMI
# DXE firmware volume with UEFITool.
#
# Default behavior uses the Tianocore Fedora 41 development container so the
# build does not depend on a pre-existing host edk2 checkout. A host build mode
# is also available by setting NO_CONTAINER=1 and EDK2_DIR.

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# Resolve all repository paths from the script location so the helper can be
# invoked from any working directory.
REPO_DIR="$SCRIPT_DIR"
PKG_DIR="$REPO_DIR/BC250DXEv3ColdBootPkg"
PLATFORM_DSC="$PKG_DIR/BC250DXEv3ColdBootPkg.dsc"
OUTPUT_DIR="$REPO_DIR/Build/Output"
BUILD_PACKAGE_DIR="MeiMeiDXEv3_ColdBoot"
MODULE_INFS=(
  "$PKG_DIR/BC250DXEv3ColdBootDxe/BC250DXEv3ColdBootDxe.inf"
)
MODULE_NAMES=(
  "MeiMeiDXEv3_ColdBoot"
)
MODULE_GUIDS=(
  "6c98e4f3-9d8b-4e5a-b2e7-31a45b9c5d81"
)

ARCH="${ARCH:-X64}"
TARGET="${TARGET:-RELEASE}"
TOOL_CHAIN_TAG="${TOOL_CHAIN_TAG:-CLANGDWARF}"

# Container mode is the default because it provides a reproducible EDK II
# environment.  NO_CONTAINER=1 switches to an existing host checkout.
CONTAINER_ENGINE="${CONTAINER_ENGINE:-podman}"
EDK2_IMAGE="${EDK2_IMAGE:-ghcr.io/tianocore/containers/fedora-41-dev:latest}"
EDK2_REPO="${EDK2_REPO:-https://github.com/tianocore/edk2.git}"
EDK2_REF="${EDK2_REF:-master}"
NO_CONTAINER="${NO_CONTAINER:-0}"

CACHE_DIR="$REPO_DIR/.cache"
EDK2_CACHE_DIR="$CACHE_DIR/edk2"
CONTAINER_CACHE_ROOT="/cache"
CONTAINER_EDK2_DIR="$CONTAINER_CACHE_ROOT/edk2"
CONTAINER_WORKSPACE="/workspace/BC250-DXEv3-Cold-Boot"

find_basetool() {
  # Locate a built EDK II utility in either its wrapper or native binary path.
  # The caller must have already selected and initialized EDK2_DIR.
  local tool_name="$1"
  local candidate

  for candidate in \
    "$EDK2_DIR/BaseTools/BinWrappers/PosixLike/${tool_name}" \
    "$EDK2_DIR/BaseTools/Source/C/bin/${tool_name}"; do
    if [[ -x "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done

  return 1
}

source_edksetup() {
  # edksetup.sh changes shell variables and may alter errexit/nounset state.
  # Temporarily relaxing both options lets upstream setup scripts complete;
  # this function restores the caller's original shell-option state afterward.
  local edksetup_path="$1"
  local had_nounset=0
  local had_errexit=0

  case $- in
    *e*) had_errexit=1 ;;
    *u*) had_nounset=1 ;;
  esac

  set +eu
  # shellcheck disable=SC1090
  source "$edksetup_path" >/dev/null

  if [[ "$had_errexit" == "1" ]]; then
    set -e
  else
    set +e
  fi

  if [[ "$had_nounset" == "1" ]]; then
    set -u
  else
    set +u
  fi
}

generate_ffs() {
  # Convert one built PE/COFF EFI image into PE32 and user-interface sections,
  # then combine those sections into a driver-type firmware file.  The copied
  # EFI image and intermediate sections are retained in OUTPUT_DIR for review.
  local efi_path="$1"
  local module_name="$2"
  local module_guid="$3"
  local gensec="$4"
  local genffs="$5"
  local pe32_sec ui_sec ffs_path efi_copy

  mkdir -p "$OUTPUT_DIR"
  rm -f "$OUTPUT_DIR"/BC250DXEv3ColdBootDxe.{efi,ffs,pe32.sec,ui.sec}

  pe32_sec="$OUTPUT_DIR/${module_name}.pe32.sec"
  ui_sec="$OUTPUT_DIR/${module_name}.ui.sec"
  ffs_path="$OUTPUT_DIR/${module_name}.ffs"
  efi_copy="$OUTPUT_DIR/${module_name}.efi"

  cp "$efi_path" "$efi_copy"

  "$gensec" \
    -s EFI_SECTION_PE32 \
    -o "$pe32_sec" \
    "$efi_copy"

  "$gensec" \
    -s EFI_SECTION_USER_INTERFACE \
    -n "$module_name" \
    -o "$ui_sec"

  "$genffs" \
    -t EFI_FV_FILETYPE_DRIVER \
    -g "$module_guid" \
    -o "$ffs_path" \
    -i "$pe32_sec" \
    -i "$ui_sec"

  echo "Build complete:"
  echo "  EFI: $efi_copy"
  echo "  FFS: $ffs_path"
}

build_with_host_edk2() {
  # Build using a caller-provided edk2 checkout.  This path is useful for local
  # development and avoids container startup, but requires BaseTools to exist.
  local gensec genffs efi_path index

  if [[ -z "${EDK2_DIR:-}" ]]; then
    echo "error: set EDK2_DIR to your edk2 workspace root when NO_CONTAINER=1" >&2
    exit 1
  fi

  if [[ ! -d "$EDK2_DIR" ]]; then
    echo "error: EDK2_DIR does not exist: $EDK2_DIR" >&2
    exit 1
  fi

  if [[ ! -f "$EDK2_DIR/edksetup.sh" ]]; then
    echo "error: $EDK2_DIR does not look like an edk2 workspace (missing edksetup.sh)" >&2
    exit 1
  fi

  cd "$EDK2_DIR"
  export WORKSPACE="$EDK2_DIR"
  export PACKAGES_PATH="$REPO_DIR:$EDK2_DIR${PACKAGES_PATH:+:$PACKAGES_PATH}"
  export PYTHON_COMMAND="${PYTHON_COMMAND:-python3}"

  source_edksetup "$EDK2_DIR/edksetup.sh"

  gensec="$(find_basetool GenSec || true)"
  genffs="$(find_basetool GenFfs || true)"

  if [[ -z "$gensec" || -z "$genffs" ]]; then
    echo "error: BaseTools GenSec/GenFfs binaries are missing. Build BaseTools first." >&2
    exit 1
  fi

  build -a "$ARCH" -b "$TARGET" -t "$TOOL_CHAIN_TAG" -p "$PLATFORM_DSC"

  for index in "${!MODULE_NAMES[@]}"; do
    efi_path="$EDK2_DIR/Build/${BUILD_PACKAGE_DIR}/${TARGET}_${TOOL_CHAIN_TAG}/$ARCH/${MODULE_NAMES[$index]}.efi"
    if [[ ! -f "$efi_path" ]]; then
      efi_path="$(find "$EDK2_DIR/Build/${BUILD_PACKAGE_DIR}" -type f -name "${MODULE_NAMES[$index]}.efi" | head -n 1)"
    fi
    if [[ -z "$efi_path" || ! -f "$efi_path" ]]; then
      echo "error: failed to locate ${MODULE_NAMES[$index]}.efi" >&2
      exit 1
    fi
    generate_ffs "$efi_path" "${MODULE_NAMES[$index]}" "${MODULE_GUIDS[$index]}" "$gensec" "$genffs"
  done
}

build_with_container() {
  # Run the complete build inside the configured Tianocore container.  The
  # repository and a persistent edk2 cache are mounted so source changes remain
  # local and repeated builds do not reclone the toolchain unnecessarily.
  if ! command -v "$CONTAINER_ENGINE" >/dev/null 2>&1; then
    echo "error: container engine not found: $CONTAINER_ENGINE" >&2
    exit 1
  fi

  mkdir -p "$CACHE_DIR" "$EDK2_CACHE_DIR" "$OUTPUT_DIR"

  "$CONTAINER_ENGINE" run --rm \
    -v "$REPO_DIR:$CONTAINER_WORKSPACE:Z" \
    -v "$CACHE_DIR:$CONTAINER_CACHE_ROOT:Z" \
    -w "$CONTAINER_WORKSPACE" \
    -e ARCH="$ARCH" \
    -e TARGET="$TARGET" \
    -e TOOL_CHAIN_TAG="$TOOL_CHAIN_TAG" \
    -e EDK2_REPO="$EDK2_REPO" \
    -e EDK2_REF="$EDK2_REF" \
    -e BUILD_PACKAGE_DIR="$BUILD_PACKAGE_DIR" \
    -e CONTAINER_CACHE_ROOT="$CONTAINER_CACHE_ROOT" \
    -e CONTAINER_EDK2_DIR="$CONTAINER_EDK2_DIR" \
    -e CONTAINER_WORKSPACE="$CONTAINER_WORKSPACE" \
    "$EDK2_IMAGE" \
    bash -lc '
      set -euo pipefail

      mkdir -p "$CONTAINER_CACHE_ROOT"

      # Refresh through a temporary checkout, then swap it into place.  This
      # avoids leaving a half-cloned cache if the network or git operation fails.
      refresh_edk2_checkout() {
        local tmp_dir="${CONTAINER_EDK2_DIR}.tmp.$$"

        rm -rf "$tmp_dir"
        git clone --depth 1 --branch "$EDK2_REF" "$EDK2_REPO" "$tmp_dir"
        rm -rf "$CONTAINER_EDK2_DIR.old"

        if [[ -e "$CONTAINER_EDK2_DIR" ]]; then
          mv "$CONTAINER_EDK2_DIR" "$CONTAINER_EDK2_DIR.old"
        fi

        mv "$tmp_dir" "$CONTAINER_EDK2_DIR"
        rm -rf "$CONTAINER_EDK2_DIR.old"
      }

      # Validate the cache before using it.  Fetch/checkout failures fall back
      # to a clean clone so stale or interrupted state cannot poison the build.
      if [[ ! -e "$CONTAINER_EDK2_DIR" ]]; then
        refresh_edk2_checkout
      elif [[ ! -d "$CONTAINER_EDK2_DIR/.git" ]]; then
        echo "warning: cache path is not a git checkout, refreshing it"
        refresh_edk2_checkout
      fi

      cd "$CONTAINER_EDK2_DIR"
      if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        echo "warning: cached checkout is invalid, refreshing it"
        cd "$CONTAINER_CACHE_ROOT"
        refresh_edk2_checkout
        cd "$CONTAINER_EDK2_DIR"
      fi

      if ! git fetch --depth 1 origin "$EDK2_REF"; then
        echo "warning: git fetch failed, refreshing cached checkout"
        cd "$CONTAINER_CACHE_ROOT"
        refresh_edk2_checkout
        cd "$CONTAINER_EDK2_DIR"
      fi

      if ! git checkout "$EDK2_REF"; then
        echo "warning: git checkout failed, refreshing cached checkout"
        cd "$CONTAINER_CACHE_ROOT"
        refresh_edk2_checkout
        cd "$CONTAINER_EDK2_DIR"
      fi

      # Only initialize the submodules required to build BaseTools and MdePkg for
      # this driver. A full recursive update can fail on unrelated optional nested
      # dependencies in upstream edk2 (for example OpenSSL test/development trees).
      git submodule update --init \
        BaseTools/Source/C/BrotliCompress/brotli \
        MdePkg/Library/BaseFdtLib/libfdt \
        MdePkg/Library/MipiSysTLib/mipisyst
      make -C BaseTools/Source/C -j"$(nproc)"

      export WORKSPACE="$CONTAINER_EDK2_DIR"
      export PACKAGES_PATH="$CONTAINER_WORKSPACE:$CONTAINER_EDK2_DIR"
      export PYTHON_COMMAND="${PYTHON_COMMAND:-python3}"
      export EDK_TOOLS_PATH="$CONTAINER_EDK2_DIR/BaseTools"

      source_edksetup() {
        local edksetup_path="$1"
        local had_nounset=0
        local had_errexit=0

        case $- in
          *e*) had_errexit=1 ;;
          *u*) had_nounset=1 ;;
        esac

        set +eu
        source "$edksetup_path" >/dev/null

        if [[ "$had_errexit" == "1" ]]; then
          set -e
        else
          set +e
        fi

        if [[ "$had_nounset" == "1" ]]; then
          set -u
        else
          set +u
        fi
      }

      source_edksetup "$CONTAINER_EDK2_DIR/edksetup.sh"

      export PATH="$CONTAINER_EDK2_DIR/BaseTools/BinWrappers/PosixLike:$CONTAINER_EDK2_DIR/BaseTools/Bin/Linux-x86_64:$PATH"

      mkdir -p "$CONTAINER_EDK2_DIR/Conf"
      cp -f "$CONTAINER_EDK2_DIR/BaseTools/Conf/build_rule.template" "$CONTAINER_EDK2_DIR/Conf/build_rule.txt"
      cp -f "$CONTAINER_EDK2_DIR/BaseTools/Conf/tools_def.template" "$CONTAINER_EDK2_DIR/Conf/tools_def.txt"
      cp -f "$CONTAINER_EDK2_DIR/BaseTools/Conf/target.template" "$CONTAINER_EDK2_DIR/Conf/target.txt"

      # CLANGDWARF emits ELF debug images and maps, but does not emit Windows
      # PDB files. Avoid the generated makefile expected-but-missing PDB copy
      # warning while retaining the normal map-file copy.
      sed -i "/DEBUG_DIR)(+).*\\.pdb.*OUTPUT_DIR/d" \
        "$CONTAINER_EDK2_DIR/Conf/build_rule.txt"

      if ! command -v build >/dev/null 2>&1; then
        echo "error: edksetup.sh completed but build is not on PATH" >&2
        echo "PATH=$PATH" >&2
        exit 1
      fi

      echo "Running EDK II build..."

      build \
        -a "$ARCH" \
        -b "$TARGET" \
        -t "$TOOL_CHAIN_TAG" \
        -p "$CONTAINER_WORKSPACE/BC250DXEv3ColdBootPkg/BC250DXEv3ColdBootPkg.dsc" \
        -Y PCD
      # Keep these arrays aligned with the host-side module metadata above; each
      # entry produces one EFI image and one corresponding FFS artifact.
      NAMES=(MeiMeiDXEv3_ColdBoot)
      GUIDS=(6c98e4f3-9d8b-4e5a-b2e7-31a45b9c5d81)
      for INDEX in 0; do
        MODULE_NAME="${NAMES[$INDEX]}"
        EFI_PATH="$CONTAINER_EDK2_DIR/Build/${BUILD_PACKAGE_DIR}/${TARGET}_${TOOL_CHAIN_TAG}/${ARCH}/${MODULE_NAME}.efi"
        if [[ ! -f "$EFI_PATH" ]]; then
          EFI_PATH="$(find "$CONTAINER_EDK2_DIR/Build/${BUILD_PACKAGE_DIR}" -type f -name "${MODULE_NAME}.efi" | head -n 1)"
        fi
        if [[ -z "$EFI_PATH" || ! -f "$EFI_PATH" ]]; then
          echo "error: failed to locate built EFI image: $MODULE_NAME" >&2
          exit 1
        fi
        mkdir -p "$CONTAINER_WORKSPACE/Build/Output"
        rm -f "$CONTAINER_WORKSPACE/Build/Output"/BC250DXEv3ColdBootDxe.{efi,ffs,pe32.sec,ui.sec}
        cp "$EFI_PATH" "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.efi"
        "$CONTAINER_EDK2_DIR/BaseTools/BinWrappers/PosixLike/GenSec" -s EFI_SECTION_PE32 -o "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.pe32.sec" "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.efi"
        "$CONTAINER_EDK2_DIR/BaseTools/BinWrappers/PosixLike/GenSec" -s EFI_SECTION_USER_INTERFACE -n "$MODULE_NAME" -o "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.ui.sec"
        "$CONTAINER_EDK2_DIR/BaseTools/BinWrappers/PosixLike/GenFfs" -t EFI_FV_FILETYPE_DRIVER -g "${GUIDS[$INDEX]}" -o "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.ffs" -i "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.pe32.sec" -i "$CONTAINER_WORKSPACE/Build/Output/${MODULE_NAME}.ui.sec"
        echo "Build complete: $MODULE_NAME"
      done
    '
}

if [[ "$NO_CONTAINER" == "1" ]]; then
  build_with_host_edk2
else
  build_with_container
fi