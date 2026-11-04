#!/usr/bin/env bash
# ==============================================================================
# get_git.sh - Pull Latest or Checkout Specific Version Tag for AIO_LPC
# ==============================================================================
# Usage:
#   ./get_git.sh                     # Pulls latest main and builds
#   ./get_git.sh -tag v6.0.1         # Checks out specific version tag and builds
#   ./get_git.sh --no-build          # Pulls without rebuilding
# ==============================================================================

set -e

BOLD='\033[1m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

TAG_NAME=""
SKIP_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        -tag|--tag|-t|-v|--version)
            TAG_NAME="$2"
            shift 2
            ;;
        -tag=*|--tag=*|-v=*|--version=*)
            TAG_NAME="${1#*=}"
            shift 1
            ;;
        --no-build|--skip-build)
            SKIP_BUILD=1
            shift 1
            ;;
        -h|--help)
            echo -e "${BOLD}Usage:${NC}"
            echo "  ./get_git.sh [-tag <tag_name>] [--no-build]"
            echo ""
            echo "Options:"
            echo "  -tag, --tag <tag>   Checkout specific release tag (e.g. v6.0.1)"
            echo "  --no-build          Skip automatic build after pulling"
            exit 0
            ;;
        *)
            if [ -z "$TAG_NAME" ] && [[ ! "$1" =~ ^- ]]; then
                TAG_NAME="$1"
                shift 1
            else
                shift 1
            fi
            ;;
    esac
done

echo -e "${CYAN}======================================================${NC}"
echo -e "${BOLD}AIO_LPC Firmware: Fetch & Sync from Git${NC}"
echo -e "${CYAN}======================================================${NC}"

echo -e "${YELLOW}1. Fetching latest branches and tags from GitHub...${NC}"
git fetch --all --tags

if [ -n "$TAG_NAME" ]; then
    echo -e "\n${YELLOW}2. Checking out tag '${TAG_NAME}'...${NC}"
    git checkout "tags/${TAG_NAME}" -B "release-${TAG_NAME}"
    echo -e "${GREEN}   ✔ Checked out tag '${TAG_NAME}'${NC}"
else
    CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")"
    if [ "$CURRENT_BRANCH" = "HEAD" ]; then
        CURRENT_BRANCH="main"
        git checkout main
    fi
    echo -e "\n${YELLOW}2. Pulling latest commits on '${CURRENT_BRANCH}'...${NC}"
    git pull origin "$CURRENT_BRANCH"
    echo -e "${GREEN}   ✔ Branch '${CURRENT_BRANCH}' up to date.${NC}"
fi

# Print active version on LCD
CONFIG_FILE="${ROOT_DIR}/LPC1768/user_config.h"
ACTIVE_VER=$(grep "#define FIRMWARE_VERSION" "$CONFIG_FILE" | sed -E 's/.*"([^"]+)".*/\1/' || echo "Unknown")

if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n${YELLOW}3. Compiling firmware...${NC}"
    if [ -f "./build_all.sh" ]; then
        chmod +x ./build_all.sh LPC1768/build.sh LPC845/build.sh
        ./build_all.sh
    fi
fi

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${BOLD}${GREEN}✔ FIRMWARE SYNCED!${NC}"
echo -e "  Active LCD Version: ${BOLD}${CYAN}${ACTIVE_VER}${NC}"
echo -e "  Ready to flash:     ${BOLD}./flash_lpc1768.sh && ./flash_lpc845.sh${NC}"
echo -e "${GREEN}======================================================${NC}\n"

