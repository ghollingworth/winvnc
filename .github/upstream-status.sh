#!/usr/bin/env bash
# Check upstream activity for each bundled C dependency and post a snapshot
# to a single rolling tracking issue. Runs weekly via release.yml's sibling
# workflow. Reads pinned SHAs from the local repo's gitlinks; fetches
# upstream and (for forked deps) our fork remote, and computes the count of
# commits on upstream that are not yet reachable from our pin.

set -euo pipefail

ISSUE_TITLE="Upstream dependency status"
ISSUE_LABEL="upstream-status"
TODAY=$(date -u +%Y-%m-%d)
WORKFLOW_FILE=".github/workflows/upstream-status.yml"

# (sm-name, submodule-path, upstream-url, upstream-branch)
DEPS=(
    "neatvnc|neatvnc|https://github.com/any1/neatvnc.git|master"
    "aml|aml|https://github.com/any1/aml.git|master"
    "pixman|deps/pixman|https://gitlab.freedesktop.org/pixman/pixman.git|master"
    "libjpeg-turbo|deps/libjpeg-turbo|https://github.com/libjpeg-turbo/libjpeg-turbo.git|main"
    "zlib|deps/zlib|https://github.com/madler/zlib.git|develop"
)

sm_pin_url() {
    git config --file .gitmodules "submodule.$1.url"
}

sm_pin_sha() {
    git ls-tree HEAD "$1" | awk '{print $3}'
}

short_date() {
    git -C "$1" show -s --format=%ci "$2" | cut -c1-10
}

short_sha() {
    echo "$1" | cut -c1-7
}

check_dep() {
    local name="$1" sm_path="$2" url_upstream="$3" branch="$4"
    local url_pin sha_pin workdir sha_upstream date_pin date_upstream ahead

    url_pin=$(sm_pin_url "$sm_path")
    sha_pin=$(sm_pin_sha "$sm_path")

    workdir=$(mktemp -d)
    git -C "$workdir" init -q
    git -C "$workdir" remote add upstream "$url_upstream"
    git -C "$workdir" fetch -q upstream "$branch"
    if [[ "$url_pin" != "$url_upstream" ]]; then
        git -C "$workdir" remote add pin "$url_pin"
        git -C "$workdir" fetch -q pin
    fi

    sha_upstream=$(git -C "$workdir" rev-parse "upstream/$branch")
    date_pin=$(short_date "$workdir" "$sha_pin")
    date_upstream=$(short_date "$workdir" "$sha_upstream")
    ahead=$(git -C "$workdir" rev-list --count "$sha_upstream" "^$sha_pin")

    rm -rf "$workdir"

    printf '| **%s** | `%s` %s | `%s` %s | %s |\n' \
        "$name" "$(short_sha "$sha_pin")" "$date_pin" \
        "$(short_sha "$sha_upstream")" "$date_upstream" "$ahead"
}

build_report() {
    cat <<HEADER
### Upstream dependency status — $TODAY

Generated weekly by [\`$WORKFLOW_FILE\`]($WORKFLOW_FILE). Compares the
SHAs pinned in this repo's submodules against each upstream's default
branch; the last column is the count of upstream commits that are not
reachable from our pin.

| dep | pinned | upstream tip | commits since pin |
|---|---|---|---|
HEADER

    for entry in "${DEPS[@]}"; do
        IFS='|' read -r name sm_path url_upstream branch <<< "$entry"
        check_dep "$name" "$sm_path" "$url_upstream" "$branch"
    done

    cat <<FOOTER

_neatvnc and aml are tracked against their true upstreams ([\`any1/neatvnc\`](https://github.com/any1/neatvnc), [\`any1/aml\`](https://github.com/any1/aml)); our pinned commits live on Windows-port branches of the [ghollingworth forks](https://github.com/ghollingworth)._
FOOTER
}

ensure_label() {
    gh label create "$ISSUE_LABEL" \
        --description "Tracking issue for weekly upstream-dependency snapshot" \
        --color BFD4F2 2>/dev/null || true
}

publish() {
    local body
    body=$(build_report)

    local existing
    existing=$(gh issue list --label "$ISSUE_LABEL" --state open \
        --limit 1 --json number --jq '.[0].number // empty')

    if [[ -n "$existing" ]]; then
        echo "Updating issue #$existing"
        printf '%s\n' "$body" | gh issue edit "$existing" --body-file -
    else
        echo "Creating new tracking issue"
        printf '%s\n' "$body" | gh issue create \
            --title "$ISSUE_TITLE" \
            --label "$ISSUE_LABEL" \
            --body-file -
    fi
}

ensure_label
publish
