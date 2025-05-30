load("//tools/workspace:pkg_config.bzl", "setup_pkg_config_repository")

def _impl(repo_ctx):
    # N.B. We do not check the return value here for errors. Sometimes this
    # rule is evaluated on systems where pkg-config will fail but where the
    # package will not actually be used. Instead, we'll rely on the deferred
    # reporting inside the generated BUILD file to report the error message
    # later on, if and only if the library is actually used.
    setup_pkg_config_repository(repo_ctx)

libblas_repository = repository_rule(
    attrs = {
        # Note that on Debian and Ubuntu in particular, the pkg-config blas
        # is chosen by the Ubuntu alternatives system, which might be one of
        # any number of different vendor implementations.
        "modname": attr.string(default = "blas"),
        "extra_deprecation": attr.string(default = "DRAKE DEPRECATED: The @libblas repository is deprecated. Use the @blas repository, instead. The deprecated code will be removed from Drake on or after 2025-05-01."),  # noqa
    },
    local = True,
    configure = True,
    implementation = _impl,
)
