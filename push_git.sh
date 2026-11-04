#!/usr/bin/env bash
# ==============================================================================
# push_git.sh - Auto-Version, Compile, Commit, Tag, and Push Script for AIO_LPC
# ==============================================================================
# Usage:
#   ./push_git.sh                       # Auto-bumps patch version (e.g. v6.0.0 -> v6.0.1)
#   ./push_git.sh -m "Fix sensor ADC"   # Auto-bumps version with custom commit message
#   ./push_git.sh -v 6.1.0              # Explicitly sets version to v6.1.0
#   ./push_git.sh --no-build            # Push without running build_all.sh
# ==============================================================================

set -e

# Terminal colors
BOLD='\033[1m'
GREEN='\033[0;32m'
CYAN='\033[0;36m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m' # No Color

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [ ! -d ".git" ]; then
    echo -e "${RED}[ERROR] This directory is not a Git repository (.git not found)!${NC}"
    exit 1
fi

CONFIG_FILE="${ROOT_DIR}/LPC1768/user_config.h"
if [ ! -f "$CONFIG_FILE" ]; then
    echo -e "${RED}[ERROR] Configuration file not found: ${CONFIG_FILE}${NC}"
    exit 1
fi

COMMIT_MSG=""
TARGET_VER=""
SKIP_BUILD=0
FORCE_TAG=0

# Parse CLI options
while [[ $# -gt 0 ]]; do
    case "$1" in
        -v|--version|-tag|--tag|-t)
            TARGET_VER="$2"
            shift 2
            ;;
        -v=*|--version=*|-tag=*|--tag=*)
            TARGET_VER="${1#*=}"
            shift 1
            ;;
        -m|--message|-msg)
            COMMIT_MSG="$2"
            shift 2
            ;;
        -m=*|--message=*)
            COMMIT_MSG="${1#*=}"
            shift 1
            ;;
        --no-build|--skip-build)
            SKIP_BUILD=1
            shift 1
            ;;
        -f|--force)
            FORCE_TAG=1
            shift 1
            ;;
        -h|--help)
            echo -e "${BOLD}Usage:${NC}"
            echo "  ./push_git.sh [-m \"<message>\"] [-v \"<version>\"] [--no-build] [-f]"
            echo ""
            echo -e "${BOLD}Options:${NC}"
            echo "  -m, --message <msg>   Commit message"
            echo "  -v, --version <ver>   Specify new version (e.g. 6.0.1 or v6.0.1). If omitted, patch bumps automatically."
            echo "  --no-build            Skip build_all.sh compilation"
            echo "  -f, --force           Force overwrite tag if it already exists"
            echo "  -h, --help            Show this help"
            exit 0
            ;;
        *)
            if [ -z "$COMMIT_MSG" ] && [[ ! "$1" =~ ^- ]]; then
                COMMIT_MSG="$1"
                shift 1
            else
                echo -e "${YELLOW}[WARNING] Unknown argument: $1${NC}"
                shift 1
            fi
            ;;
    esac
done

# Read current version from user_config.h
CURRENT_RAW_VER=$(grep "#define LCD_VERSION_STRING" "$CONFIG_FILE" | sed -E 's/.*"v?([^"]+)".*/\1/' || true)
if [ -z "$CURRENT_RAW_VER" ]; then
    CURRENT_RAW_VER="6.0.0"
fi

# Calculate new version
if [ -n "$TARGET_VER" ]; then
    NEW_RAW_VER="${TARGET_VER#v}"
else
    # Auto-increment patch version
    IFS='.' read -r MAJOR MINOR PATCH <<< "$CURRENT_RAW_VER"
    MAJOR=${MAJOR:-6}
    MINOR=${MINOR:-0}
    PATCH=${PATCH:-0}
    NEW_PATCH=$((PATCH + 1))
    NEW_RAW_VER="${MAJOR}.${MINOR}.${NEW_PATCH}"
fi

NEW_TAG="v${NEW_RAW_VER}"

# Default commit message
if [ -z "$COMMIT_MSG" ]; then
    TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
    COMMIT_MSG="Release ${NEW_TAG} (${TIMESTAMP})"
fi

CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")"
if [ "$CURRENT_BRANCH" = "HEAD" ]; then
    CURRENT_BRANCH="main"
fi

echo -e "${CYAN}======================================================${NC}"
echo -e "${BOLD}AIO_LPC Firmware: Auto-Version, Build & Push${NC}"
echo -e "  Current Version: ${YELLOW}v${CURRENT_RAW_VER}${NC}"
echo -e "  New Version:     ${GREEN}${NEW_TAG}${NC} (Will display on LCD Line 2)"
echo -e "  Branch:          ${GREEN}${CURRENT_BRANCH}${NC}"
echo -e "  Commit Message:  ${BOLD}${COMMIT_MSG}${NC}"
echo -e "${CYAN}======================================================${NC}"

# 1. Update version in user_config.h
python3 -c "
import re
with open('${CONFIG_FILE}', 'r') as f:
    content = f.read()
content = re.sub(r'#define FIRMWARE_VERSION\s+\"[^\"]+\"', '#define FIRMWARE_VERSION    \"AIO_${NEW_RAW_VER}\"', content)
content = re.sub(r'#define LCD_VERSION_STRING\s+\"[^\"]+\"', '#define LCD_VERSION_STRING  \"${NEW_TAG}\"', content)
with open('${CONFIG_FILE}', 'w') as f:
    f.write(content)
"
echo -e "${GREEN}✔ Updated version to ${NEW_TAG} in LPC1768/user_config.h${NC}"

# 2. Build firmware
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n${YELLOW}1. Compiling firmware with new version (${NEW_TAG})...${NC}"
    if [ -f "./build_all.sh" ]; then
        set +e
        ./build_all.sh
        BUILD_STATUS=$?
        set -e
        if [ $BUILD_STATUS -ne 0 ]; then
            echo -e "${RED}[ERROR] Build failed! Reverting version in user_config.h and aborting push.${NC}"
            git checkout "$CONFIG_FILE"
            exit 1
        fi
        echo -e "${GREEN}   ✔ Compilation successful! Ready-to-flash binaries generated.${NC}"
    fi
else
    echo -e "\n${YELLOW}1. Skipping compilation (--no-build requested).${NC}"
fi

# 3. Stage and Commit
echo -e "\n${YELLOW}2. Staging and committing changes...${NC}"
git add -A
if git diff --cached --quiet; then
    echo -e "${YELLOW}   No changes detected to commit.${NC}"
else
    git commit -m "$COMMIT_MSG"
    echo -e "${GREEN}   ✔ Commit created successfully.${NC}"
fi

# 4. Tag the commit
echo -e "\n${YELLOW}3. Creating Git tag '${NEW_TAG}'...${NC}"
if git rev-parse "$NEW_TAG" >/dev/null 2>&1; then
    if [ "$FORCE_TAG" -eq 1 ]; then
        git tag -a "$NEW_TAG" -m "$COMMIT_MSG" -f
    else
        echo -e "${YELLOW}   Tag '${NEW_TAG}' already exists locally. Updating tag...${NC}"
        git tag -a "$NEW_TAG" -m "$COMMIT_MSG" -f
        FORCE_TAG=1
    fi
else
    git tag -a "$NEW_TAG" -m "$COMMIT_MSG"
fi
echo -e "${GREEN}   ✔ Tag '${NEW_TAG}' ready.${NC}"

# 5. Push to GitHub
echo -e "\n${YELLOW}4. Pushing branch '${CURRENT_BRANCH}' and tag '${NEW_TAG}' to GitHub...${NC}"
git push origin "$CURRENT_BRANCH"
if [ "$FORCE_TAG" -eq 1 ]; then
    git push origin "$NEW_TAG" --force
else
    git push origin "$NEW_TAG"
fi

echo -e "\n${GREEN}======================================================${NC}"
echo -e "${BOLD}${GREEN}✔ PUSH SUCCESSFUL!${NC}"
echo -e "  Repository: https://github.com/satishvspatika/AIO_LPC"
echo -e "  Branch:     ${CURRENT_BRANCH}"
echo -e "  Version:    ${BOLD}${NEW_TAG}${NC}"
echo -e "  LCD View:   ${CYAN}Line 2 will show: ID:001922 ${NEW_TAG}${NC}"
echo -e "${GREEN}======================================================${NC}"
echo -e "To update and flash on your other laptop:"
echo -e "  ${BOLD}git pull && ./flash_lpc1768.sh${NC}\n"
