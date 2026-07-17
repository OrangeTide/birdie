#!/bin/sh
# scripts/update-ludica.sh — refresh the vendored copy of ludica.
#
# Usage:
#   scripts/update-ludica.sh            # tip of the default branch (main)
#   scripts/update-ludica.sh v26.04.14  # specific tag, branch, or commit
#
# Environment variables:
#   GITHUB_REPO   GitHub owner/repo (default: OrangeTide/ludica)
#
# Fetches a shallow clone of the requested ref and copies the parts birdie
# needs (src/, tools/, VERSION, LICENSE.txt, UPSTREAM, etc.) into
# src/thirdparty/ludica/. Unused parts are dropped: samples, docs, editor/IDE
# metadata, build outputs, ludica assets (birdie ships its own fonts), the
# tiny/ build variant, configs/, ludica's own scripts/ and GNUmakefile.

set -eu

GITHUB_REPO="${GITHUB_REPO:-OrangeTide/ludica}"
HERE=$(cd -- "$(dirname "$0")/.." && pwd)
DEST=$HERE/src/thirdparty/ludica

if ! command -v git >/dev/null 2>&1; then
	echo "update-ludica: git is required" >&2
	exit 1
fi

REF="${1:-main}"

URL="https://github.com/${GITHUB_REPO}.git"

tmpdir=$(mktemp -d) || exit 1
trap 'rm -rf "$tmpdir"' EXIT

# --depth 1 with an explicit ref via fetch handles tags, branches, and commits.
git -c advice.detachedHead=false clone --quiet --depth 1 --branch "$REF" \
	"$URL" "$tmpdir/ludica" 2>/dev/null || {
	# Branch/tag lookup failed; try resolving as a commit SHA.
	git -c advice.detachedHead=false init --quiet "$tmpdir/ludica"
	git -C "$tmpdir/ludica" remote add origin "$URL"
	git -C "$tmpdir/ludica" fetch --quiet --depth 1 origin "$REF"
	git -C "$tmpdir/ludica" checkout --quiet FETCH_HEAD
}

upstream_commit=$(git -C "$tmpdir/ludica" rev-parse HEAD)
upstream_desc=$(git -C "$tmpdir/ludica" describe --always --tags 2>/dev/null || echo "$REF")
upstream_date=$(date -u +%Y-%m-%dT%H:%M:%SZ)

mkdir -p "$DEST"

# Wipe DEST (preserve nothing — everything comes from upstream).
find "$DEST" -mindepth 1 -delete

# Copy upstream tree minus .git and dev-only directories.
rsync -a \
	--exclude '/.git' \
	--exclude '/.github' \
	--exclude '/.claude' \
	--exclude '/_build' \
	--exclude '/_out' \
	--exclude '/_site' \
	--exclude '/JUNK' \
	--exclude '/doc' \
	--exclude '/samples' \
	--exclude '/screenshots' \
	--exclude '*.o' \
	--exclude '*.dep' \
	--exclude '*~' \
	--exclude 'compile_commands.json' \
	--exclude '/assets' \
	--exclude '/tiny' \
	--exclude '/configs' \
	--exclude '/scripts' \
	--exclude '/GNUmakefile' \
	--exclude '/src/imgui' \
	--exclude '/tests' \
	--exclude '/.mcp.json' \
	--exclude '/cga-compat-test.txt' \
	"$tmpdir/ludica"/ "$DEST"/

cat > "$DEST/UPSTREAM" <<EOF
name: ludica
url: $URL
ref: $REF
commit: $upstream_commit
describe: $upstream_desc
synced_at: $upstream_date
EOF

# Merge ludica's Claude Code skills into birdie's .claude/skills/.
if [ -d "$tmpdir/ludica/.claude/skills" ]; then
	for skill_dir in "$tmpdir/ludica/.claude/skills"/*/; do
		skill_name=$(basename "$skill_dir")
		mkdir -p "$HERE/.claude/skills/$skill_name"
		cp "$skill_dir"SKILL.md "$HERE/.claude/skills/$skill_name/SKILL.md" 2>/dev/null || true
	done
	echo "update-ludica: merged skills into .claude/skills/"
fi

echo "update-ludica: synced $upstream_desc (ref: $REF) into $DEST"
