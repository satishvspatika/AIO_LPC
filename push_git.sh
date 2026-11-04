#!/usr/bin/env bash
# ==============================================================================
# push_git.sh - Version Tag, Compile, Commit, Tag, and Push Script for AIO_LPC
# ==============================================================================
# Usage:
#   ./push_git.sh -t <tag> [-m "<description>"]
#
# Examples:
#   ./push_git.sh -t "v6.0.1" -m "Fixed LCD boot timing"
#   ./push_git.sh -t "v6.0.1"              # (-m is optional; defaults to "Release v6.0.1")
#   ./push_git.sh -t "v6.0.2" --no-build  # Push without running build_all.sh
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
        -t|--tag|-v|--version)
            if [ -n "$2" ] && [[ ! "$2" =~ ^- ]]; then
                TARGET_VER="$2"
                shift 2
            else
                echo -e "${RED}[ERROR] Option $1 requires a version tag value (e.g. -t \"v6.0.1\")${NC}"
                exit 1
            fi
            ;;
        -t=*|--tag=*|-v=*|--version=*)
            TARGET_VER="${1#*=}"
            shift 1
            ;;
        -m|--message|-msg)
            if [ -n "$2" ] && [[ ! "$2" =~ ^- ]]; then
                COMMIT_MSG="$2"
                shift 2
            else
                COMMIT_MSG=""
                shift 1
            fi
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
            echo "  ./push_git.sh -t <tag> [-m \"<message>\"] [--no-build] [-f]"
            echo ""
            echo -e "${BOLD}Options:${NC}"
            echo "  -t, --tag <tag>       (Required) Version tag (e.g. v6.0.1 or 6.0.1)"
            echo "  -m, --message <msg>   (Optional) Commit message"
            echo "  --no-build            Skip build_all.sh compilation"
            echo "  -f, --force           Force overwrite tag if it already exists"
            echo "  -h, --help            Show this help"
            echo ""
            echo -e "${BOLD}Examples:${NC}"
            echo "  ./push_git.sh -t \"v6.0.1\" -m \"Fixed LCD initialization\""
            echo "  ./push_git.sh -t \"v6.0.1\""
            exit 0
            ;;
        *)
            if [ -z "$TARGET_VER" ] && [[ ! "$1" =~ ^- ]]; then
                TARGET_VER="$1"
                shift 1
            else
                echo -e "${YELLOW}[WARNING] Unknown argument: $1${NC}"
                shift 1
            fi
            ;;
    esac
done

# Read current version from user_config.h
CURRENT_RAW_VER=$(grep "#define LCD_VERSION_STRING" "$CONFIG_FILE" | head -n1 | sed -E 's/.*"v?([^"]+)".*/\1/' || true)
if [ -z "$CURRENT_RAW_VER" ]; then
    CURRENT_RAW_VER="6.0.0"
fi

# If target version was not provided on CLI, prompt or exit
if [ -z "$TARGET_VER" ]; then
    if [ -t 0 ]; then
        echo -e "${CYAN}Current configured version in LPC1768 is: ${YELLOW}v${CURRENT_RAW_VER}${NC}"
        read -r -p "Enter new version tag to push (e.g. v6.0.1): " TARGET_VER
    fi
    if [ -z "$TARGET_VER" ]; then
        echo -e "${RED}[ERROR] A version tag is required!${NC}"
        echo -e "Usage:   ${BOLD}./push_git.sh -t <tag> [-m \"<description>\"]${NC}"
        echo -e "Example: ${BOLD}./push_git.sh -t \"v6.0.1\" -m \"Fixed LCD boot timing\"${NC}"
        echo -e "         ${BOLD}./push_git.sh -t \"v6.0.1\"${NC}"
        exit 1
    fi
fi

# Clean up version string (strip leading 'v' for raw number, ensure 'v' for tag)
NEW_RAW_VER="${TARGET_VER#v}"
NEW_TAG="v${NEW_RAW_VER}"

# LCD 16x2 sanity check: Line 2 format is "ID:001922 vX.Y.Z" (max 16 chars)
if [ ${#NEW_TAG} -gt 9 ]; then
    echo -e "${YELLOW}[WARNING] Version '${NEW_TAG}' is ${#NEW_TAG} characters. Line 2 on 16x2 LCD may be truncated.${NC}"
fi

# Default commit message if -m was not provided
if [ -z "$COMMIT_MSG" ]; then
    COMMIT_MSG="Release ${NEW_TAG}"
fi

CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "main")"
if [ "$CURRENT_BRANCH" = "HEAD" ]; then
    CURRENT_BRANCH="main"
fi

echo -e "${CYAN}======================================================${NC}"
echo -e "${BOLD}AIO_LPC Firmware: Version, Build & Push${NC}"
echo -e "  Current Version: ${YELLOW}v${CURRENT_RAW_VER}${NC}"
echo -e "  Target Tag:      ${GREEN}${NEW_TAG}${NC} (Will display on LCD Line 2)"
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
echo -e "${GREEN}✔ Configured version as ${NEW_TAG} in LPC1768/user_config.h${NC}"

# 2. Build firmware
if [ "$SKIP_BUILD" -eq 0 ]; then
    echo -e "\n${YELLOW}1. Compiling firmware with version ${NEW_TAG}...${NC}"
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
    echo -e "${YELLOW}   No uncommitted code changes detected.${NC}"
else
    git commit -m "$COMMIT_MSG"
    echo -e "${GREEN}   ✔ Commit created successfully.${NC}"
fi

# 4. Tag the commit
echo -e "\n${YELLOW}3. Creating Git tag '${NEW_TAG}'...${NC}"
if git rev-parse "$NEW_TAG" >/dev/null 2>&1; then
    echo -e "${YELLOW}   Tag '${NEW_TAG}' already exists locally. Updating tag...${NC}"
    git tag -a "$NEW_TAG" -m "$COMMIT_MSG" -f
    FORCE_TAG=1
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
echo -e "  Tag:        ${BOLD}${NEW_TAG}${NC}"
echo -e "  LCD View:   ${CYAN}Line 2 will show: ID:001922 ${NEW_TAG}${NC}"
echo -e "${GREEN}======================================================${NC}"
echo -e "To update and flash on your other laptop:"
echo -e "  ${BOLD}git pull && ./flash_lpc1768.sh${NC}\n"

