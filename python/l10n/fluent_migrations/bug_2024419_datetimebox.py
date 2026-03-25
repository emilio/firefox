# Any copyright is dedicated to the Public Domain.
# http://creativecommons.org/publicdomain/zero/1.0/

from fluent.migrate.helpers import transforms_from


def migrate(ctx):
    """Bug 2024419 - Migrate datetime field placeholders into field labels, part {index}."""

    source = "toolkit/toolkit/global/datetimebox.ftl"
    target = source

    ctx.add_transforms(
        target,
        source,
        transforms_from(
            """
datetime-year-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-year.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-year-placeholder") }

datetime-month-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-month.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-month-placeholder") }

datetime-day-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-day.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-day-placeholder") }

datetime-hour-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-hour.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-time-placeholder") }

datetime-minute-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-minute.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-time-placeholder") }

datetime-second-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-second.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-time-placeholder") }

datetime-millisecond-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-millisecond.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-time-placeholder") }

datetime-dayperiod-2 =
    .aria-label = { COPY_PATTERN(from_path, "datetime-dayperiod.aria-label") }
    .placeholder = { COPY_PATTERN(from_path, "datetime-time-placeholder") }
""",
            from_path=source,
        ),
    )
