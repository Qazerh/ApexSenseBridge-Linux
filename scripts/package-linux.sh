#!/usr/bin/env bash
# Package a clean Git revision together with its Release executable.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="$(realpath -- "${1:-$repo_dir/build-linux}")"
output_dir="$(realpath -m -- "${2:-$repo_dir/dist}")"
revision="$(git -C "$repo_dir" rev-parse HEAD)"
package_name="ApexSenseBridge-linux-x86_64"
mkdir -p -- "$output_dir"
staging_dir="$(mktemp -d)"
trap 'rm -rf -- "$staging_dir"' EXIT
# Source corresponds to the checked-out commit; CI builds this same revision.
mkdir -p -- "$staging_dir/$package_name/source" "$staging_dir/$package_name/bin"
git -C "$repo_dir" archive HEAD | tar -x -C "$staging_dir/$package_name/source"
install -m 0755 "$build_dir/ApexSenseBridge" "$staging_dir/$package_name/bin/ApexSenseBridge"
install -m 0755 "$repo_dir/scripts/apexsensebridge-steam" "$staging_dir/$package_name/bin/apexsensebridge-steam"
cp -- "$repo_dir/README.md" "$repo_dir/LICENSE" "$repo_dir/THIRD_PARTY_NOTICES.md" "$staging_dir/$package_name/"
cp -R -- "$repo_dir/packaging" "$repo_dir/docs" "$staging_dir/$package_name/"
printf '%s\n' "$revision" > "$staging_dir/$package_name/SOURCE-COMMIT"
tar -czf "$output_dir/$package_name.tar.gz" -C "$staging_dir" "$package_name"
(cd -- "$output_dir" && sha256sum "$package_name.tar.gz" > "$package_name.sha256")
printf 'Created %s/%s.tar.gz\n' "$output_dir" "$package_name"
