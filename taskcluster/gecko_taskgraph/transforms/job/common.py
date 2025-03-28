# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Common support for various job types.  These functions are all named after the
worker implementation they operate on, and take the same three parameters, for
consistency.
"""

from taskgraph.transforms.run.common import CACHES, add_cache
from taskgraph.util.keyed_by import evaluate_keyed_by
from taskgraph.util.taskcluster import get_artifact_prefix

SECRET_SCOPE = "secrets:get:project/releng/{trust_domain}/{kind}/level-{level}/{secret}"


def add_artifacts(config, job, taskdesc, path):
    taskdesc["worker"].setdefault("artifacts", []).append(
        {
            "name": get_artifact_prefix(taskdesc),
            "path": path,
            "type": "directory",
        }
    )


def docker_worker_add_artifacts(config, job, taskdesc):
    """Adds an artifact directory to the task"""
    path = "{workdir}/artifacts/".format(**job["run"])
    taskdesc["worker"].setdefault("env", {})["UPLOAD_DIR"] = path
    add_artifacts(config, job, taskdesc, path)


def generic_worker_add_artifacts(config, job, taskdesc):
    """Adds an artifact directory to the task"""
    # The path is the location on disk; it doesn't necessarily
    # mean the artifacts will be public or private; that is set via the name
    # attribute in add_artifacts.
    path = get_artifact_prefix(taskdesc)
    taskdesc["worker"].setdefault("env", {})["UPLOAD_DIR"] = path
    add_artifacts(config, job, taskdesc, path=path)


def get_cache_name(config, job):
    cache_name = "checkouts"

    # Sparse checkouts need their own cache because they can interfere
    # with clients that aren't sparse aware.
    if job["run"]["sparse-profile"]:
        cache_name += "-sparse"

    # Workers using Mercurial >= 5.8 will enable revlog-compression-zstd, which
    # workers using older versions can't understand, so they can't share cache.
    # At the moment, only docker workers use the newer version.
    if job["worker"]["implementation"] == "docker-worker":
        cache_name += "-hg58"

    return cache_name


def support_vcs_checkout(config, job, taskdesc):
    """Update a job/task with parameters to enable a VCS checkout.

    This can only be used with ``run-task`` tasks, as the cache name is
    reserved for ``run-task`` tasks.
    """
    worker = job["worker"]
    is_mac = worker["os"] == "macosx"
    is_win = worker["os"] == "windows"
    is_linux = worker["os"] == "linux" or "linux-bitbar" or "linux-lambda"
    is_docker = worker["implementation"] == "docker-worker"
    assert is_mac or is_win or is_linux

    if is_win:
        checkoutdir = "build"
        geckodir = f"{checkoutdir}/src"
        if "aarch64" in job["worker-type"] or "a64" in job["worker-type"]:
            # arm64 instances on azure don't support local ssds
            hgstore = f"{checkoutdir}/hg-store"
        else:
            hgstore = "y:/hg-shared"
    elif is_docker:
        checkoutdir = "{workdir}/checkouts".format(**job["run"])
        geckodir = f"{checkoutdir}/gecko"
        hgstore = f"{checkoutdir}/hg-store"
    else:
        checkoutdir = "checkouts"
        geckodir = f"{checkoutdir}/gecko"
        hgstore = f"{checkoutdir}/hg-shared"

    # Use some Gecko specific logic to determine cache name.
    CACHES["checkout"]["cache_name"] = get_cache_name

    env = taskdesc["worker"].setdefault("env", {})
    env.update(
        {
            "GECKO_BASE_REPOSITORY": config.params["base_repository"],
            "GECKO_HEAD_REPOSITORY": config.params["head_repository"],
            "GECKO_HEAD_REV": config.params["head_rev"],
            "HG_STORE_PATH": hgstore,
        }
    )

    gecko_path = env.setdefault("GECKO_PATH", geckodir)

    if "comm_base_repository" in config.params:
        taskdesc["worker"]["env"].update(
            {
                "COMM_BASE_REPOSITORY": config.params["comm_base_repository"],
                "COMM_HEAD_REPOSITORY": config.params["comm_head_repository"],
                "COMM_HEAD_REV": config.params["comm_head_rev"],
            }
        )
    elif job["run"].get("comm-checkout", False):
        raise Exception(
            "Can't checkout from comm-* repository if not given a repository."
        )

    # Give task access to hgfingerprint secret so it can pin the certificate
    # for hg.mozilla.org.
    taskdesc["scopes"].append("secrets:get:project/taskcluster/gecko/hgfingerprint")
    taskdesc["scopes"].append("secrets:get:project/taskcluster/gecko/hgmointernal")

    # only some worker platforms have taskcluster-proxy enabled
    if job["worker"]["implementation"] in ("docker-worker",):
        taskdesc["worker"]["taskcluster-proxy"] = True

    return gecko_path


def setup_secrets(config, job, taskdesc):
    """Set up access to secrets via taskcluster-proxy.  The value of
    run['secrets'] should be a boolean or a list of secret names that
    can be accessed."""
    if not job["run"].get("secrets"):
        return

    taskdesc["worker"]["taskcluster-proxy"] = True
    secrets = job["run"]["secrets"]
    if secrets is True:
        secrets = ["*"]
    for secret in secrets:
        taskdesc["scopes"].append(
            SECRET_SCOPE.format(
                trust_domain=config.graph_config["trust-domain"],
                kind=job["treeherder"]["kind"],
                level=config.params["level"],
                secret=secret,
            )
        )


def add_tooltool(config, job, taskdesc, internal=False):
    """Give the task access to tooltool.

    Enables the tooltool cache. Adds releng proxy. Configures scopes.

    By default, only public tooltool access will be granted. Access to internal
    tooltool can be enabled via ``internal=True``.

    This can only be used with ``run-task`` tasks, as the cache name is
    reserved for use with ``run-task``.
    """

    if job["worker"]["implementation"] in ("docker-worker",):
        add_cache(
            job,
            taskdesc,
            "tooltool-cache",
            "{workdir}/tooltool-cache".format(**job["run"]),
        )

        taskdesc["worker"].setdefault("env", {}).update(
            {
                "TOOLTOOL_CACHE": "{workdir}/tooltool-cache".format(**job["run"]),
            }
        )
    elif not internal:
        return

    taskdesc["worker"]["taskcluster-proxy"] = True
    taskdesc["scopes"].extend(
        [
            "project:releng:services/tooltool/api/download/public",
        ]
    )

    if internal:
        taskdesc["scopes"].extend(
            [
                "project:releng:services/tooltool/api/download/internal",
            ]
        )


def get_expiration(config, policy="default"):
    expires = evaluate_keyed_by(
        config.graph_config["expiration-policy"],
        "artifact expiration",
        {"project": config.params["project"]},
    )[policy]
    return expires
