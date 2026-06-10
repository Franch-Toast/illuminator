#!/bin/bash
# Bazel workspace status command — provides version info at build time.
# Called automatically by Bazel via --workspace_status_command.
# Outputs KEY VALUE pairs; STABLE_ prefix → stable-status.txt, others → volatile.

# Version rule: tag > describe > commit hash
if git_tag=$(git describe --tags --exact-match 2>/dev/null); then
    version="${git_tag#v}"
elif git_desc=$(git describe --tags --always 2>/dev/null); then
    version="$git_desc"
elif git_hash=$(git rev-parse --short=8 HEAD 2>/dev/null); then
    version="$git_hash"
else
    version="dev"
fi

echo "STABLE_GIT_VERSION $version"
echo "STABLE_GIT_COMMIT $(git rev-parse HEAD 2>/dev/null || echo unknown)"
echo "BUILD_TIMESTAMP $(date -u +%Y-%m-%dT%H:%M:%SZ)"
