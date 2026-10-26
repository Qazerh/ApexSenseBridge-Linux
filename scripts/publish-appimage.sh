#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
asset_dir=${1:-dist}
: "${GITHUB_REPOSITORY:?}" "${GITHUB_REF:?}" "${GITHUB_SHA:?}"
shopt -s nullglob
images=("$asset_dir"/*.AppImage)
[[ ${#images[@]} -eq 1 && -s "$asset_dir/SHA256SUMS" ]] || { echo 'Expected one AppImage and SHA256SUMS' >&2; exit 1; }
(cd "$asset_dir" && sha256sum --check SHA256SUMS)
assets=("${images[0]}" "$asset_dir/SHA256SUMS")
notes=$(mktemp)
trap 'rm -f "$notes"' EXIT

if [[ "$GITHUB_REF" == refs/heads/main ]]; then
    # A slower, older run must not replace the preview of a newer commit.
    head=$(gh api "repos/$GITHUB_REPOSITORY/commits/main" --jq .sha)
    [[ "$head" == "$GITHUB_SHA" ]] || { echo 'Skipping superseded preview'; exit 0; }
    {
        printf 'Automated preview of `%s`.\n\n' "$GITHUB_SHA"
        printf 'Download the AppImage and SHA256SUMS below. Requires x86_64 Linux with glibc 2.41 or newer.\n\n'
        printf 'This preview updates after successful main-branch engine, GUI, artwork and visual-layout checks.\n\n'
        printf 'Source: https://github.com/%s/tree/%s\n' "$GITHUB_REPOSITORY" "$GITHUB_SHA"
    } > "$notes"
    if gh release view preview --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
        gh release upload preview "${assets[@]}" --repo "$GITHUB_REPOSITORY" --clobber
        gh api --method PATCH "repos/$GITHUB_REPOSITORY/git/refs/tags/preview" -f "sha=$GITHUB_SHA" -F force=true >/dev/null
        gh release edit preview --repo "$GITHUB_REPOSITORY" --title 'Latest preview — AppImage' --notes-file "$notes" --prerelease
    else
        gh release create preview "${assets[@]}" --repo "$GITHUB_REPOSITORY" --target "$GITHUB_SHA" --title 'Latest preview — AppImage' --notes-file "$notes" --prerelease
    fi
elif [[ "$GITHUB_REF" == refs/tags/v* ]]; then
    tag=${GITHUB_REF#refs/tags/}
    if gh release view "$tag" --repo "$GITHUB_REPOSITORY" >/dev/null 2>&1; then
        gh release upload "$tag" "${assets[@]}" --repo "$GITHUB_REPOSITORY" --clobber
    else
        flags=()
        [[ "$tag" != *-* ]] || flags+=(--prerelease)
        gh release create "$tag" "${assets[@]}" --repo "$GITHUB_REPOSITORY" --target "$GITHUB_SHA" --generate-notes "${flags[@]}"
    fi
else
    echo 'Ref is not eligible for publishing' >&2
    exit 1
fi
