#!/bin/bash
# The old name of tools/publish-pages.sh, which publishes the privacy policy
# and the accessibility page together.
exec "$(dirname "$0")/publish-pages.sh" "$@"
