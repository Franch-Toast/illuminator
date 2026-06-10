"""Generates version_generated.h from Bazel workspace status (stamp) info."""

def _version_header_impl(ctx):
    output = ctx.outputs.out

    # ctx.info_file = stable-status.txt (contains STABLE_* keys)
    # ctx.version_file = volatile-status.txt (contains BUILD_TIMESTAMP etc.)
    ctx.actions.run_shell(
        outputs = [output],
        inputs = [ctx.info_file, ctx.version_file],
        command = """
            VERSION=$(grep STABLE_GIT_VERSION {stable} | cut -d' ' -f2-)
            COMMIT=$(grep STABLE_GIT_COMMIT {stable} | cut -d' ' -f2-)
            TIMESTAMP=$(grep BUILD_TIMESTAMP {volatile} | cut -d' ' -f2-)
            VERSION=${{VERSION:-dev}}
            COMMIT=${{COMMIT:-unknown}}
            TIMESTAMP=${{TIMESTAMP:-unknown}}
            cat > {out} <<HEADER
#pragma once
namespace illuminator {{
inline constexpr const char* kBuildVersion = "$VERSION";
inline constexpr const char* kBuildCommit = "$COMMIT";
inline constexpr const char* kBuildTimestamp = "$TIMESTAMP";
}}
HEADER
        """.format(
            stable = ctx.info_file.path,
            volatile = ctx.version_file.path,
            out = output.path,
        ),
    )
    return [DefaultInfo(files = depset([output]))]

version_header = rule(
    implementation = _version_header_impl,
    attrs = {
        "out": attr.output(mandatory = True),
    },
)
