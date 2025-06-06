# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Transform the signing task into an actual task description.
"""

from taskgraph.transforms.base import TransformSequence
from taskgraph.util.dependencies import get_primary_dependency

from gecko_taskgraph.util.attributes import copy_attributes_from_dependent_job
from gecko_taskgraph.util.partners import get_partner_config_by_kind
from gecko_taskgraph.util.signed_artifacts import (
    generate_specifications_of_artifacts_to_sign,
)

transforms = TransformSequence()


@transforms.add
def set_mac_label(config, jobs):
    for job in jobs:
        dep_job = get_primary_dependency(config, job)
        if "mac-notarization" in config.kind:
            default_label = dep_job.label.replace("mac-signing", "mac-notarization")
            job.setdefault("label", default_label)
        assert (
            job["label"] != dep_job.label
        ), f"Unable to determine label for {config.kind}"
        yield job


@transforms.add
def define_upstream_artifacts(config, jobs):
    partner_configs = get_partner_config_by_kind(config, config.kind)
    if not partner_configs:
        return

    for job in jobs:
        dep_job = get_primary_dependency(config, job)
        job.setdefault("attributes", {}).update(
            copy_attributes_from_dependent_job(dep_job)
        )

        repack_ids = job["extra"]["repack_ids"]
        artifacts_specifications = generate_specifications_of_artifacts_to_sign(
            config,
            job,
            keep_locale_template=True,
            kind=config.kind,
        )
        task_type = "build"
        if "notarization" in dep_job.label or "mac-signing" in dep_job.label:
            task_type = "scriptworker"
        job["upstream-artifacts"] = [
            {
                "taskId": {"task-reference": f"<{dep_job.kind}>"},
                "taskType": task_type,
                "paths": [
                    path_template.format(locale=repack_id)
                    for path_template in spec["artifacts"]
                    for repack_id in repack_ids
                ],
                "formats": spec["formats"],
            }
            for spec in artifacts_specifications
        ]
        yield job
